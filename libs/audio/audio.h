#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <functional>

namespace audio {

struct PacketInfo {
    int64_t  timestamp_qpc; // device-position QPC from IAudioCaptureClient::GetBuffer
    uint32_t frame_count;   // PCM frames in this packet
    uint32_t sample_rate;
    uint16_t channels;
    bool     silent;        // AUDCLNT_BUFFERFLAGS_SILENT — don't feed to encoder
    uint32_t seq;
};

using PacketCallback = std::function<void(const PacketInfo&)>;

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
    void stop();
    bool running() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace audio
