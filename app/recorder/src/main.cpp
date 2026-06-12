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
#include <cstdio>
#include <mutex>
#include <string>

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

enum HotkeyId : int {
    HK_SAVE_REPLAY     = 1,
    HK_RECORDING_START = 2,
    HK_RECORDING_STOP  = 3,
    HK_PAUSE_RESUME    = 4,
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
    _wfopen_s(&g_log, path.c_str(), L"a");
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

// ── Globals ───────────────────────────────────────────────────────────────────

static capture::DisplayCapture      g_video;
static audio::WasapiCapture         g_audio_system;
static audio::WasapiCapture         g_audio_mic;
static encoding::VideoEncoder       g_video_enc;
static encoding::AudioEncoder       g_audio_enc;
static replay_buffer::ReplayBuffer  g_replay;
static recording::ManualRecorder    g_recording;

static int g_enc_w = 0; // configured encoder width (even-aligned)
static int g_enc_h = 0; // configured encoder height (even-aligned)
static std::atomic<bool> g_video_enc_open_attempted{ false };
static std::wstring g_recordings_dir;
static settings::Config g_settings;

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

    char msg[128];
    snprintf(msg, sizeof(msg), "settings applied: replay=%ds / %lldMB",
             g_settings.replay_duration_seconds,
             static_cast<long long>(g_settings.replay_memory_budget_mb));
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

    for (const auto& warning : result.warnings)
        log_msg("settings", warning.c_str());

    log_path("settings", "config path: ", g_settings.user_config_path);
}

static void show_settings(HWND hwnd)
{
    settings_window::show(hwnd, WM_SETTINGS_RELOAD);
}

static void reload_settings_from_disk()
{
    load_app_settings();
    apply_runtime_settings();
    log_msg("settings", "settings reloaded from WinUI app");
}

// ── Media start / stop ────────────────────────────────────────────────────────

static void media_start(HWND hwnd)
{
    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);

    // ── Determine monitor resolution ─────────────────────────────────────────
    g_video_enc_open_attempted.store(false, std::memory_order_release);
    g_enc_w = 0;
    g_enc_h = 0;

    // ── Video encoder ─────────────────────────────────────────────────────────
    log_msg("encoding", "video encoder deferred until first WGC frame");

    // ── Audio encoder ─────────────────────────────────────────────────────────
    {
        encoding::AudioEncoder::Config acfg;
        acfg.sample_rate = 48000;
        acfg.channels    = 2;
        acfg.bitrate     = 192'000;

        bool ok = g_audio_enc.open(acfg, [](encoding::EncodedPacket pkt) {
            auto replay_pkt = pkt;
            g_replay.push(std::move(replay_pkt));
            g_recording.push(std::move(pkt));
        });
        log_msg("encoding", ok ? "AAC encoder opened (48kHz stereo 192kbps)"
                               : "WARNING: AAC encoder failed");

        if (ok) {
            auto params = g_audio_enc.stream_params();
            g_replay.set_audio_params(params);
            g_recording.set_audio_params(params);
        }
    }

    // ── Replay buffer config ───────────────────────────────────────────────────
    {
        apply_runtime_settings();
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
            // Encode every 2nd frame → 30fps effective for software encoder.
            if (f.seq % 2 != 0 || f.bgra_data == nullptr) return;
            if (!g_video_enc.is_open()) {
                bool expected = false;
                if (!g_video_enc_open_attempted.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    return;
                }

                g_enc_w = static_cast<int>(f.width & ~1u);
                g_enc_h = static_cast<int>(f.height & ~1u);

                encoding::VideoEncoder::Config vcfg;
                vcfg.width   = g_enc_w;
                vcfg.height  = g_enc_h;
                vcfg.fps     = 30;
                vcfg.bitrate = 20'000'000;

                bool enc_ok = g_video_enc.open(vcfg, [](encoding::EncodedPacket pkt) {
                    auto replay_pkt = pkt;
                    g_replay.push(std::move(replay_pkt));
                    g_recording.push(std::move(pkt));
                });

                char enc_msg[128];
                snprintf(enc_msg, sizeof(enc_msg),
                    "%dx%d @30fps bitrate=20Mbps", g_enc_w, g_enc_h);
                log_msg("encoding", enc_ok ? enc_msg : "WARNING: no H.264 encoder - replay disabled");

                if (enc_ok) {
                    auto params = g_video_enc.stream_params();
                    g_replay.set_video_params(params);
                    g_recording.set_video_params(params);
                } else {
                    return;
                }
            }
            g_video_enc.push_bgra(
                f.bgra_data,
                static_cast<int>(f.bgra_stride),
                static_cast<int>(f.width),
                static_cast<int>(f.height));
        });
        log_msg("capture", ok ? "WGC display capture started"
                               : "WARNING: WGC display capture failed");
    } else {
        log_msg("capture", "WARNING: Windows.Graphics.Capture not supported");
    }

    // ── WASAPI loopback (system audio → encoder + replay buffer) ─────────────
    bool ok_sys = g_audio_system.start(
        audio::WasapiCapture::Mode::Loopback,
        [](audio::PacketInfo const& p) {
            if (p.seq % 500 == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "seq=%-6u  frames=%-4u  sr=%u  ch=%u%s",
                    p.seq, p.frame_count, p.sample_rate, p.channels,
                    p.silent ? "  [silent]" : "");
                log_msg("audio.sys", buf);
            }
            if (p.data_bytes > 0) {
                g_audio_enc.push_pcm(
                    p.data, static_cast<int>(p.data_bytes),
                    static_cast<int>(p.sample_rate),
                    static_cast<int>(p.channels),
                    static_cast<int>(p.bit_depth), p.is_float);
            }
        });
    log_msg("audio.sys", ok_sys ? "WASAPI loopback started"
                                 : "WARNING: WASAPI loopback failed");

    // ── WASAPI microphone (monitor only, not fed to replay buffer for MVP) ───
    bool ok_mic = g_audio_mic.start(
        audio::WasapiCapture::Mode::Microphone,
        [](audio::PacketInfo const& p) {
            if (p.seq % 500 == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "seq=%-6u  frames=%-4u  sr=%u  ch=%u%s",
                    p.seq, p.frame_count, p.sample_rate, p.channels,
                    p.silent ? "  [silent]" : "");
                log_msg("audio.mic", buf);
            }
        });
    log_msg("audio.mic", ok_mic ? "WASAPI mic started"
                                 : "WARNING: WASAPI mic failed (no mic?)");
}

static void media_stop()
{
    g_video.stop();
    g_audio_system.stop();
    g_audio_mic.stop();
    g_video_enc.close(); // flushes + frees encoder
    g_audio_enc.close();
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

static void tray_show_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    recording::RecordingState state = g_recording.state();
    UINT start_flags = (state == recording::RecordingState::Idle) ? MF_STRING : (MF_STRING | MF_GRAYED);
    UINT stop_flags  = (state == recording::RecordingState::Idle) ? (MF_STRING | MF_GRAYED) : MF_STRING;
    UINT pause_flags = (state == recording::RecordingState::Idle) ? (MF_STRING | MF_GRAYED) : MF_STRING;
    const wchar_t* pause_text =
        (state == recording::RecordingState::Paused)
            ? L"Resume Recording\tCtrl+Shift+F11"
            : L"Pause Recording\tCtrl+Shift+F11";
    AppendMenuW(menu, MF_STRING,             CMD_SAVE_REPLAY,     L"Save Replay\tCtrl+Shift+F8");
    AppendMenuW(menu, MF_SEPARATOR,          0,                   nullptr);
    AppendMenuW(menu, start_flags,            CMD_RECORDING_START, L"Start Recording\tCtrl+Shift+F9");
    AppendMenuW(menu, stop_flags,             CMD_RECORDING_STOP,  L"Stop Recording\tCtrl+Shift+F10");
    AppendMenuW(menu, pause_flags,            CMD_PAUSE_RESUME,    pause_text);
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

static void hotkeys_register(HWND hwnd)
{
    if (!RegisterHotKey(hwnd, HK_SAVE_REPLAY,
                        MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F8))
        log_msg("hotkey", "WARNING: save_replay (Ctrl+Shift+F8) failed - key in use");
    else
        log_msg("hotkey", "save_replay (Ctrl+Shift+F8) registered");

    if (!RegisterHotKey(hwnd, HK_RECORDING_START,
                        MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F9))
        log_msg("hotkey", "WARNING: recording_start (Ctrl+Shift+F9) failed - key in use");
    else
        log_msg("hotkey", "recording_start (Ctrl+Shift+F9) registered");

    if (!RegisterHotKey(hwnd, HK_RECORDING_STOP,
                        MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F10))
        log_msg("hotkey", "WARNING: recording_stop (Ctrl+Shift+F10) failed - key in use");
    else
        log_msg("hotkey", "recording_stop (Ctrl+Shift+F10) registered");

    if (!RegisterHotKey(hwnd, HK_PAUSE_RESUME,
                        MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F11))
        log_msg("hotkey", "WARNING: pause_resume (Ctrl+Shift+F11) failed - key in use");
    else
        log_msg("hotkey", "pause_resume (Ctrl+Shift+F11) registered");
}

static void hotkeys_unregister(HWND hwnd)
{
    UnregisterHotKey(hwnd, HK_SAVE_REPLAY);
    UnregisterHotKey(hwnd, HK_RECORDING_START);
    UnregisterHotKey(hwnd, HK_RECORDING_STOP);
    UnregisterHotKey(hwnd, HK_PAUSE_RESUME);
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
        if (g_recording.start(g_recordings_dir)) {
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
        log_msg("app", "app shell ready");
        return 0;

    case WM_DESTROY:
        media_stop();
        hotkeys_unregister(hwnd);
        tray_remove();
        log_msg("app", "app shell stopped");
        PostQuitMessage(0);
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK)
            tray_show_menu(hwnd);
        return 0;

    case WM_COMMAND:
        dispatch(static_cast<Cmd>(LOWORD(wp)), hwnd);
        return 0;

    case WM_HOTKEY:
        if (wp == HK_SAVE_REPLAY)
            dispatch(CMD_SAVE_REPLAY, hwnd);
        else if (wp == HK_RECORDING_START)
            dispatch(CMD_RECORDING_START, hwnd);
        else if (wp == HK_RECORDING_STOP)
            dispatch(CMD_RECORDING_STOP, hwnd);
        else if (wp == HK_PAUSE_RESUME)
            dispatch(CMD_PAUSE_RESUME, hwnd);
        return 0;

    case WM_SETTINGS_RELOAD:
        reload_settings_from_disk();
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
        0, kWindowClass, kAppName,
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        log_msg("app", "FATAL: CreateWindowExW failed");
        winrt::uninit_apartment();
        CloseHandle(mutex);
        return 1;
    }

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
