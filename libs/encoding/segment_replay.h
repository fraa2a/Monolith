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

    // One entry per audio track. Each becomes its own named pipe, ffmpeg input,
    // and a separate output audio stream (preserving Monolith's multi-track
    // layout, e.g. game on track 1 + mic on track 2). Empty = video-only. All
    // tracks share the canonical mixer format (interleaved float).
    struct AudioTrack {
        int stream_index = 1;      // 1..6, matches Monolith's logical track id
        int sample_rate  = 48000;
        int channels     = 2;
    };
    std::vector<AudioTrack> audio_tracks;

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

    // Feed raw interleaved-float audio for one track (matching that track's
    // sample_rate/channels). `stream_index` selects the track (1..6). No-op for
    // unknown tracks. Returns false if that track's pipe broke.
    bool push_audio(int stream_index, const uint8_t* pcm, size_t size);

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
