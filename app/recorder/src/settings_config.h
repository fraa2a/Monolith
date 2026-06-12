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

    // Capture (capture/encoder restart when no manual recording is active).
    std::wstring monitor_device;          // e.g. L"\\\\.\\DISPLAY1"; empty = primary
    std::string resolution_mode = "source"; // "source" | "custom"
    int output_width = 0;                 // used when resolution_mode == "custom"
    int output_height = 0;
    bool show_capture_border = false;

    // Encoder (capture/encoder restart when no manual recording is active).
    std::string encoder_backend = "auto"; // auto | h264_* | hevc_* | libx264 | libx265
    int video_bitrate_kbps = 20000;       // clamped 1000–100000
    std::string extra_ffmpeg_options;     // "key=value:key=value" AVOptions

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

struct RuntimeStatus {
    std::vector<RuntimeMonitor> monitors;
    std::vector<RuntimeAudioDevice> input_devices;
    std::vector<RuntimeAudioSession> audio_sessions;
    RuntimeAudioSession active_game;
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
