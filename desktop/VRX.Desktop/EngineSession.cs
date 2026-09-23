using System.Diagnostics;
using System.IO;

namespace VRX.Desktop;
public sealed class EngineSession
{
    private Process? process;
    private string control = "";
    private uint recenter, menu;
    public bool Running => process != null;
    public event Action<string>? Log;
    public event Action<int>? Exited;
    public static string RepositoryRoot()
    {
        if (File.Exists(Path.Combine(AppContext.BaseDirectory, "engine", "xrplayer.exe"))) return AppContext.BaseDirectory;
        for (DirectoryInfo? dir = new(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "bench", "native", "openxr", "xrapp5.cpp"))) return dir.FullName;
        throw new DirectoryNotFoundException(Loc.Get("ErrorRendererMissing"));
    }
    public static string EnginePath()
    {
        string root = RepositoryRoot();
        string engine = Path.Combine(root, "engine", "xrplayer.exe");
        if (!File.Exists(engine)) engine = Path.Combine(root, "bench", "native", "openxr", "out", "xrplayer.exe");
        if (!File.Exists(engine)) throw new FileNotFoundException(Loc.Get("ErrorBuildRenderer"));
        return engine;
    }
    public void Update(Profile profile, bool reset = false, bool dismiss = false, bool stop = false)
    {
        if (reset) recenter++;
        if (dismiss) menu++;
        if (control.Length > 0) ProfileStore.AtomicWrite(control, profile.Control(recenter, menu, stop));
    }
    public void Start(RunningApp app, GameWindow window, Profile profile, string dataRoot)
    {
        if (Running) throw new InvalidOperationException(Loc.Get("ErrorStopFirst"));
        foreach (string name in new[] { "xrapp5", "xrplayer" })
        {
            var existing = Process.GetProcessesByName(name);
            bool found = existing.Length > 0;
            foreach (var item in existing) item.Dispose();
            if (found) throw new InvalidOperationException(Loc.Get("ErrorAnotherSession"));
        }
        bool screen = window.Monitor >= 0;
        if (screen)
        {
            // A display: it must still be the display that was listed (same place in the order).
            var now = ScreenSources.List();
            if (window.Monitor >= now.Count || now[window.Monitor].Handle != window.Handle)
                throw new InvalidOperationException(Loc.Get("ErrorScreenChanged"));
        }
        else if (!string.Equals(RunningApps.ProcessPath(app.Pid), app.FullPath, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException(Loc.Get("ErrorProcessChanged"));
        string root = RepositoryRoot();
        string engine = EnginePath();
        string sessionRoot = Path.Combine(dataRoot, "sessions", Guid.NewGuid().ToString("N"));
        control = Path.Combine(sessionRoot, "control.txt"); recenter = menu = 0;
        Update(profile);
        var start = new ProcessStartInfo(engine) { WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = System.Text.Encoding.UTF8,
            StandardErrorEncoding = System.Text.Encoding.UTF8 };
        foreach (string arg in Arguments(app, window, profile, control)) start.ArgumentList.Add(arg);
        var child = new Process { StartInfo = start };
        Debug.WriteLine($"[Engine] start {engine} for {(screen ? "display " + window.Monitor : app.FullPath)} pid {app.Pid}: {string.Join(' ', start.ArgumentList)}");
        try
        {
            child.Start(); process = child;
            Debug.WriteLine($"[Engine] started pid {child.Id}");
            _ = Observe(child, sessionRoot);
            if (!screen) RunningApps.SetForegroundWindow(window.Handle);
        }
        catch (System.ComponentModel.Win32Exception ex) when (BlockedMessage(ex.NativeErrorCode) is { } blocked)
        {
            Debug.WriteLine($"[Engine] Windows refused to run the engine: error {ex.NativeErrorCode}, {ex.Message}");
            child.Dispose(); process = null;
            throw new InvalidOperationException(blocked, ex);
        }
        catch { child.Dispose(); process = null; throw; }
    }
    // The engine's command line: a game's window by its process and handle, or a whole
    // display by its place in Windows' display order (--monitor=N).
    public static List<string> Arguments(RunningApp app, GameWindow window, Profile profile, string controlFile)
    {
        ArgumentNullException.ThrowIfNull(app);
        ArgumentNullException.ThrowIfNull(window);
        ArgumentNullException.ThrowIfNull(profile);
        ArgumentException.ThrowIfNullOrEmpty(controlFile);
        // Culture-invariant numbers: the engine parses these whatever the Windows language.
        var invariant = System.Globalization.CultureInfo.InvariantCulture;
        var args = new List<string> { "0" };
        if (window.Monitor >= 0) args.Add("--monitor=" + window.Monitor.ToString(invariant));
        else args.AddRange(["--exe=" + Path.GetFileName(app.FullPath), "--exe-path=" + app.FullPath,
            "--pid=" + app.Pid.ToString(invariant), "--hwnd=" + window.Handle.ToInt64().ToString(invariant)]);
        args.Add("--control=" + controlFile);
        if (profile.DepthGpu != Gpus.Same && Gpus.ValidId(profile.DepthGpu)) args.Add("--depth-gpu=" + profile.DepthGpu);
        return args;
    }
    // Windows refusing to run the engine, in plain words: Smart App Control or App Control for
    // Business (4551), AppLocker or a software restriction policy (1260), antivirus (225, 226).
    // Null for any other start error, which is shown as Windows words it.
    public static string? BlockedMessage(int error) => error switch
    {
        4551 or 1260 => Loc.Get("ErrorEngineBlockedPolicy"),
        225 or 226 => Loc.Get("ErrorEngineBlockedAntivirus"),
        _ => null
    };
    private async Task Observe(Process child, string sessionRoot)
    {
        using var log = new StreamWriter(Path.Combine(sessionRoot, "engine.log"));
        var gate = new SemaphoreSlim(1);
        async Task Read(StreamReader stream)
        {
            while (await stream.ReadLineAsync() is { } line)
            {
                await gate.WaitAsync();
                try { await log.WriteLineAsync(line); await log.FlushAsync(); }
                finally { gate.Release(); }
                Log?.Invoke(line);
            }
        }
        await Task.WhenAll(Read(child.StandardOutput), Read(child.StandardError), child.WaitForExitAsync());
        int code = child.ExitCode;
        process = null; child.Dispose(); gate.Dispose();
        Exited?.Invoke(code);
    }
    public async Task Stop(Profile profile)
    {
        var child = process;
        if (child == null) return;
        Update(profile, stop: true);
        try { await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(8)); }
        catch (TimeoutException)
        {
            Log?.Invoke(Loc.Get("LogStopTimeout"));
            child.Kill(); await child.WaitForExitAsync();
        }
        catch (InvalidOperationException) { } // Observe already disposed the exited child.
    }
}
