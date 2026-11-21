namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// Broadcasts every view the model is shown, so a UI can display the descent as it
/// happens.
///
/// Separate from the chat transcript on purpose. The transcript shows what the
/// conversation contained; this shows what the model actually looked at, in order,
/// including views that pruning has since removed from the conversation entirely. It
/// is also the only place a whole descent can be reviewed step by step after the
/// fact, without scrolling back through a chat.
///
/// Static, matching <c>SystemEvents</c>: subscribers are windows that come and go,
/// and threading a notification bus through the DI graph to reach them buys nothing.
/// Deliberately free of any platform annotation so UI projects can subscribe without
/// taking a dependency on the Windows-only capture stack.
/// </summary>
public static class ComputerUseViewFeed
{
    private static int _sequence;

    /// <summary>
    /// Raised on the thread that rendered the view — a background tool-execution
    /// thread, never the UI thread. Subscribers must marshal before touching UI.
    /// </summary>
    public static event EventHandler<ComputerUseViewEventArgs>? ViewPublished;

    /// <summary>Raised when a session takes control, so a viewer can reset and open.</summary>
    public static event EventHandler<ComputerUseSessionEventArgs>? SessionStarted;

    /// <summary>Raised when control is released, whatever the reason.</summary>
    public static event EventHandler<ComputerUseSessionEventArgs>? SessionEnded;

    /// <summary>
    /// Raised when the model aims at a point — to zoom, hover, click, drag or scroll —
    /// so a ghost cursor can travel there. Raised on the tool thread, before any real
    /// input for that action is injected.
    /// </summary>
    public static event EventHandler<ComputerUsePointerEventArgs>? PointerAimed;

    /// <summary>
    /// True when something is drawing the ghost cursor. The session only waits for the
    /// ghost to arrive when there is a ghost to wait for, so a headless host with no UI
    /// clicks exactly as fast as it always did.
    /// </summary>
    public static bool HasPointerSubscribers => PointerAimed != null;

    public static void PublishPointer(ComputerUsePointerEventArgs args)
    {
        ArgumentNullException.ThrowIfNull(args);

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ComputerUseViewFeed] PublishPointer: {args.Action} {args.From} -> {args.Target} over {args.Travel.TotalMilliseconds:0}ms, label={args.Label}");

        try
        {
            PointerAimed?.Invoke(null, args);
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"[ComputerUseViewFeed] PublishPointer: a subscriber threw, ignoring: {ex.GetType().Name}: {ex.Message}");
        }
    }

    public static void PublishView(ComputerUseViewEventArgs args)
    {
        ArgumentNullException.ThrowIfNull(args);

        args.Sequence = Interlocked.Increment(ref _sequence);

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ComputerUseViewFeed] PublishView: step {args.Sequence}, kind={args.Kind}, region={args.Region}, {args.ImageBytes.Length} bytes");

        try
        {
            ViewPublished?.Invoke(null, args);
        }
        catch (Exception ex)
        {
            // A broken viewer must never take the descent down with it.
            AutomatonDebug.LogError($"[ComputerUseViewFeed] PublishView: a subscriber threw, ignoring: {ex.GetType().Name}: {ex.Message}");
        }
    }

    public static void PublishSessionStarted(string reason)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ComputerUseViewFeed] PublishSessionStarted: reason='{reason}'");

        try
        {
            SessionStarted?.Invoke(null, new ComputerUseSessionEventArgs { Reason = reason ?? string.Empty });
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"[ComputerUseViewFeed] PublishSessionStarted: a subscriber threw, ignoring: {ex.GetType().Name}: {ex.Message}");
        }
    }

    public static void PublishSessionEnded(string reason)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ComputerUseViewFeed] PublishSessionEnded: reason='{reason}'");

        try
        {
            SessionEnded?.Invoke(null, new ComputerUseSessionEventArgs { Reason = reason ?? string.Empty });
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"[ComputerUseViewFeed] PublishSessionEnded: a subscriber threw, ignoring: {ex.GetType().Name}: {ex.Message}");
        }
    }
}

/// <summary>
/// One view the model was shown.
/// </summary>
public sealed class ComputerUseViewEventArgs : EventArgs
{
    /// <summary>1-based position in the whole run, for ordering and display.</summary>
    public int Sequence { get; set; }

    /// <summary>"grid", "zoom out" or "screenshot".</summary>
    public string Kind { get; set; } = "grid";

    /// <summary>Encoded image exactly as the model received it.</summary>
    public byte[] ImageBytes { get; set; } = Array.Empty<byte>();

    public string MediaType { get; set; } = "image/jpeg";

    public ScreenRect Region { get; set; }

    public int Rows { get; set; }

    public int Columns { get; set; }

    public double Magnification { get; set; }

    public double ScreenPitchX { get; set; }

    public double ScreenPitchY { get; set; }

    /// <summary>Zoom depth at the time, so the descent's shape is visible in the list.</summary>
    public int ZoomDepth { get; set; }

    /// <summary>Plain-language position on the desktop, e.g. "the bottom-left of the desktop".</summary>
    public string Where { get; set; } = string.Empty;

    public DateTimeOffset TimestampUtc { get; set; } = DateTimeOffset.UtcNow;
}

/// <summary>
/// What the model is doing at the point it aimed at, which decides how the ghost
/// cursor marks its arrival.
/// </summary>
public enum ComputerUsePointerAction
{
    /// <summary>Control was taken; the ghost appears where the real pointer is.</summary>
    Appear,

    /// <summary>Zooming towards a point. Nothing is pressed.</summary>
    Look,

    /// <summary>Hovering.</summary>
    Move,

    Click,

    /// <summary>Button pressed at the start of a drag.</summary>
    Press,

    /// <summary>Travelling with the button held, ending in a release.</summary>
    Drag,

    Scroll
}

/// <summary>
/// One aim of the pointer.
///
/// Carries the start point, travel time and seed as well as the target, because the
/// session has already committed to how long this move takes: it waits that long
/// before injecting the real input. The ghost must replay the same move, not plan its
/// own.
/// </summary>
public sealed class ComputerUsePointerEventArgs : EventArgs
{
    public ComputerUsePointerAction Action { get; set; }

    public ScreenPoint From { get; set; }

    public ScreenPoint Target { get; set; }

    public TimeSpan Travel { get; set; }

    /// <summary>Seed for <see cref="HumanPointerMotion.Plan"/>, so the path is reproducible.</summary>
    public int Seed { get; set; }

    /// <summary>The grid label aimed at, when there was one.</summary>
    public string Label { get; set; } = string.Empty;

    public MouseButton Button { get; set; } = MouseButton.Left;

    /// <summary>Clicks for <see cref="ComputerUsePointerAction.Click"/>; 2 for a double click.</summary>
    public int ClickCount { get; set; } = 1;

    /// <summary>Wheel notches for <see cref="ComputerUsePointerAction.Scroll"/>; positive is up or left.</summary>
    public int ScrollNotches { get; set; }

    public bool ScrollHorizontal { get; set; }

    public DateTimeOffset TimestampUtc { get; set; } = DateTimeOffset.UtcNow;

    /// <summary>The exact path the session planned for this aim.</summary>
    public PointerPath PlanPath() =>
        HumanPointerMotion.Plan(From, Target, Seed, overshoot: Action != ComputerUsePointerAction.Drag);
}

/// <summary>
/// A desktop-control session starting or ending.
/// </summary>
public sealed class ComputerUseSessionEventArgs : EventArgs
{
    public string Reason { get; set; } = string.Empty;

    public DateTimeOffset TimestampUtc { get; set; } = DateTimeOffset.UtcNow;
}
