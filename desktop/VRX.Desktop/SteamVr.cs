using System.Diagnostics;
using System.IO;
using System.Text.Json;
using Microsoft.Win32;

namespace VRX.Desktop;

// Starting SteamVR for Attach / Play. Only matters when SteamVR is the active OpenXR
// runtime: another runtime is left to itself. The decisions and the path handling are
// pure (and covered by the smoke test); only Launch and the process checks touch the
// system.
public static class SteamVr
{
    public const string SteamUrl = "steam://rungameid/250820";
    public const string ServerProcess = "vrserver";
    public const string CompositorProcess = "vrcompositor";
    public static readonly string[] StartingProcesses = ["vrstartup", "vrmonitor"];
    public static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(90);
    // vrserver alone is enough once it has been up this long without a compositor
    // (the engine itself still waits up to 30 s for the headset).
    public static readonly TimeSpan ServerGrace = TimeSpan.FromSeconds(15);
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(500);

    public enum Plan
    {
        StartEngine,   // not SteamVR's runtime, or SteamVR is already running
        WaitForStart,  // SteamVR is starting already: wait for it, don't start it again
        Launch,        // start SteamVR, then wait for it
        NotRunning     // SteamVR is needed but the game's profile says not to start it
    }

    public static Plan Decide(bool steamVrRuntime, bool serverRunning, bool starting, bool allowStart)
    {
        if (!steamVrRuntime) return Plan.StartEngine;
        if (serverRunning) return Plan.StartEngine;
        if (starting) return Plan.WaitForStart;
        return allowStart ? Plan.Launch : Plan.NotRunning;
    }

    // SteamVR's OpenXR manifest is steamxr_win64.json in the SteamVR folder.
    public static bool IsSteamVrRuntime(string? manifestPath)
    {
        if (string.IsNullOrWhiteSpace(manifestPath)) return false;
        string name = Path.GetFileName(manifestPath.Trim().Trim('"'));
        return name.StartsWith("steamxr_", StringComparison.OrdinalIgnoreCase) &&
            name.EndsWith(".json", StringComparison.OrdinalIgnoreCase);
    }

    // The SteamVR folders listed under "runtime" in %LOCALAPPDATA%\openvr\openvrpaths.vrpath.
    public static IReadOnlyList<string> ParseRuntimeFolders(string? vrpathJson)
    {
        if (string.IsNullOrWhiteSpace(vrpathJson)) return [];
        try
        {
            using var document = JsonDocument.Parse(vrpathJson, new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
            if (document.RootElement.ValueKind != JsonValueKind.Object) return [];
            if (!document.RootElement.TryGetProperty("runtime", out var runtime)) return [];
            if (runtime.ValueKind != JsonValueKind.Array) return [];
            var folders = new List<string>();
            foreach (var entry in runtime.EnumerateArray())
            {
                if (entry.ValueKind != JsonValueKind.String) continue;
                string? folder = entry.GetString();
                if (string.IsNullOrWhiteSpace(folder)) continue;
                folders.Add(folder.Trim());
            }
            return folders;
        }
        catch (JsonException) { return []; }
    }

    // <SteamVR>\bin\win64\vrstartup.exe
    public static string? VrStartupPath(string? steamVrFolder)
    {
        if (string.IsNullOrWhiteSpace(steamVrFolder)) return null;
        return Path.Combine(steamVrFolder.Trim(), "bin", "win64", "vrstartup.exe");
    }

    // The SteamVR folder holding an active steamxr_win64.json, or null for another runtime.
    public static string? FolderFromManifest(string? manifestPath)
    {
        if (!IsSteamVrRuntime(manifestPath)) return null;
        string? folder = Path.GetDirectoryName(manifestPath!.Trim().Trim('"'));
        return string.IsNullOrWhiteSpace(folder) ? null : folder;
    }

    // Where vrstartup.exe may be, in order: openvrpaths.vrpath's runtimes, then the
    // folder of the active OpenXR manifest. Duplicates are dropped.
    public static IReadOnlyList<string> VrStartupCandidates(string? vrpathJson, string? manifestPath)
    {
        var candidates = new List<string>();
        foreach (string folder in ParseRuntimeFolders(vrpathJson))
        {
            string? exe = VrStartupPath(folder);
            if (exe != null && !candidates.Contains(exe, StringComparer.OrdinalIgnoreCase)) candidates.Add(exe);
        }
        string? fromManifest = VrStartupPath(FolderFromManifest(manifestPath));
        if (fromManifest != null && !candidates.Contains(fromManifest, StringComparer.OrdinalIgnoreCase)) candidates.Add(fromManifest);
        return candidates;
    }

    // Up: the compositor is running with the server, or the server has been up long
    // enough without it.
    public static bool Ready(bool serverRunning, bool compositorRunning, TimeSpan serverUpFor)
    {
        if (!serverRunning) return false;
        return compositorRunning || serverUpFor >= ServerGrace;
    }

    // The loader's choice: XR_RUNTIME_JSON overrides the registry's ActiveRuntime.
    public static string? ActiveRuntimePath()
    {
        string? overridden = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
        if (!string.IsNullOrWhiteSpace(overridden)) return overridden;
        try
        {
            using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using var key = machine.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1");
            return key?.GetValue("ActiveRuntime") as string;
        }
        catch (Exception ex) when (ex is System.Security.SecurityException or UnauthorizedAccessException or IOException)
        {
            Debug.WriteLine("SteamVr.ActiveRuntimePath: registry unreadable: " + ex.Message);
            return null;
        }
    }

    public static string VrPathFile() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "openvr", "openvrpaths.vrpath");

    public static bool IsRunning(string processName)
    {
        if (string.IsNullOrWhiteSpace(processName)) return false;
        var found = Process.GetProcessesByName(processName);
        bool any = found.Length > 0;
        foreach (var process in found) process.Dispose();
        return any;
    }
    public static bool ServerRunning() => IsRunning(ServerProcess);
    public static bool Starting() => StartingProcesses.Any(IsRunning);

    // Starts SteamVR through vrstartup.exe, or through Steam when it can't be found.
    // Returns what was started, for the log. Throws if Windows refuses both.
    public static string Launch(string? manifestPath, Action<string>? log)
    {
        log?.Invoke("SteamVr.Launch: enter, manifest " + (manifestPath ?? "(none)"));
        string? vrpath = null;
        try
        {
            string file = VrPathFile();
            if (File.Exists(file)) vrpath = File.ReadAllText(file);
            else log?.Invoke("SteamVr.Launch: no " + file);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            log?.Invoke("SteamVr.Launch: could not read openvrpaths.vrpath: " + ex.Message);
        }
        foreach (string exe in VrStartupCandidates(vrpath, manifestPath))
        {
            if (!File.Exists(exe)) { log?.Invoke("SteamVr.Launch: not found " + exe); continue; }
            var start = new ProcessStartInfo(exe) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(exe) ?? "" };
            Process.Start(start)?.Dispose();
            log?.Invoke("SteamVr.Launch: exit, started " + exe);
            return exe;
        }
        log?.Invoke("SteamVr.Launch: vrstartup.exe not found, asking Steam (" + SteamUrl + ")");
        Process.Start(new ProcessStartInfo(SteamUrl) { UseShellExecute = true })?.Dispose();
        log?.Invoke("SteamVr.Launch: exit, requested " + SteamUrl);
        return SteamUrl;
    }

    // Polls off the UI thread until SteamVR is up (true) or the timeout passes (false).
    // Cancelling throws OperationCanceledException.
    public static async Task<bool> WaitUntilReady(TimeSpan timeout, CancellationToken cancel, Action<string>? log)
    {
        log?.Invoke($"SteamVr.WaitUntilReady: enter, timeout {timeout.TotalSeconds:F0}s");
        var clock = Stopwatch.StartNew();
        TimeSpan? serverSeen = null;
        bool loggedServer = false;
        while (clock.Elapsed < timeout)
        {
            cancel.ThrowIfCancellationRequested();
            var (server, compositor) = await Task.Run(() => (ServerRunning(), IsRunning(CompositorProcess)), cancel);
            if (server && serverSeen == null) serverSeen = clock.Elapsed;
            if (!server) serverSeen = null;
            if (server && !loggedServer) { log?.Invoke($"SteamVr.WaitUntilReady: vrserver up after {clock.Elapsed.TotalSeconds:F1}s"); loggedServer = true; }
            if (Ready(server, compositor, serverSeen == null ? TimeSpan.Zero : clock.Elapsed - serverSeen.Value))
            {
                log?.Invoke($"SteamVr.WaitUntilReady: exit, ready after {clock.Elapsed.TotalSeconds:F1}s (compositor {(compositor ? "running" : "not seen")})");
                return true;
            }
            await Task.Delay(PollInterval, cancel);
        }
        log?.Invoke($"SteamVr.WaitUntilReady: exit, timed out after {clock.Elapsed.TotalSeconds:F0}s");
        return false;
    }
}
