#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Continuous manual-recording engine backed by an external ffmpeg.exe.
//
// Same external-process model as SegmentReplay, but writes one continuous output
// file instead of rotating segments. Raw video on stdin, one raw-audio named
// pipe per track, CBR encode. Keeps Monolith's multi-track audio layout.
//
// Pause/resume is handled at the feed level: while paused, the pacer simply
// stops pushing frames and audio is dropped, so wall-clock gaps collapse to the
// CFR the pacer would otherwise emit. (ffmpeg keeps running; nothing is fed.)
//
// Drives ffmpeg via FfmpegProcess; no libav* dependency.

namespace encoding {

class FfmpegProcess; // fwd

struct RecordingProcessConfig {
    std::wstring ffmpeg_path;      // resolved ffmpeg.exe
    std::wstring output_path;      // full path to the output file (with extension)
    std::string  container = "mkv"; // "mkv" | "mp4"

    int  width        = 0;
    int  height       = 0;
    int  fps          = 60;
    int  bitrate_kbps = 20000;     // CBR target
    std::string video_encoder = "libx264";

    struct AudioTrack {
        int stream_index = 1;      // 1..6, Monolith logical track id
        int sample_rate  = 48000;
        int channels     = 2;
    };
    std::vector<AudioTrack> audio_tracks;
};

class RecordingProcess {
public:
     RecordingProcess();
    ~RecordingProcess();
    RecordingProcess(const RecordingProcess&)            = delete;
    RecordingProcess& operator=(const RecordingProcess&) = delete;

    bool start(const RecordingProcessConfig&           cfg,
               std::function<void(const std::string&)> log = nullptr);

    bool is_running() const;

    bool push_video(const uint8_t* bgra, size_t size);
    bool push_audio(int stream_index, const uint8_t* pcm, size_t size);

    // Closes stdin so ffmpeg finalizes the file, waits for exit. Returns the
    // output path on success, or empty on failure.
    std::wstring stop();

    std::wstring output_path() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace encoding
