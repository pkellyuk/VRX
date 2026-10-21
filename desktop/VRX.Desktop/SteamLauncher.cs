using System.Diagnostics;
using System.IO;

namespace VRX.Desktop;

// Steam's per-game Launch Options for true-geometry launches name a fixed program,
// %LOCALAPPDATA%\VRX\launcher\vrx-steam-launch.exe, never a VRX install folder. That launcher
// hands over to the VRX copy recorded in launcher\target.txt, so moving or reinstalling VRX,
// or switching between its Steam and GitHub copies, never leaves Steam pointing at a stale
// path. Each complete VRX copy (one with the true-geometry runtime) refreshes the launcher
// and records itself at start-up; the last one started wins. With no copy recorded, the
// launcher starts the game normally (bench\native\openxr\vrx-steam-launch.cpp).
internal static class SteamLauncher
{
    public const string FileName = "vrx-steam-launch.exe";
    public static string Folder => Path.Combine(SteamGeometry.DataRoot, "launcher");
    public static string InstalledPath => Path.Combine(Folder, FileName);
    internal static string TargetFile => Path.Combine(Folder, "target.txt");
    private static string Bundled => Path.Combine(AppContext.BaseDirectory, FileName);

    // True when the fixed launcher is in place, so Launch Options can name it.
    public static bool Available => File.Exists(InstalledPath);

    // The VRX.Desktop.exe that Steam launches currently go to, or null.
    public static string? Target()
    {
        try
        {
            if (!File.Exists(TargetFile)) return null;
            string path = File.ReadAllText(TargetFile).Trim();
            return path.Length > 0 && File.Exists(path) ? path : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Debug.WriteLine($"[Steam launcher] Target: unreadable: {ex.Message}");
            return null;
        }
    }

    // A copy can take Steam launches when it is the published app with the true-geometry
    // runtime beside it; a development build without that runtime never takes them over.
    internal static bool CanTakeLaunches(string executable, string appRoot)
    {
        ArgumentException.ThrowIfNullOrEmpty(executable);
        ArgumentException.ThrowIfNullOrEmpty(appRoot);
        if (!Path.GetFileName(executable).Equals("VRX.Desktop.exe", StringComparison.OrdinalIgnoreCase)) return false;
        string layer = Path.Combine(appRoot, "geometry", "layer");
        return File.Exists(Path.Combine(appRoot, FileName))
            && File.Exists(Path.Combine(layer, GeometryRuntime.LayerLibrary))
            && File.Exists(Path.Combine(layer, GeometryRuntime.LayerManifest));
    }

    // Start-up: copies the launcher in when it is missing or different, and records this copy
    // as the one Steam launches go to. Never throws: a failure only leaves the previous state.
    public static void Refresh()
    {
        Debug.WriteLine("[Steam launcher] Refresh enter");
        string? executable = Environment.ProcessPath;
        if (string.IsNullOrEmpty(executable) || !CanTakeLaunches(executable, AppContext.BaseDirectory))
        {
            Debug.WriteLine($"[Steam launcher] Refresh exit: {executable ?? "(no path)"} is not a complete VRX copy");
            return;
        }
        try
        {
            Directory.CreateDirectory(Folder);
            if (!File.Exists(InstalledPath) || !GeometryRuntime.Hash(InstalledPath).Equals(GeometryRuntime.Hash(Bundled), StringComparison.OrdinalIgnoreCase))
            {
                // Copy beside, then swap in: a launcher that is running right now (a game
                // started from Steam) cannot be replaced, and is then updated next time.
                string staged = InstalledPath + ".new";
                File.Copy(Bundled, staged, true);
                File.Move(staged, InstalledPath, true);
                Debug.WriteLine($"[Steam launcher] Refresh: launcher updated from {Bundled}");
            }
            if (!string.Equals(Target(), executable, StringComparison.OrdinalIgnoreCase))
            {
                ProfileStore.AtomicWrite(TargetFile, executable);
                Debug.WriteLine($"[Steam launcher] Refresh: Steam launches now go to {executable}");
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Debug.WriteLine($"[Steam launcher] Refresh: left as it was: {ex.Message}");
            return;
        }
        Debug.WriteLine("[Steam launcher] Refresh exit");
    }

    // Launch Options that name a VRX install folder directly (set up before the fixed
    // launcher existed, or by a development build). They work only while that copy stays put.
    public static bool IsDirectOption(string? launchOptions)
    {
        if (string.IsNullOrWhiteSpace(launchOptions)) return false;
        return launchOptions.Contains("--steam-game", StringComparison.OrdinalIgnoreCase)
            && launchOptions.Contains("VRX.Desktop.exe", StringComparison.OrdinalIgnoreCase);
    }
}
