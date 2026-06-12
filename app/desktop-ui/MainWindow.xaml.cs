using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Monolith.Settings.Models;
using Monolith.Settings.ViewModels;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace Monolith.Settings;

public sealed partial class MainWindow : Window
{
    private readonly SettingsViewModel viewModel = new();
    private AppWindow? appWindow;
    private bool closeAllowed;

    // Suppress programmatic ComboBox changes from triggering ViewModel updates.
    private bool suppressComboBoxEvents;

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
    };

    public MainWindow()
    {
        InitializeComponent();
        RootLayout.DataContext = viewModel;
        viewModel.Load();
        ConfigureWindow();
        PopulateCaptureCombos();
        UpdateCaptureBorderWarning();
        UpdateCorruptConfigBar();
    }

    // ── Window setup ─────────────────────────────────────────────────────────

    private void ConfigureWindow()
    {
        nint hwnd = WindowNative.GetWindowHandle(this);
        WindowId windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(hwnd);
        appWindow = AppWindow.GetFromWindowId(windowId);
        appWindow.Title = "Monolith Settings";
        appWindow.Resize(new Windows.Graphics.SizeInt32(1160, 760));
        appWindow.Closing += OnAppWindowClosing;
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
                    tagToSelect = tag;
                    matchedPreset = true;
                    // Update ViewModel to treat as preset (mode source with preset size)
                    // Actually spec says: when loading a config whose custom size matches a preset, select the preset
                    // But the mode is "custom" — we select the preset item but keep viewmodel in sync
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

    // ── Corrupt config InfoBar ────────────────────────────────────────────────

    private void UpdateCorruptConfigBar()
    {
        CorruptConfigBar.IsOpen = viewModel.LoadWarning is not null;
    }

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
        closeAllowed = true;
        Close();
    }

    private async void OnAppWindowClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (closeAllowed || !viewModel.HasUnsavedChanges)
            return;

        args.Cancel = true;
        ContentDialogResult result = await ConfirmClose();
        if (result == ContentDialogResult.None)
            return;

        if (result == ContentDialogResult.Primary && !viewModel.Save())
            return;

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
