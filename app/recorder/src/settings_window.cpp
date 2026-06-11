#include "settings_window.h"

#include <shlobj.h>

#include <cwchar>
#include <vector>

namespace settings_window {
namespace {

constexpr wchar_t kClassName[] = L"MonolithSettingsWindow";

enum ControlId : int {
    IDC_CLIPS = 2001,
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

struct State {
    Model model;
    SaveCallback on_save;
    HWND clips = nullptr;
    HWND recordings = nullptr;
    HWND replay_duration = nullptr;
    HWND memory_budget = nullptr;
};

HWND g_window = nullptr;

void set_font(HWND hwnd)
{
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND make_control(
    HWND parent,
    const wchar_t* cls,
    const wchar_t* text,
    DWORD style,
    int id,
    int x,
    int y,
    int w,
    int h)
{
    HWND control = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        cls,
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

HWND make_label(HWND parent, const wchar_t* text, int x, int y, int w, int h)
{
    HWND label = CreateWindowW(
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
    if (label) set_font(label);
    return label;
}

HWND make_button(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h)
{
    HWND button = CreateWindowW(
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
    if (button) set_font(button);
    return button;
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

State* state_from(HWND hwnd)
{
    return reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void create_controls(HWND hwnd, State* state)
{
    make_label(hwnd, L"Replay clip folder", 16, 18, 160, 22);
    state->clips = make_control(
        hwnd, L"EDIT", state->model.clips_directory.c_str(),
        ES_AUTOHSCROLL, IDC_CLIPS, 180, 16, 360, 24);
    make_button(hwnd, L"Browse...", IDC_BROWSE_CLIPS, 550, 15, 80, 26);

    make_label(hwnd, L"Recording folder", 16, 54, 160, 22);
    state->recordings = make_control(
        hwnd, L"EDIT", state->model.recordings_directory.c_str(),
        ES_AUTOHSCROLL, IDC_RECORDINGS, 180, 52, 360, 24);
    make_button(hwnd, L"Browse...", IDC_BROWSE_RECORDINGS, 550, 51, 80, 26);

    make_label(hwnd, L"Replay duration seconds", 16, 94, 160, 22);
    wchar_t duration[32] = {};
    swprintf_s(duration, L"%d", state->model.replay_duration_seconds);
    state->replay_duration = make_control(
        hwnd, L"EDIT", duration,
        ES_NUMBER | ES_AUTOHSCROLL, IDC_REPLAY_DURATION, 180, 92, 100, 24);

    make_label(hwnd, L"Replay memory MB", 310, 94, 150, 22);
    wchar_t memory[32] = {};
    swprintf_s(memory, L"%lld", static_cast<long long>(state->model.replay_memory_budget_mb));
    state->memory_budget = make_control(
        hwnd, L"EDIT", memory,
        ES_NUMBER | ES_AUTOHSCROLL, IDC_MEMORY_BUDGET, 460, 92, 80, 24);

    make_label(hwnd, L"Hotkeys (read-only)", 16, 138, 160, 22);
    make_label(hwnd, L"Save replay", 32, 170, 140, 22);
    make_control(hwnd, L"EDIT", state->model.save_replay_hotkey.c_str(),
        ES_READONLY | ES_AUTOHSCROLL, IDC_HK_SAVE_REPLAY, 180, 168, 180, 24);
    make_label(hwnd, L"Start recording", 32, 202, 140, 22);
    make_control(hwnd, L"EDIT", state->model.recording_start_hotkey.c_str(),
        ES_READONLY | ES_AUTOHSCROLL, IDC_HK_RECORDING_START, 180, 200, 180, 24);
    make_label(hwnd, L"Stop recording", 32, 234, 140, 22);
    make_control(hwnd, L"EDIT", state->model.recording_stop_hotkey.c_str(),
        ES_READONLY | ES_AUTOHSCROLL, IDC_HK_RECORDING_STOP, 180, 232, 180, 24);
    make_label(hwnd, L"Pause/resume", 32, 266, 140, 22);
    make_control(hwnd, L"EDIT", state->model.pause_resume_hotkey.c_str(),
        ES_READONLY | ES_AUTOHSCROLL, IDC_HK_PAUSE_RESUME, 180, 264, 180, 24);

    make_label(hwnd, L"Hotkey rebinding is not implemented yet.", 380, 170, 250, 22);
    make_label(hwnd, L"Folder and replay changes apply live.", 380, 202, 250, 22);

    make_button(hwnd, L"Save", IDC_SAVE, 450, 320, 80, 28);
    make_button(hwnd, L"Cancel", IDC_CANCEL, 540, 320, 80, 28);
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

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* state = reinterpret_cast<State*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        create_controls(hwnd, state);
        return 0;
    }
    case WM_COMMAND: {
        State* state = state_from(hwnd);
        if (!state) return 0;

        switch (LOWORD(wp)) {
        case IDC_BROWSE_CLIPS: {
            std::wstring selected;
            if (browse_folder(hwnd, &selected)) set_text(state->clips, selected);
            return 0;
        }
        case IDC_BROWSE_RECORDINGS: {
            std::wstring selected;
            if (browse_folder(hwnd, &selected)) set_text(state->recordings, selected);
            return 0;
        }
        case IDC_SAVE: {
            Model next;
            if (!collect_model(hwnd, state, &next)) return 0;
            std::wstring error;
            if (state->on_save && !state->on_save(next, &error)) {
                MessageBoxW(hwnd, error.c_str(), L"Monolith Settings", MB_ICONERROR);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case IDC_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            return 0;
        }
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY: {
        State* state = state_from(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete state;
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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        660,
        400,
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
