#pragma once
#include <encoding/encoding.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <set>

namespace replay_buffer::detail {

// Logical payload cap, NOT process RAM: allocator/packet/index overhead and the
// single in-flight save snapshot are additional. Empty packets are never kept.
// A slow save can pin one previous cap's worth of payload while this ring fills.
struct PacketRing {
    using Clock = std::chrono::steady_clock;
    static constexpr size_t kMaxPackets = 262144;
    std::deque<encoding::EncodedPacket> ring;
    size_t total_bytes = 0;
    int keyframes = 0;

    void clear_ring() {
        ring.clear(); arrivals.clear(); timestamps.clear();
        total_bytes = 0; keyframes = 0; watermark = std::numeric_limits<int64_t>::min();
    }

    void configure_limits(int64_t megabytes, int seconds) {
        constexpr size_t unit = 1024 * 1024;
        max_bytes = megabytes > 0 && static_cast<uint64_t>(megabytes) <=
            std::numeric_limits<size_t>::max() / unit ? static_cast<size_t>(megabytes) * unit : 0;
        max_age = seconds > 0 ? static_cast<int64_t>(seconds) * 1000000 : 0;
        trim(Clock::now());
    }

    void push_packet(encoding::EncodedPacket pkt, Clock::time_point now = Clock::now()) {
        watermark = std::max(watermark, pkt.dts_usec);
        trim(now);
        if (pkt.empty() || pkt.size() > max_bytes || max_age == 0 || expired(pkt.dts_usec)) {
            // Missing video data invalidates references in the current GOP.
            // Clear conservatively rather than save a silently broken chain.
            if (pkt.stream_index == 0) clear_ring();
            return;
        }
        while (!ring.empty() && (total_bytes > max_bytes - pkt.size() || ring.size() >= kMaxPackets))
            pop_gop();
        if (ring.empty() && !(pkt.stream_index == 0 && pkt.is_keyframe)) return;
        arrivals.push_back(now);
        timestamps.insert(pkt.dts_usec);
        total_bytes += pkt.size();
        if (pkt.stream_index == 0 && pkt.is_keyframe) ++keyframes;
        ring.push_back(std::move(pkt));
    }

    // Called on push, configure, stats and snapshot: stalled producers do not
    // make expired packets available to a later save. No wall-clock timer thread.
    void trim(Clock::time_point now = Clock::now()) {
        while (!ring.empty() && (total_bytes > max_bytes || max_age == 0 ||
            expired(*timestamps.begin()) ||
            now - arrivals.front() > std::chrono::microseconds(max_age))) pop_gop();
    }

    int64_t oldest_dts() const { return timestamps.empty() ? 0 : *timestamps.begin(); }
    int64_t newest_dts() const { return timestamps.empty() ? 0 : *timestamps.rbegin(); }

    // Scan the entire bounded ring. Audio arrival must not end the search for
    // a video key, nor define the video tail. Selection happens before copying.
    std::vector<encoding::EncodedPacket> snapshot(int seconds) const {
        std::vector<encoding::EncodedPacket> out;
        if (ring.empty() || seconds <= 0) return out;
        int64_t newest_video = std::numeric_limits<int64_t>::min();
        int64_t presentation_tail = newest_video;
        for (const auto& p : ring) if (p.stream_index == 0) {
            newest_video = std::max(newest_video, p.dts_usec);
            if (p.tb_num > 0 && p.tb_den > 0) {
                const long double pts_usec = static_cast<long double>(p.pts) * p.tb_num * 1000000 / p.tb_den;
                if (pts_usec >= std::numeric_limits<int64_t>::min() && pts_usec <= std::numeric_limits<int64_t>::max())
                    presentation_tail = std::max(presentation_tail, static_cast<int64_t>(pts_usec));
            }
        }
        presentation_tail = std::max(presentation_tail, newest_video);
        const int64_t duration = static_cast<int64_t>(seconds) * 1000000;
        const int64_t cutoff = newest_video < std::numeric_limits<int64_t>::min() + duration ?
            std::numeric_limits<int64_t>::min() : newest_video - duration;
        const encoding::EncodedPacket* before = nullptr;
        const encoding::EncodedPacket* first = nullptr;
        for (const auto& p : ring) {
            if (p.stream_index != 0 || !p.is_keyframe) continue;
            if (!first || p.dts_usec < first->dts_usec) first = &p;
            if (p.dts_usec <= cutoff && (!before || p.dts_usec > before->dts_usec)) before = &p;
        }
        const auto* key = before ? before : first;
        if (!key) return out;
        for (const auto& p : ring)
            if (p.dts_usec >= key->dts_usec && p.dts_usec <= (p.stream_index == 0 ? newest_video : presentation_tail)) out.push_back(p);
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.dts_usec != b.dts_usec) return a.dts_usec < b.dts_usec;
            return a.stream_index == 0 && a.is_keyframe && !(b.stream_index == 0 && b.is_keyframe);
        });
        return out;
    }

private:
    size_t max_bytes = 128 * 1024 * 1024;
    int64_t max_age = 30000000;
    int64_t watermark = std::numeric_limits<int64_t>::min();
    std::deque<Clock::time_point> arrivals;
    std::multiset<int64_t> timestamps;

    bool expired(int64_t dts) const {
        // Unsigned subtraction avoids overflow at signed timestamp extremes.
        return dts < watermark && static_cast<uint64_t>(watermark) - static_cast<uint64_t>(dts) >
            static_cast<uint64_t>(max_age);
    }
    void pop_packet() {
        const auto& p = ring.front();
        total_bytes -= p.size();
        if (p.stream_index == 0 && p.is_keyframe) --keyframes;
        timestamps.erase(timestamps.find(p.dts_usec));
        ring.pop_front(); arrivals.pop_front();
    }
    void pop_gop() {
        pop_packet();
        while (!ring.empty() && !(ring.front().stream_index == 0 && ring.front().is_keyframe)) pop_packet();
    }
};
} // namespace replay_buffer::detail
