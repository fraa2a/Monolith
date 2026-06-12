#include "recording.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstring>
#include <mutex>
#include <utility>

namespace recording {

struct StreamTiming {
    bool    input_anchor_set = false;
    bool    pause_start_set  = false;
    int64_t input_anchor_pts = 0;
    int64_t input_anchor_dts = 0;
    int64_t pause_start_pts  = 0;
    int64_t paused_pts       = 0;
};

struct ManualRecorder::Impl {
    mutable std::mutex          mutex;
    RecordingState              state = RecordingState::Idle;
    std::wstring                output_dir;
    std::string                 container = "mkv";
    std::wstring                path;

    encoding::VideoStreamParams vsp;
    encoding::AudioStreamParams asp;
    bool                        vsp_set = false;
    bool                        asp_set = false;

    AVFormatContext*            fmt = nullptr;
    AVStream*                   video_stream = nullptr;
    AVStream*                   audio_stream = nullptr;
    bool                        header_written = false;
    bool                        wrote_packet = false;
    bool                        started_at_keyframe = false;
    bool                        resume_needs_keyframe = false;
    std::array<StreamTiming, 2> timing{};
};

static std::string wcs_to_utf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0,
        ws.c_str(), static_cast<int>(ws.size()),
        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0,
        ws.c_str(), static_cast<int>(ws.size()),
        s.data(), n, nullptr, nullptr);
    return s;
}

static const char* muxer_name(const std::string& container)
{
    return container == "mp4" ? "mp4" : "matroska";
}

static const wchar_t* file_extension(const std::string& container)
{
    return container == "mp4" ? L"mp4" : L"mkv";
}

static AVCodecID video_codec_id(encoding::VideoCodec codec)
{
    return codec == encoding::VideoCodec::H265
        ? AV_CODEC_ID_HEVC
        : AV_CODEC_ID_H264;
}

static std::wstring generate_recording_path(
    const std::wstring& dir,
    const std::string& container)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH,
        L"%s\\%04d%02d%02d_%02d%02d%02d_recording.%s",
        dir.c_str(),
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        file_extension(container));
    return path;
}

static bool copy_extradata(AVCodecParameters* codecpar, const std::vector<uint8_t>& data)
{
    if (data.empty()) return true;
    codecpar->extradata = reinterpret_cast<uint8_t*>(
        av_mallocz(data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codecpar->extradata) return false;
    memcpy(codecpar->extradata, data.data(), data.size());
    codecpar->extradata_size = static_cast<int>(data.size());
    return true;
}

template <typename ImplT>
static bool open_output(ImplT* impl)
{
    if (impl->header_written) return true;
    if (!impl->vsp_set || !impl->asp_set || impl->path.empty()) return false;

    CreateDirectoryW(impl->output_dir.c_str(), nullptr);

    std::string path_utf = wcs_to_utf8(impl->path);
    if (path_utf.empty()) return false;

    if (avformat_alloc_output_context2(&impl->fmt, nullptr, muxer_name(impl->container),
                                       path_utf.c_str()) < 0)
        return false;

    impl->video_stream = avformat_new_stream(impl->fmt, nullptr);
    if (!impl->video_stream) return false;
    impl->video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    impl->video_stream->codecpar->codec_id   = video_codec_id(impl->vsp.codec);
    impl->video_stream->codecpar->width      = impl->vsp.width;
    impl->video_stream->codecpar->height     = impl->vsp.height;
    impl->video_stream->time_base            = {impl->vsp.tb_num, impl->vsp.tb_den};
    if (!copy_extradata(impl->video_stream->codecpar, impl->vsp.extradata)) return false;

    impl->audio_stream = avformat_new_stream(impl->fmt, nullptr);
    if (!impl->audio_stream) return false;
    impl->audio_stream->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    impl->audio_stream->codecpar->codec_id    = AV_CODEC_ID_AAC;
    impl->audio_stream->codecpar->sample_rate = impl->asp.sample_rate;
    av_channel_layout_default(&impl->audio_stream->codecpar->ch_layout,
                              impl->asp.channels);
    impl->audio_stream->time_base = {impl->asp.tb_num, impl->asp.tb_den};
    if (!copy_extradata(impl->audio_stream->codecpar, impl->asp.extradata)) return false;

    if (avio_open(&impl->fmt->pb, path_utf.c_str(), AVIO_FLAG_WRITE) < 0) return false;

    AVDictionary* opts = nullptr;
    if (impl->container == "mp4")
        av_dict_set(&opts, "movflags", "+faststart", 0);
    const bool header_ok = avformat_write_header(impl->fmt, &opts) >= 0;
    av_dict_free(&opts);
    if (!header_ok) return false;

    impl->header_written = true;
    return true;
}

template <typename ImplT>
static void close_output(ImplT* impl)
{
    if (impl->fmt) {
        if (impl->header_written) av_write_trailer(impl->fmt);
        if (impl->fmt->pb) avio_closep(&impl->fmt->pb);
        avformat_free_context(impl->fmt);
    }
    impl->fmt = nullptr;
    impl->video_stream = nullptr;
    impl->audio_stream = nullptr;
    impl->header_written = false;
    impl->wrote_packet = false;
    impl->started_at_keyframe = false;
    impl->resume_needs_keyframe = false;
    impl->timing = {};
}

template <typename ImplT>
static bool write_packet(ImplT* impl, const encoding::EncodedPacket& ep)
{
    if (!open_output(impl)) return false;
    if (ep.stream_index < 0 || ep.stream_index > 1) return false;

    StreamTiming& timing = impl->timing[ep.stream_index];
    if (!timing.input_anchor_set) {
        timing.input_anchor_set = true;
        timing.input_anchor_pts = ep.pts;
        timing.input_anchor_dts = ep.dts;
    }

    if (timing.pause_start_set) {
        if (ep.pts > timing.pause_start_pts)
            timing.paused_pts += ep.pts - timing.pause_start_pts;
        timing.pause_start_set = false;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;
    if (av_new_packet(pkt, static_cast<int>(ep.data.size())) < 0) {
        av_packet_free(&pkt);
        return false;
    }

    pkt->stream_index = ep.stream_index;
    memcpy(pkt->data, ep.data.data(), ep.data.size());
    pkt->flags = ep.is_keyframe ? AV_PKT_FLAG_KEY : 0;
    pkt->pts = ep.pts - timing.input_anchor_pts - timing.paused_pts;
    pkt->dts = ep.dts - timing.input_anchor_dts - timing.paused_pts;
    if (pkt->dts > pkt->pts) pkt->dts = pkt->pts;

    AVRational src_tb = {ep.tb_num, ep.tb_den};
    AVRational dst_tb = (ep.stream_index == 0)
        ? impl->video_stream->time_base
        : impl->audio_stream->time_base;
    av_packet_rescale_ts(pkt, src_tb, dst_tb);

    bool ok = av_interleaved_write_frame(impl->fmt, pkt) >= 0;
    av_packet_free(&pkt);
    if (ok) impl->wrote_packet = true;
    return ok;
}

ManualRecorder::ManualRecorder() : impl_(new Impl()) {}
ManualRecorder::~ManualRecorder()
{
    {
        std::lock_guard lk(impl_->mutex);
        close_output(impl_);
    }
    delete impl_;
}

void ManualRecorder::set_video_params(encoding::VideoStreamParams const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->vsp = p;
    impl_->vsp_set = true;
}

void ManualRecorder::set_audio_params(encoding::AudioStreamParams const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->asp = p;
    impl_->asp_set = true;
}

bool ManualRecorder::start(std::wstring output_dir, std::string container)
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->state != RecordingState::Idle) return false;
    impl_->output_dir = std::move(output_dir);
    impl_->container = (container == "mp4") ? "mp4" : "mkv";
    impl_->path = generate_recording_path(impl_->output_dir, impl_->container);
    impl_->state = RecordingState::Recording;
    return true;
}

bool ManualRecorder::pause()
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->state != RecordingState::Recording) return false;
    impl_->state = RecordingState::Paused;
    return true;
}

bool ManualRecorder::resume()
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->state != RecordingState::Paused) return false;
    impl_->state = RecordingState::Recording;
    impl_->resume_needs_keyframe = true;
    return true;
}

bool ManualRecorder::stop(std::wstring* output_path)
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->state == RecordingState::Idle) return false;
    if (output_path) *output_path = impl_->wrote_packet ? impl_->path : std::wstring{};
    close_output(impl_);
    impl_->path.clear();
    impl_->state = RecordingState::Idle;
    return true;
}

void ManualRecorder::push(encoding::EncodedPacket pkt)
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->state == RecordingState::Idle) return;

    if (impl_->state == RecordingState::Paused) {
        if (pkt.stream_index >= 0 && pkt.stream_index <= 1) {
            StreamTiming& timing = impl_->timing[pkt.stream_index];
            if (timing.input_anchor_set && !timing.pause_start_set) {
                timing.pause_start_set = true;
                timing.pause_start_pts = pkt.pts;
            }
        }
        return;
    }

    if (!impl_->started_at_keyframe) {
        if (pkt.stream_index != 0 || !pkt.is_keyframe) return;
        impl_->started_at_keyframe = true;
    }

    if (impl_->resume_needs_keyframe) {
        if (pkt.stream_index != 0 || !pkt.is_keyframe) return;
        impl_->resume_needs_keyframe = false;
    }

    write_packet(impl_, pkt);
}

RecordingState ManualRecorder::state() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->state;
}

std::wstring ManualRecorder::current_path() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->path;
}

const char* state_name(RecordingState state)
{
    switch (state) {
    case RecordingState::Idle:      return "idle";
    case RecordingState::Recording: return "recording";
    case RecordingState::Paused:    return "paused";
    }
    return "unknown";
}

} // namespace recording
