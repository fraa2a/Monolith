#include "replay_buffer.h"
#include "packet_ring.h"

#include <disk-segments/disk_segments.h>
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
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace replay_buffer {

namespace mux = encoding::mux;

// ── Impl ──────────────────────────────────────────────────────────────────────

struct ReplayBuffer::Impl : detail::PacketRing {
    mutable std::mutex              mutex;
    Config                          cfg;
    encoding::VideoStreamParams     vsp;
    std::vector<encoding::AudioStreamParams> audio_params;
    bool                            vsp_set = false;

    // Disk storage mode ("disk" in Config::storage): all packet traffic is
    // forwarded to this; the ring above stays empty. Created/destroyed on
    // configure() so switching storage mid-run drops the old state.
    std::unique_ptr<disk_segments::DiskSegmentBuffer> disk;

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
    impl_->configure_limits(cfg.memory_cap_mb, cfg.duration_sec);
    const bool want_disk = (cfg.storage == "disk");
    if (want_disk && !impl_->disk) {
        // Switching ram → disk: drop the ring, spin up the segment buffer.
        impl_->clear_ring();
        impl_->disk = std::make_unique<disk_segments::DiskSegmentBuffer>();
        impl_->disk->configure(disk_segments::DiskSegmentBuffer::Config{
            cfg.duration_sec, cfg.segment_dir, cfg.container, cfg.disk_budget_mb });
        // Forward already-known stream params (settings reload path).
        if (impl_->vsp_set) impl_->disk->set_video_params(impl_->vsp);
        if (!impl_->audio_params.empty()) impl_->disk->set_audio_params(impl_->audio_params);
    } else if (!want_disk && impl_->disk) {
        // Switching disk → ram: the segment files are already finalized;
        // discard the buffer (and its segments on disk).
        impl_->disk.reset();
    } else if (want_disk && impl_->disk) {
        impl_->disk->configure(disk_segments::DiskSegmentBuffer::Config{
            cfg.duration_sec, cfg.segment_dir, cfg.container, cfg.disk_budget_mb });
    }
}

void ReplayBuffer::clear()
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->disk) { impl_->disk->clear(); return; }
    impl_->clear_ring();
}

void ReplayBuffer::set_video_params(encoding::VideoStreamParams const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->vsp     = p;
    impl_->vsp_set = true;
    if (impl_->disk) impl_->disk->set_video_params(p);
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
    if (impl_->disk) impl_->disk->set_audio_params(impl_->audio_params);
}

// Hard RAM retention limits are independent of keyframe availability.

void ReplayBuffer::push(encoding::EncodedPacket pkt)
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->disk) {
        impl_->disk->push(std::move(pkt));
        return;
    }

    impl_->push_packet(std::move(pkt));
}

size_t ReplayBuffer::packet_count() const
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->disk) return impl_->disk->stats().segment_count;
    impl_->trim();
    return impl_->ring.size();
}

size_t ReplayBuffer::memory_bytes() const
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->disk) return static_cast<size_t>(impl_->disk->stats().disk_bytes);
    impl_->trim();
    return impl_->total_bytes;
}

ReplayBufferStats ReplayBuffer::stats() const
{
    std::lock_guard lk(impl_->mutex);
    ReplayBufferStats s;
    if (impl_->disk) {
        const auto ds = impl_->disk->stats();
        s.packet_count  = ds.segment_count;
        s.logical_bytes = static_cast<size_t>(ds.logical_bytes);
        s.disk_dropped_packets = ds.dropped_packets;
        s.disk_delete_failures = ds.delete_failures;
        s.disk_pressure = ds.pressure;
        s.keyframes     = static_cast<int>(ds.segment_count);
        s.saving        = ds.saving;
        return s;
    }
    impl_->trim();
    s.packet_count = impl_->ring.size();
    s.logical_bytes = impl_->total_bytes;
    s.keyframes = impl_->keyframes;
    s.oldest_dts_usec = impl_->oldest_dts();
    s.newest_dts_usec = impl_->newest_dts();
    s.saving = impl_->saving.load();
    return s;
}

// ── Clip save internals ───────────────────────────────────────────────────────

static std::wstring generate_clip_path(const std::wstring& dir, int duration_sec,
                                       const std::string& container)
{
    return mux::generate_clip_path(dir, duration_sec, container);
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
        // pb is already closed by open_file_and_write_header on failure.
        avformat_free_context(fmt);
        return {};
    }

    // One presentation origin for every stream, preserving A/V offsets and
    // video composition delay. The snapshot starts at a video key in DTS order.
    const auto& key = pkts.front();
    const AVRational anchor_tb{key.tb_num, key.tb_den};

    // ── Write packets (already in DTS order) ─────────────────────────────────
    for (const auto& ep : pkts) {
        AVStream* dst_stream = nullptr;
        if (ep.stream_index == 0)
            dst_stream = vs;
        else if (ep.stream_index >= 1 && ep.stream_index <= 6)
            dst_stream = audio_streams[ep.stream_index];
        if (!dst_stream) continue;

        const int64_t anchor = av_rescale_q(key.pts, anchor_tb, AVRational{ep.tb_num, ep.tb_den});
        if (!mux::write_packet(fmt, dst_stream, ep, anchor, anchor)) {
            avio_closep(&fmt->pb);
            avformat_free_context(fmt);
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return {};
        }
    }

    const bool finalized = av_write_trailer(fmt) >= 0;
    const bool closed = avio_closep(&fmt->pb) >= 0;
    avformat_free_context(fmt);
    if (!finalized || !closed) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return {};
    }
    return path;
}

// ── save_clip ─────────────────────────────────────────────────────────────────

void ReplayBuffer::save_clip(std::function<void(std::wstring)> cb)
{
    // Drop concurrent save requests - don't queue.
    bool expected = false;
    if (!impl_->saving.compare_exchange_strong(expected, true))
        return;

    // Disk mode: hand off to the segment buffer (it owns its own save
    // thread); the facade's saving flag mirrors it so stats() stays honest.
    // Read cfg/disk under the lock - configure() can swap storage modes and
    // reset the disk buffer concurrently (settings reload thread). RAM mode
    // (disk == nullptr, the normal configuration) falls through to the
    // snapshot path below.
    {
        std::lock_guard lk(impl_->mutex);
        if (impl_->disk) {
            const bool accepted = impl_->disk->save_clip(impl_->cfg.output_dir,
                [this, cb = std::move(cb)](std::wstring path) mutable {
                    // Run the completion callback before clearing the flag so
                    // a follow-up save cannot join this thread while its
                    // callback (catalog/thumbnail work) is still running.
                    if (cb) cb(std::move(path));
                    impl_->saving.store(false);
                });
            // The segment buffer dropped the request (its own save still
            // running): release our flag too, or saves would stall forever.
            if (!accepted) impl_->saving.store(false);
            return;
        }
    }

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

        impl_->trim();
        snapshot = impl_->snapshot(impl_->cfg.duration_sec);
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
            // Callback first, flag after: see the disk branch above.
            if (cb) cb(result);
            impl_->saving.store(false);
        });
}

} // namespace replay_buffer
