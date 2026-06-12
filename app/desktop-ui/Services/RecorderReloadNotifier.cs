using System.Runtime.InteropServices;

namespace Monolith.Settings.Services;

internal static class RecorderReloadNotifier
{
    private const int WM_APP = 0x8000;
    private const int WM_SETTINGS_RELOAD = WM_APP + 2;

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern nint FindWindowW(string lpClassName, string? lpWindowName);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessageW(nint hWnd, uint msg, nint wParam, nint lParam);

    public static bool NotifySettingsSaved()
    {
        nint hwnd = FindWindowW("MonolithMsgWnd", "Monolith");
        return hwnd != 0 && PostMessageW(hwnd, WM_SETTINGS_RELOAD, 0, 0);
    }
}
