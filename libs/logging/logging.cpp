#include "logging.h"
#include "log_writer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <memory>

#include <cstdio>
#include <mutex>

namespace logging {

namespace {

bool         g_enabled = false;
std::mutex   g_mutex;
std::unique_ptr<LogWriter> g_writer;

void write_locked(const char* record)
{
    if (!g_writer || !g_writer->write(record)) {
        OutputDebugStringA("Monolith log queue unavailable or full\n");
        std::fputs(record, stderr);
    }
}

} // namespace

void init(bool enabled, const std::wstring& dir)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_writer = std::make_unique<LogWriter>(std::filesystem::path(dir) / L"monolith.log");
    g_enabled = enabled;
}

void set_enabled(bool enabled)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (enabled == g_enabled) return;
    g_enabled = enabled;
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
    write_locked(buf);
    OutputDebugStringA(buf);
}

void log_error(const char* tag, const char* msg)
{
    std::lock_guard<std::mutex> lk(g_mutex);

    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "[%04d-%02d-%02dT%02d:%02d:%02dZ] [%-12s] ERROR: %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        tag, msg);

    OutputDebugStringA(buf);
    write_locked(buf);
}

} // namespace logging
