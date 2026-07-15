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
#include <shellscalingapi.h>
#include <strsafe.h>
#include <share.h>

#include <winrt/base.h>

#include <unordered_map>

#include <capture/capture.h>
#include <audio/audio.h>
#include <gamelist/gamelist.h>
#include <encoding/encoding.h>
#include <storage/storage.h>
#include <replay-buffer/replay_buffer.h>
#include <recording/recording.h>
#include <platform-win/platform_win.h>
#include <logging/logging.h>

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
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr WCHAR kWindowClass[] = L"MonolithMsgWnd";
static constexpr WCHAR kAppName[]     = L"Monolith";
static constexpr WCHAR kMutexName[]   = L"Monolith_SingleInstance";
static constexpr UINT  WM_TRAYICON        = WM_APP + 1;
static constexpr UINT  WM_SETTINGS_RELOAD = WM_APP + 2;
static constexpr UINT  WM_FAST_SCAN       = WM_APP + 3; // posted by the WinEvent fg hook
static constexpr UINT  WM_REFRESH_AUDIO_SOURCES = WM_APP + 4; // posted by Settings UI on dropdown open

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
// Implementation lives in libs/logging (disabled by default; toggled via
// Settings > Advanced > advanced.logging_enabled). These are thin wrappers so
// call sites across this file don't need to change.

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

static void log_init(bool enabled)
{
    logging::init(enabled, app_data_dir());
}

static void log_msg(const char* tag, const char* msg)
{
    logging::log(tag, msg);
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

// Always-on error variants (bypass advanced.logging_enabled) — for failures the
// user must be able to diagnose even without opting into verbose logging.
static void log_error(const char* tag, const char* msg)
{
    logging::log_error(tag, msg);
}

static void log_error_path(const char* tag, const char* prefix, const std::wstring& path)
{
    char narrow[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
        narrow, sizeof(narrow), nullptr, nullptr);
    char msg[MAX_PATH * 2 + 64];
    snprintf(msg, sizeof(msg), "%s%s", prefix, narrow);
    log_error(tag, msg);
}

using platform_win::utf8_to_wide;
using platform_win::wide_to_utf8;

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

    // Resolve the dwmapi/uxtheme entry points once and cache them — this runs on
    // every tray right-click and theme change, so repeated LoadLibrary/FreeLibrary
    // would be pure waste.
    using DwmSetWindowAttributeFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    using SetPreferredAppModeFn   = int (WINAPI*)(int);
    using FlushMenuThemesFn       = void (WINAPI*)();
    using SetWindowThemeFn        = HRESULT (WINAPI*)(HWND, LPCWSTR, LPCWSTR);

    static const auto fns = [] {
        struct {
            DwmSetWindowAttributeFn set_attr        = nullptr;
            SetPreferredAppModeFn   set_app_mode    = nullptr;
            FlushMenuThemesFn       flush_menu      = nullptr;
            SetWindowThemeFn        set_theme       = nullptr;
        } f;
        // Leaked intentionally for process lifetime — these modules stay loaded
        // anyway and the handles never need releasing.
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
            f.set_attr = reinterpret_cast<DwmSetWindowAttributeFn>(
                GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (HMODULE ux = LoadLibraryW(L"uxtheme.dll")) {
            f.set_app_mode = reinterpret_cast<SetPreferredAppModeFn>(
                GetProcAddress(ux, MAKEINTRESOURCEA(135)));
            f.flush_menu = reinterpret_cast<FlushMenuThemesFn>(
                GetProcAddress(ux, MAKEINTRESOURCEA(136)));
            f.set_theme = reinterpret_cast<SetWindowThemeFn>(
                GetProcAddress(ux, "SetWindowTheme"));
        }
        return f;
    }();

    if (fns.set_attr) {
        DWORD attr = 20;
        if (FAILED(fns.set_attr(hwnd, attr, &dark, sizeof(dark)))) {
            attr = 19;
            fns.set_attr(hwnd, attr, &dark, sizeof(dark));
        }
    }

    if (fns.set_app_mode) {
        fns.set_app_mode(dark ? 2 : 3); // ForceDark / ForceLight.
        if (fns.flush_menu)
            fns.flush_menu();
    }
    if (fns.set_theme)
        fns.set_theme(hwnd, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

static capture::DisplayCapture      g_video;
static encoding::VideoEncoder       g_video_enc;
static std::array<encoding::AudioEncoder, 6> g_audio_encoders;

// Per-track mixers (index 1..6; index 0 unused). A track gets a mixer only when
// two or more enabled sources route to it; single-source tracks push straight to
// their encoder. The mixer's sink forwards summed PCM into that track's encoder.
static std::array<std::unique_ptr<encoding::TrackMixer>, 7> g_track_mixers;

// One output route for a source: which track, and (when that track is mixed) the
// source's id within the mixer. mixer_src == -1 means push directly to encoder.
// gain is the source's linear volume (0.0–1.0), applied to the recorded audio:
// for mixed tracks it lives in the mixer source; for direct tracks it is applied
// in-place before push_pcm.
struct AudioRoute { int track = 0; int mixer_src = -1; float gain = 1.0f; };

// A live capture plus the routes its callback writes to. Routes are kept so that
// when a capture is removed (e.g. Active Game swap) its mixer slots are released.
struct ActiveCapture {
    std::unique_ptr<audio::WasapiCapture> capture;
    std::vector<AudioRoute> routes;
};
static std::vector<ActiveCapture> g_audio_captures;
static replay_buffer::ReplayBuffer  g_replay;
static recording::ManualRecorder    g_recording;

static int g_enc_w = 0; // configured encoder width (even-aligned)
static int g_enc_h = 0; // configured encoder height (even-aligned)
static std::atomic<bool> g_video_enc_open_attempted{ false };
static std::atomic<uint64_t> g_perf_pacer_frames_submitted{ 0 };
static std::atomic<uint64_t> g_perf_encoder_frames_submitted{ 0 };
static std::atomic<uint64_t> g_perf_encoder_push_time_us_total{ 0 };
static std::atomic<uint64_t> g_perf_encoder_packets_output{ 0 };
static std::atomic<uint64_t> g_perf_bgra_memcpy_frames{ 0 };
static std::atomic<uint64_t> g_perf_bgra_memcpy_time_us_total{ 0 };
// Backoff after a failed encoder open: don't re-probe on every captured frame.
static std::atomic<uint64_t> g_video_enc_retry_after_ms{ 0 };
static constexpr uint64_t kVideoEncRetryBackoffMs = 2000;
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
static int64_t              g_pacer_qpc_freq     = 0;  // QueryPerformanceFrequency
static int64_t              g_pacer_start_qpc    = 0;  // QPC at pacer start
static int64_t              g_pacer_frames_done  = 0;  // CFR frames emitted so far
static int                  g_pacer_fps          = 0;
static std::atomic<bool>    g_pacer_running{ false };
static bool                 g_pacer_time_period_set = false;
static std::wstring g_recordings_dir;
static std::string g_recording_container = "mkv";
static std::atomic<bool> g_replay_enabled{ true };
static std::atomic<bool> g_recording_enabled{ true };

// Runtime mutex between the replay buffer and manual recording. Unless the user
// opts into concurrent capture, a recording temporarily suspends the replay
// buffer (two encoders would otherwise encode every frame twice). This flag is
// AND-ed with the user's g_replay_enabled setting at every replay gate, so the
// user's own on/off preference is never overwritten.
static std::atomic<bool> g_replay_suspended_for_recording{ false };

static inline bool replay_active()
{
    return g_replay_enabled.load(std::memory_order_relaxed) &&
           !g_replay_suspended_for_recording.load(std::memory_order_relaxed);
}

// Snapshot of the two output folders, guarded so the IPC thread can resolve the
// right clip DB for UI-driven mutations without racing settings reloads on the
// UI thread. Updated in apply_runtime_settings().
static std::mutex    g_dirs_mutex;
static std::wstring  g_clips_dir_snapshot;
static std::wstring  g_recs_dir_snapshot;
static settings::Config g_settings;

// Bumped each time a clip is cataloged (replay save or manual stop). Read by the
// IPC get_status handler so the UI host can push a live clip-list refresh.
static std::atomic<uint64_t>     g_clip_generation{0};

// Active-game audio source: re-evaluated every poll_interval_ms by a UI-thread timer
// and also on foreground-window changes (fast scan, rate-limited).
static constexpr UINT_PTR kActiveGameTimerId = 1;

// Replay-buffer memory budget, fixed internally (no UI knob). 512 MB is a safe
// ceiling for the encoded-packet ring across supported bitrates/durations.
static constexpr int64_t kReplayMemoryBudgetMb = 512;

// Active-game detection runs on a fixed 3 s cadence. Detection is DB-gated: a
// process only counts as a game when its executable is in the locally-cached
// Discord detectable list (see libs/gamelist). Window facts (foreground/
// fullscreen) only order candidates and pick the capture target, never gate.
static constexpr int kActiveGamePollMs = 3000;

static std::vector<int>          g_active_game_tracks;
static float                     g_active_game_gain    = 1.0f; // volume of the active-game source
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

// Re-writes AppData\Local\Monolith\runtime-status.json from g_runtime_status,
// but only when the serialized content actually changed — the active-game poll
// runs every few seconds and on every foreground change, and most ticks produce
// identical status, so skipping no-op disk writes avoids needless I/O.
static std::string g_last_runtime_status_json;
static std::mutex  g_last_runtime_status_mutex;

static void publish_runtime_status()
{
    settings::RuntimeStatus snapshot;
    {
        std::lock_guard<std::mutex> lk(g_status_mutex);
        snapshot = g_runtime_status;
    }
    std::string error;
    std::string json = settings::serialize_runtime_status(snapshot);
    {
        std::lock_guard<std::mutex> lk(g_last_runtime_status_mutex);
        if (json == g_last_runtime_status_json) return; // unchanged: skip disk write
        g_last_runtime_status_json = json;
    }
    if (!settings::write_runtime_status(app_data_dir(), snapshot, &error))
        log_msg("settings", ("runtime-status write failed: " + error).c_str());
}

// ── OBS-style video pacer ──────────────────────────────────────────────
// Fixed-rate video thread feeds encoder exactly like OBS video_output.
// Producer (WGC callback): copies BGRA to g_pacer_shared under mutex.
// Consumer (pacer thread): reads latest frame each tick, pushes to encoder.

static uint64_t qpc_elapsed_us(const LARGE_INTEGER& start,
                               const LARGE_INTEGER& end);

static void pacer_push_frame(const uint8_t* bgra, int stride, int width, int height)
{
    LARGE_INTEGER copy_start{}, copy_end{};
    QueryPerformanceCounter(&copy_start);
    std::lock_guard<std::mutex> lock(g_pacer_mutex);
    size_t size = static_cast<size_t>(height) * static_cast<size_t>(stride);
    g_pacer_shared.bgra.resize(size);
    memcpy(g_pacer_shared.bgra.data(), bgra, size);
    QueryPerformanceCounter(&copy_end);
    g_perf_bgra_memcpy_frames.fetch_add(1, std::memory_order_relaxed);
    g_perf_bgra_memcpy_time_us_total.fetch_add(
        qpc_elapsed_us(copy_start, copy_end), std::memory_order_relaxed);
    g_pacer_shared.stride   = stride;
    g_pacer_shared.width    = width;
    g_pacer_shared.height   = height;
    g_pacer_shared.frame_id++;
}

static uint64_t qpc_elapsed_us(const LARGE_INTEGER& start,
                               const LARGE_INTEGER& end)
{
    return g_pacer_qpc_freq > 0
        ? static_cast<uint64_t>((end.QuadPart - start.QuadPart) * 1000000ll / g_pacer_qpc_freq)
        : 0;
}

static void pacer_release_buffers()
{
    std::lock_guard<std::mutex> lock(g_pacer_mutex);
    std::vector<uint8_t>().swap(g_pacer_shared.bgra);
    g_pacer_shared.stride = 0;
    g_pacer_shared.width = 0;
    g_pacer_shared.height = 0;
    g_pacer_shared.frame_id = 0;
}

// Capture target window for game_only mode; read by the pacer thread for the
// frozen-frame (minimized) handling. nullptr = full-screen/monitor capture.
// Declared here (before the pacer) since the pacer thread reads it.
static std::atomic<HWND> g_capture_target_hwnd{ nullptr };

static DWORD WINAPI pacer_thread_proc(LPVOID)
{
    std::vector<uint8_t> local_bgra;
    int local_stride = 0, local_width = 0, local_height = 0;
    uint64_t local_last_id = 0;
    bool frozen = false; // edge-detect for the minimized-window log

    // High-resolution waitable timer (Win10 1803+).  Falls back to the stop
    // event's timeout wait if unavailable.
    HANDLE htimer = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    // Tick at ~2x frame rate so the CFR loop has fine-grained wakeups to land
    // each frame slot near its real deadline (dup/skip absorbs the rest).
    int tick_ms = (g_pacer_fps > 0) ? (1000 / (g_pacer_fps * 2)) : 4;
    if (tick_ms < 1) tick_ms = 1;

    while (true) {
        // Wait for the next tick, or stop signal.
        if (htimer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(tick_ms) * 10000; // 100ns units, relative
            SetWaitableTimer(htimer, &due, 0, nullptr, nullptr, FALSE);
            HANDLE handles[2] = { g_pacer_stop_event, htimer };
            DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) break;          // stop event
        } else {
            DWORD w = WaitForSingleObject(g_pacer_stop_event,
                                          static_cast<DWORD>(tick_ms));
            if (w == WAIT_OBJECT_0) break;
        }

        // Frozen-frame: when the captured game window is minimized, WGC delivers
        // no (or degenerate) frames. Deliberately hold the last good frame — the
        // CFR loop keeps duplicating local_bgra — instead of swapping in whatever
        // WGC produces while iconic. Applies to both recording and replay since
        // both consume this pacer -> encoder path.
        HWND cap_target = g_capture_target_hwnd.load(std::memory_order_relaxed);
        const bool minimized = cap_target && IsIconic(cap_target);
        if (minimized != frozen) {
            frozen = minimized;
            log_msg("capture", minimized ? "captured game minimized: holding last frame"
                                         : "captured game restored");
        }

        // Pull the latest captured frame (if a new one arrived).  Swap buffers
        // under the lock instead of copying: the consumer takes ownership of the
        // shared frame's storage and hands back its own (reused next push), so
        // there is no per-frame full-frame memcpy on the consumer side.
        if (!minimized) {
            std::lock_guard<std::mutex> lock(g_pacer_mutex);
            if (g_pacer_shared.frame_id != local_last_id) {
                std::swap(local_bgra, g_pacer_shared.bgra);
                local_stride  = g_pacer_shared.stride;
                local_width   = g_pacer_shared.width;
                local_height  = g_pacer_shared.height;
                local_last_id = g_pacer_shared.frame_id;
            }
        }

        if (!g_video_enc.is_open() || local_bgra.empty())
            continue;

        // ── Clock-locked CFR: emit frames until we've caught up to wall-clock.
        // target = number of frames that SHOULD exist by now.  Duplicate the
        // latest frame to fill gaps; this makes playback speed independent of
        // timer jitter, encode cost, and configured fps.
        LARGE_INTEGER now_qpc;
        QueryPerformanceCounter(&now_qpc);
        int64_t target = llround(
            static_cast<double>(now_qpc.QuadPart - g_pacer_start_qpc)
            * g_pacer_fps / static_cast<double>(g_pacer_qpc_freq));

        // Avoid catch-up bursts after stalls. For a lightweight clipping app it
        // is better to drop late duplicate frames than to flood CPU/encoder work.
        if (target - g_pacer_frames_done > 1)
            g_pacer_frames_done = target - 1;

        if (g_pacer_frames_done < target) {
            LARGE_INTEGER push_start{}, push_end{};
            QueryPerformanceCounter(&push_start);
            g_video_enc.push_bgra(local_bgra.data(), local_stride,
                                  local_width, local_height,
                                  g_pacer_frames_done);
            QueryPerformanceCounter(&push_end);
            g_perf_encoder_push_time_us_total.fetch_add(
                qpc_elapsed_us(push_start, push_end), std::memory_order_relaxed);
            g_perf_pacer_frames_submitted.fetch_add(1, std::memory_order_relaxed);
            g_perf_encoder_frames_submitted.fetch_add(1, std::memory_order_relaxed);
            g_pacer_frames_done++;
        }
    }

    std::vector<uint8_t>().swap(local_bgra);
    if (htimer) CloseHandle(htimer);
    g_pacer_running = false;
    return 0;
}

static void pacer_start()
{
    if (g_settings.video_fps <= 0) return;
    g_pacer_fps          = g_settings.video_fps;

    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    g_pacer_qpc_freq    = freq.QuadPart;
    g_pacer_start_qpc   = start.QuadPart;
    g_pacer_frames_done = 0;

    g_pacer_time_period_set = false;
    if (g_pacer_fps > 60) {
        timeBeginPeriod(1); // high-FPS mode: tighten system timer resolution.
        g_pacer_time_period_set = true;
    }

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
        if (g_pacer_time_period_set) {
            timeEndPeriod(1);
            g_pacer_time_period_set = false;
        }
    }
    pacer_release_buffers();
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
    // Memory budget is fixed internally (no longer user-configurable).
    rbcfg.memory_cap_mb = kReplayMemoryBudgetMb;
    rbcfg.output_dir    = g_settings.clips_directory;
    rbcfg.container     = g_settings.replay_clip_container;
    g_replay.configure(rbcfg);

    g_recordings_dir = g_settings.recordings_directory;
    g_recording_container = g_settings.recording_container;
    g_replay_enabled.store(g_settings.replay_buffer_enabled, std::memory_order_relaxed);
    g_recording_enabled.store(g_settings.recording_enabled, std::memory_order_relaxed);

    {
        std::lock_guard lk(g_dirs_mutex);
        g_clips_dir_snapshot = g_settings.clips_directory;
        g_recs_dir_snapshot  = g_settings.recordings_directory;
    }

    char msg[224];
    snprintf(msg, sizeof(msg),
             "settings applied: replay=%ds / %lldMB / %s%s recording=%s%s "
             "encoder=%s/%s fps=%d bitrate=%dkbps (CBR)",
             g_settings.replay_duration_seconds,
             static_cast<long long>(kReplayMemoryBudgetMb),
             g_settings.replay_clip_container.c_str(),
             g_settings.replay_buffer_enabled ? "" : " (DISABLED)",
             g_recording_container.c_str(),
             g_settings.recording_enabled ? "" : " (DISABLED)",
             g_settings.encoder_device.c_str(),
             g_settings.encoder_codec.c_str(),
             g_settings.video_fps,
             g_settings.video_bitrate_kbps);
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

// Generates the first-frame thumbnail and inserts a row into the co-located
// clip DB for a freshly written clip. Runs off the UI/tray thread (blocking
// decode + DB I/O). `video_path` is the full path returned by the writer;
// `source` is "replay" or "manual". Failures are logged, never fatal — a clip
// with no DB row/thumb still plays and gets picked up by reconcile later.
static void catalog_clip(std::wstring video_path, std::string source,
                         double duration_seconds)
{
    if (video_path.empty()) return;
    std::error_code ec;
    std::filesystem::path vp(video_path);
    const std::wstring folder = vp.parent_path().wstring();
    const std::wstring base   = vp.filename().wstring();
    if (folder.empty() || base.empty()) return;

    const std::wstring thumb_base = vp.stem().wstring() + L".png";
    const std::wstring thumb_path = folder + L"\\.thumbs\\" + thumb_base;

    std::wstring thumb_stored;
    if (encoding::generate_thumbnail(video_path, thumb_path))
        thumb_stored = thumb_base;
    else
        // Always surfaced (not gated by verbose logging): a missing thumbnail is
        // the visible symptom, so the cause must be diagnosable by default.
        log_error_path("storage", "thumbnail generation failed for ", video_path);

    std::string error;
    auto dbh = storage::ClipDb::open(folder, source, &error);
    if (!dbh) {
        log_msg("storage", ("clip DB open failed: " + error).c_str());
        return;
    }

    const double probed_duration = encoding::probe_duration_seconds(video_path);

    storage::ClipRow row;
    row.video_file       = base;
    row.thumbnail_file   = thumb_stored;
    row.created_at_utc   = storage::now_iso8601_utc();
    row.source           = source;
    row.duration_seconds = probed_duration > 0.0 ? probed_duration : duration_seconds;

    // Best-effort game association: DB-gated detection at save time
    // (self-contained, off the UI thread). Empty when no known game is running.
    audio::ProcessInfo game = audio::detect_active_game();
    if (game.process_id != 0) {
        row.game_process_name = wide_to_utf8(game.process_name);
        row.game_display_name = wide_to_utf8(
            game.display_name.empty() ? game.process_name : game.display_name);
        row.game_executable_path = wide_to_utf8(game.executable_path);
        row.game_source = "gamelist";
    }

    if (dbh->insert_clip(row, &error) <= 0)
        log_msg("storage", ("clip DB insert failed: " + error).c_str());
    else {
        g_clip_generation.fetch_add(1, std::memory_order_relaxed);
        log_path("storage", "clip cataloged: ", base);
    }
}

// Self-heal both catalogs on startup: drop rows whose video vanished, rebuild
// missing thumbnails, and import pre-existing videos that have no row yet.
// Detached background thread — never blocks startup.
static void reconcile_catalogs()
{
    const std::wstring clips_dir = g_settings.clips_directory;
    const std::wstring recs_dir  = g_settings.recordings_directory;
    std::thread([clips_dir, recs_dir]() {
        auto run = [](const std::wstring& folder, const char* source) {
            if (folder.empty()) return;
            std::string error;
            auto dbh = storage::ClipDb::open(folder, source, &error);
            if (!dbh) {
                log_msg("storage", ("reconcile skipped (" + std::string(source)
                                    + "): " + error).c_str());
                return;
            }
            storage::ReconcileStats st = dbh->reconcile(&error);
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "reconcile %s: removed=%d thumbs=%d imported=%d",
                     source, st.removed, st.thumbs_regenerated, st.imported);
            log_msg("storage", msg);
        };
        run(clips_dir, "replay");
        run(recs_dir,  "manual");
    }).detach();
}

// Performs a UI-driven clip mutation (favorite/hashtag/delete) on the IPC
// thread. The recorder is the single writer, so the UI process routes these
// here instead of writing the DB itself. Returns "" on success or an error message.
static std::string handle_clip_mutation(const ipc::ClipMutation& m)
{
    std::wstring folder;
    {
        std::lock_guard lk(g_dirs_mutex);
        folder = (m.source == "manual") ? g_recs_dir_snapshot : g_clips_dir_snapshot;
    }
    if (folder.empty()) return "output folder unknown";

    std::string err;
    auto db = storage::ClipDb::open(folder, m.source, &err);
    if (!db) return "clip DB open failed: " + err;

    bool ok = false;
    if (m.method == "clip_set_favorite")       ok = db->set_favorite(m.id, m.favorite, &err);
    else if (m.method == "clip_add_hashtag")    ok = db->add_hashtag(m.id, m.tag, &err);
    else if (m.method == "clip_remove_hashtag") ok = db->remove_hashtag(m.id, m.tag, &err);
    else if (m.method == "clip_rename")         ok = db->rename_clip(m.id, utf8_to_wide(m.new_name), &err);
    else if (m.method == "clip_set_title")      ok = db->set_title(m.id, m.title, &err);
    else if (m.method == "clip_regen_thumb")    ok = db->regenerate_thumbnail(m.id, &err);
    else if (m.method == "clip_delete")         ok = db->remove_clip(m.id, /*remove_files=*/true, &err);
    else return "unknown mutation: " + m.method;

    if (!ok) {
        // Always-on so a failed thumbnail regen / mutation is diagnosable.
        log_error("storage", ("clip mutation '" + m.method + "' failed: " + err).c_str());
    } else if (m.method == "clip_regen_thumb") {
        // A regenerated thumbnail changes displayed data but doesn't insert a
        // row; bump the generation so the UI clip-watch reloads and picks up the
        // now-populated thumbnail_file.
        g_clip_generation.fetch_add(1, std::memory_order_relaxed);
    }

    return ok ? std::string() : err;
}

// media_start/media_stop are defined far below; forward-declare them here since
// evaluate_capture_mode (game_only path) calls them before their definition.
static void media_start(HWND hwnd);
static void media_stop();
static void tray_set_recording(bool recording);

// ── Detection / multi-game / auto-record shared state (UI thread only) ─────────
//
// poll_active_game() computes these each detection pass; evaluate_capture_mode()
// consumes them for the capture pipeline + auto-record state machine. Keeping the
// two split means the inline poll_active_game() calls inside media_start() don't
// trigger capture churn.
static uint64_t g_last_game_seen_ms  = 0;
static bool     g_capture_mode_stopped = false; // capture idled off by game_only
static bool     g_auto_recording_active = false;

// The DB-matched games running right now, and which one is the effective target
// (user selection, else most-recently-focused). Written by poll_active_game().
static std::vector<uint32_t>                  g_candidate_pids;
static std::unordered_map<uint32_t, uint64_t> g_focus_ms;        // pid -> last foreground tick
static uint32_t                               g_effective_game_pid = 0;
static uint32_t                               g_recording_game_pid = 0; // pid of the auto recording

// User's explicit game selection (from the topbar via IPC). Shared with the IPC
// thread. Empty exe = auto (most-recently-focused wins).
static std::mutex  g_selected_game_mutex;
static std::string g_selected_game_exe;   // lowercased exe basename

// Auto-record engine-startup grace: when games are already running as the engine
// starts, wait for the user to foreground one (up to 60s) before auto-recording.
enum class AutoState { WaitingForStartupFocus, Idle, AutoRecording };
static AutoState g_auto_state = AutoState::WaitingForStartupFocus;
static uint64_t  g_engine_start_ms = 0;
static constexpr uint64_t kStartupFocusGraceMs = 60000;

// Lowercased UTF-8 exe basename, used as the stable selection/lookup key.
static std::string exe_key_of(const std::wstring& process_name)
{
    std::string s = wide_to_utf8(process_name);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Starts the auto recording for the current effective target, if allowed. Returns
// true when a recording was started.
static bool auto_record_start(HWND hwnd)
{
    if (!g_settings.capture_auto_record) return false;
    if (!g_recording_enabled.load(std::memory_order_relaxed)) return false;
    if (g_recording.state() != recording::RecordingState::Idle) return false;
    if (!g_video.running()) media_start(hwnd);
    if (!g_recording.start(g_recordings_dir, g_recording_container)) return false;
    suspend_replay_for_recording();
    g_auto_recording_active = true;
    g_auto_state            = AutoState::AutoRecording;
    g_recording_game_pid    = g_effective_game_pid;
    log_path("recording", "auto recording started: ", g_recording.current_path());
    feedback::play(feedback::Sound::RecordStart);
    tray_set_recording(true);
    return true;
}

// Stops + catalogs the current auto recording (never a user manual recording).
static void auto_record_stop()
{
    if (!g_auto_recording_active ||
        g_recording.state() == recording::RecordingState::Idle)
        return;
    std::wstring path;
    if (g_recording.stop(&path)) {
        if (path.empty()) log_msg("recording", "auto recording stopped: no packets written");
        else {
            log_path("recording", "auto recording saved: ", path);
            std::thread(catalog_clip, path, std::string("manual"), 0.0).detach();
        }
        feedback::play(feedback::Sound::RecordStop);
        tray_set_recording(false);
    }
    restore_replay_after_recording();
    g_auto_recording_active = false;
    g_recording_game_pid    = 0;
}

// ── Replay/recording mutual exclusion ───────────────────────────────────────
// Unless the user opted into concurrent capture, a manual/auto recording
// temporarily suspends the replay buffer so only one encoder runs. Suspending
// clears the ring (its retained history is meaningless while paused) and the
// gate stops feeding it; restoring re-arms the gate for future frames.

static void suspend_replay_for_recording()
{
    if (g_settings.allow_concurrent_capture) return;
    if (g_replay_suspended_for_recording.exchange(true)) return;
    g_replay.clear();
    log_msg("replay", "replay buffer suspended while recording (concurrent capture off)");
}

static void restore_replay_after_recording()
{
    if (!g_replay_suspended_for_recording.exchange(false)) return;
    if (g_replay_enabled.load(std::memory_order_relaxed))
        log_msg("replay", "replay buffer resumed after recording");
}

// Runs the game_only capture pipeline + auto-record state machine from the shared
// detection state computed by the preceding poll_active_game(). UI thread only.
static void evaluate_capture_mode(HWND hwnd)
{
    if (g_settings.capture_mode != "game_only") {
        // Screen mode: full-screen capture always runs; detection still happened
        // (naming/icon/candidates), but capture/auto-record here is a no-op.
        g_capture_mode_stopped  = false;
        return;
    }

    const uint64_t now = GetTickCount64();
    if (g_engine_start_ms == 0) g_engine_start_ms = now;
    if (g_last_game_seen_ms == 0) g_last_game_seen_ms = now; // grace on first tick

    const bool has_game = g_effective_game_pid != 0;
    const bool clip_without_game = g_settings.capture_clip_without_game;

    // ── A game is available ──────────────────────────────────────────────────
    if (has_game) {
        g_last_game_seen_ms = now;

        // Resume the replay pipeline if it was idled off while no game ran.
        if (g_replay_enabled.load(std::memory_order_relaxed) &&
            g_capture_mode_stopped && !g_video.running()) {
            log_msg("capture", "game_only: game detected, resuming capture");
            media_start(hwnd);
            g_capture_mode_stopped = false;
        }

        // Auto-next-on-close: the recorded game closed while others remain — the
        // effective target already moved on; stop the old recording and start the
        // new target.
        if (g_auto_state == AutoState::AutoRecording &&
            g_recording_game_pid != 0 &&
            g_recording_game_pid != g_effective_game_pid) {
            log_msg("recording", "recorded game changed; switching auto recording");
            auto_record_stop();
        }

        // Startup grace: don't auto-start until the user foregrounds a running
        // game (or 60s elapses).
        if (g_auto_state == AutoState::WaitingForStartupFocus) {
            bool focused_a_game = false;
            for (uint32_t pid : g_candidate_pids) {
                if (pid != g_effective_game_pid) continue;
                auto it = g_focus_ms.find(pid);
                if (it != g_focus_ms.end() && it->second >= g_engine_start_ms)
                    focused_a_game = true;
                break;
            }
            if (focused_a_game || (now - g_engine_start_ms) >= kStartupFocusGraceMs)
                g_auto_state = AutoState::Idle; // grace resolved; normal rules apply
        }

        if (g_auto_state != AutoState::WaitingForStartupFocus &&
            g_recording.state() == recording::RecordingState::Idle) {
            auto_record_start(hwnd);
        }
        return;
    }

    // ── No game detected ─────────────────────────────────────────────────────
    // Stop an auto recording (never a user-started manual recording).
    if (g_auto_recording_active) {
        auto_record_stop();
    } else if (g_recording.state() != recording::RecordingState::Idle) {
        return; // never stop a user-started manual recording
    }
    g_auto_state = AutoState::Idle;

    // Clipping-without-game ON: keep the replay buffer alive on full-screen
    // capture (capture-options falls back to the monitor when no target window).
    if (clip_without_game) {
        if (g_replay_enabled.load(std::memory_order_relaxed) && !g_video.running()) {
            log_msg("capture", "no game: clip-without-game on, capturing full screen");
            media_start(hwnd);
        }
        g_capture_mode_stopped = false;
        return;
    }

    // Clipping-without-game OFF (default): disable the replay buffer once the idle
    // timeout elapses with no game.
    const uint64_t idle_ms =
        static_cast<uint64_t>(g_settings.capture_idle_timeout_seconds) * 1000ull;
    if (g_replay_enabled.load(std::memory_order_relaxed) &&
        g_video.running() && !g_capture_mode_stopped &&
        (now - g_last_game_seen_ms) >= idle_ms) {
        log_msg("capture", "game_only: no game past idle timeout, stopping capture");
        media_stop();
        g_replay.clear();
        g_capture_mode_stopped = true;
    }
}

static void show_settings(HWND hwnd)
{
    settings_window::show(hwnd, WM_SETTINGS_RELOAD);
}

static void hotkeys_register(HWND hwnd);
static void hotkeys_unregister(HWND hwnd);
static void stop_audio_system();
static void start_audio_system();
static void poll_active_game();
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
        && a.volume == b.volume
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
        || a.resolution_preset != b.resolution_preset
        || a.show_capture_border != b.show_capture_border
        || a.encoder_device != b.encoder_device
        || a.encoder_codec != b.encoder_codec
        || a.video_bitrate_kbps != b.video_bitrate_kbps
        || a.video_fps != b.video_fps
        || a.extra_ffmpeg_options != b.extra_ffmpeg_options;
}

static void reload_settings_from_disk(HWND hwnd)
{
    settings::Config previous = g_settings;
    load_app_settings();
    const bool capture_or_encoder_changed =
        capture_encoder_config_changed(previous, g_settings);
    const bool audio_changed = audio_config_changed(previous, g_settings);

    if (previous.logging_enabled != g_settings.logging_enabled)
        logging::set_enabled(g_settings.logging_enabled);

    apply_runtime_settings();
    log_msg("settings", "settings reloaded from the UI process");

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
        poll_active_game();
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
    out.window_title = proc.window_title;
    out.window_class = proc.window_class;
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
        if (replay_active()) {
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

// Number of enabled sources planned for each track (index 1..6). Used to decide
// whether a track can be pushed directly (single source) or needs a mixer
// (multiple sources on the same logical track). Recomputed before audio start.
static std::array<int, 7> g_track_plan_count{};

// Ensure a mixer exists for `track`, wired to feed that track's encoder.
static encoding::TrackMixer* ensure_track_mixer(int track)
{
    if (track < 1 || track > 6) return nullptr;
    if (g_track_mixers[track]) return g_track_mixers[track].get();

    auto mixer = std::make_unique<encoding::TrackMixer>();
    bool ok = mixer->open(48000, 2,
        [track](const uint8_t* data, int bytes, int sr, int ch, int bd, bool is_float) {
            g_audio_encoders[track - 1].push_pcm(data, bytes, sr, ch, bd, is_float);
        });
    if (!ok) return nullptr;
    g_track_mixers[track] = std::move(mixer);
    return g_track_mixers[track].get();
}

// Build output routes for a source given its requested tracks. A single source
// on a track pushes directly to the encoder. Only tracks with multiple sources
// pay the continuous mixer cost.
static std::vector<AudioRoute> make_routes(const std::vector<int>& requested,
                                           float gain = 1.0f)
{
    if (gain < 0.0f) gain = 0.0f;
    std::vector<AudioRoute> routes;
    for (int track : valid_tracks(requested)) {
        if (!open_audio_track(track)) continue;
        AudioRoute r;
        r.track = track;
        r.mixer_src = -1;
        r.gain = gain;
        if (g_track_plan_count[track] > 1) {
            encoding::TrackMixer* mixer = ensure_track_mixer(track);
            if (!mixer) continue;
            // Mixed track: gain lives in the mixer source so summing is correct.
            r.mixer_src = mixer->add_source(gain);
            if (r.mixer_src < 0) continue;
        }
        routes.push_back(r);
    }
    return routes;
}

// Release the mixer slots a capture held; close encoders only for direct routes.
static void release_routes(const std::vector<AudioRoute>& routes)
{
    for (const auto& r : routes) {
        if (r.mixer_src >= 0 && g_track_mixers[r.track])
            g_track_mixers[r.track]->remove_source(r.mixer_src);
    }
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

    // Diagnostics: which logical audio tracks end up as streams in the output.
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "published %zu audio track(s):", params.size());
    for (const auto& p : params) {
        if (n < 0 || n >= static_cast<int>(sizeof(buf))) break;
        n += snprintf(buf + n, sizeof(buf) - n, " %d", p.stream_index);
    }
    log_msg("audio", buf);
}

// Applies a linear gain to a raw PCM buffer in place. Handles the two formats
// WASAPI delivers: 32-bit IEEE float and 16-bit signed integer. Other bit depths
// are left untouched (gain simply doesn't apply). Clamps to the format range.
static void apply_gain_inplace(uint8_t* data, int bytes, int bit_depth,
                               bool is_float, float gain)
{
    if (!data || bytes <= 0 || gain == 1.0f) return;
    if (is_float && bit_depth == 32) {
        float* s = reinterpret_cast<float*>(data);
        const int n = bytes / static_cast<int>(sizeof(float));
        for (int i = 0; i < n; ++i) {
            float v = s[i] * gain;
            if (v >  1.0f) v =  1.0f;
            else if (v < -1.0f) v = -1.0f;
            s[i] = v;
        }
    } else if (!is_float && bit_depth == 16) {
        int16_t* s = reinterpret_cast<int16_t*>(data);
        const int n = bytes / static_cast<int>(sizeof(int16_t));
        for (int i = 0; i < n; ++i) {
            int v = static_cast<int>(std::lround(s[i] * gain));
            if (v >  32767) v =  32767;
            else if (v < -32768) v = -32768;
            s[i] = static_cast<int16_t>(v);
        }
    }
}

static void push_audio_to_routes(const std::vector<AudioRoute>& routes,
                                 const audio::PacketInfo& p,
                                 const char* log_tag)
{
    if (p.seq % 500 == 0) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "seq=%-6u  frames=%-4u  sr=%u  ch=%u routes=%zu%s",
            p.seq, p.frame_count, p.sample_rate, p.channels,
            routes.size(), p.silent ? "  [silent]" : "");
        log_msg(log_tag, buf);
    }

    if (p.data_bytes == 0) return;

    // Scratch buffer reused across direct-route gain application. thread_local so
    // concurrent capture callbacks (each on its own capture thread) don't clash.
    thread_local std::vector<uint8_t> gain_buf;

    for (const auto& r : routes) {
        if (r.track < 1 || r.track > 6) continue;
        if (r.mixer_src >= 0 && g_track_mixers[r.track]) {
            // Mixed track: the mixer source already carries the gain.
            g_track_mixers[r.track]->push(
                r.mixer_src, p.data, static_cast<int>(p.data_bytes),
                static_cast<int>(p.sample_rate), static_cast<int>(p.channels),
                static_cast<int>(p.bit_depth), p.is_float);
        } else if (r.mixer_src < 0) {
            const uint8_t* out_data = p.data;
            int out_bytes = static_cast<int>(p.data_bytes);
            // Direct track: apply per-source gain in place on a scratch copy.
            if (r.gain != 1.0f && p.data) {
                gain_buf.assign(p.data, p.data + p.data_bytes);
                apply_gain_inplace(gain_buf.data(), out_bytes,
                                   static_cast<int>(p.bit_depth), p.is_float, r.gain);
                out_data = gain_buf.data();
            }
            g_audio_encoders[r.track - 1].push_pcm(
                out_data, out_bytes,
                static_cast<int>(p.sample_rate), static_cast<int>(p.channels),
                static_cast<int>(p.bit_depth), p.is_float);
        }
    }
}

static bool start_endpoint_source(audio::WasapiCapture::Mode mode,
                                  const std::wstring& device_id,
                                  std::vector<int> tracks,
                                  const char* log_tag,
                                  const char* label,
                                  float gain = 1.0f)
{
    std::vector<AudioRoute> routes = make_routes(tracks, gain);
    if (routes.empty()) return false;

    auto capture = std::make_unique<audio::WasapiCapture>();
    bool ok = capture->start_device(mode, device_id,
        [routes, log_tag](audio::PacketInfo const& p) {
            push_audio_to_routes(routes, p, log_tag);
        });

    if (ok) {
        g_audio_captures.push_back({std::move(capture), routes});
        log_msg(log_tag, label);
        return true;
    }

    log_msg(log_tag, "WARNING: source failed to start");
    release_routes(routes);
    return false;
}

static bool start_process_source(uint32_t pid,
                                 std::vector<int> tracks,
                                 const char* log_tag,
                                 const char* label,
                                 float gain = 1.0f)
{
    if (pid == 0) return false;
    std::vector<AudioRoute> routes = make_routes(tracks, gain);
    if (routes.empty()) return false;

    auto capture = std::make_unique<audio::WasapiCapture>();
    bool ok = capture->start_process_loopback(pid,
        [routes, log_tag](audio::PacketInfo const& p) {
            push_audio_to_routes(routes, p, log_tag);
        });

    if (ok) {
        g_audio_captures.push_back({std::move(capture), routes});
        log_msg(log_tag, label);
        return true;
    }

    log_msg(log_tag, "WARNING: process loopback failed");
    release_routes(routes);
    return false;
}

struct WindowProcessMatch {
    const settings::AudioSourceConfig* source = nullptr;
    uint32_t process_id = 0;
};

static uint32_t resolve_configured_process_window(const settings::AudioSourceConfig& source)
{
    if (source.window_title.empty() || source.window_class.empty()) return 0;

    WindowProcessMatch match;
    match.source = &source;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto* match = reinterpret_cast<WindowProcessMatch*>(param);
        if (match->process_id != 0) return FALSE;
        if (!platform_win::is_capture_candidate_window(hwnd)) return TRUE;

        if (platform_win::window_text(hwnd) != match->source->window_title) return TRUE;
        if (platform_win::window_class_name(hwnd) != match->source->window_class) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

        if (!match->source->executable_path.empty()) {
            std::wstring path = platform_win::process_info(pid).executable_path;
            if (path.empty() ||
                _wcsicmp(path.c_str(), match->source->executable_path.c_str()) != 0)
                return TRUE;
        }

        match->process_id = pid;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&match));

    if (match.process_id != 0) {
        char buf[320];
        snprintf(buf, sizeof(buf), "resolved window '%s' -> pid %u",
                 wide_to_utf8(source.window_title).c_str(), match.process_id);
        log_msg("audio.app", buf);
    }
    return match.process_id;
}

// Resolve a configured `process` audio source to a live PID. The PID is never
// trusted as a persistent identity (it dies/recycles across reboots); we always
// re-resolve from the live render sessions, matching by executable path first
// and falling back to the process name. The saved process_id is used only as a
// best-effort hint when it is still alive AND points at the wanted executable.
static uint32_t resolve_configured_process(const settings::AudioSourceConfig& source)
{
    uint32_t window_pid = resolve_configured_process_window(source);
    if (window_pid != 0) return window_pid;

    const std::wstring wanted_path = source.executable_path;
    const std::string  wanted_name = source.process_name;

    auto sessions = audio::enumerate_render_sessions();

    // 0 = no match, 1 = name match, 2 = exact executable-path match.
    auto match_rank = [&](const audio::ProcessAudioSessionInfo& s) -> int {
        if (!wanted_path.empty() &&
            _wcsicmp(s.executable_path.c_str(), wanted_path.c_str()) == 0)
            return 2;
        if (!wanted_name.empty() &&
            _stricmp(wide_to_utf8(s.process_name).c_str(), wanted_name.c_str()) == 0)
            return 1;
        return 0;
    };

    uint32_t by_path = 0, by_name = 0;
    for (const auto& s : sessions) {
        int rank = match_rank(s);
        if (rank == 2 && by_path == 0) by_path = s.process_id;
        else if (rank == 1 && by_name == 0) by_name = s.process_id;
    }

    uint32_t resolved = by_path ? by_path : by_name;

    // Saved PID hint: accept only if still alive and matching the wanted exe.
    if (resolved == 0 && source.process_id != 0 &&
        audio::process_alive(source.process_id)) {
        for (const auto& s : sessions) {
            if (s.process_id == source.process_id && match_rank(s) > 0) {
                resolved = source.process_id;
                break;
            }
        }
    }

    const std::string wanted_label =
        !wanted_name.empty() ? wanted_name : wide_to_utf8(wanted_path);
    char buf[320];
    if (resolved != 0) {
        snprintf(buf, sizeof(buf), "resolved process '%s' -> pid %u (%s)",
                 wanted_label.c_str(), resolved,
                 by_path ? "by path" : "by name");
    } else {
        snprintf(buf, sizeof(buf),
                 "process '%s' not found among active render sessions",
                 wanted_label.c_str());
    }
    log_msg("audio.app", buf);
    return resolved;
}

static void stop_audio_system()
{
    for (auto& ac : g_audio_captures) {
        if (ac.capture) ac.capture->stop();
    }
    g_audio_captures.clear();
    for (auto& mixer : g_track_mixers)
        mixer.reset();
    for (auto& encoder : g_audio_encoders)
        encoder.close();
    g_track_plan_count.fill(0);
    g_active_game_tracks.clear();
    g_active_game_gain          = 1.0f;
    g_active_game_pid           = 0;
    g_active_game_capture       = nullptr;
    g_active_game_capture_mode  = "none";
    g_pending_game_pid          = 0;
    g_pending_game_first_ms     = 0;
}

static void start_default_audio_system()
{
    // start_endpoint_source() already closes its own tracks on failure, so the
    // return values are only informative here.
    std::vector<int> desktop_tracks{1};
    start_endpoint_source(
        audio::WasapiCapture::Mode::Loopback,
        L"",
        desktop_tracks,
        "audio.sys",
        "WASAPI loopback started -> audio track 1");

    std::vector<int> mic_tracks{2};
    start_endpoint_source(
        audio::WasapiCapture::Mode::Microphone,
        g_settings.primary_microphone_device_id,
        mic_tracks,
        "audio.mic",
        "WASAPI microphone started -> audio track 2");
}

static void start_custom_audio_system()
{
    // Plan first: count how many enabled sources route to each track so make_routes()
    // knows which tracks need a mixer (>= 2 sources) vs a direct encoder feed.
    g_track_plan_count.fill(0);
    for (const auto& source : g_settings.audio_sources) {
        if (!source.enabled) continue;
        for (int track : valid_tracks(source.tracks))
            g_track_plan_count[track]++;
    }

    // Pre-create encoders for configured tracks. Only tracks with multiple
    // sources pre-create a mixer; single-source tracks use direct encoder input
    // to avoid an unnecessary continuous mixer thread and silence generation.
    for (int track = 1; track <= 6; ++track) {
        if (g_track_plan_count[track] > 0) {
            open_audio_track(track);
            if (g_track_plan_count[track] > 1)
                ensure_track_mixer(track);
        }
    }

    for (const auto& source : g_settings.audio_sources) {
        if (!source.enabled) continue;
        std::vector<int> tracks = valid_tracks(source.tracks);
        if (tracks.empty()) continue;

        {
            char buf[128];
            int n = snprintf(buf, sizeof(buf), "starting source type=%s tracks:",
                             source.type.c_str());
            for (int t : tracks) {
                if (n < 0 || n >= static_cast<int>(sizeof(buf))) break;
                n += snprintf(buf + n, sizeof(buf) - n, " %d", t);
            }
            log_msg("audio", buf);
        }

        if (source.type == "desktop") {
            start_endpoint_source(audio::WasapiCapture::Mode::Loopback, L"",
                                  tracks, "audio.sys",
                                  "custom desktop audio started", source.volume);
        } else if (source.type == "input") {
            start_endpoint_source(audio::WasapiCapture::Mode::Microphone,
                                  source.device_id,
                                  tracks, "audio.in",
                                  "custom input device started", source.volume);
        } else if (source.type == "process") {
            uint32_t pid = resolve_configured_process(source);
            if (!start_process_source(pid, tracks, "audio.app",
                                      "custom process audio started", source.volume)) {
                log_msg("audio.app", "configured process not running or not capturable");
            }
        } else if (source.type == "active_game") {
            g_active_game_tracks = tracks;
            g_active_game_gain = source.volume;
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
[[maybe_unused]] static bool active_game_source_configured()
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
    // Keep the live game's foreground bonus stable while a Monolith window
    // (e.g. Settings) holds focus, so detection isn't invalidated temporarily.
    cfg.sticky_foreground_pid = g_active_game_pid;
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
        if (it->capture.get() == g_active_game_capture) {
            it->capture->stop();
            release_routes(it->routes);
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
// Drives ONLY the Active Game audio process-loopback capture toward `result`
// (the effective target chosen by poll_active_game()). Does not publish the
// active_game display block on the no-audio-source path — that is handled by
// poll_active_game() so the UI shows the game even without an active-game audio
// source configured.
static void poll_active_game_impl(const audio::ActiveGameResult& result)
{
    if (g_active_game_tracks.empty()) return;
    if (!g_settings.active_game.detection_enabled) return;

    // Drop current capture if the process has exited.
    if (g_active_game_pid != 0 && !audio::process_alive(g_active_game_pid))
        drop_active_game_capture("active game exited; watching for a new one");

    uint32_t best_pid = result.process.process_id;

    // ── No valid candidate ────────────────────────────────────────────────────
    if (best_pid == 0) {
        g_pending_game_pid      = 0;
        g_pending_game_first_ms = 0;
        // Reflect "no game" in the status only when nothing is being captured.
        // (A live capture with a transient 0 this cycle keeps its display.)
        if (g_active_game_pid == 0) {
            std::lock_guard<std::mutex> lk(g_status_mutex);
            g_runtime_status.active_game.process_id      = 0;
            g_runtime_status.active_game.process_name.clear();
            g_runtime_status.active_game.display_name.clear();
            g_runtime_status.active_game.executable_path.clear();
            g_runtime_status.active_game.confidence      = 0;
            g_runtime_status.active_game.reason.clear();
            g_runtime_status.active_game.capture_mode    = "none";
            g_runtime_status.active_game.process_loopback_available = false;
            g_runtime_status.active_game.poll_interval_ms  = g_settings.active_game.poll_interval_ms;
            g_runtime_status.active_game.fast_scan_enabled = g_settings.active_game.fast_scan_enabled;
        }
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
                                       "audio.game", "active game audio started",
                                       g_active_game_gain);
        if (ok) {
            g_active_game_pid          = best_pid;
            g_active_game_capture      = g_audio_captures.back().capture.get();
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
        // Status flushed by the poll_active_game() wrapper after this returns.
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
                                   "audio.game", "active game switched",
                                   g_active_game_gain);
    if (!ok) {
        g_active_game_capture_mode = "unavailable";
        log_msg("audio.game", "process-loopback unavailable for new game; switch aborted");
        g_pending_game_pid      = 0;
        g_pending_game_first_ms = 0;
        return;
    }

    audio::WasapiCapture* new_capture = g_audio_captures.back().capture.get();

    // Remove old capture — only the Active Game source, nothing else.
    if (g_active_game_capture != nullptr) {
        for (auto it = g_audio_captures.begin(); it != g_audio_captures.end(); ++it) {
            if (it->capture.get() == g_active_game_capture) {
                it->capture->stop();
                release_routes(it->routes);
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
    // Status flushed by the poll_active_game() wrapper after this returns.
}

// Resolves the effective target among candidates: an explicit user selection (by
// exe basename) wins; otherwise the most-recently-foregrounded game; ties broken
// by fullscreen then audio session. Returns nullptr when there are no candidates.
static const audio::GameCandidateInfo* resolve_effective_target(
    const std::vector<audio::GameCandidateInfo>& candidates)
{
    if (candidates.empty()) return nullptr;

    std::string sel;
    {
        std::lock_guard<std::mutex> lk(g_selected_game_mutex);
        sel = g_selected_game_exe;
    }
    if (!sel.empty()) {
        for (const auto& c : candidates)
            if (exe_key_of(c.process.process_name) == sel) return &c;
        // Selected game not currently running — fall through to auto.
    }

    const audio::GameCandidateInfo* best = nullptr;
    uint64_t best_focus = 0;
    int best_rank = -1;
    for (const auto& c : candidates) {
        auto it = g_focus_ms.find(c.process.process_id);
        const uint64_t focus = (it != g_focus_ms.end()) ? it->second : 0;
        const int rank = (c.fullscreen ? 2 : 0) + (c.has_session ? 1 : 0);
        if (!best || focus > best_focus ||
            (focus == best_focus && rank > best_rank)) {
            best = &c;
            best_focus = focus;
            best_rank = rank;
        }
    }
    return best;
}

// Public entry point: run one DB-gated detection pass, resolve the effective
// target, drive the Active Game audio capture toward it, publish the active game
// + full candidate list, and flush runtime-status.json. Does NOT touch the
// capture pipeline / auto-record — evaluate_capture_mode() does, from the shared
// state this leaves behind.
static void poll_active_game()
{
    if (!g_settings.active_game.detection_enabled) {
        g_candidate_pids.clear();
        g_effective_game_pid = 0;
        g_capture_target_hwnd.store(nullptr, std::memory_order_relaxed);
        publish_runtime_status();
        return;
    }

    auto candidates = audio::detect_game_candidates(build_detect_config());

    // Stamp foreground time; prune focus entries for games that are gone.
    const uint64_t now = GetTickCount64();
    std::vector<uint32_t> live_pids;
    live_pids.reserve(candidates.size());
    for (const auto& c : candidates) {
        live_pids.push_back(c.process.process_id);
        if (c.foreground) g_focus_ms[c.process.process_id] = now;
    }
    for (auto it = g_focus_ms.begin(); it != g_focus_ms.end();) {
        if (std::find(live_pids.begin(), live_pids.end(), it->first) == live_pids.end())
            it = g_focus_ms.erase(it);
        else
            ++it;
    }
    g_candidate_pids = live_pids;

    const audio::GameCandidateInfo* target = resolve_effective_target(candidates);
    g_effective_game_pid = target ? target->process.process_id : 0;
    // Only the game_only window-capture path uses this (capture target + pacer
    // frozen-frame). In screen mode we capture the whole monitor, so leave it
    // null — a minimized game must NOT freeze full-screen capture.
    g_capture_target_hwnd.store(
        (target && g_settings.capture_mode == "game_only") ? target->capture_window : nullptr,
        std::memory_order_relaxed);

    // Build the effective ActiveGameResult and drive the audio capture toward it.
    audio::ActiveGameResult eff;
    if (target) {
        eff.process = target->process;
        if (!target->display_name.empty())
            eff.process.display_name = utf8_to_wide(target->display_name);
        eff.confidence  = 100;
        eff.reason      = "gamelist";
        eff.has_session = target->has_session;
        eff.fullscreen  = target->fullscreen;
    }
    poll_active_game_impl(eff);

    // Publish the active game (display block) + full candidate list + selection.
    {
        std::lock_guard<std::mutex> lk(g_status_mutex);
        auto& ag = g_runtime_status.active_game;
        if (target) {
            ag.process_id      = target->process.process_id;
            ag.process_name    = target->process.process_name;
            ag.display_name    = eff.process.display_name;
            ag.executable_path = target->process.executable_path;
            if (ag.confidence == 0) ag.confidence = 100;
            if (ag.reason.empty()) ag.reason = "gamelist";
        } else {
            ag.process_id = 0;
            ag.process_name.clear();
            ag.display_name.clear();
            ag.executable_path.clear();
            ag.confidence = 0;
            ag.reason.clear();
            ag.capture_mode = "none";
            ag.process_loopback_available = false;
        }
        ag.poll_interval_ms  = g_settings.active_game.poll_interval_ms;
        ag.fast_scan_enabled = g_settings.active_game.fast_scan_enabled;

        g_runtime_status.selected_game_pid = g_effective_game_pid;
        g_runtime_status.game_candidates.clear();
        for (const auto& c : candidates) {
            settings::GameCandidateStatus s;
            s.process_id      = c.process.process_id;
            s.process_name    = c.process.process_name;
            s.display_name    = c.display_name.empty()
                                    ? c.process.display_name
                                    : utf8_to_wide(c.display_name);
            s.discord_app_id  = c.discord_app_id;
            s.executable_path = c.process.executable_path;
            s.foreground      = c.foreground;
            s.fullscreen      = c.fullscreen;
            g_runtime_status.game_candidates.push_back(std::move(s));
        }
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

struct WindowSearch {
    DWORD pid = 0;
    HWND hwnd = nullptr;
};

static BOOL CALLBACK find_main_window_proc(HWND hwnd, LPARAM lp)
{
    auto* search = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    if ((rc.right - rc.left) <= 0 || (rc.bottom - rc.top) <= 0) return TRUE;
    search->hwnd = hwnd;
    return FALSE;
}

static HWND main_window_for_process(uint32_t pid)
{
    if (pid == 0) return nullptr;
    WindowSearch search{ static_cast<DWORD>(pid), nullptr };
    EnumWindows(find_main_window_proc, reinterpret_cast<LPARAM>(&search));
    return search.hwnd;
}

// Output resolution derives from a preset target height while preserving the
// source aspect ratio. Never upscales beyond native (a preset taller than the
// source falls back to source), so no pixels are invented. Shared by the
// upfront GPU-downscale sizing (CaptureOptions::output_width/height, known
// before the first frame for monitor capture) and the encoder-open fallback
// (which resolves against the first captured frame's actual size, needed for
// game_only window capture where native size isn't known upfront).
static void resolve_target_size(int src_w, int src_h, const std::string& preset,
                                 int* out_w, int* out_h)
{
    src_w &= ~1;
    src_h &= ~1;
    int target_h = 0;
    if (preset == "480p")       target_h = 480;
    else if (preset == "720p")  target_h = 720;
    else if (preset == "1080p") target_h = 1080;
    else if (preset == "1440p") target_h = 1440;
    if (target_h > 0 && src_h > 0 && target_h < src_h) {
        *out_h = target_h & ~1;
        *out_w = (static_cast<int>(std::lround(
            static_cast<double>(src_w) * target_h / src_h))) & ~1;
    } else {
        *out_w = src_w;
        *out_h = src_h;
    }
}

static void media_start(HWND hwnd)
{
    // ── Replay buffer config ───────────────────────────────────────────────────
    apply_runtime_settings();

    // Do not run WGC/pacer/audio when no video output is active. Keep this
    // before monitor enumeration and encoder probing, which can be relatively
    // expensive and should not happen for replay-off idle startup.
    if (!g_settings.replay_buffer_enabled &&
        g_recording.state() == recording::RecordingState::Idle) {
        log_msg("app", "media pipeline not started: replay disabled and recording idle");
        return;
    }

    // ── Capture monitor from config (restart-required setting) ───────────────
    std::vector<MonitorEntry> monitors = enumerate_monitors();
    std::wstring used_device;
    HMONITOR hmon = monitor_from_settings(monitors, &used_device);

    // Screen-mode auto-follow: with no manual monitor pin, capture the screen the
    // detected game is on. A manual pick (monitor_device set) always wins and is
    // already returned by monitor_from_settings above.
    if (g_settings.capture_mode == "always" && g_settings.monitor_device.empty() &&
        g_effective_game_pid != 0) {
        HWND gw = main_window_for_process(g_effective_game_pid);
        if (gw) {
            HMONITOR gm = MonitorFromWindow(gw, MONITOR_DEFAULTTONEAREST);
            for (const auto& entry : monitors) {
                if (entry.hmon == gm) {
                    hmon = gm;
                    used_device = entry.info.device;
                    log_msg("capture", "screen mode: following detected game's monitor");
                    break;
                }
            }
        }
    }

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
    g_video_enc_retry_after_ms.store(0, std::memory_order_release);
    g_enc_w = 0;
    g_enc_h = 0;

    g_perf_pacer_frames_submitted.store(0, std::memory_order_relaxed);
    g_perf_encoder_frames_submitted.store(0, std::memory_order_relaxed);
    g_perf_encoder_push_time_us_total.store(0, std::memory_order_relaxed);
    g_perf_encoder_packets_output.store(0, std::memory_order_relaxed);
    g_perf_bgra_memcpy_frames.store(0, std::memory_order_relaxed);
    g_perf_bgra_memcpy_time_us_total.store(0, std::memory_order_relaxed);

    // ── Video encoder ─────────────────────────────────────────────────────────
    log_msg("encoding", "video encoder deferred until first WGC frame");

    // ── Start pacer thread (OBS-style fixed-rate encoder feed) ────────────────
    pacer_start();

    // ── Audio capture/routing ──────────────────────────────────────────────────
    g_main_hwnd = hwnd;
    start_audio_system();
    if (g_settings.active_game.detection_enabled) {
        // Detection runs in ALL modes now (game_only AND screen): it drives
        // naming/icon, the candidate list, and auto-record. Fixed 3 s cadence.
        SetTimer(hwnd, kActiveGameTimerId, kActiveGamePollMs, nullptr);
        // Foreground-change fast scan is always on for snappy game switches.
        install_fg_hook();
        // Initial detection pass (doesn't wait for first timer tick).
        poll_active_game();
    }

    // ── WGC display capture ───────────────────────────────────────────────────
    if (capture::is_supported()) {
        bool ok = g_video.start(hmon, [](capture::FrameInfo const& f) {
            // Log buffer stats every 300 frames (~5s at 60fps).
            if (f.seq % 300 == 0) {
                auto stats = g_replay.stats();
                size_t raw_capacity = 0;
                {
                    std::lock_guard<std::mutex> lock(g_pacer_mutex);
                    raw_capacity = g_pacer_shared.bgra.capacity();
                }
                const double span_sec = stats.newest_dts_usec > stats.oldest_dts_usec
                    ? static_cast<double>(stats.newest_dts_usec - stats.oldest_dts_usec) / 1000000.0
                    : 0.0;
                char buf[224];
                snprintf(buf, sizeof(buf),
                    "seq=%-6u %ux%u replay=%zu pkt / %.1f MB keyframes=%d span=%.1fs saving=%s raw_cap=%.1f MB",
                    f.seq, f.width, f.height,
                    stats.packet_count,
                    static_cast<double>(stats.logical_bytes) / 1048576.0,
                    stats.keyframes,
                    span_sec,
                    stats.saving ? "true" : "false",
                    static_cast<double>(raw_capacity) / 1048576.0);
                log_msg("capture", buf);

                static capture::CaptureStats last_capture_stats{};
                static uint64_t last_pacer_submitted = 0;
                static uint64_t last_encoder_submitted = 0;
                static uint64_t last_encoder_packets = 0;
                static uint64_t last_encoder_push_us = 0;
                static uint64_t last_memcpy_frames = 0;
                static uint64_t last_memcpy_us = 0;
                static encoding::VideoEncoderPerfStats last_video_stats{};

                const capture::CaptureStats capture_stats = g_video.stats();
                if (capture_stats.frames_arrived < last_capture_stats.frames_arrived)
                    last_capture_stats = {};
                const uint64_t pacer_submitted = g_perf_pacer_frames_submitted.load(std::memory_order_relaxed);
                const uint64_t encoder_submitted = g_perf_encoder_frames_submitted.load(std::memory_order_relaxed);
                const uint64_t encoder_packets = g_perf_encoder_packets_output.load(std::memory_order_relaxed);
                const uint64_t encoder_push_us = g_perf_encoder_push_time_us_total.load(std::memory_order_relaxed);
                const uint64_t memcpy_frames = g_perf_bgra_memcpy_frames.load(std::memory_order_relaxed);
                const uint64_t memcpy_us = g_perf_bgra_memcpy_time_us_total.load(std::memory_order_relaxed);
                const encoding::VideoEncoderPerfStats video_stats = g_video_enc.perf_stats();
                if (video_stats.frames_submitted < last_video_stats.frames_submitted)
                    last_video_stats = {};
                if (pacer_submitted < last_pacer_submitted) last_pacer_submitted = 0;
                if (encoder_submitted < last_encoder_submitted) last_encoder_submitted = 0;
                if (encoder_packets < last_encoder_packets) last_encoder_packets = 0;
                if (encoder_push_us < last_encoder_push_us) last_encoder_push_us = 0;
                if (memcpy_frames < last_memcpy_frames) last_memcpy_frames = 0;
                if (memcpy_us < last_memcpy_us) last_memcpy_us = 0;

                const uint64_t readback_delta = capture_stats.frames_readback - last_capture_stats.frames_readback;
                const uint64_t readback_us_delta = capture_stats.readback_time_us_total - last_capture_stats.readback_time_us_total;
                const uint64_t encoder_delta = encoder_submitted - last_encoder_submitted;
                const uint64_t encoder_push_us_delta = encoder_push_us - last_encoder_push_us;
                const double avg_readback_ms = readback_delta
                    ? static_cast<double>(readback_us_delta) / static_cast<double>(readback_delta) / 1000.0
                    : 0.0;
                const double avg_push_ms = encoder_delta
                    ? static_cast<double>(encoder_push_us_delta) / static_cast<double>(encoder_delta) / 1000.0
                    : 0.0;
                const uint64_t memcpy_delta = memcpy_frames - last_memcpy_frames;
                const double avg_memcpy_ms = memcpy_delta
                    ? static_cast<double>(memcpy_us - last_memcpy_us) / static_cast<double>(memcpy_delta) / 1000.0
                    : 0.0;
                const uint64_t video_frames_delta = video_stats.frames_submitted - last_video_stats.frames_submitted;
                const double avg_sws_ms = video_frames_delta
                    ? static_cast<double>(video_stats.sws_scale_time_us_total - last_video_stats.sws_scale_time_us_total) /
                        static_cast<double>(video_frames_delta) / 1000.0
                    : 0.0;
                const double avg_encode_ms = video_frames_delta
                    ? static_cast<double>(video_stats.encode_time_us_total - last_video_stats.encode_time_us_total) /
                        static_cast<double>(video_frames_delta) / 1000.0
                    : 0.0;

                char perf[320];
                snprintf(perf, sizeof(perf),
                    "wgc=%llu drop_pre_readback=%llu readback=%llu pacer=%llu enc_frames=%llu packets=%llu avg_readback_ms=%.2f avg_memcpy_ms=%.2f avg_push_ms=%.2f avg_sws_ms=%.2f avg_encode_ms=%.2f fps=%d",
                    static_cast<unsigned long long>(capture_stats.frames_arrived - last_capture_stats.frames_arrived),
                    static_cast<unsigned long long>(capture_stats.frames_dropped_before_readback - last_capture_stats.frames_dropped_before_readback),
                    static_cast<unsigned long long>(readback_delta),
                    static_cast<unsigned long long>(pacer_submitted - last_pacer_submitted),
                    static_cast<unsigned long long>(encoder_delta),
                    static_cast<unsigned long long>(encoder_packets - last_encoder_packets),
                    avg_readback_ms,
                    avg_memcpy_ms,
                    avg_push_ms,
                    avg_sws_ms,
                    avg_encode_ms,
                    g_settings.video_fps);
                log_msg("perf-video", perf);

                last_capture_stats = capture_stats;
                last_pacer_submitted = pacer_submitted;
                last_encoder_submitted = encoder_submitted;
                last_encoder_packets = encoder_packets;
                last_encoder_push_us = encoder_push_us;
                last_memcpy_frames = memcpy_frames;
                last_memcpy_us = memcpy_us;
                last_video_stats = video_stats;
            }
            // Push BGRA frame to pacer. Pacer thread drives encoder at fixed FPS.
            if (f.bgra_data == nullptr) return;
            // Replay buffer disabled: encode only while a manual recording
            // is running, so an idle tray app costs no encoder CPU.
            if (!g_replay_enabled.load(std::memory_order_relaxed) &&
                g_recording.state() == recording::RecordingState::Idle)
                return;
            if (!g_video_enc.is_open()) {
                // Respect the post-failure backoff window so a transient open
                // failure (e.g. GPU busy / driver reset) doesn't get re-probed on
                // every single captured frame.
                if (GetTickCount64() <
                    g_video_enc_retry_after_ms.load(std::memory_order_acquire))
                    return;

                bool expected = false;
                if (!g_video_enc_open_attempted.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    return;
                }

                // Resolves against the actually-captured frame size, which is
                // already the GPU-downscaled size when CaptureOptions below
                // configured one (this then just confirms it) — and is still
                // native size in game_only mode (no upfront target there), so
                // this remains the source of truth either way.
                int g_enc_w_resolved = 0, g_enc_h_resolved = 0;
                resolve_target_size(static_cast<int>(f.width), static_cast<int>(f.height),
                                     g_settings.resolution_preset,
                                     &g_enc_w_resolved, &g_enc_h_resolved);
                g_enc_w = g_enc_w_resolved;
                g_enc_h = g_enc_h_resolved;

                // Resolve the concrete FFmpeg encoder from the user's CPU/GPU +
                // codec choice, given what this machine can actually open.
                std::string resolved = encoding::resolve_video_encoder(
                    g_settings.encoder_device, g_settings.encoder_codec,
                    g_enc_w, g_enc_h);

                encoding::VideoEncoder::Config vcfg;
                vcfg.width             = g_enc_w;
                vcfg.height            = g_enc_h;
                vcfg.fps               = g_settings.video_fps;
                vcfg.quality           = 0; // 0 = CBR
                vcfg.bitrate           = static_cast<int64_t>(g_settings.video_bitrate_kbps) * 1000;
                vcfg.scaling_filter    = "bilinear"; // fixed; scaler choice removed from UI
                vcfg.preferred_encoder = resolved; // "" → probe order fallback
                vcfg.extra_options     = g_settings.extra_ffmpeg_options;

                bool enc_ok = g_video_enc.open(vcfg, [](encoding::EncodedPacket pkt) {
                    g_perf_encoder_packets_output.fetch_add(1, std::memory_order_relaxed);
                    if (replay_active()) {
                        auto replay_pkt = pkt;
                        g_replay.push(std::move(replay_pkt));
                    }
                    g_recording.push(std::move(pkt));
                });

                if (enc_ok) {
                    char enc_msg[256];
                    snprintf(enc_msg, sizeof(enc_msg),
                        "%s %dx%d @%dfps %dkbps CBR%s%s",
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
                    log_msg("encoding", "WARNING: video encoder open failed - will retry");
                }

                if (enc_ok) {
                    auto params = g_video_enc.stream_params();
                    g_replay.set_video_params(params);
                    g_recording.set_video_params(params);
                    {
                        std::lock_guard<std::mutex> lk(g_status_mutex);
                        g_runtime_status.active_encoder = g_video_enc.encoder_name();
                        g_runtime_status.video_encoder_error.clear();
                        g_runtime_status.encode_width   = g_enc_w;
                        g_runtime_status.encode_height  = g_enc_h;
                    }
                    publish_runtime_status();
                } else {
                    {
                        char err[176];
                        snprintf(err, sizeof(err),
                            "No usable video encoder for %dx%d @%dfps (%s/%s). Retrying…",
                            g_enc_w, g_enc_h, g_settings.video_fps,
                            g_settings.encoder_device.c_str(),
                            g_settings.encoder_codec.c_str());
                        std::lock_guard<std::mutex> lk(g_status_mutex);
                        g_runtime_status.active_encoder.clear();
                        g_runtime_status.video_encoder_error = err;
                    }
                    publish_runtime_status();
                    // Re-arm so a later frame can retry after the backoff window.
                    g_video_enc_retry_after_ms.store(
                        GetTickCount64() + kVideoEncRetryBackoffMs,
                        std::memory_order_release);
                    g_video_enc_open_attempted.store(false, std::memory_order_release);
                    return;
                }
            }
            pacer_push_frame(f.bgra_data,
                             static_cast<int>(f.bgra_stride),
                             static_cast<int>(f.width),
                             static_cast<int>(f.height));
        }, [&]() {
            capture::CaptureOptions options;
            // Capture border is always suppressed; the toggle was removed from the
            // UI and the WGC yellow border is never wanted for a recorder.
            options.show_border = false;
            options.max_readback_fps = std::max(1, g_settings.video_fps);
            options.allow_unlimited_readback = false;

            // In game_only, capture the effective target's window (resolved by the
            // detection poll). When there is no target (no game, or its window is
            // unavailable) we fall back to full-screen monitor capture — which is
            // also the clip-without-game path.
            HWND target = nullptr;
            if (g_settings.capture_mode == "game_only") {
                target = g_capture_target_hwnd.load(std::memory_order_relaxed);
                if (!target) {
                    // First start before a detection pass has run: resolve now.
                    audio::ProcessInfo game = audio::detect_active_game();
                    target = main_window_for_process(game.process_id);
                    g_capture_target_hwnd.store(target, std::memory_order_relaxed);
                }
            }

            if (target) {
                options.target_window = target;
                log_msg("capture", "game_only: capturing detected game window");
            } else {
                if (g_settings.capture_mode == "game_only")
                    log_msg("capture", "game_only: no game window, capturing full screen");
                // Native monitor size is known upfront here (unlike a window
                // target), so the GPU downscale target can be set before start().
                for (const auto& entry : monitors) {
                    if (entry.hmon == hmon) {
                        resolve_target_size(entry.info.width, entry.info.height,
                                             g_settings.resolution_preset,
                                             &options.output_width, &options.output_height);
                        break;
                    }
                }
            }
            return options;
        }());
        log_msg("capture", ok ? "WGC display capture started"
                               : "WARNING: WGC display capture failed");
        if (ok) {
            log_msg("capture", g_video.border_suppressed()
                ? "capture border: suppressed (IsBorderRequired=false)"
                : "capture border: suppression DENIED by OS (border visible)");
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
    {
        std::lock_guard<std::mutex> lk(g_status_mutex);
        g_runtime_status.active_encoder.clear();
        g_runtime_status.video_encoder_error.clear();
    }
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
    // Save Replay needs the component enabled, not suspended for a recording, and
    // the encoder feeding the ring.
    UINT save_flags  = (replay_on && replay_active() && g_video_enc.is_open())
        ? MF_STRING : (MF_STRING | MF_GRAYED);
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
    AppendMenuW(menu, MF_STRING,             CMD_SETTINGS,        L"Show");
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, save_flags,            CMD_SAVE_REPLAY,     save_text.c_str());
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, start_flags,            CMD_RECORDING_START, start_text.c_str());
    AppendMenuW(menu, stop_flags,             CMD_RECORDING_STOP,  stop_text.c_str());
    AppendMenuW(menu, pause_flags,            CMD_PAUSE_RESUME,    pause_text.c_str());
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
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
    if (key == "SPACE" || key == "SPACEBAR" || key == " ") return VK_SPACE;
    if (key == "TAB") return VK_TAB;
    if (key == "ENTER" || key == "RETURN") return VK_RETURN;
    if (key == "BACKSPACE") return VK_BACK;
    if (key == "ESC" || key == "ESCAPE") return VK_ESCAPE;
    if (key == "PLUS") return VK_OEM_PLUS;
    if (key == "MINUS") return VK_OEM_MINUS;
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
        else if (upper == "WIN" || upper == "WINDOWS" || upper == "META" ||
                 upper == "CMD" || upper == "SUPER") vk = VK_LWIN;
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

// True when a clip/record command must be blocked: game_only mode, no known game
// running, and clip-without-game is off (so there is genuinely nothing to
// capture). When clip-without-game is on we allow it — capture falls back to the
// full screen.
static bool game_gate_blocks()
{
    if (g_settings.capture_mode != "game_only") return false;
    if (g_settings.capture_clip_without_game) return false;
    return audio::detect_active_game().process_id == 0;
}

static void dispatch(Cmd cmd, HWND hwnd)
{
    switch (cmd) {
    case CMD_SAVE_REPLAY: {
        if (!g_replay_enabled.load(std::memory_order_relaxed)) {
            log_msg("replay", "save_replay ignored: replay buffer disabled in settings");
            break;
        }
        if (g_replay_suspended_for_recording.load(std::memory_order_relaxed)) {
            log_msg("replay", "save_replay ignored: replay suspended while recording "
                              "(enable concurrent capture in Advanced to allow both)");
            break;
        }
        if (game_gate_blocks()) {
            log_msg("replay", "save_replay ignored: no supported game detected");
            break;
        }
        char stats[256];
        snprintf(stats, sizeof(stats),
            "save_replay — buffer: %zu pkt / %.1f MB",
            g_replay.packet_count(),
            static_cast<double>(g_replay.memory_bytes()) / 1048576.0);
        log_msg("replay", stats);
        feedback::play(feedback::Sound::ClipSaved);
        const double clip_duration = g_settings.replay_duration_seconds;
        g_replay.save_clip([clip_duration](std::wstring path) {
            // Runs on the replay save thread (off the UI/tray loop).
            if (path.empty()) {
                log_msg("replay", "WARNING: clip save failed or encoder not ready");
            } else {
                log_path("replay", "clip saved: ", path);
                catalog_clip(path, "replay", clip_duration);
            }
        });
        break;
    }
    case CMD_RECORDING_START:
        if (!g_recording_enabled.load(std::memory_order_relaxed)) {
            log_msg("recording", "recording_start ignored: recording disabled in settings");
            break;
        }
        if (game_gate_blocks()) {
            log_msg("recording", "recording_start ignored: no supported game detected");
            break;
        }
        if (g_recording.start(g_recordings_dir, g_recording_container)) {
            g_auto_recording_active = false;
            suspend_replay_for_recording();
            if (!g_video.running())
                media_start(hwnd);
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
            else {
                log_path("recording", "recording saved: ", path);
                // Catalog off the UI thread — decode + DB I/O must not block it.
                std::thread(catalog_clip, path, std::string("manual"), 0.0).detach();
            }
            feedback::play(feedback::Sound::RecordStop);
            tray_set_recording(false);
            restore_replay_after_recording();
            if (!g_replay_enabled.load(std::memory_order_relaxed) && g_video.running())
                media_stop();
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
                g_replay_enabled.load(std::memory_order_relaxed),
                g_recording_enabled.load(std::memory_order_relaxed),
                g_clip_generation.load(std::memory_order_relaxed),
            };
        }, handle_clip_mutation,
        [](const std::string& exe, uint32_t /*pid*/) {
            // Runs on an IPC client thread: only touch synchronized state, then
            // nudge the message loop to re-evaluate on the UI thread.
            {
                std::lock_guard<std::mutex> lk(g_selected_game_mutex);
                g_selected_game_exe = exe; // already lowercased; "" = auto
            }
            PostMessageW(g_main_hwnd, WM_FAST_SCAN, 0, 0);
        });
        publish_runtime_status();
        log_msg("app", "app shell ready");
        return 0;

    case WM_TIMER:
        if (wp == kActiveGameTimerId) {
            poll_active_game();
            evaluate_capture_mode(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_FAST_SCAN: {
        // Foreground-change fast scan, rate-limited to one pass per second. Runs
        // the capture/auto-record evaluation too, so the startup-focus grace and
        // auto-record react immediately when the user foregrounds a game.
        g_fast_scan_pending = false;
        constexpr DWORD kFastScanMinGapMs = 1000;
        DWORD now_ms = GetTickCount();
        if ((now_ms - g_last_fast_scan_ms) >= kFastScanMinGapMs) {
            g_last_fast_scan_ms = now_ms;
            poll_active_game();
            evaluate_capture_mode(hwnd);
        }
        return 0;
    }

    case WM_DESTROY:
        settings_window::close_running();
        KillTimer(hwnd, kActiveGameTimerId);
        uninstall_fg_hook();
        updater::shutdown();
        ipc::stop();
        gamelist::shutdown();
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

    case WM_REFRESH_AUDIO_SOURCES:
        // On-demand re-enumeration requested by the Settings UI (e.g. when the
        // user opens the "Add source" dropdown). Refresh the live audio session
        // list and rewrite runtime-status.json synchronously so the caller sees
        // current sessions as soon as this returns.
        refresh_audio_runtime_status();
        publish_runtime_status();
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

    // OBS-style: per-monitor DPI awareness so WGC returns physical pixels.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // Logging is gated by advanced.logging_enabled, which load_app_settings()
    // resolves — so settings must load first. Anything load_app_settings()
    // itself would have logged is lost; acceptable startup-only gap.
    load_app_settings();
    log_init(g_settings.logging_enabled);
    log_msg("app", "initializing (Monolith: replay + manual recording)");
    audio::set_log_sink([](const char* tag, const char* msg) { log_msg(tag, msg); });
    // Game-list DB: detection is gated on it. Sync failures are surfaced via the
    // always-on error log. init() kicks a background worker (startup + 72h sync);
    // the tray/UI loop never blocks on the network or DB.
    gamelist::set_log_sink([](const char* tag, const char* msg) { log_error(tag, msg); });
    gamelist::init(app_data_dir());
    reconcile_catalogs(); // self-heal clip catalogs on a background thread
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
    return static_cast<int>(msg.wParam);
}
