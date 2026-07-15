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
    std::wstring window_title;
    std::wstring window_class;
    bool enabled = true;
    float volume = 1.0f;            // linear gain 0.0–1.0, applied to the recording
    std::vector<int> tracks;
};

// Configuration for the dynamic Active Game detection subsystem.
struct ActiveGameSettings {
    bool detection_enabled         = true;
    int  poll_interval_ms          = 3000;   // clamped 3000–30000
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
    std::wstring app_data_dir;     // holds settings.db (ADR-0009 rewrite)
    std::wstring clips_directory;
    std::wstring recordings_directory;
    std::wstring temp_directory;      // managed internally; not user-configurable
    int replay_duration_seconds = 30; // presets 15/30/60/120, or custom 5–600
    std::string replay_clip_container = "mkv"; // "mkv" | "mp4"
    bool replay_buffer_enabled = true;
    std::string recording_container = "mkv"; // "mkv" | "mp4"
    bool recording_enabled = true;

    // Advanced: allow the replay buffer and a manual recording to run at the same
    // time. With the external ffmpeg encoder this means two encoder processes
    // (the frame is encoded twice), which is heavy on CPU encoders. Default off:
    // starting a recording temporarily suspends the replay buffer, then restores
    // it on stop.
    bool allow_concurrent_capture = false;

    // Audio (restarted when no manual recording is active).
    std::string audio_mode = "default"; // "default" | "custom"
    std::wstring primary_microphone_device_id;
    std::vector<AudioSourceConfig> audio_sources;

    // Active game detection settings.
    ActiveGameSettings active_game;

    // Capture mode: "always" keeps the replay buffer running; "game_only" stops
    // it after idle_timeout_seconds with no detected game and restarts on detect.
    // Never stops an active manual recording.
    std::string capture_mode = "always"; // "always" | "game_only"
    int capture_idle_timeout_seconds = 300; // clamped 30–3600
    bool capture_auto_record = false;
    // When true, the replay buffer stays active even with no detected game: in
    // game_only mode it falls back to full-screen capture until a game appears.
    // When false (default) the replay buffer is disabled once no game is present
    // (after the idle timeout).
    bool capture_clip_without_game = false;

    // Capture (capture/encoder restart when no manual recording is active).
    std::wstring monitor_device;          // e.g. L"\\\\.\\DISPLAY1"; empty = primary
    // Resolution is chosen as a preset height; the width is derived from the
    // captured monitor's aspect ratio and never upscaled beyond the monitor.
    // "source" keeps the native monitor resolution.
    std::string resolution_preset = "source"; // source | 480p | 720p | 1080p | 1440p
    bool show_capture_border = false;

    // Encoder (capture/encoder restart when no manual recording is active).
    // Simplified surface: user picks CPU/GPU + codec; the engine resolves the
    // concrete FFmpeg encoder from what the machine actually supports. Rate
    // control is always CBR with the configured bitrate.
    std::string encoder_device = "gpu";   // "gpu" | "cpu"
    std::string encoder_codec  = "h264";  // "h264" | "h265"
    int video_bitrate_kbps = 20000;       // CBR target, clamped 1000–200000
    int video_fps = 60;                   // presets 24/30/60/120/144
    std::string scaling_filter = "bilinear"; // fixed to bilinear (UI selector removed)
    std::string extra_ffmpeg_options;     // "key=value:key=value" AVOptions
    // Optional explicit path to ffmpeg.exe. Empty = auto-locate (bundled next to
    // the executable, then system PATH). Used by the external-encoder path.
    std::wstring ffmpeg_path;

    // Auto-update (applied live via WinSparkle on settings reload).
    bool update_auto_check = true;

    // Disabled by default; toggled live on settings reload (libs/logging).
    bool logging_enabled = false;

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
    std::wstring window_title;
    std::wstring window_class;
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
    int poll_interval_ms = 5000;            // fixed cadence (informational)
    bool fast_scan_enabled = true;
};

// One detected game (DB-matched running process) offered to the UI so the user
// can pick which to record/clip when several are running at once.
struct GameCandidateStatus {
    uint32_t process_id = 0;
    std::wstring process_name;
    std::wstring display_name;
    std::string  discord_app_id;
    std::wstring executable_path;
    bool foreground = false;
    bool fullscreen = false;
};

struct RuntimeStatus {
    std::vector<RuntimeMonitor> monitors;
    std::vector<RuntimeAudioDevice> input_devices;
    std::vector<RuntimeAudioSession> audio_sessions;
    ActiveGameStatus active_game;           // extended; was RuntimeAudioSession
    std::vector<GameCandidateStatus> game_candidates; // all DB-matched running games
    uint32_t selected_game_pid = 0;         // the game being recorded/clipped (0 = auto)
    std::vector<std::string> available_encoders;
    std::string active_encoder;        // "" until the encoder opens
    std::string video_encoder_error;   // "" when ok; set when encoder open fails
    std::wstring active_monitor_device;
    bool border_suppressed = false;
    int encode_width = 0;
    int encode_height = 0;
};

// Serializes RuntimeStatus to the JSON written to runtime-status.json.
// Exposed so callers can diff against the previous content and skip no-op writes.
std::string serialize_runtime_status(const RuntimeStatus& status);

bool write_runtime_status(
    const std::wstring& app_data_dir,
    const RuntimeStatus& status,
    std::string* error);

} // namespace settings
