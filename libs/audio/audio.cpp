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
#include <cstdio>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 100-nanosecond units per second
static constexpr REFERENCE_TIME kReftimesPerSec = 10000000;

namespace audio {

// Diagnostic log sink

static std::function<void(const char*, const char*)> g_log_sink;

void set_log_sink(std::function<void(const char* tag, const char* msg)> sink)
{
    g_log_sink = std::move(sink);
}

static void audio_log(const char* tag, const char* msg)
{
    if (g_log_sink) g_log_sink(tag, msg);
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

// Impl

struct WasapiCapture::Impl {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    winrt::com_ptr<IMMDevice>           device;
    winrt::com_ptr<IAudioClient>        client;
    winrt::com_ptr<IAudioCaptureClient> cap_client;
    WAVEFORMATEX*                       mix_fmt  = nullptr;
    HANDLE                              event    = nullptr; // buffer-ready event (EVENTCALLBACK)

    std::thread           thread;
    std::atomic<bool>     running{ false };
    std::atomic<uint32_t> seq{ 0 };
    PacketCallback        cb;
};

// Capture thread

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT - {00000003-0000-0010-8000-00AA00389B71}
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

struct WindowIdentity {
    uint32_t process_id = 0;
    std::wstring title;
    std::wstring window_class;
};

static bool is_capture_candidate_window(HWND hwnd)
{
    if (!IsWindowVisible(hwnd)) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    if (GetWindowTextLengthW(hwnd) <= 0) return false;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_DISABLED) != 0) return false;

    LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex_style & WS_EX_TOOLWINDOW) != 0) return false;

    return true;
}

static std::wstring window_text(HWND hwnd)
{
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(hwnd, text.data(), len + 1);
    if (copied <= 0) return {};
    text.resize(static_cast<size_t>(copied));
    return text;
}

static std::wstring window_class_name(HWND hwnd)
{
    wchar_t buf[256] = {};
    int copied = GetClassNameW(hwnd, buf, static_cast<int>(_countof(buf)));
    if (copied <= 0) return {};
    return std::wstring(buf, static_cast<size_t>(copied));
}

static std::map<uint32_t, WindowIdentity> enumerate_capture_windows()
{
    std::map<uint32_t, WindowIdentity> result;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (!is_capture_candidate_window(hwnd)) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

        auto* windows = reinterpret_cast<std::map<uint32_t, WindowIdentity>*>(param);
        if (windows->find(pid) != windows->end()) return TRUE;

        WindowIdentity identity;
        identity.process_id = pid;
        identity.title = window_text(hwnd);
        identity.window_class = window_class_name(hwnd);
        if (!identity.title.empty() && !identity.window_class.empty())
            windows->emplace(pid, std::move(identity));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

// ActivateAudioInterfaceAsync rejects non-agile completion handlers with
// E_ILLEGAL_METHOD_CALL for process-loopback activation.
class ActivateCompletionHandler final
    : public IActivateAudioInterfaceCompletionHandler
    , public IAgileObject {
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
        if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject*>(this);
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
        // Event-driven: block until WASAPI signals a buffer is ready (no busy
        // polling).  A 200 ms timeout bounds shutdown latency and guards against
        // a missed signal.  Loopback streams only raise the event while audio is
        // flowing, so the timeout also lets us re-check `running` during silence.
        if (impl->event)
            WaitForSingleObject(impl->event, 200);

        UINT32 packet_size = 0;
        HRESULT hr = impl->cap_client->GetNextPacketSize(&packet_size);
        if (FAILED(hr)) {
            OutputDebugStringA("[audio] GetNextPacketSize failed - stopping\n");
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

        // Fallback pacing when no event handle is available (should not happen
        // with EVENTCALLBACK, but keeps the loop from spinning if it ever is).
        if (!impl->event)
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

// Append the active audio sessions of a single render endpoint to `result`,
// skipping PIDs already present in `seen` (deduplication across endpoints).
static void enumerate_sessions_for_device(IMMDevice* device,
                                          std::vector<ProcessAudioSessionInfo>& result,
                                          std::vector<uint32_t>& seen,
                                          const std::map<uint32_t, WindowIdentity>& windows)
{
    winrt::com_ptr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                manager.put_void())))
        return;

    winrt::com_ptr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(sessions.put())))
        return;

    int count = 0;
    sessions->GetCount(&count);
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
        auto window_it = windows.find(pid);
        if (window_it != windows.end()) {
            info.window_title = window_it->second.title;
            info.window_class = window_it->second.window_class;
            if (!info.window_title.empty())
                info.display_name = info.window_title;
        }
        LPWSTR display = nullptr;
        if (info.display_name.empty() &&
            SUCCEEDED(session->GetDisplayName(&display)) && display && display[0])
            info.display_name = display;
        if (display) CoTaskMemFree(display);
        if (info.display_name.empty())
            info.display_name = info.process_name.empty()
                ? (L"PID " + std::to_wstring(pid))
                : info.process_name;
        result.push_back(std::move(info));
    }
}

std::vector<ProcessAudioSessionInfo> enumerate_render_sessions()
{
    std::vector<ProcessAudioSessionInfo> result;
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), enumerator.put_void());
    if (FAILED(hr)) return result;

    // Enumerate sessions on every active render endpoint, not just the default
    // one: an app (e.g. Discord) may render to a non-default output device, and
    // its session would otherwise never appear in the source list.
    winrt::com_ptr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put())))
        return result;

    UINT count = 0;
    devices->GetCount(&count);
    std::vector<uint32_t> seen;
    auto windows = enumerate_capture_windows();
    for (UINT i = 0; i < count; ++i) {
        winrt::com_ptr<IMMDevice> device;
        if (FAILED(devices->Item(i, device.put()))) continue;
        enumerate_sessions_for_device(device.get(), result, seen, windows);
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

bool process_alive(uint32_t process_id)
{
    if (process_id == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return false;
    DWORD exit_code = 0;
    bool alive = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

// Active-game detection

static std::wstring to_lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// Monolith's own processes (recorder + Settings sidecar).  Used so that opening
// the Settings window does not steal the foreground bonus from the live game,
// and shared with the shell/system exclusion list below.
static bool is_self_process(const std::wstring& lower_name)
{
    return lower_name == L"monolith.exe" || lower_name == L"monolith.settings.exe";
}

// Built-in shell/system process names that are always excluded regardless of user config.
static bool is_builtin_shell_process(const std::wstring& lower_name)
{
    static const wchar_t* kBuiltinShell[] = {
        L"explorer.exe", L"searchhost.exe", L"startmenuexperiencehost.exe",
        L"shellexperiencehost.exe", L"textinputhost.exe", L"taskmgr.exe",
        L"systemsettings.exe", L"lockapp.exe", L"dwm.exe",
    };
    if (is_self_process(lower_name)) return true;
    for (const wchar_t* name : kBuiltinShell) {
        if (lower_name == name) return true;
    }
    return false;
}

// Visible, unowned, non-tool top-level window of plausible app size.
static bool window_is_app_candidate(HWND hwnd)
{
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
    LONG ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_TOOLWINDOW) return false;
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return false;
    return (rc.right - rc.left) >= 320 && (rc.bottom - rc.top) >= 240;
}

// Window covers its entire monitor (borderless/exclusive fullscreen pattern).
static bool window_fills_monitor(HWND hwnd)
{
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return false;
    return rc.left   <= mi.rcMonitor.left  &&
           rc.top    <= mi.rcMonitor.top   &&
           rc.right  >= mi.rcMonitor.right &&
           rc.bottom >= mi.rcMonitor.bottom;
}

struct GameCandidate {
    uint32_t pid        = 0;
    int      score      = 0;
    int64_t  area       = 0;
    bool     fullscreen = false;
};

static BOOL CALLBACK game_window_enum_proc(HWND hwnd, LPARAM lp)
{
    auto* candidates = reinterpret_cast<std::vector<GameCandidate>*>(lp);
    if (!window_is_app_candidate(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    GameCandidate cand;
    cand.pid       = pid;
    cand.area      = static_cast<int64_t>(rc.right - rc.left) * (rc.bottom - rc.top);
    cand.fullscreen = window_fills_monitor(hwnd);
    candidates->push_back(cand);
    return TRUE;
}

// Scoring constants (raw points, converted to confidence 0-100 as min(100, score*10)).
// Max natural score without whitelist: 4(fullscreen) + 4(foreground) + 2(session) = 10 -> 100%
static constexpr int kScoreFullscreen = 4;
static constexpr int kScoreForeground = 4;
static constexpr int kScoreSession    = 2;
static constexpr int kScoreWhitelist  = 8; // user-explicit: guaranteed to win over heuristic

ActiveGameResult detect_active_game(const DetectConfig& cfg)
{
    auto sessions = enumerate_render_sessions();
    auto has_audio_session = [&sessions](uint32_t pid) -> bool {
        for (const auto& s : sessions)
            if (s.process_id == pid) return true;
        return false;
    };

    HWND fg = GetForegroundWindow();
    DWORD fg_pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);

    // Sticky foreground: when our own recorder/Settings window holds focus
    // (e.g. the user opened Settings, which defocuses the game), substitute the
    // caller-supplied live game pid so it keeps its foreground bonus and does
    // not drop below min_confidence while Settings is open.
    if (fg_pid != 0 && cfg.sticky_foreground_pid != 0) {
        ProcessInfo fg_info = process_info(fg_pid);
        if (is_self_process(to_lower(fg_info.process_name)))
            fg_pid = cfg.sticky_foreground_pid;
    }

    // Enumerate plausible top-level windows.
    std::vector<GameCandidate> raw_candidates;
    EnumWindows(game_window_enum_proc, reinterpret_cast<LPARAM>(&raw_candidates));

    ActiveGameResult best;
    int64_t best_area = 0;
    for (auto& cand : raw_candidates) {
        ProcessInfo info = process_info(cand.pid);
        if (info.process_name.empty()) continue;

        std::wstring lower_name = to_lower(info.process_name);

        // Always reject built-in shell/system processes.
        if (is_builtin_shell_process(lower_name)) continue;

        // Reject user-configured blacklist (case-insensitive).
        bool blacklisted = false;
        for (const auto& bl : cfg.blacklist) {
            if (lower_name == to_lower(bl)) { blacklisted = true; break; }
        }
        if (blacklisted) continue;

        // Compute score with explicit bonuses.
        int score = 0;
        std::string reason_parts;
        bool is_session  = has_audio_session(cand.pid);
        bool is_fg       = (cand.pid == fg_pid);

        if (cand.fullscreen) { score += kScoreFullscreen; reason_parts += "fullscreen+"; }
        if (is_fg)           { score += kScoreForeground; reason_parts += "foreground+"; }
        if (is_session)      { score += kScoreSession;    reason_parts += "session+"; }

        // Whitelist/manual bonus: explicit user choice wins over pure heuristic.
        bool user_explicit = false;
        for (const auto& wl : cfg.whitelist) {
            if (lower_name == to_lower(wl)) { user_explicit = true; break; }
        }
        if (!user_explicit) {
            for (const auto& mg : cfg.manual_games) {
                if (lower_name == to_lower(mg)) { user_explicit = true; break; }
            }
        }
        if (user_explicit) { score += kScoreWhitelist; reason_parts += "whitelisted+"; }

        // Must have at least one positive signal to be a candidate at all.
        if (score <= 0) continue;

        // Clean up trailing '+'.
        if (!reason_parts.empty() && reason_parts.back() == '+')
            reason_parts.pop_back();

        int confidence = std::min(100, score * 10);
        if (confidence < cfg.min_confidence) continue;

        if (score > best.score ||
            (score == best.score && cand.area > best_area)) {
            best_area        = cand.area;
            best.process     = std::move(info);
            best.score       = score;
            best.confidence  = confidence;
            best.reason      = std::move(reason_parts);
            best.has_session = is_session;
            best.fullscreen  = cand.fullscreen;
        }
    }

    // If nothing passed confidence with window scoring, try bare foreground process
    // (only when min_confidence is low enough to accept it - score would be foreground only = 40).
    if (best.process.process_id == 0 && fg_pid != 0 && fg_pid != GetCurrentProcessId()) {
        ProcessInfo fg_info = process_info(fg_pid);
        if (!fg_info.process_name.empty()) {
            std::wstring lower_name = to_lower(fg_info.process_name);
            bool blacklisted = is_builtin_shell_process(lower_name);
            if (!blacklisted) {
                for (const auto& bl : cfg.blacklist)
                    if (lower_name == to_lower(bl)) { blacklisted = true; break; }
            }
            if (!blacklisted) {
                int score = kScoreForeground;
                if (has_audio_session(fg_pid)) score += kScoreSession;
                int confidence = std::min(100, score * 10);
                if (confidence >= cfg.min_confidence) {
                    best.process    = std::move(fg_info);
                    best.score      = score;
                    best.confidence = confidence;
                    best.reason     = "foreground-fallback";
                    best.has_session = has_audio_session(fg_pid);
                    best.fullscreen  = false;
                }
            }
        }
    }

    return best;
}

// WasapiCapture

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
    flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = impl_->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        flags,
        kReftimesPerSec / 10, // 100ms buffer
        0,
        impl_->mix_fmt,
        nullptr);
    if (FAILED(hr)) return false;

    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->event) return false;
    hr = impl_->client->SetEventHandle(impl_->event);
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
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "process loopback: ActivateAudioInterfaceAsync failed (hr=0x%08lX)",
                 static_cast<unsigned long>(hr));
        audio_log("audio.app", buf);
        handler->Release();
        CloseHandle(done);
        return false;
    }

    DWORD wait_result = WaitForSingleObject(done, 5000);
    if (wait_result != WAIT_OBJECT_0 || FAILED(handler->activate_hr())) {
        char buf[112];
        snprintf(buf, sizeof(buf),
                 "process loopback: activation %s (hr=0x%08lX)",
                 wait_result != WAIT_OBJECT_0 ? "timed out" : "failed",
                 static_cast<unsigned long>(handler->activate_hr()));
        audio_log("audio.app", buf);
        handler->Release();
        CloseHandle(done);
        return false;
    }

    winrt::com_ptr<IUnknown> activated = handler->activated();
    handler->Release();
    CloseHandle(done);

    if (!activated) {
        audio_log("audio.app", "process loopback: activation returned no interface");
        return false;
    }
    impl_->client = activated.as<IAudioClient>();
    if (!impl_->client) {
        audio_log("audio.app", "process loopback: IAudioClient query failed");
        return false;
    }

    // OBS-style process loopback: the virtual device has no reliable
    // GetMixFormat path, so we provide the capture format up front, then use
    // WASAPI event callbacks to wake only when process audio is available.
    auto* wf = static_cast<WAVEFORMATEXTENSIBLE*>(
        CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!wf) return false;
    ZeroMemory(wf, sizeof(*wf));
    wf->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wf->Format.nChannels       = 2;
    wf->Format.nSamplesPerSec  = 48000;
    wf->Format.wBitsPerSample  = 32;
    wf->Format.nBlockAlign     = static_cast<WORD>(
        wf->Format.nChannels * wf->Format.wBitsPerSample / 8);
    wf->Format.nAvgBytesPerSec = wf->Format.nSamplesPerSec * wf->Format.nBlockAlign;
    wf->Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf->Samples.wValidBitsPerSample = 32;
    wf->dwChannelMask          = 0x3; // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
    wf->SubFormat              = kSubTypeFloat; // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    impl_->mix_fmt = reinterpret_cast<WAVEFORMATEX*>(wf);

    // Use WASAPI event callbacks like OBS, not timer polling.
    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = impl_->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        flags,
        5 * kReftimesPerSec,
        0,
        impl_->mix_fmt,
        nullptr);
    if (FAILED(hr)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "process loopback: Initialize failed (hr=0x%08lX)",
                 static_cast<unsigned long>(hr));
        audio_log("audio.app", buf);
        stop();
        return false;
    }

    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->event) {
        audio_log("audio.app", "process loopback: CreateEvent failed");
        stop();
        return false;
    }

    hr = impl_->client->SetEventHandle(impl_->event);
    if (FAILED(hr)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "process loopback: SetEventHandle failed (hr=0x%08lX)",
                 static_cast<unsigned long>(hr));
        audio_log("audio.app", buf);
        stop();
        return false;
    }

    hr = impl_->client->GetService(
        __uuidof(IAudioCaptureClient),
        impl_->cap_client.put_void());
    if (FAILED(hr)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "process loopback: GetService failed (hr=0x%08lX)",
                 static_cast<unsigned long>(hr));
        audio_log("audio.app", buf);
        stop();
        return false;
    }

    hr = impl_->client->Start();
    if (FAILED(hr)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "process loopback: Start failed (hr=0x%08lX)",
                 static_cast<unsigned long>(hr));
        audio_log("audio.app", buf);
        stop();
        return false;
    }

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

    if (impl_->event) {
        CloseHandle(impl_->event);
        impl_->event = nullptr;
    }

    impl_->cap_client = nullptr;
    impl_->client     = nullptr;
    impl_->device     = nullptr;
    impl_->enumerator = nullptr;
    impl_->cb         = nullptr;
    impl_->seq.store(0, std::memory_order_relaxed);
}

} // namespace audio
