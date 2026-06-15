#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace settings {

struct AudioSourceConfig {
    std::string id;
    std::string type; // desktop | input | process | active_game
    std::string name;
    std::wstring device_id;
    uint32_t process_id = 0;
    std::string process_name;
    std::wstring executable_path;
    bool enabled = true;
    std::vector<int> tracks;
};

// Configuration for the dynamic Active Game detection subsystem.
struct ActiveGameSettings {
    bool detection_enabled         = true;
    int  poll_interval_ms          = 30000;  // clamped 3000–30000
    int  switch_debounce_ms        = 3000;   // clamped 1000–15000
    int  min_confidence            = 50;     // clamped 0–100
    bool fast_scan_enabled         = true;
    int  fast_scan_min_interval_ms = 1000;   // clamped 500–5000

    // Exe names (UTF-8, compared case-insensitively) to reject or boost.
    std::vector<std::string> blacklist_processes;
    std::vector<std::string> whitelist_processes;
    std::vector<std::string> manual_games;
};

struct Config {
    std::wstring user_config_path;
    std::wstring clips_directory;
    std::wstring recordings_directory;
    std::wstring temp_directory;
    int replay_duration_seconds = 30;
    int64_t replay_memory_budget_mb = 512;
    std::string replay_clip_container = "mkv"; // "mkv" | "mp4"
    bool replay_buffer_enabled = true;
    std::string recording_container = "mkv"; // "mkv" | "mp4"
    bool recording_enabled = true;

    // Audio (restarted when no manual recording is active).
    std::string audio_mode = "default"; // "default" | "custom"
    std::wstring primary_microphone_device_id;
    std::vector<AudioSourceConfig> audio_sources;

    // Active game detection settings.
    ActiveGameSettings active_game;

    // Capture (capture/encoder restart when no manual recording is active).
    std::wstring monitor_device;          // e.g. L"\\\\.\\DISPLAY1"; empty = primary
    std::string resolution_mode = "source"; // "source" | "custom"
    int output_width = 0;                 // used when resolution_mode == "custom"
    int output_height = 0;
    bool show_capture_border = false;

    // Encoder (capture/encoder restart when no manual recording is active).
    std::string encoder_backend = "auto"; // auto | h264_* | hevc_* | libx264 | libx265
    int video_fps = 60;                   // clamped 15-120
    int video_quality = 20;               // 10-30 → CQP(HW)/CRF(SW)
    std::string scaling_filter = "bilinear"; // "bilinear" | "bicubic" | "lanczos"
    std::string extra_ffmpeg_options;     // "key=value:key=value" AVOptions

    // Auto-update (applied live via WinSparkle on settings reload).
    bool update_auto_check = true;

    std::string hotkey_save_replay = "Ctrl+Shift+F8";
    std::string hotkey_recording_start = "Ctrl+Shift+F9";
    std::string hotkey_recording_stop = "Ctrl+Shift+F10";
    std::string hotkey_pause_resume = "Ctrl+Shift+F11";

    std::string merged_json;
};

struct LoadResult {
    Config config;
    std::vector<std::string> warnings;
};

LoadResult load(
    const std::wstring& app_data_dir,
    const std::wstring& default_clips_directory,
    const std::wstring& default_recordings_directory,
    const std::wstring& default_temp_directory);

bool save(Config& config, std::string* error);

// Snapshot of runtime capabilities written to AppData\Local\Monolith\
// runtime-status.json so the Settings UI can offer only real choices
// (available encoders, attached monitors, border-suppression support).
struct RuntimeMonitor {
    std::wstring device;   // e.g. L"\\\\.\\DISPLAY1"
    int width = 0;
    int height = 0;
    bool primary = false;
};

struct RuntimeAudioDevice {
    std::wstring id;
    std::wstring name;
    bool default_device = false;
    bool available = true;
};

struct RuntimeAudioSession {
    uint32_t process_id = 0;
    std::wstring process_name;
    std::wstring display_name;
    std::wstring executable_path;
};

// Extended runtime status for the Active Game virtual source.
struct ActiveGameStatus {
    uint32_t process_id = 0;
    std::wstring process_name;
    std::wstring display_name;
    std::wstring executable_path;
    int confidence = 0;
    std::string reason;
    std::string capture_mode;               // "process_loopback" | "unavailable" | "none"
    bool process_loopback_available = false;
    std::string last_switch_time;           // ISO-8601 local time of last switch
    int poll_interval_ms = 30000;
    bool fast_scan_enabled = true;
};

struct RuntimeStatus {
    std::vector<RuntimeMonitor> monitors;
    std::vector<RuntimeAudioDevice> input_devices;
    std::vector<RuntimeAudioSession> audio_sessions;
    ActiveGameStatus active_game;           // extended; was RuntimeAudioSession
    std::vector<std::string> available_encoders;
    std::string active_encoder;        // "" until the encoder opens
    std::wstring active_monitor_device;
    bool border_suppressed = false;
    int encode_width = 0;
    int encode_height = 0;
};

bool write_runtime_status(
    const std::wstring& app_data_dir,
    const RuntimeStatus& status,
    std::string* error);

} // namespace settings
