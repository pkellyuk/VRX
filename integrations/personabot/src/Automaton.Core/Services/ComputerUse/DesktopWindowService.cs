using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// The desktop's top-level windows, as a person would list them: the ones with a title
/// that are actually on screen or minimised to the taskbar.
///
/// Two jobs. It lets an action result say "a new window 'Properties' opened" instead of
/// only "an area in the middle of the screen changed" — Steam's dialogs, like most apps',
/// are separate top-level windows, so this is the semantic fact the pixel comparison
/// cannot give. And it lets the model raise a window by name, which is far more reliable
/// than clicking a sliver of a partly covered window to bring it forward.
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class DesktopWindowService : IComputerWindows
{
    /// <summary>
    /// Lists the windows, topmost first — EnumWindows walks the z-order, so the first
    /// match for a title is the one the operator most recently had in front.
    /// </summary>
    public WindowSnapshot Capture()
    {
        var windows = new List<DesktopWindowInfo>();
        var self = (uint)Environment.ProcessId;
        var processNames = new Dictionary<uint, string>();

        EnumWindows((handle, _) =>
        {
            if (!IsListable(handle))
            {
                return true;
            }

            var title = GetTitle(handle);
            if (string.IsNullOrWhiteSpace(title))
            {
                return true;
            }

            GetWindowThreadProcessId(handle, out var processId);

            if (!processNames.TryGetValue(processId, out var processName))
            {
                processName = ProcessName(processId);
                processNames[processId] = processName;
            }

            GetWindowRect(handle, out var rect);

            windows.Add(new DesktopWindowInfo
            {
                Handle = handle,
                Title = title,
                ProcessId = processId,
                ProcessName = processName,
                Bounds = new ScreenRect(rect.Left, rect.Top, rect.Right - rect.Left, rect.Bottom - rect.Top),
                IsMinimized = IsIconic(handle),
                IsAutomaton = processId == self
            });

            return true;
        }, IntPtr.Zero);

        var foreground = GetForegroundWindow();

        return new WindowSnapshot
        {
            Windows = windows,
            Foreground = windows.FirstOrDefault(w => w.Handle == foreground),
            ForegroundHandle = foreground
        };
    }

    /// <summary>
    /// The top-level window actually showing at a screen point — whatever is on top
    /// there, so a window hidden behind another does not count. Click-through overlays
    /// such as the ghost cursor are skipped by Windows itself.
    /// </summary>
    public (IntPtr Handle, uint ProcessId) TopLevelWindowAt(ScreenPoint point)
    {
        var child = WindowFromPoint(new POINT { X = point.X, Y = point.Y });
        if (child == IntPtr.Zero)
        {
            return (IntPtr.Zero, 0);
        }

        var root = GetAncestor(child, GA_ROOT);
        GetWindowThreadProcessId(root, out var processId);
        return (root, processId);
    }

    /// <summary>
    /// Brings a window to the front and gives it keyboard focus.
    ///
    /// <c>SetForegroundWindow</c> on its own is refused whenever the calling process is
    /// not the one the user last interacted with, which for a tool running behind a chat
    /// window is nearly always. Two documented ways past that, tried in order: attach to
    /// the foreground thread's input queue for the duration of the call, and failing
    /// that, make this process the source of the last input event with a key that has
    /// no effect anywhere (VK_NONAME).
    /// </summary>
    /// <returns>True when the window ended up in the foreground.</returns>
    public bool BringToFront(IntPtr handle)
    {
        AutomatonDebug.Log($"DesktopWindowService.BringToFront: Entry - handle=0x{handle:X}");

        if (!IsWindow(handle))
        {
            AutomatonDebug.LogWarning("DesktopWindowService.BringToFront: Exit - the window no longer exists");
            return false;
        }

        if (IsIconic(handle))
        {
            ShowWindow(handle, SW_RESTORE);
        }

        if (TryAttachedForeground(handle) || TryAfterNeutralInput(handle))
        {
            AutomatonDebug.Log("DesktopWindowService.BringToFront: Exit - window is in the foreground");
            return true;
        }

        AutomatonDebug.LogWarning($"DesktopWindowService.BringToFront: Exit - Windows refused to change the foreground window (foreground is still 0x{GetForegroundWindow():X})");
        return false;
    }

    private static bool TryAttachedForeground(IntPtr handle)
    {
        var foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), out _);
        var currentThread = GetCurrentThreadId();
        var attached = foregroundThread != 0 && foregroundThread != currentThread &&
                       AttachThreadInput(currentThread, foregroundThread, true);

        try
        {
            BringWindowToTop(handle);
            SetForegroundWindow(handle);
        }
        finally
        {
            if (attached)
            {
                AttachThreadInput(currentThread, foregroundThread, false);
            }
        }

        return WaitForForeground(handle);
    }

    private static bool TryAfterNeutralInput(IntPtr handle)
    {
        // VK_NONAME is reserved and bound to nothing, so no app reacts to it — unlike the
        // usual Alt tap, which opens the menu bar of whatever window has focus.
        var inputs = new[]
        {
            new INPUT { type = INPUT_KEYBOARD, U = new InputUnion { ki = new KEYBDINPUT { wVk = VK_NONAME } } },
            new INPUT { type = INPUT_KEYBOARD, U = new InputUnion { ki = new KEYBDINPUT { wVk = VK_NONAME, dwFlags = KEYEVENTF_KEYUP } } }
        };

        SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
        SetForegroundWindow(handle);

        return WaitForForeground(handle);
    }

    /// <summary>Activation is asynchronous; give it a moment before calling it failed.</summary>
    private static bool WaitForForeground(IntPtr handle)
    {
        for (var i = 0; i < 10; i++)
        {
            if (GetForegroundWindow() == handle)
            {
                return true;
            }

            Thread.Sleep(20);
        }

        return false;
    }

    /// <summary>
    /// Visible, not cloaked, and the kind of window that gets a taskbar button. Cloaked
    /// windows are the suspended UWP apps and windows on other virtual desktops, which
    /// report themselves visible while nothing of them is on screen; tool windows are
    /// floating palettes and overlays, including Automaton's own ghost cursor.
    /// </summary>
    private static bool IsListable(IntPtr handle)
    {
        if (!IsWindowVisible(handle))
        {
            return false;
        }

        var exStyle = GetWindowLongPtr(handle, GWL_EXSTYLE).ToInt64();
        if ((exStyle & WS_EX_TOOLWINDOW) != 0 && (exStyle & WS_EX_APPWINDOW) == 0)
        {
            return false;
        }

        if (DwmGetWindowAttribute(handle, DWMWA_CLOAKED, out var cloaked, sizeof(int)) == 0 && cloaked != 0)
        {
            return false;
        }

        return true;
    }

    private static string GetTitle(IntPtr handle)
    {
        var length = GetWindowTextLength(handle);
        if (length <= 0)
        {
            return string.Empty;
        }

        var builder = new StringBuilder(length + 1);
        GetWindowText(handle, builder, builder.Capacity);
        return builder.ToString();
    }

    private static string ProcessName(uint processId)
    {
        try
        {
            using var process = Process.GetProcessById((int)processId);
            return process.ProcessName;
        }
        catch (Exception)
        {
            // Exited in the meantime, or protected. The title is what matters.
            return string.Empty;
        }
    }

    private const int GWL_EXSTYLE = -20;
    private const uint GA_ROOT = 2;
    private const long WS_EX_TOOLWINDOW = 0x00000080;
    private const long WS_EX_APPWINDOW = 0x00040000;
    private const int DWMWA_CLOAKED = 14;
    private const int SW_RESTORE = 9;
    private const uint INPUT_KEYBOARD = 1;
    private const uint KEYEVENTF_KEYUP = 0x0002;
    private const ushort VK_NONAME = 0xFC;

    private delegate bool EnumWindowsProc(IntPtr handle, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT
    {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MOUSEINPUT
    {
        public int dx;
        public int dy;
        public int mouseData;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputUnion
    {
        [FieldOffset(0)]
        public MOUSEINPUT mi;

        [FieldOffset(0)]
        public KEYBDINPUT ki;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT
    {
        public uint type;
        public InputUnion U;
    }

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern IntPtr WindowFromPoint(POINT point);

    [DllImport("user32.dll")]
    private static extern IntPtr GetAncestor(IntPtr handle, uint flags);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr handle);

    [DllImport("user32.dll")]
    private static extern bool IsWindow(IntPtr handle);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr handle);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextLength(IntPtr handle);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr handle, StringBuilder text, int maxCount);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr handle, out RECT rect);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern IntPtr GetWindowLongPtr(IntPtr handle, int index);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr handle);

    [DllImport("user32.dll")]
    private static extern bool BringWindowToTop(IntPtr handle);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr handle, int command);

    [DllImport("user32.dll")]
    private static extern bool AttachThreadInput(uint attach, uint attachTo, bool doAttach);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint count, INPUT[] inputs, int size);

    [DllImport("dwmapi.dll")]
    private static extern int DwmGetWindowAttribute(IntPtr handle, int attribute, out int value, int size);
}

/// <summary>One top-level window.</summary>
public sealed class DesktopWindowInfo
{
    public IntPtr Handle { get; set; }

    public string Title { get; set; } = string.Empty;

    public uint ProcessId { get; set; }

    public string ProcessName { get; set; } = string.Empty;

    public ScreenRect Bounds { get; set; }

    public bool IsMinimized { get; set; }

    /// <summary>Belongs to this process — the chat, the viewer. Never a target to focus.</summary>
    public bool IsAutomaton { get; set; }

    /// <summary>"'Properties' (steamwebhelper)" — how windows are named to the model.</summary>
    public string Describe() =>
        string.IsNullOrEmpty(ProcessName) ? $"'{Title}'" : $"'{Title}' ({ProcessName})";
}

/// <summary>What <c>ComputerUseSession.FocusWindow</c> did.</summary>
[SupportedOSPlatform("windows")]
public sealed class WindowFocusResult
{
    public DesktopWindowInfo Target { get; set; } = new();

    /// <summary>False when Windows refused to hand over the foreground.</summary>
    public bool Focused { get; set; }

    /// <summary>Other windows the title also matched, frontmost first.</summary>
    public IReadOnlyList<DesktopWindowInfo> OtherMatches { get; set; } = Array.Empty<DesktopWindowInfo>();

    public ScreenChangeReport? Report { get; set; }
}

/// <summary>The windows at one moment, topmost first.</summary>
public sealed class WindowSnapshot
{
    public IReadOnlyList<DesktopWindowInfo> Windows { get; set; } = Array.Empty<DesktopWindowInfo>();

    /// <summary>The foreground window, when it is one of the listed windows.</summary>
    public DesktopWindowInfo? Foreground { get; set; }

    public IntPtr ForegroundHandle { get; set; }
}

/// <summary>How the set of windows changed across an action.</summary>
public sealed class WindowChanges
{
    public IReadOnlyList<DesktopWindowInfo> Opened { get; set; } = Array.Empty<DesktopWindowInfo>();

    public IReadOnlyList<DesktopWindowInfo> Closed { get; set; } = Array.Empty<DesktopWindowInfo>();

    /// <summary>Same window, new title — a browser tab navigating, an editor opening a file.</summary>
    public IReadOnlyList<(DesktopWindowInfo Before, DesktopWindowInfo After)> Retitled { get; set; } =
        Array.Empty<(DesktopWindowInfo, DesktopWindowInfo)>();

    public DesktopWindowInfo? ForegroundBefore { get; set; }

    public DesktopWindowInfo? ForegroundAfter { get; set; }

    public bool ForegroundChanged { get; set; }

    public bool HasChanges => Opened.Count > 0 || Closed.Count > 0 || Retitled.Count > 0 || ForegroundChanged;

    public static WindowChanges Compare(WindowSnapshot before, WindowSnapshot after)
    {
        ArgumentNullException.ThrowIfNull(before);
        ArgumentNullException.ThrowIfNull(after);

        var beforeByHandle = before.Windows.ToDictionary(w => w.Handle);
        var afterByHandle = after.Windows.ToDictionary(w => w.Handle);

        return new WindowChanges
        {
            Opened = after.Windows.Where(w => !beforeByHandle.ContainsKey(w.Handle)).ToList(),
            Closed = before.Windows.Where(w => !afterByHandle.ContainsKey(w.Handle)).ToList(),
            Retitled = after.Windows
                .Where(w => beforeByHandle.TryGetValue(w.Handle, out var old) && old.Title != w.Title)
                .Select(w => (beforeByHandle[w.Handle], w))
                .ToList(),
            ForegroundBefore = before.Foreground,
            ForegroundAfter = after.Foreground,
            ForegroundChanged = before.ForegroundHandle != after.ForegroundHandle
        };
    }

    /// <summary>
    /// One sentence for the model, or empty when nothing about the windows changed.
    /// Lists at most a few of each, since an app closing can take many windows with it.
    /// </summary>
    public string Describe()
    {
        if (!HasChanges)
        {
            return string.Empty;
        }

        const int most = 3;
        var parts = new List<string>();

        if (Opened.Count > 0)
        {
            parts.Add($"{(Opened.Count == 1 ? "a new window opened" : $"{Opened.Count} new windows opened")}: {List(Opened, most)}");
        }

        if (Closed.Count > 0)
        {
            parts.Add($"{(Closed.Count == 1 ? "a window closed" : $"{Closed.Count} windows closed")}: {List(Closed, most)}");
        }

        foreach (var (was, now) in Retitled.Take(most))
        {
            parts.Add($"the window '{was.Title}' is now titled '{now.Title}'");
        }

        if (ForegroundChanged)
        {
            var now = ForegroundAfter?.Describe() ?? "a window with no title (a menu, a pop-up or the desktop)";
            var was = ForegroundBefore?.Describe() ?? "a window with no title";
            parts.Add($"the foreground window is now {now}, was {was}");
        }

        var sentence = string.Join("; ", parts);
        return $"Windows: {char.ToUpperInvariant(sentence[0])}{sentence[1..]}.";
    }

    private static string List(IReadOnlyList<DesktopWindowInfo> windows, int most)
    {
        var named = string.Join(", ", windows.Take(most).Select(w => w.Describe()));
        return windows.Count > most ? $"{named} and {windows.Count - most} more" : named;
    }
}
