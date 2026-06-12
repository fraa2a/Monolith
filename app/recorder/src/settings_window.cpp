#include "settings_window.h"

#include <shellapi.h>

#include <filesystem>
#include <string>
#include <thread>

namespace settings_window {
namespace {

// Handle of the last spawned Settings process; owned by the UI thread
// (show() is only called from the tray window's message loop).
HANDLE g_settings_process = nullptr;

struct FocusContext {
    DWORD pid;
    bool focused;
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
    return true;
}

std::filesystem::path module_dir()
{
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path settings_exe_path()
{
    const std::filesystem::path base = module_dir();
    const std::filesystem::path candidates[] = {
        base / "Monolith.Settings.exe",
        base / ".." / ".." / ".." / "desktop-ui" / "bin" / "Release"
            / "net8.0-windows10.0.19041.0" / "win-x64" / "Monolith.Settings.exe",
    };

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec))
            return std::filesystem::weakly_canonical(candidate, ec);
    }

    return base / "Monolith.Settings.exe";
}

} // namespace

void show(HWND owner, UINT reload_message)
{
    if (settings_already_running()) return;

    std::filesystem::path exe = settings_exe_path();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpFile = exe.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info) || !info.hProcess) {
        std::wstring message =
            L"Monolith.Settings.exe was not found or could not be started.\n\n"
            L"Expected location:\n" + exe.wstring() +
            L"\n\nBuild or install the WinUI settings app "
            L"(cmake builds it when the .NET 8 SDK is installed).";
        MessageBoxW(owner, message.c_str(), L"Monolith Settings", MB_ICONERROR);
        return;
    }

    // The reload-watcher thread owns one handle; the duplicate-launch guard
    // keeps another.  If duplication fails, skip the guard rather than share.
    HANDLE process = info.hProcess;
    HANDLE guard = nullptr;
    DuplicateHandle(GetCurrentProcess(), info.hProcess,
                    GetCurrentProcess(), &guard,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    g_settings_process = guard;

    std::thread([owner, reload_message, process]() {
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
        PostMessageW(owner, reload_message, 0, 0);
    }).detach();
}

} // namespace settings_window
