#include "settings_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace settings {
namespace {

using json = nlohmann::json;

constexpr const char* kFallbackDefaultConfig = R"json(
{
  "schema_version": 2,
  "capture": {
    "monitor_device": "",
    "resolution_mode": "source",
    "resolution_width": 0,
    "resolution_height": 0,
    "show_capture_border": false
  },
  "replay_buffer": {
    "duration_seconds": 30,
    "memory_budget_mb": 512,
    "save_container": "mkv"
  },
  "recording": {
    "container": "mkv",
    "pause_behavior": "timestamp_gap"
  },
  "video_encoder": {
    "backend": "auto",
    "bitrate_kbps": 20000,
    "extra_ffmpeg_options": ""
  },
  "output": {
    "clips_directory": "",
    "recordings_directory": "",
    "temp_directory": ""
  },
  "hotkeys": {
    "save_replay": "Ctrl+Shift+F8",
    "recording_start": "Ctrl+Shift+F9",
    "recording_stop": "Ctrl+Shift+F10",
    "pause_resume": "Ctrl+Shift+F11"
  }
}
)json";

std::wstring module_dir()
{
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    std::filesystem::path exe_path(path);
    return exe_path.parent_path().wstring();
}

std::filesystem::path existing_default_config_path()
{
    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path exe_dir = module_dir();

    const std::filesystem::path candidates[] = {
        cwd / "config" / "default-config.json",
        exe_dir / "config" / "default-config.json",
        exe_dir / ".." / ".." / ".." / ".." / "config" / "default-config.json",
    };

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
}

bool write_text(const std::wstring& path, const std::string& text, std::string* error)
{
    if (path.empty()) {
        if (error) *error = "config path is empty";
        return false;
    }

    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "failed to open config for writing";
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        if (error) *error = "failed to write config";
        return false;
    }
    return true;
}

bool exists(const std::wstring& path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

std::wstring utf8_to_wide(const std::string& value)
{
    if (value.empty()) return {};
    int count = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count);
    return result;
}

std::string wide_to_utf8(const std::wstring& value)
{
    if (value.empty()) return {};
    int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

void merge_overrides(json& target, const json& overrides)
{
    if (!target.is_object() || !overrides.is_object()) return;

    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (target.contains(it.key()) && target[it.key()].is_object() && it.value().is_object()) {
            merge_overrides(target[it.key()], it.value());
            continue;
        }
        target[it.key()] = it.value();
    }
}

json fallback_defaults()
{
    return json::parse(kFallbackDefaultConfig);
}

json load_defaults(
    const std::wstring& default_clips_directory,
    const std::wstring& default_recordings_directory,
    const std::wstring& default_temp_directory,
    std::vector<std::string>& warnings)
{
    json defaults;
    std::filesystem::path default_path = existing_default_config_path();
    if (!default_path.empty()) {
        try {
            defaults = json::parse(read_text(default_path));
        } catch (const std::exception& ex) {
            warnings.push_back(std::string("default config parse failed; using compiled defaults: ") + ex.what());
            defaults = fallback_defaults();
        }
    } else {
        warnings.push_back("default config file not found; using compiled defaults");
        defaults = fallback_defaults();
    }

    defaults["output"]["clips_directory"] = wide_to_utf8(default_clips_directory);
    defaults["output"]["recordings_directory"] = wide_to_utf8(default_recordings_directory);
    defaults["output"]["temp_directory"] = wide_to_utf8(default_temp_directory);
    return defaults;
}

std::wstring string_at(const json& doc, const char* section, const char* key, const std::wstring& fallback)
{
    auto section_it = doc.find(section);
    if (section_it == doc.end() || !section_it->is_object()) return fallback;
    auto value_it = section_it->find(key);
    if (value_it == section_it->end() || !value_it->is_string()) return fallback;

    std::wstring converted = utf8_to_wide(value_it->get<std::string>());
    return converted.empty() ? fallback : converted;
}

int int_at(const json& doc, const char* section, const char* key, int fallback)
{
    auto section_it = doc.find(section);
    if (section_it == doc.end() || !section_it->is_object()) return fallback;
    auto value_it = section_it->find(key);
    if (value_it == section_it->end() || !value_it->is_number_integer()) return fallback;

    return value_it->get<int>();
}

int64_t int64_at(const json& doc, const char* section, const char* key, int64_t fallback)
{
    auto section_it = doc.find(section);
    if (section_it == doc.end() || !section_it->is_object()) return fallback;
    auto value_it = section_it->find(key);
    if (value_it == section_it->end() || !value_it->is_number_integer()) return fallback;

    return value_it->get<int64_t>();
}

bool bool_at(const json& doc, const char* section, const char* key, bool fallback)
{
    auto section_it = doc.find(section);
    if (section_it == doc.end() || !section_it->is_object()) return fallback;
    auto value_it = section_it->find(key);
    if (value_it == section_it->end() || !value_it->is_boolean()) return fallback;

    return value_it->get<bool>();
}

std::string utf8_at(const json& doc, const char* section, const char* key, const std::string& fallback)
{
    auto section_it = doc.find(section);
    if (section_it == doc.end() || !section_it->is_object()) return fallback;
    auto value_it = section_it->find(key);
    if (value_it == section_it->end() || !value_it->is_string()) return fallback;

    return value_it->get<std::string>();
}

void write_runtime_fields(json& doc, const Config& config)
{
    doc["output"]["clips_directory"] = wide_to_utf8(config.clips_directory);
    doc["output"]["recordings_directory"] = wide_to_utf8(config.recordings_directory);
    doc["output"]["temp_directory"] = wide_to_utf8(config.temp_directory);
    doc["replay_buffer"]["duration_seconds"] = config.replay_duration_seconds;
    doc["replay_buffer"]["memory_budget_mb"] = config.replay_memory_budget_mb;
    doc["recording"]["container"] = config.recording_container;
    doc["video_encoder"]["backend"] = config.encoder_backend;
    doc["video_encoder"]["bitrate_kbps"] = config.video_bitrate_kbps;
    doc["video_encoder"]["extra_ffmpeg_options"] = config.extra_ffmpeg_options;
    doc["hotkeys"]["save_replay"] = config.hotkey_save_replay;
    doc["hotkeys"]["recording_start"] = config.hotkey_recording_start;
    doc["hotkeys"]["recording_stop"] = config.hotkey_recording_stop;
    doc["hotkeys"]["pause_resume"] = config.hotkey_pause_resume;
}

Config config_from_json(
    const json& doc,
    const std::wstring& user_config_path,
    const std::wstring& default_clips_directory,
    const std::wstring& default_recordings_directory,
    const std::wstring& default_temp_directory)
{
    Config config;
    config.user_config_path = user_config_path;
    config.clips_directory = string_at(
        doc, "output", "clips_directory", default_clips_directory);
    config.recordings_directory = string_at(
        doc, "output", "recordings_directory", default_recordings_directory);
    config.temp_directory = string_at(
        doc, "output", "temp_directory", default_temp_directory);
    config.replay_duration_seconds = int_at(
        doc, "replay_buffer", "duration_seconds", 30);
    config.replay_memory_budget_mb = int64_at(
        doc, "replay_buffer", "memory_budget_mb", 512);

    if (config.replay_duration_seconds < 5 || config.replay_duration_seconds > 600)
        config.replay_duration_seconds = 30;
    if (config.replay_memory_budget_mb < 64 || config.replay_memory_budget_mb > 16384)
        config.replay_memory_budget_mb = 512;

    config.recording_container = utf8_at(doc, "recording", "container", "mkv");
    if (config.recording_container != "mkv" && config.recording_container != "mp4")
        config.recording_container = "mkv";

    config.monitor_device = utf8_to_wide(
        utf8_at(doc, "capture", "monitor_device", ""));
    config.resolution_mode = utf8_at(doc, "capture", "resolution_mode", "source");
    config.output_width  = int_at(doc, "capture", "resolution_width", 0);
    config.output_height = int_at(doc, "capture", "resolution_height", 0);
    config.show_capture_border = bool_at(doc, "capture", "show_capture_border", false);

    if (config.resolution_mode != "source" && config.resolution_mode != "custom")
        config.resolution_mode = "source";
    if (config.resolution_mode == "custom") {
        // Even-align; reject sizes outside a sane encode range.
        config.output_width  &= ~1;
        config.output_height &= ~1;
        if (config.output_width  < 128 || config.output_width  > 7680 ||
            config.output_height < 128 || config.output_height > 4320) {
            config.resolution_mode = "source";
            config.output_width   = 0;
            config.output_height  = 0;
        }
    }

    config.encoder_backend = utf8_at(doc, "video_encoder", "backend", "auto");
    if (config.encoder_backend != "auto" &&
        config.encoder_backend != "h264_nvenc" &&
        config.encoder_backend != "h264_amf" &&
        config.encoder_backend != "h264_qsv" &&
        config.encoder_backend != "libx264" &&
        config.encoder_backend != "hevc_nvenc" &&
        config.encoder_backend != "hevc_amf" &&
        config.encoder_backend != "hevc_qsv" &&
        config.encoder_backend != "libx265")
        config.encoder_backend = "auto";

    config.video_bitrate_kbps = int_at(doc, "video_encoder", "bitrate_kbps", 20000);
    if (config.video_bitrate_kbps < 1000 || config.video_bitrate_kbps > 100000)
        config.video_bitrate_kbps = 20000;

    config.extra_ffmpeg_options = utf8_at(doc, "video_encoder", "extra_ffmpeg_options", "");

    config.hotkey_save_replay = utf8_at(doc, "hotkeys", "save_replay", "Ctrl+Shift+F8");
    config.hotkey_recording_start = utf8_at(doc, "hotkeys", "recording_start", "Ctrl+Shift+F9");
    config.hotkey_recording_stop = utf8_at(doc, "hotkeys", "recording_stop", "Ctrl+Shift+F10");
    config.hotkey_pause_resume = utf8_at(doc, "hotkeys", "pause_resume", "Ctrl+Shift+F11");

    json sanitized = doc;
    write_runtime_fields(sanitized, config);
    config.merged_json = sanitized.dump(2);
    return config;
}

} // namespace

LoadResult load(
    const std::wstring& app_data_dir,
    const std::wstring& default_clips_directory,
    const std::wstring& default_recordings_directory,
    const std::wstring& default_temp_directory)
{
    LoadResult result;
    const std::wstring user_config_path = app_data_dir.empty()
        ? std::wstring()
        : app_data_dir + L"\\config.json";
    if (user_config_path.empty())
        result.warnings.push_back("AppData path not available; user config persistence disabled");

    json merged = load_defaults(
        default_clips_directory,
        default_recordings_directory,
        default_temp_directory,
        result.warnings);

    const bool user_config_exists = !user_config_path.empty() && exists(user_config_path);
    if (user_config_exists) {
        try {
            json user_config = json::parse(read_text(std::filesystem::path(user_config_path)));
            merge_overrides(merged, user_config);
        } catch (const std::exception& ex) {
            result.warnings.push_back(std::string("user config parse failed; using defaults: ") + ex.what());
        }
    }

    result.config = config_from_json(
        merged,
        user_config_path,
        default_clips_directory,
        default_recordings_directory,
        default_temp_directory);

    if (!user_config_exists && !user_config_path.empty()) {
        std::string error;
        Config copy = result.config;
        if (!save(copy, &error))
            result.warnings.push_back("failed to create user config: " + error);
        else
            result.config = copy;
    }

    return result;
}

bool save(Config& config, std::string* error)
{
    json doc;
    try {
        doc = config.merged_json.empty()
            ? fallback_defaults()
            : json::parse(config.merged_json);
    } catch (const std::exception& ex) {
        if (error) *error = std::string("stored config document invalid: ") + ex.what();
        return false;
    }

    write_runtime_fields(doc, config);
    config.merged_json = doc.dump(2);
    return write_text(config.user_config_path, config.merged_json, error);
}

bool write_runtime_status(
    const std::wstring& app_data_dir,
    const RuntimeStatus& status,
    std::string* error)
{
    if (app_data_dir.empty()) {
        if (error) *error = "AppData path not available";
        return false;
    }

    json doc;
    doc["monitors"] = json::array();
    for (const auto& mon : status.monitors) {
        doc["monitors"].push_back({
            {"device",  wide_to_utf8(mon.device)},
            {"width",   mon.width},
            {"height",  mon.height},
            {"primary", mon.primary},
        });
    }
    doc["available_encoders"] = status.available_encoders;
    doc["active_encoder"] = status.active_encoder;
    doc["active_monitor_device"] = wide_to_utf8(status.active_monitor_device);
    doc["border_suppressed"] = status.border_suppressed;
    doc["encode_width"]  = status.encode_width;
    doc["encode_height"] = status.encode_height;

    return write_text(app_data_dir + L"\\runtime-status.json", doc.dump(2), error);
}

} // namespace settings
