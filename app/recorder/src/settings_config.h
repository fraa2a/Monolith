#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace settings {

struct Config {
    std::wstring user_config_path;
    std::wstring clips_directory;
    std::wstring recordings_directory;
    std::wstring temp_directory;
    int replay_duration_seconds = 30;
    int64_t replay_memory_budget_mb = 512;

    // Capture (restart-required).
    std::wstring monitor_device;          // e.g. L"\\\\.\\DISPLAY1"; empty = primary
    std::string resolution_mode = "source"; // "source" | "custom"
    int output_width = 0;                 // used when resolution_mode == "custom"
    int output_height = 0;
    bool show_capture_border = false;

    // Encoder (restart-required).
    std::string encoder_backend = "auto"; // auto | h264_nvenc | h264_amf | h264_qsv | libx264
    int video_bitrate_kbps = 20000;       // clamped 1000–100000
    std::string extra_ffmpeg_options;     // "key=value:key=value" AVOptions

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

struct RuntimeStatus {
    std::vector<RuntimeMonitor> monitors;
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
