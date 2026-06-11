#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>

#include <winrt/base.h>

#include <capture/capture.h>
#include <audio/audio.h>
#include <encoding/encoding.h>
#include <replay-buffer/replay_buffer.h>

#include <cstdio>
#include <mutex>

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr WCHAR kWindowClass[] = L"WinRecorderMsgWnd";
static constexpr WCHAR kAppName[]     = L"Windows Recorder";
static constexpr WCHAR kMutexName[]   = L"WinRecorder_SingleInstance";
static constexpr UINT  WM_TRAYICON   = WM_APP + 1;

enum Cmd : UINT {
    CMD_SAVE_REPLAY     = 1001,
    CMD_RECORDING_START = 1002,
    CMD_RECORDING_STOP  = 1003,
    CMD_PAUSE_RESUME    = 1004,
    CMD_SETTINGS        = 1005,
    CMD_EXIT            = 1006,
};

enum HotkeyId : int {
    HK_SAVE_REPLAY = 1,
};

// ── Logging ───────────────────────────────────────────────────────────────────

static FILE*      g_log       = nullptr;
static std::mutex g_log_mutex;

static void log_init()
{
    WCHAR dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    StringCchCatW(dir, MAX_PATH, L"\\WindowsRecorder");
    CreateDirectoryW(dir, nullptr);
    WCHAR path[MAX_PATH];
    StringCchCopyW(path, MAX_PATH, dir);
    StringCchCatW(path, MAX_PATH, L"\\recorder.log");
    _wfopen_s(&g_log, path, L"a");
}

static void log_msg(const char* tag, const char* msg)
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "[%04d-%02d-%02dT%02d:%02d:%02dZ] [%-12s] %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        tag, msg);
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        if (g_log) { fputs(buf, g_log); fflush(g_log); }
    }
    OutputDebugStringA(buf);
}

// ── Globals ───────────────────────────────────────────────────────────────────

static capture::DisplayCapture      g_video;
static audio::WasapiCapture         g_audio_system;
static audio::WasapiCapture         g_audio_mic;
static encoding::VideoEncoder       g_video_enc;
static encoding::AudioEncoder       g_audio_enc;
static replay_buffer::ReplayBuffer  g_replay;

static int g_enc_w = 0; // configured encoder width (even-aligned)
static int g_enc_h = 0; // configured encoder height (even-aligned)

// ── Media start / stop ────────────────────────────────────────────────────────

static void media_start(HWND hwnd)
{
    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);

    // ── Determine monitor resolution ─────────────────────────────────────────
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi)) {
        g_enc_w = (mi.rcMonitor.right  - mi.rcMonitor.left) & ~1;
        g_enc_h = (mi.rcMonitor.bottom - mi.rcMonitor.top)  & ~1;
    } else {
        g_enc_w = 1920; g_enc_h = 1080; // safe fallback
    }

    // ── Clips output directory ────────────────────────────────────────────────
    WCHAR clips_dir[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", clips_dir, MAX_PATH);
    StringCchCatW(clips_dir, MAX_PATH, L"\\WindowsRecorder\\Clips");

    // ── Video encoder ─────────────────────────────────────────────────────────
    {
        encoding::VideoEncoder::Config vcfg;
        vcfg.width   = g_enc_w;
        vcfg.height  = g_enc_h;
        vcfg.fps     = 30;           // encode at 30fps (every 2nd WGC frame)
        vcfg.bitrate = 20'000'000;

        bool ok = g_video_enc.open(vcfg, [](encoding::EncodedPacket pkt) {
            g_replay.push(std::move(pkt));
        });

        char buf[128];
        snprintf(buf, sizeof(buf), "%dx%d @30fps bitrate=20Mbps", g_enc_w, g_enc_h);
        log_msg("encoding", ok ? buf : "WARNING: no H.264 encoder — replay disabled");

        if (ok) g_replay.set_video_params(g_video_enc.stream_params());
    }

    // ── Audio encoder ─────────────────────────────────────────────────────────
    {
        encoding::AudioEncoder::Config acfg;
        acfg.sample_rate = 48000;
        acfg.channels    = 2;
        acfg.bitrate     = 192'000;

        bool ok = g_audio_enc.open(acfg, [](encoding::EncodedPacket pkt) {
            g_replay.push(std::move(pkt));
        });
        log_msg("encoding", ok ? "AAC encoder opened (48kHz stereo 192kbps)"
                               : "WARNING: AAC encoder failed");

        if (ok) g_replay.set_audio_params(g_audio_enc.stream_params());
    }

    // ── Replay buffer config ───────────────────────────────────────────────────
    {
        replay_buffer::ReplayBuffer::Config rbcfg;
        rbcfg.duration_sec  = 30;
        rbcfg.memory_cap_mb = 512;
        rbcfg.output_dir    = clips_dir;
        g_replay.configure(rbcfg);
        log_msg("replay", "replay buffer configured: 30s / 512MB cap");
    }

    // ── WGC display capture ───────────────────────────────────────────────────
    if (capture::is_supported()) {
        bool ok = g_video.start(hmon, [](capture::FrameInfo const& f) {
            // Log buffer stats every 300 frames (~5s at 60fps).
            if (f.seq % 300 == 0) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                    "seq=%-6u  %ux%u  buf=%zu pkt / %.1f MB",
                    f.seq, f.width, f.height,
                    g_replay.packet_count(),
                    static_cast<double>(g_replay.memory_bytes()) / 1048576.0);
                log_msg("capture", buf);
            }
            // Encode every 2nd frame → 30fps effective for software encoder.
            if (f.seq % 2 != 0 || f.bgra_data == nullptr) return;
            g_video_enc.push_bgra(
                f.bgra_data,
                static_cast<int>(f.bgra_stride),
                static_cast<int>(f.width),
                static_cast<int>(f.height));
        });
        log_msg("capture", ok ? "WGC display capture started"
                               : "WARNING: WGC display capture failed");
    } else {
        log_msg("capture", "WARNING: Windows.Graphics.Capture not supported");
    }

    // ── WASAPI loopback (system audio → encoder + replay buffer) ─────────────
    bool ok_sys = g_audio_system.start(
        audio::WasapiCapture::Mode::Loopback,
        [](audio::PacketInfo const& p) {
            if (p.seq % 500 == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "seq=%-6u  frames=%-4u  sr=%u  ch=%u%s",
                    p.seq, p.frame_count, p.sample_rate, p.channels,
                    p.silent ? "  [silent]" : "");
                log_msg("audio.sys", buf);
            }
            if (p.data_bytes > 0) {
                g_audio_enc.push_pcm(
                    p.data, static_cast<int>(p.data_bytes),
                    static_cast<int>(p.sample_rate),
                    static_cast<int>(p.channels),
                    static_cast<int>(p.bit_depth), p.is_float);
            }
        });
    log_msg("audio.sys", ok_sys ? "WASAPI loopback started"
                                 : "WARNING: WASAPI loopback failed");

    // ── WASAPI microphone (monitor only, not fed to replay buffer for MVP) ───
    bool ok_mic = g_audio_mic.start(
        audio::WasapiCapture::Mode::Microphone,
        [](audio::PacketInfo const& p) {
            if (p.seq % 500 == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "seq=%-6u  frames=%-4u  sr=%u  ch=%u%s",
                    p.seq, p.frame_count, p.sample_rate, p.channels,
                    p.silent ? "  [silent]" : "");
                log_msg("audio.mic", buf);
            }
        });
    log_msg("audio.mic", ok_mic ? "WASAPI mic started"
                                 : "WARNING: WASAPI mic failed (no mic?)");
}

static void media_stop()
{
    g_video.stop();
    g_audio_system.stop();
    g_audio_mic.stop();
    g_video_enc.close(); // flushes + frees encoder
    g_audio_enc.close();
    log_msg("app", "capture + audio + encoding stopped");
}

// ── System tray ───────────────────────────────────────────────────────────────

static NOTIFYICONDATAW g_nid;

static void tray_add(HWND hwnd)
{
    g_nid                  = {};
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), kAppName);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
        log_msg("tray", "WARNING: Shell_NotifyIconW NIM_ADD failed");
}

static void tray_remove() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

static void tray_show_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING,             CMD_SAVE_REPLAY,     L"Save Replay\tCtrl+Shift+F8");
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, MF_STRING,             CMD_RECORDING_START, L"Start Recording\tCtrl+Shift+F9");
    AppendMenuW(menu, MF_STRING,             CMD_RECORDING_STOP,  L"Stop Recording\tCtrl+Shift+F10");
    AppendMenuW(menu, MF_STRING,             CMD_PAUSE_RESUME,    L"Pause / Resume\tCtrl+Shift+F11");
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, CMD_SETTINGS,        L"Settings\x2026");
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, MF_STRING,             CMD_EXIT,            L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// ── Hotkeys ───────────────────────────────────────────────────────────────────

static void hotkeys_register(HWND hwnd)
{
    if (!RegisterHotKey(hwnd, HK_SAVE_REPLAY,
                        MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F8))
        log_msg("hotkey", "WARNING: save_replay (Ctrl+Shift+F8) failed — key in use");
    else
        log_msg("hotkey", "save_replay (Ctrl+Shift+F8) registered");
}

static void hotkeys_unregister(HWND hwnd)
{
    UnregisterHotKey(hwnd, HK_SAVE_REPLAY);
}

// ── Command dispatch ──────────────────────────────────────────────────────────

static void dispatch(Cmd cmd, HWND hwnd)
{
    switch (cmd) {
    case CMD_SAVE_REPLAY: {
        char stats[256];
        snprintf(stats, sizeof(stats),
            "save_replay — buffer: %zu pkt / %.1f MB",
            g_replay.packet_count(),
            static_cast<double>(g_replay.memory_bytes()) / 1048576.0);
        log_msg("replay", stats);
        g_replay.save_clip([](std::wstring path) {
            if (path.empty()) {
                log_msg("replay", "WARNING: clip save failed or encoder not ready");
            } else {
                // Convert wide path for logging.
                char narrow[MAX_PATH * 2];
                WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                    narrow, sizeof(narrow), nullptr, nullptr);
                char msg[MAX_PATH * 2 + 32];
                snprintf(msg, sizeof(msg), "clip saved: %s", narrow);
                log_msg("replay", msg);
            }
        });
        break;
    }
    case CMD_RECORDING_START:
        log_msg("tray", "recording_start triggered");
        break;
    case CMD_RECORDING_STOP:
        log_msg("tray", "recording_stop triggered");
        break;
    case CMD_PAUSE_RESUME:
        log_msg("tray", "pause_resume triggered");
        break;
    case CMD_SETTINGS:
        log_msg("tray", "settings triggered (not implemented)");
        break;
    case CMD_EXIT:
        log_msg("app", "exit requested");
        DestroyWindow(hwnd);
        break;
    }
}

// ── Window procedure ──────────────────────────────────────────────────────────

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        tray_add(hwnd);
        hotkeys_register(hwnd);
        media_start(hwnd);
        log_msg("app", "app shell ready");
        return 0;

    case WM_DESTROY:
        media_stop();
        hotkeys_unregister(hwnd);
        tray_remove();
        log_msg("app", "app shell stopped");
        PostQuitMessage(0);
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK)
            tray_show_menu(hwnd);
        return 0;

    case WM_COMMAND:
        dispatch(static_cast<Cmd>(LOWORD(wp)), hwnd);
        return 0;

    case WM_HOTKEY:
        if (wp == HK_SAVE_REPLAY)
            dispatch(CMD_SAVE_REPLAY, hwnd);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ── Entry point ───────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    log_init();
    log_msg("app", "initializing (Milestone 3: encoding + replay buffer)");

    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        log_msg("app", "FATAL: RegisterClassExW failed");
        winrt::uninit_apartment();
        CloseHandle(mutex);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, kWindowClass, kAppName,
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        log_msg("app", "FATAL: CreateWindowExW failed");
        winrt::uninit_apartment();
        CloseHandle(mutex);
        return 1;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    winrt::uninit_apartment();
    CloseHandle(mutex);
    if (g_log) fclose(g_log);
    return static_cast<int>(msg.wParam);
}
