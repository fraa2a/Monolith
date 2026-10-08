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
// Lossless cuts include reference/keyframe preroll. Audio keeps packet boundaries
// and offsets; TrimResult describes the retained interval.
bool trim_clip_lossless(const std::wstring& path, double start, double end,
                        const std::wstring& out_path, std::string* err,
                        TrimResult* result = nullptr);
// Re-encode in the same codec, retaining VFR and copying all audio tracks.
// Unsupported format/encoder/container returns failure.
bool trim_clip_reencode(const std::wstring& path, double start, double end,
                        const std::wstring& out_path, std::string* err,
                        TrimResult* result = nullptr);
bool concat_clip_segments(const std::vector<ClipSegment>& segs,
                          double start_seconds, double end_seconds,
                          const std::wstring& out_path, std::string* err,
                          TrimResult* result = nullptr,
                          const std::atomic<bool>* cancel = nullptr);
} // namespace encoding
