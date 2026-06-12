using System.Text.Json;
using System.Text.Json.Nodes;
using Monolith.Settings.Models;

namespace Monolith.Settings.Services;

public sealed class SettingsService
{
    private const string FallbackDefaults = """
    {
      "schema_version": 2,
      "capture": {
        "monitor_device": "",
        "resolution_mode": "source",
        "resolution_width": 0,
        "resolution_height": 0,
        "show_capture_border": false
      },
      "replay_buffer": {
        "duration_seconds": 30,
        "memory_budget_mb": 512,
        "save_container": "mkv"
      },
      "recording": {
        "container": "mkv",
        "pause_behavior": "timestamp_gap"
      },
      "video_encoder": {
        "backend": "auto",
        "bitrate_kbps": 20000,
        "extra_ffmpeg_options": ""
      },
      "output": {
        "clips_directory": "",
        "recordings_directory": "",
        "temp_directory": ""
      },
      "hotkeys": {
        "save_replay": "Ctrl+Shift+F8",
        "recording_start": "Ctrl+Shift+F9",
        "recording_stop": "Ctrl+Shift+F10",
        "pause_resume": "Ctrl+Shift+F11"
      }
    }
    """;

    private readonly JsonSerializerOptions jsonOptions = new() { WriteIndented = true };

    public string ConfigPath { get; }
    public string RuntimeStatusPath { get; }
    public string? LoadWarning { get; private set; }

    public string DefaultClipsDirectory { get; }
    public string DefaultRecordingsDirectory { get; }

    public SettingsService()
    {
        string appData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        string monolithDir = Path.Combine(appData, "Monolith");
        Directory.CreateDirectory(monolithDir);
        ConfigPath = Path.Combine(monolithDir, "config.json");
        RuntimeStatusPath = Path.Combine(monolithDir, "runtime-status.json");

        string videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        DefaultClipsDirectory = Path.Combine(videos, "Monolith", "Clips");
        DefaultRecordingsDirectory = Path.Combine(videos, "Monolith", "Recordings");
    }

    public SettingsData Load()
    {
        LoadWarning = null;
        JsonObject root = LoadMergedConfig();
        EnsureRuntimeDefaults(root);

        if (!File.Exists(ConfigPath))
            SaveRoot(root);

        return ToSettingsData(root);
    }

    public void Save(SettingsData settings)
    {
        JsonObject root = LoadMergedConfig();
        EnsureRuntimeDefaults(root);
        WriteRuntimeFields(root, settings);
        SaveRoot(root);
    }

    public RuntimeStatus? LoadRuntimeStatus()
    {
        if (!File.Exists(RuntimeStatusPath))
            return null;

        try
        {
            string json = File.ReadAllText(RuntimeStatusPath);
            JsonNode? node = JsonNode.Parse(json);
            if (node is not JsonObject obj)
                return null;

            RuntimeStatus status = new();

            if (obj["monitors"] is JsonArray monitorsArr)
            {
                foreach (JsonNode? item in monitorsArr)
                {
                    if (item is not JsonObject m)
                        continue;
                    status.Monitors.Add(new MonitorInfo
                    {
                        Device = StringAt(m, "device"),
                        Width = IntAt(m, "width", 0),
                        Height = IntAt(m, "height", 0),
                        Primary = BoolAt(m, "primary"),
                    });
                }
            }

            if (obj["available_encoders"] is JsonArray encArr)
            {
                foreach (JsonNode? item in encArr)
                {
                    string? s = item?.GetValue<string>();
                    if (s is not null)
                        status.AvailableEncoders.Add(s);
                }
            }

            status.ActiveEncoder = StringAt(obj, "active_encoder");
            status.ActiveMonitorDevice = StringAt(obj, "active_monitor_device");
            status.BorderSuppressed = BoolAt(obj, "border_suppressed");
            status.EncodeWidth = IntAt(obj, "encode_width", 0);
            status.EncodeHeight = IntAt(obj, "encode_height", 0);

            return status;
        }
        catch
        {
            return null;
        }
    }

    private JsonObject LoadMergedConfig()
    {
        JsonObject root = LoadDefaultConfig();

        if (!File.Exists(ConfigPath))
            return root;

        try
        {
            JsonNode? userNode = JsonNode.Parse(File.ReadAllText(ConfigPath));
            if (userNode is JsonObject userObject)
                Merge(root, userObject);
        }
        catch
        {
            // Corrupt config: back it up and remember the warning.
            try
            {
                string bakPath = ConfigPath + ".bak";
                File.Copy(ConfigPath, bakPath, overwrite: true);
            }
            catch
            {
                // Best-effort; ignore copy failure.
            }

            LoadWarning = $"Config file was corrupt and has been backed up to config.json.bak. Defaults have been loaded.";
            return root;
        }

        return root;
    }

    private JsonObject LoadDefaultConfig()
    {
        string? defaultConfigPath = FindDefaultConfig();
        if (defaultConfigPath is not null)
        {
            try
            {
                JsonNode? node = JsonNode.Parse(File.ReadAllText(defaultConfigPath));
                if (node is JsonObject obj)
                    return obj;
            }
            catch
            {
            }
        }

        return JsonNode.Parse(FallbackDefaults)!.AsObject();
    }

    private static string? FindDefaultConfig()
    {
        DirectoryInfo? dir = new(AppContext.BaseDirectory);
        for (int i = 0; dir is not null && i < 8; i++, dir = dir.Parent)
        {
            string candidate = Path.Combine(dir.FullName, "config", "default-config.json");
            if (File.Exists(candidate))
                return candidate;
        }

        return null;
    }

    private static void Merge(JsonObject target, JsonObject overrides)
    {
        foreach (KeyValuePair<string, JsonNode?> pair in overrides)
        {
            if (target[pair.Key] is JsonObject targetChild && pair.Value is JsonObject overrideChild)
            {
                Merge(targetChild, overrideChild);
                continue;
            }

            target[pair.Key] = pair.Value?.DeepClone();
        }
    }

    private static void EnsureRuntimeDefaults(JsonObject root)
    {
        string videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);

        JsonObject output = ObjectAt(root, "output");
        SetIfEmpty(output, "clips_directory", Path.Combine(videos, "Monolith", "Clips"));
        SetIfEmpty(output, "recordings_directory", Path.Combine(videos, "Monolith", "Recordings"));
        SetIfEmpty(output, "temp_directory", Path.Combine(localAppData, "Monolith", "Temp"));

        JsonObject replay = ObjectAt(root, "replay_buffer");
        SetIfMissing(replay, "duration_seconds", 30);
        SetIfMissing(replay, "memory_budget_mb", 512);
        SetIfMissing(replay, "save_container", "mkv");

        JsonObject capture = ObjectAt(root, "capture");
        SetIfMissing(capture, "monitor_device", "");
        SetIfMissing(capture, "resolution_mode", "source");
        SetIfMissing(capture, "resolution_width", 0);
        SetIfMissing(capture, "resolution_height", 0);
        SetIfMissing(capture, "show_capture_border", false);

        JsonObject videoEncoder = ObjectAt(root, "video_encoder");
        SetIfMissing(videoEncoder, "backend", "auto");
        SetIfMissing(videoEncoder, "bitrate_kbps", 20000);
        SetIfMissing(videoEncoder, "extra_ffmpeg_options", "");

        JsonObject recording = ObjectAt(root, "recording");
        SetIfMissing(recording, "container", "mkv");
        SetIfMissing(recording, "pause_behavior", "timestamp_gap");

        JsonObject hotkeys = ObjectAt(root, "hotkeys");
        SetIfMissing(hotkeys, "save_replay", "Ctrl+Shift+F8");
        SetIfMissing(hotkeys, "recording_start", "Ctrl+Shift+F9");
        SetIfMissing(hotkeys, "recording_stop", "Ctrl+Shift+F10");
        SetIfMissing(hotkeys, "pause_resume", "Ctrl+Shift+F11");
    }

    private static SettingsData ToSettingsData(JsonObject root)
    {
        JsonObject output = ObjectAt(root, "output");
        JsonObject replay = ObjectAt(root, "replay_buffer");
        JsonObject hotkeys = ObjectAt(root, "hotkeys");
        JsonObject capture = ObjectAt(root, "capture");
        JsonObject videoEncoder = ObjectAt(root, "video_encoder");
        JsonObject recording = ObjectAt(root, "recording");

        return new SettingsData
        {
            ClipsDirectory = StringAt(output, "clips_directory"),
            RecordingsDirectory = StringAt(output, "recordings_directory"),
            TempDirectory = StringAt(output, "temp_directory"),
            ReplayDurationSeconds = IntAt(replay, "duration_seconds", 30).ToString(),
            ReplayMemoryBudgetMb = IntAt(replay, "memory_budget_mb", 512).ToString(),
            SaveReplayHotkey = StringAt(hotkeys, "save_replay", "Ctrl+Shift+F8"),
            RecordingStartHotkey = StringAt(hotkeys, "recording_start", "Ctrl+Shift+F9"),
            RecordingStopHotkey = StringAt(hotkeys, "recording_stop", "Ctrl+Shift+F10"),
            PauseResumeHotkey = StringAt(hotkeys, "pause_resume", "Ctrl+Shift+F11"),
            RecordingContainer = StringAt(recording, "container", "mkv"),
            MonitorDevice = StringAt(capture, "monitor_device"),
            ResolutionMode = StringAt(capture, "resolution_mode", "source"),
            ResolutionWidth = IntAt(capture, "resolution_width", 0),
            ResolutionHeight = IntAt(capture, "resolution_height", 0),
            ShowCaptureBorder = BoolAt(capture, "show_capture_border"),
            EncoderBackend = StringAt(videoEncoder, "backend", "auto"),
            BitrateKbps = IntAt(videoEncoder, "bitrate_kbps", 20000),
            ExtraFfmpegOptions = StringAt(videoEncoder, "extra_ffmpeg_options"),
        };
    }

    private static void WriteRuntimeFields(JsonObject root, SettingsData settings)
    {
        JsonObject output = ObjectAt(root, "output");
        output["clips_directory"] = settings.ClipsDirectory;
        output["recordings_directory"] = settings.RecordingsDirectory;
        output["temp_directory"] = settings.TempDirectory;

        JsonObject replay = ObjectAt(root, "replay_buffer");
        replay["duration_seconds"] = ParseInt(settings.ReplayDurationSeconds, 30, 5, 600);
        replay["memory_budget_mb"] = ParseInt(settings.ReplayMemoryBudgetMb, 512, 64, 16384);

        JsonObject capture = ObjectAt(root, "capture");
        capture["monitor_device"] = settings.MonitorDevice;
        capture["resolution_mode"] = settings.ResolutionMode;
        capture["resolution_width"] = settings.ResolutionWidth;
        capture["resolution_height"] = settings.ResolutionHeight;
        capture["show_capture_border"] = settings.ShowCaptureBorder;

        JsonObject videoEncoder = ObjectAt(root, "video_encoder");
        videoEncoder["backend"] = settings.EncoderBackend;
        videoEncoder["bitrate_kbps"] = Math.Clamp(settings.BitrateKbps, 1000, 100000);
        videoEncoder["extra_ffmpeg_options"] = settings.ExtraFfmpegOptions;

        JsonObject recording = ObjectAt(root, "recording");
        recording["container"] = settings.RecordingContainer == "mp4" ? "mp4" : "mkv";

        JsonObject hotkeys = ObjectAt(root, "hotkeys");
        hotkeys["save_replay"] = settings.SaveReplayHotkey;
        hotkeys["recording_start"] = settings.RecordingStartHotkey;
        hotkeys["recording_stop"] = settings.RecordingStopHotkey;
        hotkeys["pause_resume"] = settings.PauseResumeHotkey;
    }

    private void SaveRoot(JsonObject root)
    {
        EnsureDirectory(StringAt(ObjectAt(root, "output"), "clips_directory"));
        EnsureDirectory(StringAt(ObjectAt(root, "output"), "recordings_directory"));
        EnsureDirectory(StringAt(ObjectAt(root, "output"), "temp_directory"));
        File.WriteAllText(ConfigPath, root.ToJsonString(jsonOptions));
    }

    private static JsonObject ObjectAt(JsonObject root, string key)
    {
        if (root[key] is JsonObject obj)
            return obj;

        JsonObject created = new();
        root[key] = created;
        return created;
    }

    private static void SetIfMissing(JsonObject root, string key, string value)
    {
        if (root[key] is null)
            root[key] = JsonValue.Create(value);
    }

    private static void SetIfMissing(JsonObject root, string key, int value)
    {
        if (root[key] is null)
            root[key] = JsonValue.Create(value);
    }

    private static void SetIfMissing(JsonObject root, string key, bool value)
    {
        if (root[key] is null)
            root[key] = JsonValue.Create(value);
    }

    private static void SetIfEmpty(JsonObject root, string key, string value)
    {
        string? current = root[key]?.GetValue<string>();
        if (string.IsNullOrEmpty(current))
            root[key] = JsonValue.Create(value);
    }

    private static string StringAt(JsonObject root, string key, string fallback = "")
    {
        return root[key]?.GetValue<string>() ?? fallback;
    }

    private static int IntAt(JsonObject root, string key, int fallback)
    {
        try { return root[key]?.GetValue<int>() ?? fallback; }
        catch { return fallback; }
    }

    private static bool BoolAt(JsonObject root, string key)
    {
        try { return root[key]?.GetValue<bool>() ?? false; }
        catch { return false; }
    }

    private static int ParseInt(string text, int fallback, int min, int max)
    {
        if (!int.TryParse(text, out int value))
            return fallback;

        return value < min || value > max ? fallback : value;
    }

    private static void EnsureDirectory(string path)
    {
        if (!string.IsNullOrWhiteSpace(path))
            Directory.CreateDirectory(path);
    }
}
