#pragma once
#include <chrono>
#include <ctime>
struct SYSTEMTIME { unsigned short wYear, wMonth, wDay, wHour, wMinute, wSecond; };
inline void GetSystemTime(SYSTEMTIME* out) {
    auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{}; gmtime_r(&time, &utc);
    *out = {static_cast<unsigned short>(1900 + utc.tm_year), static_cast<unsigned short>(1 + utc.tm_mon),
        static_cast<unsigned short>(utc.tm_mday), static_cast<unsigned short>(utc.tm_hour),
        static_cast<unsigned short>(utc.tm_min), static_cast<unsigned short>(utc.tm_sec)};
}
inline void OutputDebugStringA(const char*) {}
