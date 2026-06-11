#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>

#include <winrt/base.h>          // winrt::init_apartment / uninit_apartment

#include <capture/capture.h>
#include <audio/audio.h>

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
// Thread-safe: called from the WGC thread pool and the WASAPI capture thread.

static FILE*       g_log       = nullptr;
static std::mutex  g_log_mutex;

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
        "[%04d-%02d-%02dT%02d:%02d:%02dZ] [%-10s] %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        tag, msg);
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        if (g_log) { fputs(buf, g_log); fflush(g_log); }
    }
    OutputDebugStringA(buf);
}

// ── Capture + Audio globals ───────────────────────────────────────────────────

static capture::DisplayCapture g_video;
static audio::WasapiCapture    g_audio_system;
static audio::WasapiCapture    g_audio_mic;

static void media_start(HWND hwnd)
{
    // Capture the monitor that owns the message-window (primary display).
    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);

    if (capture::is_supported()) {
        bool ok = g_video.start(hmon, [](capture::FrameInfo const& f) {
            // Log every 300th frame (~5 s at 60 fps).
            if (f.seq % 300 == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "seq=%-6u  %ux%u", f.seq, f.width, f.height);
                log_msg("capture", buf);
            }
        });
        log_msg("capture", ok ? "WGC display capture started"
                               : "WARNING: WGC display capture failed");
    } else {
        log_msg("capture", "WARNING: Windows.Graphics.Capture not supported");
    }

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
        });
    log_msg("audio.sys", ok_sys ? "WASAPI loopback started"
                                 : "WARNING: WASAPI loopback failed");

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
    log_msg("app", "capture + audio stopped");
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

static void tray_remove()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

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
        log_msg("hotkey", "WARNING: save_replay (Ctrl+Shift+F8) failed — key may be in use");
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
    case CMD_SAVE_REPLAY:
        log_msg("hotkey", "save_replay triggered");
        // TODO M3: forward to replay buffer
        break;
    case CMD_RECORDING_START:
        log_msg("tray", "recording_start triggered");
        // TODO M5: forward to recording manager
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

    // Initialize COM (MTA) for WinRT (WGC) and WASAPI.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    log_init();
    log_msg("app", "initializing");

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
