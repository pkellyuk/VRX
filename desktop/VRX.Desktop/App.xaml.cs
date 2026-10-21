using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Threading;

namespace VRX.Desktop;
public partial class App : Application
{
    private DesktopInstance? desktopInstance;
    private bool smokeTest, showingError;

    // Unexpected errors are appended here (and to the debugger output), so a report can say
    // what went wrong. Kept small: it is rewritten once it passes MaxErrorLogBytes.
    internal static string ErrorLogFile => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VRX", "logs", "errors.log");
    private const long MaxErrorLogBytes = 1 << 20;

    protected override void OnExit(ExitEventArgs e)
    {
        desktopInstance?.Dispose();
        base.OnExit(e);
    }
    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        Debug.WriteLine($"[App] OnStartup: {string.Join(' ', e.Args)}");
        smokeTest = e.Args.Contains("--smoke-test");
        InstallErrorHandlers();
#if DEBUG
        // Fixtures for the smoke test's child processes. Never part of a release build.
        if (await RunDevelopmentFixture(e.Args)) return;
#endif
        if (e.Args.FirstOrDefault()?.StartsWith(GeometryGuard.Switch, StringComparison.Ordinal) == true)
        {
            // The cleanup guard for one geometry run: no window, independent of the desktop.
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            Shutdown(await GeometryGuard.Run(e.Args[0][GeometryGuard.Switch.Length..]));
            return;
        }
        if(e.Args.Length>=3 && e.Args[0]=="--steam-game" && e.Args[2]=="--") {
            ShutdownMode=ShutdownMode.OnExplicitShutdown;
            Shutdown(await SteamGeometry.Run(e.Args[1],e.Args.Skip(3).ToArray())); return;
        }
        if(!smokeTest) {
            desktopInstance=new DesktopInstance();
            if(!desktopInstance.IsOwner) {Shutdown();return;}
            // Steam true-geometry launches go to the copy started last (SteamLauncher).
            SteamLauncher.Refresh();
        }
        // Before any window exists, so every string is transformed from the start.
        if (e.Args.Contains("--pseudo-locale")) Loc.SetPseudo(true);
        var window = new MainWindow(smokeTest);
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
        Debug.WriteLine("[App] OnStartup exit: desktop shown");
    }

#if DEBUG
    // The smoke test starts copies of this executable with these switches. True when one ran.
    private async Task<bool> RunDevelopmentFixture(string[] args)
    {
        ArgumentNullException.ThrowIfNull(args);
        if(args.Length==4 && args[0]=="--desktop-instance-fixture") {
            Debug.WriteLine("[App] desktop instance fixture");
            Shutdown(DesktopInstanceTests.Fixture(args[1],args[2],args[3]=="hold"));return true;
        }
        if(args.FirstOrDefault()?.StartsWith("--steam-bootstrap-test=",StringComparison.Ordinal)==true) {
            Debug.WriteLine("[App] Steam bootstrap fixture");
            ShutdownMode=ShutdownMode.OnExplicitShutdown;
            var childStart=new ProcessStartInfo(args[0][23..]) {UseShellExecute=false,WorkingDirectory=Environment.CurrentDirectory};
            foreach(string arg in args.Skip(1)) childStart.ArgumentList.Add(arg);
            using var child=Process.Start(childStart)!; await child.WaitForExitAsync(); Shutdown(child.ExitCode);return true;
        }
        if(args.FirstOrDefault()?.StartsWith("--steam-child-test=",StringComparison.Ordinal)==true) {
            Debug.WriteLine("[App] Steam child fixture");
            File.WriteAllText(args[0][19..],System.Text.Json.JsonSerializer.Serialize(new {Arguments=args,Directory=Environment.CurrentDirectory,Marker=Environment.GetEnvironmentVariable("VRX_STEAM_TEST_MARKER")}));
            Shutdown();return true;
        }
        if (args.FirstOrDefault(a=>a.StartsWith("--session-lifecycle-fixture=",StringComparison.Ordinal)) is {} fixture) {
            Debug.WriteLine("[App] session lifecycle fixture");
            Shutdown(GeometrySessionLifecycleTests.Fixture(fixture[28..])); return true;
        }
        return false;
    }
#endif

    // Anything that escapes a handler is logged and shown in plain words instead of closing
    // VRX: the UI thread's exceptions are handled, unobserved task exceptions are observed,
    // and a fatal one on another thread is at least logged before Windows ends the process.
    private void InstallErrorHandlers()
    {
        DispatcherUnhandledException += OnDispatcherUnhandledException;
        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            LogUnexpected("task", e.Exception);
            e.SetObserved();
        };
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            LogUnexpected(e.IsTerminating ? "fatal" : "thread", e.ExceptionObject as Exception ?? new Exception(e.ExceptionObject?.ToString()));
        Debug.WriteLine("[App] error handlers installed");
    }

    private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        ArgumentNullException.ThrowIfNull(e);
        LogUnexpected("UI", e.Exception);
        if (smokeTest)
        {
            // The smoke test reports, it never waits for someone to close a message.
            Debug.WriteLine("[App] unexpected error during the smoke test; leaving it to fail");
            return;
        }
        e.Handled = true;
        if (showingError) return;                  // one message at a time
        showingError = true;
        try
        {
            MessageBox.Show(MainWindow, Loc.Format("ErrorUnexpected", e.Exception.Message, ErrorLogFile), Loc.Get("ErrorUnexpectedTitle"),
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
        catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            Debug.WriteLine("[App] could not show the error: " + ex.Message);
        }
        finally { showingError = false; }
    }

    internal static void LogUnexpected(string source, Exception exception)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(exception);
        Debug.WriteLine($"[App] unexpected {source} error: {exception}");
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(ErrorLogFile)!);
            if (File.Exists(ErrorLogFile) && new FileInfo(ErrorLogFile).Length > MaxErrorLogBytes) File.Delete(ErrorLogFile);
            File.AppendAllText(ErrorLogFile, $"{DateTimeOffset.Now:O} [{source}] {VRX.Desktop.MainWindow.DisplayVersion()} {exception}{Environment.NewLine}{Environment.NewLine}");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Debug.WriteLine("[App] could not write the error log: " + ex.Message);
        }
    }
}
