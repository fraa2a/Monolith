#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace audio {

struct PacketInfo {
    int64_t  timestamp_qpc; // device-position QPC from IAudioCaptureClient::GetBuffer
    uint32_t frame_count;   // PCM frames in this packet
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bit_depth;     // bits per sample (from mix format)
    bool     silent;        // AUDCLNT_BUFFERFLAGS_SILENT — don't feed to encoder
    bool     is_float;      // true = IEEE 754 float; false = signed integer PCM
    uint32_t seq;

    // Raw PCM bytes.  Valid only for the duration of the callback; do not cache.
    // nullptr when silent.  Size = frame_count * channels * (bit_depth / 8).
    const uint8_t* data;
    uint32_t       data_bytes;
};

using PacketCallback = std::function<void(const PacketInfo&)>;

// Optional diagnostic log sink (tag, message). The host wires this to its own
// logger so audio-library diagnostics (e.g. process-loopback HRESULT failures)
// land in the app log. Pass nullptr to disable. Set once at startup.
void set_log_sink(std::function<void(const char* tag, const char* msg)> sink);

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    bool default_device = false;
    bool available = true;
};

struct ProcessInfo {
    uint32_t process_id = 0;
    std::wstring process_name;
    std::wstring display_name;
    std::wstring executable_path;
    std::wstring window_title;
    std::wstring window_class;
};

using ProcessAudioSessionInfo = ProcessInfo;

std::vector<DeviceInfo> enumerate_input_devices();
std::vector<ProcessAudioSessionInfo> enumerate_render_sessions();
ProcessInfo active_foreground_process();

// Config passed to detect_active_game() to drive blacklist/whitelist/confidence.
// All exe names in blacklist/whitelist/manual_games are compared case-insensitively.
struct DetectConfig {
    std::vector<std::wstring> blacklist;    // processes to reject unconditionally
    std::vector<std::wstring> whitelist;    // processes that receive a strong bonus
    std::vector<std::wstring> manual_games; // user-explicitly-chosen games (strong bonus)
    int min_confidence = 50;               // 0–100; candidates below this are discarded
    // When Monolith's own window (recorder or Settings) is in the foreground,
    // the real game loses its foreground bonus and may drop below
    // min_confidence — invalidating detection while Settings is open.  Set this
    // to the currently-tracked game pid so its foreground bonus stays "sticky"
    // while a Monolith window holds focus.  0 = no sticky fallback.
    uint32_t sticky_foreground_pid = 0;
};

// Extended result returned by the config-driven detect_active_game overload.
// confidence 0–100; reason is a human-readable string of applied scoring factors.
struct ActiveGameResult {
    ProcessInfo process;         // process_id == 0 when nothing qualifies
    int  score       = 0;
    int  confidence  = 0;        // 0–100
    std::string reason;
    bool has_session = false;
    bool fullscreen  = false;
};

// One running process whose executable is present in the local game-list DB.
// Detection is DB-gated: only DB-matched processes ever become candidates. The
// window facts (foreground/fullscreen/capture_window) only order candidates and
// select the capture target — they never gate membership. display_name /
// discord_app_id come from the DB (UTF-8).
struct GameCandidateInfo {
    ProcessInfo process;
    std::string display_name;
    std::string discord_app_id;
    bool     foreground = false;
    bool     fullscreen = false;
    bool     has_session = false;    // has an active audio render session
    HWND     capture_window = nullptr; // main window to capture, if any
};

// DB-gated detection: returns every running process whose exe basename is in the
// game-list DB (minus the user blacklist and built-in shell/self processes).
// Selection among candidates is the caller's job. Empty when nothing matches or
// the DB hasn't synced yet.
std::vector<GameCandidateInfo> detect_game_candidates(const DetectConfig& cfg);

// Config-driven active-game detection, now DB-gated: returns the single best
// candidate from detect_game_candidates (foreground > fullscreen > audio session
// > window area). process_id == 0 when nothing qualifies. The blacklist and
// sticky_foreground_pid in cfg are honored; whitelist/manual_games/min_confidence
// are inert (detection is purely DB membership now).
ActiveGameResult detect_active_game(const DetectConfig& cfg);

// Convenience overload: built-in defaults (shell/Monolith excluded). Returns the
// best DB-matched candidate, or an empty ProcessInfo when none is running.
inline ProcessInfo detect_active_game()
{
    return detect_active_game(DetectConfig{}).process;
}

// True while the process can be opened and has not exited.
bool process_alive(uint32_t process_id);

// WASAPI capture for one endpoint: loopback (system audio) or microphone.
// Runs a dedicated capture thread internally.
class WasapiCapture {
public:
    enum class Mode { Loopback, Microphone };

     WasapiCapture();
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&)            = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    // COM must be initialized on the calling thread before start().
    // cb is invoked from the internal capture thread — must be thread-safe.
    bool start(Mode mode, PacketCallback cb);
    bool start_device(Mode mode, const std::wstring& device_id, PacketCallback cb);
    bool start_process_loopback(uint32_t process_id, PacketCallback cb);
    void stop();
    bool running() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace audio
