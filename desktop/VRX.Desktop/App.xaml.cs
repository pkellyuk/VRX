using System.Windows;
using System.IO;
using VRX.VrexBridge;

namespace VRX.Desktop;
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        System.Diagnostics.Debug.WriteLine($"[App] OnStartup: {string.Join(' ', e.Args)}");
        bool smoke = e.Args.Contains("--smoke-test");
        string? language = e.Args.FirstOrDefault(a => a.StartsWith("--language=", StringComparison.Ordinal))?[11..];
        language ??= smoke ? "en" : new ProfileStore(VrxDataDirectory.Root).LoadAppSettings().UiLanguage;
        Loc.ConfigureLanguage(language);
        // Before any window exists, so every string is transformed from the start.
        if (e.Args.Contains("--pseudo-locale")) Loc.SetPseudo(true);
        var window = new MainWindow(smoke);
        MainWindow = window;
        window.Show();
        if (!smoke && e.Args.Contains("--start-ubuntu"))
        {
            window.Dispatcher.InvokeAsync(() =>
            {
                try { VrexClient.Current.Send(new("start")); }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception)
                { window.Status.Text = ex.Message; }
            });
        }
    }
    protected override void OnExit(ExitEventArgs e)
    {
        VrexClient.Current.Stop();
        FloatingBrowserRuntime.Shutdown(); base.OnExit(e);
    }
}
