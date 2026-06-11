#include "audio.h"

#include <winrt/base.h>     // winrt::com_ptr, check_hresult

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

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

static void capture_thread(WasapiCapture::Impl* impl)
{
    // Each audio thread owns its COM context.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

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

            PacketInfo pkt{};
            pkt.timestamp_qpc = static_cast<int64_t>(qpc);
            pkt.frame_count   = frames;
            pkt.sample_rate   = impl->mix_fmt->nSamplesPerSec;
            pkt.channels      = impl->mix_fmt->nChannels;
            pkt.silent        = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            pkt.seq           = impl->seq.fetch_add(1, std::memory_order_relaxed);

            if (impl->cb) impl->cb(pkt);

            impl->cap_client->ReleaseBuffer(frames);
            impl->cap_client->GetNextPacketSize(&packet_size);
        }

        // ~10ms poll — low overhead, adequate latency for a spike.
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
