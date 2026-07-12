#include "audio.h"

#include <platform-win/platform_win.h>
#include <gamelist/gamelist.h>

#include <tlhelp32.h>

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

static ProcessInfo process_info(uint32_t pid)
{
    ProcessInfo info;
    info.process_id = pid;
    if (pid == 0) return info;

    platform_win::ProcessInfo base = platform_win::process_info(pid);
    info.executable_path = base.executable_path;
    info.process_name = base.process_name;
    info.display_name = base.display_name;
    return info;
}

struct WindowIdentity {
    uint32_t process_id = 0;
    std::wstring title;
    std::wstring window_class;
};

static std::map<uint32_t, WindowIdentity> enumerate_capture_windows()
{
    std::map<uint32_t, WindowIdentity> result;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (!platform_win::is_capture_candidate_window(hwnd)) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

        auto* windows = reinterpret_cast<std::map<uint32_t, WindowIdentity>*>(param);
        if (windows->find(pid) != windows->end()) return TRUE;

        WindowIdentity identity;
        identity.process_id = pid;
        identity.title = platform_win::window_text(hwnd);
        identity.window_class = platform_win::window_class_name(hwnd);
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

// Per-pid window facts gathered in one EnumWindows pass. The "best" window per
// process is the largest visible unowned top-level window — the capture target.
struct WindowFacts {
    HWND         hwnd       = nullptr;
    int64_t      area       = 0;
    bool         fullscreen = false;
    std::wstring title;         // main-window title, for DB-keyword disambiguation
};

struct AnnotateContext {
    const std::vector<uint32_t>*             matched_pids = nullptr;
    std::map<uint32_t, WindowFacts>*         facts        = nullptr;
};

static BOOL CALLBACK annotate_window_enum_proc(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<AnnotateContext*>(lp);
    if (!window_is_app_candidate(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    // Only annotate PIDs we already matched against the game list.
    bool matched = false;
    for (uint32_t p : *ctx->matched_pids) {
        if (p == pid) { matched = true; break; }
    }
    if (!matched) return TRUE;

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int64_t area =
        static_cast<int64_t>(rc.right - rc.left) * (rc.bottom - rc.top);

    WindowFacts& wf = (*ctx->facts)[pid];
    if (area > wf.area) {
        wf.area       = area;
        wf.hwnd       = hwnd;
        wf.fullscreen = window_fills_monitor(hwnd);
        wchar_t buf[256];
        const int n = GetWindowTextW(hwnd, buf, 256);
        wf.title.assign(buf, n > 0 ? static_cast<size_t>(n) : 0);
    }
    return TRUE;
}

// True when `title` plausibly names the DB game `display_name`: the title
// contains the whole display name, or any of its alphanumeric tokens of length
// >= 3. Case-insensitive. Only used to disambiguate several processes sharing
// one executable; a loose match here just biases selection, never gates it.
static bool title_matches_db_name(const std::string& display_name_utf8,
                                  const std::wstring& window_title)
{
    if (display_name_utf8.empty() || window_title.empty()) return false;
    const std::wstring name  = to_lower(platform_win::utf8_to_wide(display_name_utf8));
    const std::wstring title = to_lower(window_title);
    if (name.empty()) return false;
    if (title.find(name) != std::wstring::npos) return true;

    std::wstring tok;
    for (wchar_t ch : name) {
        if (iswalnum(ch)) {
            tok.push_back(ch);
        } else {
            if (tok.size() >= 3 && title.find(tok) != std::wstring::npos) return true;
            tok.clear();
        }
    }
    return tok.size() >= 3 && title.find(tok) != std::wstring::npos;
}

std::vector<GameCandidateInfo> detect_game_candidates(const DetectConfig& cfg)
{
    std::vector<GameCandidateInfo> out;

    auto games = gamelist::snapshot();
    if (!games || games->empty()) return out; // DB not synced yet -> no games

    auto sessions = enumerate_render_sessions();
    auto has_audio_session = [&sessions](uint32_t pid) -> bool {
        for (const auto& s : sessions)
            if (s.process_id == pid) return true;
        return false;
    };

    HWND fg = GetForegroundWindow();
    DWORD fg_pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);

    // Sticky foreground: when our own recorder/Settings window holds focus,
    // substitute the caller-supplied live game pid so it keeps being marked
    // foreground while a Monolith window is up.
    if (fg_pid != 0 && cfg.sticky_foreground_pid != 0) {
        ProcessInfo fg_info = process_info(fg_pid);
        if (is_self_process(to_lower(fg_info.process_name)))
            fg_pid = cfg.sticky_foreground_pid;
    }

    // Enumerate ALL processes (not just windows) so alt-tabbed / loading games
    // still match. Gate each on game-list membership.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    const uint32_t self_pid = GetCurrentProcessId();
    std::vector<uint32_t> matched_pids;
    // Parallel to `out`: the full set of DB games sharing each candidate's exe,
    // used below to resolve the right game by window title when several share one
    // executable (e.g. javaw.exe). Points into `games`, which outlives this call.
    std::vector<const gamelist::GameList*> cand_entries;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const uint32_t pid = pe.th32ProcessID;
            if (pid == 0 || pid == self_pid) continue;

            std::wstring lower = to_lower(pe.szExeFile);
            if (is_builtin_shell_process(lower)) continue;

            bool blacklisted = false;
            for (const auto& bl : cfg.blacklist)
                if (lower == to_lower(bl)) { blacklisted = true; break; }
            if (blacklisted) continue;

            const std::string exe_key = platform_win::wide_to_utf8(lower);
            auto it = games->find(exe_key);
            if (it == games->end() || it->second.empty())
                continue; // DB gate: not a known game

            GameCandidateInfo ci;
            ci.process = process_info(pid);
            if (ci.process.process_name.empty())
                ci.process.process_name = pe.szExeFile;
            // Provisional identity: the first game registered for this exe. When
            // the exe is shared it is refined by window title after the window
            // pass below. Always a DB display name (Bug 2: never a prettified exe).
            ci.display_name   = it->second.front().display_name;
            ci.discord_app_id = it->second.front().discord_app_id;
            ci.has_session    = has_audio_session(pid);
            ci.foreground     = (pid == fg_pid);
            out.push_back(std::move(ci));
            matched_pids.push_back(pid);
            cand_entries.push_back(&it->second);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (out.empty()) return out;

    // One window pass to attach capture window + fullscreen facts.
    std::map<uint32_t, WindowFacts> facts;
    AnnotateContext ctx{ &matched_pids, &facts };
    EnumWindows(annotate_window_enum_proc, reinterpret_cast<LPARAM>(&ctx));

    for (size_t i = 0; i < out.size(); ++i) {
        GameCandidateInfo& ci = out[i];
        auto fit = facts.find(ci.process.process_id);
        if (fit != facts.end()) {
            ci.capture_window = fit->second.hwnd;
            ci.fullscreen     = fit->second.fullscreen;
            if (!fit->second.title.empty())
                ci.process.window_title = fit->second.title;
        }

        // Disambiguate shared executables (e.g. javaw.exe used by Minecraft,
        // Spiral Knights, ...): pick the game whose name matches the window
        // title. With a single registered game there is nothing to resolve; if
        // none matches, the provisional first game stands (normal behavior).
        const gamelist::GameList& entries = *cand_entries[i];
        if (entries.size() > 1 && !ci.process.window_title.empty()) {
            for (const auto& e : entries) {
                if (title_matches_db_name(e.display_name, ci.process.window_title)) {
                    ci.display_name     = e.display_name;
                    ci.discord_app_id   = e.discord_app_id;
                    ci.title_matches_db = true;
                    break;
                }
            }
        }
    }
    return out;
}

ActiveGameResult detect_active_game(const DetectConfig& cfg)
{
    ActiveGameResult best;

    auto candidates = detect_game_candidates(cfg);
    if (candidates.empty()) return best;

    auto rank = [](const GameCandidateInfo& c) -> int {
        return (c.foreground ? 4 : 0) + (c.fullscreen ? 2 : 0) + (c.has_session ? 1 : 0);
    };

    const GameCandidateInfo* pick = &candidates.front();
    for (const auto& c : candidates)
        if (rank(c) > rank(*pick)) pick = &c;

    best.process = pick->process;
    // The engine treats display_name as user-facing; ensure it is the DB name.
    if (!pick->display_name.empty())
        best.process.display_name = platform_win::utf8_to_wide(pick->display_name);
    best.confidence  = 100;
    best.reason      = "gamelist";
    best.has_session = pick->has_session;
    best.fullscreen  = pick->fullscreen;
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
