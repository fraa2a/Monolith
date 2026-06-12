using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Monolith.Settings.Models;
using Monolith.Settings.ViewModels;
using System.Runtime.InteropServices;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace Monolith.Settings;

public sealed partial class MainWindow : Window
{
    private readonly SettingsViewModel viewModel = new();
    private AppWindow? appWindow;
    private bool closeAllowed;

    [DllImport("user32.dll", SetLastError = true)]
    private static extern nint SetWindowsHookExW(int idHook, LowLevelKeyboardProc callback, nint hMod, uint dwThreadId);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnhookWindowsHookEx(nint hhk);

    [DllImport("user32.dll")]
    private static extern nint CallNextHookEx(nint hhk, int nCode, nint wParam, nint lParam);

    [DllImport("kernel32.dll")]
    private static extern nint GetModuleHandleW(string? lpModuleName);

    private delegate nint LowLevelKeyboardProc(int nCode, nint wParam, nint lParam);

    // Suppress programmatic ComboBox changes from triggering ViewModel updates.
    private bool suppressComboBoxEvents;
    private bool suppressAudioEvents;
    private LowLevelKeyboardProc? hotkeyCaptureProc;
    private nint hotkeyCaptureHook;
    private string? hotkeyCaptureTarget;
    private readonly HashSet<int> hotkeyCaptureDownKeys = new();

    private const int WH_KEYBOARD_LL = 13;
    private const int WM_KEYDOWN = 0x0100;
    private const int WM_SYSKEYDOWN = 0x0104;
    private const int WM_KEYUP = 0x0101;
    private const int WM_SYSKEYUP = 0x0105;

    // Preset resolutions by tag
    private static readonly (string Tag, int Width, int Height)[] ResolutionPresets =
    {
        ("1280x720",  1280,  720),
        ("1920x1080", 1920, 1080),
        ("2560x1440", 2560, 1440),
    };

    // Friendly encoder labels
    private static readonly Dictionary<string, string> EncoderLabels = new(StringComparer.OrdinalIgnoreCase)
    {
        ["auto"]       = "Auto (recommended)",
        ["h264_nvenc"] = "NVIDIA NVENC (h264_nvenc)",
        ["h264_amf"]   = "AMD AMF (h264_amf)",
        ["h264_qsv"]   = "Intel Quick Sync (h264_qsv)",
        ["libx264"]    = "Software x264 (libx264)",
        ["hevc_nvenc"] = "NVIDIA HEVC / H.265 (hevc_nvenc)",
        ["hevc_amf"]   = "AMD HEVC / H.265 (hevc_amf)",
        ["hevc_qsv"]   = "Intel HEVC / H.265 (hevc_qsv)",
        ["libx265"]    = "Software x265 / H.265 (libx265)",
    };

    public MainWindow()
    {
        InitializeComponent();
        RootLayout.DataContext = viewModel;
        viewModel.Load();
        ConfigureWindow();
        PopulateCaptureCombos();
        SelectRecordingFormat();
        SelectClipFormat();
        PopulateAudioControls();
        RefreshAudioSourcesList();
        UpdateCaptureBorderWarning();
        SyncComponentToggles();
        UpdateCorruptConfigBar();
    }

    // ── Window setup ─────────────────────────────────────────────────────────

    private void ConfigureWindow()
    {
        RootLayout.RequestedTheme = ElementTheme.Dark;

        nint hwnd = WindowNative.GetWindowHandle(this);
        WindowId windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(hwnd);
        appWindow = AppWindow.GetFromWindowId(windowId);
        appWindow.Title = "Monolith Settings";
        appWindow.Resize(new Windows.Graphics.SizeInt32(1160, 760));
        appWindow.Closing += OnAppWindowClosing;

        string iconPath = Path.Combine(AppContext.BaseDirectory, "Assets", "Monolith.ico");
        if (File.Exists(iconPath))
        {
            try { appWindow.SetIcon(iconPath); }
            catch { /* Icon is cosmetic — ignore any failure. */ }
        }

        AppWindowTitleBar titleBar = appWindow.TitleBar;
        titleBar.BackgroundColor = ColorHelper.FromArgb(255, 32, 32, 32);
        titleBar.ForegroundColor = Colors.White;
        titleBar.InactiveBackgroundColor = ColorHelper.FromArgb(255, 32, 32, 32);
        titleBar.InactiveForegroundColor = ColorHelper.FromArgb(255, 180, 180, 180);
        titleBar.ButtonBackgroundColor = ColorHelper.FromArgb(255, 32, 32, 32);
        titleBar.ButtonForegroundColor = Colors.White;
        titleBar.ButtonHoverBackgroundColor = ColorHelper.FromArgb(255, 50, 50, 50);
        titleBar.ButtonHoverForegroundColor = Colors.White;
        titleBar.ButtonPressedBackgroundColor = ColorHelper.FromArgb(255, 65, 65, 65);
        titleBar.ButtonPressedForegroundColor = Colors.White;
        titleBar.ButtonInactiveBackgroundColor = ColorHelper.FromArgb(255, 32, 32, 32);
        titleBar.ButtonInactiveForegroundColor = ColorHelper.FromArgb(255, 180, 180, 180);
    }

    // ── Capture ComboBox population ──────────────────────────────────────────

    private void PopulateCaptureCombos()
    {
        PopulateMonitorCombo();
        PopulateEncoderCombo();
        SelectResolutionFromViewModel();
    }

    private void PopulateMonitorCombo()
    {
        suppressComboBoxEvents = true;
        MonitorComboBox.Items.Clear();

        // Always first item: Primary monitor (default) → device ""
        ComboBoxItem primaryItem = new() { Content = "Primary monitor (default)", Tag = "" };
        MonitorComboBox.Items.Add(primaryItem);

        RuntimeStatus? status = viewModel.RuntimeStatus;

        if (status is null || status.Monitors.Count == 0)
        {
            MonitorComboBox.IsEnabled = false;
            MonitorUnavailableNote.Visibility = Visibility.Visible;
        }
        else
        {
            MonitorComboBox.IsEnabled = true;
            MonitorUnavailableNote.Visibility = Visibility.Collapsed;

            int displayIndex = 1;
            foreach (MonitorInfo monitor in status.Monitors)
            {
                string label = $"Display {displayIndex} — {monitor.Width}x{monitor.Height}";
                if (monitor.Primary)
                    label += " (Primary)";
                ComboBoxItem item = new() { Content = label, Tag = monitor.Device };
                MonitorComboBox.Items.Add(item);
                displayIndex++;
            }
        }

        // Select the item matching the current MonitorDevice
        SelectComboBoxByTag(MonitorComboBox, viewModel.MonitorDevice);
        suppressComboBoxEvents = false;
    }

    private void PopulateEncoderCombo()
    {
        suppressComboBoxEvents = true;
        EncoderComboBox.Items.Clear();

        RuntimeStatus? status = viewModel.RuntimeStatus;

        // Always add Auto
        ComboBoxItem autoItem = new() { Content = "Auto (recommended)", Tag = "auto" };
        EncoderComboBox.Items.Add(autoItem);

        string currentBackend = viewModel.EncoderBackend;
        bool currentFound = string.Equals(currentBackend, "auto", StringComparison.OrdinalIgnoreCase);

        if (status is not null && status.AvailableEncoders.Count > 0)
        {
            foreach (string enc in status.AvailableEncoders)
            {
                if (string.Equals(enc, "auto", StringComparison.OrdinalIgnoreCase))
                    continue;

                string label = EncoderLabels.TryGetValue(enc, out string? friendly) ? friendly : enc;
                ComboBoxItem item = new() { Content = label, Tag = enc };
                EncoderComboBox.Items.Add(item);

                if (string.Equals(enc, currentBackend, StringComparison.OrdinalIgnoreCase))
                    currentFound = true;
            }
        }

        // If the saved backend is not in the available list, add it with " — not detected"
        if (!currentFound && !string.Equals(currentBackend, "auto", StringComparison.OrdinalIgnoreCase))
        {
            string baseLabel = EncoderLabels.TryGetValue(currentBackend, out string? fl) ? fl : currentBackend;
            ComboBoxItem notDetectedItem = new() { Content = $"{baseLabel} — not detected", Tag = currentBackend };
            EncoderComboBox.Items.Add(notDetectedItem);
        }

        SelectComboBoxByTag(EncoderComboBox, currentBackend);
        suppressComboBoxEvents = false;
    }

    private void SelectResolutionFromViewModel()
    {
        suppressComboBoxEvents = true;

        string mode = viewModel.ResolutionMode;
        int w = viewModel.ResolutionWidth;
        int h = viewModel.ResolutionHeight;

        string tagToSelect = "source";

        if (mode == "custom")
        {
            // Check if it matches a preset
            bool matchedPreset = false;
            foreach ((string tag, int pw, int ph) in ResolutionPresets)
            {
                if (w == pw && h == ph)
                {
                    // Show the matching preset item; the stored mode stays
                    // "custom" — presets are saved as custom + their size, so
                    // the two states are identical on disk.
                    tagToSelect = tag;
                    matchedPreset = true;
                    break;
                }
            }

            if (!matchedPreset)
                tagToSelect = "custom";
        }

        SelectComboBoxByTag(ResolutionComboBox, tagToSelect);
        UpdateCustomResolutionPanel(tagToSelect);

        suppressComboBoxEvents = false;
    }

    private static void SelectComboBoxByTag(ComboBox comboBox, string tag)
    {
        foreach (object item in comboBox.Items)
        {
            if (item is ComboBoxItem cbi && string.Equals(cbi.Tag as string, tag, StringComparison.Ordinal))
            {
                comboBox.SelectedItem = cbi;
                return;
            }
        }

        // Fall back to first item
        if (comboBox.Items.Count > 0)
            comboBox.SelectedIndex = 0;
    }

    private void UpdateCustomResolutionPanel(string tag)
    {
        bool isCustom = tag == "custom";
        CustomResolutionPanel.Visibility = isCustom ? Visibility.Visible : Visibility.Collapsed;
    }

    private void SelectRecordingFormat()
    {
        suppressComboBoxEvents = true;
        SelectComboBoxByTag(RecordingFormatComboBox, viewModel.RecordingContainer);
        suppressComboBoxEvents = false;
    }

    private void SelectClipFormat()
    {
        suppressComboBoxEvents = true;
        SelectComboBoxByTag(ClipFormatComboBox, viewModel.ClipContainer);
        suppressComboBoxEvents = false;
    }

    private void PopulateAudioControls()
    {
        suppressAudioEvents = true;
        SelectComboBoxByTag(AudioModeComboBox, viewModel.AudioMode);
        PopulatePrimaryMicCombo();
        PopulateAddAudioSourceCombo();
        suppressAudioEvents = false;
    }

    private void PopulatePrimaryMicCombo()
    {
        PrimaryMicComboBox.Items.Clear();
        PrimaryMicComboBox.Items.Add(new ComboBoxItem { Content = "Default microphone", Tag = "" });

        RuntimeStatus? status = viewModel.RuntimeStatus;
        bool found = string.IsNullOrEmpty(viewModel.PrimaryMicrophoneDeviceId);
        if (status is not null)
        {
            foreach (AudioDeviceInfo device in status.InputDevices)
            {
                string label = device.Name;
                if (device.DefaultDevice)
                    label += " (Default)";
                PrimaryMicComboBox.Items.Add(new ComboBoxItem { Content = label, Tag = device.Id });
                if (device.Id == viewModel.PrimaryMicrophoneDeviceId)
                    found = true;
            }
        }

        if (!found)
        {
            PrimaryMicComboBox.Items.Add(new ComboBoxItem
            {
                Content = $"{viewModel.PrimaryMicrophoneDeviceId} — unavailable",
                Tag = viewModel.PrimaryMicrophoneDeviceId,
            });
        }

        SelectComboBoxByTag(PrimaryMicComboBox, viewModel.PrimaryMicrophoneDeviceId);
    }

    private void PopulateAddAudioSourceCombo()
    {
        AddAudioSourceComboBox.Items.Clear();
        AddAudioSourceComboBox.Items.Add(new ComboBoxItem { Content = "All desktop audio", Tag = "desktop" });
        AddAudioSourceComboBox.Items.Add(new ComboBoxItem { Content = "Active Game", Tag = "active_game" });

        RuntimeStatus? status = viewModel.RuntimeStatus;
        if (status is not null)
        {
            foreach (AudioDeviceInfo device in status.InputDevices)
                AddAudioSourceComboBox.Items.Add(new ComboBoxItem { Content = $"Input: {device.Name}", Tag = device });

            foreach (AudioSessionInfo session in status.AudioSessions)
            {
                string label = string.IsNullOrWhiteSpace(session.DisplayName)
                    ? session.ProcessName
                    : session.DisplayName;
                if (string.IsNullOrWhiteSpace(label))
                    label = $"PID {session.ProcessId}";
                AddAudioSourceComboBox.Items.Add(new ComboBoxItem { Content = $"App: {label}", Tag = session });
            }
        }

        if (AddAudioSourceComboBox.Items.Count > 0)
            AddAudioSourceComboBox.SelectedIndex = 0;
    }

    private void RefreshAudioSourcesList()
    {
        AudioSourcesList.Items.Clear();
        foreach (AudioSourceData source in viewModel.AudioSources)
        {
            AudioSourcesList.Items.Add(new ListViewItem
            {
                Content = AudioSourceLabel(source),
                Tag = source,
            });
        }

        if (AudioSourcesList.Items.Count > 0 && AudioSourcesList.SelectedIndex < 0)
            AudioSourcesList.SelectedIndex = 0;
        RefreshSelectedAudioSourceEditor();
    }

    private string AudioSourceLabel(AudioSourceData source)
    {
        string label = string.IsNullOrWhiteSpace(source.Name) ? source.Id : source.Name;
        string tracks = source.Tracks.Count == 0 ? "no tracks" : $"tracks {string.Join(",", source.Tracks.OrderBy(t => t))}";
        string availability = IsAudioSourceAvailable(source) ? "" : " — unavailable";
        string disabled = source.Enabled ? "" : " — disabled";
        return $"{label} ({source.Type}, {tracks}){availability}{disabled}";
    }

    private bool IsAudioSourceAvailable(AudioSourceData source)
    {
        RuntimeStatus? status = viewModel.RuntimeStatus;
        if (source.Type is "desktop" or "active_game")
            return true;
        if (status is null)
            return false;
        if (source.Type == "input")
            return status.InputDevices.Any(d => d.Id == source.DeviceId);
        if (source.Type == "process")
            return status.AudioSessions.Any(s =>
                (source.ProcessId != 0 && s.ProcessId == source.ProcessId) ||
                (!string.IsNullOrWhiteSpace(source.ExecutablePath) && s.ExecutablePath == source.ExecutablePath) ||
                (!string.IsNullOrWhiteSpace(source.ProcessName) && s.ProcessName == source.ProcessName));
        return false;
    }

    private AudioSourceData? SelectedAudioSource()
    {
        return (AudioSourcesList.SelectedItem as ListViewItem)?.Tag as AudioSourceData;
    }

    private void RefreshSelectedAudioSourceEditor()
    {
        suppressAudioEvents = true;
        AudioSourceData? source = SelectedAudioSource();
        bool hasSource = source is not null;
        SelectedAudioSourceName.Text = hasSource ? AudioSourceLabel(source!) : "Select a source";
        AudioSourceEnabledToggle.IsEnabled = hasSource;
        AudioSourceEnabledToggle.IsOn = source?.Enabled ?? false;

        CheckBox[] boxes =
        {
            AudioTrack1Box, AudioTrack2Box, AudioTrack3Box,
            AudioTrack4Box, AudioTrack5Box, AudioTrack6Box,
        };
        for (int i = 0; i < boxes.Length; i++)
        {
            boxes[i].IsEnabled = hasSource;
            boxes[i].IsChecked = source?.Tracks.Contains(i + 1) ?? false;
        }
        suppressAudioEvents = false;
    }

    // ── Navigation ────────────────────────────────────────────────────────────

    private void OnNavSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is not NavigationViewItem item || item.Tag is not string page)
            return;

        GeneralPage.Visibility   = page == "General"   ? Visibility.Visible : Visibility.Collapsed;
        OutputPage.Visibility    = page == "Output"    ? Visibility.Visible : Visibility.Collapsed;
        ClipPage.Visibility      = page == "Clip"      ? Visibility.Visible : Visibility.Collapsed;
        RecordingPage.Visibility = page == "Recording" ? Visibility.Visible : Visibility.Collapsed;
        CapturePage.Visibility   = page == "Capture"   ? Visibility.Visible : Visibility.Collapsed;
        AudioPage.Visibility     = page == "Audio"     ? Visibility.Visible : Visibility.Collapsed;
        HotkeysPage.Visibility   = page == "Hotkeys"   ? Visibility.Visible : Visibility.Collapsed;
        AdvancedPage.Visibility  = page == "Advanced"  ? Visibility.Visible : Visibility.Collapsed;

        viewModel.SetPage(page);
    }

    // ── Monitor ComboBox ──────────────────────────────────────────────────────

    private void OnMonitorSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressComboBoxEvents)
            return;

        if (MonitorComboBox.SelectedItem is ComboBoxItem item && item.Tag is string device)
            viewModel.MonitorDevice = device;
    }

    // ── Resolution ComboBox ───────────────────────────────────────────────────

    private void OnResolutionSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressComboBoxEvents)
            return;

        if (ResolutionComboBox.SelectedItem is not ComboBoxItem item || item.Tag is not string tag)
            return;

        UpdateCustomResolutionPanel(tag);

        if (tag == "source")
        {
            viewModel.ResolutionMode = "source";
            viewModel.ResolutionWidth = 0;
            viewModel.ResolutionHeight = 0;
        }
        else if (tag == "custom")
        {
            viewModel.ResolutionMode = "custom";
            // Keep current width/height values; user edits via NumberBoxes.
        }
        else
        {
            // Preset tag format: "1920x1080"
            foreach ((string presetTag, int pw, int ph) in ResolutionPresets)
            {
                if (presetTag == tag)
                {
                    viewModel.ResolutionMode = "custom";
                    viewModel.ResolutionWidth = pw;
                    viewModel.ResolutionHeight = ph;
                    suppressComboBoxEvents = true;
                    WidthBox.Value = pw;
                    HeightBox.Value = ph;
                    suppressComboBoxEvents = false;
                    break;
                }
            }
        }
    }

    private void OnResolutionValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (suppressComboBoxEvents || double.IsNaN(args.NewValue))
            return;

        int val = (int)args.NewValue;
        // Snap to even
        if (val % 2 != 0)
            val = val % 2 == 1 ? val - 1 : val;

        if (sender == WidthBox)
            viewModel.ResolutionWidth = val;
        else if (sender == HeightBox)
            viewModel.ResolutionHeight = val;
    }

    // ── Encoder ComboBox ──────────────────────────────────────────────────────

    private void OnEncoderSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressComboBoxEvents)
            return;

        if (EncoderComboBox.SelectedItem is ComboBoxItem item && item.Tag is string backend)
            viewModel.EncoderBackend = backend;
    }

    // ── Bitrate NumberBox ─────────────────────────────────────────────────────

    private void OnBitrateValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (suppressComboBoxEvents || double.IsNaN(args.NewValue))
            return;

        viewModel.BitrateKbps = (int)args.NewValue;
    }

    // ── Capture border toggle ─────────────────────────────────────────────────

    private void OnCaptureBorderToggled(object sender, RoutedEventArgs e)
    {
        UpdateCaptureBorderWarning();
    }

    private void UpdateCaptureBorderWarning()
    {
        BorderSuppressedWarning.Visibility =
            viewModel.ShowBorderSuppressedWarning ? Visibility.Visible : Visibility.Collapsed;
    }

    // ── Component enable toggles ──────────────────────────────────────────────

    private void SyncComponentToggles()
    {
        RecordingEnabledToggle.IsOn = viewModel.RecordingEnabled;
        ReplayBufferEnabledToggle.IsOn = viewModel.ReplayBufferEnabled;
    }

    private void OnRecordingEnabledToggled(object sender, RoutedEventArgs e)
    {
        viewModel.RecordingEnabled = RecordingEnabledToggle.IsOn;
    }

    private void OnReplayBufferEnabledToggled(object sender, RoutedEventArgs e)
    {
        viewModel.ReplayBufferEnabled = ReplayBufferEnabledToggle.IsOn;
    }

    // ── Corrupt config InfoBar ────────────────────────────────────────────────

    private void UpdateCorruptConfigBar()
    {
        CorruptConfigBar.IsOpen = viewModel.LoadWarning is not null;
    }

    private void OnRecordingFormatSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressComboBoxEvents)
            return;

        if (RecordingFormatComboBox.SelectedItem is ComboBoxItem item && item.Tag is string container)
            viewModel.RecordingContainer = container;
    }

    private void OnClipFormatSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressComboBoxEvents)
            return;

        if (ClipFormatComboBox.SelectedItem is ComboBoxItem item && item.Tag is string container)
            viewModel.ClipContainer = container;
    }

    private void OnAudioModeSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressAudioEvents)
            return;

        if (AudioModeComboBox.SelectedItem is ComboBoxItem item && item.Tag is string mode)
            viewModel.AudioMode = mode;
    }

    private void OnPrimaryMicSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressAudioEvents)
            return;

        if (PrimaryMicComboBox.SelectedItem is ComboBoxItem item && item.Tag is string deviceId)
            viewModel.PrimaryMicrophoneDeviceId = deviceId;
    }

    private void OnAddAudioSource(object sender, RoutedEventArgs e)
    {
        if (AddAudioSourceComboBox.SelectedItem is not ComboBoxItem item)
            return;

        switch (item.Tag)
        {
            case string tag when tag == "desktop":
                viewModel.AddDesktopAudioSource();
                break;
            case string tag when tag == "active_game":
                viewModel.AddActiveGameSource();
                break;
            case AudioDeviceInfo device:
                viewModel.AddInputDeviceSource(device);
                break;
            case AudioSessionInfo session:
                viewModel.AddProcessSource(session);
                break;
        }

        RefreshAudioSourcesList();
    }

    private void OnRemoveAudioSource(object sender, RoutedEventArgs e)
    {
        AudioSourceData? source = SelectedAudioSource();
        if (source is null)
            return;
        viewModel.RemoveAudioSource(source);
        RefreshAudioSourcesList();
    }

    private void OnAudioSourceSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        RefreshSelectedAudioSourceEditor();
    }

    private void OnAudioSourceEnabledToggled(object sender, RoutedEventArgs e)
    {
        if (suppressAudioEvents)
            return;

        AudioSourceData? source = SelectedAudioSource();
        if (source is null)
            return;
        viewModel.SetAudioSourceEnabled(source, AudioSourceEnabledToggle.IsOn);
        RefreshAudioSourcesList();
    }

    private void OnAudioTrackChecked(object sender, RoutedEventArgs e)
    {
        if (suppressAudioEvents || sender is not CheckBox box || box.Tag is not string tagText)
            return;

        if (!int.TryParse(tagText, out int track))
            return;

        AudioSourceData? source = SelectedAudioSource();
        if (source is null)
            return;
        viewModel.SetAudioSourceTrack(source, track, box.IsChecked == true);
        RefreshAudioSourcesList();
    }

    // Hotkey capture

    private void OnHotkeyBoxGotFocus(object sender, RoutedEventArgs e)
    {
        if (sender is not TextBox textBox || textBox.Tag is not string target)
            return;

        textBox.SelectAll();
        hotkeyCaptureTarget = target;
        StartHotkeyCapture();
    }

    private void OnHotkeyBoxLostFocus(object sender, RoutedEventArgs e)
    {
        if (sender is TextBox textBox && Equals(textBox.Tag, hotkeyCaptureTarget))
            StopHotkeyCapture();
    }

    private void StartHotkeyCapture()
    {
        if (hotkeyCaptureHook != 0)
            return;

        hotkeyCaptureDownKeys.Clear();
        hotkeyCaptureProc = HotkeyCaptureHook;
        hotkeyCaptureHook = SetWindowsHookExW(
            WH_KEYBOARD_LL,
            hotkeyCaptureProc,
            GetModuleHandleW(null),
            0);
    }

    private void StopHotkeyCapture()
    {
        if (hotkeyCaptureHook != 0)
        {
            UnhookWindowsHookEx(hotkeyCaptureHook);
            hotkeyCaptureHook = 0;
        }

        hotkeyCaptureProc = null;
        hotkeyCaptureTarget = null;
        hotkeyCaptureDownKeys.Clear();
    }

    private nint HotkeyCaptureHook(int nCode, nint wParam, nint lParam)
    {
        if (nCode < 0)
            return CallNextHookEx(hotkeyCaptureHook, nCode, wParam, lParam);

        int message = wParam.ToInt32();
        if (message is not (WM_KEYDOWN or WM_SYSKEYDOWN or WM_KEYUP or WM_SYSKEYUP))
            return 1;

        int triggerKey = Marshal.ReadInt32(lParam);
        if (message is WM_KEYUP or WM_SYSKEYUP)
        {
            hotkeyCaptureDownKeys.Remove(triggerKey);
            return 1;
        }

        hotkeyCaptureDownKeys.Add(triggerKey);
        string? hotkey = BuildHotkey(hotkeyCaptureDownKeys);
        if (hotkey is null)
            return 1;

        DispatcherQueue.TryEnqueue(() => ApplyCapturedHotkey(hotkey));
        return 1;
    }

    private void ApplyCapturedHotkey(string hotkey)
    {
        if (hotkeyCaptureTarget is null)
            return;

        switch (hotkeyCaptureTarget)
        {
            case "SaveReplayHotkey":
                viewModel.SaveReplayHotkey = hotkey;
                break;
            case "RecordingStartHotkey":
                viewModel.RecordingStartHotkey = hotkey;
                break;
            case "RecordingStopHotkey":
                viewModel.RecordingStopHotkey = hotkey;
                break;
            case "PauseResumeHotkey":
                viewModel.PauseResumeHotkey = hotkey;
                break;
        }
    }

    private static string? BuildHotkey(IReadOnlySet<int> downKeys)
    {
        List<string> parts = new();
        if (HasAnyKey(downKeys, 0x11, 0xA2, 0xA3))
            parts.Add("Ctrl");
        if (HasAnyKey(downKeys, 0x10, 0xA0, 0xA1))
            parts.Add("Shift");
        if (HasAnyKey(downKeys, 0x12, 0xA4, 0xA5))
            parts.Add("Alt");
        if (HasAnyKey(downKeys, 0x5B, 0x5C))
            parts.Add("Win");

        bool hasNonModifierKey = false;
        foreach ((int key, string name) in HotkeyKeyNames)
        {
            if (downKeys.Contains(key))
            {
                if (!parts.Contains(name, StringComparer.OrdinalIgnoreCase))
                    parts.Add(name);
                hasNonModifierKey = true;
            }
        }

        if (!hasNonModifierKey)
            return null;

        return string.Join("+", parts);
    }

    private static bool HasAnyKey(IReadOnlySet<int> downKeys, params int[] keys)
    {
        foreach (int key in keys)
        {
            if (downKeys.Contains(key))
                return true;
        }

        return false;
    }

    private static readonly (int Key, string Name)[] HotkeyKeyNames =
    {
        (0x09, "Tab"), (0x0D, "Enter"), (0x1B, "Esc"), (0x20, "Space"), (0x08, "Backspace"),
        (0x2D, "Insert"), (0x2E, "Delete"), (0x24, "Home"), (0x23, "End"),
        (0x21, "PageUp"), (0x22, "PageDown"), (0x26, "Up"), (0x28, "Down"),
        (0x25, "Left"), (0x27, "Right"),
        ('A', "A"), ('B', "B"), ('C', "C"), ('D', "D"), ('E', "E"), ('F', "F"),
        ('G', "G"), ('H', "H"), ('I', "I"), ('J', "J"), ('K', "K"), ('L', "L"),
        ('M', "M"), ('N', "N"), ('O', "O"), ('P', "P"), ('Q', "Q"), ('R', "R"),
        ('S', "S"), ('T', "T"), ('U', "U"), ('V', "V"), ('W', "W"), ('X', "X"),
        ('Y', "Y"), ('Z', "Z"),
        ('0', "0"), ('1', "1"), ('2', "2"), ('3', "3"), ('4', "4"),
        ('5', "5"), ('6', "6"), ('7', "7"), ('8', "8"), ('9', "9"),
        (0x60, "Numpad0"), (0x61, "Numpad1"), (0x62, "Numpad2"), (0x63, "Numpad3"),
        (0x64, "Numpad4"), (0x65, "Numpad5"), (0x66, "Numpad6"), (0x67, "Numpad7"),
        (0x68, "Numpad8"), (0x69, "Numpad9"), (0x6A, "NumpadMultiply"),
        (0x6B, "NumpadAdd"), (0x6D, "NumpadSubtract"), (0x6E, "NumpadDecimal"),
        (0x6F, "NumpadDivide"),
        (0x70, "F1"), (0x71, "F2"), (0x72, "F3"), (0x73, "F4"), (0x74, "F5"),
        (0x75, "F6"), (0x76, "F7"), (0x77, "F8"), (0x78, "F9"), (0x79, "F10"),
        (0x7A, "F11"), (0x7B, "F12"), (0x7C, "F13"), (0x7D, "F14"), (0x7E, "F15"),
        (0x7F, "F16"), (0x80, "F17"), (0x81, "F18"), (0x82, "F19"), (0x83, "F20"),
        (0x84, "F21"), (0x85, "F22"), (0x86, "F23"), (0x87, "F24"),
        (0xBA, ";"), (0xBB, "="), (0xBC, ","), (0xBD, "-"), (0xBE, "."),
        (0xBF, "/"), (0xC0, "`"), (0xDB, "["), (0xDC, "\\"), (0xDD, "]"), (0xDE, "'"),
    };

    // ── Output folder browse / reset ──────────────────────────────────────────

    private async void OnBrowseClips(object sender, RoutedEventArgs e)
    {
        string? path = await PickFolder();
        if (path is not null)
            viewModel.SetClipsDirectory(path);
    }

    private async void OnBrowseRecordings(object sender, RoutedEventArgs e)
    {
        string? path = await PickFolder();
        if (path is not null)
            viewModel.SetRecordingsDirectory(path);
    }

    private void OnResetClips(object sender, RoutedEventArgs e)
    {
        viewModel.ResetClipsDirectory();
    }

    private void OnResetRecordings(object sender, RoutedEventArgs e)
    {
        viewModel.ResetRecordingsDirectory();
    }

    private async Task<string?> PickFolder()
    {
        FolderPicker picker = new();
        picker.FileTypeFilter.Add("*");
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));

        Windows.Storage.StorageFolder? folder = await picker.PickSingleFolderAsync();
        return folder?.Path;
    }

    // ── Save / Cancel ─────────────────────────────────────────────────────────

    private void OnSave(object sender, RoutedEventArgs e)
    {
        // Do NOT close on save — just show the status bar message.
        viewModel.Save();
    }

    private void OnCancel(object sender, RoutedEventArgs e)
    {
        StopHotkeyCapture();
        closeAllowed = true;
        Close();
    }

    private async void OnAppWindowClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (closeAllowed || !viewModel.HasUnsavedChanges)
        {
            StopHotkeyCapture();
            return;
        }

        args.Cancel = true;
        ContentDialogResult result = await ConfirmClose();
        if (result == ContentDialogResult.None)
            return;

        if (result == ContentDialogResult.Primary && !viewModel.Save())
            return;

        StopHotkeyCapture();
        closeAllowed = true;
        Close();
    }

    private async Task<ContentDialogResult> ConfirmClose()
    {
        ContentDialog dialog = new()
        {
            XamlRoot = Content.XamlRoot,
            Title = "Unsaved Changes",
            Content = "Save settings changes before closing?",
            PrimaryButtonText = "Save",
            SecondaryButtonText = "Discard",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        return await dialog.ShowAsync();
    }
}
