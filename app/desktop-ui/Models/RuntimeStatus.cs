namespace Monolith.Settings.Models;

public sealed class MonitorInfo
{
    public string Device { get; set; } = "";
    public int Width { get; set; }
    public int Height { get; set; }
    public bool Primary { get; set; }
}

public sealed class AudioDeviceInfo
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public bool DefaultDevice { get; set; }
    public bool Available { get; set; } = true;
}

public sealed class AudioSessionInfo
{
    public uint ProcessId { get; set; }
    public string ProcessName { get; set; } = "";
    public string DisplayName { get; set; } = "";
    public string ExecutablePath { get; set; } = "";
}

public sealed class RuntimeStatus
{
    public List<MonitorInfo> Monitors { get; set; } = new();
    public List<AudioDeviceInfo> InputDevices { get; set; } = new();
    public List<AudioSessionInfo> AudioSessions { get; set; } = new();
    public AudioSessionInfo? ActiveGame { get; set; }
    public List<string> AvailableEncoders { get; set; } = new();
    public string ActiveEncoder { get; set; } = "";
    public string ActiveMonitorDevice { get; set; } = "";
    public bool BorderSuppressed { get; set; }
    public int EncodeWidth { get; set; }
    public int EncodeHeight { get; set; }
}
