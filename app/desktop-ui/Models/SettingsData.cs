namespace Monolith.Settings.Models;

public sealed class SettingsData
{
    // output
    public string ClipsDirectory { get; set; } = "";
    public string RecordingsDirectory { get; set; } = "";
    public string TempDirectory { get; set; } = "";

    // replay_buffer
    public string ReplayDurationSeconds { get; set; } = "30";
    public string ReplayMemoryBudgetMb { get; set; } = "512";

    // hotkeys
    public string SaveReplayHotkey { get; set; } = "Ctrl+Shift+F8";
    public string RecordingStartHotkey { get; set; } = "Ctrl+Shift+F9";
    public string RecordingStopHotkey { get; set; } = "Ctrl+Shift+F10";
    public string PauseResumeHotkey { get; set; } = "Ctrl+Shift+F11";

    // capture
    public string MonitorDevice { get; set; } = "";
    public string ResolutionMode { get; set; } = "source";
    public int ResolutionWidth { get; set; } = 0;
    public int ResolutionHeight { get; set; } = 0;
    public bool ShowCaptureBorder { get; set; } = false;

    // video_encoder
    public string EncoderBackend { get; set; } = "auto";
    public int BitrateKbps { get; set; } = 20000;
    public string ExtraFfmpegOptions { get; set; } = "";
}
