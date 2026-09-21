using System.Windows;

namespace VRX.Desktop;
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        System.Diagnostics.Debug.WriteLine($"[App] OnStartup: {string.Join(' ', e.Args)}");
        // Before any window exists, so every string is transformed from the start.
        if (e.Args.Contains("--pseudo-locale")) Loc.SetPseudo(true);
        var window = new MainWindow(e.Args.Contains("--smoke-test"));
        MainWindow = window;
        window.Show();
    }
}
