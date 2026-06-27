#pragma once
#include <encoding/encoding.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace replay_buffer {

// Thread-safe rolling ring buffer over EncodedPackets.
// push() is safe to call concurrently from multiple threads.
// save_clip() snapshots the buffer and writes a clip file asynchronously.
struct ReplayBufferStats {
    size_t packet_count = 0;
    size_t logical_bytes = 0;
    int keyframes = 0;
    int64_t oldest_dts_usec = 0;
    int64_t newest_dts_usec = 0;
    bool saving = false;
};

class ReplayBuffer {
public:
     ReplayBuffer();
    ~ReplayBuffer();
    ReplayBuffer(const ReplayBuffer&)            = delete;
    ReplayBuffer& operator=(const ReplayBuffer&) = delete;

    struct Config {
        int          duration_sec  = 30;
        int64_t      memory_cap_mb = 128;
        std::wstring output_dir;
        std::string  container     = "mkv"; // "mkv" | "mp4"
    };

    void configure(Config const& cfg);
    void clear();
    void set_video_params(encoding::VideoStreamParams const& p);
    void set_audio_params(encoding::AudioStreamParams const& p);
    void set_audio_params(std::vector<encoding::AudioStreamParams> const& p);

    // Thread-safe — call from encoder sink callbacks.
    void push(encoding::EncodedPacket pkt);

    // Snapshot the ring buffer and save a clip asynchronously.
    // cb is invoked on the save thread with the output path (empty on failure).
    void save_clip(std::function<void(std::wstring)> cb = nullptr);

    size_t packet_count() const;
    size_t memory_bytes()  const;
    ReplayBufferStats stats() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace replay_buffer
