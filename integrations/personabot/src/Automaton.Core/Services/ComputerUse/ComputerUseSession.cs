using System.Drawing;
using System.Runtime.Versioning;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// Holds the state of one period of desktop control: whether control is granted, what
/// region the model is currently looking at, and the label-to-screen-point map for
/// that view.
///
/// The label map is the reason this is stateful. The model names a label, never a
/// coordinate, so the only place a label can be turned back into a pixel is here —
/// and only against the view that produced it. A label from a stale view resolves to
/// a point that has moved, which is exactly the class of misclick this whole design
/// exists to avoid.
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class ComputerUseSession : IDisposable
{
    /// <summary>
    /// Chord that revokes control immediately. Polled rather than registered as a
    /// hotkey because <c>RegisterHotKey</c> and low-level hooks both need a message
    /// pump, which a headless host process does not have.
    ///
    /// Ctrl+Alt+Shift+F12 rather than anything involving Esc: Ctrl+Shift+Esc is Task
    /// Manager, and a kill switch that opens a window on the way past is worse than
    /// useless.
    /// </summary>
    public static readonly ushort[] KillChord =
    {
        VirtualKeys.Control, VirtualKeys.Menu, VirtualKeys.Shift, VirtualKeys.F12
    };

    public const string KillChordDescription = "Ctrl+Alt+Shift+F12";
    public event Action? EmergencyStop;

    /// <summary>
    /// Generous because of what actually drives this: a local vision model on a 4K
    /// screenshot takes one to two minutes per round trip, so a five-minute idle
    /// window expired mid-descent and revoked control from a model that was working
    /// normally. This measures abandonment, not thinking time.
    /// </summary>
    private static readonly TimeSpan DefaultIdleTimeout = TimeSpan.FromMinutes(20);
    private static readonly TimeSpan WatchdogInterval = TimeSpan.FromMilliseconds(100);

    private readonly bool _guestOnly;
    private readonly IComputerScreen _capture;
    private readonly GridOverlayRenderer _renderer;
    private readonly IComputerInput _input;
    private readonly object _gate = new();
    private readonly List<GridView> _zoomStack = new();

    /// <summary>
    /// Every region gridded since the screen last changed, in order.
    ///
    /// Exists because pruning old screenshots also prunes the model's memory of where
    /// it has already looked. Without this it can fall into a perfect limit cycle —
    /// zoom here, find nothing, zoom out, zoom to the same place again — and it did,
    /// for eleven consecutive round trips, with identical reasoning each time.
    ///
    /// Cleared whenever the model acts on the screen and when a new session begins.
    /// Looking at the same place again after a click is checking the result, not going
    /// round in circles, and the warning used to fire on exactly that. It also never
    /// reset between sessions, so a new task could inherit an old task's count.
    /// </summary>
    private readonly List<ScreenRect> _visited = new();

    /// <summary>
    /// How long a pointer rests on a target before the "before" picture is taken. The
    /// hover highlight a button or row shows when the pointer arrives is not the click's
    /// doing, and counting it made a missed click look like a hit.
    /// </summary>
    private static readonly TimeSpan HoverSettle = TimeSpan.FromMilliseconds(150);

    private readonly ScreenChangeDetector _changes;
    private readonly IComputerWindows _windows;
    private readonly ScreenTextFinder _textFinder;

    /// <summary>
    /// Slack added to the ghost's travel time before real input is injected. The ghost
    /// starts on the UI thread a dispatcher hop after the aim is published, so without
    /// this the click lands a frame before the ghost does.
    /// </summary>
    private static readonly TimeSpan GhostArrivalSlack = TimeSpan.FromMilliseconds(40);

    private CancellationTokenSource? _watchdog;
    private bool _isActive;
    private bool _disposed;

    /// <summary>Where the ghost cursor was last sent, and so where its next move starts.</summary>
    private ScreenPoint _ghostAt;
    private int _aimSequence;

    /// <summary>Guest-only composition: there is no host screen, input or window fallback.</summary>
    /// <param name="recognizer">Reads on-screen text for cu_find; Windows OCR when omitted on Windows.</param>
    public ComputerUseSession(IComputerScreen screen, GridOverlayRenderer renderer, IComputerInput input,
        ITextRecognizer? recognizer = null, IComputerWindows? desktopWindows = null)
    {
        _guestOnly = desktopWindows == null;
        _capture = screen ?? throw new ArgumentNullException(nameof(screen));
        _renderer = renderer ?? throw new ArgumentNullException(nameof(renderer));
        _input = input ?? throw new ArgumentNullException(nameof(input));
        _windows = desktopWindows ?? NoComputerWindows.Instance;
        _changes = new ScreenChangeDetector(_capture, _windows);
        _textFinder = new ScreenTextFinder(_capture, _windows, ScreenTextFinder.DefaultUpscale, recognizer);
    }

    /// <summary>How long control survives with no tool call before it lapses.</summary>
    public TimeSpan IdleTimeout { get; set; } = DefaultIdleTimeout;

    public bool IsActive
    {
        get
        {
            lock (_gate)
            {
                return _isActive;
            }
        }
    }

    /// <summary>Why control ended, for the message the model gets when it tries to act anyway.</summary>
    public string? EndedReason { get; private set; }

    public DateTimeOffset LastActivityUtc { get; private set; }

    public IReadOnlyList<MonitorInfo> Monitors { get; private set; } = Array.Empty<MonitorInfo>();

    /// <summary>The view the model is currently looking at, or null before the first grid.</summary>
    public GridView? CurrentView
    {
        get
        {
            lock (_gate)
            {
                return _zoomStack.Count > 0 ? _zoomStack[^1] : null;
            }
        }
    }

    /// <summary>
    /// True once something has changed on screen under at least one label of the
    /// current view since it was rendered.
    /// </summary>
    public bool ViewIsStale => _allLabelsStale || _staleLabels.Count > 0;

    /// <summary>
    /// Grid labels whose point sits inside an area the screen changed in since the view
    /// was rendered. Per label rather than per view: a dialog opening on the far side of
    /// the screen used to mark every label stale and drop every text match, when what
    /// sat under the untouched labels had, by definition, not moved. Reset by a new
    /// grid view.
    /// </summary>
    private readonly HashSet<string> _staleLabels = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Set when the screen could not be compared: no evidence, so nothing is trusted.</summary>
    private bool _allLabelsStale;

    /// <summary>Text labels dropped by the most recent action, for its result.</summary>
    private IReadOnlyList<string> _lastDroppedTextLabels = Array.Empty<string>();

    /// <summary>Grid labels the most recent action put over changed pixels, for its result.</summary>
    private IReadOnlyList<string> _lastNewlyStaleLabels = Array.Empty<string>();

    /// <summary>Whether a specific grid label is over pixels that changed after its view was rendered.</summary>
    public bool IsLabelStale(string? label)
    {
        if (_allLabelsStale)
        {
            return true;
        }

        lock (_gate)
        {
            return !string.IsNullOrWhiteSpace(label) && _staleLabels.Contains(label.Trim());
        }
    }

    private void ClearStaleness()
    {
        lock (_gate)
        {
            _staleLabels.Clear();
            _allLabelsStale = false;
            _lastDroppedTextLabels = Array.Empty<string>();
            _lastNewlyStaleLabels = Array.Empty<string>();
        }
    }

    /// <summary>
    /// One line for an action result saying which labels the model can still use — the
    /// answer to "do I have to re-grid?", which it used to have to assume was yes.
    /// Empty when no labels are in play.
    /// </summary>
    public string DescribeLabelValidity()
    {
        var view = CurrentView;
        var texts = TextTargets;

        if (view is null && texts.Count == 0)
        {
            return string.Empty;
        }

        if (_allLabelsStale)
        {
            return "Labels: the screen could not be compared, so treat every label as stale — cu_grid or cu_find_text again before acting.";
        }

        List<string> stale;
        IReadOnlyList<string> dropped;
        lock (_gate)
        {
            stale = _staleLabels.OrderBy(l => l, StringComparer.OrdinalIgnoreCase).ToList();
            dropped = _lastDroppedTextLabels;
        }

        var parts = new List<string>();

        if (view is not null)
        {
            var total = view.Labels.Count;
            if (stale.Count == 0)
            {
                parts.Add($"all {total} grid labels are still over what you saw");
            }
            else if (stale.Count >= total)
            {
                parts.Add("every grid label is over changed pixels");
            }
            else
            {
                parts.Add($"grid labels {string.Join(", ", stale)} are over changed pixels; the other {total - stale.Count} are still over what you saw");
            }
        }

        if (texts.Count > 0)
        {
            parts.Add($"text labels {string.Join(", ", texts.Select(t => t.Label))} are still valid");
        }

        if (dropped.Count > 0)
        {
            parts.Add($"{string.Join(", ", dropped)} were dropped because the screen changed under them");
        }

        var anyStale = stale.Count > 0 || dropped.Count > 0;
        var advice = anyStale
            ? " Valid labels can be used as they are; re-grid or cu_find_text only for what changed."
            : " No need to re-grid to act on them.";

        return $"Labels: {string.Join("; ", parts)}.{advice}";
    }


    /// <summary>
    /// Whether the labels of the current grid view may no longer be over what the model
    /// saw — the only sense in which "stale" is worth reporting.
    ///
    /// Not <see cref="ViewIsStale"/> on its own: that flag is set by every action whether
    /// or not a grid view exists, and the click result printed it for text labels too. A
    /// run of ten cu_find_text clicks carried "the view was already stale" on eight of
    /// them with no grid view in play at all, and the model concluded that every action
    /// invalidated its labels. A text label cannot be stale: they are dropped the moment
    /// the screen changes, so one that resolves is fresh by construction.
    /// </summary>
    public bool GridViewIsStale => ViewIsStale && CurrentView is not null;

    /// <summary>True for a cu_find_text label such as "T3", as opposed to a grid label.</summary>
    public static bool IsTextLabel(string? label)
    {
        if (string.IsNullOrWhiteSpace(label))
        {
            return false;
        }

        var trimmed = label.Trim();
        return trimmed.Length >= 2 && char.ToUpperInvariant(trimmed[0]) == 'T' && trimmed[1..].All(char.IsAsciiDigit);
    }

    public int ZoomDepth
    {
        get
        {
            lock (_gate)
            {
                return _zoomStack.Count;
            }
        }
    }

    public ScreenRect DesktopBounds => _capture.GetVirtualDesktopBounds();

    /// <summary>Starts control of the session's configured desktop.</summary>
    public IReadOnlyList<MonitorInfo> Begin(string reason)
    {
        AutomatonDebug.Log($"ComputerUseSession.Begin: Entry - reason='{reason}'");

        if (string.IsNullOrWhiteSpace(reason))
        {
            throw new ArgumentException("A reason for taking control is required.", nameof(reason));
        }

        ObjectDisposedException.ThrowIf(_disposed, this);

        lock (_gate)
        {
            _zoomStack.Clear();
            _visited.Clear();
            _lastChangeReport = null;
            _textTargets = Array.Empty<TextTarget>();
            _isActive = true;
        }

        EndedReason = null;
        ClearStaleness();
        LastActivityUtc = DateTimeOffset.UtcNow;
        Monitors = _capture.GetMonitors();

        StartWatchdog();
        ComputerUseViewFeed.PublishSessionStarted(reason);

        // The ghost starts where the operator's real pointer is, so its first move
        // visibly comes from somewhere they recognise.
        var pointer = _input.GetCursorPosition();
        lock (_gate)
        {
            _ghostAt = pointer;
        }

        Aim(ComputerUsePointerAction.Appear, pointer, string.Empty, waitForArrival: false);

        AutomatonDebug.Log($"ComputerUseSession.Begin: Exit - control granted over {Monitors.Count} monitor(s), desktop={DesktopBounds}, kill chord={KillChordDescription}, idle timeout={IdleTimeout.TotalMinutes:0.#} min");
        return Monitors;
    }

    /// <summary>
    /// Releases control. Safe to call when control was never taken.
    /// </summary>
    public void End(string reason)
    {
        AutomatonDebug.Log($"ComputerUseSession.End: Entry - reason='{reason}', wasActive={IsActive}");

        lock (_gate)
        {
            _isActive = false;
            _zoomStack.Clear();
            _textTargets = Array.Empty<TextTarget>();
        }

        EndedReason = string.IsNullOrWhiteSpace(reason) ? "released" : reason;
        ClearStaleness();

        StopWatchdog();
        ComputerUseViewFeed.PublishSessionEnded(EndedReason);

        AutomatonDebug.Log($"ComputerUseSession.End: Exit - control released ({EndedReason})");
    }

    /// <summary>
    /// Captures a region, draws the labelled grid over it, and makes it the current
    /// view. Pushes onto the zoom stack unless it replaces the view at the same depth.
    /// </summary>
    public GridView Grid(ScreenRect region, int rows, int columns, bool pushZoom)
    {
        AutomatonDebug.Log($"ComputerUseSession.Grid: Entry - region={region}, rows={rows}, columns={columns}, pushZoom={pushZoom}");

        EnsureUsable();

        var clamped = region.ClampInside(DesktopBounds);
        if (clamped.IsEmpty)
        {
            throw new ArgumentException($"Region {region} does not intersect the desktop {DesktopBounds}.", nameof(region));
        }

        using var bitmap = _capture.CaptureRegion(clamped);
        var view = _renderer.Render(bitmap, clamped, rows, columns);

        lock (_gate)
        {
            if (!pushZoom && _zoomStack.Count > 0)
            {
                _zoomStack[^1] = view;
            }
            else
            {
                _zoomStack.Add(view);
            }
        }

        ClearStaleness();
        Touch();
        RecordViewed(clamped, "the grid view", view);

        lock (_gate)
        {
            _visited.Add(clamped);
        }

        PublishView(view, pushZoom ? "grid" : "re-grid");

        AutomatonDebug.Log($"ComputerUseSession.Grid: Exit - region={clamped}, depth={ZoomDepth}, pitch={view.ScreenPitchX:0.#}x{view.ScreenPitchY:0.#}px, {view.Labels.Count} labels, visitedBefore={TimesVisited(clamped) - 1}");
        return view;
    }

    /// <summary>
    /// Re-renders the view one level up the zoom stack. Returns null when already at
    /// the top, which the caller reports rather than treating as an error.
    /// </summary>
    public GridView? ZoomOut()
    {
        AutomatonDebug.Log($"ComputerUseSession.ZoomOut: Entry - depth={ZoomDepth}");

        EnsureUsable();

        GridView? parent;

        lock (_gate)
        {
            if (_zoomStack.Count < 2)
            {
                AutomatonDebug.Log("ComputerUseSession.ZoomOut: Exit - already at the top of the zoom stack");
                return null;
            }

            _zoomStack.RemoveAt(_zoomStack.Count - 1);
            parent = _zoomStack[^1];
            _zoomStack.RemoveAt(_zoomStack.Count - 1);
        }

        // Re-capture rather than reuse the stored image: the screen has very likely
        // changed since that view was rendered, and handing back a stale picture is
        // how a model ends up clicking something that is no longer there.
        var refreshed = Grid(parent.Region, parent.Rows, parent.Columns, pushZoom: true);

        AutomatonDebug.Log($"ComputerUseSession.ZoomOut: Exit - region={refreshed.Region}, depth={ZoomDepth}");
        return refreshed;
    }

    /// <summary>
    /// Captures a region with no grid drawn on it, for when the chips are covering
    /// the thing the model is trying to identify.
    /// </summary>
    /// <param name="showPointer">
    /// Marks where the mouse pointer actually is. Off by default, and never set by
    /// the grid path: this exists to close the aim loop after <c>cu_move</c>, which
    /// resolves a label to a point exactly as <c>cu_click</c> does, so what the
    /// marker confirms is the real click target.
    /// </param>
    /// <returns>The encoded image with the geometry needed to describe its scale.</returns>
    public GridView Screenshot(ScreenRect? region, bool showPointer = false)
    {
        AutomatonDebug.Log($"ComputerUseSession.Screenshot: Entry - region={region?.ToString() ?? "current"}, showPointer={showPointer}");

        EnsureUsable();

        var target = region ?? CurrentView?.Region ?? DesktopBounds;
        var clamped = target.ClampInside(DesktopBounds);

        using var bitmap = _capture.CaptureRegion(clamped);

        // Rendering a 1x1 grid gives the same crop, scaling and encoding path as a
        // normal view; the only difference is that the four labels sit on the corners
        // where they cover nothing interesting.
        ScreenPoint? pointer = null;
        if (showPointer)
        {
            pointer = _input.GetCursorPosition();
            AutomatonDebug.Log($"ComputerUseSession.Screenshot: pointer is at ({pointer.Value.X},{pointer.Value.Y})");
        }

        var view = _renderer.Render(bitmap, clamped, 1, 1, pointer: pointer);
        Touch();
        RecordViewed(clamped, "the screenshot", view);

        PublishView(view, "screenshot");

        AutomatonDebug.Log($"ComputerUseSession.Screenshot: Exit - region={clamped}, {view.ImageBytes.Length} bytes");
        return view;
    }

    /// <summary>
    /// Turns a label from the current view into the screen point it marks.
    /// </summary>
    public GridLabel ResolveLabel(string? label)
    {
        EnsureUsable();

        // Text matches first: they need no grid view to exist, which is the point of them.
        if (TryResolveTextTarget(label, out var textTarget))
        {
            return new GridLabel { Label = textTarget.Label, Point = textTarget.Point };
        }

        var view = CurrentView;
        if (view is null)
        {
            throw new InvalidOperationException("There is no current view. Call cu_grid first so the labels exist, or cu_find_text to get labels for a piece of text.");
        }

        var resolved = view.Resolve(label);
        if (resolved is null)
        {
            var first = GridLabelNaming.Build(0, 0);
            var last = GridLabelNaming.Build(view.Rows, view.Columns);

            throw new ArgumentException(
                $"'{label}' is not a label in the current view. Labels run {first} to {last}: rows are letters A onwards, columns are numbers from 1.",
                nameof(label));
        }

        return resolved;
    }

    /// <summary>
    /// The region one cell across centred on a label — what a zoom step crops.
    /// </summary>
    public ScreenRect CellAround(GridLabel label)
    {
        ArgumentNullException.ThrowIfNull(label);

        // around="T1" zooms onto found text: the text with room around it to see what
        // it belongs to — the button it labels, the row it sits in.
        var text = TextTargets.FirstOrDefault(t => t.Label.Equals(label.Label, StringComparison.OrdinalIgnoreCase));
        if (text is not null)
        {
            var marginX = Math.Max(120, text.Bounds.Width);
            var marginY = Math.Max(80, text.Bounds.Height * 3);
            return new ScreenRect(
                text.Bounds.X - marginX,
                text.Bounds.Y - marginY,
                text.Bounds.Width + 2 * marginX,
                text.Bounds.Height + 2 * marginY).ClampInside(DesktopBounds);
        }

        var view = CurrentView ?? throw new InvalidOperationException("There is no current view to zoom into.");
        return view.CellAround(label, DesktopBounds);
    }

    /// <summary>Default width and height, in screen pixels, of the square a peek shows.</summary>
    public const int DefaultPeekSize = 120;

    /// <summary>
    /// A small magnified look at what one label is sitting on, crosshair on the exact
    /// click point. Does not become the current view and does not touch the labels in
    /// play: it exists to check a marker before committing a click, at a tenth of the
    /// cost of a grid view.
    /// </summary>
    /// <summary>
    /// The region of the last image the model was shown — a grid view, a screenshot, a
    /// peek — and how it was described.
    ///
    /// This is the frame fractions are measured in. "42% across" means 42% across the
    /// picture the model is looking at, which is the only frame it can actually see; a
    /// fraction of the whole desktop would be a different point whenever the last look
    /// was a crop. The rect itself never goes stale — a region is a region whatever
    /// happens inside it — so only the pixels within it can mislead.
    /// </summary>
    /// <remarks>
    /// <c>ImageWidth</c> and <c>ImageHeight</c> are the size of the picture as sent: a model that reads a point
    /// off it in its own pixels is in those, not in screen pixels, whenever the picture was scaled.
    /// </remarks>
    public (ScreenRect Region, string Description, int ImageWidth, int ImageHeight)? LastViewed
    {
        get
        {
            lock (_gate)
            {
                return _lastViewed;
            }
        }
    }

    private (ScreenRect Region, string Description, int ImageWidth, int ImageHeight)? _lastViewed;

    private void RecordViewed(ScreenRect region, string description, GridView view)
    {
        lock (_gate)
        {
            _lastViewed = (region, description, view.ImageWidth > 0 ? view.ImageWidth : region.Width, view.ImageHeight > 0 ? view.ImageHeight : region.Height);
        }
    }

    /// <summary>
    /// Turns a fraction of a region into a screen point: (0,0) its top-left corner,
    /// (1,1) its bottom-right.
    /// </summary>
    public static ScreenPoint PointFromFraction(ScreenRect region, double fractionX, double fractionY)
    {
        var x = region.X + (int)Math.Round(Math.Clamp(fractionX, 0, 1) * (region.Width - 1));
        var y = region.Y + (int)Math.Round(Math.Clamp(fractionY, 0, 1) * (region.Height - 1));
        return new ScreenPoint(x, y);
    }

    /// <summary>
    /// A magnified look at any screen point, with a crosshair on it. What cu_peek does
    /// for a label, and what cu_click_at verify=true does for a coordinate.
    /// </summary>
    public GridView PeekAt(ScreenPoint point, int size)
    {
        AutomatonDebug.Log($"ComputerUseSession.PeekAt: Entry - point={point}, size={size}");

        EnsureUsable();

        if (size < 40 || size > 400)
        {
            throw new ArgumentOutOfRangeException(nameof(size), size, "A peek is between 40 and 400 screen pixels across.");
        }

        // Clamping may shift the square off-centre at a screen edge; the crosshair is
        // drawn at the true point regardless.
        var region = new ScreenRect(point.X - size / 2, point.Y - size / 2, size, size).ClampInside(DesktopBounds);

        using var bitmap = _capture.CaptureRegion(region);
        var view = _renderer.RenderPeek(bitmap, region, point);

        Touch();

        // Deliberately does not become the frame fractions are measured in. A peek is a
        // detail view of a point already named, and cu_click_at verify=true tells the
        // model to call again with the same at= — which would land somewhere else if the
        // peek had moved the frame under it. Measured 17px off before this.
        PublishView(view, "peek");

        AutomatonDebug.Log($"ComputerUseSession.PeekAt: Exit - region={region}, {view.ImageWidth}x{view.ImageHeight} image, {view.ImageBytes.Length} bytes");
        return view;
    }

    public GridView Peek(GridLabel label, int size)
    {
        ArgumentNullException.ThrowIfNull(label);

        AutomatonDebug.Log($"ComputerUseSession.Peek: Entry - label={label.Label}, size={size}");
        return PeekAt(label.Point, size);
    }

    /// <summary>The matches from the most recent cu_find_text, until the screen changes.</summary>
    public IReadOnlyList<TextTarget> TextTargets
    {
        get
        {
            lock (_gate)
            {
                return _textTargets;
            }
        }
    }

    private IReadOnlyList<TextTarget> _textTargets = Array.Empty<TextTarget>();

    /// <summary>
    /// Keep at most this many text labels. Past it the oldest go, so a long session does
    /// not accumulate hundreds of stale-looking numbers.
    /// </summary>
    private const int MaxTextTargets = 40;

    /// <summary>
    /// Reads the screen for a piece of text and makes each match clickable as a T label.
    ///
    /// Matches accumulate across searches: each lives until the screen changes under it,
    /// not until the next search. So after finding "Install" and then "Patriot (F:)",
    /// both are clickable, and clicking one leaves the other's label intact if its
    /// pixels did not change. A match already in play — same text, same place — keeps
    /// the label it had rather than getting a second one.
    /// </summary>
    public ScreenTextSearch FindText(string query, ScreenRect region)
    {
        EnsureUsable();

        var search = _textFinder.Find(query, region.ClampInside(DesktopBounds));

        lock (_gate)
        {
            var targets = new List<TextTarget>(_textTargets);
            var next = targets.Count == 0 ? 1 : targets.Max(t => int.Parse(t.Label[1..])) + 1;

            foreach (var target in search.Targets)
            {
                var existing = targets.FindIndex(t =>
                    t.Text.Equals(target.Text, StringComparison.Ordinal) &&
                    Math.Abs(t.Bounds.X - target.Bounds.X) <= 4 &&
                    Math.Abs(t.Bounds.Y - target.Bounds.Y) <= 4);

                if (existing >= 0)
                {
                    target.Label = targets[existing].Label;
                    targets[existing] = target;
                }
                else
                {
                    target.Label = $"T{next++}";
                    targets.Add(target);
                }
            }

            // Oldest first, but never one from this search.
            while (targets.Count > MaxTextTargets)
            {
                var victim = targets.FindIndex(t => !search.Targets.Contains(t));
                if (victim < 0)
                {
                    break;
                }

                targets.RemoveAt(victim);
            }

            _textTargets = targets;
        }

        Touch();
        return search;
    }

    /// <summary>
    /// An image of the matches, boxed and tagged, cropped to where they are with room
    /// around them. Does not become the current view — it has no grid labels.
    /// </summary>
    public GridView RenderTextTargets(IReadOnlyList<TextTarget> targets)
    {
        ArgumentNullException.ThrowIfNull(targets);

        EnsureUsable();

        if (targets.Count == 0)
        {
            throw new ArgumentException("There are no matches to show.", nameof(targets));
        }

        var left = targets.Min(t => t.Bounds.X) - 200;
        var top = targets.Min(t => t.Bounds.Y) - 200;
        var right = targets.Max(t => t.Bounds.Right) + 200;
        var bottom = targets.Max(t => t.Bounds.Bottom) + 200;

        // At least a readable 16:9 chunk of screen, so a single short match still shows
        // the button or row around it.
        const int minimumWidth = 960;
        const int minimumHeight = 540;
        var width = Math.Max(right - left, minimumWidth);
        var height = Math.Max(bottom - top, minimumHeight);
        var centreX = (left + right) / 2;
        var centreY = (top + bottom) / 2;

        var crop = new ScreenRect(centreX - width / 2, centreY - height / 2, width, height).ClampInside(DesktopBounds);

        using var bitmap = _capture.CaptureRegion(crop);
        var view = _renderer.RenderTextTargets(bitmap, crop, targets);

        Touch();
        RecordViewed(crop, "the image of the text matches", view);
        PublishView(view, "find text");
        return view;
    }

    /// <summary>
    /// Resolves "T1"-style labels. False for anything else, so grid labels carry on as
    /// before; throws for a T label that is not in play, rather than letting it fall
    /// through to the grid and fail with a message about rows and columns.
    /// </summary>
    private bool TryResolveTextTarget(string? label, out TextTarget target)
    {
        target = null!;

        if (string.IsNullOrWhiteSpace(label))
        {
            return false;
        }

        if (!IsTextLabel(label))
        {
            return false;
        }

        var trimmed = label!.Trim();

        var targets = TextTargets;
        var found = targets.FirstOrDefault(t => t.Label.Equals(trimmed, StringComparison.OrdinalIgnoreCase));

        if (found is null)
        {
            throw new ArgumentException(
                targets.Count == 0
                    ? $"'{trimmed}' is a text-match label, but none are in play. They come from cu_find_text and are cleared whenever the screen changes — call cu_find_text again."
                    : $"There is no {trimmed}. The text matches in play are T1 to T{targets.Count}.",
                nameof(label));
        }

        target = found;
        return true;
    }

    /// <summary>
    /// Sends the ghost cursor to a point the model is zooming towards. Nothing is
    /// pressed and nothing waits: this only shows where the model's attention went.
    /// </summary>
    public void Look(ScreenPoint point, string label)
    {
        EnsureUsable();
        Aim(ComputerUsePointerAction.Look, point, label, waitForArrival: false);
    }

    /// <summary>
    /// Clicks a label and reports what changed on screen as a result. The report is
    /// null only when the screen could not be compared, never when nothing changed.
    /// </summary>
    public ScreenChangeReport? Click(GridLabel label, MouseButton button, int count)
    {
        ArgumentNullException.ThrowIfNull(label);

        AutomatonDebug.Log($"ComputerUseSession.Click: Entry - label={label.Label}, point={label.Point}, button={button}, count={count}");

        EnsureUsable();
        Aim(ComputerUsePointerAction.Click, label.Point, label.Label, waitForArrival: true, button: button, clickCount: count);
        EnsureUsable();

        // Arrive first and let the hover state draw, then take the "before" picture, so
        // a row lighting up under the pointer is not mistaken for the click working.
        // Click still sends its own move in the same batch as the press.
        _input.MoveTo(label.Point);
        Thread.Sleep(HoverSettle);
        var before = TryCaptureBefore();

        _input.Click(label.Point, button, count);
        ClickPerformed?.Invoke(label.Point);
        var report = TryReportChange(before, label.Point);
        RecordActionOutcome(report);

        AutomatonDebug.Log($"ComputerUseSession.Click: Exit - clicked {label.Label} at {label.Point}, changed={report?.AnythingChanged.ToString() ?? "unknown"}");
        return report;
    }

    public ScreenChangeReport? Drag(GridLabel from, GridLabel to, MouseButton button)
    {
        ArgumentNullException.ThrowIfNull(from);
        ArgumentNullException.ThrowIfNull(to);

        AutomatonDebug.Log($"ComputerUseSession.Drag: Entry - {from.Label}{from.Point} -> {to.Label}{to.Point}, button={button}");

        EnsureUsable();
        Aim(ComputerUsePointerAction.Press, from.Point, from.Label, waitForArrival: true, button: button);
        EnsureUsable();

        _input.MoveTo(from.Point);
        Thread.Sleep(HoverSettle);
        var before = TryCaptureBefore();

        // The real drag follows the same planned path as the ghost, over the same time,
        // so the thing being dragged moves under the ghost rather than jumping at the end.
        var drag = Aim(ComputerUsePointerAction.Drag, to.Point, to.Label, waitForArrival: false, button: button);
        _input.Drag(from.Point, to.Point, button, drag.Path, drag.Travel);
        var report = TryReportChange(before, to.Point);
        RecordActionOutcome(report);

        AutomatonDebug.Log($"ComputerUseSession.Drag: Exit - dragged {from.Label} to {to.Label}, changed={report?.AnythingChanged.ToString() ?? "unknown"}");
        return report;
    }

    public ScreenChangeReport? MoveTo(GridLabel label)
    {
        ArgumentNullException.ThrowIfNull(label);

        AutomatonDebug.Log($"ComputerUseSession.MoveTo: Entry - label={label.Label}, point={label.Point}");

        EnsureUsable();
        Aim(ComputerUsePointerAction.Move, label.Point, label.Label, waitForArrival: true);
        EnsureUsable();

        // Here the hover IS the point, so the picture is taken before the pointer moves.
        var before = TryCaptureBefore();
        _input.MoveTo(label.Point);

        // A hover can change what is on screen (tooltips, menus, highlights) even though
        // nothing was pressed, so it is judged by what actually changed, like a click.
        var report = TryReportChange(before, label.Point);
        RecordActionOutcome(report);

        AutomatonDebug.Log($"ComputerUseSession.MoveTo: Exit - pointer at {label.Point}, changed={report?.AnythingChanged.ToString() ?? "unknown"}");
        return report;
    }

    public ScreenChangeReport? Scroll(GridLabel label, int notches, bool horizontal)
    {
        ArgumentNullException.ThrowIfNull(label);

        AutomatonDebug.Log($"ComputerUseSession.Scroll: Entry - label={label.Label}, notches={notches}, horizontal={horizontal}");

        EnsureUsable();
        Aim(ComputerUsePointerAction.Scroll, label.Point, label.Label, waitForArrival: true, scrollNotches: notches, scrollHorizontal: horizontal);
        EnsureUsable();

        _input.MoveTo(label.Point);
        Thread.Sleep(HoverSettle);
        var before = TryCaptureBefore();

        _input.Scroll(label.Point, notches, horizontal);
        var report = TryReportChange(before, label.Point);
        RecordActionOutcome(report);

        AutomatonDebug.Log($"ComputerUseSession.Scroll: Exit - scrolled at {label.Point}, changed={report?.AnythingChanged.ToString() ?? "unknown"}");
        return report;
    }

    public ScreenChangeReport? TypeText(string text)
    {
        AutomatonDebug.Log($"ComputerUseSession.TypeText: Entry - {text?.Length ?? 0} character(s)");

        EnsureUsable();
        var before = TryCaptureBefore();

        // Typing is paced now, so a long string takes seconds. The kill chord has to be
        // able to stop it partway rather than only after the last character.
        _input.TypeText(text ?? string.Empty, shouldStop: () => !IsActive);
        var report = TryReportChange(before, actionPoint: null);
        RecordActionOutcome(report);

        AutomatonDebug.Log($"ComputerUseSession.TypeText: Exit - changed={report?.AnythingChanged.ToString() ?? "unknown"}");
        return report;
    }

    public ScreenChangeReport? PressKeys(string keys)
    {
        AutomatonDebug.Log($"ComputerUseSession.PressKeys: Entry - keys='{keys}'");

        EnsureUsable();
        var before = TryCaptureBefore();

        _input.PressKeys(keys);
        var report = TryReportChange(before, actionPoint: null);
        RecordActionOutcome(report);

        AutomatonDebug.Log($"ComputerUseSession.PressKeys: Exit - pressed '{keys}', changed={report?.AnythingChanged.ToString() ?? "unknown"}");
        return report;
    }

    /// <summary>
    /// Brings the window whose title best matches <paramref name="title"/> to the front.
    ///
    /// Matching, in order: a title equal to it; titles containing it; windows of a
    /// process by that name. Ties go to the window nearest the front. Automaton's own
    /// windows are never candidates — focusing the chat would take the keyboard away
    /// from the app being driven.
    /// </summary>
    /// <exception cref="ArgumentException">
    /// Nothing matched. The message lists the open windows, so the model can retry with
    /// a real title instead of guessing again.
    /// </exception>
    public WindowFocusResult FocusWindow(string title)
    {
        AutomatonDebug.Log($"ComputerUseSession.FocusWindow: Entry - title='{title}'");

        if (string.IsNullOrWhiteSpace(title))
        {
            throw new ArgumentException("A window title, or part of one, is required.", nameof(title));
        }

        EnsureUsable();

        var wanted = title.Trim();
        var candidates = _windows.Capture().Windows.Where(w => !w.IsAutomaton).ToList();

        var matches = candidates.Where(w => w.Title.Equals(wanted, StringComparison.OrdinalIgnoreCase)).ToList();
        if (matches.Count == 0)
        {
            matches = candidates.Where(w => w.Title.Contains(wanted, StringComparison.OrdinalIgnoreCase)).ToList();
        }

        if (matches.Count == 0)
        {
            matches = candidates.Where(w => w.ProcessName.Equals(wanted, StringComparison.OrdinalIgnoreCase)).ToList();
        }

        if (matches.Count == 0)
        {
            var open = candidates.Take(20).Select(w => $"{w.Describe()}{(w.IsMinimized ? " [minimised]" : string.Empty)}");
            throw new ArgumentException(
                $"No window matches '{wanted}'. Open windows, frontmost first: {string.Join("; ", open)}. Call cu_focus_window again with part of one of these titles.",
                nameof(title));
        }

        var target = matches[0];
        var before = TryCaptureBefore();

        var focused = _windows.BringToFront(target.Handle);
        var report = TryReportChange(before, actionPoint: null);
        RecordActionOutcome(report);

        AutomatonDebug.Log($"ComputerUseSession.FocusWindow: Exit - target={target.Describe()} handle=0x{target.Handle:X}, focused={focused}, {matches.Count} match(es)");

        return new WindowFocusResult
        {
            Target = target,
            Focused = focused,
            OtherMatches = matches.Skip(1).ToList(),
            Report = report
        };
    }

    /// <summary>
    /// The "before" picture for a change report. A failed capture costs the report,
    /// never the action: the model asked for a click, and a click it gets.
    /// </summary>
    private ScreenSnapshot? TryCaptureBefore()
    {
        try
        {
            return _changes.CaptureBefore();
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ComputerUseSession.TryCaptureBefore: capture failed, this action will have no change report: {ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }

    private ScreenChangeReport? TryReportChange(ScreenSnapshot? before, ScreenPoint? actionPoint)
    {
        if (before is null)
        {
            return null;
        }

        try
        {
            return _changes.WaitForChange(before, actionPoint, shouldStop: () => !IsActive);
        }
        catch (Exception ex)
        {
            // Most likely a monitor was attached or rescaled mid-wait, which leaves two
            // pictures of different sizes and nothing honest to compare.
            AutomatonDebug.LogWarning($"ComputerUseSession.TryReportChange: could not compare the screen, no change report: {ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }

    /// <summary>
    /// How many times this exact region has been gridded, this one included.
    /// </summary>
    public int TimesVisited(ScreenRect region)
    {
        lock (_gate)
        {
            return _visited.Count(r =>
                r.X == region.X && r.Y == region.Y &&
                r.Width == region.Width && r.Height == region.Height);
        }
    }

    /// <summary>
    /// The distinct regions gridded so far, oldest first, excluding whole-desktop
    /// views — those are re-orientation, not places the model has searched.
    /// </summary>
    public IReadOnlyList<ScreenRect> DistinctVisitedRegions()
    {
        var desktop = DesktopBounds;
        var seen = new List<ScreenRect>();

        lock (_gate)
        {
            foreach (var region in _visited)
            {
                if (region.Width >= desktop.Width && region.Height >= desktop.Height)
                {
                    continue;
                }

                if (seen.Any(r => r.X == region.X && r.Y == region.Y && r.Width == region.Width && r.Height == region.Height))
                {
                    continue;
                }

                seen.Add(region);
            }
        }

        return seen;
    }

    /// <summary>
    /// Parses the region forms the model may name: a named third of the desktop, the
    /// whole desktop, one monitor, or the rect between two labels of the current view.
    /// </summary>
    public ScreenRect ParseRegion(string? region)
    {
        AutomatonDebug.Log($"ComputerUseSession.ParseRegion: Entry - region='{region ?? "null"}'");

        if (string.IsNullOrWhiteSpace(region) || region.Equals("screen", StringComparison.OrdinalIgnoreCase) || region.Equals("desktop", StringComparison.OrdinalIgnoreCase))
        {
            AutomatonDebug.Log($"ComputerUseSession.ParseRegion: Exit - whole desktop {DesktopBounds}");
            return DesktopBounds;
        }

        var trimmed = region.Trim();

        if (TryParseNamedThird(trimmed, out var named))
        {
            AutomatonDebug.Log($"ComputerUseSession.ParseRegion: Exit - named region '{trimmed}' = {named}");
            return named;
        }

        if (trimmed.StartsWith("monitor:", StringComparison.OrdinalIgnoreCase))
        {
            var indexText = trimmed["monitor:".Length..];

            if (!int.TryParse(indexText, out var index))
            {
                throw new ArgumentException($"Could not read a monitor number from '{region}'. Use monitor:1, monitor:2 and so on.", nameof(region));
            }

            var monitor = Monitors.FirstOrDefault(m => m.Index == index);
            if (monitor is null)
            {
                throw new ArgumentException($"There is no monitor {index}. Available: {string.Join(", ", Monitors.Select(m => m.Index))}.", nameof(region));
            }

            AutomatonDebug.Log($"ComputerUseSession.ParseRegion: Exit - monitor {index} {monitor.Bounds}");
            return monitor.Bounds;
        }

        if (IsChangedAreaRegion(trimmed))
        {
            var changedArea = ResolveChangedArea(trimmed);
            AutomatonDebug.Log($"ComputerUseSession.ParseRegion: Exit - changed area '{trimmed}' = {changedArea}");
            return changedArea;
        }

        if (IsLabelPairRegion(trimmed))
        {
            var parts = trimmed.Split(':', 2);
            var topLeft = ResolveLabel(parts[0]);
            var bottomRight = ResolveLabel(parts[1]);

            var x = Math.Min(topLeft.Point.X, bottomRight.Point.X);
            var y = Math.Min(topLeft.Point.Y, bottomRight.Point.Y);
            var width = Math.Abs(bottomRight.Point.X - topLeft.Point.X);
            var height = Math.Abs(bottomRight.Point.Y - topLeft.Point.Y);

            if (width < 8 || height < 8)
            {
                throw new ArgumentException($"The region between {parts[0]} and {parts[1]} is only {width}x{height} pixels. Pick labels further apart.", nameof(region));
            }

            AutomatonDebug.Log($"ComputerUseSession.ParseRegion: Exit - label rect {width}x{height}+{x}+{y}");
            return new ScreenRect(x, y, width, height);
        }

        throw new ArgumentException(
            $"Could not read region '{region}'. Use a named area ({string.Join(", ", NamedRegions)}), 'screen', 'monitor:N', 'changed:N' for an area your last action changed, or two labels from the current view like 'B2:C4'.",
            nameof(region));
    }

    /// <summary>
    /// Space added around a changed area before gridding it. A changed row is often
    /// exactly as tall as the change, and a grid clipped to it would put its outer
    /// markers on the row's very edges.
    /// </summary>
    private const int ChangedAreaMargin = 16;

    /// <summary>"changed" or "changed:N" — an area from the last action's change report.</summary>
    public static bool IsChangedAreaRegion(string? region)
    {
        if (string.IsNullOrWhiteSpace(region))
        {
            return false;
        }

        var trimmed = region.Trim();
        return trimmed.Equals("changed", StringComparison.OrdinalIgnoreCase) ||
               trimmed.StartsWith("changed:", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// The screen rect of changed area N from the most recent action.
    ///
    /// Exists because the model's commonest next step after "that worked" is to act on
    /// the thing that changed — the option row that just got selected, the panel that
    /// just opened. It used to get there by screenshotting, gridding a named area, then
    /// guessing two labels to bound the thing; the report already knew exactly where
    /// it was.
    /// </summary>
    private ScreenRect ResolveChangedArea(string region)
    {
        var report = LastChangeReport
            ?? throw new ArgumentException("No action has reported what changed yet, so there is no changed area to look at. Click, type or scroll first.", nameof(region));

        var number = 1;
        var colon = region.IndexOf(':');
        if (colon >= 0 && !int.TryParse(region[(colon + 1)..], out number))
        {
            throw new ArgumentException($"Could not read an area number from '{region}'. Use changed:1, changed:2 and so on, as numbered in your last action's result.", nameof(region));
        }

        if (report.Areas.Count == 0)
        {
            throw new ArgumentException("Your last action changed no area of the screen, so there is no changed area to look at.", nameof(region));
        }

        if (number < 1 || number > report.Areas.Count)
        {
            throw new ArgumentException($"Your last action changed {report.Areas.Count} area(s), numbered 1 to {report.Areas.Count}; there is no area {number}.", nameof(region));
        }

        var bounds = report.Areas[number - 1].Bounds;
        var padded = new ScreenRect(
            bounds.X - ChangedAreaMargin,
            bounds.Y - ChangedAreaMargin,
            bounds.Width + 2 * ChangedAreaMargin,
            bounds.Height + 2 * ChangedAreaMargin);

        return padded.ClampInside(DesktopBounds);
    }

    /// <summary>The named areas <see cref="ParseRegion"/> understands.</summary>
    public static readonly string[] NamedRegions =
    {
        "top-left", "top", "top-right",
        "left", "centre", "right",
        "bottom-left", "bottom", "bottom-right",
        "top-strip", "bottom-strip", "left-strip", "right-strip"
    };

    /// <summary>
    /// Named areas of the desktop: the nine thirds, plus four edge strips.
    ///
    /// These exist because of a specific failure. The model could see roughly where
    /// its target was — "to the right of the weather widget" — but the only way to
    /// look closer was <c>around</c>, which drops it into a single cell. It stepped
    /// one cell right, found nothing, and gave up rather than continuing along the
    /// taskbar, because a cell at that depth is 120px and the target was 500px away.
    ///
    /// A named area lets it say "show me the bottom strip" and get the whole taskbar
    /// at a readable scale in one call, instead of walking it 120px at a time.
    /// </summary>
    private bool TryParseNamedThird(string name, out ScreenRect region)
    {
        region = default;

        var desktop = DesktopBounds;
        var thirdWidth = Math.Max(1, desktop.Width / 3);
        var thirdHeight = Math.Max(1, desktop.Height / 3);

        // A strip is a fifth of the screen deep rather than a third: a taskbar or a
        // title bar is a thin band, and including a third of the screen with it
        // shrinks the band back down to where it cannot be read.
        var stripHeight = Math.Max(1, desktop.Height / 5);
        var stripWidth = Math.Max(1, desktop.Width / 5);

        var left = desktop.X;
        var middleX = desktop.X + thirdWidth;
        var rightX = desktop.X + (2 * thirdWidth);

        var top = desktop.Y;
        var middleY = desktop.Y + thirdHeight;
        var bottomY = desktop.Y + (2 * thirdHeight);

        region = name.ToLowerInvariant() switch
        {
            // The aliases are the vocabulary of DescribePosition — "the middle-left of
            // the desktop" — which the model naturally handed back as a region name and
            // was refused. Whatever a result calls a place, a request may call it too.
            "top-left" => new ScreenRect(left, top, thirdWidth, thirdHeight),
            "top" or "top-centre" or "top-center" or "top-middle" => new ScreenRect(middleX, top, thirdWidth, thirdHeight),
            "top-right" => new ScreenRect(rightX, top, thirdWidth, thirdHeight),
            "left" or "middle-left" or "centre-left" or "center-left" => new ScreenRect(left, middleY, thirdWidth, thirdHeight),
            "centre" or "center" or "middle" or "middle-centre" or "middle-center" or "centre-centre" => new ScreenRect(middleX, middleY, thirdWidth, thirdHeight),
            "right" or "middle-right" or "centre-right" or "center-right" => new ScreenRect(rightX, middleY, thirdWidth, thirdHeight),
            "bottom-left" => new ScreenRect(left, bottomY, thirdWidth, thirdHeight),
            "bottom" or "bottom-centre" or "bottom-center" or "bottom-middle" => new ScreenRect(middleX, bottomY, thirdWidth, thirdHeight),
            "bottom-right" => new ScreenRect(rightX, bottomY, thirdWidth, thirdHeight),
            "top-strip" => new ScreenRect(desktop.X, desktop.Y, desktop.Width, stripHeight),
            "bottom-strip" => new ScreenRect(desktop.X, desktop.Bottom - stripHeight, desktop.Width, stripHeight),
            "left-strip" => new ScreenRect(desktop.X, desktop.Y, stripWidth, desktop.Height),
            "right-strip" => new ScreenRect(desktop.Right - stripWidth, desktop.Y, stripWidth, desktop.Height),
            _ => default
        };

        return !region.IsEmpty;
    }

    /// <summary>
    /// True when a region string names two labels of the current view rather than a
    /// fixed place on the desktop. Those resolve against whatever view is current,
    /// which is not the whole screen once the model has zoomed.
    /// </summary>
    public static bool IsLabelPairRegion(string? region)
    {
        if (string.IsNullOrWhiteSpace(region))
        {
            return false;
        }

        var trimmed = region.Trim();
        return trimmed.Contains(':', StringComparison.Ordinal) &&
               !trimmed.StartsWith("monitor:", StringComparison.OrdinalIgnoreCase) &&
               !IsChangedAreaRegion(trimmed);
    }

    /// <summary>
    /// Sends the ghost cursor to a point and, for actions that inject real input, waits
    /// for it to get there first — so the operator sees the pointer arrive and then the
    /// click happen, rather than the click happening while the ghost is still travelling.
    ///
    /// The wait only happens when something is drawing the ghost. Without a subscriber
    /// the descent runs at exactly its old speed.
    /// </summary>
    private (PointerPath Path, TimeSpan Travel) Aim(
        ComputerUsePointerAction action,
        ScreenPoint target,
        string label,
        bool waitForArrival,
        MouseButton button = MouseButton.Left,
        int clickCount = 1,
        int scrollNotches = 0,
        bool scrollHorizontal = false)
    {
        ScreenPoint from;
        int seed;

        lock (_gate)
        {
            from = _ghostAt;
            _ghostAt = target;
            seed = HashCode.Combine(Environment.TickCount, ++_aimSequence);
        }

        var travel = action == ComputerUsePointerAction.Appear
            ? TimeSpan.Zero
            : HumanPointerMotion.TravelTime(from, target);

        var args = new ComputerUsePointerEventArgs
        {
            Action = action,
            From = from,
            Target = target,
            Travel = travel,
            Seed = seed,
            Label = label ?? string.Empty,
            Button = button,
            ClickCount = clickCount,
            ScrollNotches = scrollNotches,
            ScrollHorizontal = scrollHorizontal
        };

        ComputerUseViewFeed.PublishPointer(args);

        if (waitForArrival && travel > TimeSpan.Zero && ComputerUseViewFeed.HasPointerSubscribers)
        {
            Thread.Sleep(travel + GhostArrivalSlack);
        }

        return (args.PlanPath(), travel);
    }

    /// <summary>The change report from the most recent action, for <c>changed:N</c> regions.</summary>
    public ScreenChangeReport? LastChangeReport
    {
        get
        {
            lock (_gate)
            {
                return _lastChangeReport;
            }
        }
    }

    private ScreenChangeReport? _lastChangeReport;

    /// <summary>
    /// Applies what an action turned out to do.
    ///
    /// Only an action that changed something makes the view stale and restarts the
    /// repeat count. Every action used to, so after a click that missed — which the
    /// report itself said changed nothing — the next click on the same, still accurate
    /// view came with "the view was already stale", and the model could not tell
    /// whether its first click had done something subtle. A report that could not be
    /// made at all counts as a change: without evidence, assume the screen moved.
    /// </summary>
    public void RecordActionOutcome(ScreenChangeReport? report)
    {
        lock (_gate)
        {
            _lastChangeReport = report;
            _lastDroppedTextLabels = Array.Empty<string>();
            _lastNewlyStaleLabels = Array.Empty<string>();
        }

        if (report is null)
        {
            MarkScreenChanged();
            return;
        }

        if (!report.AnythingChanged)
        {
            Touch();
            return;
        }

        Touch();

        // Something changed, so looking again is checking, not circling.
        lock (_gate)
        {
            _visited.Clear();
        }

        // Only the labels the change actually reached. A label over pixels that did not
        // change is over the same thing it was, whatever happened elsewhere. A window
        // change with no area change (focus moved) alters no pixels and stales nothing.
        var areas = report.Areas;
        if (areas.Count == 0)
        {
            return;
        }

        var view = CurrentView;
        var newlyStale = new List<string>();
        var dropped = new List<string>();

        lock (_gate)
        {
            if (view is not null)
            {
                foreach (var label in view.Labels.Values)
                {
                    if (!_staleLabels.Contains(label.Label) && areas.Any(a => ScreenChangeReport.IsNear(a.Bounds, label.Point)))
                    {
                        _staleLabels.Add(label.Label);
                        newlyStale.Add(label.Label);
                    }
                }
            }

            var kept = new List<TextTarget>();
            foreach (var target in _textTargets)
            {
                if (areas.Any(a => Overlaps(Inflate(a.Bounds, ScreenChangeReport.NearActionMargin), target.Bounds)))
                {
                    dropped.Add(target.Label);
                }
                else
                {
                    kept.Add(target);
                }
            }

            _textTargets = kept;
            _lastNewlyStaleLabels = newlyStale;
            _lastDroppedTextLabels = dropped;
        }

        AutomatonDebug.Log($"ComputerUseSession.RecordActionOutcome: {areas.Count} changed area(s); grid labels newly stale: {(newlyStale.Count == 0 ? "none" : string.Join(",", newlyStale))}; text labels dropped: {(dropped.Count == 0 ? "none" : string.Join(",", dropped))}");
    }

    private static ScreenRect Inflate(ScreenRect rect, int by) =>
        new(rect.X - by, rect.Y - by, rect.Width + 2 * by, rect.Height + 2 * by);

    private static bool Overlaps(ScreenRect a, ScreenRect b) =>
        a.X < b.Right && b.X < a.Right && a.Y < b.Bottom && b.Y < a.Bottom;

    /// <summary>
    /// The middle of the foreground window, where a scroll with no label goes — the app
    /// being driven, not the chat. Null when the foreground is Automaton itself, has no
    /// title (a menu, the desktop), or is off screen.
    /// </summary>
    public (ScreenPoint Point, string Description)? ForegroundWindowCentre()
    {
        var snapshot = _windows.Capture();
        var window = snapshot.Foreground;

        if (window is null || window.IsAutomaton || window.IsMinimized)
        {
            return null;
        }

        // Maximised windows overhang the screen edges slightly, so clip to the desktop.
        var visible = window.Bounds.ClampInside(DesktopBounds);
        if (visible.IsEmpty)
        {
            return null;
        }

        return (new ScreenPoint(visible.X + visible.Width / 2, visible.Y + visible.Height / 2), $"the middle of {window.Describe()}");
    }

    /// <summary>
    /// Records that something was clicked or typed, so the next action is told the
    /// view is stale before it resolves a label against it.
    /// </summary>
    public void MarkScreenChanged()
    {
        lock (_gate)
        {
            _allLabelsStale = true;
        }
        Touch();

        lock (_gate)
        {
            _visited.Clear();

            // Unlike grid labels, which are kept and flagged stale, text matches go. A
            // grid label names a fixed point that may now cover something else; a text
            // label names a piece of text, and after a change the text may have moved or
            // gone — there is no honest point left to resolve it to.
            _textTargets = Array.Empty<TextTarget>();
        }
    }

    /// <summary>
    /// Throws with an explanation whenever control is not usable — never taken, ended,
    /// revoked by the kill chord, or lapsed through inactivity.
    /// </summary>
    public void EnsureUsable()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (!IsActive)
        {
            var suffix = string.IsNullOrEmpty(EndedReason) ? string.Empty : $" ({EndedReason})";
            throw new InvalidOperationException($"Computer use is not active{suffix}. Call cu_begin first.");
        }

        var idle = DateTimeOffset.UtcNow - LastActivityUtc;
        if (idle <= IdleTimeout)
        {
            return;
        }

        End($"lapsed after {idle.TotalMinutes:0.#} minutes of inactivity");
        throw new InvalidOperationException(
            $"Computer use lapsed after {idle.TotalMinutes:0.#} minutes without a tool call. Call cu_begin again if the task is still running.");
    }

    private void Touch()
    {
        LastActivityUtc = DateTimeOffset.UtcNow;
    }

    /// <summary>
    /// Hands a rendered view to anything watching the descent.
    ///
    /// Fire-and-forget by design: the descent must not slow down, fail, or behave
    /// differently because a window is or is not open.
    /// </summary>
    public event Action<GridView, string>? ViewShown;
    public event Action<ScreenPoint>? ClickPerformed;

    private void PublishView(GridView view, string kind)
    {
        if (view is null)
        {
            return;
        }

        ViewShown?.Invoke(view, kind);
        ComputerUseViewFeed.PublishView(new ComputerUseViewEventArgs
        {
            Kind = kind,
            ImageBytes = view.ImageBytes,
            MediaType = view.MediaType,
            Region = view.Region,
            Rows = view.Rows,
            Columns = view.Columns,
            Magnification = view.Magnification,
            ScreenPitchX = view.ScreenPitchX,
            ScreenPitchY = view.ScreenPitchY,
            ZoomDepth = ZoomDepth,
            Where = GridOverlayRenderer.DescribePosition(view.Region, DesktopBounds)
        });
    }

    private void StartWatchdog()
    {
        StopWatchdog();

        var cts = new CancellationTokenSource();
        _watchdog = cts;

        _ = Task.Run(async () =>
        {
            AutomatonDebug.Log($"ComputerUseSession.Watchdog: Entry - polling for {KillChordDescription} every {WatchdogInterval.TotalMilliseconds:0}ms");

            try
            {
                while (!cts.Token.IsCancellationRequested)
                {
                    if (!_guestOnly && DesktopInputService.IsChordDown(KillChord))
                    {
                        AutomatonDebug.LogWarning($"ComputerUseSession.Watchdog: {KillChordDescription} pressed, revoking desktop control");
                        End($"revoked by the operator with {KillChordDescription}");
                        EmergencyStop?.Invoke();
                        return;
                    }

                    await Task.Delay(WatchdogInterval, cts.Token).ConfigureAwait(false);
                }
            }
            catch (OperationCanceledException)
            {
                // Normal shutdown when control is released.
            }
            catch (Exception ex)
            {
                AutomatonDebug.LogError($"ComputerUseSession.Watchdog: Failed, the kill chord is no longer being watched: {ex.GetType().Name}: {ex.Message}");
            }

            AutomatonDebug.Log("ComputerUseSession.Watchdog: Exit");
        }, cts.Token);
    }

    private void StopWatchdog()
    {
        var watchdog = Interlocked.Exchange(ref _watchdog, null);
        if (watchdog is null)
        {
            return;
        }

        watchdog.Cancel();
        watchdog.Dispose();
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        StopWatchdog();

        lock (_gate)
        {
            _isActive = false;
            _zoomStack.Clear();
        }
    }
}
