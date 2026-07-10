#include "replay_buffer.h"

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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace replay_buffer {

namespace mux = encoding::mux;

// ── Impl ──────────────────────────────────────────────────────────────────────

struct ReplayBuffer::Impl {
    mutable std::mutex              mutex;
    std::deque<encoding::EncodedPacket> ring;

    // Tracked buffer state (OBS-style).
    size_t                          total_bytes = 0;
    int64_t                         cur_time    = 0;  // dts_usec of oldest pkt
    int                             keyframes   = 0;  // KF count in ring

    Config                          cfg;
    encoding::VideoStreamParams     vsp;
    std::vector<encoding::AudioStreamParams> audio_params;
    bool                            vsp_set = false;

    std::atomic<bool>               saving{false};
    std::thread                     save_thread;

    bool can_purge() const
    {
        return ring.size() >= 2 && keyframes > 2;
    }

    bool pop_front_packet()
    {
        if (ring.empty()) return false;

        encoding::EncodedPacket pkt = std::move(ring.front());
        ring.pop_front();

        const size_t pkt_size = pkt.size();
        total_bytes = (pkt_size <= total_bytes) ? (total_bytes - pkt_size) : 0;

        const bool was_video_keyframe = pkt.stream_index == 0 && pkt.is_keyframe;
        if (was_video_keyframe && keyframes > 0)
            keyframes--;

        cur_time = ring.empty() ? 0 : ring.front().dts_usec;
        return was_video_keyframe;
    }

    void purge_one_gop()
    {
        if (!can_purge()) return;

        const bool removed_keyframe = pop_front_packet();
        if (!removed_keyframe) return;

        while (!ring.empty() && can_purge()) {
            const auto& front = ring.front();
            if (front.stream_index == 0 && front.is_keyframe)
                break;
            pop_front_packet();
        }
    }
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

void ReplayBuffer::clear()
{
    std::lock_guard lk(impl_->mutex);
    impl_->ring.clear();
    impl_->total_bytes = 0;
    impl_->cur_time    = 0;
    impl_->keyframes   = 0;
}

void ReplayBuffer::set_video_params(encoding::VideoStreamParams const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->vsp     = p;
    impl_->vsp_set = true;
}

void ReplayBuffer::set_audio_params(encoding::AudioStreamParams const& p)
{
    set_audio_params(std::vector<encoding::AudioStreamParams>{ p });
}

void ReplayBuffer::set_audio_params(std::vector<encoding::AudioStreamParams> const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->audio_params.clear();
    for (const auto& stream : p) {
        if (stream.stream_index < 1 || stream.stream_index > 6) continue;
        if (stream.tb_den == 0 || stream.sample_rate <= 0 || stream.channels <= 0) continue;
        impl_->audio_params.push_back(stream);
    }
}

// ── OBS-style purge: before push, maintain memory + time caps with ≥2 KF ─────

void ReplayBuffer::push(encoding::EncodedPacket pkt)
{
    std::lock_guard lk(impl_->mutex);

    const size_t max_bytes = static_cast<size_t>(impl_->cfg.memory_cap_mb) * 1024 * 1024;
    const int64_t max_time = static_cast<int64_t>(impl_->cfg.duration_sec) * 1000000;

    while (impl_->can_purge() && impl_->total_bytes + pkt.size() > max_bytes)
        impl_->purge_one_gop();

    while (impl_->can_purge() && !impl_->ring.empty() &&
           pkt.dts_usec - impl_->cur_time > max_time)
        impl_->purge_one_gop();

    if (pkt.stream_index == 0 && pkt.is_keyframe)
        impl_->keyframes++;
    if (impl_->ring.empty())
        impl_->cur_time = pkt.dts_usec;
    impl_->total_bytes += pkt.size();
    impl_->ring.push_back(std::move(pkt));
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

ReplayBufferStats ReplayBuffer::stats() const
{
    std::lock_guard lk(impl_->mutex);
    ReplayBufferStats s;
    s.packet_count = impl_->ring.size();
    s.logical_bytes = impl_->total_bytes;
    s.keyframes = impl_->keyframes;
    s.oldest_dts_usec = impl_->ring.empty() ? 0 : impl_->ring.front().dts_usec;
    s.newest_dts_usec = impl_->ring.empty() ? 0 : impl_->ring.back().dts_usec;
    s.saving = impl_->saving.load();
    return s;
}

// ── Clip save internals ───────────────────────────────────────────────────────

// OBS-style: walk forward from ring start, track the last video keyframe
// seen, stop when dts_usec >= cutoff (newest - duration_sec), and return
// that keyframe index.  If no keyframe exists before cutoff, return the
// first keyframe in the ring (or ring.size() if the ring is empty).
static size_t find_clip_start(
    const std::deque<encoding::EncodedPacket>& ring,
    int duration_sec)
{
    if (ring.empty()) return ring.size();

    int64_t cutoff = ring.back().dts_usec
                   - static_cast<int64_t>(duration_sec) * 1000000;

    size_t best = ring.size(); // last KF encountered
    for (size_t i = 0; i < ring.size(); ++i) {
        const auto& p = ring[i];
        if (p.stream_index == 0 && p.is_keyframe)
            best = i;
        if (p.dts_usec >= cutoff)
            break;
    }

    // Fallback: no KF at or before cutoff → use first KF in ring.
    if (best == ring.size()) {
        for (size_t i = 0; i < ring.size(); ++i) {
            if (ring[i].stream_index == 0 && ring[i].is_keyframe)
                return i;
        }
    }

    return best;
}

static std::wstring generate_clip_path(const std::wstring& dir, int duration_sec,
                                       const std::string& container)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    const wchar_t* ext = mux::file_extension(container);
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH,
        L"%s\\%04d%02d%02d_%02d%02d%02d_%ds_clip.%s",
        dir.c_str(),
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        duration_sec,
        ext);
    return path;
}

static std::wstring write_clip(
    std::vector<encoding::EncodedPacket>         pkts,
    const encoding::VideoStreamParams&           vsp,
    const std::vector<encoding::AudioStreamParams>& audio_params,
    const std::wstring&                          out_dir,
    int                                          duration_sec,
    const std::string&                           container)
{
    if (pkts.empty()) return {};
    if (!vsp.tb_den) return {};

    // Ensure the output directory exists.
    CreateDirectoryW(out_dir.c_str(), nullptr);

    std::wstring path     = generate_clip_path(out_dir, duration_sec, container);
    std::string  path_utf = mux::wcs_to_utf8(path);
    if (path_utf.empty()) return {};

    // ── Reorder packets by DTS (OBS-style: B-frame safety) ──────────────────
    std::stable_sort(pkts.begin(), pkts.end(),
        [](const encoding::EncodedPacket& a, const encoding::EncodedPacket& b) {
            return a.dts_usec < b.dts_usec;
        });

    // ── Open output context + streams ───────────────────────────────────────
    AVFormatContext* fmt = nullptr;
    mux::StreamSet streams;
    if (!mux::alloc_output(path_utf, container, vsp, audio_params, &fmt, &streams))
        return {};
    AVStream* vs = streams.video;
    std::array<AVStream*, 7>& audio_streams = streams.audio;

    if (!mux::open_file_and_write_header(fmt, path_utf, container)) {
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        return {};
    }

    // ── Compute PTS/DTS offsets so the clip starts at 0 ──────────────────────
    // One-shot snapshot mode: each stream's anchor is just the first packet
    // seen for it in this already-sorted batch (no pause tracking needed).
    std::array<mux::TimingAnchor, 7> anchors{};
    for (const auto& ep : pkts) {
        if (ep.stream_index == 0) {
            anchors[0].observe(ep.pts, ep.dts);
        } else if (ep.stream_index >= 1 && ep.stream_index <= 6 &&
                   audio_streams[ep.stream_index]) {
            anchors[ep.stream_index].observe(ep.pts, ep.dts);
        }
    }

    // ── Write packets (already in DTS order) ─────────────────────────────────
    for (const auto& ep : pkts) {
        AVStream* dst_stream = nullptr;
        if (ep.stream_index == 0)
            dst_stream = vs;
        else if (ep.stream_index >= 1 && ep.stream_index <= 6)
            dst_stream = audio_streams[ep.stream_index];
        if (!dst_stream) continue;

        const mux::TimingAnchor& anchor = anchors[ep.stream_index];
        mux::write_packet(fmt, dst_stream, ep, anchor.pts, anchor.dts);
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
    std::vector<encoding::AudioStreamParams> audio_params;
    bool vsp_set;
    {
        std::lock_guard lk(impl_->mutex);
        cfg     = impl_->cfg;
        vsp     = impl_->vsp;
        audio_params = impl_->audio_params;
        vsp_set = impl_->vsp_set;

        size_t start = find_clip_start(impl_->ring, impl_->cfg.duration_sec);
        if (start < impl_->ring.size()) {
            snapshot.reserve(impl_->ring.size() - start);
            for (size_t i = start; i < impl_->ring.size(); ++i)
                snapshot.push_back(impl_->ring[i]);
        }
    }

    if (impl_->save_thread.joinable())
        impl_->save_thread.join();

    impl_->save_thread = std::thread(
        [this, snapshot = std::move(snapshot), cfg, vsp,
         audio_params = std::move(audio_params), vsp_set,
         cb = std::move(cb)]() mutable
        {
            std::wstring result;
            if (!snapshot.empty() && vsp_set) {
                result = write_clip(std::move(snapshot), vsp, audio_params,
                                    cfg.output_dir, cfg.duration_sec,
                                    cfg.container);
            }
            impl_->saving.store(false);
            if (cb) cb(result);
        });
}

} // namespace replay_buffer
