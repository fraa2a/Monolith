#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace settings_window {

struct Model {
    std::wstring clips_directory;
    std::wstring recordings_directory;
    int replay_duration_seconds = 30;
    int64_t replay_memory_budget_mb = 512;
    std::wstring save_replay_hotkey;
    std::wstring recording_start_hotkey;
    std::wstring recording_stop_hotkey;
    std::wstring pause_resume_hotkey;
};

using SaveCallback = std::function<bool(const Model&, std::wstring*)>;

void show(HWND owner, const Model& model, SaveCallback on_save);

} // namespace settings_window
