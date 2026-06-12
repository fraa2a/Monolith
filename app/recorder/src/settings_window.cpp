#include "settings_window.h"

#include <shellapi.h>

#include <filesystem>
#include <thread>

namespace settings_window {
namespace {

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
    std::filesystem::path exe = settings_exe_path();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpFile = exe.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info) || !info.hProcess) {
        MessageBoxW(
            owner,
            L"Monolith.Settings.exe was not found. Build or install the WinUI settings app.",
            L"Monolith Settings",
            MB_ICONERROR);
        return;
    }

    HANDLE process = info.hProcess;
    std::thread([owner, reload_message, process]() {
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
        PostMessageW(owner, reload_message, 0, 0);
    }).detach();
}

} // namespace settings_window
