#include "recording.h"

#include <encoding/mux_common.h>

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
#include <vector>

namespace recording {

namespace mux = encoding::mux;

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
    std::vector<encoding::AudioStreamParams> audio_params;
    bool                        vsp_set = false;

    AVFormatContext*            fmt = nullptr;
    AVStream*                   video_stream = nullptr;
    std::array<AVStream*, 7>    audio_streams{};
    bool                        header_written = false;
    bool                        wrote_packet = false;
    bool                        started_at_keyframe = false;
    bool                        resume_needs_keyframe = false;
    std::array<StreamTiming, 7> timing{};
};

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
        mux::file_extension(container));
    return path;
}

template <typename ImplT>
static bool open_output(ImplT* impl)
{
    if (impl->header_written) return true;
    if (!impl->vsp_set || impl->path.empty()) return false;

    CreateDirectoryW(impl->output_dir.c_str(), nullptr);

    std::string path_utf = mux::wcs_to_utf8(impl->path);
    if (path_utf.empty()) return false;

    mux::StreamSet streams;
    if (!mux::alloc_output(path_utf, impl->container, impl->vsp,
                           impl->audio_params, &impl->fmt, &streams))
        return false;

    impl->video_stream  = streams.video;
    impl->audio_streams = streams.audio;

    if (!mux::open_file_and_write_header(impl->fmt, path_utf, impl->container))
        return false;

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
    impl->audio_streams = {};
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
    if (ep.stream_index < 0 || ep.stream_index > 6) return false;

    AVStream* dst_stream = nullptr;
    if (ep.stream_index == 0)
        dst_stream = impl->video_stream;
    else
        dst_stream = impl->audio_streams[ep.stream_index];
    if (!dst_stream) return false;

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

    const int64_t pts_off = timing.input_anchor_pts + timing.paused_pts;
    const int64_t dts_off = timing.input_anchor_dts + timing.paused_pts;
    bool ok = mux::write_packet(impl->fmt, dst_stream, ep, pts_off, dts_off);
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
    set_audio_params(std::vector<encoding::AudioStreamParams>{ p });
}

void ManualRecorder::set_audio_params(std::vector<encoding::AudioStreamParams> const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->audio_params.clear();
    for (const auto& stream : p) {
        if (stream.stream_index < 1 || stream.stream_index > 6) continue;
        if (stream.tb_den == 0 || stream.sample_rate <= 0 || stream.channels <= 0) continue;
        impl_->audio_params.push_back(stream);
    }
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
        if (pkt.stream_index >= 0 && pkt.stream_index <= 6) {
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
