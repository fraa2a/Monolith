using Microsoft.UI.Xaml;

namespace Monolith.Settings;

public sealed partial class App : Application
{
    private Window? window;
    private static Mutex? singleInstanceMutex;

    public App()
    {
        RequestedTheme = ApplicationTheme.Dark;
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        singleInstanceMutex = new Mutex(initiallyOwned: true, "Monolith.Settings_SingleInstance", out bool createdNew);
        if (!createdNew)
        {
            Environment.Exit(0);
            return;
        }

        window = new MainWindow();
        window.Activate();
    }
}
