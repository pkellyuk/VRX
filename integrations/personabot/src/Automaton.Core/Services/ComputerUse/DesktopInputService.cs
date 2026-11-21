using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// Injects mouse and keyboard input into the Windows desktop.
///
/// <c>SendInput</c> rather than the older <c>mouse_event</c>/<c>keybd_event</c> pair
/// that <see cref="ScreenViewService"/> uses: SendInput delivers a batch atomically,
/// so no other input can interleave between a move and the click that follows it,
/// and it can address the whole virtual desktop in absolute coordinates. The legacy
/// calls cannot do either, which for a click derived from a grid label means the
/// pointer can arrive somewhere other than where the label said.
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class DesktopInputService : IComputerInput
{
    private const uint INPUT_MOUSE = 0;
    private const uint INPUT_KEYBOARD = 1;

    private const uint MOUSEEVENTF_MOVE = 0x0001;
    private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    private const uint MOUSEEVENTF_LEFTUP = 0x0004;
    private const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
    private const uint MOUSEEVENTF_RIGHTUP = 0x0010;
    private const uint MOUSEEVENTF_MIDDLEDOWN = 0x0020;
    private const uint MOUSEEVENTF_MIDDLEUP = 0x0040;
    private const uint MOUSEEVENTF_WHEEL = 0x0800;
    private const uint MOUSEEVENTF_HWHEEL = 0x1000;
    private const uint MOUSEEVENTF_ABSOLUTE = 0x8000;
    private const uint MOUSEEVENTF_VIRTUALDESK = 0x4000;

    private const uint KEYEVENTF_KEYUP = 0x0002;
    private const uint KEYEVENTF_UNICODE = 0x0004;

    private const int WheelDeltaPerNotch = 120;

    /// <summary>
    /// Windows delivers a double-click only when two clicks land inside the system
    /// double-click time. Injecting them back to back with no pause at all has been
    /// seen to coalesce oddly in some apps, so there is a small deliberate gap.
    /// </summary>
    private const int MultiClickGapMs = 40;

    /// <summary>
    /// Pause after each typed character.
    ///
    /// Typing used to go out as one SendInput batch, which the OS accepted in full and
    /// the target then partly lost: search-as-you-type boxes (Start, the Store) do real
    /// work per keystroke and drop characters that arrive while they are busy. A
    /// 70-character message arrived with about half its characters missing, and
    /// SendInput's return value reported success throughout, because the loss happens
    /// after delivery. Human-ish pacing is the only fix the sender controls.
    /// </summary>
    private const int KeystrokeGapMs = 15;

    /// <summary>
    /// Extra pause before a character identical to the one just typed. Two identical
    /// key events back to back are the easiest to coalesce — ms-windows-store://home
    /// arrived as ms-windows-store:/home.
    /// </summary>
    private const int RepeatedKeystrokeGapMs = 35;

    private readonly DesktopCaptureService _capture;

    public DesktopInputService(DesktopCaptureService capture)
    {
        _capture = capture ?? throw new ArgumentNullException(nameof(capture));
    }

    /// <summary>
    /// Moves the pointer to a screen point without pressing anything.
    /// </summary>
    public void MoveTo(ScreenPoint point)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] MoveTo: Entry - point={point}");

        Send(new[] { BuildMove(point) });

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] MoveTo: Exit - point={point}");
    }

    /// <summary>
    /// Moves to a point and clicks it.
    /// </summary>
    /// <param name="point">Screen point, resolved from a grid label by the caller.</param>
    /// <param name="button">left, right or middle.</param>
    /// <param name="count">Number of clicks; 2 for a double-click.</param>
    public void Click(ScreenPoint point, MouseButton button = MouseButton.Left, int count = 1)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] Click: Entry - point={point}, button={button}, count={count}");

        if (count < 1 || count > 3)
        {
            throw new ArgumentOutOfRangeException(nameof(count), count, "Click count must be between 1 and 3.");
        }

        var (down, up) = ButtonFlags(button);

        // The move goes in the same batch as the first press so nothing can move the
        // pointer between the two.
        Send(new[] { BuildMove(point), BuildMouseFlag(down), BuildMouseFlag(up) });

        for (var i = 1; i < count; i++)
        {
            Thread.Sleep(MultiClickGapMs);
            Send(new[] { BuildMouseFlag(down), BuildMouseFlag(up) });
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] Click: Exit - clicked {count}x {button} at {point}");
    }

    /// <summary>
    /// Presses at one point, moves, and releases at another.
    /// </summary>
    /// <param name="path">
    /// Where the pointer should be at each normalised time from 0 to 1. Defaults to a
    /// straight line. The session passes the ghost cursor's planned path so the real
    /// drag and the ghost travel together.
    /// </param>
    /// <param name="duration">How long the move with the button held should take.</param>
    public void Drag(
        ScreenPoint from,
        ScreenPoint to,
        MouseButton button = MouseButton.Left,
        PointerPath? path = null,
        TimeSpan? duration = null) => Drag(from, to, button, path, duration, default);

    public void Drag(ScreenPoint from, ScreenPoint to, MouseButton button, PointerPath? path,
        TimeSpan? duration, CancellationToken cancellationToken)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] Drag: Entry - from={from}, to={to}, button={button}, path={(path is null ? "straight" : "planned")}, duration={duration?.TotalMilliseconds ?? 0:0}ms");

        var (down, up) = ButtonFlags(button);

        cancellationToken.ThrowIfCancellationRequested();
        Send(new[] { BuildMove(from), BuildMouseFlag(down) });

        // Intermediate moves: a single jump from press to release reads as a teleport
        // and drag handlers that track motion (selection, sliders, canvas tools) never
        // see the drag happen at all.
        const int stepMs = 12;
        var steps = Math.Max(8, (int)((duration?.TotalMilliseconds ?? 0) / stepMs));

        try
        {
            for (var i = 1; i <= steps; i++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var t = (double)i / steps;
                var (x, y) = path == null || i == steps
                    ? (from.X + (to.X - from.X) * t, from.Y + (to.Y - from.Y) * t)
                    : path.Sample(t);
                Send(new[] { BuildMove(new ScreenPoint((int)Math.Round(x), (int)Math.Round(y))) });
                Thread.Sleep(stepMs);
            }
        }
        finally { Send(new[] { BuildMouseFlag(up) }); }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] Drag: Exit - dragged {button} from {from} to {to}");
    }

    /// <summary>
    /// Scrolls the wheel at a point. Positive notches scroll up or left.
    /// </summary>
    public void Scroll(ScreenPoint point, int notches, bool horizontal = false)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] Scroll: Entry - point={point}, notches={notches}, horizontal={horizontal}");

        if (notches == 0)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[DesktopInputService] Scroll: Exit - zero notches, nothing to do");
            return;
        }

        var input = BuildMouseFlag(horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL);
        input.U.mi.mouseData = notches * WheelDeltaPerNotch;

        Send(new[] { BuildMove(point), input });

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] Scroll: Exit - scrolled {notches} notch(es) at {point}");
    }

    /// <summary>
    /// Types literal text.
    ///
    /// Sent as unicode scan codes rather than virtual keys, so the text arrives the
    /// same way whatever keyboard layout is active — a virtual-key path would turn
    /// the same call into different characters on a non-US layout.
    /// </summary>
    /// <param name="shouldStop">
    /// Checked before every character. Paced typing takes real time, so revoking control
    /// has to be able to cut a long string off partway.
    /// </param>
    public void TypeText(string text, Func<bool>? shouldStop = null)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] TypeText: Entry - {text?.Length ?? 0} character(s)");

        if (string.IsNullOrEmpty(text))
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[DesktopInputService] TypeText: Exit - empty text");
            return;
        }

        var typed = 0;
        string? previous = null;

        for (var i = 0; i < text.Length; i++)
        {
            var character = text[i];

            if (character == '\r')
            {
                continue;
            }

            if (shouldStop?.Invoke() == true)
            {
                AutomatonDebug.LogWarning($"[DesktopInputService] TypeText: Exit - stopped after {typed} of {text.Length} character(s)");
                return;
            }

            // One keystroke is one Send, with a pause after it. A surrogate pair is one
            // character to the reader, so both halves go out together.
            string unit;
            INPUT[] inputs;

            if (character == '\n')
            {
                // A newline in typed text means "press Enter"; sending U+000A as a scan
                // code does nothing in most controls.
                unit = "\n";
                inputs = new[] { BuildKey(VirtualKeys.Return, false), BuildKey(VirtualKeys.Return, true) };
            }
            else if (char.IsHighSurrogate(character) && i + 1 < text.Length && char.IsLowSurrogate(text[i + 1]))
            {
                var low = text[++i];
                unit = new string(new[] { character, low });
                inputs = new[]
                {
                    BuildUnicode(character, false), BuildUnicode(character, true),
                    BuildUnicode(low, false), BuildUnicode(low, true)
                };
            }
            else
            {
                unit = character.ToString();
                inputs = new[] { BuildUnicode(character, false), BuildUnicode(character, true) };
            }

            if (unit == previous)
            {
                Thread.Sleep(RepeatedKeystrokeGapMs);
            }

            Send(inputs);
            Thread.Sleep(KeystrokeGapMs);

            previous = unit;
            typed += unit.Length;
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] TypeText: Exit - typed {typed} character(s)");
    }

    /// <summary>
    /// Presses a key combination written as "ctrl+shift+esc", "alt+f4", "enter".
    ///
    /// Modifiers are held for the duration and released in reverse order, so a
    /// combination behaves the way a human pressing it would.
    /// </summary>
    public void PressKeys(string keys)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] PressKeys: Entry - keys='{keys}'");

        if (string.IsNullOrWhiteSpace(keys))
        {
            throw new ArgumentException("Key combination is required.", nameof(keys));
        }

        var parts = keys.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length == 0)
        {
            throw new ArgumentException($"Could not parse key combination '{keys}'.", nameof(keys));
        }

        var codes = new List<ushort>();

        foreach (var part in parts)
        {
            var code = VirtualKeys.Resolve(part);
            if (code == 0)
            {
                throw new ArgumentException(
                    $"Unknown key '{part}' in combination '{keys}'. Use names like ctrl, alt, shift, win, enter, tab, esc, backspace, delete, up, down, left, right, home, end, pageup, pagedown, f1-f24, or a single character.",
                    nameof(keys));
            }

            codes.Add(code);
        }

        var inputs = new List<INPUT>(codes.Count * 2);

        foreach (var code in codes)
        {
            inputs.Add(BuildKey(code, false));
        }

        for (var i = codes.Count - 1; i >= 0; i--)
        {
            inputs.Add(BuildKey(codes[i], true));
        }

        Send(inputs.ToArray());

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopInputService] PressKeys: Exit - pressed '{keys}' ({codes.Count} key(s))");
    }

    /// <summary>
    /// Where the pointer is right now. Used to notice that a human moved it.
    /// </summary>
    public ScreenPoint GetCursorPosition()
    {
        if (!GetCursorPos(out var point))
        {
            AutomatonDebug.LogWarning("[DesktopInputService] GetCursorPosition: GetCursorPos failed, reporting (0,0)");
            return new ScreenPoint(0, 0);
        }

        return new ScreenPoint(point.x, point.y);
    }

    /// <summary>
    /// True while every key in the combination is physically down. Polled rather than
    /// hooked because a hook needs a message pump, and computer use has to work from
    /// a headless host process that does not have one.
    /// </summary>
    public static bool IsChordDown(params ushort[] keys)
    {
        if (keys is null || keys.Length == 0)
        {
            return false;
        }

        foreach (var key in keys)
        {
            if ((GetAsyncKeyState(key) & 0x8000) == 0)
            {
                return false;
            }
        }

        return true;
    }

    private INPUT BuildMove(ScreenPoint point)
    {
        var bounds = _capture.GetVirtualDesktopBounds();

        // SendInput's absolute coordinates are a 0..65535 fraction of the virtual
        // desktop, not pixels, and the denominator is width-1: using width puts the
        // rightmost column permanently out of reach.
        var spanX = Math.Max(1, bounds.Width - 1);
        var spanY = Math.Max(1, bounds.Height - 1);

        var normalizedX = (int)Math.Round((point.X - bounds.X) * 65535.0 / spanX);
        var normalizedY = (int)Math.Round((point.Y - bounds.Y) * 65535.0 / spanY);

        return new INPUT
        {
            type = INPUT_MOUSE,
            U = new InputUnion
            {
                mi = new MOUSEINPUT
                {
                    dx = Math.Clamp(normalizedX, 0, 65535),
                    dy = Math.Clamp(normalizedY, 0, 65535),
                    dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK
                }
            }
        };
    }

    private static INPUT BuildMouseFlag(uint flags)
    {
        return new INPUT
        {
            type = INPUT_MOUSE,
            U = new InputUnion
            {
                mi = new MOUSEINPUT { dwFlags = flags }
            }
        };
    }

    private static INPUT BuildKey(ushort virtualKey, bool up)
    {
        return new INPUT
        {
            type = INPUT_KEYBOARD,
            U = new InputUnion
            {
                ki = new KEYBDINPUT
                {
                    wVk = virtualKey,
                    dwFlags = up ? KEYEVENTF_KEYUP : 0
                }
            }
        };
    }

    private static INPUT BuildUnicode(char character, bool up)
    {
        return new INPUT
        {
            type = INPUT_KEYBOARD,
            U = new InputUnion
            {
                ki = new KEYBDINPUT
                {
                    wVk = 0,
                    wScan = character,
                    dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0)
                }
            }
        };
    }

    private static (uint down, uint up) ButtonFlags(MouseButton button)
    {
        return button switch
        {
            MouseButton.Left => (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP),
            MouseButton.Right => (MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP),
            MouseButton.Middle => (MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP),
            _ => throw new ArgumentOutOfRangeException(nameof(button), button, "Unsupported mouse button.")
        };
    }

    private static void Send(INPUT[] inputs)
    {
        if (inputs is null || inputs.Length == 0)
        {
            return;
        }

        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException("Desktop input injection is only implemented on Windows.");
        }

        var sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());

        if (sent == inputs.Length)
        {
            return;
        }

        // A partial send is almost always UIPI: the foreground window belongs to a
        // process at a higher integrity level (anything elevated, or the secure
        // desktop), and an unelevated sender is blocked from touching it.
        var error = Marshal.GetLastWin32Error();
        AutomatonDebug.LogError($"[DesktopInputService] Send: SendInput accepted {sent} of {inputs.Length} event(s), Win32 error {error}. A blocked send usually means the foreground window is elevated and this process is not.");

        throw new InvalidOperationException(
            $"Input was blocked: SendInput accepted {sent} of {inputs.Length} events (Win32 error {error}). " +
            "The foreground window is most likely running elevated, which refuses input from a process at a lower integrity level.");
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int x;
        public int y;
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
    private struct HARDWAREINPUT
    {
        public uint uMsg;
        public ushort wParamL;
        public ushort wParamH;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputUnion
    {
        [FieldOffset(0)]
        public MOUSEINPUT mi;

        [FieldOffset(0)]
        public KEYBDINPUT ki;

        [FieldOffset(0)]
        public HARDWAREINPUT hi;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT
    {
        public uint type;
        public InputUnion U;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out POINT lpPoint);

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);
}

/// <summary>
/// Mouse buttons computer use can press.
/// </summary>
public enum MouseButton
{
    Left,
    Right,
    Middle
}

/// <summary>
/// Virtual-key codes and the names the model is allowed to use for them.
/// </summary>
public static class VirtualKeys
{
    public const ushort Back = 0x08;
    public const ushort Tab = 0x09;
    public const ushort Return = 0x0D;
    public const ushort Shift = 0x10;
    public const ushort Control = 0x11;
    public const ushort Menu = 0x12;
    public const ushort Escape = 0x1B;
    public const ushort Space = 0x20;
    public const ushort LWin = 0x5B;
    public const ushort F12 = 0x7B;

    private static readonly Dictionary<string, ushort> Named = new(StringComparer.OrdinalIgnoreCase)
    {
        ["ctrl"] = Control,
        ["control"] = Control,
        ["alt"] = Menu,
        ["shift"] = Shift,
        ["win"] = LWin,
        ["cmd"] = LWin,
        ["meta"] = LWin,
        ["enter"] = Return,
        ["return"] = Return,
        ["tab"] = Tab,
        ["esc"] = Escape,
        ["escape"] = Escape,
        ["backspace"] = Back,
        ["space"] = Space,
        ["delete"] = 0x2E,
        ["del"] = 0x2E,
        ["insert"] = 0x2D,
        ["home"] = 0x24,
        ["end"] = 0x23,
        ["pageup"] = 0x21,
        ["pagedown"] = 0x22,
        ["left"] = 0x25,
        ["up"] = 0x26,
        ["right"] = 0x27,
        ["down"] = 0x28,
        ["printscreen"] = 0x2C,
        ["capslock"] = 0x14
    };

    /// <summary>
    /// Resolves a key name, an fN name, or a single character to a virtual-key code.
    /// Returns 0 when the name is not recognised, which the caller reports rather
    /// than silently pressing something else.
    /// </summary>
    public static ushort Resolve(string? name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return 0;
        }

        var trimmed = name.Trim();

        if (Named.TryGetValue(trimmed, out var known))
        {
            return known;
        }

        if (trimmed.Length > 1 &&
            (trimmed[0] == 'f' || trimmed[0] == 'F') &&
            int.TryParse(trimmed.AsSpan(1), out var functionKey) &&
            functionKey >= 1 &&
            functionKey <= 24)
        {
            return (ushort)(0x70 + functionKey - 1);
        }

        if (trimmed.Length == 1)
        {
            var character = char.ToUpperInvariant(trimmed[0]);

            if (character is >= 'A' and <= 'Z' or >= '0' and <= '9')
            {
                return character;
            }
        }

        return 0;
    }
}
