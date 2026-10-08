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
    bool     silent;        // AUDCLNT_BUFFERFLAGS_SILENT - don't feed to encoder
    bool     is_float;      // true = IEEE 754 float; false = signed integer PCM
    uint32_t seq;

    // Raw PCM bytes.  Valid only for the duration of the callback; do not cache.
    // nullptr when silent.  Size = frame_count * channels * (bit_depth / 8).
    const uint8_t* data;
    uint32_t       data_bytes;
};

using PacketCallback = std::function<void(const PacketInfo&)>;

// Set the diagnostic sink once before starting capture.
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
// Blacklist names are matched case-insensitively; legacy whitelist fields are inert.
struct DetectConfig {
    std::vector<std::wstring> blacklist;    // processes to reject unconditionally
    std::vector<std::wstring> whitelist;    // legacy input, ignored by database detection
    std::vector<std::wstring> manual_games; // legacy input, ignored by database detection
    int min_confidence = 50;               // legacy input, ignored by database detection
    // Retain the tracked game foreground bonus while Monolith has focus; 0 disables it.
    uint32_t sticky_foreground_pid = 0;
};

// Extended result returned by the config-driven detect_active_game overload.
// confidence 0-100; reason is a human-readable string of applied scoring factors.
struct ActiveGameResult {
    ProcessInfo process;         // process_id == 0 when nothing qualifies
    int  score       = 0;
    int  confidence  = 0;        // 0-100
    std::string reason;
    bool has_session = false;
    bool fullscreen  = false;
};

// Candidates require local database membership. Window facts rank them;
// display names and app IDs come from the database as UTF-8.
struct GameCandidateInfo {
    ProcessInfo process;
    std::string display_name;
    std::string discord_app_id;
    bool     foreground = false;
    bool     fullscreen = false;
    bool     has_session = false;    // has an active audio render session
    HWND     capture_window = nullptr; // main window to capture, if any
    // Marks shared-executable identity resolved through a window-title match.
    bool     title_matches_db = false;
};

// Return database-matched processes minus blacklist/self/shell entries; callers select the target.
std::vector<GameCandidateInfo> detect_game_candidates(const DetectConfig& cfg);

// Rank database candidates by foreground, fullscreen, audio and area.
// A zero process_id means no target; legacy confidence/whitelist fields are inert.
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
    // cb runs on the capture thread; synchronize its shared state.
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
