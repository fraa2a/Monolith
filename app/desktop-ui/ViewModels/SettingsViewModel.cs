using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml.Controls;
using Monolith.Settings.Models;
using Monolith.Settings.Services;

namespace Monolith.Settings.ViewModels;

public sealed class SettingsViewModel : INotifyPropertyChanged
{
    private readonly SettingsService service = new();
    private bool loading;
    private bool hasUnsavedChanges;

    // output
    private string clipsDirectory = "";
    private string recordingsDirectory = "";
    private string tempDirectory = "";

    // replay_buffer
    private string replayDurationSeconds = "30";
    private string replayMemoryBudgetMb = "512";
    private string clipContainer = "mkv";
    private bool replayBufferEnabled = true;
    private string recordingContainer = "mkv";
    private bool recordingEnabled = true;
    private string audioMode = "default";
    private string primaryMicrophoneDeviceId = "";
    private List<AudioSourceData> audioSources = new();
    private string saveReplayHotkey = "Ctrl+Shift+F8";
    private string recordingStartHotkey = "Ctrl+Shift+F9";
    private string recordingStopHotkey = "Ctrl+Shift+F10";
    private string pauseResumeHotkey = "Ctrl+Shift+F11";

    // capture — restart-required fields
    private string monitorDevice = "";
    private string resolutionMode = "source";
    private int resolutionWidth = 0;
    private int resolutionHeight = 0;
    private bool showCaptureBorder = false;

    // video_encoder — restart-required fields
    private string encoderBackend = "auto";
    private int bitrateKbps = 20000;
    private string extraFfmpegOptions = "";

    // page UI
    private string pageTitle = "Settings / General";
    private string pageSubtitle = "";
    private string statusMessage = "";
    private InfoBarSeverity statusSeverity = InfoBarSeverity.Informational;

    // restart-required tracking
    private string loadedMonitorDevice = "";
    private string loadedResolutionMode = "source";
    private int loadedResolutionWidth = 0;
    private int loadedResolutionHeight = 0;
    private bool loadedShowCaptureBorder = false;
    private string loadedEncoderBackend = "auto";
    private int loadedBitrateKbps = 20000;
    private string loadedExtraFfmpegOptions = "";
    private string loadedAudioFingerprint = "";
    private bool restartRequired;

    // runtime status (may be null if recorder never ran)
    private RuntimeStatus? runtimeStatus;

    public event PropertyChangedEventHandler? PropertyChanged;

    // ── output ──────────────────────────────────────────────────────────────

    public string ClipsDirectory
    {
        get => clipsDirectory;
        set => SetField(ref clipsDirectory, value);
    }

    public string RecordingsDirectory
    {
        get => recordingsDirectory;
        set => SetField(ref recordingsDirectory, value);
    }

    public string DefaultClipsDirectory => service.DefaultClipsDirectory;
    public string DefaultRecordingsDirectory => service.DefaultRecordingsDirectory;

    // ── replay_buffer ────────────────────────────────────────────────────────

    public string ReplayDurationSeconds
    {
        get => replayDurationSeconds;
        set => SetField(ref replayDurationSeconds, value);
    }

    public string ReplayMemoryBudgetMb
    {
        get => replayMemoryBudgetMb;
        set => SetField(ref replayMemoryBudgetMb, value);
    }

    public string ClipContainer
    {
        get => clipContainer;
        set => SetField(ref clipContainer, value);
    }

    public bool ReplayBufferEnabled
    {
        get => replayBufferEnabled;
        set => SetField(ref replayBufferEnabled, value);
    }

    public string RecordingContainer
    {
        get => recordingContainer;
        set => SetField(ref recordingContainer, value);
    }

    public bool RecordingEnabled
    {
        get => recordingEnabled;
        set => SetField(ref recordingEnabled, value);
    }

    public string AudioMode
    {
        get => audioMode;
        set
        {
            SetField(ref audioMode, value == "custom" ? "custom" : "default");
            OnPropertyChanged(nameof(IsCustomAudioMode));
        }
    }

    public bool IsCustomAudioMode => audioMode == "custom";

    public string PrimaryMicrophoneDeviceId
    {
        get => primaryMicrophoneDeviceId;
        set => SetField(ref primaryMicrophoneDeviceId, value);
    }

    public List<AudioSourceData> AudioSources
    {
        get => audioSources;
        private set => SetField(ref audioSources, value);
    }

    // ── capture ──────────────────────────────────────────────────────────────

    public string MonitorDevice
    {
        get => monitorDevice;
        set => SetField(ref monitorDevice, value);
    }

    public string ResolutionMode
    {
        get => resolutionMode;
        set
        {
            SetField(ref resolutionMode, value);
            OnPropertyChanged(nameof(IsCustomResolution));
        }
    }

    public int ResolutionWidth
    {
        get => resolutionWidth;
        set => SetField(ref resolutionWidth, value);
    }

    public int ResolutionHeight
    {
        get => resolutionHeight;
        set => SetField(ref resolutionHeight, value);
    }

    public bool ShowCaptureBorder
    {
        get => showCaptureBorder;
        set => SetField(ref showCaptureBorder, value);
    }

    public bool IsCustomResolution => resolutionMode == "custom";

    // ── video_encoder ────────────────────────────────────────────────────────

    public string EncoderBackend
    {
        get => encoderBackend;
        set => SetField(ref encoderBackend, value);
    }

    public int BitrateKbps
    {
        get => bitrateKbps;
        set
        {
            SetField(ref bitrateKbps, value);
            OnPropertyChanged(nameof(BitrateMbps));
        }
    }

    public string BitrateMbps => $"{bitrateKbps / 1000.0:F1} Mbps";

    public string ExtraFfmpegOptions
    {
        get => extraFfmpegOptions;
        set => SetField(ref extraFfmpegOptions, value);
    }

    // ── hotkeys (read-only) ─────────────────────────────────────────────────

    public string SaveReplayHotkey
    {
        get => saveReplayHotkey;
        set => SetField(ref saveReplayHotkey, value);
    }

    public string RecordingStartHotkey
    {
        get => recordingStartHotkey;
        set => SetField(ref recordingStartHotkey, value);
    }

    public string RecordingStopHotkey
    {
        get => recordingStopHotkey;
        set => SetField(ref recordingStopHotkey, value);
    }

    public string PauseResumeHotkey
    {
        get => pauseResumeHotkey;
        set => SetField(ref pauseResumeHotkey, value);
    }

    // ── runtime status ────────────────────────────────────────────────────────

    public RuntimeStatus? RuntimeStatus => runtimeStatus;

    public bool HasRuntimeStatus => runtimeStatus is not null;

    /// <summary>True when border_suppressed == false and ShowCaptureBorder is off.</summary>
    public bool ShowBorderSuppressedWarning =>
        runtimeStatus is not null && !runtimeStatus.BorderSuppressed && !showCaptureBorder;

    // ── page UI ──────────────────────────────────────────────────────────────

    public string PageTitle
    {
        get => pageTitle;
        private set => SetField(ref pageTitle, value, trackDirty: false);
    }

    public string PageSubtitle
    {
        get => pageSubtitle;
        private set => SetField(ref pageSubtitle, value, trackDirty: false);
    }

    public bool HasUnsavedChanges
    {
        get => hasUnsavedChanges;
        private set => SetField(ref hasUnsavedChanges, value, trackDirty: false);
    }

    public string StatusMessage
    {
        get => statusMessage;
        private set
        {
            SetField(ref statusMessage, value, trackDirty: false);
            OnPropertyChanged(nameof(HasStatusMessage));
        }
    }

    public bool HasStatusMessage => !string.IsNullOrWhiteSpace(StatusMessage);

    public InfoBarSeverity StatusSeverity
    {
        get => statusSeverity;
        private set => SetField(ref statusSeverity, value, trackDirty: false);
    }

    /// <summary>True after a successful save that changed a restart-required field.</summary>
    public bool RestartRequired
    {
        get => restartRequired;
        private set => SetField(ref restartRequired, value, trackDirty: false);
    }

    public string? LoadWarning => service.LoadWarning;
    public string ConfigPath => service.ConfigPath;
    public string LogFilePath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Monolith", "monolith.log");

    // ── public API ───────────────────────────────────────────────────────────

    public void Load()
    {
        loading = true;
        SettingsData data = service.Load();
        runtimeStatus = service.LoadRuntimeStatus();

        ClipsDirectory = data.ClipsDirectory;
        RecordingsDirectory = data.RecordingsDirectory;
        tempDirectory = data.TempDirectory;
        ReplayDurationSeconds = data.ReplayDurationSeconds;
        ReplayMemoryBudgetMb = data.ReplayMemoryBudgetMb;
        ClipContainer = data.ClipContainer == "mp4" ? "mp4" : "mkv";
        replayBufferEnabled = data.ReplayBufferEnabled;
        RecordingContainer = data.RecordingContainer == "mp4" ? "mp4" : "mkv";
        recordingEnabled = data.RecordingEnabled;
        SaveReplayHotkey = data.SaveReplayHotkey;
        RecordingStartHotkey = data.RecordingStartHotkey;
        RecordingStopHotkey = data.RecordingStopHotkey;
        PauseResumeHotkey = data.PauseResumeHotkey;
        audioMode = data.AudioMode == "custom" ? "custom" : "default";
        primaryMicrophoneDeviceId = data.PrimaryMicrophoneDeviceId;
        audioSources = CloneAudioSources(data.AudioSources);
        monitorDevice = data.MonitorDevice;
        resolutionMode = data.ResolutionMode;
        resolutionWidth = data.ResolutionWidth;
        resolutionHeight = data.ResolutionHeight;
        showCaptureBorder = data.ShowCaptureBorder;
        encoderBackend = data.EncoderBackend;
        bitrateKbps = data.BitrateKbps;
        extraFfmpegOptions = data.ExtraFfmpegOptions;

        // snapshot restart-required baseline
        loadedMonitorDevice = monitorDevice;
        loadedResolutionMode = resolutionMode;
        loadedResolutionWidth = resolutionWidth;
        loadedResolutionHeight = resolutionHeight;
        loadedShowCaptureBorder = showCaptureBorder;
        loadedEncoderBackend = encoderBackend;
        loadedBitrateKbps = bitrateKbps;
        loadedExtraFfmpegOptions = extraFfmpegOptions;
        loadedAudioFingerprint = AudioFingerprint();

        loading = false;
        HasUnsavedChanges = false;
        RestartRequired = false;
        StatusMessage = "";
        NotifyHotkeys();
        NotifyCaptureFields();
        NotifyAudioFields();
        OnPropertyChanged(nameof(HasRuntimeStatus));
        OnPropertyChanged(nameof(ShowBorderSuppressedWarning));
        OnPropertyChanged(nameof(LoadWarning));
        OnPropertyChanged(nameof(ConfigPath));
        OnPropertyChanged(nameof(LogFilePath));
    }

    public bool Save()
    {
        if (!Validate(out string error))
        {
            StatusSeverity = InfoBarSeverity.Error;
            StatusMessage = error;
            return false;
        }

        SaveReplayHotkey = NormalizeHotkey(SaveReplayHotkey);
        RecordingStartHotkey = NormalizeHotkey(RecordingStartHotkey);
        RecordingStopHotkey = NormalizeHotkey(RecordingStopHotkey);
        PauseResumeHotkey = NormalizeHotkey(PauseResumeHotkey);

        service.Save(new SettingsData
        {
            ClipsDirectory = ClipsDirectory,
            RecordingsDirectory = RecordingsDirectory,
            TempDirectory = tempDirectory,
            ReplayDurationSeconds = ReplayDurationSeconds,
            ReplayMemoryBudgetMb = ReplayMemoryBudgetMb,
            ClipContainer = ClipContainer,
            ReplayBufferEnabled = ReplayBufferEnabled,
            RecordingContainer = RecordingContainer,
            RecordingEnabled = RecordingEnabled,
            SaveReplayHotkey = SaveReplayHotkey,
            RecordingStartHotkey = RecordingStartHotkey,
            RecordingStopHotkey = RecordingStopHotkey,
            PauseResumeHotkey = PauseResumeHotkey,
            AudioMode = AudioMode,
            PrimaryMicrophoneDeviceId = PrimaryMicrophoneDeviceId,
            AudioSources = CloneAudioSources(AudioSources),
            MonitorDevice = MonitorDevice,
            ResolutionMode = ResolutionMode,
            ResolutionWidth = ResolutionWidth,
            ResolutionHeight = ResolutionHeight,
            ShowCaptureBorder = ShowCaptureBorder,
            EncoderBackend = EncoderBackend,
            BitrateKbps = BitrateKbps,
            ExtraFfmpegOptions = ExtraFfmpegOptions,
        });

        bool restartNeeded = IsRestartRequired();

        // Update baseline after save
        loadedMonitorDevice = monitorDevice;
        loadedResolutionMode = resolutionMode;
        loadedResolutionWidth = resolutionWidth;
        loadedResolutionHeight = resolutionHeight;
        loadedShowCaptureBorder = showCaptureBorder;
        loadedEncoderBackend = encoderBackend;
        loadedBitrateKbps = bitrateKbps;
        loadedExtraFfmpegOptions = extraFfmpegOptions;
        loadedAudioFingerprint = AudioFingerprint();

        HasUnsavedChanges = false;
        RestartRequired = restartNeeded;

        StatusSeverity = InfoBarSeverity.Success;
        string msg = "Settings saved.";
        if (restartNeeded)
            msg += " Capture, encoder, and audio routing changes apply after Monolith restarts.";
        StatusMessage = msg;
        return true;
    }

    public void SetPage(string page)
    {
        switch (page)
        {
            case "General":
                PageTitle = "Settings / General";
                PageSubtitle = "";
                break;
            case "Output":
                PageTitle = "Settings / Output";
                PageSubtitle = "Folder changes apply to new replay clips and recordings after save.";
                break;
            case "Clip":
                PageTitle = "Settings / Clip";
                PageSubtitle = "Replay buffer settings.";
                break;
            case "Recording":
                PageTitle = "Settings / Recording";
                PageSubtitle = "Manual recording output settings.";
                break;
            case "Capture":
                PageTitle = "Settings / Capture";
                PageSubtitle = "Display capture and encoder settings.";
                break;
            case "Audio":
                PageTitle = "Settings / Audio";
                PageSubtitle = "Default or custom audio routing. Changes apply after Monolith restarts.";
                break;
            case "Hotkeys":
                PageTitle = "Settings / Hotkeys";
                PageSubtitle = "Global shortcuts apply after save and Settings closes.";
                break;
            case "Advanced":
                PageTitle = "Settings / Advanced";
                PageSubtitle = "Diagnostics paths are read-only.";
                break;
            default:
                PageTitle = "Settings";
                PageSubtitle = "";
                break;
        }
    }

    public void SetClipsDirectory(string path) => ClipsDirectory = path;
    public void SetRecordingsDirectory(string path) => RecordingsDirectory = path;
    public void ResetClipsDirectory() => ClipsDirectory = service.DefaultClipsDirectory;
    public void ResetRecordingsDirectory() => RecordingsDirectory = service.DefaultRecordingsDirectory;

    public void AddDesktopAudioSource()
    {
        if (AudioSources.Any(s => s.Type == "desktop"))
            return;
        AddAudioSource(new AudioSourceData
        {
            Id = "desktop",
            Type = "desktop",
            Name = "All desktop audio",
            Enabled = true,
            Tracks = new List<int> { 1 },
        });
    }

    public void AddActiveGameSource()
    {
        if (AudioSources.Any(s => s.Type == "active_game"))
            return;
        AddAudioSource(new AudioSourceData
        {
            Id = "active_game",
            Type = "active_game",
            Name = "Active Game",
            Enabled = true,
            Tracks = new List<int> { 1 },
        });
    }

    public void AddInputDeviceSource(AudioDeviceInfo device)
    {
        if (AudioSources.Any(s => s.Type == "input" && s.DeviceId == device.Id))
            return;
        AddAudioSource(new AudioSourceData
        {
            Id = $"input:{device.Id}",
            Type = "input",
            Name = device.Name,
            DeviceId = device.Id,
            Enabled = true,
            Tracks = new List<int> { 2 },
        });
    }

    public void AddProcessSource(AudioSessionInfo session)
    {
        string id = !string.IsNullOrWhiteSpace(session.ExecutablePath)
            ? $"process:{session.ExecutablePath}"
            : $"process:{session.ProcessId}";
        if (AudioSources.Any(s => s.Id == id))
            return;
        AddAudioSource(new AudioSourceData
        {
            Id = id,
            Type = "process",
            Name = string.IsNullOrWhiteSpace(session.DisplayName) ? session.ProcessName : session.DisplayName,
            ProcessId = session.ProcessId,
            ProcessName = session.ProcessName,
            ExecutablePath = session.ExecutablePath,
            Enabled = true,
            Tracks = new List<int> { 1 },
        });
    }

    public void RemoveAudioSource(AudioSourceData source)
    {
        AudioSources = AudioSources.Where(s => !ReferenceEquals(s, source)).ToList();
        MarkDirty();
    }

    public void SetAudioSourceEnabled(AudioSourceData source, bool enabled)
    {
        source.Enabled = enabled;
        OnPropertyChanged(nameof(AudioSources));
        MarkDirty();
    }

    public void SetAudioSourceTrack(AudioSourceData source, int track, bool enabled)
    {
        if (track < 1 || track > 6) return;
        if (enabled)
        {
            if (!source.Tracks.Contains(track))
                source.Tracks.Add(track);
        }
        else
        {
            source.Tracks.Remove(track);
        }
        source.Tracks = source.Tracks.Distinct().Where(t => t is >= 1 and <= 6).OrderBy(t => t).ToList();
        OnPropertyChanged(nameof(AudioSources));
        MarkDirty();
    }

    private void AddAudioSource(AudioSourceData source)
    {
        List<AudioSourceData> next = CloneAudioSources(AudioSources);
        next.Add(source);
        AudioSources = next;
        MarkDirty();
    }

    // ── private helpers ──────────────────────────────────────────────────────

    private bool IsRestartRequired()
    {
        return monitorDevice != loadedMonitorDevice
            || resolutionMode != loadedResolutionMode
            || resolutionWidth != loadedResolutionWidth
            || resolutionHeight != loadedResolutionHeight
            || showCaptureBorder != loadedShowCaptureBorder
            || encoderBackend != loadedEncoderBackend
            || bitrateKbps != loadedBitrateKbps
            || extraFfmpegOptions != loadedExtraFfmpegOptions
            || loadedAudioFingerprint != AudioFingerprint();
    }

    private bool Validate(out string error)
    {
        if (string.IsNullOrWhiteSpace(ClipsDirectory))
        {
            error = "Clips folder cannot be empty.";
            return false;
        }

        if (!Path.IsPathRooted(ClipsDirectory))
        {
            error = "Clips folder must be an absolute path.";
            return false;
        }

        if (string.IsNullOrWhiteSpace(RecordingsDirectory))
        {
            error = "Recordings folder cannot be empty.";
            return false;
        }

        if (!Path.IsPathRooted(RecordingsDirectory))
        {
            error = "Recordings folder must be an absolute path.";
            return false;
        }

        if (!IsIntInRange(ReplayDurationSeconds, 5, 600))
        {
            error = "Replay duration must be 5 to 600 seconds.";
            return false;
        }

        if (!IsIntInRange(ReplayMemoryBudgetMb, 64, 16384))
        {
            error = "Replay memory must be 64 to 16384 MB.";
            return false;
        }

        if (ClipContainer != "mkv" && ClipContainer != "mp4")
        {
            error = "Clip format must be MKV or MP4.";
            return false;
        }

        if (RecordingContainer != "mkv" && RecordingContainer != "mp4")
        {
            error = "Recording format must be MKV or MP4.";
            return false;
        }

        if (AudioMode != "default" && AudioMode != "custom")
        {
            error = "Audio mode must be Default or Custom.";
            return false;
        }

        foreach (AudioSourceData source in AudioSources)
        {
            if (source.Type is not ("desktop" or "input" or "process" or "active_game"))
            {
                error = $"Audio source '{source.Name}' has an unsupported type.";
                return false;
            }

            if (source.Tracks.Count == 0 || source.Tracks.Any(t => t < 1 || t > 6))
            {
                error = $"Audio source '{source.Name}' must use tracks 1 to 6.";
                return false;
            }
        }

        if (!IsIntInRange(BitrateKbps.ToString(), 1000, 100000))
        {
            error = "Video bitrate must be 1000 to 100000 kbps.";
            return false;
        }

        if (EncoderBackend != "auto" &&
            EncoderBackend != "h264_nvenc" &&
            EncoderBackend != "h264_amf" &&
            EncoderBackend != "h264_qsv" &&
            EncoderBackend != "libx264" &&
            EncoderBackend != "hevc_nvenc" &&
            EncoderBackend != "hevc_amf" &&
            EncoderBackend != "hevc_qsv" &&
            EncoderBackend != "libx265")
        {
            EncoderBackend = "auto";
        }

        if (resolutionMode == "custom")
        {
            if (resolutionWidth < 128 || resolutionWidth > 7680 || resolutionWidth % 2 != 0)
            {
                error = "Custom width must be 128–7680 and even.";
                return false;
            }

            if (resolutionHeight < 128 || resolutionHeight > 4320 || resolutionHeight % 2 != 0)
            {
                error = "Custom height must be 128–4320 and even.";
                return false;
            }
        }

        (string Name, string Value)[] hotkeys =
        {
            ("Save replay", SaveReplayHotkey),
            ("Start recording", RecordingStartHotkey),
            ("Stop recording", RecordingStopHotkey),
            ("Pause / resume", PauseResumeHotkey),
        };

        HashSet<string> seenHotkeys = new(StringComparer.OrdinalIgnoreCase);
        foreach ((string name, string value) in hotkeys)
        {
            if (!IsValidHotkey(value))
            {
                error = $"{name} hotkey must be a valid key or key combination.";
                return false;
            }

            string normalized = NormalizeHotkey(value);
            if (!seenHotkeys.Add(normalized))
            {
                error = "Hotkeys must be unique.";
                return false;
            }
        }

        error = "";
        return true;
    }

    private static bool IsIntInRange(string text, int min, int max)
    {
        return int.TryParse(text, out int value) && value >= min && value <= max;
    }

    private static bool IsValidHotkey(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
            return false;

        string[] parts = text.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length < 1)
            return false;

        bool sawKey = false;
        HashSet<string> modifiers = new(StringComparer.OrdinalIgnoreCase);
        HashSet<string> keys = new(StringComparer.OrdinalIgnoreCase);

        foreach (string part in parts)
        {
            string? modifier = NormalizeHotkeyModifier(part);
            if (modifier is not null)
            {
                if (!modifiers.Add(modifier))
                    return false;
                continue;
            }

            if (!IsValidHotkeyKey(part))
                return false;

            if (!keys.Add(NormalizeHotkeyKey(part)))
                return false;
            sawKey = true;
        }

        return sawKey;
    }

    private static bool IsValidHotkeyKey(string key)
    {
        string upper = key.Trim().ToUpperInvariant();
        if (upper.Length == 1)
        {
            char c = upper[0];
            return (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9')
                || ";=,-./`[]\\'".Contains(c);
        }

        if (upper.Length >= 2 && upper[0] == 'F' &&
            int.TryParse(upper[1..], out int functionKey))
            return functionKey >= 1 && functionKey <= 24;

        return upper is "SPACE" or "TAB" or "ESC" or "ESCAPE"
            or "ENTER" or "BACKSPACE"
            or "INSERT" or "INS" or "DELETE" or "DEL"
            or "HOME" or "END"
            or "PAGEUP" or "PGUP" or "PAGEDOWN" or "PGDN"
            or "UP" or "DOWN" or "LEFT" or "RIGHT"
            or "NUMPAD0" or "NUMPAD1" or "NUMPAD2" or "NUMPAD3" or "NUMPAD4"
            or "NUMPAD5" or "NUMPAD6" or "NUMPAD7" or "NUMPAD8" or "NUMPAD9"
            or "NUMPADMULTIPLY" or "NUMPADADD" or "NUMPADSUBTRACT"
            or "NUMPADDECIMAL" or "NUMPADDIVIDE";
    }

    private static string NormalizeHotkey(string text)
    {
        string[] parts = text.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        List<string> modifiers = new();
        List<string> keys = new();

        foreach (string part in parts)
        {
            string? modifier = NormalizeHotkeyModifier(part);
            if (modifier is not null)
                modifiers.Add(modifier);
            else
                keys.Add(NormalizeHotkeyKey(part));
        }

        modifiers.AddRange(keys);
        return string.Join("+", modifiers);
    }

    private static string? NormalizeHotkeyModifier(string token)
    {
        return token.Trim().ToUpperInvariant() switch
        {
            "CTRL" or "CONTROL" => "Ctrl",
            "SHIFT" => "Shift",
            "ALT" => "Alt",
            "WIN" or "WINDOWS" => "Win",
            _ => null,
        };
    }

    private static string NormalizeHotkeyKey(string token)
    {
        string upper = token.Trim().ToUpperInvariant();
        if (upper.Length == 1)
            return upper;

        if (upper.Length >= 2 && upper[0] == 'F' &&
            int.TryParse(upper[1..], out int functionKey))
            return $"F{functionKey}";

        return upper switch
        {
            "ESCAPE" => "Esc",
            "ESC" => "Esc",
            "ENTER" => "Enter",
            "BACKSPACE" => "Backspace",
            "INS" => "Insert",
            "INSERT" => "Insert",
            "DEL" => "Delete",
            "DELETE" => "Delete",
            "PGUP" => "PageUp",
            "PAGEUP" => "PageUp",
            "PGDN" => "PageDown",
            "PAGEDOWN" => "PageDown",
            "NUMPAD0" => "Numpad0",
            "NUMPAD1" => "Numpad1",
            "NUMPAD2" => "Numpad2",
            "NUMPAD3" => "Numpad3",
            "NUMPAD4" => "Numpad4",
            "NUMPAD5" => "Numpad5",
            "NUMPAD6" => "Numpad6",
            "NUMPAD7" => "Numpad7",
            "NUMPAD8" => "Numpad8",
            "NUMPAD9" => "Numpad9",
            "NUMPADMULTIPLY" => "NumpadMultiply",
            "NUMPADADD" => "NumpadAdd",
            "NUMPADSUBTRACT" => "NumpadSubtract",
            "NUMPADDECIMAL" => "NumpadDecimal",
            "NUMPADDIVIDE" => "NumpadDivide",
            _ => upper[0] + upper[1..].ToLowerInvariant(),
        };
    }

    private void NotifyHotkeys()
    {
        OnPropertyChanged(nameof(ClipContainer));
        OnPropertyChanged(nameof(RecordingContainer));
        OnPropertyChanged(nameof(ReplayBufferEnabled));
        OnPropertyChanged(nameof(RecordingEnabled));
        OnPropertyChanged(nameof(SaveReplayHotkey));
        OnPropertyChanged(nameof(RecordingStartHotkey));
        OnPropertyChanged(nameof(RecordingStopHotkey));
        OnPropertyChanged(nameof(PauseResumeHotkey));
    }

    private void NotifyCaptureFields()
    {
        OnPropertyChanged(nameof(MonitorDevice));
        OnPropertyChanged(nameof(ResolutionMode));
        OnPropertyChanged(nameof(ResolutionWidth));
        OnPropertyChanged(nameof(ResolutionHeight));
        OnPropertyChanged(nameof(ShowCaptureBorder));
        OnPropertyChanged(nameof(IsCustomResolution));
        OnPropertyChanged(nameof(EncoderBackend));
        OnPropertyChanged(nameof(BitrateKbps));
        OnPropertyChanged(nameof(BitrateMbps));
        OnPropertyChanged(nameof(ExtraFfmpegOptions));
    }

    private void NotifyAudioFields()
    {
        OnPropertyChanged(nameof(AudioMode));
        OnPropertyChanged(nameof(IsCustomAudioMode));
        OnPropertyChanged(nameof(PrimaryMicrophoneDeviceId));
        OnPropertyChanged(nameof(AudioSources));
    }

    private string AudioFingerprint()
    {
        IEnumerable<string> sourceBits = AudioSources
            .OrderBy(s => s.Id, StringComparer.OrdinalIgnoreCase)
            .Select(s => string.Join("|",
                s.Id,
                s.Type,
                s.Name,
                s.DeviceId,
                s.ProcessId.ToString(),
                s.ProcessName,
                s.ExecutablePath,
                s.Enabled ? "1" : "0",
                string.Join(",", s.Tracks.OrderBy(t => t))));

        return string.Join(";", new[]
        {
            AudioMode,
            PrimaryMicrophoneDeviceId,
            string.Join(";", sourceBits),
        });
    }

    private static List<AudioSourceData> CloneAudioSources(IEnumerable<AudioSourceData> sources)
    {
        return sources.Select(s => new AudioSourceData
        {
            Id = s.Id,
            Type = s.Type,
            Name = s.Name,
            DeviceId = s.DeviceId,
            ProcessId = s.ProcessId,
            ProcessName = s.ProcessName,
            ExecutablePath = s.ExecutablePath,
            Enabled = s.Enabled,
            Tracks = s.Tracks.Distinct().Where(t => t is >= 1 and <= 6).OrderBy(t => t).ToList(),
        }).ToList();
    }

    private void MarkDirty()
    {
        if (!loading)
            HasUnsavedChanges = true;
    }

    private void SetField<T>(ref T field, T value, bool trackDirty = true, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
            return;

        field = value;
        OnPropertyChanged(name);
        if (!loading && trackDirty)
            HasUnsavedChanges = true;
    }

    private void OnPropertyChanged([CallerMemberName] string? name = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }
}
