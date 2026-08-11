#include "disk_segments.h"

#include <encoding/mux_common.h>
#include <encoding/trim.h>

extern "C" {
#include <libavformat/avformat.h>
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
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace disk_segments {

namespace mux = encoding::mux;

// Roll a new segment once the open one has held this much media time (rolled
// only on video keyframes, so every segment starts on a keyframe).
constexpr double kSegmentSeconds = 2.0;
// Sequence counter width in segment filenames.
constexpr int kSegCounterDigits = 6;

struct DiskSegmentBuffer::Impl {
    mutable std::mutex mutex;

    Config cfg;
    encoding::VideoStreamParams vsp;
    std::vector<encoding::AudioStreamParams> audio_params;
    bool vsp_set = false;

    // Open (being-written) segment.
    AVFormatContext*          seg_fmt   = nullptr;
    std::array<AVStream*, 7>  seg_streams{};
    std::array<mux::TimingAnchor, 7> seg_anchor{};
    std::wstring              seg_path;
    double                    seg_start_sec = 0.0; // first video packet time
    double                    seg_now_sec    = 0.0; // newest video packet time
    bool                      seg_has_keyframe = false;

    // Closed segments, oldest first. end_seconds is exclusive and equals the
    // next segment's start_seconds, so the run is contiguous.
    std::vector<encoding::ClipSegment> segments;
    uint64_t                           disk_bytes = 0;

    uint64_t seg_counter = 0;

    std::atomic<bool> saving{false};
    std::thread       save_thread;

    ~Impl()
    {
        if (save_thread.joinable()) save_thread.join();
        close_segment_locked();
    }

    AVStream* stream_for(int stream_index) const
    {
        if (stream_index == 0) return seg_streams[0];
        if (stream_index >= 1 && stream_index <= 6) return seg_streams[stream_index];
        return nullptr;
    }

    std::wstring segment_path_locked(uint64_t seq) const
    {
        const wchar_t* ext = mux::file_extension(cfg.container);
        wchar_t name[64];
        swprintf_s(name, 64, L"seg_%0*u.%s", kSegCounterDigits, static_cast<unsigned>(seq), ext);
        return cfg.segment_dir + L"\\" + name;
    }

    // Opens a fresh segment file (must hold mutex). Returns false on failure;
    // the buffer simply stays without an open segment (next push retries).
    bool open_segment_locked()
    {
        close_segment_locked();
        if (!vsp_set || cfg.segment_dir.empty()) return false;

        const uint64_t seq = seg_counter++;
        seg_path = segment_path_locked(seq);
        std::filesystem::create_directories(std::filesystem::path(cfg.segment_dir));

        AVFormatContext* fmt = nullptr;
        mux::StreamSet streams;
        if (!mux::alloc_output(mux::wcs_to_utf8(seg_path), cfg.container,
                               vsp, audio_params, &fmt, &streams))
            return false;
        if (!mux::open_file_and_write_header(fmt, mux::wcs_to_utf8(seg_path),
                                             cfg.container)) {
            avformat_free_context(fmt);
            return false;
        }
        seg_fmt        = fmt;
        seg_streams    = streams.audio;
        seg_streams[0] = streams.video;
        seg_anchor     = {};
        seg_start_sec  = 0.0;
        seg_now_sec    = 0.0;
        seg_has_keyframe = false;
        return true;
    }

    // Finalizes the open segment into `segments` (must hold mutex).
    void close_segment_locked()
    {
        if (!seg_fmt) return;
        av_write_trailer(seg_fmt);
        avio_closep(&seg_fmt->pb);
        avformat_free_context(seg_fmt);
        seg_fmt = nullptr;

        uint64_t size = 0;
        std::error_code ec;
        const auto fs = std::filesystem::file_size(std::filesystem::path(seg_path), ec);
        if (!ec) size = fs;

        encoding::ClipSegment seg;
        seg.path          = std::move(seg_path);
        seg.start_seconds = seg_start_sec;
        seg.end_seconds   = seg_now_sec;
        segments.push_back(std::move(seg));
        disk_bytes += size;
        seg_path.clear();
    }

    // Age-based retention: drop segments fully older than the window behind
    // the newest packet (must hold mutex). Always keeps the newest segment so
    // a just-started buffer still has something to save. Never runs while a
    // save is in flight — the saver reads segment files from its snapshot and
    // must not race a delete.
    void purge_locked()
    {
        if (saving.load()) return;
        if (segments.size() < 2) return;
        const double cutoff = seg_now_sec - static_cast<double>(cfg.duration_sec);
        while (segments.size() >= 2 && segments.front().end_seconds <= cutoff) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(segments.front().path), ec);
            segments.erase(segments.begin());
        }
    }
};

DiskSegmentBuffer::DiskSegmentBuffer() : impl_(new Impl()) {}
DiskSegmentBuffer::~DiskSegmentBuffer() { delete impl_; }

void DiskSegmentBuffer::configure(Config const& cfg)
{
    std::lock_guard lk(impl_->mutex);
    impl_->cfg = cfg;
}

void DiskSegmentBuffer::clear()
{
    std::lock_guard lk(impl_->mutex);
    impl_->close_segment_locked();
    for (const auto& seg : impl_->segments) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(seg.path), ec);
    }
    impl_->segments.clear();
    impl_->disk_bytes = 0;
}

void DiskSegmentBuffer::set_video_params(encoding::VideoStreamParams const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->vsp     = p;
    impl_->vsp_set = true;
}

void DiskSegmentBuffer::set_audio_params(encoding::AudioStreamParams const& p)
{
    set_audio_params(std::vector<encoding::AudioStreamParams>{ p });
}

void DiskSegmentBuffer::set_audio_params(std::vector<encoding::AudioStreamParams> const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->audio_params.clear();
    for (const auto& stream : p) {
        if (stream.stream_index < 1 || stream.stream_index > 6) continue;
        if (stream.tb_den == 0 || stream.sample_rate <= 0 || stream.channels <= 0) continue;
        impl_->audio_params.push_back(stream);
    }
}

void DiskSegmentBuffer::push(encoding::EncodedPacket pkt)
{
    std::lock_guard lk(impl_->mutex);
    if (!impl_->vsp_set) return; // stream layout unknown yet

    const bool is_video = pkt.stream_index == 0;
    if (is_video) impl_->seg_now_sec = static_cast<double>(pkt.dts_usec) / 1000000.0;

    // Roll to a new segment on a keyframe once the open one is >= 2 s. The
    // roll happens before the keyframe is written so it leads the new segment.
    if (is_video && pkt.is_keyframe && impl_->seg_fmt) {
        if (impl_->seg_has_keyframe &&
            impl_->seg_now_sec - impl_->seg_start_sec >= kSegmentSeconds) {
            impl_->close_segment_locked();
            impl_->purge_locked();
        }
        impl_->seg_has_keyframe = true;
    }

    if (!impl_->seg_fmt && !impl_->open_segment_locked())
        return;
    if (is_video && impl_->seg_start_sec == 0.0)
        impl_->seg_start_sec = impl_->seg_now_sec;

    AVStream* dst = impl_->stream_for(pkt.stream_index);
    if (!dst) return;

    mux::TimingAnchor& anchor = impl_->seg_anchor[pkt.stream_index];
    anchor.observe(pkt.pts, pkt.dts);
    mux::write_packet(impl_->seg_fmt, dst, pkt, anchor.pts, anchor.dts);
}

DiskSegmentBuffer::Stats DiskSegmentBuffer::stats() const
{
    std::lock_guard lk(impl_->mutex);
    Stats s;
    s.segment_count = impl_->segments.size();
    s.disk_bytes    = impl_->disk_bytes;
    s.saving        = impl_->saving.load();
    if (!impl_->segments.empty()) {
        s.window_seconds = impl_->segments.back().end_seconds
                         - impl_->segments.front().start_seconds;
    }
    return s;
}

void DiskSegmentBuffer::save_clip(const std::wstring& out_dir,
                                  std::function<void(std::wstring)> cb)
{
    bool expected = false;
    if (!impl_->saving.compare_exchange_strong(expected, true))
        return;

    Config cfg;
    std::vector<encoding::ClipSegment> segs;
    double newest = 0.0;
    {
        std::lock_guard lk(impl_->mutex);
        // Finalize the open segment first so the save thread only ever reads
        // completed files (the writer reopens a fresh segment on the next
        // push). This rolls at most one extra segment per save — negligible
        // next to the ~2 s segments. Retention pauses while saving (the
        // purge_locked guard), so the snapshot's files stay on disk.
        impl_->close_segment_locked();
        segs   = impl_->segments;
        cfg    = impl_->cfg;
        newest = impl_->seg_now_sec;
    }

    if (impl_->save_thread.joinable())
        impl_->save_thread.join();

    impl_->save_thread = std::thread(
        [this, segs = std::move(segs), cfg, newest, out_dir,
         cb = std::move(cb)]() mutable
        {
            std::wstring result;
            if (!segs.empty() && newest > 0.0) {
                // Window = last duration_sec of the buffer timeline; clamp to
                // what the segments actually cover.
                double start = newest - static_cast<double>(cfg.duration_sec);
                if (start < segs.front().start_seconds)
                    start = segs.front().start_seconds;

                SYSTEMTIME st;
                GetLocalTime(&st);
                const wchar_t* ext = mux::file_extension(cfg.container);
                wchar_t path[MAX_PATH];
                swprintf_s(path, MAX_PATH,
                    L"%s\\%04d%02d%02d_%02d%02d%02d_%ds_clip.%s",
                    out_dir.c_str(),
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond,
                    cfg.duration_sec, ext);
                CreateDirectoryW(out_dir.c_str(), nullptr);

                std::string err;
                if (encoding::concat_clip_segments(segs, start, newest, path, &err))
                    result = path;
            }
            impl_->saving.store(false);
            if (cb) cb(result);
        });
}

} // namespace disk_segments
