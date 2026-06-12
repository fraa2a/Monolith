#include "audio.h"

#include <winrt/base.h>     // winrt::com_ptr, check_hresult

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>     // WAVE_FORMAT_IEEE_FLOAT, WAVE_FORMAT_EXTENSIBLE
#include <propsys.h>
#include <propvarutil.h>
#include <psapi.h>

#include <atomic>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

static std::wstring device_id(IMMDevice* device)
{
    if (!device) return {};
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw)) || !raw) return {};
    std::wstring id(raw);
    CoTaskMemFree(raw);
    return id;
}

static std::wstring device_name(IMMDevice* device)
{
    if (!device) return {};
    winrt::com_ptr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, props.put()))) return {};

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

static std::wstring file_name_from_path(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

static ProcessInfo process_info(uint32_t pid)
{
    ProcessInfo info;
    info.process_id = pid;
    if (pid == 0) return info;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return info;

    wchar_t path[MAX_PATH * 4] = {};
    DWORD size = static_cast<DWORD>(_countof(path));
    if (QueryFullProcessImageNameW(process, 0, path, &size) && size > 0) {
        info.executable_path.assign(path, size);
        info.process_name = file_name_from_path(info.executable_path);
        info.display_name = info.process_name;
    }
    CloseHandle(process);
    return info;
}

class ActivateCompletionHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit ActivateCompletionHandler(HANDLE done) : done_(done) {}

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ref_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG ref = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (ref == 0) delete this;
        return ref;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
    {
        IUnknown* raw = nullptr;
        if (operation)
            operation->GetActivateResult(&activate_hr_, &raw);
        activated_.attach(raw);
        SetEvent(done_);
        return S_OK;
    }

    HRESULT activate_hr() const { return activate_hr_; }
    winrt::com_ptr<IUnknown> activated() const { return activated_; }

private:
    std::atomic<ULONG> ref_{1};
    HANDLE done_ = nullptr;
    HRESULT activate_hr_ = E_FAIL;
    winrt::com_ptr<IUnknown> activated_;
};

template <typename ImplT>
static void capture_thread(ImplT* impl)
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
            pkt.data_bytes    = bytes;

            if (impl->cb) impl->cb(pkt);

            impl->cap_client->ReleaseBuffer(frames);
            impl->cap_client->GetNextPacketSize(&packet_size);
        }

        Sleep(10);
    }

    CoUninitialize();
}

std::vector<DeviceInfo> enumerate_input_devices()
{
    std::vector<DeviceInfo> result;
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), enumerator.put_void());
    if (FAILED(hr)) return result;

    std::wstring default_id;
    winrt::com_ptr<IMMDevice> default_device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, default_device.put())))
        default_id = device_id(default_device.get());

    winrt::com_ptr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, devices.put())))
        return result;

    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        winrt::com_ptr<IMMDevice> device;
        if (FAILED(devices->Item(i, device.put()))) continue;
        DeviceInfo info;
        info.id = device_id(device.get());
        info.name = device_name(device.get());
        if (info.name.empty()) info.name = info.id;
        info.default_device = !default_id.empty() && info.id == default_id;
        info.available = true;
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<ProcessAudioSessionInfo> enumerate_render_sessions()
{
    std::vector<ProcessAudioSessionInfo> result;
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), enumerator.put_void());
    if (FAILED(hr)) return result;

    winrt::com_ptr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put())))
        return result;

    winrt::com_ptr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                manager.put_void())))
        return result;

    winrt::com_ptr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(sessions.put())))
        return result;

    int count = 0;
    sessions->GetCount(&count);
    std::vector<uint32_t> seen;
    for (int i = 0; i < count; ++i) {
        winrt::com_ptr<IAudioSessionControl> session;
        if (FAILED(sessions->GetSession(i, session.put()))) continue;

        winrt::com_ptr<IAudioSessionControl2> session2;
        if (FAILED(session->QueryInterface(__uuidof(IAudioSessionControl2),
                                           session2.put_void())))
            continue;

        DWORD pid = 0;
        if (FAILED(session2->GetProcessId(&pid)) || pid == 0) continue;
        if (std::find(seen.begin(), seen.end(), pid) != seen.end()) continue;
        seen.push_back(pid);

        ProcessAudioSessionInfo info = process_info(pid);
        LPWSTR display = nullptr;
        if (SUCCEEDED(session->GetDisplayName(&display)) && display && display[0])
            info.display_name = display;
        if (display) CoTaskMemFree(display);
        if (info.display_name.empty())
            info.display_name = info.process_name.empty()
                ? (L"PID " + std::to_wstring(pid))
                : info.process_name;
        result.push_back(std::move(info));
    }
    return result;
}

ProcessInfo active_foreground_process()
{
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return {};

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return process_info(pid);
}

// ── WasapiCapture ─────────────────────────────────────────────────────────────

WasapiCapture::WasapiCapture() : impl_(new Impl()) {}
WasapiCapture::~WasapiCapture() { stop(); delete impl_; }
bool WasapiCapture::running() const { return impl_->running.load(std::memory_order_relaxed); }

bool WasapiCapture::start(Mode mode, PacketCallback cb)
{
    return start_device(mode, L"", std::move(cb));
}

bool WasapiCapture::start_device(Mode mode, const std::wstring& device_id_value, PacketCallback cb)
{
    if (impl_->running) return false;
    impl_->cb = std::move(cb);

    HRESULT hr;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        impl_->enumerator.put_void());
    if (FAILED(hr)) return false;

    // Loopback: render endpoint + AUDCLNT_STREAMFLAGS_LOOPBACK.
    // Microphone: capture endpoint, no extra flags.
    if (device_id_value.empty()) {
        EDataFlow flow = (mode == Mode::Loopback) ? eRender : eCapture;
        hr = impl_->enumerator->GetDefaultAudioEndpoint(
            flow, eConsole, impl_->device.put());
        if (FAILED(hr)) return false;
    } else {
        hr = impl_->enumerator->GetDevice(device_id_value.c_str(), impl_->device.put());
        if (FAILED(hr)) return false;
    }

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
    impl_->thread  = std::thread([impl = impl_]() { capture_thread(impl); });
    return true;
}

bool WasapiCapture::start_process_loopback(uint32_t process_id, PacketCallback cb)
{
    if (process_id == 0 || impl_->running) return false;
    impl_->cb = std::move(cb);

    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!done) return false;

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = process_id;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT prop;
    PropVariantInit(&prop);
    prop.vt = VT_BLOB;
    prop.blob.cbSize = sizeof(params);
    prop.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivateCompletionHandler(done);
    winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &prop,
        handler,
        operation.put());

    if (FAILED(hr)) {
        handler->Release();
        CloseHandle(done);
        return false;
    }

    DWORD wait_result = WaitForSingleObject(done, 5000);
    if (wait_result != WAIT_OBJECT_0 || FAILED(handler->activate_hr())) {
        handler->Release();
        CloseHandle(done);
        return false;
    }

    winrt::com_ptr<IUnknown> activated = handler->activated();
    handler->Release();
    CloseHandle(done);

    if (!activated) return false;
    impl_->client = activated.as<IAudioClient>();
    if (!impl_->client) return false;

    hr = impl_->client->GetMixFormat(&impl_->mix_fmt);
    if (FAILED(hr)) return false;

    hr = impl_->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        kReftimesPerSec / 10,
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
    impl_->thread = std::thread([impl = impl_]() { capture_thread(impl); });
    return true;
}

void WasapiCapture::stop()
{
    const bool was_running = impl_->running.exchange(false, std::memory_order_acq_rel);
    if (impl_->thread.joinable()) impl_->thread.join();

    if (was_running && impl_->client) impl_->client->Stop();

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
