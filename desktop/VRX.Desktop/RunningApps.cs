using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace VRX.Desktop;
public sealed record GameWindow(nint Handle, string Title)
{
    public override string ToString() => string.IsNullOrWhiteSpace(Title) ? Loc.Format("WindowUntitled", Handle.ToString("X", System.Globalization.CultureInfo.InvariantCulture)) : Title;
}
public sealed record RunningApp(int Pid, string Name, string FullPath, List<GameWindow> Windows, ImageSource? Icon)
{
    public bool CanAttach => FullPath.Length > 0 && Windows.Count > 0;
    public string Description => !CanAttach ? Loc.Get("AppNoGameWindow") :
        Windows.Count == 1 ? Loc.Format("AppDescriptionOne", Pid) : Loc.Format("AppDescriptionOther", Pid, Windows.Count);
}
public static class RunningApps
{
    private delegate bool EnumWindow(nint hwnd, nint context);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindow callback, nint context);
    [DllImport("user32.dll")] internal static extern bool IsWindowVisible(nint hwnd);
    [DllImport("user32.dll")] internal static extern uint GetWindowThreadProcessId(nint hwnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] internal static extern int GetWindowText(nint hwnd, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] internal static extern int GetClassName(nint hwnd, StringBuilder text, int count);
    [DllImport("user32.dll")] internal static extern bool GetClientRect(nint hwnd, out Rect rect);
    [StructLayout(LayoutKind.Sequential)] internal struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("kernel32.dll")] private static extern nint OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern bool QueryFullProcessImageName(nint process, int flags, StringBuilder path, ref int size);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(nint handle);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(nint hwnd);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(nint icon);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] private struct FileInfo
    {
        public nint Icon; public int IconIndex; public uint Attributes;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string DisplayName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)] public string TypeName;
    }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern nint SHGetFileInfo(string path, uint attributes, out FileInfo info, uint size, uint flags);
    private static readonly Dictionary<string, ImageSource?> Icons = new(StringComparer.OrdinalIgnoreCase);
    public static string ProcessPath(int pid)
    {
        nint process = OpenProcess(0x1000, false, pid);
        if (process == 0) return "";
        try { var path = new StringBuilder(32768); int size = path.Capacity; return QueryFullProcessImageName(process, 0, path, ref size) ? path.ToString() : ""; }
        finally { CloseHandle(process); }
    }
    // The executable's icon, cached per path; null when there is none.
    internal static ImageSource? IconFor(string? path)
    {
        if (string.IsNullOrEmpty(path)) return null;
        if (Icons.TryGetValue(path, out var cached)) return cached;
        ImageSource? image = null;
        if (SHGetFileInfo(path, 0, out var info, (uint)Marshal.SizeOf<FileInfo>(), 0x101) != 0 && info.Icon != 0)
        {
            try { image = Imaging.CreateBitmapSourceFromHIcon(info.Icon, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions()); image.Freeze(); }
            finally { DestroyIcon(info.Icon); }
        }
        Icons[path] = image; return image;
    }
    // The engine and terminals are never offered as games (their windows are dropped).
    public static bool IsExcludedExecutable(string? name) => (name ?? "").ToLowerInvariant() is "xrapp5.exe" or "xrplayer.exe" or
        "windowsterminal.exe" or "conhost.exe" or "openconsole.exe" or "cmd.exe" or "powershell.exe" or "pwsh.exe";
    // A window that can be captured: visible, with a client area, and not a console.
    public static bool IsCaptureWindow(bool visible, int clientWidth, int clientHeight, string? className) =>
        visible && clientWidth > 0 && clientHeight > 0 && className is not ("ConsoleWindowClass" or "CASCADIA_HOSTING_WINDOW_CLASS");
    public static List<RunningApp> List(bool showAll)
    {
        var windows = new Dictionary<int, List<GameWindow>>();
        EnumWindows((hwnd, _) =>
        {
            if (!IsWindowVisible(hwnd) || !GetClientRect(hwnd, out var rect) || rect.Right <= 0 || rect.Bottom <= 0) return true;
            GetWindowThreadProcessId(hwnd, out uint pid);
            var cls = new StringBuilder(256); GetClassName(hwnd, cls, cls.Capacity);
            if (!IsCaptureWindow(true, rect.Right, rect.Bottom, cls.ToString())) return true;
            var title = new StringBuilder(2048); GetWindowText(hwnd, title, title.Capacity);
            if (!windows.ContainsKey((int)pid)) windows[(int)pid] = [];
            windows[(int)pid].Add(new GameWindow(hwnd, title.ToString()));
            return true;
        }, 0);
        var apps = new List<RunningApp>();
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                try
                {
                    if (process.Id == Environment.ProcessId) continue;
                    string path = ProcessPath(process.Id), name = path.Length == 0 ? process.ProcessName : Path.GetFileName(path);
                    var gameWindows = windows.GetValueOrDefault(process.Id) ?? [];
                    if (IsExcludedExecutable(name)) gameWindows = [];
                    if (!showAll && (gameWindows.Count == 0 || path.Length == 0)) continue;
                    apps.Add(new(process.Id, name, path, gameWindows, IconFor(path)));
                }
                catch (InvalidOperationException) { }
                catch (System.ComponentModel.Win32Exception) { }
            }
        }
        return apps.OrderByDescending(a => a.CanAttach).ThenBy(a => a.Name, StringComparer.OrdinalIgnoreCase).ThenBy(a => a.Pid).ToList();
    }
}
