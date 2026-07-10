#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

// Shared Win32 utility primitives used across the recorder, storage, encoding
// and audio libraries. Pure extraction of previously duplicated code — no
// behavior changes vs. the original per-file copies.
namespace platform_win {

// UTF-8 <-> UTF-16 string conversion helpers.
std::wstring utf8_to_wide(const std::string& value);
std::string wide_to_utf8(const std::wstring& value);

// Basic info about a process, resolved from its PID via
// QueryFullProcessImageNameW. Empty fields mean the process could not be
// opened/queried (e.g. insufficient privileges or it has already exited).
struct ProcessInfo {
    uint32_t process_id = 0;
    std::wstring executable_path;
    std::wstring process_name; // file name portion of executable_path
    std::wstring display_name; // defaults to process_name
};

ProcessInfo process_info(uint32_t pid);

// True if `hwnd` looks like a real, user-facing top-level window (visible,
// not a tool window, not disabled, has window text) — i.e. a reasonable
// candidate for "this is a window belonging to a running application/game".
bool is_capture_candidate_window(HWND hwnd);

// GetWindowTextW wrapper returning the title as a std::wstring.
std::wstring window_text(HWND hwnd);

// GetClassNameW wrapper returning the window class name as a std::wstring.
std::wstring window_class_name(HWND hwnd);

} // namespace platform_win
