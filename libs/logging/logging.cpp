#include "logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <share.h>

#include <cstdio>
#include <mutex>

namespace logging {

namespace {

bool         g_enabled = false;
std::wstring g_dir;
FILE*        g_file = nullptr;
std::mutex   g_mutex;

std::wstring log_file_path()
{
    return g_dir + L"\\monolith.log";
}

bool file_size_bytes(const std::wstring& path, uint64_t* size)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return false;
    if (size) {
        *size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                static_cast<uint64_t>(data.nFileSizeLow);
    }
    return true;
}

void rotate_log_if_needed(const std::wstring& path, uint64_t max_bytes, int backups)
{
    uint64_t size = 0;
    if (!file_size_bytes(path, &size) || size <= max_bytes)
        return;

    for (int i = backups; i >= 1; --i) {
        std::wstring src = (i == 1) ? path : (path + L"." + std::to_wstring(i - 1));
        std::wstring dst = path + L"." + std::to_wstring(i);
        if (i == backups)
            DeleteFileW(dst.c_str());
        MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
}

// Must hold g_mutex.
void open_locked()
{
    if (g_file || g_dir.empty()) return;
    std::wstring path = log_file_path();
    rotate_log_if_needed(path, 5ull * 1024ull * 1024ull, 3);
    // _SH_DENYNO: keep the log readable by Settings/diagnostics while we run.
    g_file = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
}

// Must hold g_mutex.
void close_locked()
{
    if (!g_file) return;
    fclose(g_file);
    g_file = nullptr;
}

} // namespace

void init(bool enabled, const std::wstring& dir)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_dir = dir;
    g_enabled = enabled;
    if (g_enabled) open_locked();
}

void set_enabled(bool enabled)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (enabled == g_enabled) return;
    g_enabled = enabled;
    if (g_enabled) open_locked();
    else close_locked();
}

bool enabled()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_enabled;
}

void log(const char* tag, const char* msg)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_enabled) return;

    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "[%04d-%02d-%02dT%02d:%02d:%02dZ] [%-12s] %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        tag, msg);
    if (g_file) { fputs(buf, g_file); fflush(g_file); }
    OutputDebugStringA(buf);
}

} // namespace logging
