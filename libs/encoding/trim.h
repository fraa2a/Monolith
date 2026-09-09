#pragma once
#include <atomic>
#include <string>
#include <optional>
#include <vector>

namespace encoding {
struct ClipSegment {
    std::wstring path;
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    // Presentation origin within the file, relative to its format start_time.
    // Disk segments use a common DTS anchor, so reordered video can start > 0.
    double file_origin_seconds = 0.0;
};
struct TrimResult {
    double start_seconds = 0.0; // actual retained presentation anchor in source
    double duration_seconds = 0.0; // actual output presentation extent
    std::optional<double> retime_bookmark(double source_seconds) const {
        const double shifted = source_seconds - start_seconds;
        if (shifted >= 0 && shifted < duration_seconds) return shifted;
        return std::nullopt;
    }
};
// Lossless is keyframe-aware, not exact: preceding keyframe and required
// reordered reference packets can extend the requested interval. Audio is
// copied at packet boundaries, with its original offset, not sample-trimmed.
// Results describe the retained timeline; failed output is removed by caller.
bool trim_clip_lossless(const std::wstring& path, double start, double end,
                        const std::wstring& out_path, std::string* err,
                        TrimResult* result = nullptr);
// Explicit same-video-codec software fallback. Decode preroll, filter decoded
// frame timestamps, preserve VFR/timebase and copy ALL audio tracks unchanged.
// Unsupported encoder/pixel format/container fails; never substitutes a codec
// or silently drops audio. No packet vectors proportional to clip duration.
bool trim_clip_reencode(const std::wstring& path, double start, double end,
                        const std::wstring& out_path, std::string* err,
                        TrimResult* result = nullptr);
bool concat_clip_segments(const std::vector<ClipSegment>& segs,
                          double start_seconds, double end_seconds,
                          const std::wstring& out_path, std::string* err,
                          TrimResult* result = nullptr,
                          const std::atomic<bool>* cancel = nullptr);
} // namespace encoding
