#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>

#include <winrt/base.h>

#include <capture/capture.h>
#include <audio/audio.h>
#include <encoding/encoding.h>
#include <replay-buffer/replay_buffer.h>
#include <recording/recording.h>

#include "feedback_sounds.h"
#include "ipc_server.h"
#include "resource.h"
#include "settings_config.h"
#include "settings_window.h"
#include "updater.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr WCHAR kWindowClass[] = L"MonolithMsgWnd";
static constexpr WCHAR kAppName[]     = L"Monolith";
static constexpr WCHAR kMutexName[]   = L"Monolith_SingleInstance";
static constexpr UINT  WM_TRAYICON        = WM_APP + 1;
static constexpr UINT  WM_SETTINGS_RELOAD = WM_APP + 2;
static constexpr UINT  WM_FAST_SCAN       = WM_APP + 3; // posted by the WinEvent fg hook

enum Cmd : UINT {
    CMD_SAVE_REPLAY     = 1001,
    CMD_RECORDING_START = 1002,
    CMD_RECORDING_STOP  = 1003,
    CMD_PAUSE_RESUME    = 1004,
    CMD_SETTINGS        = 1005,
    CMD_EXIT            = 1006,
    CMD_CHECK_UPDATE    = 1007,
};

// ── Logging ───────────────────────────────────────────────────────────────────

static FILE*      g_log       = nullptr;
static std::mutex g_log_mutex;

static std::wstring known_folder_path(REFKNOWNFOLDERID folder)
{
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &raw)) && raw)
        result = raw;
    if (raw) CoTaskMemFree(raw);
    return result;
}

static std::wstring env_path(const wchar_t* name)
{
    WCHAR buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return buf;
}

static void ensure_directory(const std::wstring& path)
{
    if (!path.empty()) SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
}

static std::wstring app_data_dir()
{
    std::wstring dir = known_folder_path(FOLDERID_LocalAppData);
    if (dir.empty()) dir = env_path(L"LOCALAPPDATA");
    if (dir.empty()) return {};
    dir += L"\\Monolith";
    ensure_directory(dir);
    return dir;
}

static std::wstring videos_dir(const wchar_t* child)
{
    std::wstring dir = known_folder_path(FOLDERID_Videos);
    if (dir.empty()) {
        dir = env_path(L"USERPROFILE");
        if (!dir.empty()) dir += L"\\Videos";
    }
    if (dir.empty()) return {};
    dir += L"\\Monolith";
    ensure_directory(dir);
    dir += L"\\";
    dir += child;
    ensure_directory(dir);
    return dir;
}

static std::wstring app_temp_dir()
{
    std::wstring dir = app_data_dir();
    if (dir.empty()) return {};
    dir += L"\\Temp";
    ensure_directory(dir);
    return dir;
}

static void log_init()
{
    std::wstring dir = app_data_dir();
    if (dir.empty()) return;
    std::wstring path = dir + L"\\monolith.log";
    // _SH_DENYNO: keep the log readable by Settings/diagnostics while we run.
    g_log = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
}

static void log_msg(const char* tag, const char* msg)
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "[%04d-%02d-%02dT%02d:%02d:%02dZ] [%-12s] %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        tag, msg);
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        if (g_log) { fputs(buf, g_log); fflush(g_log); }
    }
    OutputDebugStringA(buf);
}

static void log_path(const char* tag, const char* prefix, const std::wstring& path)
{
    char narrow[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
        narrow, sizeof(narrow), nullptr, nullptr);
    char msg[MAX_PATH * 2 + 64];
    snprintf(msg, sizeof(msg), "%s%s", prefix, narrow);
    log_msg(tag, msg);
}

static std::wstring utf8_to_wide(const std::string& value)
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

static std::string wide_to_utf8(const std::wstring& value)
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

// ── Globals ───────────────────────────────────────────────────────────────────

static bool apps_use_light_theme()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    return status != ERROR_SUCCESS || value != 0;
}

static void apply_native_window_theme(HWND hwnd)
{
    BOOL dark = apps_use_light_theme() ? FALSE : TRUE;

    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using DwmSetWindowAttributeFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto set_attr = reinterpret_cast<DwmSetWindowAttributeFn>(
            GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (set_attr) {
            DWORD attr = 20;
            if (FAILED(set_attr(hwnd, attr, &dark, sizeof(dark)))) {
                attr = 19;
                set_attr(hwnd, attr, &dark, sizeof(dark));
            }
        }
        FreeLibrary(dwm);
    }

    if (HMODULE ux = LoadLibraryW(L"uxtheme.dll")) {
        using SetPreferredAppModeFn = int (WINAPI*)(int);
        using FlushMenuThemesFn = void (WINAPI*)();
        auto set_app_mode = reinterpret_cast<SetPreferredAppModeFn>(
            GetProcAddress(ux, MAKEINTRESOURCEA(135)));
        auto flush_menu_themes = reinterpret_cast<FlushMenuThemesFn>(
            GetProcAddress(ux, MAKEINTRESOURCEA(136)));
        if (set_app_mode) {
            set_app_mode(dark ? 2 : 3); // ForceDark / ForceLight.
            if (flush_menu_themes)
                flush_menu_themes();
        }

        using SetWindowThemeFn = HRESULT (WINAPI*)(HWND, LPCWSTR, LPCWSTR);
        auto set_theme = reinterpret_cast<SetWindowThemeFn>(
            GetProcAddress(ux, "SetWindowTheme"));
        if (set_theme)
            set_theme(hwnd, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        FreeLibrary(ux);
    }
}

static capture::DisplayCapture      g_video;
static encoding::VideoEncoder       g_video_enc;
static std::array<encoding::AudioEncoder, 6> g_audio_encoders;
static std::vector<std::unique_ptr<audio::WasapiCapture>> g_audio_captures;
static replay_buffer::ReplayBuffer  g_replay;
static recording::ManualRecorder    g_recording;

static int g_enc_w = 0; // configured encoder width (even-aligned)
static int g_enc_h = 0; // configured encoder height (even-aligned)
static std::atomic<bool> g_video_enc_open_attempted{ false };
// ── OBS-style video pacer: fixed-rate thread feeds encoder ──────────────
static HANDLE               g_pacer_thread       = nullptr;
static HANDLE               g_pacer_stop_event   = nullptr;
static std::mutex           g_pacer_mutex;

struct PacerFrame {
    std::vector<uint8_t> bgra;
    int stride = 0;
    int width  = 0;
    int height = 0;
    uint64_t frame_id = 0;
};

static PacerFrame           g_pacer_shared;
static uint64_t             g_pacer_last_read_id = 0;
static int                  g_pacer_interval_ms  = 0;
static std::atomic<bool>    g_pacer_running{ false };
static std::wstring g_recordings_dir;
static std::string g_recording_container = "mkv";
static std::atomic<bool> g_replay_enabled{ true };
static std::atomic<bool> g_recording_enabled{ true };
static settings::Config g_settings;

// Active-game audio source: re-evaluated every poll_interval_ms by a UI-thread timer
// and also on foreground-window changes (fast scan, rate-limited).
static constexpr UINT_PTR kActiveGameTimerId = 1;

static std::vector<int>          g_active_game_tracks;
static uint32_t                  g_active_game_pid     = 0;
static audio::WasapiCapture*     g_active_game_capture = nullptr;
static std::string               g_active_game_capture_mode; // "process_loopback"|"unavailable"|"none"

// Debounce state: pending switch candidate and when it was first seen.
static uint32_t g_pending_game_pid       = 0;
static DWORD    g_pending_game_first_ms  = 0;

// Fast-scan state (UI thread only).
static HWINEVENTHOOK g_fg_hook           = nullptr;
static HWND          g_main_hwnd         = nullptr; // set in WM_CREATE for the hook callback
static bool          g_fast_scan_pending = false;
static DWORD         g_last_fast_scan_ms = 0;

static std::mutex g_status_mutex;
static settings::RuntimeStatus g_runtime_status;

// Re-writes AppData\Local\Monolith\runtime-status.json from g_runtime_status.
// Callable from the WGC callback thread (encoder opens on first frame).
static void publish_runtime_status()
{
    settings::RuntimeStatus snapshot;
    {
        std::lock_guard<std::mutex> lk(g_status_mutex);
        snapshot = g_runtime_status;
    }
    std::string error;
    if (!settings::write_runtime_status(app_data_dir(), snapshot, &error))
        log_msg("settings", ("runtime-status write failed: " + error).c_str());
}

// ── OBS-style video pacer ──────────────────────────────────────────────
// Fixed-rate video thread feeds encoder exactly like OBS video_output.
// Producer (WGC callback): copies BGRA to g_pacer_shared under mutex.
// Consumer (pacer thread): reads latest frame each tick, pushes to encoder.

static void pacer_push_frame(const uint8_t* bgra, int stride, int width, int height)
{
    std::lock_guard<std::mutex> lock(g_pacer_mutex);
    size_t size = static_cast<size_t>(height) * static_cast<size_t>(stride);
    g_pacer_shared.bgra.resize(size);
    memcpy(g_pacer_shared.bgra.data(), bgra, size);
    g_pacer_shared.stride   = stride;
    g_pacer_shared.width    = width;
    g_pacer_shared.height   = height;
    g_pacer_shared.frame_id++;
}

static DWORD WINAPI pacer_thread_proc(LPVOID)
{
    std::vector<uint8_t> local_bgra;
    int local_stride = 0, local_width = 0, local_height = 0;
    uint64_t local_last_id = 0;

    while (true) {
        DWORD wait = WaitForSingleObject(g_pacer_stop_event,
                                         static_cast<DWORD>(g_pacer_interval_ms));
        if (wait == WAIT_OBJECT_0)
            break;

        {
            std::lock_guard<std::mutex> lock(g_pacer_mutex);
            if (g_pacer_shared.frame_id != local_last_id) {
                local_bgra    = g_pacer_shared.bgra;
                local_stride  = g_pacer_shared.stride;
                local_width   = g_pacer_shared.width;
                local_height  = g_pacer_shared.height;
                local_last_id = g_pacer_shared.frame_id;
            }
        }

        if (g_video_enc.is_open() && !local_bgra.empty()) {
            g_video_enc.push_bgra(local_bgra.data(), local_stride,
                                  local_width, local_height);
        }
    }

    g_pacer_running = false;
    return 0;
}

static void pacer_start()
{
    if (g_settings.video_fps <= 0) return;
    g_pacer_interval_ms = 1000 / g_settings.video_fps;
    if (g_pacer_interval_ms < 1) g_pacer_interval_ms = 1;
    g_pacer_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_pacer_running = true;
    g_pacer_thread = CreateThread(nullptr, 0, pacer_thread_proc, nullptr, 0, nullptr);
}

static void pacer_stop()
{
    if (g_pacer_stop_event) {
        SetEvent(g_pacer_stop_event);
        if (g_pacer_thread) {
            WaitForSingleObject(g_pacer_thread, 3000);
            CloseHandle(g_pacer_thread);
            g_pacer_thread = nullptr;
        }
        CloseHandle(g_pacer_stop_event);
        g_pacer_stop_event = nullptr;
    }
}

// ── Monitor enumeration / selection ──────────────────────────────────────────

struct MonitorEntry {
    HMONITOR hmon = nullptr;
    settings::RuntimeMonitor info;
};

static BOOL CALLBACK monitor_enum_proc(HMONITOR hmon, HDC, LPRECT, LPARAM lp)
{
    auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi)) {
        MonitorEntry entry;
        entry.hmon         = hmon;
        entry.info.device  = mi.szDevice;
        entry.info.width   = mi.rcMonitor.right - mi.rcMonitor.left;
        entry.info.height  = mi.rcMonitor.bottom - mi.rcMonitor.top;
        entry.info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        list->push_back(std::move(entry));
    }
    return TRUE;
}

static std::vector<MonitorEntry> enumerate_monitors()
{
    std::vector<MonitorEntry> list;
    EnumDisplayMonitors(nullptr, nullptr, monitor_enum_proc,
                        reinterpret_cast<LPARAM>(&list));
    return list;
}

// Picks the configured capture monitor by device name; falls back to primary.
static HMONITOR monitor_from_settings(const std::vector<MonitorEntry>& monitors,
                                      std::wstring* used_device)
{
    HMONITOR primary = nullptr;
    std::wstring primary_device;
    for (const auto& entry : monitors) {
        if (entry.info.primary) {
            primary = entry.hmon;
            primary_device = entry.info.device;
        }
        if (!g_settings.monitor_device.empty() &&
            entry.info.device == g_settings.monitor_device) {
            if (used_device) *used_device = entry.info.device;
            return entry.hmon;
        }
    }
    if (!g_settings.monitor_device.empty())
        log_path("capture", "configured monitor not found, using primary: ",
                 g_settings.monitor_device);
    if (used_device) *used_device = primary_device;
    if (primary) return primary;
    const POINT origin{ 0, 0 };
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

static void apply_runtime_settings()
{
    ensure_directory(g_settings.clips_directory);
    ensure_directory(g_settings.recordings_directory);
    ensure_directory(g_settings.temp_directory);

    replay_buffer::ReplayBuffer::Config rbcfg;
    rbcfg.duration_sec  = g_settings.replay_duration_seconds;
    rbcfg.memory_cap_mb = g_settings.replay_memory_budget_mb;
    rbcfg.output_dir    = g_settings.clips_directory;
    rbcfg.container     = g_settings.replay_clip_container;
    g_replay.configure(rbcfg);

    g_recordings_dir = g_settings.recordings_directory;
    g_recording_container = g_settings.recording_container;
    g_replay_enabled.store(g_settings.replay_buffer_enabled, std::memory_order_relaxed);
    g_recording_enabled.store(g_settings.recording_enabled, std::memory_order_relaxed);

    char msg[192];
    snprintf(msg, sizeof(msg),
             "settings applied: replay=%ds / %lldMB / %s%s recording=%s%s fps=%d",
             g_settings.replay_duration_seconds,
             static_cast<long long>(g_settings.replay_memory_budget_mb),
             g_settings.replay_clip_container.c_str(),
             g_settings.replay_buffer_enabled ? "" : " (DISABLED)",
             g_recording_container.c_str(),
             g_settings.recording_enabled ? "" : " (DISABLED)",
             g_settings.video_fps);
    log_msg("settings", msg);
    log_path("replay", "clips dir: ", g_settings.clips_directory);
    log_path("recording", "recordings dir: ", g_recordings_dir);
}

static void load_app_settings()
{
    settings::LoadResult result = settings::load(
        app_data_dir(),
        videos_dir(L"Clips"),
        videos_dir(L"Recordings"),
        app_temp_dir());

    g_settings = result.config;
    g_recordings_dir = g_settings.recordings_directory;
    g_recording_container = g_settings.recording_container;

    for (const auto& warning : result.warnings)
        log_msg("settings", warning.c_str());

    log_path("settings", "config path: ", g_settings.user_config_path);
}

static void show_settings(HWND hwnd)
{
    settings_window::show(hwnd, WM_SETTINGS_RELOAD);
}

static void hotkeys_register(HWND hwnd);
static void hotkeys_unregister(HWND hwnd);
static void stop_audio_system();
static void start_audio_system();
static void media_start(HWND hwnd);
static void media_stop();
static std::vector<int> valid_tracks(const std::vector<int>& tracks);

static bool same_tracks(const std::vector<int>& a, const std::vector<int>& b)
{
    return valid_tracks(a) == valid_tracks(b);
}

static bool same_audio_source(const settings::AudioSourceConfig& a,
                              const settings::AudioSourceConfig& b)
{
    return a.id == b.id
        && a.type == b.type
        && a.name == b.name
        && a.device_id == b.device_id
        && a.process_id == b.process_id
        && a.process_name == b.process_name
        && a.executable_path == b.executable_path
        && a.enabled == b.enabled
        && same_tracks(a.tracks, b.tracks);
}

static bool audio_config_changed(const settings::Config& a, const settings::Config& b)
{
    if (a.audio_mode != b.audio_mode ||
        a.primary_microphone_device_id != b.primary_microphone_device_id ||
        a.audio_sources.size() != b.audio_sources.size()) {
        return true;
    }

    for (size_t i = 0; i < a.audio_sources.size(); ++i) {
        if (!same_audio_source(a.audio_sources[i], b.audio_sources[i]))
            return true;
    }
    return false;
}

static bool capture_encoder_config_changed(const settings::Config& a,
                                           const settings::Config& b)
{
    return a.monitor_device != b.monitor_device
        || a.resolution_mode != b.resolution_mode
        || a.output_width != b.output_width
        || a.output_height != b.output_height
        || a.show_capture_border != b.show_capture_border
        || a.encoder_backend != b.encoder_backend
        || a.video_fps != b.video_fps
        || a.video_bitrate_kbps != b.video_bitrate_kbps
        || a.extra_ffmpeg_options != b.extra_ffmpeg_options;
}

static void reload_settings_from_disk(HWND hwnd)
{
    settings::Config previous = g_settings;
    load_app_settings();
    const bool capture_or_encoder_changed =
        capture_encoder_config_changed(previous, g_settings);
    const bool audio_changed = audio_config_changed(previous, g_settings);

    apply_runtime_settings();
    log_msg("settings", "settings reloaded from WinUI app");

    if (previous.hotkey_save_replay      != g_settings.hotkey_save_replay ||
        previous.hotkey_recording_start  != g_settings.hotkey_recording_start ||
        previous.hotkey_recording_stop   != g_settings.hotkey_recording_stop ||
        previous.hotkey_pause_resume     != g_settings.hotkey_pause_resume) {
        hotkeys_unregister(hwnd);
        hotkeys_register(hwnd);
        log_msg("hotkey", "hotkeys reloaded from settings");
    }

    if (previous.update_auto_check != g_settings.update_auto_check) {
        updater::set_auto_check(g_settings.update_auto_check);
        log_msg("updater", g_settings.update_auto_check
            ? "automatic update checks enabled"
            : "automatic update checks disabled");
    }

    // Component toggles gate commands/menu immediately, but the media pipeline
    // shape (capture/encoders started or skipped) is decided at media_start.
    if (previous.replay_buffer_enabled != g_settings.replay_buffer_enabled ||
        previous.recording_enabled     != g_settings.recording_enabled) {
        log_msg("settings", "component toggles saved: restart Monolith to fully apply");
    }

    if (!capture_or_encoder_changed && !audio_changed)
        return;

    if (g_recording.state() != recording::RecordingState::Idle) {
        log_msg("settings", "capture/audio changes deferred: manual recording is active");
        return;
    }

    if (capture_or_encoder_changed) {
        log_msg("settings", "capture/encoder settings changed: restarting capture pipeline");
        media_stop();
        g_replay.clear();
        media_start(hwnd);
        return;
    }

    if (audio_changed) {
        log_msg("settings", "audio routing changed: restarting audio pipeline");
        stop_audio_system();
        g_replay.clear();
        start_audio_system();
        publish_runtime_status();
    }
}

// ── Audio routing ─────────────────────────────────────────────────────────────

static settings::RuntimeAudioDevice runtime_audio_device(const audio::DeviceInfo& dev)
{
    settings::RuntimeAudioDevice out;
    out.id = dev.id;
    out.name = dev.name;
    out.default_device = dev.default_device;
    out.available = dev.available;
    return out;
}

static settings::RuntimeAudioSession runtime_audio_session(const audio::ProcessInfo& proc)
{
    settings::RuntimeAudioSession out;
    out.process_id = proc.process_id;
    out.process_name = proc.process_name;
    out.display_name = proc.display_name;
    out.executable_path = proc.executable_path;
    return out;
}

static void refresh_audio_runtime_status()
{
    auto devices = audio::enumerate_input_devices();
    auto sessions = audio::enumerate_render_sessions();
    auto active = audio::detect_active_game();

    std::lock_guard<std::mutex> lk(g_status_mutex);
    g_runtime_status.input_devices.clear();
    for (const auto& dev : devices)
        g_runtime_status.input_devices.push_back(runtime_audio_device(dev));

    g_runtime_status.audio_sessions.clear();
    for (const auto& session : sessions)
        g_runtime_status.audio_sessions.push_back(runtime_audio_session(session));

    {
        settings::ActiveGameStatus ag_status;
        ag_status.process_id      = active.process_id;
        ag_status.process_name    = active.process_name;
        ag_status.display_name    = active.display_name;
        ag_status.executable_path = active.executable_path;
        ag_status.capture_mode    = g_active_game_capture_mode.empty() ? "none" : g_active_game_capture_mode;
        ag_status.process_loopback_available = (g_active_game_capture_mode == "process_loopback");
        ag_status.poll_interval_ms  = g_settings.active_game.poll_interval_ms;
        ag_status.fast_scan_enabled = g_settings.active_game.fast_scan_enabled;
        g_runtime_status.active_game = ag_status;
    }
}

static std::vector<int> valid_tracks(const std::vector<int>& tracks)
{
    std::vector<int> result;
    for (int track : tracks) {
        if (track < 1 || track > 6) continue;
        if (std::find(result.begin(), result.end(), track) == result.end())
            result.push_back(track);
    }
    return result;
}

static bool open_audio_track(int track)
{
    if (track < 1 || track > 6) return false;
    encoding::AudioEncoder& encoder = g_audio_encoders[track - 1];
    if (encoder.is_open()) return true;

    encoding::AudioEncoder::Config acfg;
    acfg.sample_rate = 48000;
    acfg.channels = 2;
    acfg.bitrate = 192'000;
    acfg.stream_index = track;

    bool ok = encoder.open(acfg, [](encoding::EncodedPacket pkt) {
        if (g_replay_enabled.load(std::memory_order_relaxed)) {
            auto replay_pkt = pkt;
            g_replay.push(std::move(replay_pkt));
        }
        g_recording.push(std::move(pkt));
    });

    char msg[128];
    snprintf(msg, sizeof(msg), ok ? "AAC encoder opened for audio track %d"
                                  : "WARNING: AAC encoder failed for audio track %d",
             track);
    log_msg("encoding", msg);
    return ok;
}

static void close_audio_track(int track)
{
    if (track < 1 || track > 6) return;
    g_audio_encoders[track - 1].close();
}

static void publish_audio_params()
{
    std::vector<encoding::AudioStreamParams> params;
    for (auto& encoder : g_audio_encoders) {
        if (encoder.is_open())
            params.push_back(encoder.stream_params());
    }
    g_replay.set_audio_params(params);
    g_recording.set_audio_params(params);
}

static void push_audio_to_tracks(const std::vector<int>& tracks,
                                 const audio::PacketInfo& p,
                                 const char* log_tag)
{
    if (p.seq % 500 == 0) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "seq=%-6u  frames=%-4u  sr=%u  ch=%u tracks=%zu%s",
            p.seq, p.frame_count, p.sample_rate, p.channels,
            tracks.size(), p.silent ? "  [silent]" : "");
        log_msg(log_tag, buf);
    }

    if (p.data_bytes == 0) return;
    for (int track : tracks) {
        if (track < 1 || track > 6) continue;
        g_audio_encoders[track - 1].push_pcm(
            p.data, static_cast<int>(p.data_bytes),
            static_cast<int>(p.sample_rate),
            static_cast<int>(p.channels),
            static_cast<int>(p.bit_depth), p.is_float);
    }
}

static bool start_endpoint_source(audio::WasapiCapture::Mode mode,
                                  const std::wstring& device_id,
                                  std::vector<int> tracks,
                                  const char* log_tag,
                                  const char* label)
{
    tracks = valid_tracks(tracks);
    if (tracks.empty()) return false;
    for (int track : tracks) {
        if (!open_audio_track(track)) {
            for (int opened : tracks) {
                if (opened == track) break;
                close_audio_track(opened);
            }
            return false;
        }
    }

    auto capture = std::make_unique<audio::WasapiCapture>();
    bool ok = capture->start_device(mode, device_id,
        [tracks = std::move(tracks), log_tag](audio::PacketInfo const& p) {
            push_audio_to_tracks(tracks, p, log_tag);
        });

    if (ok) {
        g_audio_captures.push_back(std::move(capture));
        log_msg(log_tag, label);
        return true;
    }

    log_msg(log_tag, "WARNING: source failed to start");
    for (int track : tracks)
        close_audio_track(track);
    return false;
}

static bool start_process_source(uint32_t pid,
                                 std::vector<int> tracks,
                                 const char* log_tag,
                                 const char* label)
{
    tracks = valid_tracks(tracks);
    if (pid == 0 || tracks.empty()) return false;
    for (int track : tracks) {
        if (!open_audio_track(track)) {
            for (int opened : tracks) {
                if (opened == track) break;
                close_audio_track(opened);
            }
            return false;
        }
    }

    auto capture = std::make_unique<audio::WasapiCapture>();
    bool ok = capture->start_process_loopback(pid,
        [tracks = std::move(tracks), log_tag](audio::PacketInfo const& p) {
            push_audio_to_tracks(tracks, p, log_tag);
        });

    if (ok) {
        g_audio_captures.push_back(std::move(capture));
        log_msg(log_tag, label);
        return true;
    }

    log_msg(log_tag, "WARNING: process loopback failed");
    for (int track : tracks)
        close_audio_track(track);
    return false;
}

static uint32_t resolve_configured_process(const settings::AudioSourceConfig& source)
{
    if (source.process_id != 0)
        return source.process_id;

    std::wstring wanted_path = source.executable_path;
    std::string wanted_name = source.process_name;
    auto sessions = audio::enumerate_render_sessions();
    for (const auto& session : sessions) {
        if (!wanted_path.empty() && session.executable_path == wanted_path)
            return session.process_id;
        if (!wanted_name.empty() &&
            _stricmp(wide_to_utf8(session.process_name).c_str(), wanted_name.c_str()) == 0)
            return session.process_id;
    }
    return 0;
}

static void stop_audio_system()
{
    for (auto& capture : g_audio_captures) {
        if (capture) capture->stop();
    }
    g_audio_captures.clear();
    for (auto& encoder : g_audio_encoders)
        encoder.close();
    g_active_game_tracks.clear();
    g_active_game_pid           = 0;
    g_active_game_capture       = nullptr;
    g_active_game_capture_mode  = "none";
    g_pending_game_pid          = 0;
    g_pending_game_first_ms     = 0;
}

static void start_default_audio_system()
{
    std::vector<int> desktop_tracks{1};
    bool desktop_ok = start_endpoint_source(
        audio::WasapiCapture::Mode::Loopback,
        L"",
        desktop_tracks,
        "audio.sys",
        "WASAPI loopback started -> audio track 1");
    if (!desktop_ok) close_audio_track(1);

    std::vector<int> mic_tracks{2};
    bool mic_ok = start_endpoint_source(
        audio::WasapiCapture::Mode::Microphone,
        g_settings.primary_microphone_device_id,
        mic_tracks,
        "audio.mic",
        "WASAPI microphone started -> audio track 2");
    if (!mic_ok) close_audio_track(2);
}

static void start_custom_audio_system()
{
    std::array<std::string, 7> track_owner{};
    auto claim_tracks = [&](const settings::AudioSourceConfig& source) {
        std::vector<int> tracks;
        for (int track : valid_tracks(source.tracks)) {
            if (track_owner[track].empty()) {
                track_owner[track] = source.id.empty() ? source.name : source.id;
                tracks.push_back(track);
                continue;
            }
            char msg[256];
            snprintf(msg, sizeof(msg),
                "track %d already owned by %s; skipping source %s until mixer is implemented",
                track, track_owner[track].c_str(),
                source.name.empty() ? source.id.c_str() : source.name.c_str());
            log_msg("audio.route", msg);
        }
        return tracks;
    };

    for (const auto& source : g_settings.audio_sources) {
        if (!source.enabled) continue;
        std::vector<int> tracks = claim_tracks(source);
        if (tracks.empty()) continue;

        if (source.type == "desktop") {
            start_endpoint_source(audio::WasapiCapture::Mode::Loopback, L"",
                                  tracks, "audio.sys",
                                  "custom desktop audio started");
        } else if (source.type == "input") {
            start_endpoint_source(audio::WasapiCapture::Mode::Microphone,
                                  source.device_id,
                                  tracks, "audio.in",
                                  "custom input device started");
        } else if (source.type == "process") {
            uint32_t pid = resolve_configured_process(source);
            if (!start_process_source(pid, tracks, "audio.app",
                                      "custom process audio started")) {
                log_msg("audio.app", "configured process not running or not capturable");
            }
        } else if (source.type == "active_game") {
            g_active_game_tracks = tracks;
            // Initial acquire: call poll_active_game() inline after audio start to pick
            // up any game already running.  poll_active_game() uses the configured
            // blacklist/whitelist and will set g_active_game_pid/capture/mode.
            // If nothing is found yet the timer/hook will handle acquisition.
        }
    }
}

static void start_audio_system()
{
    stop_audio_system();
    refresh_audio_runtime_status();

    if (g_settings.audio_mode == "custom")
        start_custom_audio_system();
    else
        start_default_audio_system();

    publish_audio_params();
}

// True when the custom audio config contains an enabled active_game source.
static bool active_game_source_configured()
{
    if (g_settings.audio_mode != "custom") return false;
    for (const auto& source : g_settings.audio_sources) {
        if (source.enabled && source.type == "active_game") return true;
    }
    return false;
}

// Build an audio::DetectConfig from the currently loaded settings.
static audio::DetectConfig build_detect_config()
{
    audio::DetectConfig cfg;
    cfg.min_confidence = g_settings.active_game.min_confidence;
    for (const auto& s : g_settings.active_game.blacklist_processes)
        cfg.blacklist.push_back(utf8_to_wide(s));
    for (const auto& s : g_settings.active_game.whitelist_processes)
        cfg.whitelist.push_back(utf8_to_wide(s));
    for (const auto& s : g_settings.active_game.manual_games)
        cfg.manual_games.push_back(utf8_to_wide(s));
    return cfg;
}

// Drop the active-game capture if it is present (does NOT clear g_active_game_tracks).
static void drop_active_game_capture(const char* reason_msg)
{
    if (g_active_game_capture == nullptr) return;
    for (auto it = g_audio_captures.begin(); it != g_audio_captures.end(); ++it) {
        if (it->get() == g_active_game_capture) {
            (*it)->stop();
            g_audio_captures.erase(it);
            break;
        }
    }
    g_active_game_capture      = nullptr;
    g_active_game_pid          = 0;
    g_active_game_capture_mode = "none";
    g_pending_game_pid         = 0;
    g_pending_game_first_ms    = 0;
    log_msg("audio.game", reason_msg);
}

// Called from the UI thread (via WM_TIMER or WM_FAST_SCAN) to evaluate whether
// the Active Game source should be acquired, retained, or switched to a new process.
//
// Hysteresis: a new candidate must remain the best choice for switch_debounce_ms
// before the capture is swapped.  Only the Active Game capture is touched; other
// captures, encoders, and the replay buffer are never restarted.
static void poll_active_game()
{
    if (g_active_game_tracks.empty()) return;
    if (!g_settings.active_game.detection_enabled) return;

    // Drop current capture if the process has exited.
    if (g_active_game_pid != 0 && !audio::process_alive(g_active_game_pid))
        drop_active_game_capture("active game exited; watching for a new one");

    // Detect best candidate with config-driven blacklist/whitelist.
    audio::ActiveGameResult result = audio::detect_active_game(build_detect_config());
    uint32_t best_pid = result.process.process_id;

    // ── No valid candidate ────────────────────────────────────────────────────
    if (best_pid == 0) {
        g_pending_game_pid      = 0;
        g_pending_game_first_ms = 0;
        return;
    }

    // ── Same as current live capture: refresh status, no change ───────────────
    if (best_pid == g_active_game_pid) {
        g_pending_game_pid      = 0;
        g_pending_game_first_ms = 0;
        {
            std::lock_guard<std::mutex> lk(g_status_mutex);
            g_runtime_status.active_game.confidence   = result.confidence;
            g_runtime_status.active_game.reason       = result.reason;
            g_runtime_status.active_game.poll_interval_ms  = g_settings.active_game.poll_interval_ms;
            g_runtime_status.active_game.fast_scan_enabled = g_settings.active_game.fast_scan_enabled;
        }
        return;
    }

    // ── No current capture and a valid candidate: acquire immediately ──────────
    if (g_active_game_pid == 0) {
        bool ok = start_process_source(best_pid, g_active_game_tracks,
                                       "audio.game", "active game audio started");
        if (ok) {
            g_active_game_pid          = best_pid;
            g_active_game_capture      = g_audio_captures.back().get();
            g_active_game_capture_mode = "process_loopback";
        } else {
            g_active_game_capture_mode = "unavailable";
            log_msg("audio.game", "process-loopback unavailable for active game candidate");
        }
        g_pending_game_pid      = 0;
        g_pending_game_first_ms = 0;
        publish_audio_params();
        {
            SYSTEMTIME st; GetLocalTime(&st);
            char ts[32]; snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            std::lock_guard<std::mutex> lk(g_status_mutex);
            g_runtime_status.active_game.process_id      = ok ? best_pid : 0;
            g_runtime_status.active_game.process_name    = result.process.process_name;
            g_runtime_status.active_game.display_name    = result.process.display_name;
            g_runtime_status.active_game.executable_path = result.process.executable_path;
            g_runtime_status.active_game.confidence      = result.confidence;
            g_runtime_status.active_game.reason          = result.reason;
            g_runtime_status.active_game.capture_mode    = g_active_game_capture_mode;
            g_runtime_status.active_game.process_loopback_available = ok;
            g_runtime_status.active_game.last_switch_time = ts;
            g_runtime_status.active_game.poll_interval_ms  = g_settings.active_game.poll_interval_ms;
            g_runtime_status.active_game.fast_scan_enabled = g_settings.active_game.fast_scan_enabled;
        }
        publish_runtime_status();
        return;
    }

    // ── Different candidate: debounce before switching ────────────────────────
    DWORD now_ms = GetTickCount();
    if (g_pending_game_pid != best_pid) {
        // New pending candidate; reset the debounce timer.
        g_pending_game_pid      = best_pid;
        g_pending_game_first_ms = now_ms;
        return;
    }

    // Same pending candidate — check whether debounce period has elapsed.
    DWORD elapsed = now_ms - g_pending_game_first_ms;
    if (elapsed < static_cast<DWORD>(g_settings.active_game.switch_debounce_ms))
        return; // Not yet; keep waiting.

    // Debounce passed → swap the Active Game capture.
    // Start the new capture first (so there is minimal gap), then stop the old one.
    bool ok = start_process_source(best_pid, g_active_game_tracks,
                                   "audio.game", "active game switched");
    if (!ok) {
        g_active_game_capture_mode = "unavailable";
        log_msg("audio.game", "process-loopback unavailable for new game; switch aborted");
        g_pending_game_pid      = 0;
        g_pending_game_first_ms = 0;
        return;
    }

    audio::WasapiCapture* new_capture = g_audio_captures.back().get();

    // Remove old capture — only the Active Game source, nothing else.
    if (g_active_game_capture != nullptr) {
        for (auto it = g_audio_captures.begin(); it != g_audio_captures.end(); ++it) {
            if (it->get() == g_active_game_capture) {
                (*it)->stop();
                g_audio_captures.erase(it);
                break;
            }
        }
    }

    g_active_game_pid          = best_pid;
    g_active_game_capture      = new_capture;
    g_active_game_capture_mode = "process_loopback";
    g_pending_game_pid         = 0;
    g_pending_game_first_ms    = 0;

    publish_audio_params();
    {
        SYSTEMTIME st; GetLocalTime(&st);
        char ts[32]; snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::lock_guard<std::mutex> lk(g_status_mutex);
        g_runtime_status.active_game.process_id      = best_pid;
        g_runtime_status.active_game.process_name    = result.process.process_name;
        g_runtime_status.active_game.display_name    = result.process.display_name;
        g_runtime_status.active_game.executable_path = result.process.executable_path;
        g_runtime_status.active_game.confidence      = result.confidence;
        g_runtime_status.active_game.reason          = result.reason;
        g_runtime_status.active_game.capture_mode    = "process_loopback";
        g_runtime_status.active_game.process_loopback_available = true;
        g_runtime_status.active_game.last_switch_time = ts;
        g_runtime_status.active_game.poll_interval_ms  = g_settings.active_game.poll_interval_ms;
        g_runtime_status.active_game.fast_scan_enabled = g_settings.active_game.fast_scan_enabled;
    }
    publish_runtime_status();
}

// WinEvent hook callback — called on the UI thread when the foreground window changes.
// Rate-limited fast scan: coalesces rapid events by checking g_fast_scan_pending,
// then posts WM_FAST_SCAN so poll_active_game() runs on the next message-loop turn.
static void CALLBACK active_game_fg_hook(
    HWINEVENTHOOK /*hWinEventHook*/, DWORD /*event*/, HWND /*hwnd*/,
    LONG /*idObject*/, LONG /*idChild*/, DWORD /*dwEventThread*/, DWORD /*dwmsEventTime*/)
{
    if (g_fast_scan_pending) return; // already queued; coalesce
    g_fast_scan_pending = true;
    PostMessageW(g_main_hwnd, WM_FAST_SCAN, 0, 0);
}

// Install / uninstall the foreground-change WinEvent hook for fast scan.
static void install_fg_hook()
{
    if (g_fg_hook) return;
    g_fg_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr,               // no DLL — out-of-context callback
        active_game_fg_hook,
        0, 0,                  // all processes/threads
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

static void uninstall_fg_hook()
{
    if (!g_fg_hook) return;
    UnhookWinEvent(g_fg_hook);
    g_fg_hook = nullptr;
}

// ── Media start / stop ────────────────────────────────────────────────────────

static void media_start(HWND hwnd)
{
    // ── Capture monitor from config (restart-required setting) ───────────────
    std::vector<MonitorEntry> monitors = enumerate_monitors();
    std::wstring used_device;
    HMONITOR hmon = monitor_from_settings(monitors, &used_device);
    log_path("capture", "capture monitor: ",
             used_device.empty() ? L"(primary)" : used_device);

    {
        std::lock_guard<std::mutex> lk(g_status_mutex);
        g_runtime_status.monitors.clear();
        for (const auto& entry : monitors)
            g_runtime_status.monitors.push_back(entry.info);
        g_runtime_status.active_monitor_device = used_device;
        // Probe with a representative size; availability does not depend on it.
        g_runtime_status.available_encoders =
            encoding::available_video_encoders(1920, 1080);
    }

    g_video_enc_open_attempted.store(false, std::memory_order_release);
    g_enc_w = 0;
    g_enc_h = 0;

    // ── Video encoder ─────────────────────────────────────────────────────────
    log_msg("encoding", "video encoder deferred until first WGC frame");

    // ── Start pacer thread (OBS-style fixed-rate encoder feed) ────────────────
    pacer_start();

    // ── Replay buffer config ───────────────────────────────────────────────────
    {
        apply_runtime_settings();
    }

    // Both components off: skip the whole media pipeline (low-end mode).
    if (!g_settings.replay_buffer_enabled && !g_settings.recording_enabled) {
        log_msg("app", "recording + replay buffer disabled: media pipeline not started");
        return;
    }

    // ── Audio capture/routing ──────────────────────────────────────────────────
    g_main_hwnd = hwnd;
    start_audio_system();
    if (active_game_source_configured()) {
        UINT poll_ms = static_cast<UINT>(
            std::max(3000, std::min(30000, g_settings.active_game.poll_interval_ms)));
        SetTimer(hwnd, kActiveGameTimerId, poll_ms, nullptr);
        if (g_settings.active_game.fast_scan_enabled)
            install_fg_hook();
        // Initial detection pass (doesn't wait for first timer tick).
        poll_active_game();
    }

    // ── WGC display capture ───────────────────────────────────────────────────
    if (capture::is_supported()) {
        bool ok = g_video.start(hmon, [](capture::FrameInfo const& f) {
            // Log buffer stats every 300 frames (~5s at 60fps).
            if (f.seq % 300 == 0) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                    "seq=%-6u  %ux%u  buf=%zu pkt / %.1f MB",
                    f.seq, f.width, f.height,
                    g_replay.packet_count(),
                    static_cast<double>(g_replay.memory_bytes()) / 1048576.0);
                log_msg("capture", buf);
            }
            // Push BGRA frame to pacer. Pacer thread drives encoder at fixed FPS.
            if (f.bgra_data == nullptr) return;
            // Replay buffer disabled: encode only while a manual recording
            // is running, so an idle tray app costs no encoder CPU.
            if (!g_replay_enabled.load(std::memory_order_relaxed) &&
                g_recording.state() == recording::RecordingState::Idle)
                return;
            if (!g_video_enc.is_open()) {
                bool expected = false;
                if (!g_video_enc_open_attempted.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    return;
                }

                // Output resolution: capture-native unless config requests custom.
                if (g_settings.resolution_mode == "custom" &&
                    g_settings.output_width > 0 && g_settings.output_height > 0) {
                    g_enc_w = g_settings.output_width;
                    g_enc_h = g_settings.output_height;
                } else {
                    g_enc_w = static_cast<int>(f.width & ~1u);
                    g_enc_h = static_cast<int>(f.height & ~1u);
                }

                encoding::VideoEncoder::Config vcfg;
                vcfg.width             = g_enc_w;
                vcfg.height            = g_enc_h;
                vcfg.fps               = g_settings.video_fps;
                vcfg.bitrate           = static_cast<int64_t>(g_settings.video_bitrate_kbps) * 1000;
                vcfg.preferred_encoder = g_settings.encoder_backend;
                vcfg.extra_options     = g_settings.extra_ffmpeg_options;

                bool enc_ok = g_video_enc.open(vcfg, [](encoding::EncodedPacket pkt) {
                    if (g_replay_enabled.load(std::memory_order_relaxed)) {
                        auto replay_pkt = pkt;
                        g_replay.push(std::move(replay_pkt));
                    }
                    g_recording.push(std::move(pkt));
                });

                if (enc_ok) {
                    char enc_msg[256];
                    snprintf(enc_msg, sizeof(enc_msg),
                        "%s %dx%d @%dfps bitrate=%dkbps%s%s",
                        g_video_enc.encoder_name().c_str(), g_enc_w, g_enc_h,
                        g_settings.video_fps,
                        g_settings.video_bitrate_kbps,
                        g_settings.extra_ffmpeg_options.empty() ? "" :
                            (g_video_enc.extra_options_rejected()
                                ? "  extra_options=REJECTED (opened without them)"
                                : "  extra_options="),
                        (!g_settings.extra_ffmpeg_options.empty() &&
                         !g_video_enc.extra_options_rejected())
                            ? g_settings.extra_ffmpeg_options.c_str() : "");
                    log_msg("encoding", enc_msg);
                } else {
                    log_msg("encoding", "WARNING: no video encoder - replay disabled");
                }

                if (enc_ok) {
                    auto params = g_video_enc.stream_params();
                    g_replay.set_video_params(params);
                    g_recording.set_video_params(params);
                    {
                        std::lock_guard<std::mutex> lk(g_status_mutex);
                        g_runtime_status.active_encoder = g_video_enc.encoder_name();
                        g_runtime_status.encode_width   = g_enc_w;
                        g_runtime_status.encode_height  = g_enc_h;
                    }
                    publish_runtime_status();
                } else {
                    return;
                }
            }
            pacer_push_frame(f.bgra_data,
                             static_cast<int>(f.bgra_stride),
                             static_cast<int>(f.width),
                             static_cast<int>(f.height));
        }, g_settings.show_capture_border);
        log_msg("capture", ok ? "WGC display capture started"
                               : "WARNING: WGC display capture failed");
        if (ok) {
            log_msg("capture", g_settings.show_capture_border
                ? "capture border: enabled by config"
                : (g_video.border_suppressed()
                    ? "capture border: suppressed (IsBorderRequired=false)"
                    : "capture border: suppression DENIED by OS (border visible)"));
        }
        {
            std::lock_guard<std::mutex> lk(g_status_mutex);
            g_runtime_status.border_suppressed = g_video.border_suppressed();
        }
    } else {
        log_msg("capture", "WARNING: Windows.Graphics.Capture not supported");
    }

}

static void media_stop()
{
    pacer_stop();   // stop video thread before capture/encoder
    g_video.stop();
    stop_audio_system();
    g_video_enc.close(); // flushes + frees encoder
    if (g_recording.state() != recording::RecordingState::Idle) {
        std::wstring path;
        g_recording.stop(&path);
        if (!path.empty()) log_path("recording", "recording saved: ", path);
    }
    g_video_enc_open_attempted.store(false, std::memory_order_release);
    log_msg("app", "capture + audio + encoding stopped");
}

// ── System tray ───────────────────────────────────────────────────────────────

static NOTIFYICONDATAW g_nid;
static HICON g_icon_base = nullptr;
static HICON g_icon_rec  = nullptr; // created lazily on first recording start

static HICON create_rec_icon()
{
    int sz = GetSystemMetrics(SM_CXSMICON);
    if (sz <= 0) sz = 16;

    HICON base = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_MONOLITH),
        IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR));
    if (!base) return nullptr;

    HDC screen = GetDC(nullptr);
    HDC dc     = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    BITMAPV4HEADER bmi{};
    bmi.bV4Size          = sizeof(bmi);
    bmi.bV4Width         = sz;
    bmi.bV4Height        = -sz; // top-down
    bmi.bV4Planes        = 1;
    bmi.bV4BitCount      = 32;
    bmi.bV4V4Compression = BI_BITFIELDS;
    bmi.bV4RedMask       = 0x00FF0000;
    bmi.bV4GreenMask     = 0x0000FF00;
    bmi.bV4BlueMask      = 0x000000FF;
    bmi.bV4AlphaMask     = 0xFF000000;

    void* raw = nullptr;
    HBITMAP dib = CreateDIBSection(dc,
        reinterpret_cast<BITMAPINFO*>(&bmi),
        DIB_RGB_COLORS, &raw, nullptr, 0);
    if (!dib) { DestroyIcon(base); DeleteDC(dc); return nullptr; }

    auto* pixels = static_cast<uint32_t*>(raw);
    HBITMAP prev = static_cast<HBITMAP>(SelectObject(dc, dib));

    DrawIconEx(dc, 0, 0, base, sz, sz, 0, nullptr, DI_NORMAL);
    DestroyIcon(base);

    // If the base icon has no alpha channel (old-style XOR icon), set opaque
    bool has_alpha = false;
    for (int i = 0; i < sz * sz; ++i)
        if (pixels[i] >> 24) { has_alpha = true; break; }
    if (!has_alpha)
        for (int i = 0; i < sz * sz; ++i)
            if (pixels[i] & 0x00FFFFFF) pixels[i] |= 0xFF000000u;

    // Red dot: bottom-right, radius ~20% of icon size
    float r    = sz * 0.21f;
    float cx_f = sz - r - 0.5f;
    float cy_f = sz - r - 0.5f;
    float r2   = r * r;
    float ri2  = (r - 1.4f) * (r - 1.4f); // inner radius for border

    for (int py = 0; py < sz; ++py) {
        for (int px_i = 0; px_i < sz; ++px_i) {
            float dx = px_i + 0.5f - cx_f;
            float dy = py  + 0.5f - cy_f;
            float d2 = dx*dx + dy*dy;
            if (d2 > r2) continue;

            // Border (dark) vs fill (bright red)
            uint8_t nr = (d2 > ri2) ? 40  : 220;
            uint8_t ng = (d2 > ri2) ? 0   : 45;
            uint8_t nb = (d2 > ri2) ? 0   : 45;

            uint32_t ex  = pixels[py * sz + px_i];
            uint8_t  ea  = static_cast<uint8_t>(ex >> 24);
            uint8_t  er  = static_cast<uint8_t>(ex >> 16);
            uint8_t  eg  = static_cast<uint8_t>(ex >>  8);
            uint8_t  eb  = static_cast<uint8_t>(ex);

            // Hard blend: full opacity for the dot
            uint8_t out_a = 255;
            uint8_t out_r = static_cast<uint8_t>((nr * 200u + er * 55u) / 255u);
            uint8_t out_g = static_cast<uint8_t>((ng * 200u + eg * 55u) / 255u);
            uint8_t out_b = static_cast<uint8_t>((nb * 200u + eb * 55u) / 255u);
            (void)ea;

            pixels[py * sz + px_i] =
                (static_cast<uint32_t>(out_a) << 24) |
                (static_cast<uint32_t>(out_r) << 16) |
                (static_cast<uint32_t>(out_g) <<  8) |
                 static_cast<uint32_t>(out_b);
        }
    }

    SelectObject(dc, prev);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmColor = dib;
    ii.hbmMask  = mask;
    HICON result = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(dib);
    return result;
}

static void tray_set_recording(bool recording)
{
    if (recording) {
        if (!g_icon_rec) g_icon_rec = create_rec_icon();
        if (!g_icon_rec) return;
        g_nid.uFlags = NIF_ICON;
        g_nid.hIcon  = g_icon_rec;
    } else {
        g_nid.uFlags = NIF_ICON;
        g_nid.hIcon  = g_icon_base;
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void tray_add(HWND hwnd)
{
    g_nid                  = {};
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconW(GetModuleHandleW(nullptr),
                                       MAKEINTRESOURCEW(IDI_MONOLITH));
    if (!g_nid.hIcon)
        g_nid.hIcon        = LoadIconW(nullptr, IDI_APPLICATION);
    g_icon_base = g_nid.hIcon;
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), kAppName);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
        log_msg("tray", "WARNING: Shell_NotifyIconW NIM_ADD failed");
}

static void tray_remove() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

static std::string hotkey_or_default(const std::string& configured, const char* fallback);

static void tray_show_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    recording::RecordingState state = g_recording.state();
    const bool replay_on    = g_replay_enabled.load(std::memory_order_relaxed);
    const bool recording_on = g_recording_enabled.load(std::memory_order_relaxed);
    // Save Replay needs the component enabled and the encoder feeding the ring.
    UINT save_flags  = (replay_on && g_video_enc.is_open()) ? MF_STRING : (MF_STRING | MF_GRAYED);
    UINT start_flags = (recording_on && state == recording::RecordingState::Idle) ? MF_STRING : (MF_STRING | MF_GRAYED);
    UINT stop_flags  = (state == recording::RecordingState::Idle) ? (MF_STRING | MF_GRAYED) : MF_STRING;
    UINT pause_flags = (state == recording::RecordingState::Idle) ? (MF_STRING | MF_GRAYED) : MF_STRING;
    std::wstring save_text = L"Save Replay\t" + utf8_to_wide(
        hotkey_or_default(g_settings.hotkey_save_replay, "Ctrl+Shift+F8"));
    std::wstring start_text = L"Start Recording\t" + utf8_to_wide(
        hotkey_or_default(g_settings.hotkey_recording_start, "Ctrl+Shift+F9"));
    std::wstring stop_text = L"Stop Recording\t" + utf8_to_wide(
        hotkey_or_default(g_settings.hotkey_recording_stop, "Ctrl+Shift+F10"));
    std::wstring pause_hotkey = utf8_to_wide(
        hotkey_or_default(g_settings.hotkey_pause_resume, "Ctrl+Shift+F11"));
    std::wstring pause_text =
        (state == recording::RecordingState::Paused)
            ? (L"Resume Recording\t" + pause_hotkey)
            : (L"Pause Recording\t" + pause_hotkey);
    AppendMenuW(menu, save_flags,            CMD_SAVE_REPLAY,     save_text.c_str());
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, start_flags,            CMD_RECORDING_START, start_text.c_str());
    AppendMenuW(menu, stop_flags,             CMD_RECORDING_STOP,  stop_text.c_str());
    AppendMenuW(menu, pause_flags,            CMD_PAUSE_RESUME,    pause_text.c_str());
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, MF_STRING,             CMD_SETTINGS,        L"Settings\x2026");
    AppendMenuW(menu, MF_STRING,             CMD_CHECK_UPDATE,    L"Check for Updates\x2026");
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, MF_STRING,             CMD_EXIT,            L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// ── Hotkeys ───────────────────────────────────────────────────────────────────

struct HotkeySpec {
    std::vector<UINT> keys;
};

struct HotkeyBinding {
    Cmd command;
    std::string name;
    std::string label;
    HotkeySpec spec;
    bool active = false;
};

static HHOOK g_hotkey_hook = nullptr;
static HWND g_hotkey_hwnd = nullptr;
static bool g_key_down[256] = {};
static std::vector<HotkeyBinding> g_hotkey_bindings;

static std::string upper_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

static UINT key_vk(const std::string& token)
{
    std::string key = upper_ascii(token);
    if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z')
        return static_cast<UINT>(key[0]);
    if (key.size() == 1 && key[0] >= '0' && key[0] <= '9')
        return static_cast<UINT>(key[0]);
    if (key.size() == 1) {
        switch (key[0]) {
        case ';': return VK_OEM_1;
        case '=': return VK_OEM_PLUS;
        case ',': return VK_OEM_COMMA;
        case '-': return VK_OEM_MINUS;
        case '.': return VK_OEM_PERIOD;
        case '/': return VK_OEM_2;
        case '`': return VK_OEM_3;
        case '[': return VK_OEM_4;
        case '\\': return VK_OEM_5;
        case ']': return VK_OEM_6;
        case '\'': return VK_OEM_7;
        default: break;
        }
    }
    if (key.size() >= 2 && key[0] == 'F') {
        int n = atoi(key.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + static_cast<UINT>(n - 1);
    }
    if (key == "SPACE") return VK_SPACE;
    if (key == "TAB") return VK_TAB;
    if (key == "ENTER") return VK_RETURN;
    if (key == "BACKSPACE") return VK_BACK;
    if (key == "ESC" || key == "ESCAPE") return VK_ESCAPE;
    if (key == "INSERT" || key == "INS") return VK_INSERT;
    if (key == "DELETE" || key == "DEL") return VK_DELETE;
    if (key == "HOME") return VK_HOME;
    if (key == "END") return VK_END;
    if (key == "PAGEUP" || key == "PGUP") return VK_PRIOR;
    if (key == "PAGEDOWN" || key == "PGDN") return VK_NEXT;
    if (key == "UP") return VK_UP;
    if (key == "DOWN") return VK_DOWN;
    if (key == "LEFT") return VK_LEFT;
    if (key == "RIGHT") return VK_RIGHT;
    if (key == "NUMPAD0") return VK_NUMPAD0;
    if (key == "NUMPAD1") return VK_NUMPAD1;
    if (key == "NUMPAD2") return VK_NUMPAD2;
    if (key == "NUMPAD3") return VK_NUMPAD3;
    if (key == "NUMPAD4") return VK_NUMPAD4;
    if (key == "NUMPAD5") return VK_NUMPAD5;
    if (key == "NUMPAD6") return VK_NUMPAD6;
    if (key == "NUMPAD7") return VK_NUMPAD7;
    if (key == "NUMPAD8") return VK_NUMPAD8;
    if (key == "NUMPAD9") return VK_NUMPAD9;
    if (key == "NUMPADMULTIPLY") return VK_MULTIPLY;
    if (key == "NUMPADADD") return VK_ADD;
    if (key == "NUMPADSUBTRACT") return VK_SUBTRACT;
    if (key == "NUMPADDECIMAL") return VK_DECIMAL;
    if (key == "NUMPADDIVIDE") return VK_DIVIDE;
    return 0;
}

static bool parse_hotkey(const std::string& text, HotkeySpec* out)
{
    HotkeySpec spec;
    size_t start = 0;
    bool saw_key = false;
    while (start <= text.size()) {
        size_t end = text.find('+', start);
        std::string token = text.substr(start, end == std::string::npos ? end : end - start);
        token.erase(std::remove_if(token.begin(), token.end(),
            [](unsigned char c) { return std::isspace(c) != 0; }), token.end());
        if (token.empty()) return false;

        std::string upper = upper_ascii(token);
        UINT vk = 0;
        if (upper == "CTRL" || upper == "CONTROL") vk = VK_CONTROL;
        else if (upper == "SHIFT") vk = VK_SHIFT;
        else if (upper == "ALT") vk = VK_MENU;
        else if (upper == "WIN" || upper == "WINDOWS") vk = VK_LWIN;
        else vk = key_vk(token);

        if (vk == 0) return false;
        if (std::find(spec.keys.begin(), spec.keys.end(), vk) != spec.keys.end())
            return false;
        spec.keys.push_back(vk);
        if (vk != VK_CONTROL && vk != VK_SHIFT && vk != VK_MENU &&
            vk != VK_LWIN && vk != VK_RWIN)
            saw_key = true;

        if (end == std::string::npos) break;
        start = end + 1;
    }

    if (!saw_key || spec.keys.empty()) return false;
    *out = spec;
    return true;
}

static std::string hotkey_or_default(const std::string& configured, const char* fallback)
{
    HotkeySpec spec;
    return parse_hotkey(configured, &spec) ? configured : std::string(fallback);
}

static void register_one_hotkey(
    HWND hwnd,
    Cmd command,
    const char* name,
    const std::string& configured,
    const char* fallback)
{
    (void)hwnd;

    // "NONE" means the hotkey is disabled — skip registration entirely
    std::string upper_configured = upper_ascii(configured);
    if (upper_configured == "NONE") {
        log_msg("hotkey", (std::string(name) + " disabled (NONE)").c_str());
        return;
    }

    std::string label = hotkey_or_default(configured, fallback);
    HotkeySpec spec;
    if (!parse_hotkey(label, &spec)) {
        log_msg("hotkey", (std::string("WARNING: ") + name + " (" + label + ") invalid").c_str());
        return;
    }
    g_hotkey_bindings.push_back({ command, name, label, std::move(spec), false });
    log_msg("hotkey", (std::string(name) + " (" + label + ") registered").c_str());
}

static bool key_down(UINT vk)
{
    if (vk == VK_CONTROL)
        return g_key_down[VK_CONTROL] || g_key_down[VK_LCONTROL] || g_key_down[VK_RCONTROL];
    if (vk == VK_SHIFT)
        return g_key_down[VK_SHIFT] || g_key_down[VK_LSHIFT] || g_key_down[VK_RSHIFT];
    if (vk == VK_MENU)
        return g_key_down[VK_MENU] || g_key_down[VK_LMENU] || g_key_down[VK_RMENU];
    if (vk == VK_LWIN || vk == VK_RWIN)
        return g_key_down[VK_LWIN] || g_key_down[VK_RWIN];
    return vk < 256 && g_key_down[vk];
}

static bool hotkey_down(const HotkeySpec& spec)
{
    for (UINT vk : spec.keys) {
        if (!key_down(vk)) return false;
    }
    return true;
}

static void set_key_state(UINT vk, bool down)
{
    if (vk >= 256) return;
    g_key_down[vk] = down;
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) g_key_down[VK_CONTROL] = down;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) g_key_down[VK_SHIFT] = down;
    if (vk == VK_LMENU || vk == VK_RMENU) g_key_down[VK_MENU] = down;
}

static LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code < 0)
        return CallNextHookEx(g_hotkey_hook, code, wp, lp);

    auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
    const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    const bool up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
    if (!down && !up)
        return CallNextHookEx(g_hotkey_hook, code, wp, lp);

    set_key_state(info->vkCode, down);

    if (up) {
        for (auto& binding : g_hotkey_bindings) {
            if (!hotkey_down(binding.spec))
                binding.active = false;
        }
        return CallNextHookEx(g_hotkey_hook, code, wp, lp);
    }

    for (auto& binding : g_hotkey_bindings) {
        if (!hotkey_down(binding.spec)) {
            binding.active = false;
            continue;
        }

        if (!binding.active && g_hotkey_hwnd) {
            binding.active = true;
            PostMessageW(g_hotkey_hwnd, WM_COMMAND, MAKEWPARAM(binding.command, 0), 0);
        }
        break;
    }

    return CallNextHookEx(g_hotkey_hook, code, wp, lp);
}

static void hotkeys_register(HWND hwnd)
{
    hotkeys_unregister(hwnd);
    g_hotkey_hwnd = hwnd;
    memset(g_key_down, 0, sizeof(g_key_down));

    register_one_hotkey(hwnd, CMD_SAVE_REPLAY, "save_replay",
        g_settings.hotkey_save_replay, "Ctrl+Shift+F8");
    register_one_hotkey(hwnd, CMD_RECORDING_START, "recording_start",
        g_settings.hotkey_recording_start, "Ctrl+Shift+F9");
    register_one_hotkey(hwnd, CMD_RECORDING_STOP, "recording_stop",
        g_settings.hotkey_recording_stop, "Ctrl+Shift+F10");
    register_one_hotkey(hwnd, CMD_PAUSE_RESUME, "pause_resume",
        g_settings.hotkey_pause_resume, "Ctrl+Shift+F11");

    std::sort(g_hotkey_bindings.begin(), g_hotkey_bindings.end(),
        [](const HotkeyBinding& a, const HotkeyBinding& b) {
            return a.spec.keys.size() > b.spec.keys.size();
        });

    g_hotkey_hook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        keyboard_hook_proc,
        GetModuleHandleW(nullptr),
        0);
    if (!g_hotkey_hook)
        log_msg("hotkey", "WARNING: low-level keyboard hook failed");
}

static void hotkeys_unregister(HWND hwnd)
{
    (void)hwnd;
    if (g_hotkey_hook) {
        UnhookWindowsHookEx(g_hotkey_hook);
        g_hotkey_hook = nullptr;
    }
    g_hotkey_bindings.clear();
    g_hotkey_hwnd = nullptr;
    memset(g_key_down, 0, sizeof(g_key_down));
}

// ── Command dispatch ──────────────────────────────────────────────────────────

static void dispatch(Cmd cmd, HWND hwnd)
{
    switch (cmd) {
    case CMD_SAVE_REPLAY: {
        if (!g_replay_enabled.load(std::memory_order_relaxed)) {
            log_msg("replay", "save_replay ignored: replay buffer disabled in settings");
            break;
        }
        char stats[256];
        snprintf(stats, sizeof(stats),
            "save_replay — buffer: %zu pkt / %.1f MB",
            g_replay.packet_count(),
            static_cast<double>(g_replay.memory_bytes()) / 1048576.0);
        log_msg("replay", stats);
        feedback::play(feedback::Sound::ClipSaved);
        g_replay.save_clip([](std::wstring path) {
            if (path.empty()) {
                log_msg("replay", "WARNING: clip save failed or encoder not ready");
            } else {
                log_path("replay", "clip saved: ", path);
            }
        });
        break;
    }
    case CMD_RECORDING_START:
        if (!g_recording_enabled.load(std::memory_order_relaxed)) {
            log_msg("recording", "recording_start ignored: recording disabled in settings");
            break;
        }
        if (g_recording.start(g_recordings_dir, g_recording_container)) {
            log_path("recording", "recording started: ", g_recording.current_path());
            feedback::play(feedback::Sound::RecordStart);
            tray_set_recording(true);
        } else {
            char msg[96];
            snprintf(msg, sizeof(msg), "recording_start rejected: state=%s",
                     recording::state_name(g_recording.state()));
            log_msg("recording", msg);
        }
        break;
    case CMD_RECORDING_STOP: {
        std::wstring path;
        if (g_recording.stop(&path)) {
            if (path.empty()) log_msg("recording", "recording stopped: no packets written");
            else log_path("recording", "recording saved: ", path);
            feedback::play(feedback::Sound::RecordStop);
            tray_set_recording(false);
        } else {
            log_msg("recording", "recording_stop rejected: state=idle");
        }
        break;
    }
    case CMD_PAUSE_RESUME:
        if (g_recording.state() == recording::RecordingState::Paused) {
            if (g_recording.resume()) log_msg("recording", "recording resumed");
        } else if (g_recording.pause()) {
            log_msg("recording", "recording paused");
        } else {
            char msg[96];
            snprintf(msg, sizeof(msg), "pause_resume rejected: state=%s",
                     recording::state_name(g_recording.state()));
            log_msg("recording", msg);
        }
        break;
    case CMD_SETTINGS:
        log_msg("tray", "settings opened");
        show_settings(hwnd);
        break;
    case CMD_CHECK_UPDATE:
        log_msg("updater", "manual update check requested");
        updater::check_now();
        break;
    case CMD_EXIT:
        log_msg("app", "exit requested");
        DestroyWindow(hwnd);
        break;
    }
}

// ── Window procedure ──────────────────────────────────────────────────────────

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        tray_add(hwnd);
        hotkeys_register(hwnd);
        media_start(hwnd);
        ipc::start(hwnd, [&]() -> ipc::RecordingState {
            auto st = g_recording.state();
            return {
                st == recording::RecordingState::Recording,
                st == recording::RecordingState::Paused,
                g_settings.replay_buffer_enabled,
                g_settings.recording_enabled,
            };
        });
        publish_runtime_status();
        log_msg("app", "app shell ready");
        return 0;

    case WM_TIMER:
        if (wp == kActiveGameTimerId) {
            poll_active_game();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_FAST_SCAN:
        // Foreground-change fast scan: rate-limited by fast_scan_min_interval_ms.
        g_fast_scan_pending = false;
        if (g_settings.active_game.fast_scan_enabled) {
            DWORD now_ms = GetTickCount();
            DWORD min_gap = static_cast<DWORD>(
                std::max(500, std::min(5000, g_settings.active_game.fast_scan_min_interval_ms)));
            if ((now_ms - g_last_fast_scan_ms) >= min_gap) {
                g_last_fast_scan_ms = now_ms;
                poll_active_game();
            }
        }
        return 0;

    case WM_DESTROY:
        settings_window::close_running();
        KillTimer(hwnd, kActiveGameTimerId);
        uninstall_fg_hook();
        updater::shutdown();
        ipc::stop();
        media_stop();
        hotkeys_unregister(hwnd);
        tray_remove();
        if (g_icon_rec) { DestroyIcon(g_icon_rec); g_icon_rec = nullptr; }
        log_msg("app", "app shell stopped");
        PostQuitMessage(0);
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
            apply_native_window_theme(hwnd);
            tray_show_menu(hwnd);
        }
        return 0;

    case WM_SETTINGCHANGE:
        apply_native_window_theme(hwnd);
        return 0;

    case WM_COMMAND:
        dispatch(static_cast<Cmd>(LOWORD(wp)), hwnd);
        return 0;

    case WM_SETTINGS_RELOAD:
        reload_settings_from_disk(hwnd);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ── Entry point ───────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    log_init();
    log_msg("app", "initializing (Monolith: replay + manual recording)");
    load_app_settings();
    updater::init(g_settings.update_auto_check);
    if (g_settings.update_auto_check)
        updater::check_silent(); // immediate check at startup

    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        log_msg("app", "FATAL: RegisterClassExW failed");
        winrt::uninit_apartment();
        CloseHandle(mutex);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, kWindowClass, kAppName,
        WS_OVERLAPPED, 0, 0, 0, 0,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        log_msg("app", "FATAL: CreateWindowExW failed");
        winrt::uninit_apartment();
        CloseHandle(mutex);
        return 1;
    }
    apply_native_window_theme(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    winrt::uninit_apartment();
    CloseHandle(mutex);
    if (g_log) fclose(g_log);
    return static_cast<int>(msg.wParam);
}
