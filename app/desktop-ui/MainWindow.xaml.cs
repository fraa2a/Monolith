using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Monolith.Settings.ViewModels;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace Monolith.Settings;

public sealed partial class MainWindow : Window
{
    private readonly SettingsViewModel viewModel = new();
    private AppWindow? appWindow;
    private bool closeAllowed;

    public MainWindow()
    {
        InitializeComponent();
        RootLayout.DataContext = viewModel;
        viewModel.Load();
        ConfigureWindow();
    }

    private void ConfigureWindow()
    {
        nint hwnd = WindowNative.GetWindowHandle(this);
        WindowId windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(hwnd);
        appWindow = AppWindow.GetFromWindowId(windowId);
        appWindow.Title = "Monolith Settings";
        appWindow.Resize(new Windows.Graphics.SizeInt32(1160, 760));
        appWindow.Closing += OnAppWindowClosing;
    }

    private void OnNavSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is not NavigationViewItem item || item.Tag is not string page)
            return;

        BasicPage.Visibility = page == "Basic" ? Visibility.Visible : Visibility.Collapsed;
        OutputPage.Visibility = page == "Output" ? Visibility.Visible : Visibility.Collapsed;
        HotkeysPage.Visibility = page == "Hotkeys" ? Visibility.Visible : Visibility.Collapsed;
        viewModel.SetPage(page);
    }

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

    private async Task<string?> PickFolder()
    {
        FolderPicker picker = new();
        picker.FileTypeFilter.Add("*");
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));

        Windows.Storage.StorageFolder? folder = await picker.PickSingleFolderAsync();
        return folder?.Path;
    }

    private void OnSave(object sender, RoutedEventArgs e)
    {
        if (!viewModel.Save())
            return;

        closeAllowed = true;
        Close();
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
