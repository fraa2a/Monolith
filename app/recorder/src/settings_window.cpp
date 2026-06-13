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
        base / "settings" / "Monolith.Settings.exe",
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

    std::wstring command_line = L"\"" + exe.wstring() + L"\"";
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION process_info{};
    std::wstring working_dir = exe.parent_path().wstring();
    BOOL started = CreateProcessW(
        exe.c_str(),
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
            L"Monolith.Settings.exe was not found or could not be started.\n\n"
            L"Expected location:\n" + exe.wstring() +
            L"\n\nBuild or install the WinUI settings app "
            L"(cmake builds it when the .NET 8 SDK is installed).";
        MessageBoxW(owner, message.c_str(), L"Monolith Settings", MB_ICONERROR);
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
