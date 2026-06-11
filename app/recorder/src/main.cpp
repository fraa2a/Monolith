#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <cstdio>

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr WCHAR kWindowClass[] = L"WinRecorderMsgWnd";
static constexpr WCHAR kAppName[]     = L"Windows Recorder";
static constexpr WCHAR kMutexName[]   = L"WinRecorder_SingleInstance";
static constexpr UINT  WM_TRAYICON   = WM_APP + 1;

// Tray context-menu command IDs (match canonical command vocab in default-config.json)
enum Cmd : UINT {
    CMD_SAVE_REPLAY     = 1001,
    CMD_RECORDING_START = 1002,
    CMD_RECORDING_STOP  = 1003,
    CMD_PAUSE_RESUME    = 1004,
    CMD_SETTINGS        = 1005,
    CMD_EXIT            = 1006,
};

// Registered hotkey IDs (WPARAM in WM_HOTKEY)
enum HotkeyId : int {
    HK_SAVE_REPLAY = 1,
};

// ── Logging ───────────────────────────────────────────────────────────────────
// Writes timestamped structured lines to %LOCALAPPDATA%\WindowsRecorder\recorder.log
// and to the debugger via OutputDebugStringA.

static FILE* g_log = nullptr;

static void log_init()
{
    WCHAR dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH))
        return;
    StringCchCatW(dir, MAX_PATH, L"\\WindowsRecorder");
    CreateDirectoryW(dir, nullptr); // no-op if already exists

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
        "[%04d-%02d-%02dT%02d:%02d:%02dZ] [%-8s] %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        tag, msg);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
    OutputDebugStringA(buf);
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
    g_nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION); // placeholder icon
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), kAppName);

    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
        log_msg("tray", "WARNING: Shell_NotifyIconW NIM_ADD failed");
    else
        log_msg("tray", "tray icon added");
}

static void tray_remove()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    log_msg("tray", "tray icon removed");
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
    // SetForegroundWindow is required so the menu dismisses when the user clicks elsewhere
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// ── Hotkeys ───────────────────────────────────────────────────────────────────
// Bindings come from default-config.json; hardcoded defaults for Milestone 1.

static void hotkeys_register(HWND hwnd)
{
    // save_replay: Ctrl+Shift+F8
    if (!RegisterHotKey(hwnd, HK_SAVE_REPLAY,
                        MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F8))
        log_msg("hotkey", "WARNING: save_replay (Ctrl+Shift+F8) registration failed - key may be in use");
    else
        log_msg("hotkey", "save_replay (Ctrl+Shift+F8) registered");
}

static void hotkeys_unregister(HWND hwnd)
{
    UnregisterHotKey(hwnd, HK_SAVE_REPLAY);
}

// ── Command dispatch ──────────────────────────────────────────────────────────
// Placeholder: logs the command and returns. Real handlers wired in later milestones.

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
        log_msg("app", "app shell started - recording engine not yet active");
        return 0;

    case WM_DESTROY:
        hotkeys_unregister(hwnd);
        tray_remove();
        log_msg("app", "app shell stopped");
        PostQuitMessage(0);
        return 0;

    case WM_TRAYICON:
        // Right-click or left double-click opens the context menu
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
    // Single-instance guard: abort if another copy is already running
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    log_init();
    log_msg("app", "initializing");

    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        log_msg("app", "FATAL: RegisterClassExW failed");
        CloseHandle(mutex);
        return 1;
    }

    // HWND_MESSAGE: message-only window - invisible, no taskbar entry, no painting
    HWND hwnd = CreateWindowExW(
        0, kWindowClass, kAppName,
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        log_msg("app", "FATAL: CreateWindowExW failed");
        CloseHandle(mutex);
        return 1;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(mutex);
    if (g_log) fclose(g_log);
    return static_cast<int>(msg.wParam);
}
