#include "replay_buffer.h"

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

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace replay_buffer {

// ── Impl ──────────────────────────────────────────────────────────────────────

struct ReplayBuffer::Impl {
    mutable std::mutex              mutex;
    std::deque<encoding::EncodedPacket> ring;
    size_t                          total_bytes = 0;

    Config                          cfg;
    encoding::VideoStreamParams     vsp;
    encoding::AudioStreamParams     asp;
    bool                            vsp_set = false;
    bool                            asp_set = false;

    std::atomic<bool>               saving{false};
    std::thread                     save_thread;
};

// ── ReplayBuffer ──────────────────────────────────────────────────────────────

ReplayBuffer::ReplayBuffer()  : impl_(new Impl()) {}
ReplayBuffer::~ReplayBuffer()
{
    if (impl_->save_thread.joinable())
        impl_->save_thread.join();
    delete impl_;
}

void ReplayBuffer::configure(Config const& cfg)
{
    std::lock_guard lk(impl_->mutex);
    impl_->cfg = cfg;
}

void ReplayBuffer::set_video_params(encoding::VideoStreamParams const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->vsp     = p;
    impl_->vsp_set = true;
}

void ReplayBuffer::set_audio_params(encoding::AudioStreamParams const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->asp     = p;
    impl_->asp_set = true;
}

void ReplayBuffer::push(encoding::EncodedPacket pkt)
{
    const size_t pkt_size = pkt.data.size();
    {
        std::lock_guard lk(impl_->mutex);
        impl_->total_bytes += pkt_size;
        impl_->ring.push_back(std::move(pkt));

        // Trim oldest packets until within memory cap.
        const size_t cap = static_cast<size_t>(impl_->cfg.memory_cap_mb) * 1024 * 1024;
        while (impl_->total_bytes > cap && !impl_->ring.empty()) {
            impl_->total_bytes -= impl_->ring.front().data.size();
            impl_->ring.pop_front();
        }
    }
}

size_t ReplayBuffer::packet_count() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->ring.size();
}

size_t ReplayBuffer::memory_bytes() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->total_bytes;
}

// ── Clip save internals ───────────────────────────────────────────────────────

static double pkt_seconds(const encoding::EncodedPacket& p)
{
    if (p.tb_den == 0) return 0.0;
    return static_cast<double>(p.pts) * p.tb_num / p.tb_den;
}

// Returns the index of the video keyframe that begins the clip, or ring.size()
// if no suitable keyframe exists.
//
// Strategy: find the *last* video keyframe at or before start_time so the
// clip duration is >= duration_sec.  If no such keyframe exists (buffer is
// shorter than requested), fall back to the earliest keyframe we have.
static size_t find_clip_start(
    const std::deque<encoding::EncodedPacket>& ring,
    double duration_sec)
{
    if (ring.empty()) return ring.size();

    double end_time   = pkt_seconds(ring.back());
    double start_time = end_time - duration_sec;

    size_t best = ring.size(); // sentinel: no suitable KF found yet
    for (size_t i = 0; i < ring.size(); ++i) {
        const auto& p = ring[i];
        if (p.stream_index != 0 || !p.is_keyframe) continue;
        if (pkt_seconds(p) > start_time) break; // past the window — stop searching
        best = i; // this KF is at or before start_time: update last-seen candidate
    }

    // If no KF is at or before start_time, use the first available KF.
    if (best == ring.size()) {
        for (size_t i = 0; i < ring.size(); ++i) {
            if (ring[i].stream_index == 0 && ring[i].is_keyframe) {
                best = i;
                break;
            }
        }
    }

    return best;
}

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

static std::wstring generate_clip_path(const std::wstring& dir, int duration_sec)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH,
        L"%s\\%04d%02d%02d_%02d%02d%02d_%ds_clip.mkv",
        dir.c_str(),
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        duration_sec);
    return path;
}

static std::wstring write_mkv(
    const std::vector<encoding::EncodedPacket>& pkts,
    const encoding::VideoStreamParams&          vsp,
    const encoding::AudioStreamParams&          asp,
    const std::wstring&                         out_dir,
    int                                         duration_sec)
{
    if (pkts.empty()) return {};
    if (!vsp.tb_den || !asp.tb_den) return {};

    // Ensure the output directory exists.
    CreateDirectoryW(out_dir.c_str(), nullptr);

    std::wstring path     = generate_clip_path(out_dir, duration_sec);
    std::string  path_utf = wcs_to_utf8(path);
    if (path_utf.empty()) return {};

    // ── Open Matroska output context ─────────────────────────────────────────
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "matroska",
                                       path_utf.c_str()) < 0)
        return {};

    // ── Video stream ─────────────────────────────────────────────────────────
    AVStream* vs = avformat_new_stream(fmt, nullptr);
    if (!vs) { avformat_free_context(fmt); return {}; }
    vs->codecpar->codec_type  = AVMEDIA_TYPE_VIDEO;
    vs->codecpar->codec_id    = AV_CODEC_ID_H264;
    vs->codecpar->width       = vsp.width;
    vs->codecpar->height      = vsp.height;
    vs->time_base             = {vsp.tb_num, vsp.tb_den};
    if (!vsp.extradata.empty()) {
        vs->codecpar->extradata = reinterpret_cast<uint8_t*>(
            av_mallocz(vsp.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (vs->codecpar->extradata) {
            memcpy(vs->codecpar->extradata,
                   vsp.extradata.data(), vsp.extradata.size());
            vs->codecpar->extradata_size = static_cast<int>(vsp.extradata.size());
        }
    }

    // ── Audio stream ─────────────────────────────────────────────────────────
    AVStream* as = avformat_new_stream(fmt, nullptr);
    if (!as) { avformat_free_context(fmt); return {}; }
    as->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    as->codecpar->codec_id    = AV_CODEC_ID_AAC;
    as->codecpar->sample_rate = asp.sample_rate;
    av_channel_layout_default(&as->codecpar->ch_layout, asp.channels);
    as->time_base             = {asp.tb_num, asp.tb_den};
    if (!asp.extradata.empty()) {
        as->codecpar->extradata = reinterpret_cast<uint8_t*>(
            av_mallocz(asp.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (as->codecpar->extradata) {
            memcpy(as->codecpar->extradata,
                   asp.extradata.data(), asp.extradata.size());
            as->codecpar->extradata_size = static_cast<int>(asp.extradata.size());
        }
    }

    // ── Open file + write header ──────────────────────────────────────────────
    if (avio_open(&fmt->pb, path_utf.c_str(), AVIO_FLAG_WRITE) < 0) {
        avformat_free_context(fmt);
        return {};
    }
    if (avformat_write_header(fmt, nullptr) < 0) {
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        return {};
    }

    // ── Compute PTS offsets so the clip starts at 0 ───────────────────────────
    int64_t v_offset = AV_NOPTS_VALUE;
    int64_t a_offset = AV_NOPTS_VALUE;
    for (const auto& ep : pkts) {
        if (ep.stream_index == 0 && v_offset == AV_NOPTS_VALUE) v_offset = ep.pts;
        if (ep.stream_index == 1 && a_offset == AV_NOPTS_VALUE) a_offset = ep.pts;
    }
    if (v_offset == AV_NOPTS_VALUE) v_offset = 0;
    if (a_offset == AV_NOPTS_VALUE) a_offset = 0;

    // ── Write packets ─────────────────────────────────────────────────────────
    for (const auto& ep : pkts) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) continue;
        if (av_new_packet(pkt, static_cast<int>(ep.data.size())) < 0) {
            av_packet_free(&pkt);
            continue;
        }

        pkt->stream_index = ep.stream_index;
        memcpy(pkt->data, ep.data.data(), ep.data.size());
        pkt->flags        = ep.is_keyframe ? AV_PKT_FLAG_KEY : 0;

        const int64_t offset = (ep.stream_index == 0) ? v_offset : a_offset;
        pkt->pts = ep.pts - offset;
        pkt->dts = ep.dts - offset;

        // Rescale from packet timebase to stream timebase (both are set the same,
        // so this is a no-op but guards against future divergence).
        AVRational src_tb = {ep.tb_num, ep.tb_den};
        AVRational dst_tb = (ep.stream_index == 0) ? vs->time_base : as->time_base;
        av_packet_rescale_ts(pkt, src_tb, dst_tb);

        av_interleaved_write_frame(fmt, pkt);
        av_packet_free(&pkt);
    }

    av_write_trailer(fmt);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return path;
}

// ── save_clip ─────────────────────────────────────────────────────────────────

void ReplayBuffer::save_clip(std::function<void(std::wstring)> cb)
{
    // Drop concurrent save requests — don't queue.
    bool expected = false;
    if (!impl_->saving.compare_exchange_strong(expected, true))
        return;

    // Snapshot the ring buffer and stream params under the lock.
    std::vector<encoding::EncodedPacket> snapshot;
    Config                               cfg;
    encoding::VideoStreamParams          vsp;
    encoding::AudioStreamParams          asp;
    bool vsp_set, asp_set;
    {
        std::lock_guard lk(impl_->mutex);
        cfg     = impl_->cfg;
        vsp     = impl_->vsp;
        asp     = impl_->asp;
        vsp_set = impl_->vsp_set;
        asp_set = impl_->asp_set;

        size_t start = find_clip_start(impl_->ring, cfg.duration_sec);
        if (start < impl_->ring.size()) {
            snapshot.reserve(impl_->ring.size() - start);
            for (size_t i = start; i < impl_->ring.size(); ++i)
                snapshot.push_back(impl_->ring[i]);
        }
    }

    if (impl_->save_thread.joinable())
        impl_->save_thread.join();

    impl_->save_thread = std::thread(
        [this, snapshot = std::move(snapshot), cfg, vsp, asp, vsp_set, asp_set,
         cb = std::move(cb)]() mutable
        {
            std::wstring result;
            if (!snapshot.empty() && vsp_set && asp_set) {
                result = write_mkv(snapshot, vsp, asp,
                                   cfg.output_dir, cfg.duration_sec);
            }
            impl_->saving.store(false);
            if (cb) cb(result);
        });
}

} // namespace replay_buffer
