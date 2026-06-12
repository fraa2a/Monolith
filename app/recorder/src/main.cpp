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

#include "settings_config.h"
#include "settings_window.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cctype>
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
static constexpr UINT  WM_TRAYICON   = WM_APP + 1;
static constexpr UINT  WM_SETTINGS_RELOAD = WM_APP + 2;

enum Cmd : UINT {
    CMD_SAVE_REPLAY     = 1001,
    CMD_RECORDING_START = 1002,
    CMD_RECORDING_STOP  = 1003,
    CMD_PAUSE_RESUME    = 1004,
    CMD_SETTINGS        = 1005,
    CMD_EXIT            = 1006,
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
static std::wstring g_recordings_dir;
static std::string g_recording_container = "mkv";
static settings::Config g_settings;

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
    g_replay.configure(rbcfg);

    g_recordings_dir = g_settings.recordings_directory;
    g_recording_container = g_settings.recording_container;

    char msg[128];
    snprintf(msg, sizeof(msg), "settings applied: replay=%ds / %lldMB recording=%s",
             g_settings.replay_duration_seconds,
             static_cast<long long>(g_settings.replay_memory_budget_mb),
             g_recording_container.c_str());
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

static void reload_settings_from_disk(HWND hwnd)
{
    settings::Config previous = g_settings;
    load_app_settings();
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

    // Capture/encoder settings are read once at media_start; flag the
    // difference instead of pretending they were applied live.
    if (previous.monitor_device       != g_settings.monitor_device ||
        previous.resolution_mode      != g_settings.resolution_mode ||
        previous.output_width         != g_settings.output_width ||
        previous.output_height        != g_settings.output_height ||
        previous.show_capture_border  != g_settings.show_capture_border ||
        previous.encoder_backend      != g_settings.encoder_backend ||
        previous.video_bitrate_kbps   != g_settings.video_bitrate_kbps ||
        previous.extra_ffmpeg_options != g_settings.extra_ffmpeg_options ||
        previous.audio_mode           != g_settings.audio_mode ||
        previous.primary_microphone_device_id != g_settings.primary_microphone_device_id ||
        previous.audio_sources.size() != g_settings.audio_sources.size()) {
        log_msg("settings", "capture/encoder/audio changes saved: restart Monolith to apply");
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
    auto active = audio::active_foreground_process();

    std::lock_guard<std::mutex> lk(g_status_mutex);
    g_runtime_status.input_devices.clear();
    for (const auto& dev : devices)
        g_runtime_status.input_devices.push_back(runtime_audio_device(dev));

    g_runtime_status.audio_sessions.clear();
    for (const auto& session : sessions)
        g_runtime_status.audio_sessions.push_back(runtime_audio_session(session));

    g_runtime_status.active_game = runtime_audio_session(active);
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
        auto replay_pkt = pkt;
        g_replay.push(std::move(replay_pkt));
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
            auto active = audio::active_foreground_process();
            if (!start_process_source(active.process_id, tracks, "audio.game",
                                      "active game audio started")) {
                log_msg("audio.game", "no active game/process captured");
            }
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

// ── Media start / stop ────────────────────────────────────────────────────────

static void media_start(HWND hwnd)
{
    (void)hwnd;

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

    // ── Replay buffer config ───────────────────────────────────────────────────
    {
        apply_runtime_settings();
    }

    // ── Audio capture/routing ──────────────────────────────────────────────────
    start_audio_system();

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
            // Encode every 2nd frame → 30fps effective for software encoder.
            if (f.seq % 2 != 0 || f.bgra_data == nullptr) return;
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
                vcfg.fps               = 30;
                vcfg.bitrate           = static_cast<int64_t>(g_settings.video_bitrate_kbps) * 1000;
                vcfg.preferred_encoder = g_settings.encoder_backend;
                vcfg.extra_options     = g_settings.extra_ffmpeg_options;

                bool enc_ok = g_video_enc.open(vcfg, [](encoding::EncodedPacket pkt) {
                    auto replay_pkt = pkt;
                    g_replay.push(std::move(replay_pkt));
                    g_recording.push(std::move(pkt));
                });

                if (enc_ok) {
                    char enc_msg[256];
                    snprintf(enc_msg, sizeof(enc_msg),
                        "%s %dx%d @30fps bitrate=%dkbps%s%s",
                        g_video_enc.encoder_name().c_str(), g_enc_w, g_enc_h,
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
            g_video_enc.push_bgra(
                f.bgra_data,
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

static void tray_add(HWND hwnd)
{
    g_nid                  = {};
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
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
    UINT start_flags = (state == recording::RecordingState::Idle) ? MF_STRING : (MF_STRING | MF_GRAYED);
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
    AppendMenuW(menu, MF_STRING,             CMD_SAVE_REPLAY,     save_text.c_str());
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, start_flags,            CMD_RECORDING_START, start_text.c_str());
    AppendMenuW(menu, stop_flags,             CMD_RECORDING_STOP,  stop_text.c_str());
    AppendMenuW(menu, pause_flags,            CMD_PAUSE_RESUME,    pause_text.c_str());
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, MF_STRING,             CMD_SETTINGS,        L"Settings\x2026");
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
        char stats[256];
        snprintf(stats, sizeof(stats),
            "save_replay — buffer: %zu pkt / %.1f MB",
            g_replay.packet_count(),
            static_cast<double>(g_replay.memory_bytes()) / 1048576.0);
        log_msg("replay", stats);
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
        if (g_recording.start(g_recordings_dir, g_recording_container)) {
            log_path("recording", "recording started: ", g_recording.current_path());
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
        publish_runtime_status();
        log_msg("app", "app shell ready");
        return 0;

    case WM_DESTROY:
        settings_window::close_running();
        media_stop();
        hotkeys_unregister(hwnd);
        tray_remove();
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
