using System.Diagnostics;
using System.IO;

namespace VRX.Desktop;

// A small detached process (VRX.Desktop.exe --geometry-guard=<run folder>, no window) that
// outlives VRX: it waits for the run's game - and anything else started from the game's
// folder, such as a Steam DRM stub restarting the same executable - to exit, then removes
// exactly the files the run installed, each only if it is still unchanged (by hash). It does
// not inject anything or touch the game process. Recover at VRX's startup stays as a second
// safety net.
internal static class GeometryGuard
{
    public const string Switch = "--geometry-guard=";

    // How long the game may take to appear after the guard starts (Steam can show dialogs
    // first), and how long the game's folder must stay quiet before cleaning (long enough for
    // a DRM relaunch to have started).
    public static readonly TimeSpan StartupWait = TimeSpan.FromMinutes(5), RelaunchGrace = TimeSpan.FromSeconds(10), Poll = TimeSpan.FromSeconds(2);

    private static string MutexName(string runRoot) => @"Local\VRX.geometry.guard." + Path.GetFileName(Path.TrimEndingDirectorySeparator(runRoot));

    // Starts the guard for a run. A failure is logged, not fatal: the in-process observer and
    // Recover at the next startup still clean up.
    public static void Start(string runRoot)
    {
        ArgumentException.ThrowIfNullOrEmpty(runRoot);
        Debug.WriteLine($"[Geometry guard] Start enter: {runRoot}");
        string? executable = Environment.ProcessPath;
        if (executable == null || !Path.GetFileNameWithoutExtension(executable).Equals("VRX.Desktop", StringComparison.OrdinalIgnoreCase))
        {
            Debug.WriteLine($"[Geometry guard] Start exit: not running as VRX.Desktop ({executable}); no guard");
            return;
        }
        try
        {
            var start = new ProcessStartInfo(executable) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = AppContext.BaseDirectory };
            start.ArgumentList.Add(Switch + runRoot);
            using var guard = Process.Start(start);
            Debug.WriteLine($"[Geometry guard] Start exit: guard pid {guard?.Id}");
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            Debug.WriteLine($"[Geometry guard] Start failed: {ex.Message}");
        }
    }

    // True while a guard is waiting on this run (its mutex is held).
    public static bool Waiting(string runRoot)
    {
        ArgumentException.ThrowIfNullOrEmpty(runRoot);
        try
        {
            using var marker = EventWaitHandle.OpenExisting(MutexName(runRoot));
            return true;
        }
        catch (WaitHandleCannotBeOpenedException) { return false; }
        catch (UnauthorizedAccessException) { return true; }
    }

    // Only run folders of a VRX data root (...\geometry\runs\<id>) are accepted.
    public static bool ValidRunFolder(string runRoot)
    {
        if (string.IsNullOrWhiteSpace(runRoot) || !Path.IsPathFullyQualified(runRoot)) return false;
        var folder = new DirectoryInfo(Path.TrimEndingDirectorySeparator(Path.GetFullPath(runRoot)));
        return folder.Exists && folder.Parent?.Name.Equals("runs", StringComparison.OrdinalIgnoreCase) == true &&
            folder.Parent.Parent?.Name.Equals("geometry", StringComparison.OrdinalIgnoreCase) == true &&
            File.Exists(Path.Combine(folder.FullName, "run.json"));
    }

    // The guard process's body. 0 when the run ended retired (or another guard has it).
    public static Task<int> Run(string runRoot) => Run(runRoot, StartupWait, RelaunchGrace, Poll, CancellationToken.None);

    internal static async Task<int> Run(string runRoot, TimeSpan startupWait, TimeSpan grace, TimeSpan poll, CancellationToken token)
    {
        Debug.WriteLine($"[Geometry guard] Run enter: {runRoot}, startup {startupWait}, grace {grace}");
        if (!ValidRunFolder(runRoot))
        {
            Debug.WriteLine("[Geometry guard] Run exit: not a geometry run folder");
            return 2;
        }
        // A named marker (not a mutex: this method awaits, and a mutex belongs to one thread)
        // that exists while a guard watches the run; Windows removes it when the guard exits.
        using var marker = new EventWaitHandle(false, EventResetMode.ManualReset, MutexName(runRoot), out bool created);
        if (!created)
        {
            Debug.WriteLine("[Geometry guard] Run exit: another guard already watches this run");
            return 0;
        }
        {
            var clock = Stopwatch.StartNew();
            TimeSpan lastSeen = TimeSpan.Zero;
            bool seen = false;
            int failures = 0;
            while (true)
            {
                token.ThrowIfCancellationRequested();
                if (GeometryLaunch.Retired(runRoot))
                {
                    Debug.WriteLine("[Geometry guard] Run exit: VRX already removed this run's files");
                    return 0;
                }
                var run = GeometryLaunch.ReadRun(runRoot);
                if (run == null)
                {
                    Debug.WriteLine("[Geometry guard] Run exit: run.json unreadable");
                    return 1;
                }
                // A run with a process ID has started its game, even if that one already exited.
                if (run.Pid != 0 && !seen) { seen = true; lastSeen = clock.Elapsed; }
                if (GeometryLaunch.GameFolderBusy(run))
                {
                    seen = true;
                    lastSeen = clock.Elapsed;
                }
                else if (seen ? clock.Elapsed - lastSeen >= grace : clock.Elapsed >= startupWait)
                {
                    Debug.WriteLine($"[Geometry guard] game gone (seen {seen}) after {clock.Elapsed}; cleaning up");
                    GeometryLaunch.Cleanup(runRoot);
                    bool retired = GeometryLaunch.Retired(runRoot);
                    Debug.WriteLine($"[Geometry guard] Run exit: retired {retired}");
                    if (retired) return 0;
                    // Something started in the folder at the last moment, or the files could
                    // not be removed yet (locked): try again a few times.
                    if (++failures >= 5)
                    {
                        Debug.WriteLine("[Geometry guard] Run exit: cleanup keeps failing; Recover at the next start will retry");
                        return 1;
                    }
                    lastSeen = clock.Elapsed;
                    seen = true;
                }
                await Task.Delay(poll, token);
            }
        }
    }
}
