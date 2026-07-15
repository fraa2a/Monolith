#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Segment-based replay engine backed by an external ffmpeg.exe.
//
// Instead of buffering encoded packets in RAM (the in-process ring), this feeds
// raw video (stdin) and raw mixed audio (a Windows named pipe) into one ffmpeg
// process that writes short, rotating .ts segments to a temp directory
// (-f segment -segment_wrap). Only the last few seconds of segments exist on
// disk at any time, so RAM stays flat and the GPL software encoders (x264/x265)
// live in a separate process — keeping Monolith's own binary GPL-free.
//
// save_clip() concatenates the most recent complete segments (stream copy, no
// re-encode) into the final clip.
//
// This module drives ffmpeg via FfmpegProcess and does not link libav*.

namespace encoding {

class FfmpegProcess; // fwd

struct SegmentReplayConfig {
    std::wstring ffmpeg_path;      // resolved ffmpeg.exe (locate_ffmpeg())
    std::wstring segment_dir;      // scratch dir for rotating .ts segments
    std::wstring output_dir;       // where saved clips land
    std::string  save_container = "mkv"; // "mkv" | "mp4" for the concatenated clip

    int  width        = 0;         // encoded video size
    int  height       = 0;
    int  fps          = 60;
    int  bitrate_kbps = 20000;     // CBR target

    // Concrete ffmpeg video encoder (e.g. "libx264", "h264_nvenc"). Resolved by
    // the caller from device/codec + probe.
    std::string video_encoder = "libx264";

    // Audio input format (matches the TrackMixer canonical output). Set
    // audio_enabled=false for a video-only replay.
    bool audio_enabled     = true;
    int  audio_sample_rate = 48000;
    int  audio_channels    = 2;

    int  duration_sec  = 30;       // how much history to keep
    int  segment_sec   = 2;        // length of each rotating segment
};

// Owns the running ffmpeg segmenter plus the audio named pipe. Not thread-safe
// for start/stop; push_video/push_audio may each be called from their own
// single producer thread (video pacer, audio mixer) once started.
class SegmentReplay {
public:
     SegmentReplay();
    ~SegmentReplay();
    SegmentReplay(const SegmentReplay&)            = delete;
    SegmentReplay& operator=(const SegmentReplay&) = delete;

    // Starts the ffmpeg segmenter and (if audio_enabled) the audio pipe.
    // log receives ffmpeg stderr lines. Returns false on failure.
    bool start(const SegmentReplayConfig&               cfg,
               std::function<void(const std::string&)>  log = nullptr);

    bool is_running() const;

    // Feed one raw BGRA frame (pacer-driven, CFR). stride = bytes per row.
    // Returns false if the pipe broke (ffmpeg exited).
    bool push_video(const uint8_t* bgra, size_t size);

    // Feed raw interleaved-float audio matching cfg.audio_sample_rate/channels.
    // No-op when audio is disabled. Returns false if the audio pipe broke.
    bool push_audio(const uint8_t* pcm, size_t size);

    // Concatenates the most recent complete segments covering ~duration_sec into
    // a clip in output_dir. Runs synchronously on the calling thread; intended to
    // be called from a background save thread. Returns the clip path, or empty
    // on failure.
    std::wstring save_clip();

    // Stops ffmpeg (finalizes the current segment) and tears down the pipe.
    void stop();

private:
    struct Impl;
    Impl* impl_;
};

} // namespace encoding
