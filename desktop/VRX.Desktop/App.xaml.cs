using System.Windows;
using System.IO;

namespace VRX.Desktop;
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        System.Diagnostics.Debug.WriteLine($"[App] OnStartup: {string.Join(' ', e.Args)}");
        bool smoke = e.Args.Contains("--smoke-test");
        string? language = e.Args.FirstOrDefault(a => a.StartsWith("--language=", StringComparison.Ordinal))?[11..];
        language ??= smoke ? "en" : new ProfileStore(Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VRX")).LoadAppSettings().UiLanguage;
        Loc.ConfigureLanguage(language);
        // Before any window exists, so every string is transformed from the start.
        if (e.Args.Contains("--pseudo-locale")) Loc.SetPseudo(true);
        var window = new MainWindow(smoke);
        MainWindow = window;
        window.Show();
    }
    protected override void OnExit(ExitEventArgs e)
    {
        FloatingBrowserRuntime.Shutdown(); base.OnExit(e);
    }
}
