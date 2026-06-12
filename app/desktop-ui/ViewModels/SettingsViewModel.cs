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
    private string clipsDirectory = "";
    private string recordingsDirectory = "";
    private string tempDirectory = "";
    private string replayDurationSeconds = "30";
    private string replayMemoryBudgetMb = "512";
    private string pageTitle = "Settings / Basic";
    private string pageSubtitle = "Recorder behavior currently wired to runtime.";
    private string statusMessage = "";
    private InfoBarSeverity statusSeverity = InfoBarSeverity.Informational;

    public event PropertyChangedEventHandler? PropertyChanged;

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

    public string SaveReplayHotkey { get; private set; } = "Ctrl+Shift+F8";
    public string RecordingStartHotkey { get; private set; } = "Ctrl+Shift+F9";
    public string RecordingStopHotkey { get; private set; } = "Ctrl+Shift+F10";
    public string PauseResumeHotkey { get; private set; } = "Ctrl+Shift+F11";

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

    public void Load()
    {
        loading = true;
        SettingsData data = service.Load();
        ClipsDirectory = data.ClipsDirectory;
        RecordingsDirectory = data.RecordingsDirectory;
        tempDirectory = data.TempDirectory;
        ReplayDurationSeconds = data.ReplayDurationSeconds;
        ReplayMemoryBudgetMb = data.ReplayMemoryBudgetMb;
        SaveReplayHotkey = data.SaveReplayHotkey;
        RecordingStartHotkey = data.RecordingStartHotkey;
        RecordingStopHotkey = data.RecordingStopHotkey;
        PauseResumeHotkey = data.PauseResumeHotkey;
        loading = false;
        HasUnsavedChanges = false;
        StatusMessage = "";
        NotifyHotkeys();
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
        });

        HasUnsavedChanges = false;
        StatusSeverity = InfoBarSeverity.Success;
        StatusMessage = "Settings saved.";
        return true;
    }

    public void SetPage(string page)
    {
        switch (page)
        {
            case "Output":
                PageTitle = "Settings / Output";
                PageSubtitle = "Folder changes apply to new replay clips and recordings after save.";
                break;
            case "Hotkeys":
                PageTitle = "Settings / Hotkeys";
                PageSubtitle = "Current global shortcuts. Rebinding comes later.";
                break;
            default:
                PageTitle = "Settings / Basic";
                PageSubtitle = "Recorder behavior currently wired to runtime.";
                break;
        }
    }

    public void SetClipsDirectory(string path) => ClipsDirectory = path;
    public void SetRecordingsDirectory(string path) => RecordingsDirectory = path;

    private bool Validate(out string error)
    {
        if (string.IsNullOrWhiteSpace(ClipsDirectory) || string.IsNullOrWhiteSpace(RecordingsDirectory))
        {
            error = "Output folders cannot be empty.";
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
