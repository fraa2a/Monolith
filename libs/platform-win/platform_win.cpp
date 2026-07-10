#include "platform_win.h"

namespace platform_win {

std::wstring utf8_to_wide(const std::string& value)
{
    if (value.empty()) return {};
    int count = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count);
    return result;
}

std::string wide_to_utf8(const std::wstring& value)
{
    if (value.empty()) return {};
    int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

namespace {

std::wstring file_name_from_path(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

} // namespace

ProcessInfo process_info(uint32_t pid)
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

bool is_capture_candidate_window(HWND hwnd)
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

std::wstring window_text(HWND hwnd)
{
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(hwnd, text.data(), len + 1);
    if (copied <= 0) return {};
    text.resize(static_cast<size_t>(copied));
    return text;
}

std::wstring window_class_name(HWND hwnd)
{
    wchar_t buf[256] = {};
    int copied = GetClassNameW(hwnd, buf, static_cast<int>(_countof(buf)));
    if (copied <= 0) return {};
    return std::wstring(buf, static_cast<size_t>(copied));
}

} // namespace platform_win
