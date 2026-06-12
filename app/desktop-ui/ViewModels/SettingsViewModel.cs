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

    public string SaveReplayHotkey { get; private set; } = "Ctrl+Shift+F8";
    public string RecordingStartHotkey { get; private set; } = "Ctrl+Shift+F9";
    public string RecordingStopHotkey { get; private set; } = "Ctrl+Shift+F10";
    public string PauseResumeHotkey { get; private set; } = "Ctrl+Shift+F11";

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
        SaveReplayHotkey = data.SaveReplayHotkey;
        RecordingStartHotkey = data.RecordingStartHotkey;
        RecordingStopHotkey = data.RecordingStopHotkey;
        PauseResumeHotkey = data.PauseResumeHotkey;
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

        loading = false;
        HasUnsavedChanges = false;
        RestartRequired = false;
        StatusMessage = "";
        NotifyHotkeys();
        NotifyCaptureFields();
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

        service.Save(new SettingsData
        {
            ClipsDirectory = ClipsDirectory,
            RecordingsDirectory = RecordingsDirectory,
            TempDirectory = tempDirectory,
            ReplayDurationSeconds = ReplayDurationSeconds,
            ReplayMemoryBudgetMb = ReplayMemoryBudgetMb,
            SaveReplayHotkey = SaveReplayHotkey,
            RecordingStartHotkey = RecordingStartHotkey,
            RecordingStopHotkey = RecordingStopHotkey,
            PauseResumeHotkey = PauseResumeHotkey,
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

        HasUnsavedChanges = false;
        RestartRequired = restartNeeded;

        StatusSeverity = InfoBarSeverity.Success;
        string msg = "Settings saved.";
        if (restartNeeded)
            msg += " Capture & encoder changes apply after Monolith restarts.";
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
                PageSubtitle = "Audio capture status.";
                break;
            case "Hotkeys":
                PageTitle = "Settings / Hotkeys";
                PageSubtitle = "Current global shortcuts. Rebinding comes later.";
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
            || extraFfmpegOptions != loadedExtraFfmpegOptions;
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

        if (!IsIntInRange(BitrateKbps.ToString(), 1000, 100000))
        {
            error = "Video bitrate must be 1000 to 100000 kbps.";
            return false;
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

        error = "";
        return true;
    }

    private static bool IsIntInRange(string text, int min, int max)
    {
        return int.TryParse(text, out int value) && value >= min && value <= max;
    }

    private void NotifyHotkeys()
    {
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
