#pragma once
#include <encoding/encoding.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace replay_buffer {

// Serialize lifecycle changes; push/save use synchronized packet state.
struct ReplayBufferStats {
    size_t packet_count = 0;
    // RAM: retained encoded payload only, excluding bounded packet/index
    // metadata, allocator overhead and payload pinned by an in-flight save.
    size_t logical_bytes = 0;
    int keyframes = 0;
    int64_t oldest_dts_usec = 0;
    int64_t newest_dts_usec = 0;
    bool saving = false;
    uint64_t disk_dropped_packets = 0;
    uint64_t disk_delete_failures = 0;
    bool disk_pressure = false;
};

class ReplayBuffer {
public:
     ReplayBuffer();
    ~ReplayBuffer();
    void wait_for_saves();
    ReplayBuffer(const ReplayBuffer&)            = delete;
    ReplayBuffer& operator=(const ReplayBuffer&) = delete;

    struct Config {
        int          duration_sec  = 30;
        // Hard logical payload ceiling in RAM mode; invalid/nonpositive limits
        // disable retention. Pressure can empty the ring until a new keyframe.
        int64_t      memory_cap_mb = 128;
        std::wstring output_dir;
        std::string  container     = "mkv"; // "mkv" | "mp4"
        // Where the rolling buffer lives: "ram" (in-memory ring, default) or
        // "disk" (keyframe-aligned segments written to segment_dir). In disk
        // mode memory_cap_mb is ignored; retention is age-based.
        std::string  storage       = "ram"; // "ram" | "disk"
        std::wstring segment_dir;           // disk mode: segment file location
        int disk_budget_mb = 2048; // validated 512..65536 MiB; backend setting
    };

    void configure(Config const& cfg);
    void clear();
    void set_video_params(encoding::VideoStreamParams const& p);
    void set_audio_params(encoding::AudioStreamParams const& p);
    void set_audio_params(std::vector<encoding::AudioStreamParams> const& p);

    // Encoder callbacks may push concurrently; serialize lifecycle changes.
    void push(encoding::EncodedPacket pkt);

    // Snapshot the ring buffer and save a clip asynchronously.
    // cb is invoked on the save thread with the output path (empty on failure).
    void save_clip(std::function<void(std::wstring)> cb = nullptr);

    // RAM stats/count and save trim expired packets even if producers stopped.
    // Zero packets/keyframes means no replay is currently available.
    size_t packet_count() const;
    size_t memory_bytes()  const;
    ReplayBufferStats stats() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace replay_buffer
