using Automaton.Core.Services.ComputerUse;
using SkiaSharp;

namespace PersonaBot;

/// <summary>
/// Automaton's screen and input contracts on top of one guest screen (the main display or a bot's own). Clicks, drags and scrolls
/// are built here from the runtime's primitive pointer and button events, so QEMU and proot
/// behave the same way and both report the pointer to the viewer.
/// </summary>
public sealed class GuestComputer : IComputerScreen, IComputerInput
{
    private readonly IGuestScreen _screen;
    private readonly IGuestInput _input;
    private ScreenPoint _pointer;

    public GuestComputer(IGuestScreen screen, string displayName = "PersonaBot Linux guest")
    {
        _screen = screen ?? throw new ArgumentNullException(nameof(screen));
        DisplayName = displayName;
        _input = screen.CreateInput();
        var bounds = screen.ScreenBounds;
        _pointer = new ScreenPoint(bounds.X + bounds.Width / 2, bounds.Y + bounds.Height / 2);
        System.Diagnostics.Trace.WriteLine($"GuestComputer: on {screen.GetType().Name}, {bounds.Width}x{bounds.Height} at {bounds.X},{bounds.Y}");
    }

    public string DisplayName { get; }

    /// <summary>The screen as a PNG, straight from the screen (cu_tags decodes it once, itself).</summary>
    public Task<byte[]> CaptureAsync(CancellationToken token) => _screen.CaptureAsync(token);

    public event Action<ScreenPoint>? PointerMoved;

    public event Action<ScreenPoint>? PointerClicked;

    /// <summary>Clamps to PersonaBot's standard 1280x800 guest screen.</summary>
    public static ScreenPoint ClampPoint(ScreenPoint point) => ClampPoint(point, new ScreenRect(0, 0, 1280, 800));

    public static ScreenPoint ClampPoint(ScreenPoint point, ScreenRect bounds) =>
        new(Math.Clamp(point.X, bounds.X, bounds.Right - 1), Math.Clamp(point.Y, bounds.Y, bounds.Bottom - 1));

    /// <summary>A guest pixel as a 0-32767 absolute coordinate, reaching the last pixel exactly.</summary>
    public static int AbsoluteCoordinate(int coordinate, int pixels) =>
        (int)Math.Round(Math.Clamp(coordinate, 0, pixels - 1) * 32767d / (pixels - 1));

    public ScreenRect GetVirtualDesktopBounds() => _screen.ScreenBounds;

    public IReadOnlyList<MonitorInfo> GetMonitors() =>
        [new() { Index = 1, DeviceName = DisplayName, Bounds = _screen.ScreenBounds, WorkArea = _screen.ScreenBounds, IsPrimary = true }];

    public SKBitmap CaptureVirtualDesktop()
    {
        var bytes = _screen.CaptureAsync(CancellationToken.None).GetAwaiter().GetResult();
        return SkiaImages.Decode(bytes);
    }

    public SKBitmap CaptureRegion(ScreenRect region)
    {
        using var source = CaptureVirtualDesktop();
        return SkiaImages.Crop(source, region);
    }

    public void MoveTo(ScreenPoint point)
    {
        _pointer = ClampPoint(point, _screen.ScreenBounds);
        _input.MoveTo(_pointer);
        PointerMoved?.Invoke(_pointer);
    }

    public void Click(ScreenPoint point, MouseButton button = MouseButton.Left, int count = 1)
    {
        System.Diagnostics.Trace.WriteLine($"GuestComputer.Click enter: {button} x{count} at {point.X},{point.Y}");
        MoveTo(point);
        for (var i = 0; i < Math.Clamp(count, 1, 3); i++)
        {
            _input.Button(button, true);
            PointerClicked?.Invoke(_pointer);
            Thread.Sleep(40);
            _input.Button(button, false);
            Thread.Sleep(70);
        }
        System.Diagnostics.Trace.WriteLine($"GuestComputer.Click exit: at {_pointer.X},{_pointer.Y}");
    }

    public void Drag(ScreenPoint from, ScreenPoint to, MouseButton button = MouseButton.Left, PointerPath? path = null, TimeSpan? duration = null)
    {
        System.Diagnostics.Trace.WriteLine($"GuestComputer.Drag enter: {button} from {from.X},{from.Y} to {to.X},{to.Y}");
        MoveTo(from);
        _input.Button(button, true);
        try
        {
            for (var i = 1; i <= 20; i++)
            {
                MoveTo(new ScreenPoint(from.X + (to.X - from.X) * i / 20, from.Y + (to.Y - from.Y) * i / 20));
                Thread.Sleep(15);
            }
        }
        finally
        {
            _input.Button(button, false);
            System.Diagnostics.Trace.WriteLine($"GuestComputer.Drag exit: released at {_pointer.X},{_pointer.Y}");
        }
    }

    /// <summary>Streams a real operator gesture, retaining the button until release or cancellation.</summary>
    public async Task PointerGestureAsync(ScreenPoint from, MouseButton button, IAsyncEnumerable<ScreenPoint> positions, CancellationToken token)
    {
        System.Diagnostics.Trace.WriteLine($"GuestComputer.PointerGestureAsync enter: {button} from {from.X},{from.Y}");
        MoveTo(from);
        _input.Button(button, true);
        PointerClicked?.Invoke(_pointer);
        try
        {
            // Give the emulated USB device a poll even for a very quick click.
            await Task.Delay(40, token);
            await foreach (var point in positions.WithCancellation(token)) MoveTo(point);
        }
        finally
        {
            _input.Button(button, false);
            System.Diagnostics.Trace.WriteLine($"GuestComputer.PointerGestureAsync exit: {button} released at {_pointer.X},{_pointer.Y}{(token.IsCancellationRequested ? " (cancelled)" : "")}");
        }
    }

    public void Scroll(ScreenPoint point, int notches, bool horizontal = false) => Scroll(point, notches, horizontal, GuestKeyModifiers.None);

    public void Scroll(ScreenPoint point, int notches, bool horizontal, GuestKeyModifiers modifiers)
    {
        MoveTo(point);
        for (var i = 0; i < Math.Min(50, Math.Abs(notches)); i++)
        {
            _input.Wheel(notches > 0, horizontal, modifiers);
            Thread.Sleep(15);
        }
    }

    public void PressKeys(string keys)
    {
        if (string.IsNullOrWhiteSpace(keys))
        {
            throw new ArgumentException("A key combination is required.", nameof(keys));
        }
        _input.PressKeys(keys);
        Thread.Sleep(30);
    }

    public void TypeText(string text, Func<bool>? shouldStop = null)
    {
        ArgumentNullException.ThrowIfNull(text);
        _input.TypeText(text.Replace("\r\n", "\n"), shouldStop);
    }

    public ScreenPoint GetCursorPosition() => _pointer;
}
