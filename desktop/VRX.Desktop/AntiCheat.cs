using System.Diagnostics;
using System.IO;
using Microsoft.Win32;

namespace VRX.Desktop;

// Games protected by anti-cheat are refused true geometry: its Vulkan layer and translation
// DLLs change how the game draws, which anti-cheat may report and which can get the
// player's account banned. Checked before every geometry launch or restart: the game's
// install folder (EasyAntiCheat, BattlEye, EA Javelin, PunkBuster, GameGuard, XIGNCODE3,
// Tencent ACE, miHoYo protect, EQU8, Riot Vanguard files), anti-cheat processes started
// from that folder or by the game, and Riot Vanguard's kernel service (vgk) while its
// client runs or the game is a Riot game. A clean result does not prove a game is safe:
// the warning still says to prefer offline games.
internal static class AntiCheat
{
    public sealed record Marker(string Name, string Where);

    // Folder names (exact, any case) and the product they mean.
    private static readonly Dictionary<string, string> Folders = new(StringComparer.OrdinalIgnoreCase)
    {
        ["EasyAntiCheat"] = "Easy Anti-Cheat", ["EasyAntiCheat_EOS"] = "Easy Anti-Cheat", ["BattlEye"] = "BattlEye",
        ["EAAntiCheat"] = "EA Javelin anti-cheat", ["GameGuard"] = "nProtect GameGuard", ["XIGNCODE"] = "XIGNCODE3",
        ["AntiCheatExpert"] = "Tencent Anti-Cheat Expert", ["equ8"] = "EQU8", ["Riot Vanguard"] = "Riot Vanguard",
    };

    // File names: an exact name, or a prefix ending in '*', and the product.
    private static readonly (string Pattern, string Product)[] Files =
    [
        ("start_protected_game.exe", "Easy Anti-Cheat"), ("EasyAntiCheat*", "Easy Anti-Cheat"),
        ("BEService*", "BattlEye"), ("BEClient*", "BattlEye"), ("BEDaisy.sys", "BattlEye"), ("*_BE.exe", "BattlEye"),
        ("EAAntiCheat*", "EA Javelin anti-cheat"), ("PnkBstrA.exe", "PunkBuster"), ("PnkBstrB.exe", "PunkBuster"),
        ("GameMon.des", "nProtect GameGuard"), ("GameGuard.des", "nProtect GameGuard"), ("x3.xem", "XIGNCODE3"),
        ("mhyprot*", "miHoYo protect"), ("SGuard64.exe", "Tencent Anti-Cheat Expert"), ("ACE-Base*", "Tencent Anti-Cheat Expert"),
        ("vgk.sys", "Riot Vanguard"), ("vgc.exe", "Riot Vanguard"), ("equ8*", "EQU8"),
    ];

    // Anti-cheat services' process names (without .exe).
    private static readonly (string Pattern, string Product)[] Processes =
    [
        ("EasyAntiCheat*", "Easy Anti-Cheat"), ("BEService*", "BattlEye"), ("EAAntiCheat*", "EA Javelin anti-cheat"),
        ("PnkBstr*", "PunkBuster"), ("GameMon*", "nProtect GameGuard"), ("SGuard64", "Tencent Anti-Cheat Expert"), ("vgc", "Riot Vanguard"),
    ];

    private const int MaxDepth = 5, MaxEntries = 20000;

    public static IReadOnlyList<Marker> Detect(string executable, int pid = 0)
    {
        ArgumentException.ThrowIfNullOrEmpty(executable);
        Debug.WriteLine($"[Anti-cheat] Detect enter: {executable}, pid {pid}");
        var found = new List<Marker>();
        string root = InstallRoot(executable);
        ScanFolder(root, found);
        ScanProcesses(root, pid, found);
        ScanVanguard(executable, found);
        var distinct = found.DistinctBy(m => m.Name + "|" + m.Where, StringComparer.OrdinalIgnoreCase).ToArray();
        Debug.WriteLine($"[Anti-cheat] Detect exit: {distinct.Length} marker(s) under {root}: {string.Join("; ", distinct.Select(m => m.Name + " @ " + m.Where))}");
        return distinct;
    }

    // The products found, for a message ("Easy Anti-Cheat, BattlEye").
    public static string Products(IEnumerable<Marker> markers)
    {
        ArgumentNullException.ThrowIfNull(markers);
        return string.Join(", ", markers.Select(m => m.Name).Distinct(StringComparer.OrdinalIgnoreCase));
    }

    // The game's whole installation: a Steam library's steamapps\common\<game>, else the folder
    // two levels above the executable (engines keep it in Binaries\Win64 or bin\x64), never
    // a drive root or a shared folder such as Program Files.
    internal static string InstallRoot(string executable)
    {
        var folder = new DirectoryInfo(Path.GetDirectoryName(Path.GetFullPath(executable))!);
        for (var dir = folder; dir?.Parent != null; dir = dir.Parent)
            if (dir.Parent.Name.Equals("common", StringComparison.OrdinalIgnoreCase) && dir.Parent.Parent?.Name.Equals("steamapps", StringComparison.OrdinalIgnoreCase) == true)
                return dir.FullName;
        // Out of the engine's binary folders (Binaries\Win64, bin\x64...), then past an Unreal
        // project folder to the installation that holds Engine\ (and the anti-cheat beside it).
        var root = folder;
        while (BinaryFolders.Contains(root.Name) && root.Parent?.Parent != null && !Shared(root.Parent)) root = root.Parent;
        if (root.Parent?.Parent != null && !Shared(root.Parent) && Directory.Exists(Path.Combine(root.Parent.FullName, "Engine"))) root = root.Parent;
        return root.FullName;
    }

    private static readonly HashSet<string> BinaryFolders = new(StringComparer.OrdinalIgnoreCase)
        { "bin", "binaries", "win64", "win32", "x64", "x86", "x86_64", "retail", "shipping" };

    private static bool Shared(DirectoryInfo dir)
    {
        string[] shared = [Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), Environment.GetFolderPath(Environment.SpecialFolder.Windows)];
        return dir.Parent == null || shared.Any(s => s.Length > 0 && Path.TrimEndingDirectorySeparator(s).Equals(Path.TrimEndingDirectorySeparator(dir.FullName), StringComparison.OrdinalIgnoreCase)) ||
            dir.Name.Equals("Games", StringComparison.OrdinalIgnoreCase) || dir.Name.Equals("common", StringComparison.OrdinalIgnoreCase);
    }

    internal static bool Matches(string name, string pattern)
    {
        if (pattern.StartsWith('*')) return name.EndsWith(pattern[1..], StringComparison.OrdinalIgnoreCase);
        if (pattern.EndsWith('*')) return name.StartsWith(pattern[..^1], StringComparison.OrdinalIgnoreCase);
        return name.Equals(pattern, StringComparison.OrdinalIgnoreCase);
    }

    private static void ScanFolder(string root, List<Marker> found)
    {
        int entries = 0;
        var pending = new Queue<(string Path, int Depth)>();
        pending.Enqueue((root, 0));
        while (pending.Count > 0 && entries < MaxEntries)
        {
            var (folder, depth) = pending.Dequeue();
            try
            {
                foreach (string directory in Directory.EnumerateDirectories(folder))
                {
                    entries++;
                    string name = Path.GetFileName(directory);
                    if (Folders.TryGetValue(name, out string? product)) found.Add(new Marker(product, directory));
                    if (depth + 1 < MaxDepth) pending.Enqueue((directory, depth + 1));
                }
                foreach (string file in Directory.EnumerateFiles(folder))
                {
                    entries++;
                    string name = Path.GetFileName(file);
                    foreach (var (pattern, product) in Files)
                        if (Matches(name, pattern)) { found.Add(new Marker(product, file)); break; }
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                Debug.WriteLine($"[Anti-cheat] {folder}: {ex.Message}");
            }
        }
        if (entries >= MaxEntries) Debug.WriteLine($"[Anti-cheat] stopped after {entries} entries under {root}");
    }

    // Anti-cheat services started from the game's folder, or by the game itself.
    private static void ScanProcesses(string root, int pid, List<Marker> found)
    {
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                try
                {
                    var match = Processes.FirstOrDefault(p => Matches(process.ProcessName, p.Pattern));
                    if (match.Product == null) continue;
                    string path = RunningApps.ProcessPath(process.Id);
                    bool related = (path.Length > 0 && GeometryRuntime.IsInside(path, root)) || (pid > 0 && ProcessCommandLine.Parent(process.Id) == pid);
                    if (related) found.Add(new Marker(match.Product, path.Length > 0 ? path : process.ProcessName));
                }
                catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception)
                {
                    Debug.WriteLine($"[Anti-cheat] process: {ex.Message}");
                }
            }
        }
    }

    // Riot Vanguard is a boot-time kernel driver (service vgk): refuse when it is installed and
    // either the game is a Riot game or Vanguard's client (vgc) is running.
    private static void ScanVanguard(string executable, List<Marker> found)
    {
        try
        {
            using var service = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\vgk");
            if (service == null) return;
            bool riotGame = executable.Contains(@"\Riot Games\", StringComparison.OrdinalIgnoreCase);
            var clients = Process.GetProcessesByName("vgc");
            bool client = clients.Length > 0;
            foreach (var c in clients) c.Dispose();
            if (riotGame || client) found.Add(new Marker("Riot Vanguard", "vgk"));
        }
        catch (Exception ex) when (ex is System.Security.SecurityException or UnauthorizedAccessException or IOException)
        {
            Debug.WriteLine($"[Anti-cheat] vgk: {ex.Message}");
        }
    }
}
