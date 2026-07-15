#include "segment_replay.h"
#include "ffmpeg_process.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace encoding {

// ── helpers ──────────────────────────────────────────────────────────────────

static std::string wide_to_utf8_local(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

static void ensure_dir(const std::wstring& dir)
{
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
}

static int ceil_div(int a, int b) { return (b <= 0) ? a : (a + b - 1) / b; }

// ── Impl ─────────────────────────────────────────────────────────────────────

struct SegmentReplay::Impl {
    SegmentReplayConfig cfg;
    FfmpegProcess       ffmpeg;

    // Audio named pipe (we are the server; ffmpeg opens it as an input file).
    HANDLE       audio_pipe = INVALID_HANDLE_VALUE;
    std::wstring audio_pipe_name;
    std::atomic<bool> audio_connected{false};
    std::thread  audio_connect_thread;

    std::atomic<bool> running{false};

    std::function<void(const std::string&)> log;

    void log_line(const std::string& s) { if (log) log(s); }
};

SegmentReplay::SegmentReplay()  : impl_(new Impl()) {}
SegmentReplay::~SegmentReplay() { stop(); delete impl_; }

// ── ffmpeg command construction ──────────────────────────────────────────────

// Builds: raw video on stdin (+ raw audio on the named pipe) → CBR encode →
// rotating mpegts segments.
static std::vector<std::string> build_segment_args(
    const SegmentReplayConfig& cfg,
    const std::string&         audio_pipe_utf8,
    const std::string&         segment_pattern_utf8,
    int                        seg_wrap)
{
    std::vector<std::string> a;
    a.push_back("-hide_banner");
    a.push_back("-loglevel"); a.push_back("warning");
    a.push_back("-y");

    // Input 0: raw video from stdin. The pacer guarantees CFR at `fps`.
    a.push_back("-f");            a.push_back("rawvideo");
    a.push_back("-pix_fmt");      a.push_back("bgra");
    a.push_back("-video_size");   a.push_back(std::to_string(cfg.width) + "x" + std::to_string(cfg.height));
    a.push_back("-framerate");    a.push_back(std::to_string(cfg.fps));
    a.push_back("-i");            a.push_back("pipe:0");

    // Input 1: raw interleaved-float audio from the named pipe (mixer is CFR).
    if (cfg.audio_enabled) {
        a.push_back("-f");        a.push_back("f32le");
        a.push_back("-ar");       a.push_back(std::to_string(cfg.audio_sample_rate));
        a.push_back("-ac");       a.push_back(std::to_string(cfg.audio_channels));
        a.push_back("-i");        a.push_back(audio_pipe_utf8);
    }

    // Video encode — CBR, pin avg/max and a 1s buffer to honour the target.
    const std::string br = std::to_string(cfg.bitrate_kbps) + "k";
    a.push_back("-c:v");          a.push_back(cfg.video_encoder);
    a.push_back("-b:v");          a.push_back(br);
    a.push_back("-maxrate");      a.push_back(br);
    a.push_back("-bufsize");      a.push_back(br);
    a.push_back("-pix_fmt");      a.push_back("yuv420p");
    // Keyframe every segment so each .ts starts clean and concat is exact.
    a.push_back("-g");            a.push_back(std::to_string(std::max(1, cfg.fps * cfg.segment_sec)));
    a.push_back("-force_key_frames"); a.push_back("expr:gte(t,n_forced*" + std::to_string(cfg.segment_sec) + ")");
    const bool is_sw = (cfg.video_encoder == "libx264" || cfg.video_encoder == "libx265");
    if (is_sw) {
        a.push_back("-preset");   a.push_back("fast");
        a.push_back("-tune");     a.push_back("zerolatency");
    } else if (cfg.video_encoder.find("nvenc") != std::string::npos) {
        a.push_back("-preset");   a.push_back("p4");
        a.push_back("-rc");       a.push_back("cbr");
    }

    if (cfg.audio_enabled) {
        a.push_back("-c:a");      a.push_back("aac");
        a.push_back("-b:a");      a.push_back("192k");
        a.push_back("-ar");       a.push_back(std::to_string(cfg.audio_sample_rate));
        a.push_back("-ac");       a.push_back(std::to_string(cfg.audio_channels));
    }

    // Rotating mpegts segments. -segment_wrap caps the count on disk; each ~=
    // segment_sec long, so RAM/disk stay flat while ~duration_sec is retained.
    a.push_back("-f");            a.push_back("segment");
    a.push_back("-segment_time"); a.push_back(std::to_string(cfg.segment_sec));
    a.push_back("-segment_wrap"); a.push_back(std::to_string(seg_wrap));
    a.push_back("-segment_format"); a.push_back("mpegts");
    a.push_back("-reset_timestamps"); a.push_back("1");
    a.push_back(segment_pattern_utf8);
    return a;
}

// ── start ────────────────────────────────────────────────────────────────────

bool SegmentReplay::start(const SegmentReplayConfig&              cfg,
                          std::function<void(const std::string&)> log)
{
    if (impl_->running.load()) return false;
    if (cfg.ffmpeg_path.empty() || cfg.width <= 0 || cfg.height <= 0) return false;

    impl_->cfg = cfg;
    impl_->log = std::move(log);

    ensure_dir(cfg.segment_dir);
    ensure_dir(cfg.output_dir);

    // Keep a couple of extra segments beyond the window so save_clip always has
    // enough complete coverage even mid-rotation.
    const int seg_wrap = ceil_div(cfg.duration_sec, std::max(1, cfg.segment_sec)) + 2;

    std::string audio_pipe_utf8;
    if (cfg.audio_enabled) {
        // Unique-ish pipe name per process; ffmpeg opens it as a plain file path.
        impl_->audio_pipe_name =
            L"\\\\.\\pipe\\monolith_replay_audio_" + std::to_wstring(GetCurrentProcessId());
        audio_pipe_utf8 = wide_to_utf8_local(impl_->audio_pipe_name);

        impl_->audio_pipe = CreateNamedPipeW(
            impl_->audio_pipe_name.c_str(),
            PIPE_ACCESS_OUTBOUND,
            PIPE_TYPE_BYTE | PIPE_WAIT,
            1,                    // one instance
            1 << 20, 1 << 20,     // out/in buffer sizes
            0, nullptr);
        if (impl_->audio_pipe == INVALID_HANDLE_VALUE) {
            impl_->log_line("segment_replay: failed to create audio named pipe");
            return false;
        }
    }

    const std::wstring seg_pattern = cfg.segment_dir + L"\\seg_%03d.ts";
    const std::string  seg_pattern_utf8 = wide_to_utf8_local(seg_pattern);

    std::vector<std::string> args =
        build_segment_args(cfg, audio_pipe_utf8, seg_pattern_utf8, seg_wrap);

    // Start ffmpeg first; it will try to open the named pipe as input 1.
    if (!impl_->ffmpeg.start(cfg.ffmpeg_path, args,
                             [this](const std::string& l) { impl_->log_line("ffmpeg: " + l); })) {
        impl_->log_line("segment_replay: ffmpeg failed to start");
        if (impl_->audio_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(impl_->audio_pipe);
            impl_->audio_pipe = INVALID_HANDLE_VALUE;
        }
        return false;
    }

    // Wait (on a helper thread) for ffmpeg to connect to the audio pipe, so
    // push_video can begin immediately without blocking on the audio side.
    if (cfg.audio_enabled) {
        HANDLE pipe = impl_->audio_pipe;
        impl_->audio_connect_thread = std::thread([this, pipe]() {
            BOOL ok = ConnectNamedPipe(pipe, nullptr);
            if (ok || GetLastError() == ERROR_PIPE_CONNECTED) {
                impl_->audio_connected.store(true);
            } else {
                impl_->log_line("segment_replay: ffmpeg never connected to audio pipe");
            }
        });
    }

    impl_->running.store(true);
    return true;
}

bool SegmentReplay::is_running() const
{
    return impl_->running.load() && impl_->ffmpeg.is_running();
}

// ── push ─────────────────────────────────────────────────────────────────────

bool SegmentReplay::push_video(const uint8_t* bgra, size_t size)
{
    if (!impl_->running.load() || !bgra) return false;
    return impl_->ffmpeg.write_stdin(bgra, size);
}

bool SegmentReplay::push_audio(const uint8_t* pcm, size_t size)
{
    if (!impl_->cfg.audio_enabled) return true; // nothing to do
    if (!impl_->running.load() || !pcm) return false;
    if (!impl_->audio_connected.load()) return true; // drop until ffmpeg attaches
    if (impl_->audio_pipe == INVALID_HANDLE_VALUE) return false;

    size_t off = 0;
    while (off < size) {
        DWORD written = 0;
        DWORD chunk = (DWORD)std::min<size_t>(size - off, 1u << 20);
        if (!WriteFile(impl_->audio_pipe, pcm + off, chunk, &written, nullptr) ||
            written == 0)
            return false; // ffmpeg closed the pipe
        off += written;
    }
    return true;
}

// ── save_clip ────────────────────────────────────────────────────────────────

namespace {
struct SegFile {
    std::wstring path;
    ULONGLONG    write_time = 0;
};
}

// Lists the rotating segments, newest last (by last-write time).
static std::vector<SegFile> list_segments(const std::wstring& dir)
{
    std::vector<SegFile> segs;
    WIN32_FIND_DATAW fd{};
    std::wstring glob = dir + L"\\seg_*.ts";
    HANDLE h = FindFirstFileW(glob.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return segs;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        SegFile s;
        s.path = dir + L"\\" + fd.cFileName;
        ULARGE_INTEGER t;
        t.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        s.write_time = t.QuadPart;
        segs.push_back(std::move(s));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(segs.begin(), segs.end(),
              [](const SegFile& a, const SegFile& b) { return a.write_time < b.write_time; });
    return segs;
}

std::wstring SegmentReplay::save_clip()
{
    const SegmentReplayConfig& cfg = impl_->cfg;

    std::vector<SegFile> segs = list_segments(cfg.segment_dir);
    if (segs.empty()) {
        impl_->log_line("segment_replay: no segments to save");
        return {};
    }

    // Take the most recent segments covering ~duration_sec (+1 for the partial
    // head), preserving chronological order for concat.
    const int want = ceil_div(cfg.duration_sec, std::max(1, cfg.segment_sec)) + 1;
    size_t start = (segs.size() > (size_t)want) ? segs.size() - want : 0;

    // Write a concat list file (safe against arbitrary paths).
    std::wstring list_path = cfg.segment_dir + L"\\concat_" +
        std::to_wstring(GetTickCount64()) + L".txt";
    {
        HANDLE lf = CreateFileW(list_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lf == INVALID_HANDLE_VALUE) return {};
        for (size_t i = start; i < segs.size(); ++i) {
            // concat demuxer: file '<path>' — single quotes escaped as '\''.
            std::string line = "file '" + wide_to_utf8_local(segs[i].path) + "'\n";
            DWORD wr = 0;
            WriteFile(lf, line.data(), (DWORD)line.size(), &wr, nullptr);
        }
        CloseHandle(lf);
    }

    // Output clip path.
    SYSTEMTIME st; GetLocalTime(&st);
    const wchar_t* ext = (cfg.save_container == "mp4") ? L"mp4" : L"mkv";
    wchar_t name[MAX_PATH];
    swprintf_s(name, MAX_PATH, L"%s\\%04d%02d%02d_%02d%02d%02d_%ds_clip.%s",
               cfg.output_dir.c_str(), st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, cfg.duration_sec, ext);
    std::wstring out_path = name;

    // Concatenate via stream copy (no re-encode).
    std::vector<std::string> args = {
        "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "concat", "-safe", "0",
        "-i", wide_to_utf8_local(list_path),
        "-c", "copy",
        "-movflags", "+faststart",
        wide_to_utf8_local(out_path),
    };

    FfmpegRunResult r = run_ffmpeg_capture(cfg.ffmpeg_path, args, 60000);
    DeleteFileW(list_path.c_str());

    if (!r.ran || r.exit_code != 0) {
        impl_->log_line("segment_replay: concat failed: " + r.output);
        return {};
    }
    return out_path;
}

// ── stop ─────────────────────────────────────────────────────────────────────

void SegmentReplay::stop()
{
    if (!impl_->running.exchange(false)) return;

    impl_->ffmpeg.stop(8000);

    impl_->audio_connected.store(false);
    if (impl_->audio_pipe != INVALID_HANDLE_VALUE) {
        // Unblock a pending ConnectNamedPipe by disconnecting/closing.
        CancelIoEx(impl_->audio_pipe, nullptr);
        DisconnectNamedPipe(impl_->audio_pipe);
        CloseHandle(impl_->audio_pipe);
        impl_->audio_pipe = INVALID_HANDLE_VALUE;
    }
    if (impl_->audio_connect_thread.joinable())
        impl_->audio_connect_thread.join();
}

} // namespace encoding
