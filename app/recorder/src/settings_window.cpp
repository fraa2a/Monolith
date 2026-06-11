#include "settings_window.h"

#include <shlobj.h>

#include <cwchar>
#include <vector>

namespace settings_window {
namespace {

constexpr wchar_t kClassName[] = L"MonolithSettingsWindow";

constexpr COLORREF kBg = RGB(18, 18, 18);
constexpr COLORREF kSidebar = RGB(30, 30, 30);
constexpr COLORREF kPanel = RGB(42, 42, 42);
constexpr COLORREF kText = RGB(245, 245, 245);

enum ControlId : int {
    IDC_NAV_BASIC = 2001,
    IDC_NAV_OUTPUT,
    IDC_NAV_HOTKEYS,
    IDC_NAV_CAPTURE,
    IDC_NAV_AUDIO,
    IDC_NAV_ENCODING,
    IDC_NAV_ADVANCED,
    IDC_CLIPS,
    IDC_RECORDINGS,
    IDC_REPLAY_DURATION,
    IDC_MEMORY_BUDGET,
    IDC_HK_SAVE_REPLAY,
    IDC_HK_RECORDING_START,
    IDC_HK_RECORDING_STOP,
    IDC_HK_PAUSE_RESUME,
    IDC_BROWSE_CLIPS,
    IDC_BROWSE_RECORDINGS,
    IDC_SAVE,
    IDC_CANCEL,
};

enum class Page {
    Basic,
    Output,
    Hotkeys,
};

struct State {
    Model model;
    SaveCallback on_save;
    Page page = Page::Basic;
    bool dirty = false;
    bool creating = false;

    HWND title = nullptr;
    HWND subtitle = nullptr;

    std::vector<HWND> basic_controls;
    std::vector<HWND> output_controls;
    std::vector<HWND> hotkey_controls;

    HWND clips = nullptr;
    HWND recordings = nullptr;
    HWND replay_duration = nullptr;
    HWND memory_budget = nullptr;

    HBRUSH bg_brush = nullptr;
    HBRUSH sidebar_brush = nullptr;
    HBRUSH panel_brush = nullptr;
};

HWND g_window = nullptr;

void set_font(HWND hwnd)
{
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

State* state_from(HWND hwnd)
{
    return reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void add(std::vector<HWND>& controls, HWND hwnd)
{
    if (hwnd) controls.push_back(hwnd);
}

HWND make_static(HWND parent, const wchar_t* text, int x, int y, int w, int h)
{
    HWND control = CreateWindowW(
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE,
        x,
        y,
        w,
        h,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (control) set_font(control);
    return control;
}

HWND make_group(HWND parent, const wchar_t* text, int x, int y, int w, int h)
{
    HWND control = CreateWindowW(
        L"BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x,
        y,
        w,
        h,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (control) set_font(control);
    return control;
}

HWND make_button(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, bool enabled = true)
{
    HWND control = CreateWindowW(
        L"BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x,
        y,
        w,
        h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (control) {
        set_font(control);
        EnableWindow(control, enabled ? TRUE : FALSE);
    }
    return control;
}

HWND make_edit(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, DWORD style)
{
    HWND control = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        w,
        h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (control) set_font(control);
    return control;
}

std::wstring get_text(HWND hwnd)
{
    int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return {};
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
    GetWindowTextW(hwnd, buffer.data(), static_cast<int>(buffer.size()));
    return std::wstring(buffer.data());
}

void set_text(HWND hwnd, const std::wstring& text)
{
    SetWindowTextW(hwnd, text.c_str());
}

void set_visible(const std::vector<HWND>& controls, bool visible)
{
    for (HWND control : controls)
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

void set_page(HWND hwnd, State* state, Page page)
{
    state->page = page;
    set_visible(state->basic_controls, page == Page::Basic);
    set_visible(state->output_controls, page == Page::Output);
    set_visible(state->hotkey_controls, page == Page::Hotkeys);

    switch (page) {
    case Page::Basic:
        set_text(state->title, L"Settings / Basic");
        set_text(state->subtitle, L"Recorder behavior currently wired to runtime.");
        break;
    case Page::Output:
        set_text(state->title, L"Settings / Output");
        set_text(state->subtitle, L"Folder changes apply live to new replay clips and recordings.");
        break;
    case Page::Hotkeys:
        set_text(state->title, L"Settings / Hotkeys");
        set_text(state->subtitle, L"Current global shortcuts. Rebinding comes later.");
        break;
    }

    InvalidateRect(hwnd, nullptr, TRUE);
}

bool parse_int(HWND hwnd, int min_value, int max_value, int* value)
{
    std::wstring text = get_text(hwnd);
    wchar_t* end = nullptr;
    long parsed = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L'\0') return false;
    if (parsed < min_value || parsed > max_value) return false;
    *value = static_cast<int>(parsed);
    return true;
}

bool browse_folder(HWND owner, std::wstring* selected)
{
    BROWSEINFOW info{};
    info.hwndOwner = owner;
    info.lpszTitle = L"Choose folder";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&info);
    if (!pidl) return false;

    wchar_t path[MAX_PATH] = {};
    bool ok = SHGetPathFromIDListW(pidl, path) == TRUE;
    CoTaskMemFree(pidl);
    if (!ok) return false;

    *selected = path;
    return true;
}

bool collect_model(HWND hwnd, State* state, Model* model)
{
    Model next = state->model;
    next.clips_directory = get_text(state->clips);
    next.recordings_directory = get_text(state->recordings);

    if (next.clips_directory.empty() || next.recordings_directory.empty()) {
        MessageBoxW(hwnd, L"Output folders cannot be empty.", L"Monolith Settings", MB_ICONWARNING);
        return false;
    }

    int duration = 0;
    if (!parse_int(state->replay_duration, 5, 600, &duration)) {
        MessageBoxW(hwnd, L"Replay duration must be 5 to 600 seconds.", L"Monolith Settings", MB_ICONWARNING);
        return false;
    }
    next.replay_duration_seconds = duration;

    int memory = 0;
    if (!parse_int(state->memory_budget, 64, 16384, &memory)) {
        MessageBoxW(hwnd, L"Replay memory must be 64 to 16384 MB.", L"Monolith Settings", MB_ICONWARNING);
        return false;
    }
    next.replay_memory_budget_mb = memory;

    *model = next;
    return true;
}

bool save(HWND hwnd, State* state)
{
    Model next;
    if (!collect_model(hwnd, state, &next)) return false;

    std::wstring error;
    if (state->on_save && !state->on_save(next, &error)) {
        MessageBoxW(hwnd, error.c_str(), L"Monolith Settings", MB_ICONERROR);
        return false;
    }

    state->model = next;
    state->dirty = false;
    return true;
}

bool can_close(HWND hwnd, State* state)
{
    if (!state || !state->dirty) return true;

    int result = MessageBoxW(
        hwnd,
        L"Save settings changes before closing?",
        L"Monolith Settings",
        MB_ICONQUESTION | MB_YESNOCANCEL);

    if (result == IDCANCEL) return false;
    if (result == IDNO) return true;
    return save(hwnd, state);
}

void create_sidebar(HWND hwnd)
{
    make_static(hwnd, L"Monolith", 22, 20, 190, 26);
    make_static(hwnd, L"Settings", 22, 48, 190, 20);

    make_button(hwnd, L"Basic Settings", IDC_NAV_BASIC, 18, 92, 204, 32);
    make_button(hwnd, L"Output Settings", IDC_NAV_OUTPUT, 18, 132, 204, 32);
    make_button(hwnd, L"Hotkeys", IDC_NAV_HOTKEYS, 18, 172, 204, 32);

    make_static(hwnd, L"Later", 22, 236, 190, 20);
    make_button(hwnd, L"Capture", IDC_NAV_CAPTURE, 18, 266, 204, 30, false);
    make_button(hwnd, L"Audio", IDC_NAV_AUDIO, 18, 302, 204, 30, false);
    make_button(hwnd, L"Encoding", IDC_NAV_ENCODING, 18, 338, 204, 30, false);
    make_button(hwnd, L"Advanced", IDC_NAV_ADVANCED, 18, 374, 204, 30, false);
}

void create_basic_page(HWND hwnd, State* state)
{
    add(state->basic_controls, make_group(hwnd, L"Replay Buffer", 260, 112, 560, 126));
    add(state->basic_controls, make_static(hwnd, L"Duration seconds", 286, 150, 170, 22));

    wchar_t duration[32] = {};
    swprintf_s(duration, L"%d", state->model.replay_duration_seconds);
    state->replay_duration = make_edit(
        hwnd, duration, IDC_REPLAY_DURATION, 470, 146, 120, 26,
        ES_NUMBER | ES_AUTOHSCROLL);
    add(state->basic_controls, state->replay_duration);

    add(state->basic_controls, make_static(hwnd, L"Memory budget MB", 286, 190, 170, 22));
    wchar_t memory[32] = {};
    swprintf_s(memory, L"%lld", static_cast<long long>(state->model.replay_memory_budget_mb));
    state->memory_budget = make_edit(
        hwnd, memory, IDC_MEMORY_BUDGET, 470, 186, 120, 26,
        ES_NUMBER | ES_AUTOHSCROLL);
    add(state->basic_controls, state->memory_budget);

    add(state->basic_controls, make_static(
        hwnd, L"Changes apply live to the replay buffer.", 610, 150, 190, 44));

    add(state->basic_controls, make_group(hwnd, L"App Behavior", 260, 260, 560, 84));
    add(state->basic_controls, make_static(
        hwnd, L"Startup, tray behavior, language, and logging controls are not wired yet.",
        286, 298, 500, 24));
}

void create_output_page(HWND hwnd, State* state)
{
    add(state->output_controls, make_group(hwnd, L"Clip Storage", 260, 112, 560, 102));
    add(state->output_controls, make_static(hwnd, L"Replay clip folder", 286, 150, 150, 22));
    state->clips = make_edit(
        hwnd, state->model.clips_directory.c_str(), IDC_CLIPS,
        436, 146, 286, 26, ES_AUTOHSCROLL);
    add(state->output_controls, state->clips);
    add(state->output_controls, make_button(hwnd, L"Browse...", IDC_BROWSE_CLIPS, 732, 145, 78, 28));

    add(state->output_controls, make_group(hwnd, L"Recording Storage", 260, 242, 560, 102));
    add(state->output_controls, make_static(hwnd, L"Recording folder", 286, 280, 150, 22));
    state->recordings = make_edit(
        hwnd, state->model.recordings_directory.c_str(), IDC_RECORDINGS,
        436, 276, 286, 26, ES_AUTOHSCROLL);
    add(state->output_controls, state->recordings);
    add(state->output_controls, make_button(hwnd, L"Browse...", IDC_BROWSE_RECORDINGS, 732, 275, 78, 28));
}

void create_hotkeys_page(HWND hwnd, State* state)
{
    add(state->hotkey_controls, make_group(hwnd, L"Global Hotkeys", 260, 112, 560, 210));

    add(state->hotkey_controls, make_static(hwnd, L"Save replay", 286, 150, 160, 22));
    add(state->hotkey_controls, make_edit(
        hwnd, state->model.save_replay_hotkey.c_str(), IDC_HK_SAVE_REPLAY,
        470, 146, 180, 26, ES_READONLY | ES_AUTOHSCROLL));

    add(state->hotkey_controls, make_static(hwnd, L"Start recording", 286, 188, 160, 22));
    add(state->hotkey_controls, make_edit(
        hwnd, state->model.recording_start_hotkey.c_str(), IDC_HK_RECORDING_START,
        470, 184, 180, 26, ES_READONLY | ES_AUTOHSCROLL));

    add(state->hotkey_controls, make_static(hwnd, L"Stop recording", 286, 226, 160, 22));
    add(state->hotkey_controls, make_edit(
        hwnd, state->model.recording_stop_hotkey.c_str(), IDC_HK_RECORDING_STOP,
        470, 222, 180, 26, ES_READONLY | ES_AUTOHSCROLL));

    add(state->hotkey_controls, make_static(hwnd, L"Pause / resume", 286, 264, 160, 22));
    add(state->hotkey_controls, make_edit(
        hwnd, state->model.pause_resume_hotkey.c_str(), IDC_HK_PAUSE_RESUME,
        470, 260, 180, 26, ES_READONLY | ES_AUTOHSCROLL));

    add(state->hotkey_controls, make_static(
        hwnd, L"Rebinding is intentionally disabled until conflict handling is wired.",
        286, 344, 520, 24));
}

void create_controls(HWND hwnd, State* state)
{
    state->creating = true;
    create_sidebar(hwnd);

    state->title = make_static(hwnd, L"Settings / Basic", 260, 24, 420, 30);
    state->subtitle = make_static(
        hwnd, L"Recorder behavior currently wired to runtime.", 260, 58, 560, 24);

    create_basic_page(hwnd, state);
    create_output_page(hwnd, state);
    create_hotkeys_page(hwnd, state);

    make_button(hwnd, L"Save", IDC_SAVE, 650, 508, 80, 30);
    make_button(hwnd, L"Cancel", IDC_CANCEL, 742, 508, 80, 30);

    state->creating = false;
    set_page(hwnd, state, Page::Basic);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* state = reinterpret_cast<State*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->bg_brush = CreateSolidBrush(kBg);
        state->sidebar_brush = CreateSolidBrush(kSidebar);
        state->panel_brush = CreateSolidBrush(kPanel);
        create_controls(hwnd, state);
        return 0;
    }
    case WM_COMMAND: {
        State* state = state_from(hwnd);
        if (!state) return 0;

        if (HIWORD(wp) == EN_CHANGE && !state->creating) {
            state->dirty = true;
            return 0;
        }

        switch (LOWORD(wp)) {
        case IDC_NAV_BASIC:
            set_page(hwnd, state, Page::Basic);
            return 0;
        case IDC_NAV_OUTPUT:
            set_page(hwnd, state, Page::Output);
            return 0;
        case IDC_NAV_HOTKEYS:
            set_page(hwnd, state, Page::Hotkeys);
            return 0;
        case IDC_BROWSE_CLIPS: {
            std::wstring selected;
            if (browse_folder(hwnd, &selected)) {
                set_text(state->clips, selected);
                state->dirty = true;
            }
            return 0;
        }
        case IDC_BROWSE_RECORDINGS: {
            std::wstring selected;
            if (browse_folder(hwnd, &selected)) {
                set_text(state->recordings, selected);
                state->dirty = true;
            }
            return 0;
        }
        case IDC_SAVE:
            if (save(hwnd, state)) DestroyWindow(hwnd);
            return 0;
        case IDC_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            return 0;
        }
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText);
        SetBkMode(dc, TRANSPARENT);
        State* state = state_from(hwnd);
        return reinterpret_cast<LRESULT>(state ? state->bg_brush : nullptr);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(20, 20, 20));
        SetBkColor(dc, RGB(250, 250, 250));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        State* state = state_from(hwnd);
        if (!state) {
            EndPaint(hwnd, &ps);
            return 0;
        }

        FillRect(dc, &ps.rcPaint, state->bg_brush);

        RECT side{ 0, 0, 240, 560 };
        FillRect(dc, &side, state->sidebar_brush);

        RECT footer{ 240, 492, 850, 560 };
        FillRect(dc, &footer, state->panel_brush);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE: {
        State* state = state_from(hwnd);
        if (can_close(hwnd, state)) DestroyWindow(hwnd);
        return 0;
    }
    case WM_NCDESTROY: {
        State* state = state_from(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (state) {
            DeleteObject(state->bg_brush);
            DeleteObject(state->sidebar_brush);
            DeleteObject(state->panel_brush);
            delete state;
        }
        g_window = nullptr;
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void register_class()
{
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kBg);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

} // namespace

void show(HWND owner, const Model& model, SaveCallback on_save)
{
    if (g_window) {
        ShowWindow(g_window, SW_SHOWNORMAL);
        SetForegroundWindow(g_window);
        return;
    }

    register_class();

    auto* state = new State{ model, std::move(on_save) };
    g_window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kClassName,
        L"Monolith Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        850,
        600,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        state);

    if (!g_window) {
        delete state;
        MessageBoxW(owner, L"Failed to open settings.", L"Monolith Settings", MB_ICONERROR);
        return;
    }

    ShowWindow(g_window, SW_SHOWNORMAL);
    SetForegroundWindow(g_window);
}

} // namespace settings_window
