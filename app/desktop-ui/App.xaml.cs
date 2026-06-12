using Microsoft.UI.Xaml;
using Monolith.Settings.Services;
using System.Diagnostics;

namespace Monolith.Settings;

public sealed partial class App : Application
{
    private Window? window;
    private static Mutex? singleInstanceMutex;

    public App()
    {
        StartupTrace.MarkProcessStart();
        Stopwatch sw = Stopwatch.StartNew();
        RequestedTheme = ApplicationTheme.Dark;
        InitializeComponent();
        StartupTrace.MarkDuration("App.InitializeComponent", sw.ElapsedMilliseconds);
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        StartupTrace.Mark("App.OnLaunched");
        singleInstanceMutex = new Mutex(initiallyOwned: true, "Monolith.Settings_SingleInstance", out bool createdNew);
        if (!createdNew)
        {
            Environment.Exit(0);
            return;
        }

        Stopwatch sw = Stopwatch.StartNew();
        window = new MainWindow();
        StartupTrace.MarkDuration("MainWindow constructor", sw.ElapsedMilliseconds);
        window.Activate();
        StartupTrace.Mark("Window.Activate");
    }
}
