#pragma once
// Path/time adapters only. Production FFmpeg and media code run unmodified;
// this does not simulate Win32 file locking, replacement or MSVC behavior.
#include <chrono>

#include <ctime>
#include <cwchar>
#include <filesystem>

#include <string>
#include <algorithm>
constexpr int CP_UTF8 = 65001;
inline int WideCharToMultiByte(int, int, const wchar_t* p, int count, char* dst, int size, void*, void*) {
    // Fixtures intentionally use ASCII paths; non-ASCII paths need Windows testing.
    std::string text;
    for (int i = 0; i < count; ++i) { if (p[i] > 127) return 0; text.push_back(static_cast<char>(p[i])); }
    if (dst && size >= static_cast<int>(text.size())) std::copy(text.begin(), text.end(), dst);
    return static_cast<int>(text.size());
}
struct SYSTEMTIME { unsigned short wYear, wMonth, wDay, wHour, wMinute, wSecond, wMilliseconds; };
inline void GetLocalTime(SYSTEMTIME* out) {
    const auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now); std::tm local{}; localtime_r(&time, &local);
    *out = {static_cast<unsigned short>(1900 + local.tm_year), static_cast<unsigned short>(1 + local.tm_mon),
        static_cast<unsigned short>(local.tm_mday), static_cast<unsigned short>(local.tm_hour),
        static_cast<unsigned short>(local.tm_min), static_cast<unsigned short>(local.tm_sec),
        static_cast<unsigned short>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000)};
}
template<class... Args> int swprintf_s(wchar_t* dst, size_t size, const wchar_t* fmt, Args... args) {
    std::wstring format(fmt); auto pos = format.find(L"%s");
    if (pos != std::wstring::npos) format.replace(pos, 2, L"%ls");
    return std::swprintf(dst, size, format.c_str(), args...);
}
inline bool CreateDirectoryW(const wchar_t* path, void*) {
    std::error_code ec; std::filesystem::create_directories(path, ec); return !ec;
}
inline int gmtime_s(std::tm* out, const std::time_t* in) { return gmtime_r(in, out) ? 0 : 1; }
using HWND = void*;
