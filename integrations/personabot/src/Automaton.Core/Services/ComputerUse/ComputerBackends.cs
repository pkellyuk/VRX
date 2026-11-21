using SkiaSharp;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// The screen for a desktop or guest computer-use session.
/// </summary>
public interface IComputerScreen
{
    /// <summary>The whole screen area, in the coordinates input is given in.</summary>
    ScreenRect GetVirtualDesktopBounds();

    IReadOnlyList<MonitorInfo> GetMonitors();

    /// <summary>A picture of part of the screen, in <see cref="SKColorType.Bgra8888"/>. The caller disposes it.</summary>
    SKBitmap CaptureRegion(ScreenRect region);

    /// <summary>A picture of the whole screen, in <see cref="SKColorType.Bgra8888"/>. The caller disposes it.</summary>
    SKBitmap CaptureVirtualDesktop();
}

/// <summary>The mouse and keyboard computer use drives, on the same machine as its <see cref="IComputerScreen"/>.</summary>
public interface IComputerInput
{
    void MoveTo(ScreenPoint point);

    void Click(ScreenPoint point, MouseButton button = MouseButton.Left, int count = 1);

    void Drag(ScreenPoint from, ScreenPoint to, MouseButton button = MouseButton.Left, PointerPath? path = null, TimeSpan? duration = null);

    /// <summary>Scrolls the wheel at a point. Positive notches scroll up or left.</summary>
    void Scroll(ScreenPoint point, int notches, bool horizontal = false);

    void TypeText(string text, Func<bool>? shouldStop = null);

    /// <summary>A key combination such as "ctrl+l", "alt+f4", "enter".</summary>
    void PressKeys(string keys);

    /// <summary>Where the pointer is, as far as this side knows.</summary>
    ScreenPoint GetCursorPosition();
}

/// <summary>
/// The windows on the machine computer use is driving - listed, found by point, brought to the front.
/// Guest sessions use <see cref="NoComputerWindows"/>.
/// </summary>
public interface IComputerWindows
{
    WindowSnapshot Capture();

    (IntPtr Handle, uint ProcessId) TopLevelWindowAt(ScreenPoint point);

    bool BringToFront(IntPtr handle);
}

/// <summary>A machine whose windows cannot be seen from here: no windows, nothing to focus.</summary>
public sealed class NoComputerWindows : IComputerWindows
{
    public static readonly NoComputerWindows Instance = new();

    public WindowSnapshot Capture() => new();

    public (IntPtr Handle, uint ProcessId) TopLevelWindowAt(ScreenPoint point) => (IntPtr.Zero, 0);

    public bool BringToFront(IntPtr handle) => false;
}
