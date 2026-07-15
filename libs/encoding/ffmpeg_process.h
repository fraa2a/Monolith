#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ffmpeg.exe subprocess helpers.
//
// Monolith keeps the GPL software encoders (x264/x265) out of its own binary by
// invoking an external ffmpeg.exe (a separate program) rather than linking the
// GPL libraries in-process. This module only locates and drives that process; it
// pulls in no FFmpeg headers and has no libav* dependency, so it stays on the
// clean side of the licensing boundary.
//
// Resolution and launch mirror the robust patterns from the Record-able mod
// (FfmpegBundleManager / FFmpegEncoder) minus the auto-download, which is a
// possible later addition.

namespace encoding {

// Directory where an auto-downloaded ffmpeg lives:
// <localAppData>\Monolith\ffmpeg\bin. Returned as an absolute path (empty if
// %LocalAppData% can't be resolved). locate_ffmpeg searches here too.
std::wstring ffmpeg_download_dir();

// Resolves a usable ffmpeg.exe. Order:
//   1. `configured` — explicit user override (settings), if set and it runs.
//   2. Bundled next to the running executable: <exe_dir>\ffmpeg.exe, then
//      <exe_dir>\ffmpeg\ffmpeg.exe, then <exe_dir>\bin\ffmpeg.exe.
//   3. Auto-downloaded copy under ffmpeg_download_dir().
//   4. System PATH ("ffmpeg"), resolved to an absolute path.
// Each candidate is verified by actually running it (`-version`). Returns the
// resolved absolute path, or an empty string if nothing runnable was found.
std::wstring locate_ffmpeg(const std::wstring& configured);

// Same resolution for ffprobe (used by metadata/duration probing). Bundled
// name is ffprobe.exe; falls through to PATH.
std::wstring locate_ffprobe(const std::wstring& configured);

// Downloads a trusted Windows FFmpeg build (gyan.dev release-essentials),
// verifies its SHA-256 against the upstream .sha256, extracts ffmpeg.exe +
// ffprobe.exe into ffmpeg_download_dir(), and returns true on success. Skips
// the work and returns true immediately if a runnable ffmpeg is already present
// there. Blocking + network + disk I/O — call from a background thread only.
// `progress` (optional) receives short human-readable status lines.
bool ensure_ffmpeg_downloaded(std::function<void(const std::string&)> progress = nullptr);

// Verifies a binary launches and self-identifies (runs "<exe> -version" and
// checks the exit code and banner). Safe to call on an untrusted path.
bool verify_ffmpeg_binary(const std::wstring& exe, const char* expect_token);

// Result of a short, capture-to-completion invocation (diagnostics like
// -version / -encoders). NOT for the streaming encode path.
struct FfmpegRunResult {
    bool        ran       = false; // process launched and exited within timeout
    int         exit_code = -1;
    std::string output;            // merged stdout+stderr, UTF-8
};

// Runs `exe args...` to completion, capturing merged stdout+stderr. Intended for
// brief diagnostic calls; kills the process if it overruns `timeout_ms`.
FfmpegRunResult run_ffmpeg_capture(const std::wstring&             exe,
                                   const std::vector<std::string>& args,
                                   int                             timeout_ms = 5000);

// Queries `ffmpeg -encoders` and returns the subset of Monolith's candidate
// video encoders this build actually exposes, in vendor-preference order
// (h264_nvenc, h264_amf, h264_qsv, libx264, hevc_nvenc, hevc_amf, hevc_qsv,
// libx265). Empty if ffmpeg can't be run. Used to populate the Settings UI.
std::vector<std::string> ffmpeg_available_encoders(const std::wstring& ffmpeg_exe);

// Resolves the user's device ("gpu"/"cpu") + codec ("h264"/"h265") to a concrete
// ffmpeg encoder name present in `available` (from ffmpeg_available_encoders).
// GPU tries the vendor HW encoders for the codec (NVENC -> AMF -> QSV) then falls
// back to software; CPU prefers software (libx264/libx265) then HW. For CPU+H.265
// with no libx265, falls back to hardware HEVC, then H.264. Returns "" if nothing
// matches.
std::string ffmpeg_resolve_encoder(const std::string&              device,
                                   const std::string&              codec,
                                   const std::vector<std::string>& available);

// Long-running ffmpeg subprocess with a writable stdin (the raw-frame pipe) and
// a drained stderr. Backs the streaming encode paths (replay segmenter and
// manual recorder). Not thread-safe: drive stdin from a single writer thread.
class FfmpegProcess {
public:
     FfmpegProcess();
    ~FfmpegProcess();
    FfmpegProcess(const FfmpegProcess&)            = delete;
    FfmpegProcess& operator=(const FfmpegProcess&) = delete;

    // Launches `exe` with `args`. stdin is a pipe fed via write_stdin(); stdout
    // is discarded; stderr is drained line-by-line to `log` (may be null).
    // Returns false if the process could not be started.
    bool start(const std::wstring&                      exe,
               const std::vector<std::string>&          args,
               std::function<void(const std::string&)>  log = nullptr);

    bool is_running() const;

    // Writes raw bytes to ffmpeg's stdin. Blocks until handed to the pipe.
    // Returns false if the pipe broke (process exited) — the caller should stop.
    bool write_stdin(const uint8_t* data, size_t size);

    // Closes stdin (signalling ffmpeg to finalize its output) and waits up to
    // timeout_ms for a clean exit, escalating to terminate on overrun. Returns
    // the process exit code, or -1 if it was never started.
    int stop(int timeout_ms = 10000);

    // Process id for logging, or 0 if not running.
    unsigned long pid() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace encoding
