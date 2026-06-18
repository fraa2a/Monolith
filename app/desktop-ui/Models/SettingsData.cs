namespace Monolith.Settings.Models;

public sealed class AudioSourceData
{
    public string Id { get; set; } = "";
    public string Type { get; set; } = "";
    public string Name { get; set; } = "";
    public string DeviceId { get; set; } = "";
    public uint ProcessId { get; set; }
    public string ProcessName { get; set; } = "";
    public string ExecutablePath { get; set; } = "";
    public string WindowTitle { get; set; } = "";
    public string WindowClass { get; set; } = "";
    public bool Enabled { get; set; } = true;
    public List<int> Tracks { get; set; } = new();
}

public sealed class SettingsData
{
    // output
    public string ClipsDirectory { get; set; } = "";
    public string RecordingsDirectory { get; set; } = "";
    public string TempDirectory { get; set; } = "";

    // replay_buffer
    public string ReplayDurationSeconds { get; set; } = "30";
    public string ReplayMemoryBudgetMb { get; set; } = "512";
    public string ClipContainer { get; set; } = "mkv";

    // hotkeys
    public string SaveReplayHotkey { get; set; } = "Ctrl+Shift+F8";
    public string RecordingStartHotkey { get; set; } = "Ctrl+Shift+F9";
    public string RecordingStopHotkey { get; set; } = "Ctrl+Shift+F10";
    public string PauseResumeHotkey { get; set; } = "Ctrl+Shift+F11";
    public string RecordingContainer { get; set; } = "mkv";
    public bool RecordingEnabled { get; set; } = true;
    public bool ReplayBufferEnabled { get; set; } = true;
    public bool AutoCheckUpdates { get; set; } = true;

    // startup
    public bool AutoStart { get; set; } = false;

    // audio
    public string AudioMode { get; set; } = "default";
    public string PrimaryMicrophoneDeviceId { get; set; } = "";
    public List<AudioSourceData> AudioSources { get; set; } = new();

    // capture
    public string MonitorDevice { get; set; } = "";
    public string ResolutionMode { get; set; } = "source";
    public int ResolutionWidth { get; set; } = 0;
    public int ResolutionHeight { get; set; } = 0;
    public bool ShowCaptureBorder { get; set; } = false;

    // video_encoder
    public string EncoderBackend { get; set; } = "auto";
    public int VideoFps { get; set; } = 60;
    public int VideoQuality { get; set; } = 20;
    public string ScalingFilter { get; set; } = "bilinear";
    public string ExtraFfmpegOptions { get; set; } = "";
}
