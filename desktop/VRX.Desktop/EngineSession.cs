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
        for (DirectoryInfo? dir = new(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "bench", "native", "openxr", "xrapp5.cpp"))) return dir.FullName;
        throw new DirectoryNotFoundException("Keep the desktop application inside the VRX project folder.");
    }
    public void Update(Profile profile, bool reset = false, bool dismiss = false, bool stop = false)
    {
        if (reset) recenter++;
        if (dismiss) menu++;
        if (control.Length > 0) ProfileStore.AtomicWrite(control, profile.Control(recenter, menu, stop));
    }
    public void Start(RunningApp app, GameWindow window, Profile profile, string dataRoot)
    {
        if (Running) throw new InvalidOperationException("Stop the current session first.");
        foreach (string name in new[] { "xrapp5", "xrplayer" })
        {
            var existing = Process.GetProcessesByName(name);
            bool found = existing.Length > 0;
            foreach (var item in existing) item.Dispose();
            if (found) throw new InvalidOperationException("Another VRX session is running. Close that session before attaching here.");
        }
        if (!string.Equals(RunningApps.ProcessPath(app.Pid), app.FullPath, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The selected process has closed or changed. Refresh the list.");
        string root = RepositoryRoot();
        string engine = Path.Combine(root, "bench", "native", "openxr", "out", "xrplayer.exe");
        if (!File.Exists(engine)) throw new FileNotFoundException("Build the desktop renderer first using bench/native/openxr/build.bat --desktop.");
        string sessionRoot = Path.Combine(dataRoot, "sessions", Guid.NewGuid().ToString("N"));
        control = Path.Combine(sessionRoot, "control.txt"); recenter = menu = 0;
        Update(profile);
        var start = new ProcessStartInfo(engine) { WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = System.Text.Encoding.UTF8,
            StandardErrorEncoding = System.Text.Encoding.UTF8 };
        foreach (string arg in new[] { "0", "--exe=" + Path.GetFileName(app.FullPath), "--exe-path=" + app.FullPath,
            "--pid=" + app.Pid, "--hwnd=" + window.Handle.ToInt64(), "--control=" + control }) start.ArgumentList.Add(arg);
        var child = new Process { StartInfo = start };
        try
        {
            child.Start(); process = child;
            _ = Observe(child, sessionRoot);
            RunningApps.SetForegroundWindow(window.Handle);
        }
        catch { child.Dispose(); process = null; throw; }
    }
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
            Log?.Invoke("VRX did not stop in time; terminating its renderer. The game is unaffected.");
            child.Kill(); await child.WaitForExitAsync();
        }
        catch (InvalidOperationException) { } // Observe already disposed the exited child.
    }
}
