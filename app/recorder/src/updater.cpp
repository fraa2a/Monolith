#include "updater.h"

#include <logging/logging.h>

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace updater {

namespace {

bool g_auto_check = true;

std::filesystem::path module_dir()
{
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(path).parent_path();
}

// Locates Updater.exe (installed next to the recorder, or a dev cargo build
// tree) and returns an empty path when it is not present.
std::filesystem::path resolve_updater(const std::filesystem::path& base)
{
    const std::filesystem::path candidates[] = {
        base / "Updater.exe", // installed / CMake copy (app root)
        // Dev: running the recorder straight from a build tree, before the copy.
        base / ".." / ".." / ".." / "app" / "updater" / "src-tauri"
            / "target" / "release" / "updater.exe",
        base / ".." / ".." / ".." / "app" / "updater" / "src-tauri"
            / "target" / "debug" / "updater.exe",
    };
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec))
            return std::filesystem::weakly_canonical(candidate, ec);
    }
    return {};
}

void launch(bool auto_mode)
{
    std::filesystem::path exe = resolve_updater(module_dir());
    if (exe.empty()) {
        logging::log_error("updater", "Updater.exe not found (built by CMake "
                                      "when Rust/Cargo and Node.js/npm are installed)");
        return;
    }

    std::wstring command_line =
        L"\"" + exe.wstring() + L"\"" + (auto_mode ? L" --auto" : L"");

    STARTUPINFOW startup{};
    startup.cb          = sizeof(startup);
    startup.dwFlags     = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;

    std::vector<wchar_t> mutable_command_line(
        command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    std::wstring working_dir = exe.parent_path().wstring();

    // Detached: the updater outlives this process by design — it asks the
    // engine to exit (update_engine_exit over IPC) when it replaces it.
    PROCESS_INFORMATION process_info{};
    BOOL started = CreateProcessW(
        exe.c_str(),
        mutable_command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        DETACHED_PROCESS,
        nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        &startup,
        &process_info);

    if (!started || !process_info.hProcess) {
        logging::log_error("updater", "failed to launch Updater.exe");
        return;
    }
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
}

} // namespace

void init(bool auto_check_enabled)
{
    g_auto_check = auto_check_enabled;
}

void set_auto_check(bool enabled)
{
    g_auto_check = enabled;
}

void check_now()
{
    launch(false);
}

void check_silent()
{
    if (!g_auto_check) return;
    launch(true);
}

void post_update_cleanup()
{
    const std::filesystem::path base = module_dir();
    if (base.empty()) return;

    // *.old files parked by the updater's swap dance (a replaced engine exe
    // or DLL cannot be deleted while loaded — deletion is retried here).
    const std::filesystem::path dirs[] = { base, base / "ui" };
    std::error_code ec;
    for (const auto& dir : dirs) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            ec.clear();
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() == L".old")
                std::filesystem::remove(entry.path(), ec);
        }
        ec.clear();
    }

    // Legacy: installs migrated from the WinSparkle updater.
    std::filesystem::remove(base / "WinSparkle.dll", ec);
}

void shutdown() {}

} // namespace updater
