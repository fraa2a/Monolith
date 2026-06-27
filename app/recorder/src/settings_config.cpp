#include "settings_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
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
    "enabled": true,
    "duration_seconds": 30,
    "memory_budget_mb": 128,
    "save_container": "mkv"
  },
  "recording": {
    "enabled": true,
    "container": "mkv"
  },
  "audio": {
    "mode": "default",
    "primary_microphone_device_id": "",
    "sources": [
      {
        "id": "desktop",
        "type": "desktop",
        "name": "All desktop audio",
        "enabled": true,
        "tracks": [1]
      }
    ]
  },
  "video_encoder": {
    "backend": "auto",
    "fps": 60,
    "quality": 20,
    "scaling_filter": "bilinear",
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
  },
  "update": {
    "auto_check": true
  },
  "active_game": {
    "detection_enabled": true,
    "poll_interval_ms": 5000,
    "switch_debounce_ms": 3000,
    "min_confidence": 50,
    "fast_scan_enabled": true,
    "fast_scan_min_interval_ms": 1000,
    "blacklist_processes": [
      "Monolith.exe",
      "Monolith.Settings.exe",
      "Discord.exe",
      "chrome.exe",
      "msedge.exe",
      "firefox.exe",
      "obs64.exe",
      "explorer.exe",
      "dwm.exe",
      "SearchHost.exe",
      "StartMenuExperienceHost.exe",
      "ShellExperienceHost.exe"
    ],
    "whitelist_processes": [],
    "manual_games": []
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

std::vector<int> sanitize_tracks(const json& value)
{
    std::vector<int> tracks;
    if (!value.is_array()) return tracks;
    for (const auto& item : value) {
        if (!item.is_number_integer()) continue;
        int track = item.get<int>();
        if (track < 1 || track > 6) continue;
        if (std::find(tracks.begin(), tracks.end(), track) == tracks.end())
            tracks.push_back(track);
    }
    return tracks;
}

AudioSourceConfig audio_source_from_json(const json& item)
{
    AudioSourceConfig source;
    if (!item.is_object()) return source;
    source.id = item.value("id", "");
    source.type = item.value("type", "");
    source.name = item.value("name", "");
    source.device_id = utf8_to_wide(item.value("device_id", ""));
    source.process_id = item.value("process_id", 0u);
    source.process_name = item.value("process_name", "");
    source.executable_path = utf8_to_wide(item.value("executable_path", ""));
    source.window_title = utf8_to_wide(item.value("window_title", ""));
    source.window_class = utf8_to_wide(item.value("window_class", ""));
    source.enabled = item.value("enabled", true);
    auto tracks_it = item.find("tracks");
    if (tracks_it != item.end())
        source.tracks = sanitize_tracks(*tracks_it);

    if (source.type != "desktop" && source.type != "input" &&
        source.type != "process" && source.type != "active_game") {
        source.type.clear();
    }
    if (source.id.empty()) {
        if (source.type == "desktop")
            source.id = "desktop";
        else if (source.type == "active_game")
            source.id = "active_game";
        else if (source.type == "input" && !source.device_id.empty())
            source.id = "input:" + wide_to_utf8(source.device_id);
        else if (source.type == "process" && !source.window_class.empty() &&
                 !source.window_title.empty())
            source.id = "window:" + wide_to_utf8(source.executable_path) + ":" +
                wide_to_utf8(source.window_class) + ":" +
                wide_to_utf8(source.window_title);
        else if (source.type == "process" && source.process_id != 0)
            source.id = "process:" + std::to_string(source.process_id);
    }
    if (source.name.empty()) source.name = source.id;
    return source;
}

std::vector<AudioSourceConfig> parse_audio_sources(const json& doc)
{
    std::vector<AudioSourceConfig> sources;
    auto audio_it = doc.find("audio");
    if (audio_it == doc.end() || !audio_it->is_object()) return sources;
    auto sources_it = audio_it->find("sources");
    if (sources_it == audio_it->end() || !sources_it->is_array()) return sources;

    for (const auto& item : *sources_it) {
        AudioSourceConfig source = audio_source_from_json(item);
        if (source.type.empty() || source.tracks.empty()) continue;
        sources.push_back(std::move(source));
    }
    return sources;
}

bool has_audio_sources_array(const json& doc)
{
    auto audio_it = doc.find("audio");
    if (audio_it == doc.end() || !audio_it->is_object()) return false;
    auto sources_it = audio_it->find("sources");
    return sources_it != audio_it->end() && sources_it->is_array();
}

json audio_source_to_json(const AudioSourceConfig& source)
{
    json item;
    item["id"] = source.id;
    item["type"] = source.type;
    item["name"] = source.name;
    item["enabled"] = source.enabled;
    item["tracks"] = source.tracks;
    if (!source.device_id.empty())
        item["device_id"] = wide_to_utf8(source.device_id);
    if (source.process_id != 0)
        item["process_id"] = source.process_id;
    if (!source.process_name.empty())
        item["process_name"] = source.process_name;
    if (!source.executable_path.empty())
        item["executable_path"] = wide_to_utf8(source.executable_path);
    if (!source.window_title.empty())
        item["window_title"] = wide_to_utf8(source.window_title);
    if (!source.window_class.empty())
        item["window_class"] = wide_to_utf8(source.window_class);
    return item;
}

void write_runtime_fields(json& doc, const Config& config)
{
    doc["output"]["clips_directory"] = wide_to_utf8(config.clips_directory);
    doc["output"]["recordings_directory"] = wide_to_utf8(config.recordings_directory);
    doc["output"]["temp_directory"] = wide_to_utf8(config.temp_directory);
    doc["replay_buffer"]["enabled"] = config.replay_buffer_enabled;
    doc["replay_buffer"]["duration_seconds"] = config.replay_duration_seconds;
    doc["replay_buffer"]["memory_budget_mb"] = config.replay_memory_budget_mb;
    doc["replay_buffer"]["save_container"] = config.replay_clip_container;
    doc["recording"]["enabled"] = config.recording_enabled;
    doc["recording"]["container"] = config.recording_container;
    // Dead key removed from the schema; scrub it from older configs.
    if (doc.contains("recording") && doc["recording"].is_object())
        doc["recording"].erase("pause_behavior");
    doc["video_encoder"]["backend"] = config.encoder_backend;
    doc["video_encoder"]["fps"] = config.video_fps;
    doc["video_encoder"]["quality"] = config.video_quality;
    doc["video_encoder"]["scaling_filter"] = config.scaling_filter;
    doc["video_encoder"]["extra_ffmpeg_options"] = config.extra_ffmpeg_options;
    doc["audio"]["mode"] = config.audio_mode;
    doc["audio"]["primary_microphone_device_id"] = wide_to_utf8(config.primary_microphone_device_id);
    doc["audio"]["sources"] = json::array();
    for (const auto& source : config.audio_sources)
        doc["audio"]["sources"].push_back(audio_source_to_json(source));
    doc["hotkeys"]["save_replay"] = config.hotkey_save_replay;
    doc["hotkeys"]["recording_start"] = config.hotkey_recording_start;
    doc["hotkeys"]["recording_stop"] = config.hotkey_recording_stop;
    doc["hotkeys"]["pause_resume"] = config.hotkey_pause_resume;
    doc["update"]["auto_check"] = config.update_auto_check;
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
        doc, "replay_buffer", "memory_budget_mb", 128);

    if (config.replay_duration_seconds < 5 || config.replay_duration_seconds > 600)
        config.replay_duration_seconds = 30;
    if (config.replay_memory_budget_mb < 64 || config.replay_memory_budget_mb > 16384)
        config.replay_memory_budget_mb = 128;

    config.replay_clip_container = utf8_at(doc, "replay_buffer", "save_container", "mkv");
    if (config.replay_clip_container != "mkv" && config.replay_clip_container != "mp4")
        config.replay_clip_container = "mkv";
    config.replay_buffer_enabled = bool_at(doc, "replay_buffer", "enabled", true);

    config.recording_container = utf8_at(doc, "recording", "container", "mkv");
    if (config.recording_container != "mkv" && config.recording_container != "mp4")
        config.recording_container = "mkv";
    config.recording_enabled = bool_at(doc, "recording", "enabled", true);

    config.update_auto_check = bool_at(doc, "update", "auto_check", true);

    config.audio_mode = utf8_at(doc, "audio", "mode", "default");
    if (config.audio_mode != "default" && config.audio_mode != "custom")
        config.audio_mode = "default";
    config.primary_microphone_device_id = utf8_to_wide(
        utf8_at(doc, "audio", "primary_microphone_device_id", ""));
    const bool explicit_audio_sources = has_audio_sources_array(doc);
    config.audio_sources = parse_audio_sources(doc);
    if (!explicit_audio_sources && config.audio_sources.empty()) {
        AudioSourceConfig desktop;
        desktop.id = "desktop";
        desktop.type = "desktop";
        desktop.name = "All desktop audio";
        desktop.enabled = true;
        desktop.tracks = {1};
        config.audio_sources.push_back(std::move(desktop));
    }

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

    config.video_quality = int_at(doc, "video_encoder", "quality", 20);
    if (config.video_quality < 10 || config.video_quality > 30)
        config.video_quality = 20;
    config.scaling_filter = utf8_at(doc, "video_encoder", "scaling_filter", "bilinear");

    config.video_fps = int_at(doc, "video_encoder", "fps", 60);
    if (config.video_fps < 15 || config.video_fps > 120)
        config.video_fps = 60;

    config.extra_ffmpeg_options = utf8_at(doc, "video_encoder", "extra_ffmpeg_options", "");

    config.hotkey_save_replay = utf8_at(doc, "hotkeys", "save_replay", "Ctrl+Shift+F8");
    config.hotkey_recording_start = utf8_at(doc, "hotkeys", "recording_start", "Ctrl+Shift+F9");
    config.hotkey_recording_stop = utf8_at(doc, "hotkeys", "recording_stop", "Ctrl+Shift+F10");
    config.hotkey_pause_resume = utf8_at(doc, "hotkeys", "pause_resume", "Ctrl+Shift+F11");

    // ── active_game detection settings ───────────────────────────────────────────
    {
        auto ag_it = doc.find("active_game");
        const json* ag = (ag_it != doc.end() && ag_it->is_object()) ? &(*ag_it) : nullptr;
        auto ag_bool  = [&](const char* key, bool fallback) -> bool {
            if (!ag) return fallback;
            auto it = ag->find(key); return (it != ag->end() && it->is_boolean()) ? it->get<bool>() : fallback;
        };
        auto ag_int   = [&](const char* key, int fallback) -> int {
            if (!ag) return fallback;
            auto it = ag->find(key); return (it != ag->end() && it->is_number_integer()) ? it->get<int>() : fallback;
        };
        config.active_game.detection_enabled         = ag_bool("detection_enabled",         true);
        config.active_game.fast_scan_enabled         = ag_bool("fast_scan_enabled",         true);
        config.active_game.poll_interval_ms          = ag_int ("poll_interval_ms",          5000);
        config.active_game.switch_debounce_ms        = ag_int ("switch_debounce_ms",        3000);
        config.active_game.min_confidence            = ag_int ("min_confidence",            50);
        config.active_game.fast_scan_min_interval_ms = ag_int ("fast_scan_min_interval_ms", 1000);

        // Clamp to safe ranges.
        if (config.active_game.poll_interval_ms < 3000 || config.active_game.poll_interval_ms > 30000)
            config.active_game.poll_interval_ms = 5000;
        if (config.active_game.switch_debounce_ms < 1000 || config.active_game.switch_debounce_ms > 15000)
            config.active_game.switch_debounce_ms = 3000;
        if (config.active_game.min_confidence < 0 || config.active_game.min_confidence > 100)
            config.active_game.min_confidence = 50;
        if (config.active_game.fast_scan_min_interval_ms < 500 || config.active_game.fast_scan_min_interval_ms > 5000)
            config.active_game.fast_scan_min_interval_ms = 1000;

        auto parse_string_array = [&](const char* key, std::vector<std::string>& dest) {
            if (!ag) return;
            auto it = ag->find(key);
            if (it == ag->end() || !it->is_array()) return;
            for (const auto& item : *it)
                if (item.is_string()) dest.push_back(item.get<std::string>());
        };
        parse_string_array("blacklist_processes",  config.active_game.blacklist_processes);
        parse_string_array("whitelist_processes",  config.active_game.whitelist_processes);
        parse_string_array("manual_games",         config.active_game.manual_games);
    }

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

std::string serialize_runtime_status(const RuntimeStatus& status)
{
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
    doc["input_devices"] = json::array();
    for (const auto& dev : status.input_devices) {
        doc["input_devices"].push_back({
            {"id", wide_to_utf8(dev.id)},
            {"name", wide_to_utf8(dev.name)},
            {"default_device", dev.default_device},
            {"available", dev.available},
        });
    }
    doc["audio_sessions"] = json::array();
    for (const auto& session : status.audio_sessions) {
        doc["audio_sessions"].push_back({
            {"process_id", session.process_id},
            {"process_name", wide_to_utf8(session.process_name)},
            {"display_name", wide_to_utf8(session.display_name)},
            {"executable_path", wide_to_utf8(session.executable_path)},
            {"window_title", wide_to_utf8(session.window_title)},
            {"window_class", wide_to_utf8(session.window_class)},
        });
    }
    doc["active_game"] = {
        {"process_id",                status.active_game.process_id},
        {"process_name",              wide_to_utf8(status.active_game.process_name)},
        {"display_name",              wide_to_utf8(status.active_game.display_name)},
        {"executable_path",           wide_to_utf8(status.active_game.executable_path)},
        {"confidence",                status.active_game.confidence},
        {"reason",                    status.active_game.reason},
        {"capture_mode",              status.active_game.capture_mode},
        {"process_loopback_available",status.active_game.process_loopback_available},
        {"last_switch_time",          status.active_game.last_switch_time},
        {"poll_interval_ms",          status.active_game.poll_interval_ms},
        {"fast_scan_enabled",         status.active_game.fast_scan_enabled},
    };
    doc["available_encoders"] = status.available_encoders;
    doc["active_encoder"] = status.active_encoder;
    doc["video_encoder_error"] = status.video_encoder_error;
    doc["active_monitor_device"] = wide_to_utf8(status.active_monitor_device);
    doc["border_suppressed"] = status.border_suppressed;
    doc["encode_width"]  = status.encode_width;
    doc["encode_height"] = status.encode_height;

    return doc.dump(2);
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

    return write_text(app_data_dir + L"\\runtime-status.json",
                      serialize_runtime_status(status), error);
}

} // namespace settings
