using System.Windows;

namespace VRX.Desktop;
public partial class App : Application
{
    private DesktopInstance? desktopInstance;
    protected override void OnExit(ExitEventArgs e)
    {
        desktopInstance?.Dispose();
        base.OnExit(e);
    }
    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        if(e.Args.Length==4 && e.Args[0]=="--desktop-instance-fixture") {
            Shutdown(DesktopInstanceTests.Fixture(e.Args[1],e.Args[2],e.Args[3]=="hold"));return;
        }
        if(e.Args.FirstOrDefault()?.StartsWith("--steam-bootstrap-test=",StringComparison.Ordinal)==true) {
            ShutdownMode=ShutdownMode.OnExplicitShutdown;
            var childStart=new System.Diagnostics.ProcessStartInfo(e.Args[0][23..]) {UseShellExecute=false,WorkingDirectory=Environment.CurrentDirectory};
            foreach(string arg in e.Args.Skip(1)) childStart.ArgumentList.Add(arg);
            using var child=System.Diagnostics.Process.Start(childStart)!; await child.WaitForExitAsync(); Shutdown(child.ExitCode);return;
        }
        if(e.Args.FirstOrDefault()?.StartsWith("--steam-child-test=",StringComparison.Ordinal)==true) {
            System.IO.File.WriteAllText(e.Args[0][19..],System.Text.Json.JsonSerializer.Serialize(new {Arguments=e.Args,Directory=Environment.CurrentDirectory,Marker=Environment.GetEnvironmentVariable("VRX_STEAM_TEST_MARKER")}));
            Shutdown();return;
        }
        if(e.Args.Length>=3 && e.Args[0]=="--steam-game" && e.Args[2]=="--") {
            ShutdownMode=ShutdownMode.OnExplicitShutdown;
            Shutdown(await SteamGeometry.Run(e.Args[1],e.Args.Skip(3).ToArray())); return;
        }
        if (e.Args.FirstOrDefault(a=>a.StartsWith("--session-lifecycle-fixture=",StringComparison.Ordinal)) is {} fixture) {
            Shutdown(GeometrySessionLifecycleTests.Fixture(fixture[28..])); return;
        }
        System.Diagnostics.Debug.WriteLine($"[App] OnStartup: {string.Join(' ', e.Args)}");
        if(!e.Args.Contains("--smoke-test")) {
            desktopInstance=new DesktopInstance();
            if(!desktopInstance.IsOwner) {Shutdown();return;}
        }
        // Before any window exists, so every string is transformed from the start.
        if (e.Args.Contains("--pseudo-locale")) Loc.SetPseudo(true);
        var window = new MainWindow(e.Args.Contains("--smoke-test"));
        MainWindow = window;
        window.Show();
        desktopInstance?.Listen(()=>{
            if(Dispatcher.HasShutdownStarted) return;
            _=Dispatcher.BeginInvoke(new Action(()=>{
                if(Dispatcher.HasShutdownStarted) return;
                if(window.WindowState==WindowState.Minimized) window.WindowState=WindowState.Normal;
                window.Show();window.Activate();
                RunningApps.SetForegroundWindow(new System.Windows.Interop.WindowInteropHelper(window).Handle);
            }));
        });
    }
}
