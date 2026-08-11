#pragma once

#include <string>
#include <vector>

namespace encoding {

// One segment of a video file on disk (produced by the disk-backed replay
// buffer; each segment starts on a keyframe). start_seconds/end_seconds are
// the media-time window the segment covers in the original clip timeline.
struct ClipSegment {
    std::wstring path;
    double       start_seconds = 0.0;
    double       end_seconds   = 0.0; // exclusive; == next segment's start
};

// Lossless trim: remuxes the [start, end] seconds of `path` into `out_path`
// with stream copy for every video/audio stream. Seeks to the keyframe at or
// before `start` (playback-safe start, matching ffmpeg `-ss` input seeking)
// and cuts at the first packet past `end`; output pts/dts are re-anchored to
// zero. Works for any codec the container accepts (H.264/H.265/AV1 in mkv,
// H.264/H.265 in mp4). Returns false + `err` on failure — `out_path` may be a
// partial file and the caller must delete it.
bool trim_clip_lossless(const std::wstring& path,
                        double start, double end,
                        const std::wstring& out_path,
                        std::string* err);

// Re-encode fallback for trim when stream copy is impossible (e.g. codec the
// target container can't mux). Decodes [start, end] and re-encodes
// H.264 + AAC via VideoEncoder/AudioEncoder (probe-selected encoder, CBR at
// the source bitrate). Same success/error contract as trim_clip_lossless.
bool trim_clip_reencode(const std::wstring& path,
                        double start, double end,
                        const std::wstring& out_path,
                        std::string* err);

// Trims a run of keyframe-aligned segments down to [start_seconds,
// end_seconds] of the original clip timeline and concatenates the survivors
// into `out_path`. Segments fully outside the window are dropped; the first
// covered segment is cut at `start` (keyframe-aligned) and the last at `end`.
// Container is derived from out_path's extension ("mp4" vs mkv/matroska).
// Used by quick trim on disk-backed clips and by save_clip for disk storage.
// Returns false + `err` on failure — `out_path` may be partial; caller deletes.
bool concat_clip_segments(const std::vector<ClipSegment>& segs,
                          double start_seconds, double end_seconds,
                          const std::wstring& out_path,
                          std::string* err);

} // namespace encoding
