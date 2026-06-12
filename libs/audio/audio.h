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
};

using ProcessAudioSessionInfo = ProcessInfo;

std::vector<DeviceInfo> enumerate_input_devices();
std::vector<ProcessAudioSessionInfo> enumerate_render_sessions();
ProcessInfo active_foreground_process();

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
