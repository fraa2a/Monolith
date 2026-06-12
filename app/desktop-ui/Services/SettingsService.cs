using System.Text.Json;
using System.Text.Json.Nodes;
using Monolith.Settings.Models;

namespace Monolith.Settings.Services;

public sealed class SettingsService
{
    private const string FallbackDefaults = """
    {
      "schema_version": 1,
      "replay_buffer": {
        "enabled": true,
        "duration_seconds": 30,
        "memory_budget_mb": 512,
        "save_container": "mkv",
        "auto_remux_to_mp4": false
      },
      "recording": {
        "default_container": "mkv",
        "auto_remux_to_mp4": true
      },
      "output": {
        "clips_directory": "",
        "recordings_directory": "",
        "temp_directory": "",
        "storage_cap_gb": 200,
        "auto_cleanup": true
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

    public SettingsService()
    {
        string appData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        string monolithDir = Path.Combine(appData, "Monolith");
        Directory.CreateDirectory(monolithDir);
        ConfigPath = Path.Combine(monolithDir, "config.json");
    }

    public SettingsData Load()
    {
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
        SetIfMissing(output, "clips_directory", Path.Combine(videos, "Monolith", "Clips"));
        SetIfMissing(output, "recordings_directory", Path.Combine(videos, "Monolith", "Recordings"));
        SetIfMissing(output, "temp_directory", Path.Combine(localAppData, "Monolith", "Temp"));

        JsonObject replay = ObjectAt(root, "replay_buffer");
        SetIfMissing(replay, "duration_seconds", 30);
        SetIfMissing(replay, "memory_budget_mb", 512);

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

    private static string StringAt(JsonObject root, string key, string fallback = "")
    {
        return root[key]?.GetValue<string>() ?? fallback;
    }

    private static int IntAt(JsonObject root, string key, int fallback)
    {
        return root[key]?.GetValue<int>() ?? fallback;
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
