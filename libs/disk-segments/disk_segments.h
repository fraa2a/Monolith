#pragma once
#include <encoding/encoding.h>
#include "disk_budget.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace disk_segments {

// Roll segments at video keys near five seconds. Each file uses a common decode
// origin with composition/audio offsets for later concatenation.
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
        int disk_budget_mb = kDefaultDiskBudgetMb; // live + pinned encoded payload, not output/overhead
    };

    void configure(Config const& cfg);
    void clear();

    // Must be called before the first push (the first segment is opened with
    // the encoder's stream layout).
    void set_video_params(encoding::VideoStreamParams const& p);
    void set_audio_params(encoding::AudioStreamParams const& p);
    void set_audio_params(std::vector<encoding::AudioStreamParams> const& p);

    // Encoder callbacks may push concurrently; serialize lifecycle changes.
    void push(encoding::EncodedPacket pkt);

    // Save runs on a worker; an empty path reports failure. A rejected concurrent
    // save returns false without invoking cb. The callback must not clear or destroy
    // the buffer; teardown cancels I/O and joins the worker.
    bool save_clip(const std::wstring& out_dir,
                   std::function<void(std::wstring)> cb = nullptr);

    struct Stats {
        size_t   segment_count = 0;
        uint64_t disk_bytes = 0;      // bytes of the retained segment files
        double   window_seconds = 0.0; // coverage of the retained window
        bool     saving = false;
        uint64_t logical_bytes = 0;
        uint64_t pinned_bytes = 0;
        uint64_t dropped_packets = 0;
        uint64_t delete_failures = 0;
        bool pressure = false;
    };
    Stats stats() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace disk_segments
