#include "settings_window.h"

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace settings_window {
namespace {

// Handle of the last spawned Settings process; owned by the UI thread
// (show() is only called from the tray window's message loop).
HANDLE g_settings_process = nullptr;

struct FocusContext {
    DWORD pid;
    bool focused;
};

struct CloseContext {
    DWORD pid;
};

BOOL CALLBACK focus_enum_proc(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FocusContext*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && IsWindowVisible(hwnd)) {
        SetForegroundWindow(hwnd);
        ctx->focused = true;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK close_enum_proc(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<CloseContext*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && IsWindowVisible(hwnd)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

// Returns true when a previously launched Settings window is still running
// (and tries to bring it to the foreground instead of spawning a duplicate).
bool settings_already_running()
{
    if (!g_settings_process) return false;
    if (WaitForSingleObject(g_settings_process, 0) != WAIT_TIMEOUT) {
        CloseHandle(g_settings_process);
        g_settings_process = nullptr;
        return false;
    }
    FocusContext ctx{ GetProcessId(g_settings_process), false };
    EnumWindows(focus_enum_proc, reinterpret_cast<LPARAM>(&ctx));
    // Only treat the process as "already running" when it actually has a visible
    // window we could focus. A live process with no window must not block
    // relaunches forever.
    return ctx.focused;
}

std::filesystem::path module_dir()
{
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(path).parent_path();
}

// How to launch the UI: the executable, its command line, and the working
// directory. The Tauri host is a single self-contained exe (Monolith.UI.exe)
// that opens the WebView2 window, so a plain exe path is all that's needed.
struct UiLaunch {
    std::filesystem::path exe;
    std::wstring          command_line; // quoted exe + args
    std::filesystem::path working_dir;
    bool                  found = false;
};

// Locates the Tauri UI exe (installed under ui/Monolith.UI.exe next to the
// recorder, or a dev cargo build tree) and returns its launch spec. Empty
// result if no exe is present.
UiLaunch resolve_tauri_ui(const std::filesystem::path& base)
{
    const std::filesystem::path candidates[] = {
        base / "ui" / "Monolith.UI.exe",                           // installed / CMake copy
        // Dev: running the recorder straight from a build tree, before the copy.
        base / ".." / ".." / ".." / "app" / "desktop-ui" / "src-tauri"
            / "target" / "release" / "monolith_ui.exe",
        base / ".." / ".." / ".." / "app" / "desktop-ui" / "src-tauri"
            / "target" / "debug" / "monolith_ui.exe",
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            UiLaunch l;
            l.exe          = std::filesystem::weakly_canonical(candidate, ec);
            l.working_dir  = l.exe.parent_path();
            l.command_line = L"\"" + l.exe.wstring() + L"\"";
            l.found        = true;
            return l;
        }
    }
    return {};
}

UiLaunch resolve_ui_launch()
{
    return resolve_tauri_ui(module_dir());
}

} // namespace

void show(HWND owner, UINT reload_message)
{
    if (settings_already_running()) return;

    UiLaunch launch = resolve_ui_launch();
    if (!launch.found) {
        MessageBoxW(owner,
            L"The Monolith UI (Monolith.UI.exe) was not found.\n\n"
            L"It is built by CMake when Rust/Cargo and Node.js/npm are installed; "
            L"reinstall or rebuild to restore it.",
            L"Monolith", MB_ICONERROR);
        return;
    }

    std::vector<wchar_t> mutable_command_line(
        launch.command_line.begin(), launch.command_line.end());
    mutable_command_line.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION process_info{};
    std::wstring working_dir = launch.working_dir.wstring();
    BOOL started = CreateProcessW(
        launch.exe.c_str(),
        mutable_command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        &startup,
        &process_info);

    if (!started || !process_info.hProcess) {
        std::wstring message =
            L"The Monolith UI could not be started.\n\nExpected location:\n"
            + launch.exe.wstring();
        MessageBoxW(owner, message.c_str(), L"Monolith", MB_ICONERROR);
        return;
    }
    CloseHandle(process_info.hThread);

    // The reload-watcher thread owns one handle; the duplicate-launch guard
    // keeps another.  If duplication fails, skip the guard rather than share.
    HANDLE process = process_info.hProcess;
    HANDLE guard = nullptr;
    DuplicateHandle(GetCurrentProcess(), process_info.hProcess,
                    GetCurrentProcess(), &guard,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    g_settings_process = guard;

    std::thread([owner, reload_message, process]() {
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
        PostMessageW(owner, reload_message, 0, 0);
    }).detach();
}

void close_running()
{
    if (!g_settings_process) return;

    DWORD wait = WaitForSingleObject(g_settings_process, 0);
    if (wait != WAIT_TIMEOUT) {
        CloseHandle(g_settings_process);
        g_settings_process = nullptr;
        return;
    }

    DWORD pid = GetProcessId(g_settings_process);
    CloseContext ctx{ pid };
    EnumWindows(close_enum_proc, reinterpret_cast<LPARAM>(&ctx));

    wait = WaitForSingleObject(g_settings_process, 2000);
    if (wait == WAIT_TIMEOUT) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (process) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 1000);
            CloseHandle(process);
        }
    }

    CloseHandle(g_settings_process);
    g_settings_process = nullptr;
}

} // namespace settings_window
