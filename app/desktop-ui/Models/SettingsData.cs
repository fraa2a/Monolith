namespace Monolith.Settings.Models;

public sealed class SettingsData
{
    public string ClipsDirectory { get; set; } = "";
    public string RecordingsDirectory { get; set; } = "";
    public string TempDirectory { get; set; } = "";
    public string ReplayDurationSeconds { get; set; } = "30";
    public string ReplayMemoryBudgetMb { get; set; } = "512";
    public string SaveReplayHotkey { get; set; } = "Ctrl+Shift+F8";
    public string RecordingStartHotkey { get; set; } = "Ctrl+Shift+F9";
    public string RecordingStopHotkey { get; set; } = "Ctrl+Shift+F10";
    public string PauseResumeHotkey { get; set; } = "Ctrl+Shift+F11";
}
