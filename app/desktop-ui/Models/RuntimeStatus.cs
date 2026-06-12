namespace Monolith.Settings.Models;

public sealed class MonitorInfo
{
    public string Device { get; set; } = "";
    public int Width { get; set; }
    public int Height { get; set; }
    public bool Primary { get; set; }
}

public sealed class RuntimeStatus
{
    public List<MonitorInfo> Monitors { get; set; } = new();
    public List<string> AvailableEncoders { get; set; } = new();
    public string ActiveEncoder { get; set; } = "";
    public string ActiveMonitorDevice { get; set; } = "";
    public bool BorderSuppressed { get; set; }
    public int EncodeWidth { get; set; }
    public int EncodeHeight { get; set; }
}
