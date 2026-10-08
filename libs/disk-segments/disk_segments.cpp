#include "disk_segments.h"
#include "disk_budget.h"
#include <encoding/mux_common.h>
#include <encoding/trim.h>
extern "C" {
#include <libavformat/avformat.h>
}
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace disk_segments {
namespace mux = encoding::mux;
namespace fs = std::filesystem;
namespace {
constexpr double kSegmentSeconds = 5.0;
std::atomic<uint64_t> directory_sequence{0};
struct Segment {
    encoding::ClipSegment clip;
    uint64_t payload = 0;
    uint64_t file_bytes = 0;
    bool retained = true;
    bool owns_parent = true;
};
}
struct DiskSegmentBuffer::Impl {
    mutable std::mutex mutex;
    // Only lifecycle callers touch save_thread; never held by the save worker.
    std::mutex lifecycle;
    Config cfg;
    encoding::VideoStreamParams vsp{};
    std::vector<encoding::AudioStreamParams> audio;
    bool vsp_set = false;
    bool reconfigure = false;
    AVFormatContext* fmt = nullptr;
    std::array<AVStream*, 7> streams{};
    std::shared_ptr<Segment> current;
    std::vector<std::shared_ptr<Segment>> files; // includes pinned/failed-delete files
    fs::path owned_dir;
    uint64_t sequence = 0;
    uint64_t payload = 0;
    uint64_t dropped = 0;
    uint64_t delete_failures = 0;
    bool pressure = false;
    double newest = 0;
    int64_t anchor_dts = 0;
    AVRational anchor_tb{};
    std::atomic<bool> saving{false};
    std::atomic<bool> cancel{false};
    std::thread save_thread;

    uint64_t budget() const { return static_cast<uint64_t>(validated_disk_budget_mb(cfg.disk_budget_mb)) * 1024 * 1024; }
    void close_segment(bool discard = false) {
        if (!fmt) return;
        const bool ok = av_write_trailer(fmt) >= 0;
        const bool closed = avio_closep(&fmt->pb) >= 0;
        avformat_free_context(fmt); fmt = nullptr;
        std::error_code ec;
        current->file_bytes = fs::file_size(current->clip.path, ec);
        if (ec) current->file_bytes = 0;
        if (discard || !ok || !closed) {
            current->retained = false; pressure = true;
            // A failed write may have emitted bytes before returning an error.
            // Keep those charged if unlink fails; never forget an owned file.
            if (current->file_bytes > current->payload) {
                payload += current->file_bytes - current->payload;
                current->payload = current->file_bytes;
            }
        }
        if (current->retained) {
            // Container start_time can be the first presentation timestamp,
            // not our DTS origin (video-only B frames are the common case).
            AVFormatContext* input = nullptr;
            if (avformat_open_input(&input, mux::wcs_to_utf8(current->clip.path).c_str(), nullptr, nullptr) < 0 ||
                avformat_find_stream_info(input, nullptr) < 0) {
                current->retained = false; pressure = true;
            } else if (input->start_time != AV_NOPTS_VALUE) {
                current->clip.file_origin_seconds = -static_cast<double>(input->start_time) / AV_TIME_BASE;
            }
            avformat_close_input(&input);
        }
        current.reset();
    }
    // Unlink only files created by this instance. A snapshot's shared ownership
    // protects exactly its inputs; failed deletes remain charged and retryable.
    void purge(bool all = false) {
        const double cutoff = newest - std::max(0, cfg.duration_sec);
        for (auto& file : files) {
            if (file == current) continue;
            if (all || file->clip.end_seconds <= cutoff) file->retained = false;
        }
        for (auto it = files.begin(); it != files.end();) {
            auto& file = *it;
            if (file != current && !file->retained && file.use_count() == 1) {
                std::error_code ec;
                fs::remove(file->clip.path, ec);
                if (!ec) {
                    const auto parent = fs::path(file->clip.path).parent_path();
                    const bool owns_parent = file->owns_parent;
                    payload -= file->payload; it = files.erase(it);
                    if (owns_parent && parent != owned_dir) fs::remove(parent, ec); // old owned directory, only if empty
                    continue;
                }
                ++delete_failures; pressure = true;
            }
            ++it;
        }
    }
    bool make_room(uint64_t bytes) {
        purge();
        while (payload > budget() || bytes > budget() - payload || files.size() >= kMaxSegments) {
            auto candidate = std::find_if(files.begin(), files.end(), [&](const auto& file) {
                return file != current && file.use_count() == 1 && file->retained;
            });
            if (candidate == files.end()) break;
            (*candidate)->retained = false;
            purge();
        }
        const bool failed_delete = std::any_of(files.begin(), files.end(), [&](const auto& file) {
            return file != current && !file->retained && file.use_count() == 1;
        });
        return !failed_delete && payload <= budget() && bytes <= budget() - payload && files.size() < kMaxSegments;
    }
    bool ensure_directory() {
        if (!owned_dir.empty()) return true;
        std::error_code ec;
        fs::create_directories(cfg.segment_dir, ec);
        if (ec) return false;
        for (int i = 0; i < 16; ++i) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            auto path = fs::path(cfg.segment_dir) / (L"monolith-segments-" + std::to_wstring(stamp) + L"-" +
                                                     std::to_wstring(directory_sequence.fetch_add(1)));
            if (fs::create_directory(path, ec)) { owned_dir = std::move(path); return true; }
            if (ec) return false;
        }
        return false;
    }
    bool free_space(uint64_t bytes) {
        std::error_code ec;
        auto space = fs::space(owned_dir, ec);
        return !ec && space.available >= kFreeSpaceReserve && bytes <= space.available - kFreeSpaceReserve;
    }
    bool open_segment(const encoding::EncodedPacket& key) {
        if (!ensure_directory()) return false;
        auto file = std::make_shared<Segment>();
        file->clip.path = (owned_dir / (L"seg_" + std::to_wstring(sequence++) + L"." + mux::file_extension(cfg.container))).wstring();
        file->clip.start_seconds = static_cast<double>(key.dts_usec) / 1000000.0;
        file->clip.end_seconds = file->clip.start_seconds;
        // DTS zero is the common mux origin. Preserve composition delay and
        // audio offsets instead of independently zeroing each stream.
        file->clip.file_origin_seconds = 0;
        files.push_back(file); current = file;
        mux::StreamSet set;
        if (!mux::alloc_output(mux::wcs_to_utf8(file->clip.path), cfg.container, vsp, audio, &fmt, &set)) {
            file->retained = false; current.reset(); return false;
        }
        if (!mux::open_file_and_write_header(fmt, mux::wcs_to_utf8(file->clip.path), cfg.container)) {
            avformat_free_context(fmt); fmt = nullptr; file->retained = false; current.reset(); return false;
        }
        streams = set.audio; streams[0] = set.video;
        anchor_dts = key.dts; anchor_tb = {key.tb_num, key.tb_den};
        return true;
    }
    void apply_pending() {
        if (!reconfigure) return;
        close_segment();
        if (!owned_dir.empty() && owned_dir.parent_path() != fs::path(cfg.segment_dir)) {
            std::error_code ec; fs::remove(owned_dir, ec); // empty only; pinned files keep their parent
            owned_dir.clear();
        }
        reconfigure = false;
    }
    void stop_save() {
        cancel.store(true);
        if (save_thread.joinable()) save_thread.join();
    }
    ~Impl() {
        stop_save();
        close_segment(true); purge(true);
        std::error_code ec;
        if (!owned_dir.empty()) fs::remove(owned_dir, ec); // only if empty, never recursive
    }
};
DiskSegmentBuffer::DiskSegmentBuffer() : impl_(new Impl) {}
DiskSegmentBuffer::~DiskSegmentBuffer() { delete impl_; }
void DiskSegmentBuffer::configure(Config const& cfg) {
    std::lock_guard lk(impl_->mutex);
    impl_->reconfigure = impl_->reconfigure || cfg.container != impl_->cfg.container || cfg.segment_dir != impl_->cfg.segment_dir;
    impl_->cfg = cfg;
    impl_->cfg.disk_budget_mb = validated_disk_budget_mb(cfg.disk_budget_mb);
    // configure is called on the message loop: do not add mux/unlink I/O here.
    // The new cap governs the very next admission/snapshot. Physical eviction
    // occurs there; pinned inputs can temporarily exceed a reduced budget.
    impl_->pressure = impl_->payload > impl_->budget();
}
void DiskSegmentBuffer::clear() {
    std::lock_guard lifecycle(impl_->lifecycle);
    impl_->stop_save();
    std::lock_guard lk(impl_->mutex);
    impl_->close_segment(true); impl_->purge(true); impl_->newest = 0;
}
void DiskSegmentBuffer::set_video_params(encoding::VideoStreamParams const& p) {
    std::lock_guard lk(impl_->mutex);
    impl_->vsp = p; impl_->vsp_set = true;
}
void DiskSegmentBuffer::set_audio_params(encoding::AudioStreamParams const& p) { set_audio_params(std::vector<encoding::AudioStreamParams>{p}); }
void DiskSegmentBuffer::set_audio_params(std::vector<encoding::AudioStreamParams> const& params) {
    std::lock_guard lk(impl_->mutex);
    impl_->audio.clear();
    for (const auto& p : params) if (p.stream_index >= 1 && p.stream_index <= 6 && p.tb_num > 0 && p.tb_den > 0 &&
                                   p.sample_rate > 0 && p.channels > 0) impl_->audio.push_back(p);
}
void DiskSegmentBuffer::push(encoding::EncodedPacket pkt) {
    std::lock_guard lk(impl_->mutex);
    if (!impl_->vsp_set) return;
    impl_->apply_pending();
    const bool video = pkt.stream_index == 0;
    const bool key = video && pkt.is_keyframe;
    const double t = static_cast<double>(pkt.dts_usec) / 1000000.0;
    if (video) impl_->newest = std::max(impl_->newest, t);
    if (pkt.empty() || pkt.size() > kMaxSegmentPayload || pkt.tb_num <= 0 || pkt.tb_den <= 0 ||
        pkt.stream_index < 0 || pkt.stream_index > 6) {
        if (video) impl_->close_segment(true);
        ++impl_->dropped; impl_->pressure = true; impl_->purge(); return;
    }
    if (impl_->current && key && t - impl_->current->clip.start_seconds >= kSegmentSeconds) {
        impl_->current->clip.end_seconds = t;
        impl_->close_segment();
    }
    if (impl_->current && (pkt.size() > kMaxSegmentPayload - impl_->current->payload ||
        t - impl_->current->clip.start_seconds > std::max(kSegmentSeconds, static_cast<double>(impl_->cfg.duration_sec)))) {
        impl_->close_segment(true);
        impl_->pressure = true;
    }
    if (!impl_->current && !key) { ++impl_->dropped; impl_->purge(); return; }
    if (!impl_->make_room(pkt.size()) || !impl_->ensure_directory() || !impl_->free_space(pkt.size())) {
        impl_->close_segment(true); impl_->purge(); ++impl_->dropped; impl_->pressure = true; return;
    }
    if (!impl_->fmt && !impl_->open_segment(pkt)) { ++impl_->dropped; impl_->pressure = true; impl_->purge(); return; }
    // Late packets from the prior GOP are not allowed to create negative DTS
    // or an undecodable start in the new segment after a save/roll.
    if (t < impl_->current->clip.start_seconds || !impl_->streams[pkt.stream_index]) { ++impl_->dropped; return; }
    const int64_t offset = av_rescale_q(impl_->anchor_dts, impl_->anchor_tb, AVRational{pkt.tb_num, pkt.tb_den});
    if (!mux::write_packet(impl_->fmt, impl_->streams[pkt.stream_index], pkt, offset, offset)) {
        impl_->close_segment(true); impl_->purge(); ++impl_->dropped; impl_->pressure = true; return;
    }
    impl_->current->payload += pkt.size(); impl_->payload += pkt.size();
    if (video) {
        const double frame_duration = impl_->vsp.fps_num > 0 ? static_cast<double>(impl_->vsp.fps_den) / impl_->vsp.fps_num : 0;
        impl_->current->clip.end_seconds = std::max(impl_->current->clip.end_seconds, t + frame_duration);
    }
    impl_->purge();
}
DiskSegmentBuffer::Stats DiskSegmentBuffer::stats() const {
    std::lock_guard lk(impl_->mutex);
    Stats out;
    out.saving = impl_->saving.load(); out.logical_bytes = impl_->payload;
    out.dropped_packets = impl_->dropped; out.delete_failures = impl_->delete_failures; out.pressure = impl_->pressure;
    double first = impl_->newest;
    for (const auto& file : impl_->files) {
        out.disk_bytes += file->file_bytes;
        if (file == impl_->current && impl_->fmt && impl_->fmt->pb) {
            const int64_t position = avio_tell(impl_->fmt->pb);
            if (position > 0) out.disk_bytes += static_cast<uint64_t>(position);
        }
        if (file->retained) { ++out.segment_count; first = std::min(first, file->clip.start_seconds); }
        if (file.use_count() > (file == impl_->current ? 2 : 1)) out.pinned_bytes += file->payload;
    }
    out.window_seconds = std::max(0.0, impl_->newest - first);
    return out;
}
bool DiskSegmentBuffer::save_clip(const std::wstring& out_dir, std::function<void(std::wstring)> cb) {
    if (impl_->saving.load()) return false; // callback reentry must not wait on clear/teardown
    std::lock_guard lifecycle(impl_->lifecycle);
    bool expected = false;
    if (!impl_->saving.compare_exchange_strong(expected, true)) return false;
    if (impl_->save_thread.joinable()) impl_->save_thread.join();
    impl_->cancel.store(false);
    try {
        std::vector<std::shared_ptr<Segment>> pinned;
        auto failed_output = std::make_shared<Segment>();
        Config cfg;
        double newest = 0;
        {
            std::lock_guard lk(impl_->mutex);
            impl_->files.reserve(kMaxSegments + 1); // one retryable failed save output beyond the segment cap
            impl_->apply_pending(); impl_->close_segment(); cfg = impl_->cfg;
            if (impl_->make_room(0)) for (const auto& file : impl_->files) if (file->retained) {
                pinned.push_back(file); newest = std::max(newest, file->clip.end_seconds);
            }
        }
        impl_->save_thread = std::thread([this, pinned = std::move(pinned), failed_output = std::move(failed_output), cfg, newest, out_dir, cb = std::move(cb)]() mutable {
            try {
            std::wstring result, path;
            try {
                std::vector<encoding::ClipSegment> segs;
                for (const auto& file : pinned) segs.push_back(file->clip);
                if (!segs.empty()) {
                    std::error_code ec; fs::create_directories(out_dir, ec);
                    if (!ec) {
                        path = mux::generate_clip_path(out_dir, cfg.duration_sec, cfg.container);
                        const double start = std::max(segs.front().start_seconds, newest - cfg.duration_sec);
                        std::string err;
                        if (encoding::concat_clip_segments(segs, start, newest, path, &err, nullptr, &impl_->cancel)) result = path;
                    }
                }
            } catch (...) { /* release snapshot and report failure below */ }
            if (result.empty() && !path.empty()) {
                std::error_code ec; fs::remove(path, ec);
                if (ec) {
                    // Retain failed-output cleanup ownership and block admission until unlink succeeds.
                    auto& failed = failed_output;
                    failed->clip.path = std::move(path); failed->retained = false; failed->owns_parent = false;
                    failed->file_bytes = fs::file_size(failed->clip.path, ec);
                    std::lock_guard lk(impl_->mutex);
                    if (ec) failed->file_bytes = impl_->budget(); // unknown size: conservative charge
                    failed->payload = failed->file_bytes;
                    impl_->payload += failed->payload; impl_->files.push_back(std::move(failed));
                    ++impl_->delete_failures; impl_->pressure = true;
                }
            }
            {
                // concat returned and its input contexts are closed. Release
                // pins BEFORE a possibly slow catalog/thumbnail callback.
                std::lock_guard lk(impl_->mutex);
                pinned.clear(); impl_->purge();
            }
            try { if (cb) cb(result); } catch (...) { /* keep lifecycle usable */ }
            } catch (...) {
                { std::lock_guard lk(impl_->mutex); pinned.clear(); }
                try { if (cb) cb({}); } catch (...) {}
            }
            impl_->saving.store(false);
        });
    } catch (...) { impl_->saving.store(false); return false; }
    return true;
}
} // namespace disk_segments
