#pragma once
#include <encoding/encoding.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace disk_segments {

// Rolling keyframe-aligned buffer of clip segments on disk — the "disk"
// storage mode for the replay buffer. Mirrors replay_buffer::ReplayBuffer's
// public surface so that class can route on its Config::storage. Segments are
// ~2 s long (rolled on video keyframes); retention is age-based, keeping only
// the last Config::duration_sec behind the newest packet.
//
// Each segment is an independent, fully muxed file whose packets restart at
// pts 0, so it can be concatenated losslessly with its neighbours via
// encoding::concat_clip_segments (which also trims the window edges).
class DiskSegmentBuffer {
public:
     DiskSegmentBuffer();
    ~DiskSegmentBuffer();
    DiskSegmentBuffer(const DiskSegmentBuffer&)            = delete;
    DiskSegmentBuffer& operator=(const DiskSegmentBuffer&) = delete;

    struct Config {
        int          duration_sec  = 30;
        std::wstring segment_dir;             // where segment files live
        std::string  container     = "mkv";   // "mkv" | "mp4"
    };

    void configure(Config const& cfg);
    void clear();

    // Must be called before the first push (the first segment is opened with
    // the encoder's stream layout).
    void set_video_params(encoding::VideoStreamParams const& p);
    void set_audio_params(encoding::AudioStreamParams const& p);
    void set_audio_params(std::vector<encoding::AudioStreamParams> const& p);

    // Thread-safe — call from encoder sink callbacks.
    void push(encoding::EncodedPacket pkt);

    // Concatenates the retained window [newest − duration, newest] into a
    // clip in `out_dir` on a worker thread. cb runs on that thread with the
    // output path (empty on failure / empty buffer). Concurrent calls are
    // dropped (one save at a time), mirroring ReplayBuffer::save_clip.
    void save_clip(const std::wstring& out_dir,
                   std::function<void(std::wstring)> cb = nullptr);

    struct Stats {
        size_t   segment_count = 0;
        uint64_t disk_bytes = 0;      // bytes of the retained segment files
        double   window_seconds = 0.0; // coverage of the retained window
        bool     saving = false;
    };
    Stats stats() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace disk_segments
