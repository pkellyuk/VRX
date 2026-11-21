using Automaton.Core.Services.ComputerUse;

namespace PersonaBot;

/// <summary>
/// QEMU's virtual USB tablet and keyboard, driven over QMP. Coordinates are absolute (0-32767),
/// keys go through the monitor's sendkey, and text is typed key by key on a US layout.
/// </summary>
public sealed class QemuInput : IGuestInput
{
    private readonly IQmpRuntime _runtime;
    private bool _pointerInitialized;

    public QemuInput(IQmpRuntime runtime)
    {
        _runtime = runtime ?? throw new ArgumentNullException(nameof(runtime));
        System.Diagnostics.Trace.WriteLine("QemuInput: created for QMP-driven USB tablet and keyboard");
    }

    private void Events(params object[] events) => _runtime.Qmp.Execute("input-send-event", new { events });

    public void MoveTo(ScreenPoint point)
    {
        var width = _runtime.ScreenBounds.Width;
        var height = _runtime.ScreenBounds.Height;
        if (!_pointerInitialized)
        {
            System.Diagnostics.Trace.WriteLine($"QemuInput.MoveTo: initializing USB tablet on first move, screen {width}x{height}");
            // A fresh USB tablet starts at (0,0), while X starts its cursor in the
            // centre. A first (0,0) report can otherwise be suppressed as unchanged.
            Events(new { type = "abs", data = new { axis = "x", value = GuestComputer.AbsoluteCoordinate(point.X == 0 ? 1 : point.X - 1, width) } },
                new { type = "abs", data = new { axis = "y", value = GuestComputer.AbsoluteCoordinate(point.Y == 0 ? 1 : point.Y - 1, height) } });
            // Let the guest poll the initial USB report before publishing the target;
            // otherwise QEMU can coalesce the two reports back into the unchanged zero.
            Thread.Sleep(100);
            _pointerInitialized = true;
            System.Diagnostics.Trace.WriteLine("QemuInput.MoveTo: USB tablet initialized");
        }
        Events(new { type = "abs", data = new { axis = "x", value = GuestComputer.AbsoluteCoordinate(point.X, width) } },
            new { type = "abs", data = new { axis = "y", value = GuestComputer.AbsoluteCoordinate(point.Y, height) } });
    }

    public void Button(MouseButton button, bool down)
    {
        Events(new { type = "btn", data = new { button = button.ToString().ToLowerInvariant(), down } });
    }

    public void Wheel(bool positive, bool horizontal)
    {
        var button = horizontal ? (positive ? "wheel-left" : "wheel-right") : (positive ? "wheel-up" : "wheel-down");
        Events(new { type = "btn", data = new { button, down = true } });
        Thread.Sleep(15);
        Events(new { type = "btn", data = new { button, down = false } });
    }

    public void Wheel(bool positive, bool horizontal, GuestKeyModifiers modifiers)
    {
        var keys = new[] { (GuestKeyModifiers.Control, "ctrl"), (GuestKeyModifiers.Shift, "shift"), (GuestKeyModifiers.Alt, "alt") }
            .Where(k => modifiers.HasFlag(k.Item1)).Select(k => k.Item2).ToArray();
        object Key(string code, bool down) => new { type = "key", data = new { down, key = new { type = "qcode", data = code } } };
        try
        {
            if (keys.Length > 0) { Events(keys.Select(k => Key(k, true)).ToArray()); Thread.Sleep(20); }
            Wheel(positive, horizontal);
        }
        finally
        {
            if (keys.Length > 0) Events(keys.Reverse().Select(k => Key(k, false)).ToArray());
        }
    }

    private static readonly Dictionary<string, string> KeyNames = new(StringComparer.OrdinalIgnoreCase)
    {
        ["ctrl"] = "ctrl", ["control"] = "ctrl", ["alt"] = "alt", ["shift"] = "shift", ["enter"] = "ret", ["return"] = "ret", ["escape"] = "esc", ["esc"] = "esc",
        ["backspace"] = "backspace", ["delete"] = "delete", ["tab"] = "tab", ["space"] = "spc", ["left"] = "left", ["right"] = "right", ["up"] = "up", ["down"] = "down",
        ["home"] = "home", ["end"] = "end", ["pageup"] = "pgup", ["pagedown"] = "pgdn", ["insert"] = "insert", ["super"] = "meta_l",
        ["equal"] = "equal", ["minus"] = "minus", ["slash"] = "slash", ["period"] = "dot", ["comma"] = "comma", ["backslash"] = "backslash"
    };

    public void PressKeys(string keys)
    {
        var codes = keys.Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).Select(KeyCode).ToArray();
        if (codes.Length == 0 || codes.Length > 6)
        {
            System.Diagnostics.Trace.TraceWarning($"QemuInput.PressKeys: rejected key combination of {codes.Length} keys");
            throw new ArgumentException("Use a simple guest key combination.");
        }
        _runtime.Qmp.Execute("human-monitor-command", new Dictionary<string, object> { ["command-line"] = "sendkey " + string.Join('-', codes) + " 20" });
    }

    private static string KeyCode(string key)
    {
        key = key.ToLowerInvariant();
        if (KeyNames.TryGetValue(key, out var code)) return code;
        if (key.Length == 1 && char.IsAsciiLetterOrDigit(key[0])) return key.ToLowerInvariant();
        if (key.StartsWith('f') && int.TryParse(key[1..], out var f) && f >= 1 && f <= 12) return key;
        System.Diagnostics.Trace.TraceWarning($"QemuInput.KeyCode: unsupported guest key '{key}'");
        throw new ArgumentException("Unsupported guest key: " + key);
    }

    public void TypeText(string text, Func<bool>? shouldStop)
    {
        ArgumentNullException.ThrowIfNull(text);
        System.Diagnostics.Trace.WriteLine($"QemuInput.TypeText enter: {text.Length} chars");
        var typed = 0;
        const string plain = "`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./ ";
        const string shifted = "~!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>? ";
        string[] codes = ["grave_accent", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "minus", "equal", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "bracket_left", "bracket_right", "backslash", "a", "s", "d", "f", "g", "h", "j", "k", "l", "semicolon", "apostrophe", "z", "x", "c", "v", "b", "n", "m", "comma", "dot", "slash", "spc"];
        foreach (var ch in text)
        {
            if (shouldStop?.Invoke() == true)
            {
                System.Diagnostics.Trace.WriteLine($"QemuInput.TypeText: stopped after {typed} of {text.Length} chars");
                break;
            }
            var index = plain.IndexOf(ch);
            var shift = index < 0;
            if (shift) index = shifted.IndexOf(ch);
            if (index < 0 && ch != '\n' && ch != '\t') System.Diagnostics.Trace.TraceWarning($"QemuInput.TypeText: non-ASCII character U+{(int)ch:X4} at {typed}; guest needs an input method");
            var code = ch == '\n' ? "ret" : ch == '\t' ? "tab" : index >= 0 ? (shift ? "shift-" : "") + codes[index] : throw new ArgumentException("Non-ASCII input requires a guest keyboard input method.");
            _runtime.Qmp.Execute("human-monitor-command", new Dictionary<string, object> { ["command-line"] = "sendkey " + code + " 1" });
            Thread.Sleep(12);
            typed++;
        }
        System.Diagnostics.Trace.WriteLine($"QemuInput.TypeText exit: typed {typed} of {text.Length} chars");
    }
}
