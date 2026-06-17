using System.Runtime.InteropServices;

namespace Monolith.Settings.Services;

internal static class RecorderReloadNotifier
{
    private const int WM_APP = 0x8000;
    private const int WM_SETTINGS_RELOAD = WM_APP + 2;
    private const int WM_REFRESH_AUDIO_SOURCES = WM_APP + 4;

    private const uint SMTO_ABORTIFHUNG = 0x0002;

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern nint FindWindowW(string lpClassName, string? lpWindowName);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessageW(nint hWnd, uint msg, nint wParam, nint lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern nint SendMessageTimeoutW(
        nint hWnd, uint msg, nint wParam, nint lParam,
        uint flags, uint timeoutMs, out nint result);

    public static bool NotifySettingsSaved()
    {
        nint hwnd = FindWindowW("MonolithMsgWnd", "Monolith");
        return hwnd != 0 && PostMessageW(hwnd, WM_SETTINGS_RELOAD, 0, 0);
    }

    // Ask the recorder to re-enumerate live audio sessions and rewrite
    // runtime-status.json. Synchronous: returns only after the recorder has
    // finished the refresh (or the timeout elapses / it isn't running), so the
    // caller can re-read the status file immediately afterwards. Call off the UI
    // thread to avoid blocking it.
    public static bool RequestAudioSourceRefresh()
    {
        nint hwnd = FindWindowW("MonolithMsgWnd", "Monolith");
        if (hwnd == 0)
            return false;
        return SendMessageTimeoutW(
            hwnd, WM_REFRESH_AUDIO_SOURCES, 0, 0,
            SMTO_ABORTIFHUNG, 1000, out _) != 0;
    }
}
