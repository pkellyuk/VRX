using System.Buffers.Binary;
using Automaton.Core.Services.ComputerUse;

namespace PersonaBot;

/// <summary>
/// A pointer and keyboard sent over RFB (VNC), for a bot's own display inside the guest (XMEETING.md §3).
/// The machine's main display keeps QMP input (<see cref="QemuInput"/>). Each instance has its own
/// connection, so its messages never interleave with a viewer's update requests. Keys are sent as X
/// keysyms, so text is not tied to a keyboard layout and may be any Unicode.
/// </summary>
internal sealed class RfbInput : IGuestInput, IAsyncDisposable
{
    /// <summary>Opens a new negotiated connection: its stream, and what closes it.</summary>
    internal delegate Task<(Stream Stream, IAsyncDisposable? Connection)> Connector(CancellationToken token);

    /// <summary>How long a reconnection may take before the input gives up on this message.</summary>
    private static readonly TimeSpan ReconnectWait = TimeSpan.FromSeconds(30);

    private Stream _stream;
    private IAsyncDisposable? _connection;
    private readonly Connector? _connector;
    private readonly object _gate = new();
    private int _buttons;
    private ScreenPoint _pointer;
    private bool _broken;
    private bool _disposed;

    /// <summary>
    /// Writes to an already negotiated RFB connection. With <paramref name="connector"/>, a connection that breaks
    /// (the display server restarted, or QEMU's forward dropped it) is opened again and the message sent again.
    /// </summary>
    internal RfbInput(Stream stream, IAsyncDisposable? connection = null, Connector? connector = null)
    {
        _stream = stream ?? throw new ArgumentNullException(nameof(stream));
        _connection = connection;
        _connector = connector;
        if (connector != null) Drain(stream);
        System.Diagnostics.Trace.WriteLine($"RfbInput: created, reconnects={connector != null}");
    }

    /// <summary>Opens a connection of its own to a loopback RFB server.</summary>
    public static async Task<RfbInput> ConnectAsync(int port, CancellationToken token)
    {
        System.Diagnostics.Trace.WriteLine($"RfbInput.ConnectAsync enter: loopback port {port}");
        async Task<(Stream, IAsyncDisposable?)> Open(CancellationToken t)
        {
            var connection = new QemuFramebuffer();
            try
            {
                await connection.ConnectAsync(port, t);
                return (connection.Stream, connection);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Trace.TraceWarning($"RfbInput.ConnectAsync: port {port} failed: {ex.GetType().Name}: {ex.Message}");
                await connection.DisposeAsync();
                throw;
            }
        }
        var input = await ConnectAsync(Open, token);
        System.Diagnostics.Trace.WriteLine($"RfbInput.ConnectAsync exit: connected on port {port}");
        return input;
    }

    /// <summary>Opens a connection through <paramref name="connector"/>, which is used again whenever it breaks.</summary>
    internal static async Task<RfbInput> ConnectAsync(Connector connector, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(connector);
        System.Diagnostics.Trace.WriteLine("RfbInput.ConnectAsync: opening the first connection");
        var (stream, connection) = await connector(token);
        return new RfbInput(stream, connection, connector);
    }

    /// <summary>
    /// Reads and drops what the server sends (a bell, the clipboard), so nothing waits unread on this input-only
    /// connection; when the server closes it, the next message opens it again first.
    /// </summary>
    private void Drain(Stream stream)
    {
        _ = Task.Run(async () =>
        {
            var buffer = new byte[4096];
            try
            {
                while (await stream.ReadAsync(buffer) > 0) { }
            }
            catch (Exception ex) when (ex is IOException or ObjectDisposedException or System.Net.Sockets.SocketException)
            {
                System.Diagnostics.Trace.WriteLine($"RfbInput.Drain: read ended: {ex.GetType().Name}: {ex.Message}");
            }
            lock (_gate)
            {
                if (ReferenceEquals(stream, _stream) && !_disposed)
                {
                    _broken = true;
                    System.Diagnostics.Trace.WriteLine("RfbInput: the server closed the connection; the next message reconnects");
                }
            }
        });
    }

    /// <summary>Sends one message, reconnecting once if the connection is broken. Called with the gate held.</summary>
    private void Send(ReadOnlySpan<byte> message)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_broken) Reconnect();
        try
        {
            _stream.Write(message);
            _stream.Flush();
            return;
        }
        catch (Exception ex) when (_connector != null && ex is IOException or ObjectDisposedException or System.Net.Sockets.SocketException)
        {
            System.Diagnostics.Trace.WriteLine($"RfbInput.Send: the connection broke ({ex.Message}); reconnecting");
        }
        var copy = message.ToArray();
        Reconnect();
        _stream.Write(copy);
        _stream.Flush();
    }

    /// <summary>Replaces the connection with a new one (the connector may restart the display server). Called with the gate held.</summary>
    private void Reconnect()
    {
        if (_connector == null)
        {
            System.Diagnostics.Trace.TraceError("RfbInput.Reconnect: the connection was lost and this input can't reconnect");
            throw new IOException("The connection to the display was lost.");
        }
        System.Diagnostics.Trace.WriteLine($"RfbInput.Reconnect enter: waiting up to {ReconnectWait.TotalSeconds:0}s");
        var watch = System.Diagnostics.Stopwatch.StartNew();
        var old = _connection;
        using var timeout = new CancellationTokenSource(ReconnectWait);
        (Stream Stream, IAsyncDisposable? Connection) opened;
        try
        {
            opened = Task.Run(() => _connector(timeout.Token)).GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Trace.TraceError($"RfbInput.Reconnect: failed after {watch.ElapsedMilliseconds}ms: {ex.GetType().Name}: {ex.Message}");
            throw;
        }
        var (stream, connection) = opened;
        _stream = stream;
        _connection = connection;
        _broken = false;
        Drain(stream);
        if (old != null) _ = Task.Run(async () => { try { await old.DisposeAsync(); } catch (Exception ex) { System.Diagnostics.Trace.WriteLine("RfbInput: closing the old connection: " + ex.Message); } });
        // The server forgets held buttons with the connection; the next pointer message restates them.
        System.Diagnostics.Trace.WriteLine($"RfbInput.Reconnect exit: connected again after {watch.ElapsedMilliseconds}ms");
    }

    public void MoveTo(ScreenPoint point)
    {
        lock (_gate)
        {
            _pointer = point;
            SendPointer();
        }
    }

    public void Button(MouseButton button, bool down)
    {
        var bit = button switch
        {
            MouseButton.Left => 1,
            MouseButton.Middle => 2,
            MouseButton.Right => 4,
            _ => throw UnsupportedButton(button)
        };
        lock (_gate)
        {
            _buttons = down ? _buttons | bit : _buttons & ~bit;
            SendPointer();
        }
    }

    private static ArgumentException UnsupportedButton(MouseButton button)
    {
        System.Diagnostics.Trace.TraceWarning($"RfbInput.Button: unsupported mouse button {button}");
        return new ArgumentException("Unsupported guest mouse button: " + button);
    }

    public void Wheel(bool positive, bool horizontal)
    {
        // RFB carries the wheel as buttons 4-7: up, down, left, right.
        var bit = horizontal ? (positive ? 32 : 64) : (positive ? 8 : 16);
        lock (_gate)
        {
            _buttons |= bit;
            SendPointer();
            _buttons &= ~bit;
            SendPointer();
        }
    }

    public void PressKeys(string keys)
    {
        ArgumentNullException.ThrowIfNull(keys);
        var symbols = keys.Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).Select(KeySymbol).ToArray();
        if (symbols.Length == 0 || symbols.Length > 6)
        {
            System.Diagnostics.Trace.TraceWarning($"RfbInput.PressKeys: rejected a combination of {symbols.Length} keys");
            throw new ArgumentException("Use a simple guest key combination.");
        }
        lock (_gate)
        {
            foreach (var symbol in symbols) SendKey(symbol, true);
            Thread.Sleep(20);
            foreach (var symbol in symbols.Reverse()) SendKey(symbol, false);
        }
    }

    public void Wheel(bool positive, bool horizontal, GuestKeyModifiers modifiers)
    {
        var keys = new[] { (GuestKeyModifiers.Control, 0xffe3u), (GuestKeyModifiers.Shift, 0xffe1u), (GuestKeyModifiers.Alt, 0xffe9u) }
            .Where(k => modifiers.HasFlag(k.Item1)).Select(k => k.Item2).ToArray();
        lock (_gate)
        {
            try { foreach (var key in keys) SendKey(key, true); Wheel(positive, horizontal); }
            finally { foreach (var key in keys.Reverse()) SendKey(key, false); }
        }
    }

    public void TypeText(string text, Func<bool>? shouldStop)
    {
        ArgumentNullException.ThrowIfNull(text);
        System.Diagnostics.Trace.WriteLine($"RfbInput.TypeText enter: {text.Length} chars");
        var typed = 0;
        foreach (var rune in text.EnumerateRunes())
        {
            if (shouldStop?.Invoke() == true)
            {
                System.Diagnostics.Trace.WriteLine($"RfbInput.TypeText: stopped after {typed} characters");
                break;
            }
            var symbol = TextSymbol(rune.Value);
            lock (_gate)
            {
                SendKey(symbol, true);
                SendKey(symbol, false);
            }
            Thread.Sleep(12);
            typed++;
        }
        System.Diagnostics.Trace.WriteLine($"RfbInput.TypeText exit: typed {typed} characters");
    }

    private static readonly Dictionary<string, uint> KeyNames = new(StringComparer.OrdinalIgnoreCase)
    {
        ["ctrl"] = 0xffe3, ["control"] = 0xffe3, ["alt"] = 0xffe9, ["shift"] = 0xffe1, ["super"] = 0xffeb,
        ["enter"] = 0xff0d, ["return"] = 0xff0d, ["escape"] = 0xff1b, ["esc"] = 0xff1b, ["backspace"] = 0xff08,
        ["delete"] = 0xffff, ["tab"] = 0xff09, ["space"] = 0x20, ["insert"] = 0xff63,
        ["home"] = 0xff50, ["left"] = 0xff51, ["up"] = 0xff52, ["right"] = 0xff53, ["down"] = 0xff54,
        ["pageup"] = 0xff55, ["pagedown"] = 0xff56, ["end"] = 0xff57,
        ["equal"] = '=', ["minus"] = '-', ["slash"] = '/', ["period"] = '.', ["comma"] = ',', ["backslash"] = '\\'
    };

    /// <summary>The X keysym for a key name in a combination ("ctrl", "enter", "f5", "l"); the names <see cref="QemuInput"/> takes.</summary>
    internal static uint KeySymbol(string key)
    {
        ArgumentNullException.ThrowIfNull(key);
        if (KeyNames.TryGetValue(key, out var symbol)) return symbol;
        key = key.ToLowerInvariant();
        if (key.Length == 1 && char.IsAsciiLetterOrDigit(key[0])) return key[0];
        if (key.StartsWith('f') && int.TryParse(key[1..], out var f) && f >= 1 && f <= 12) return (uint)(0xffbe + f - 1);
        System.Diagnostics.Trace.TraceWarning($"RfbInput.KeySymbol: unsupported guest key '{key}'");
        throw new ArgumentException("Unsupported guest key: " + key);
    }

    /// <summary>The X keysym for one typed character: Latin-1 is itself, the rest of Unicode is 0x01000000 + the code point.</summary>
    internal static uint TextSymbol(int codePoint) => codePoint switch
    {
        '\n' => 0xff0d,
        '\t' => 0xff09,
        >= 0x20 and <= 0x7e or >= 0xa0 and <= 0xff => (uint)codePoint,
        >= 0x100 => 0x01000000u + (uint)codePoint,
        _ => throw new ArgumentException($"Control character U+{codePoint:X4} can't be typed; use a key combination.")
    };

    private void SendPointer()
    {
        Span<byte> message = stackalloc byte[6];
        message[0] = 5;
        message[1] = (byte)_buttons;
        BinaryPrimitives.WriteUInt16BigEndian(message[2..], (ushort)Math.Clamp(_pointer.X, 0, ushort.MaxValue));
        BinaryPrimitives.WriteUInt16BigEndian(message[4..], (ushort)Math.Clamp(_pointer.Y, 0, ushort.MaxValue));
        Send(message);
    }

    private void SendKey(uint symbol, bool down)
    {
        Span<byte> message = stackalloc byte[8];
        message[0] = 4;
        message[1] = down ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt32BigEndian(message[4..], symbol);
        Send(message);
    }

    public ValueTask DisposeAsync()
    {
        IAsyncDisposable? connection;
        lock (_gate)
        {
            _disposed = true;
            connection = _connection;
            _connection = null;
        }
        System.Diagnostics.Trace.WriteLine($"RfbInput.DisposeAsync: closing (had connection={connection != null})");
        return connection?.DisposeAsync() ?? ValueTask.CompletedTask;
    }
}
