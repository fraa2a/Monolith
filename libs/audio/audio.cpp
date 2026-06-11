#include "audio.h"

#include <winrt/base.h>     // winrt::com_ptr, check_hresult

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>     // WAVE_FORMAT_IEEE_FLOAT, WAVE_FORMAT_EXTENSIBLE

#include <atomic>
#include <thread>

// 100-nanosecond units per second
static constexpr REFERENCE_TIME kReftimesPerSec = 10000000;

namespace audio {

// ── Impl ──────────────────────────────────────────────────────────────────────

struct WasapiCapture::Impl {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    winrt::com_ptr<IMMDevice>           device;
    winrt::com_ptr<IAudioClient>        client;
    winrt::com_ptr<IAudioCaptureClient> cap_client;
    WAVEFORMATEX*                       mix_fmt  = nullptr;

    std::thread           thread;
    std::atomic<bool>     running{ false };
    std::atomic<uint32_t> seq{ 0 };
    PacketCallback        cb;
};

// ── Capture thread ────────────────────────────────────────────────────────────

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT — {00000003-0000-0010-8000-00AA00389B71}
static const GUID kSubTypeFloat =
    { 0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71} };

static bool detect_float(const WAVEFORMATEX* fmt)
{
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return (ext->SubFormat == kSubTypeFloat);
    }
    return false;
}

static void capture_thread(WasapiCapture::Impl* impl)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const bool     is_float  = detect_float(impl->mix_fmt);
    const uint16_t bit_depth = impl->mix_fmt->wBitsPerSample;

    while (impl->running.load(std::memory_order_acquire)) {
        UINT32 packet_size = 0;
        HRESULT hr = impl->cap_client->GetNextPacketSize(&packet_size);
        if (FAILED(hr)) {
            OutputDebugStringA("[audio] GetNextPacketSize failed — stopping\n");
            break;
        }

        while (packet_size > 0) {
            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            UINT64 qpc    = 0;

            hr = impl->cap_client->GetBuffer(&data, &frames, &flags, nullptr, &qpc);
            if (FAILED(hr)) break;

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const uint32_t bytes = frames
                * impl->mix_fmt->nChannels
                * (bit_depth / 8u);

            PacketInfo pkt{};
            pkt.timestamp_qpc = static_cast<int64_t>(qpc);
            pkt.frame_count   = frames;
            pkt.sample_rate   = impl->mix_fmt->nSamplesPerSec;
            pkt.channels      = static_cast<uint16_t>(impl->mix_fmt->nChannels);
            pkt.bit_depth     = bit_depth;
            pkt.silent        = silent;
            pkt.is_float      = is_float;
            pkt.seq           = impl->seq.fetch_add(1, std::memory_order_relaxed);
            pkt.data          = silent ? nullptr : reinterpret_cast<const uint8_t*>(data);
            pkt.data_bytes    = silent ? 0u : bytes;

            if (impl->cb) impl->cb(pkt);

            impl->cap_client->ReleaseBuffer(frames);
            impl->cap_client->GetNextPacketSize(&packet_size);
        }

        Sleep(10);
    }

    CoUninitialize();
}

// ── WasapiCapture ─────────────────────────────────────────────────────────────

WasapiCapture::WasapiCapture() : impl_(new Impl()) {}
WasapiCapture::~WasapiCapture() { stop(); delete impl_; }
bool WasapiCapture::running() const { return impl_->running.load(std::memory_order_relaxed); }

bool WasapiCapture::start(Mode mode, PacketCallback cb)
{
    if (impl_->running) return false;
    impl_->cb = std::move(cb);

    HRESULT hr;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        impl_->enumerator.put_void());
    if (FAILED(hr)) return false;

    // Loopback: render endpoint + AUDCLNT_STREAMFLAGS_LOOPBACK
    // Microphone: capture endpoint, no extra flags
    EDataFlow flow = (mode == Mode::Loopback) ? eRender : eCapture;
    hr = impl_->enumerator->GetDefaultAudioEndpoint(
        flow, eConsole, impl_->device.put());
    if (FAILED(hr)) return false;

    hr = impl_->device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        impl_->client.put_void());
    if (FAILED(hr)) return false;

    hr = impl_->client->GetMixFormat(&impl_->mix_fmt);
    if (FAILED(hr)) return false;

    DWORD flags = (mode == Mode::Loopback) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = impl_->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        flags,
        kReftimesPerSec / 10, // 100ms buffer
        0,
        impl_->mix_fmt,
        nullptr);
    if (FAILED(hr)) return false;

    hr = impl_->client->GetService(
        __uuidof(IAudioCaptureClient),
        impl_->cap_client.put_void());
    if (FAILED(hr)) return false;

    hr = impl_->client->Start();
    if (FAILED(hr)) return false;

    impl_->running = true;
    impl_->thread  = std::thread(capture_thread, impl_);
    return true;
}

void WasapiCapture::stop()
{
    if (!impl_->running) return;

    impl_->running.store(false, std::memory_order_release);
    if (impl_->thread.joinable()) impl_->thread.join();

    if (impl_->client) impl_->client->Stop();

    if (impl_->mix_fmt) {
        CoTaskMemFree(impl_->mix_fmt);
        impl_->mix_fmt = nullptr;
    }

    impl_->cap_client = nullptr;
    impl_->client     = nullptr;
    impl_->device     = nullptr;
    impl_->enumerator = nullptr;
    impl_->cb         = nullptr;
    impl_->seq.store(0, std::memory_order_relaxed);
}

} // namespace audio
