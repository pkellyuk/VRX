using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;

namespace VRX.Desktop;

// App-wide settings (not per game), kept in app-settings.json in the ProfileStore root.
public sealed class AppSettings
{
    public const int MinSeconds = 3;
    public const int MaxSeconds = 30;
    public const int DefaultSeconds = 5;

    // Auto-attach: count down, then attach to the game in front. Off by default (Easy mode
    // turns it on).
    public bool AutoAttach { get; set; }
    public int AutoAttachSeconds { get; set; } = DefaultSeconds;

    // "Easy" or "Expert". Null in files written before the modes existed; ProfileStore
    // then picks Expert for anyone who has used VRX before and Easy for a new install.
    public const string EasyMode = "Easy";
    public const string ExpertMode = "Expert";
    public string? Mode { get; set; }
    [System.Text.Json.Serialization.JsonIgnore]
    public bool IsExpert => Mode == ExpertMode;
    public static bool ValidMode(string? mode) => mode is EasyMode or ExpertMode;

    // Expert's collapsible sections: name -> open. Sections not listed use DefaultSections.
    public const string SectionGame = "Game", SectionScreen = "Screen", SectionAround = "Around", SectionRoom = "Room",
        SectionDepth = "Depth", SectionSteamVr = "SteamVr", SectionProfile = "Profile", SectionSession = "Session";
    public static readonly IReadOnlyDictionary<string, bool> DefaultSections = new Dictionary<string, bool>(StringComparer.Ordinal)
    {
        [SectionGame] = false, [SectionScreen] = true, [SectionAround] = true, [SectionRoom] = true,
        [SectionDepth] = false, [SectionSteamVr] = false, [SectionProfile] = false, [SectionSession] = false,
    };
    public Dictionary<string, bool>? Sections { get; set; }

    public bool SectionOpen(string name)
    {
        if (string.IsNullOrEmpty(name)) return false;
        if (Sections != null && Sections.TryGetValue(name, out bool open)) return open;
        return DefaultSections.TryGetValue(name, out bool byDefault) && byDefault;
    }

    public static int ClampSeconds(int seconds) => Math.Clamp(seconds, MinSeconds, MaxSeconds);
}

// A screen rectangle in physical pixels (right and bottom exclusive).
public readonly record struct ScreenRect(int Left, int Top, int Right, int Bottom)
{
    public int Width => Math.Max(0, Right - Left);
    public int Height => Math.Max(0, Bottom - Top);
    public long Area => (long)Width * Height;
}

// What auto-attach knows about the window in front. Plain data, so the rules can be tested
// without real windows.
public sealed record ForegroundSnapshot(nint Handle, int Pid, string FullPath, string Title, bool IsVrx, bool TopLevel,
    bool Offerable, bool Visible, bool Minimized, bool CaptionedMaximized, ScreenRect Window, ScreenRect Client,
    ScreenRect Monitor, bool HasProfile)
{
    public string ProcessName => Path.GetFileName(FullPath);
}

// Which window in front may be attached to automatically.
public static class AutoAttachRules
{
    // Share of the monitor a window (or its client area) must cover to count as full screen.
    public const double FullScreenShare = 0.98;

    // Never attached automatically: VRX itself, Steam and SteamVR, the Windows shell and its
    // helpers, overlays. RunningApps' own exclusions (the engine, terminals) are checked too.
    private static readonly HashSet<string> Excluded = new(StringComparer.OrdinalIgnoreCase)
    {
        "vrx.desktop", "xrplayer", "xrapp5",
        "steam", "steamwebhelper", "steamservice", "gameoverlayui", "gameoverlayui64",
        "vrmonitor", "vrserver", "vrcompositor", "vrdashboard", "vrwebhelper", "vrstartup", "vrserverhelper",
        "vrprismhost", "vrcmd", "vrpathreg",
        "explorer", "shellexperiencehost", "startmenuexperiencehost", "searchhost", "searchapp", "searchui",
        "lockapp", "textinputhost", "applicationframehost", "systemsettings", "taskmgr", "dwm", "sihost",
        "widgets", "shellhost", "logonui", "screenclippinghost", "snippingtool",
        "nvidia overlay", "nvcontainer", "gamebar", "gamebarftserver", "xboxgamebarwidgets",
    };

    // Web browsers: only full screen counts for these, even with a saved profile, because a
    // browser in a window is almost always just browsing.
    private static readonly HashSet<string> Browsers = new(StringComparer.OrdinalIgnoreCase)
    {
        "chrome", "msedge", "firefox", "opera", "opera_gx", "brave", "vivaldi", "iexplore", "arc", "chromium",
        "waterfox", "librewolf", "floorp", "thorium", "zen", "safari", "yandex", "browser",
    };

    // "Game.exe", "game" and a full path all give "game".
    public static string BaseName(string? processName)
    {
        if (string.IsNullOrWhiteSpace(processName)) return "";
        string name = Path.GetFileName(processName.Trim());
        return name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? name[..^4] : name;
    }

    public static bool IsExcluded(string? processName)
    {
        string name = BaseName(processName);
        if (name.Length == 0) return true;
        if (name.EndsWith(".scr", StringComparison.OrdinalIgnoreCase)) return true;          // screen savers
        return Excluded.Contains(name) || RunningApps.IsExcludedExecutable(name + ".exe");
    }

    public static bool IsBrowser(string? processName) => Browsers.Contains(BaseName(processName));

    // True when `rect` covers at least FullScreenShare of `monitor`.
    public static bool CoversMonitor(ScreenRect rect, ScreenRect monitor)
    {
        if (monitor.Area <= 0 || rect.Area <= 0) return false;
        long width = Math.Min(rect.Right, monitor.Right) - Math.Max(rect.Left, monitor.Left);
        long height = Math.Min(rect.Bottom, monitor.Bottom) - Math.Max(rect.Top, monitor.Top);
        if (width <= 0 || height <= 0) return false;
        return width * height >= monitor.Area * FullScreenShare;
    }

    // The rule itself. A window qualifies when it is visible, not minimized, its process is
    // not excluded, and either it covers its monitor (full screen or borderless; a normal
    // captioned window that is merely maximized does not count) or its executable has a saved
    // VRX profile and is not a web browser.
    public static bool Qualifies(string? processName, bool hasProfile, ScreenRect window, ScreenRect client, ScreenRect monitor,
        bool visible, bool minimized, bool captionedMaximized, out string reason)
    {
        if (string.IsNullOrWhiteSpace(processName)) { reason = "no process"; return false; }
        if (!visible) { reason = "not visible"; return false; }
        if (minimized) { reason = "minimized"; return false; }
        if (IsExcluded(processName)) { reason = "excluded process"; return false; }
        if (!captionedMaximized && (CoversMonitor(window, monitor) || CoversMonitor(client, monitor))) { reason = "full screen"; return true; }
        if (!hasProfile) { reason = "windowed, no saved profile"; return false; }
        if (IsBrowser(processName)) { reason = "windowed web browser"; return false; }
        reason = "saved profile";
        return true;
    }

    public static bool Qualifies(string? processName, bool hasProfile, ScreenRect window, ScreenRect client, ScreenRect monitor,
        bool visible, bool minimized, bool captionedMaximized = false) =>
        Qualifies(processName, hasProfile, window, client, monitor, visible, minimized, captionedMaximized, out _);

    // The whole check for a window in front: never VRX, only a top-level window that
    // RunningApps would offer as attachable, then Qualifies.
    public static bool Evaluate(ForegroundSnapshot? snapshot, out string reason)
    {
        if (snapshot == null || snapshot.Handle == 0) { reason = "no window in front"; return false; }
        if (snapshot.IsVrx) { reason = "VRX is in front"; return false; }
        if (!snapshot.TopLevel) { reason = "not a top-level window"; return false; }
        if (!snapshot.Offerable) { reason = "not attachable"; return false; }
        return Qualifies(snapshot.ProcessName, snapshot.HasProfile, snapshot.Window, snapshot.Client, snapshot.Monitor,
            snapshot.Visible, snapshot.Minimized, snapshot.CaptionedMaximized, out reason);
    }
}

public enum AutoAttachPhase { Idle, Counting, Attached, Spent }
public enum AutoAttachAction { None, Countdown, Cancel, Attach }

// One poll: is auto-attach on, is a session running or starting, which window is in front
// (0 for none), is it VRX's own, does it qualify, and the time in seconds.
public readonly record struct AutoAttachInput(bool Enabled, bool SessionActive, nint Foreground, bool VrxInFront, bool Qualifies, double Now);

// Idle -> Counting (a qualifying window in front) -> Attach at zero -> Attached (session
// running) -> Spent (session ended or attach failed). A spent window is not attached again
// until another window (not VRX) has been in front and it comes back. Any change of the
// window in front, or the window no longer qualifying, cancels a countdown.
public sealed class AutoAttachMachine
{
    private int seconds = AppSettings.DefaultSeconds;
    private double started;
    private nint spent;

    public AutoAttachPhase Phase { get; private set; } = AutoAttachPhase.Idle;
    public nint Target { get; private set; }
    public int Remaining { get; private set; }
    public nint SpentWindow => spent;
    public int Seconds
    {
        get => seconds;
        set => seconds = AppSettings.ClampSeconds(value);
    }

    public AutoAttachAction Step(AutoAttachInput input)
    {
        // Leaving the foreground: another real window in front. VRX in front (e.g. after
        // Stop VR) or no window at all does not count, so stopping never re-attaches.
        if (spent != 0 && input.Foreground != 0 && input.Foreground != spent && !input.VrxInFront)
        {
            Debug.WriteLine($"[AutoAttach] window {spent:X} left the foreground; it may be attached again");
            spent = 0;
        }

        bool blocked = !input.Enabled || input.SessionActive || input.VrxInFront || input.Foreground == 0 ||
            !input.Qualifies || input.Foreground == spent;
        if (blocked)
        {
            bool wasCounting = Phase == AutoAttachPhase.Counting;
            Phase = input.SessionActive ? AutoAttachPhase.Attached : spent != 0 ? AutoAttachPhase.Spent : AutoAttachPhase.Idle;
            if (!wasCounting) return AutoAttachAction.None;
            Debug.WriteLine($"[AutoAttach] countdown for {Target:X} cancelled (in front {input.Foreground:X}, qualifies {input.Qualifies}, session {input.SessionActive}, enabled {input.Enabled})");
            Target = 0;
            Remaining = 0;
            return AutoAttachAction.Cancel;
        }

        if (Phase != AutoAttachPhase.Counting || Target != input.Foreground)
        {
            Debug.WriteLine($"[AutoAttach] countdown of {seconds} s for {input.Foreground:X} (was {Phase}, target {Target:X})");
            Phase = AutoAttachPhase.Counting;
            Target = input.Foreground;
            started = input.Now;
            Remaining = seconds;
            return AutoAttachAction.Countdown;
        }

        double elapsed = Math.Max(0, input.Now - started);
        if (elapsed >= seconds)
        {
            Debug.WriteLine($"[AutoAttach] countdown for {Target:X} reached zero; attaching");
            Phase = AutoAttachPhase.Attached;
            Remaining = 0;
            return AutoAttachAction.Attach;
        }
        Remaining = Math.Max(1, seconds - (int)Math.Floor(elapsed));
        return AutoAttachAction.Countdown;
    }

    // A session with this window ended, or attaching to it failed: leave it alone until it
    // has left the foreground and come back.
    public void MarkSpent(nint window)
    {
        Debug.WriteLine($"[AutoAttach] window {window:X} is spent (was {Phase}, target {Target:X})");
        if (window == 0) return;
        spent = window;
        Phase = AutoAttachPhase.Spent;
        Target = 0;
        Remaining = 0;
    }
}

// Reads the window in front (Win32). Nothing here attaches or changes any window.
public static class ForegroundWindow
{
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool IsIconic(nint hwnd);
    [DllImport("user32.dll")] private static extern bool IsZoomed(nint hwnd);
    [DllImport("user32.dll")] private static extern nint GetAncestor(nint hwnd, uint flags);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(nint hwnd, out RunningApps.Rect rect);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(nint hwnd, ref Point32 point);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern nint GetWindowLongPtr(nint hwnd, int index);
    [DllImport("user32.dll")] private static extern nint MonitorFromWindow(nint hwnd, uint flags);
    [DllImport("user32.dll")] private static extern bool GetMonitorInfo(nint monitor, ref MonitorInfo info);
    [StructLayout(LayoutKind.Sequential)] private struct Point32 { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo
    {
        public int Size;
        public RunningApps.Rect Monitor, Work;
        public uint Flags;
    }
    private const uint GaRoot = 2, MonitorDefaultToNearest = 2;
    private const long WsCaption = 0x00C00000;

    public static nint Handle() => GetForegroundWindow();

    // The window in front, or null when there is none. `hasProfile` says whether an
    // executable has saved VRX settings.
    public static ForegroundSnapshot? Read(Func<string, bool> hasProfile)
    {
        ArgumentNullException.ThrowIfNull(hasProfile);
        nint hwnd = GetForegroundWindow();
        if (hwnd == 0) return null;
        RunningApps.GetWindowThreadProcessId(hwnd, out uint pid);
        bool isVrx = pid == (uint)Environment.ProcessId;
        if (isVrx || pid == 0)
            return new ForegroundSnapshot(hwnd, (int)pid, "", "", isVrx, false, false, false, false, false, default, default, default, false);

        string path = RunningApps.ProcessPath((int)pid);
        var cls = new StringBuilder(256);
        RunningApps.GetClassName(hwnd, cls, cls.Capacity);
        var title = new StringBuilder(2048);
        RunningApps.GetWindowText(hwnd, title, title.Capacity);
        bool visible = RunningApps.IsWindowVisible(hwnd);
        bool minimized = IsIconic(hwnd);
        bool topLevel = GetAncestor(hwnd, GaRoot) == hwnd;
        bool captionedMaximized = IsZoomed(hwnd) && (GetWindowLongPtr(hwnd, -16).ToInt64() & WsCaption) == WsCaption;

        ScreenRect window = GetWindowRect(hwnd, out var w) ? new(w.Left, w.Top, w.Right, w.Bottom) : default;
        ScreenRect client = default;
        int clientWidth = 0, clientHeight = 0;
        if (RunningApps.GetClientRect(hwnd, out var c))
        {
            clientWidth = c.Right;
            clientHeight = c.Bottom;
            var origin = new Point32();
            if (ClientToScreen(hwnd, ref origin)) client = new(origin.X, origin.Y, origin.X + c.Right, origin.Y + c.Bottom);
        }
        ScreenRect monitor = default;
        var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        nint handle = MonitorFromWindow(hwnd, MonitorDefaultToNearest);
        if (handle != 0 && GetMonitorInfo(handle, ref info)) monitor = new(info.Monitor.Left, info.Monitor.Top, info.Monitor.Right, info.Monitor.Bottom);

        // As RunningApps.List would offer it: a real executable and a capturable window.
        bool offerable = path.Length > 0 && RunningApps.IsCaptureWindow(visible, clientWidth, clientHeight, cls.ToString()) &&
            !RunningApps.IsExcludedExecutable(Path.GetFileName(path));
        bool profile = false;
        if (path.Length > 0)
        {
            try { profile = hasProfile(path); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException) { profile = false; }
        }
        return new ForegroundSnapshot(hwnd, (int)pid, path, title.ToString(), false, topLevel, offerable, visible, minimized,
            captionedMaximized, window, client, monitor, profile);
    }
}

// "Attaching VRX to <game> in 5 - switch window to cancel": a small topmost, click-through
// window at the top centre of the game's monitor. It never takes the focus from the game.
public sealed class AutoAttachOverlay : Window
{
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern nint GetWindowLongPtr(nint hwnd, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")] private static extern nint SetWindowLongPtr(nint hwnd, int index, nint value);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(nint hwnd, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(nint hwnd, out RunningApps.Rect rect);
    private const int GwlExStyle = -20;
    private const long WsExTopmost = 0x8, WsExTransparent = 0x20, WsExToolWindow = 0x80, WsExNoActivate = 0x08000000;
    private const uint SwpNoSize = 0x1, SwpNoActivate = 0x10, SwpShowWindow = 0x40;
    private readonly TextBlock text = new() { Foreground = new SolidColorBrush(Color.FromRgb(0xE8, 0xEE, 0xF8)), FontSize = 20, FontFamily = new FontFamily("Segoe UI") };

    public AutoAttachOverlay()
    {
        WindowStyle = WindowStyle.None;
        AllowsTransparency = true;
        Background = Brushes.Transparent;
        ResizeMode = ResizeMode.NoResize;
        SizeToContent = SizeToContent.WidthAndHeight;
        Topmost = true;
        ShowActivated = false;
        ShowInTaskbar = false;
        Focusable = false;
        IsHitTestVisible = false;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = -20000;
        Top = -20000;
        Title = Loc.Get("OverlayTitle");
        Content = new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(0xE6, 0x10, 0x18, 0x27)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x76, 0xDC, 0xC6)),
            BorderThickness = new Thickness(2),
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(20, 10, 20, 10),
            Child = text,
        };
        SourceInitialized += (_, _) =>
        {
            // Before the window is first shown: click-through, never activated, not in Alt+Tab.
            nint hwnd = new WindowInteropHelper(this).Handle;
            long style = GetWindowLongPtr(hwnd, GwlExStyle).ToInt64() | WsExTopmost | WsExTransparent | WsExToolWindow | WsExNoActivate;
            SetWindowLongPtr(hwnd, GwlExStyle, new nint(style));
            Debug.WriteLine($"[AutoAttach] overlay created {hwnd:X}, extended style {style:X}");
        };
    }

    public string Message => text.Text;

    // Shows (or updates) the message at the top centre of `monitor` (physical pixels).
    public void ShowOn(ScreenRect monitor, string message)
    {
        ArgumentNullException.ThrowIfNull(message);
        text.Text = message;
        if (!IsVisible) Show();                        // ShowActivated = false: shown without activation
        UpdateLayout();
        nint hwnd = new WindowInteropHelper(this).Handle;
        if (hwnd == 0 || monitor.Area <= 0) return;
        if (!GetWindowRect(hwnd, out var own)) return;
        int width = own.Right - own.Left;
        int x = monitor.Left + (monitor.Width - width) / 2;
        int y = monitor.Top + Math.Max(16, monitor.Height / 30);
        SetWindowPos(hwnd, new nint(-1), x, y, 0, 0, SwpNoSize | SwpNoActivate | SwpShowWindow);
    }
}
