#include "ffmpeg_process.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <thread>

namespace encoding {

// ── UTF-8 / UTF-16 conversion ──────────────────────────────────────────────────

static std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// ── Command-line quoting (CommandLineToArgvW rules) ─────────────────────────────

// Quotes one argument per the Windows CRT / CommandLineToArgvW convention:
// wrap in quotes, backslashes before a quote (or the closing quote) are doubled.
static void append_quoted_arg(std::wstring& cmd, const std::wstring& arg)
{
    if (!cmd.empty()) cmd.push_back(L' ');

    const bool needs_quotes =
        arg.empty() ||
        arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;

    if (!needs_quotes) {
        cmd += arg;
        return;
    }

    cmd.push_back(L'"');
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }

        if (it == arg.end()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        } else if (*it == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
            cmd.push_back(*it);
        } else {
            cmd.append(backslashes, L'\\');
            cmd.push_back(*it);
        }
    }
    cmd.push_back(L'"');
}

static std::wstring build_command_line(const std::wstring&             exe,
                                       const std::vector<std::string>& args)
{
    std::wstring cmd;
    append_quoted_arg(cmd, exe);
    for (const auto& a : args)
        append_quoted_arg(cmd, utf8_to_wide(a));
    return cmd;
}

// ── Path helpers ────────────────────────────────────────────────────────────────

static std::wstring executable_dir()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    size_t slash = buf.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring{} : buf.substr(0, slash);
}

static bool file_exists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// ── verify_ffmpeg_binary ────────────────────────────────────────────────────────

bool verify_ffmpeg_binary(const std::wstring& exe, const char* expect_token)
{
    if (exe.empty()) return false;
    FfmpegRunResult r = run_ffmpeg_capture(exe, {"-hide_banner", "-version"}, 5000);
    if (!r.ran || r.exit_code != 0) return false;
    if (!expect_token || !*expect_token) return true;

    std::string lo = r.output;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    std::string tok(expect_token);
    std::transform(tok.begin(), tok.end(), tok.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return lo.find(tok) != std::string::npos;
}

// ── locate_ffmpeg / locate_ffprobe ──────────────────────────────────────────────

static std::wstring locate_tool(const std::wstring& configured,
                                const wchar_t*       exe_name,
                                const char*          expect_token)
{
    // 1. Explicit user override.
    if (!configured.empty() && file_exists(configured) &&
        verify_ffmpeg_binary(configured, expect_token))
        return configured;

    // 2. Bundled next to the running executable.
    std::wstring dir = executable_dir();
    if (!dir.empty()) {
        const std::wstring candidates[] = {
            dir + L"\\" + exe_name,
            dir + L"\\ffmpeg\\" + exe_name,
            dir + L"\\bin\\" + exe_name,
        };
        for (const auto& c : candidates) {
            if (file_exists(c) && verify_ffmpeg_binary(c, expect_token))
                return c;
        }
    }

    // 3. System PATH — let verify run the bare name; if it works, resolve to an
    // absolute path via SearchPath for a stable command line.
    std::wstring bare(exe_name);
    if (verify_ffmpeg_binary(bare, expect_token)) {
        wchar_t resolved[MAX_PATH];
        DWORD n = SearchPathW(nullptr, exe_name, nullptr, MAX_PATH, resolved, nullptr);
        if (n > 0 && n < MAX_PATH) return resolved;
        return bare; // usable via PATH even if SearchPath didn't resolve it
    }

    return {};
}

std::wstring locate_ffmpeg(const std::wstring& configured)
{
    return locate_tool(configured, L"ffmpeg.exe", "ffmpeg");
}

std::wstring locate_ffprobe(const std::wstring& configured)
{
    return locate_tool(configured, L"ffprobe.exe", "ffprobe");
}

// ── run_ffmpeg_capture ──────────────────────────────────────────────────────────

FfmpegRunResult run_ffmpeg_capture(const std::wstring&             exe,
                                   const std::vector<std::string>& args,
                                   int                             timeout_ms)
{
    FfmpegRunResult result;
    if (exe.empty()) return result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_out = nullptr, write_out = nullptr;
    if (!CreatePipe(&read_out, &write_out, &sa, 0))
        return result;
    // The child inherits only the write end; keep our read end private.
    SetHandleInformation(read_out, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_out;
    si.hStdError  = write_out;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmd = build_command_line(exe, args);
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_out); // parent no longer needs the write end
    if (!ok) {
        CloseHandle(read_out);
        return result;
    }

    // Drain the pipe until EOF (child closed its write end / exited).
    std::string out;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(read_out, buf, sizeof(buf), &read, nullptr) && read > 0)
        out.append(buf, read);
    CloseHandle(read_out);

    DWORD wait = WaitForSingleObject(pi.hProcess,
                                     timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    } else {
        result.ran = true;
    }

    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = (int)code;
    result.output    = std::move(out);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

// ── Encoder detection ────────────────────────────────────────────────────────

// Monolith's candidate encoders in vendor-preference order.
static const char* kMonolithEncoders[] = {
    "h264_nvenc", "h264_amf", "h264_qsv", "libx264",
    "hevc_nvenc", "hevc_amf", "hevc_qsv", "libx265",
};

std::vector<std::string> ffmpeg_available_encoders(const std::wstring& ffmpeg_exe)
{
    std::vector<std::string> result;
    if (ffmpeg_exe.empty()) return result;

    FfmpegRunResult r = run_ffmpeg_capture(ffmpeg_exe, {"-hide_banner", "-encoders"}, 8000);
    if (!r.ran) return result;

    // `ffmpeg -encoders` lines look like:
    //   " V....D h264_nvenc           NVIDIA NVENC H.264 encoder"
    // The first whitespace-delimited token after the capability flags is the
    // encoder name. Rather than rely on a fixed flag-field width (fragile), we
    // scan each candidate name as a whole word in the output — robust against
    // format drift across ffmpeg versions.
    const std::string& out = r.output;
    for (const char* name : kMonolithEncoders) {
        std::string needle = name;
        size_t pos = 0;
        bool found = false;
        while ((pos = out.find(needle, pos)) != std::string::npos) {
            const bool left_ok  = (pos == 0) ||
                (unsigned char)out[pos - 1] <= ' ';
            const size_t end = pos + needle.size();
            const bool right_ok = (end >= out.size()) ||
                (unsigned char)out[end] <= ' ';
            if (left_ok && right_ok) { found = true; break; }
            pos = end;
        }
        if (found) result.push_back(name);
    }
    return result;
}

std::string ffmpeg_resolve_encoder(const std::string&              device,
                                   const std::string&              codec,
                                   const std::vector<std::string>& available)
{
    auto has = [&](const std::string& n) {
        for (const auto& a : available) if (a == n) return true;
        return false;
    };
    auto first = [&](std::initializer_list<const char*> names) -> std::string {
        for (const char* n : names) if (has(n)) return n;
        return {};
    };

    const bool h265 = (codec == "h265" || codec == "hevc");

    if (device == "cpu") {
        if (!h265) {
            std::string r = first({"libx264"});
            if (!r.empty()) return r;
            return first({"h264_nvenc", "h264_amf", "h264_qsv"});
        }
        // CPU + H.265: prefer software x265, else fall back to HW HEVC, else H.264.
        std::string r = first({"libx265"});
        if (!r.empty()) return r;
        r = first({"hevc_nvenc", "hevc_amf", "hevc_qsv"});
        if (!r.empty()) return r;
        return first({"libx264", "h264_nvenc", "h264_amf", "h264_qsv"});
    }

    // device == "gpu" (default): HW first, then software.
    if (!h265) {
        std::string r = first({"h264_nvenc", "h264_amf", "h264_qsv"});
        if (!r.empty()) return r;
        return first({"libx264"});
    }
    std::string r = first({"hevc_nvenc", "hevc_amf", "hevc_qsv"});
    if (!r.empty()) return r;
    return first({"libx265", "h264_nvenc", "h264_amf", "h264_qsv", "libx264"});
}

// ── FfmpegProcess ────────────────────────────────────────────────────────────────

struct FfmpegProcess::Impl {
    HANDLE       proc        = nullptr;
    HANDLE       stdin_write = nullptr;
    HANDLE       stderr_read = nullptr;
    std::thread  stderr_thread;
    unsigned long pid        = 0;
};

FfmpegProcess::FfmpegProcess()  : impl_(new Impl()) {}
FfmpegProcess::~FfmpegProcess() { stop(3000); delete impl_; }

bool FfmpegProcess::start(const std::wstring&                     exe,
                          const std::vector<std::string>&         args,
                          std::function<void(const std::string&)> log)
{
    if (impl_->proc || exe.empty()) return false;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE in_read = nullptr, in_write = nullptr;
    HANDLE err_read = nullptr, err_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 8 * 1024 * 1024))
        return false;
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&err_read, &err_write, &sa, 0)) {
        CloseHandle(in_read); CloseHandle(in_write);
        return false;
    }
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = in_read;
    si.hStdOutput  = GetStdHandle(STD_OUTPUT_HANDLE); // discarded by ffmpeg (-f to file)
    si.hStdError   = err_write;

    std::wstring cmd = build_command_line(exe, args);
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // Close the ends now owned by the child (or unused on failure).
    CloseHandle(in_read);
    CloseHandle(err_write);
    if (!ok) {
        CloseHandle(in_write);
        CloseHandle(err_read);
        return false;
    }
    CloseHandle(pi.hThread);

    impl_->proc        = pi.hProcess;
    impl_->stdin_write = in_write;
    impl_->stderr_read = err_read;
    impl_->pid         = pi.dwProcessId;

    // Drain stderr so ffmpeg never blocks on a full pipe; forward lines to log.
    HANDLE err = err_read;
    impl_->stderr_thread = std::thread([err, log]() {
        std::string line;
        char buf[4096];
        DWORD read = 0;
        while (ReadFile(err, buf, sizeof(buf), &read, nullptr) && read > 0) {
            for (DWORD i = 0; i < read; ++i) {
                char c = buf[i];
                if (c == '\n' || c == '\r') {
                    if (!line.empty()) {
                        if (log) log(line);
                        line.clear();
                    }
                } else {
                    line.push_back(c);
                }
            }
        }
        if (!line.empty() && log) log(line);
    });

    return true;
}

bool FfmpegProcess::is_running() const
{
    if (!impl_->proc) return false;
    return WaitForSingleObject(impl_->proc, 0) == WAIT_TIMEOUT;
}

bool FfmpegProcess::write_stdin(const uint8_t* data, size_t size)
{
    if (!impl_->stdin_write || !data) return false;
    size_t off = 0;
    while (off < size) {
        DWORD written = 0;
        DWORD chunk = (DWORD)std::min<size_t>(size - off, 1u << 20);
        if (!WriteFile(impl_->stdin_write, data + off, chunk, &written, nullptr) ||
            written == 0)
            return false; // pipe broken → process exited
        off += written;
    }
    return true;
}

int FfmpegProcess::stop(int timeout_ms)
{
    if (!impl_->proc) return -1;

    // Closing stdin is ffmpeg's cue to flush and finalize the output file.
    if (impl_->stdin_write) {
        CloseHandle(impl_->stdin_write);
        impl_->stdin_write = nullptr;
    }

    DWORD wait = WaitForSingleObject(impl_->proc,
                                     timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(impl_->proc, 1);
        WaitForSingleObject(impl_->proc, 2000);
    }

    if (impl_->stderr_thread.joinable())
        impl_->stderr_thread.join();
    if (impl_->stderr_read) {
        CloseHandle(impl_->stderr_read);
        impl_->stderr_read = nullptr;
    }

    DWORD code = (DWORD)-1;
    GetExitCodeProcess(impl_->proc, &code);
    CloseHandle(impl_->proc);
    impl_->proc = nullptr;
    impl_->pid  = 0;
    return (int)code;
}

unsigned long FfmpegProcess::pid() const
{
    return impl_->pid;
}

} // namespace encoding
