using System.Runtime.Versioning;
using Automaton.Core.Abstracts;
using Automaton.Core.Interfaces;
using Automaton.Core.Services;
using Automaton.Core.Services.ComputerUse;
using Automaton.Tools.Models;

namespace Automaton.Tools.Tools;

/// <summary>
/// Shared plumbing for the computer-use tools: turning a rendered view into a tool
/// result, and the bits of vocabulary the model is allowed to use.
/// </summary>
[SupportedOSPlatform("windows")]
internal static class ComputerUseToolSupport
{
    /// <summary>
    /// What each region form means, spelled out.
    ///
    /// The names alone were ambiguous: "left" reads as "the left side of the screen",
    /// but it is only the middle-left block of nine, while "left-strip" is the whole
    /// left edge. A model used both believing them to be the same thing.
    /// </summary>
    public const string RegionDescription =
        "What to look at when not zooming. Forms: " +
        "(1) \"screen\" — the whole desktop; \"monitor:2\" — one display. " +
        "(2) The nine blocks — the desktop cut 3x3, each block a third wide and a third tall: \"top-left\", \"top\", \"top-right\", \"left\", \"centre\", \"right\", \"bottom-left\", \"bottom\", \"bottom-right\". " +
        "These are single blocks, not whole sides: \"left\" is the middle-left block only, \"top\" is the top-middle block only. " +
        "(3) The four edge strips — a whole edge, a fifth of the screen deep: \"top-strip\" and \"bottom-strip\" run the full width (bottom-strip holds the taskbar), \"left-strip\" and \"right-strip\" run the full height. " +
        "(4) Two labels of the current view, \"B2:C4\" — the rectangle between those two markers. Label pairs always refer to the view you have now, not the whole screen. " +
        "Two NEIGHBOURING labels, e.g. \"B2:C3\", give exactly the one cell between them: that is how to zoom onto a target sitting between markers. " +
        "(5) \"changed:1\", \"changed:2\" … — an area your last click, key press, scroll or typing changed, numbered as in that action's result. The fastest way to act on what just appeared or got selected: no screenshot, no guessing labels. " +
        "Use a named area when you know roughly where something is but cannot read it at full-screen scale — it is far faster than stepping there one cell at a time.";

    /// <summary>
    /// Packages a rendered view. The image goes on as an attachment, which the agent
    /// loop turns into a synthetic user message — a tool result cannot carry an image
    /// itself. See <see cref="ToolImageMessages"/>.
    /// </summary>
    public static ToolResult BuildViewResult(GridView view, ComputerUseSession session, string title, string? preamble)
    {
        ArgumentNullException.ThrowIfNull(view);
        ArgumentNullException.ThrowIfNull(session);

        var lines = new List<string>();

        if (!string.IsNullOrWhiteSpace(preamble))
        {
            lines.Add(preamble);
        }

        lines.Add(GridOverlayRenderer.BuildLegend(view, session.DesktopBounds));

        var repetition = DescribeRepetition(view, session);
        if (!string.IsNullOrEmpty(repetition))
        {
            lines.Add(repetition);
        }

        lines.Add(DescribeState(session));

        return new ToolResult
        {
            Title = title,
            Output = string.Join(Environment.NewLine, lines),
            Metadata = new Dictionary<string, object>
            {
                ["region"] = view.Region.ToString(),
                ["rows"] = view.Rows,
                ["columns"] = view.Columns,
                ["labels"] = view.Labels.Count,
                ["magnification"] = Math.Round(view.Magnification, 3),
                ["screenPitchX"] = Math.Round(view.ScreenPitchX, 1),
                ["screenPitchY"] = Math.Round(view.ScreenPitchY, 1),
                ["zoomDepth"] = session.ZoomDepth
            },
            Attachments =
            {
                new ToolAttachment
                {
                    Filename = $"cu-view-{DateTime.UtcNow:HHmmss}-{view.Region.Width}x{view.Region.Height}.jpg",
                    Content = Convert.ToBase64String(view.ImageBytes),
                    ContentType = view.MediaType
                }
            }
        };
    }

    /// <summary>
    /// Tells the model when it is going round in circles, and lists where it has
    /// already looked.
    ///
    /// Old screenshots get pruned out of the conversation, and with them goes the
    /// model's memory of where it has been. Left to itself it produced a perfect
    /// limit cycle — zoom to a spot, find nothing, zoom out, zoom to the same spot,
    /// with word-for-word identical reasoning, eleven round trips running. This is
    /// the memory the pruning takes away, restored as a few lines of text.
    /// </summary>
    public static string DescribeRepetition(GridView view, ComputerUseSession session)
    {
        ArgumentNullException.ThrowIfNull(view);
        ArgumentNullException.ThrowIfNull(session);

        var times = session.TimesVisited(view.Region);
        if (times < 2)
        {
            return string.Empty;
        }

        var desktop = session.DesktopBounds;
        var elsewhere = session.DistinctVisitedRegions()
            .Where(r => r.X != view.Region.X || r.Y != view.Region.Y || r.Width != view.Region.Width || r.Height != view.Region.Height)
            .TakeLast(5)
            .Select(r => GridOverlayRenderer.DescribePosition(r, desktop))
            .ToList();

        var alreadyTried = elsewhere.Count == 0
            ? string.Empty
            : $" You have also already examined: {string.Join("; ", elsewhere)}.";

        return
            $"REPEAT: you have now looked at this exact region {times} times and have not found what you are looking for here.{alreadyTried} " +
            "Stop returning to it. Your assumption about where the target is, is wrong — go back to a whole-screen or named-area view " +
            "(region=\"bottom-strip\", \"bottom-left\", \"centre\" and so on) and search a genuinely different part of the screen.";
    }

    /// <summary>
    /// One line telling the model where it is: how deep the zoom is, how precise the
    /// current view is, and whether the view can still be trusted.
    /// </summary>
    public static string DescribeState(ComputerUseSession session)
    {
        ArgumentNullException.ThrowIfNull(session);

        var view = session.CurrentView;
        if (view is null)
        {
            return "No view yet. Call cu_grid to see the screen.";
        }

        var precision = $"zoom depth {session.ZoomDepth}, labels {view.ScreenPitchX:0} x {view.ScreenPitchY:0} screen pixels apart";

        if (session.ViewIsStale)
        {
            return $"{precision}. {session.DescribeLabelValidity()}";
        }

        // Either axis, not both. A wide region like the taskbar strip reaches a fine
        // vertical pitch long before a fine horizontal one, and requiring both to be
        // fine meant the advice arrived a zoom step too late — by which point the
        // model had already dived inside the icon it was aiming at.
        var finest = Math.Min(view.ScreenPitchX, view.ScreenPitchY);

        // A typical button or icon is around 40 screen pixels. Once the markers are
        // closer together than that, the nearest one is already somewhere inside the
        // target, and a click anywhere inside a button works exactly as well as a
        // click at its centre. Left without this, the model kept zooming in search of
        // a marker dead-centre on the icon, went straight past the useful depth, and
        // ended up "inside the logo's top-left pane" — its own words — with the thing
        // it wanted now filling the entire view.
        if (finest < 60)
        {
            return
                $"{precision}. The markers are now closer together than a typical button or icon (~40px), " +
                "so if you can see your target at all, the nearest marker is already inside it — report or click that label now rather than zooming again. " +
                "Not sure whether the marker is on the target? cu_peek that label: a tenth of the cost of another grid, and it shows exactly where the click lands. " +
                "You do not need a marker at the exact centre; anywhere inside the target works. " +
                "If the target sits BETWEEN two markers with none actually on it, do not zoom around either one — that recentres on a corner and leaves the target off to the side again. " +
                "Call cu_grid with region=\"<labelA>:<labelB>\" instead, which re-grids just the rectangle spanning those two markers and lays fresh ones across the target. " +
                "If the target is NOT visible here, you are in the wrong part of the screen: zoom out and re-aim rather than zooming further in.";
        }

        return $"{precision}. If no marker is clearly on top of the target, call cu_grid with around=<nearest label> to zoom in; otherwise cu_click it.";
    }

    /// <summary>
    /// Result for an action that changed the screen. Deliberately does not return a
    /// fresh view: the model asks for one when it needs it, and a screenshot after
    /// every keystroke would exhaust the context window. What it does return is a
    /// text account of what changed, which is often enough to know whether the action
    /// worked without spending a view on it.
    /// </summary>
    /// <param name="action">How the change report names the action, e.g. "the click".</param>
    /// <param name="actionPoint">Where the pointer acted; null for keyboard input.</param>
    public static ToolResult BuildActionResult(
        string title,
        string what,
        ComputerUseSession session,
        ScreenChangeReport? report,
        string action,
        ScreenPoint? actionPoint)
    {
        ArgumentNullException.ThrowIfNull(session);

        var lines = new List<string> { what };

        if (report is null)
        {
            lines.Add("The screen could not be compared before and after, so there is no account of what changed. Call cu_grid to see the result before acting again.");
        }
        else
        {
            lines.Add(report.Describe(action, actionPoint, session.CurrentView?.Region));

            if (report.AnythingChanged)
            {
                var validity = session.DescribeLabelValidity();
                lines.Add(string.IsNullOrEmpty(validity) ? "Call cu_grid to see the result before acting again." : validity);
            }
        }

        var metadata = new Dictionary<string, object>
        {
            ["zoomDepth"] = session.ZoomDepth
        };

        if (report is not null)
        {
            metadata["screenChanged"] = report.AnythingChanged;
            metadata["changedAreas"] = report.Areas.Count;
            metadata["changedFraction"] = Math.Round(report.ChangedFraction, 4);
        }

        return new ToolResult
        {
            Title = title,
            Output = string.Join(Environment.NewLine, lines),
            Metadata = metadata
        };
    }


    /// <summary>Lines are quoted for context; a paragraph read as one line is cut to this.</summary>
    private const int MaxQuotedLine = 100;

    /// <summary>"the whole screen", or where a narrower search looked.</summary>
    public static string DescribeSearchArea(ScreenRect region, ScreenRect desktop) =>
        region.Width >= desktop.Width && region.Height >= desktop.Height
            ? "the whole screen"
            : GridOverlayRenderer.DescribePosition(region, desktop);

    /// <summary>
    /// The lines that describe a text search to the model — shared by cu_find_text and
    /// cu_click text=, so the two never describe the same matches differently.
    /// </summary>
    /// <param name="suggestClick">Whether to end with how to click a match; false when the caller was already a click.</param>
    public static List<string> DescribeSearch(ScreenTextSearch search, string where, ScreenRect desktop, bool suggestClick)
    {
        ArgumentNullException.ThrowIfNull(search);

        var lines = new List<string>();

        if (search.Targets.Count == 0)
        {
            lines.Add(DescribeNoMatch(search, where));
        }
        else
        {
            lines.Add(search.Approximate
                ? $"No exact match for \"{search.Query}\" in {where}. Closest readings — OCR may have misread the text, or it may say something slightly different:"
                : $"Found {search.TotalMatches} match(es) for \"{search.Query}\" in {where}:");

            foreach (var target in search.Targets)
            {
                lines.Add(DescribeTarget(target, desktop));
            }

            if (search.TotalMatches > search.Targets.Count)
            {
                lines.Add($"…and {search.TotalMatches - search.Targets.Count} more, not labelled. Pass region to narrow the search.");
            }

            if (suggestClick)
            {
                var first = search.Targets[0].Label;
                lines.Add($"Click one with cu_click label=\"{first}\" — it lands on the middle of that text. cu_move, cu_drag, cu_scroll and cu_peek take these labels too, and cu_grid around=\"{first}\" zooms onto it. They stay valid until the screen changes under them, and matches from earlier searches stay usable too.");
            }
        }

        if (search.IgnoredInAutomaton > 0)
        {
            lines.Add($"({search.IgnoredInAutomaton} match(es) inside Automaton's own windows were left out.)");
        }

        return lines;
    }

    public static string DescribeTarget(TextTarget target, ScreenRect desktop)
    {
        ArgumentNullException.ThrowIfNull(target);

        var similarity = target.Similarity < 1 ? $" ({target.Similarity:P0} similar)" : string.Empty;
        var window = target.Window is null
            ? string.Empty
            : $"in {target.Window.Describe()}{(target.InForegroundWindow ? " (the foreground window)" : string.Empty)}, ";

        return $"{target.Label} \"{target.Text}\"{similarity} — {window}{GridOverlayRenderer.DescribePosition(target.Bounds, desktop)}; line: \"{Quote(target.LineText)}\"";
    }

    private static string DescribeNoMatch(ScreenTextSearch search, string where)
    {
        var read = search.Retried ? "read twice, 300 ms apart" : "read";
        var message = $"No text matching \"{search.Query}\" was found in {where} ({search.Lines.Count} lines of text {read}).";

        var related = search.LinesSharingAWord(8);
        if (related.Count > 0)
        {
            var quoted = related.Select(line =>
            {
                var box = line.Words[0].Bounds;
                return $"\"{Quote(line.Text)}\" at ({box.X:0},{box.Y:0})";
            });

            message += $" Lines that share a word with it: {string.Join("; ", quoted)}. If one of those is what you meant, search for its exact wording.";
        }

        return message + " Text drawn inside an image or icon cannot be read — use cu_grid to look for those.";
    }

    private static string Quote(string text) =>
        text.Length <= MaxQuotedLine ? text : text[..MaxQuotedLine] + "…";
    public static MouseButton ParseButton(string? button)
    {
        if (string.IsNullOrWhiteSpace(button))
        {
            return MouseButton.Left;
        }

        return button.Trim().ToLowerInvariant() switch
        {
            "left" => MouseButton.Left,
            "right" => MouseButton.Right,
            "middle" => MouseButton.Middle,
            _ => throw new ArgumentException($"Unknown mouse button '{button}'. Use left, right or middle.", nameof(button))
        };
    }
}

/// <summary>
/// Takes control of the desktop.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseBeginTool : BaseTool<ComputerUseBeginParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseBeginTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["reason"] = Property("string", "What you intend to do with the desktop, in one line. Logged so the operator can see why you took control.")
            },
            new List<string> { "reason" });
    }

    public override string Id => "cu_begin";

    public override string Description =>
        "Start a desktop control session for the mouse and keyboard. " +
        "You never work out pixel coordinates. For anything with a visible text label — a button, link, menu item, tab or list row — cu_click text=\"that text\" finds and clicks it in one call. " +
        "For everything else, call cu_grid to see the screen with a labelled green grid drawn over it, " +
        "name the label nearest your target to zoom in, and repeat until a marker sits on the target, then cu_click that label.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseBeginParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseBeginTool.ExecuteInternalAsync: Entry - reason='{parameters?.Reason ?? "null"}'");

        ArgumentNullException.ThrowIfNull(parameters);
        ArgumentNullException.ThrowIfNull(context);

        if (string.IsNullOrWhiteSpace(parameters.Reason))
        {
            throw new ArgumentException("reason is required — it is logged so the operator can see why control was taken.");
        }

        if (parameters.Machine != null &&
            !parameters.Machine.Trim().Equals("desktop", StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException("The desktop control tool only accepts machine=desktop.");
        }

        var monitors = _session.Begin(parameters.Reason);
        var lines = new List<string> { $"Desktop control granted. {monitors.Count} monitor(s):" };

        foreach (var monitor in monitors)
        {
            lines.Add($"  monitor:{monitor.Index} {monitor.Bounds.Width}x{monitor.Bounds.Height} at ({monitor.Bounds.X},{monitor.Bounds.Y}){(monitor.IsPrimary ? " (primary)" : string.Empty)}");
        }

        lines.Add($"The operator can revoke control at any time with {ComputerUseSession.KillChordDescription}, and it lapses after {_session.IdleTimeout.TotalMinutes:0.#} minutes of inactivity.");
        lines.Add("Call cu_grid to see the screen.");

        AutomatonDebug.Log($"ComputerUseBeginTool.ExecuteInternalAsync: Exit - desktop control granted over {monitors.Count} monitor(s)");

        return Task.FromResult(new ToolResult
        {
            Title = "Desktop control granted",
            Output = string.Join(Environment.NewLine, lines),
            Metadata = new Dictionary<string, object>
            {
                ["monitors"] = monitors.Count,
                ["machine"] = "desktop"
            }
        });
    }
}

/// <summary>
/// Releases desktop control.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseEndTool : BaseTool<ComputerUseEndParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseEndTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        Parameters = CreateSchema(new Dictionary<string, JsonSchemaProperty>());
    }

    public override string Id => "cu_end";

    public override string Description => "Release desktop control. Call this as soon as the task is done so the operator has their machine back.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseEndParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseEndTool.ExecuteInternalAsync: Entry - wasActive={_session.IsActive}");

        var wasActive = _session.IsActive;
        _session.End("released by the model");

        AutomatonDebug.Log("ComputerUseEndTool.ExecuteInternalAsync: Exit - control released");

        return Task.FromResult(new ToolResult
        {
            Title = "Desktop control released",
            Output = wasActive ? "Desktop control released." : "Desktop control was not active.",
            Metadata = new Dictionary<string, object>
            {
                ["wasActive"] = wasActive
            }
        });
    }
}

/// <summary>
/// Lists the attached displays.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseScreensTool : BaseTool<ComputerUseScreensParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseScreensTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        Parameters = CreateSchema(new Dictionary<string, JsonSchemaProperty>());
    }

    public override string Id => "cu_screens";

    public override string Description => "List the monitors, their sizes and which is primary, so you can grid one of them by name.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseScreensParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log("ComputerUseScreensTool.ExecuteInternalAsync: Entry");

        _session.EnsureUsable();

        var lines = new List<string> { $"Virtual desktop: {_session.DesktopBounds}" };

        foreach (var monitor in _session.Monitors)
        {
            lines.Add($"monitor:{monitor.Index} {monitor.DeviceName} bounds={monitor.Bounds} workArea={monitor.WorkArea}{(monitor.IsPrimary ? " primary" : string.Empty)}");
        }

        AutomatonDebug.Log($"ComputerUseScreensTool.ExecuteInternalAsync: Exit - {_session.Monitors.Count} monitor(s)");

        return Task.FromResult(new ToolResult
        {
            Title = "Monitors",
            Output = string.Join(Environment.NewLine, lines),
            Metadata = new Dictionary<string, object>
            {
                ["monitors"] = _session.Monitors.Count
            }
        });
    }
}

/// <summary>
/// The workhorse: capture a region, draw the labelled grid, hand back the image.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseGridTool : BaseTool<ComputerUseGridParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseGridTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["around"] = Property("string", "A label from the current view to zoom into, e.g. \"C4\". Crops one cell centred ON THAT MARKER and re-labels it, magnifying by roughly the column count. Use it when a marker is on or right next to your target. If the target sits between markers instead, use region with two neighbouring labels (see region) — around would centre on the marker, not the target."),
                ["region"] = Property("string", ComputerUseToolSupport.RegionDescription),
                ["rows"] = Property("integer", "Cell rows, 1-12. Default 3. Produces rows+1 labelled rows. With around, an odd count is rounded up to even so a marker sits exactly on the point you zoomed around."),
                ["columns"] = Property("integer", "Cell columns, 1-12. Default 4. Produces columns+1 labelled columns, and sets the magnification per zoom step. With around, an odd count is rounded up to even.")
            });
    }

    public override string Id => "cu_grid";

    public override string Description =>
        "Screenshot the desktop with a green grid drawn over it, labelled at every intersection (rows are letters, columns are numbers, so \"C4\"). " +
        "Each intersection is a point you can click by name. Use around=<label> to zoom into the marker nearest your target and get a fresh, magnified grid. " +
        "Repeat until a marker is visibly on top of the target, then call cu_click with that label. Never guess pixel coordinates.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseGridParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseGridTool.ExecuteInternalAsync: Entry - around='{parameters?.Around ?? "null"}', region='{parameters?.Region ?? "null"}', rows={parameters?.Rows}, columns={parameters?.Columns}");

        ArgumentNullException.ThrowIfNull(parameters);

        _session.EnsureUsable();

        var rows = parameters.Rows ?? GridOverlayRenderer.DefaultRows;
        var columns = parameters.Columns ?? GridOverlayRenderer.DefaultColumns;

        ScreenRect region;
        var pushZoom = false;
        string? preamble = null;
        GridLabel? zoomedAround = null;

        if (!string.IsNullOrWhiteSpace(parameters.Around))
        {
            zoomedAround = _session.ResolveLabel(parameters.Around);
            region = _session.CellAround(zoomedAround);
            pushZoom = true;

            // Even divisions put a marker exactly on the point being zoomed around.
            rows = GridView.EvenDivisions(rows);
            columns = GridView.EvenDivisions(columns);

            _session.Look(zoomedAround.Point, zoomedAround.Label);
        }
        else if (!string.IsNullOrWhiteSpace(parameters.Region))
        {
            var previous = _session.CurrentView;
            region = _session.ParseRegion(parameters.Region);
            pushZoom = true;

            if (ComputerUseSession.IsLabelPairRegion(parameters.Region) && previous is not null)
            {
                // Label pairs always mean the view the model already had, never the whole
                // screen. Saying which view they came from is what lets the model notice
                // when it meant labels from an earlier, wider view and landed elsewhere.
                preamble =
                    $"Region {parameters.Region.Trim()} was read from the labels of your previous view, which covered {GridOverlayRenderer.DescribePosition(previous.Region, _session.DesktopBounds)} — label pairs always refer to the view you currently have, not the whole screen. " +
                    "If this is not where you meant, use a named area, \"screen\" or \"monitor:N\" to jump somewhere fixed.";

                _session.Look(new ScreenPoint(region.X + region.Width / 2, region.Y + region.Height / 2), parameters.Region.Trim());
            }
            else if (ComputerUseSession.IsChangedAreaRegion(parameters.Region))
            {
                preamble = $"This view is {parameters.Region.Trim()}: the part of the screen your last action changed, with a small margin around it.";
                _session.Look(new ScreenPoint(region.X + region.Width / 2, region.Y + region.Height / 2), parameters.Region.Trim());
            }
        }
        else
        {
            region = _session.CurrentView?.Region ?? _session.DesktopBounds;
            preamble = _session.CurrentView is null ? null : "Re-captured the current view.";
        }

        var view = _session.Grid(region, rows, columns, pushZoom);

        if (zoomedAround is not null)
        {
            preamble = DescribeZoomCentre(zoomedAround, view);
        }

        var result = ComputerUseToolSupport.BuildViewResult(view, _session, "Desktop grid", preamble);

        AutomatonDebug.Log($"ComputerUseGridTool.ExecuteInternalAsync: Exit - region={view.Region}, depth={_session.ZoomDepth}, {view.ImageBytes.Length} image bytes");
        return Task.FromResult(result);
    }

    /// <summary>
    /// Names the fresh label that now sits on the point the model zoomed around, so it
    /// does not have to find its own target again in a view full of new codes.
    /// </summary>
    private static string DescribeZoomCentre(GridLabel zoomedAround, GridView view)
    {
        var nearest = view.NearestLabel(zoomedAround.Point);
        if (nearest is null)
        {
            return $"Zoomed into {zoomedAround.Label}.";
        }

        var dx = nearest.Point.X - zoomedAround.Point.X;
        var dy = nearest.Point.Y - zoomedAround.Point.Y;
        var offBy = Math.Sqrt((double)dx * dx + (double)dy * dy);

        // A pixel or two is rounding. More than that means the crop was slid back inside
        // the desktop because the point was near its edge, so it is not centred.
        if (offBy <= 2)
        {
            return $"Zoomed into {zoomedAround.Label}. The point {zoomedAround.Label} marked is the centre of this view, under the new marker {nearest.Label}.";
        }

        return $"Zoomed into {zoomedAround.Label}. That point was near the edge of the screen, so this view could not be centred on it; the nearest new marker is {nearest.Label}, about {offBy:0} screen pixels away.";
    }
}

/// <summary>
/// Steps back out one zoom level.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseZoomOutTool : BaseTool<ComputerUseZoomOutParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseZoomOutTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        Parameters = CreateSchema(new Dictionary<string, JsonSchemaProperty>());
    }

    public override string Id => "cu_zoom_out";

    public override string Description => "Go back one zoom level and re-capture it. Use when the zoom went towards the wrong thing and you need to see more context again.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseZoomOutParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseZoomOutTool.ExecuteInternalAsync: Entry - depth={_session.ZoomDepth}");

        _session.EnsureUsable();

        var view = _session.ZoomOut();

        if (view is null)
        {
            AutomatonDebug.Log("ComputerUseZoomOutTool.ExecuteInternalAsync: Exit - already at the top");

            return Task.FromResult(new ToolResult
            {
                Title = "Already at the top",
                Output = "This is already the outermost view. Use cu_grid with region=\"screen\" to re-capture the whole desktop."
            });
        }

        var result = ComputerUseToolSupport.BuildViewResult(view, _session, "Zoomed out", "Zoomed back out one level.");

        AutomatonDebug.Log($"ComputerUseZoomOutTool.ExecuteInternalAsync: Exit - region={view.Region}, depth={_session.ZoomDepth}");
        return Task.FromResult(result);
    }
}

/// <summary>
/// An unannotated view, for when the labels are covering the thing being identified.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseScreenshotTool : BaseTool<ComputerUseScreenshotParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseScreenshotTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["region"] = Property("string", "What to capture; defaults to the current view's region. Takes the same forms as cu_grid's region: \"screen\", \"monitor:N\", a named area such as \"bottom-strip\" or \"left\", \"changed:N\" for an area your last action changed, or two labels of the current view like \"B2:C4\"."),
                ["showPointer"] = Property("boolean", "Draw a crosshair where the mouse pointer is. Use after cu_move to check an aim before clicking. Default: false.")
            });
    }

    public override string Id => "cu_screenshot";

    public override string Description => "Screenshot without the grid drawn on it. Use when the grid labels are covering the thing you are trying to identify. You cannot click from this view — call cu_grid to get labels back. Set showPointer=true after cu_move to see where the pointer actually landed: cu_move resolves a label to the same point cu_click would use, so a crosshair sitting on your target means the click will hit it. Worth the extra step when a target is small or you have lost track of where a zoom left you; skip it when a marker is plainly on the thing already.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseScreenshotParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseScreenshotTool.ExecuteInternalAsync: Entry - region='{parameters?.Region ?? "null"}'");

        ArgumentNullException.ThrowIfNull(parameters);

        _session.EnsureUsable();

        ScreenRect? region = string.IsNullOrWhiteSpace(parameters.Region)
            ? null
            : _session.ParseRegion(parameters.Region);

        var shot = _session.Screenshot(region, parameters.ShowPointer);
        var captured = shot.Region;

        // A screenshot deliberately does not become the current view: it has no
        // labels of its own. Saying so matters, because the labels still in play
        // belong to a different — possibly much smaller — region, and a model that
        // assumed otherwise would name a label meaning a point in this picture and
        // click somewhere else entirely.
        var labelled = _session.CurrentView;
        var caveat = labelled is null
            ? "There are no labels in play at all — call cu_grid before naming any label."
            : $"This image has NO labels. The labels you can still name belong to the previous cu_grid view, which covered {GridOverlayRenderer.DescribePosition(labelled.Region, _session.DesktopBounds)} — not this image. Call cu_grid to get labels for what you are looking at now.";

        AutomatonDebug.Log($"ComputerUseScreenshotTool.ExecuteInternalAsync: Exit - region={captured}, {shot.ImageBytes.Length} bytes, labels still belong to {labelled?.Region.ToString() ?? "nothing"}");

        return Task.FromResult(new ToolResult
        {
            Title = "Screenshot",
            Output = $"Unannotated view of {GridOverlayRenderer.DescribePosition(captured, _session.DesktopBounds)}, at {shot.Magnification:0.##}x."
                + $"{Environment.NewLine}{GridOverlayRenderer.DescribeScale(shot)}"
                + (parameters.ShowPointer
                    ? $"{Environment.NewLine}The red crosshair is where the mouse pointer actually is - that is the point a click would land on."
                    : string.Empty)
                + $"{Environment.NewLine}{caveat}",
            Metadata = new Dictionary<string, object>
            {
                ["region"] = captured.ToString(),
                ["imageWidth"] = shot.ImageWidth,
                ["imageHeight"] = shot.ImageHeight,
                ["magnification"] = Math.Round(shot.Magnification, 3)
            },
            Attachments =
            {
                new ToolAttachment
                {
                    Filename = $"cu-shot-{DateTime.UtcNow:HHmmss}.jpg",
                    Content = Convert.ToBase64String(shot.ImageBytes),
                    ContentType = shot.MediaType
                }
            }
        });
    }
}

/// <summary>
/// Clicks a labelled intersection, a text match, or a piece of text found on the spot.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseClickTool : BaseTool<ComputerUseClickParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseClickTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["label"] = Property("string", "What to click: a label from the most recent cu_grid view, e.g. \"C4\", or a text match from cu_find_text, e.g. \"T1\". Give label or text, not both."),
                ["text"] = Property("string", "Text to find and click in one call, e.g. \"Add to Bag\" — the fastest way to press anything with a visible label. Clicks only when the text appears exactly once; otherwise nothing is clicked and the matches are listed as T labels for you to choose from."),
                ["region"] = Property("string", "With text: where to look, in cu_grid's region forms (\"right\", \"changed:1\", \"monitor:2\" …). The whole screen if omitted. Narrow it when the text appears in more than one place."),
                ["button"] = Property("string", "Which button.", new List<string> { "left", "right", "middle" }),
                ["count"] = Property("integer", "1 for a single click, 2 for a double click. Default 1.")
            });
    }

    public override string Id => "cu_click";

    public override string Description =>
        "Click something. With text=\"…\" it finds that text on screen and clicks it in one call, when it appears exactly once. With label it clicks a grid marker or a cu_find_text match — " +
        "only call that when the marker is visibly on top of your target; if it is not, zoom in first or cu_peek it. " +
        "The result says what changed on screen afterwards, and where: use it to tell a click that worked from one that missed before deciding whether to click again.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseClickParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseClickTool.ExecuteInternalAsync: Entry - label='{parameters?.Label ?? "null"}', text='{parameters?.Text ?? "null"}', region='{parameters?.Region ?? "null"}', button='{parameters?.Button ?? "null"}', count={parameters?.Count}");

        ArgumentNullException.ThrowIfNull(parameters);

        _session.EnsureUsable();

        var button = ComputerUseToolSupport.ParseButton(parameters.Button);
        var count = parameters.Count ?? 1;
        var hasLabel = !string.IsNullOrWhiteSpace(parameters.Label);
        var hasText = !string.IsNullOrWhiteSpace(parameters.Text);

        if (hasLabel && hasText)
        {
            throw new ArgumentException("Give either label or text, not both.");
        }

        if (hasText)
        {
            return Task.FromResult(ClickText(parameters.Text!, parameters.Region, button, count));
        }

        if (!hasLabel)
        {
            throw new ArgumentException("label or text is required: a grid label such as \"C4\", a text match such as \"T1\", or text=\"…\" to find and click in one call.");
        }

        var label = _session.ResolveLabel(parameters.Label);

        // Only a grid label can be stale. A text label that resolved is fresh by
        // construction, and printing this for text labels made the model believe every
        // action invalidated all its labels.
        var wasStale = !ComputerUseSession.IsTextLabel(parameters.Label) && _session.IsLabelStale(parameters.Label);

        var report = _session.Click(label, button, count);

        var what = $"Clicked {label.Label} at ({label.Point.X},{label.Point.Y}) with the {button.ToString().ToLowerInvariant()} button{(count > 1 ? $" x{count}" : string.Empty)}.";

        if (wasStale)
        {
            what += " Note: the screen had changed under this label after its view was captured, so it may not have been over what you last saw there.";
        }

        AutomatonDebug.Log($"ComputerUseClickTool.ExecuteInternalAsync: Exit - {what}");
        return Task.FromResult(ComputerUseToolSupport.BuildActionResult("Click", what, _session, report, "the click", label.Point));
    }

    /// <summary>
    /// Find-and-click. Clicks only on exactly one exact match: with several, or with
    /// only close readings, guessing would be a misclick the model never saw coming, so
    /// the matches are labelled and handed back instead.
    /// </summary>
    private ToolResult ClickText(string text, string? regionName, MouseButton button, int count)
    {
        var desktop = _session.DesktopBounds;
        var region = string.IsNullOrWhiteSpace(regionName) ? desktop : _session.ParseRegion(regionName);
        var search = _session.FindText(text, region);
        var where = ComputerUseToolSupport.DescribeSearchArea(region, desktop);

        if (search.TotalMatches == 1 && !search.Approximate)
        {
            var target = search.Targets[0];
            var label = new GridLabel { Label = target.Label, Point = target.Point };
            var report = _session.Click(label, button, count);

            var what = $"Found \"{search.Query}\" once — {ComputerUseToolSupport.DescribeTarget(target, desktop)} — and clicked it at ({target.Point.X},{target.Point.Y}) with the {button.ToString().ToLowerInvariant()} button{(count > 1 ? $" x{count}" : string.Empty)}.";

            AutomatonDebug.Log($"ComputerUseClickTool.ClickText: Exit - clicked the single match for '{search.Query}' at {target.Point}");
            return ComputerUseToolSupport.BuildActionResult("Click", what, _session, report, "the click", target.Point);
        }

        var lines = new List<string>
        {
            search.TotalMatches == 0
                ? "Nothing was clicked."
                : search.Approximate
                    ? $"Nothing was clicked: \"{search.Query}\" was not found exactly, only close readings. If one of them is your target, cu_click label=\"T1\" (or the right T label)."
                    : $"Nothing was clicked: \"{search.Query}\" appears {search.TotalMatches} times. Pick one with cu_click label=\"Tn\", or call again with region to narrow the search."
        };

        lines.AddRange(ComputerUseToolSupport.DescribeSearch(search, where, desktop, suggestClick: false));

        AutomatonDebug.Log($"ComputerUseClickTool.ClickText: Exit - not clicked, {search.TotalMatches} match(es), approximate={search.Approximate}");

        return new ToolResult
        {
            Title = "Click",
            Output = string.Join(Environment.NewLine, lines),
            Metadata = new Dictionary<string, object>
            {
                ["clicked"] = false,
                ["matches"] = search.TotalMatches,
                ["approximate"] = search.Approximate
            }
        };
    }
}

/// <summary>
/// <summary>
/// Moves the pointer without pressing, to reveal hovers and menus.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseMoveTool : BaseTool<ComputerUseMoveParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseMoveTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["label"] = Property("string", "The grid label, or cu_find_text match such as \"T1\", to move the pointer to.")
            },
            new List<string> { "label" });
    }

    public override string Id => "cu_move";

    public override string Description => "Move the pointer to a labelled point without clicking, to reveal a tooltip, a hover state, or a menu that opens on hover.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseMoveParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseMoveTool.ExecuteInternalAsync: Entry - label='{parameters?.Label ?? "null"}'");

        ArgumentNullException.ThrowIfNull(parameters);

        _session.EnsureUsable();

        var label = _session.ResolveLabel(parameters.Label);
        var report = _session.MoveTo(label);

        var what = $"Pointer moved to {label.Label} at ({label.Point.X},{label.Point.Y}).";

        AutomatonDebug.Log($"ComputerUseMoveTool.ExecuteInternalAsync: Exit - {what}");
        return Task.FromResult(ComputerUseToolSupport.BuildActionResult("Move", what, _session, report, "the hover", label.Point));
    }
}

/// <summary>
/// Drags between two labelled points.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseDragTool : BaseTool<ComputerUseDragParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseDragTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["from"] = Property("string", "Label to press at."),
                ["to"] = Property("string", "Label to release at. Grid labels must both come from the current view; cu_find_text matches (T1, T2 …) can be mixed with them."),
                ["button"] = Property("string", "Which button to hold.", new List<string> { "left", "right", "middle" })
            },
            new List<string> { "from", "to" });
    }

    public override string Id => "cu_drag";

    public override string Description => "Press at one labelled point, move, and release at another. Both labels must come from the same cu_grid view, so zoom out far enough that both ends are visible before dragging.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseDragParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseDragTool.ExecuteInternalAsync: Entry - from='{parameters?.From ?? "null"}', to='{parameters?.To ?? "null"}'");

        ArgumentNullException.ThrowIfNull(parameters);

        _session.EnsureUsable();

        var from = _session.ResolveLabel(parameters.From);
        var to = _session.ResolveLabel(parameters.To);
        var button = ComputerUseToolSupport.ParseButton(parameters.Button);

        var report = _session.Drag(from, to, button);

        var what = $"Dragged from {from.Label} ({from.Point.X},{from.Point.Y}) to {to.Label} ({to.Point.X},{to.Point.Y}) with the {button.ToString().ToLowerInvariant()} button.";

        AutomatonDebug.Log($"ComputerUseDragTool.ExecuteInternalAsync: Exit - {what}");
        return Task.FromResult(ComputerUseToolSupport.BuildActionResult("Drag", what, _session, report, "the drag", to.Point));
    }
}

/// <summary>
/// Scrolls the wheel at a labelled point.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseScrollTool : BaseTool<ComputerUseScrollParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseScrollTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["direction"] = Property("string", "Which way to scroll.", new List<string> { "up", "down", "left", "right" }),
                ["label"] = Property("string", "Label to scroll over — the wheel acts on whatever is under the pointer. Optional: leave it out to scroll the middle of the foreground window, which needs no cu_grid first. Give a label only to scroll a particular pane, such as a sidebar."),
                ["amount"] = Property("integer", "Wheel notches, 1-20 (more is clamped to 20). Default 3.")
            },
            new List<string> { "direction" });
    }

    public override string Id => "cu_scroll";

    public override string Description => "Scroll the mouse wheel. With no label it scrolls the middle of the foreground window — the usual case, scrolling a page — with no need to grid first. With a label it scrolls over that point, for a specific pane.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseScrollParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseScrollTool.ExecuteInternalAsync: Entry - direction='{parameters?.Direction ?? "null"}', label='{parameters?.Label ?? "null"}', amount={parameters?.Amount}");

        ArgumentNullException.ThrowIfNull(parameters);

        _session.EnsureUsable();

        if (string.IsNullOrWhiteSpace(parameters.Direction))
        {
            throw new ArgumentException("direction is required: up, down, left or right.");
        }

        // Clamped, not refused: a model that asks for 30 notches wants "a lot", and an
        // error costs it a round trip to learn a number it could have been given.
        var requested = parameters.Amount ?? 3;
        var amount = Math.Clamp(requested, 1, 20);
        var clampNote = amount == requested ? string.Empty : $" (you asked for {requested}; the most one call scrolls is 20 — call again to scroll further)";

        var direction = parameters.Direction.Trim().ToLowerInvariant();

        var (notches, horizontal) = direction switch
        {
            "up" => (amount, false),
            "down" => (-amount, false),
            "right" => (amount, true),
            "left" => (-amount, true),
            _ => throw new ArgumentException($"Unknown scroll direction '{parameters.Direction}'. Use up, down, left or right.")
        };

        var (label, where) = ResolveScrollTarget(parameters.Label);
        var report = _session.Scroll(label, notches, horizontal);

        var what = $"Scrolled {direction} {amount} notch(es){clampNote} over {where} at ({label.Point.X},{label.Point.Y}).";

        AutomatonDebug.Log($"ComputerUseScrollTool.ExecuteInternalAsync: Exit - {what}");
        return Task.FromResult(ComputerUseToolSupport.BuildActionResult("Scroll", what, _session, report, "the scroll", label.Point));
    }

    /// <summary>
    /// Where the wheel goes.
    ///
    /// With no label, the middle of the foreground window. The old default was the
    /// middle of the current view, which needed a view to exist and pointed wherever the
    /// last grid happened to be — so the model gridded "centre" before every scroll, four
    /// calls in one run spent only on getting somewhere to put the wheel. Falls back to
    /// the current view, then the desktop, when the foreground is Automaton itself or a
    /// window with no title.
    /// </summary>
    private (GridLabel Label, string Description) ResolveScrollTarget(string? label)
    {
        if (!string.IsNullOrWhiteSpace(label))
        {
            var resolved = _session.ResolveLabel(label);
            return (resolved, resolved.Label);
        }

        var window = _session.ForegroundWindowCentre();
        if (window.HasValue)
        {
            return (new GridLabel { Label = "window centre", Point = window.Value.Point }, window.Value.Description);
        }

        var view = _session.CurrentView;
        if (view is not null)
        {
            var centre = _session.ResolveLabel(GridLabelNaming.Build(view.Rows / 2, view.Columns / 2));
            return (centre, $"{centre.Label}, the middle of the current view (the foreground window could not be used)");
        }

        var desktop = _session.DesktopBounds;
        var point = new ScreenPoint(desktop.X + desktop.Width / 2, desktop.Y + desktop.Height / 2);
        return (new GridLabel { Label = "screen centre", Point = point }, "the middle of the screen (no foreground window or view to use)");
    }
}

/// <summary>
/// Types literal text.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseTypeTool : BaseTool<ComputerUseTypeParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseTypeTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["text"] = Property("string", "Text to type into whatever currently has keyboard focus. A newline presses Enter.")
            },
            new List<string> { "text" });
    }

    public override string Id => "cu_type";

    public override string Description => "Type text into whatever has keyboard focus. Click the field first — this does not focus anything itself.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseTypeParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseTypeTool.ExecuteInternalAsync: Entry - {parameters?.Text?.Length ?? 0} character(s)");

        ArgumentNullException.ThrowIfNull(parameters);

        if (parameters.Text is null)
        {
            throw new ArgumentException("text is required.");
        }

        _session.EnsureUsable();
        var report = _session.TypeText(parameters.Text);

        var what = $"Typed {parameters.Text.Length} character(s).";

        AutomatonDebug.Log($"ComputerUseTypeTool.ExecuteInternalAsync: Exit - {what}");
        return Task.FromResult(ComputerUseToolSupport.BuildActionResult("Type", what, _session, report, "the typing", actionPoint: null));
    }
}

/// <summary>
/// Presses a key combination.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseKeyTool : BaseTool<ComputerUseKeyParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseKeyTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["keys"] = Property("string", "Combination like \"ctrl+c\", \"alt+f4\", \"win\", \"enter\", \"tab\", \"esc\". Modifiers are held while the last key is pressed.")
            },
            new List<string> { "keys" });
    }

    public override string Id => "cu_key";

    public override string Description => "Press a key or key combination — enter, tab, esc, arrows, function keys, or a chord like ctrl+shift+n. Often faster and more reliable than hunting for a menu item to click.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseKeyParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseKeyTool.ExecuteInternalAsync: Entry - keys='{parameters?.Keys ?? "null"}'");

        ArgumentNullException.ThrowIfNull(parameters);

        if (string.IsNullOrWhiteSpace(parameters.Keys))
        {
            throw new ArgumentException("keys is required.");
        }

        _session.EnsureUsable();
        var report = _session.PressKeys(parameters.Keys);

        var what = $"Pressed {parameters.Keys}.";

        AutomatonDebug.Log($"ComputerUseKeyTool.ExecuteInternalAsync: Exit - {what}");
        return Task.FromResult(ComputerUseToolSupport.BuildActionResult("Key press", what, _session, report, "the key press", actionPoint: null));
    }
}

/// <summary>
/// Finds text on screen and labels each place it appears.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseFindTextTool : BaseTool<ComputerUseFindTextParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseFindTextTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["text"] = Property("string", "The text to find, as it appears on screen: a word or short phrase such as \"Move install folder\", \"Add to Bag\" or \"16TB\". Case and punctuation do not matter. A few distinctive words work better than a long sentence."),
                ["region"] = Property("string", "Where to look; the whole screen if omitted. Takes cu_grid's region forms — a named area such as \"right\", \"changed:1\" for what your last action changed, \"monitor:N\", or two labels. Narrow it when the text appears in many places."),
                ["show"] = Property("boolean", "Also return an image of the matches, boxed and tagged with their labels. Only needed when several matches read the same and the quoted lines do not tell them apart. Default false.")
            },
            new List<string> { "text" });
    }

    public override string Id => "cu_find_text";

    public override string Description =>
        "Find text on screen by reading it, and get a label on each place it appears — T1, T2 and so on — that cu_click, cu_move, cu_drag, cu_scroll and cu_peek accept, and cu_grid around= zooms onto. " +
        "When the text appears once, cu_click text=\"…\" does the find and the click in one call; use this when you want to see the matches first, or need one of several. " +
        "Reads the whole screen in well under a second, tolerates OCR misreadings, and offers close readings when nothing matches exactly. Text in Automaton's own windows is ignored. " +
        "It cannot read text drawn inside an image or icon — use cu_grid for those. The labels last until the screen changes under them; each search adds to the ones in play rather than replacing them.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseFindTextParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseFindTextTool.ExecuteInternalAsync: Entry - text='{parameters?.Text ?? "null"}', region='{parameters?.Region ?? "null"}', show={parameters?.Show}");

        ArgumentNullException.ThrowIfNull(parameters);

        if (string.IsNullOrWhiteSpace(parameters.Text))
        {
            throw new ArgumentException("text is required: the word or phrase to find on screen.");
        }

        _session.EnsureUsable();

        var desktop = _session.DesktopBounds;
        var region = string.IsNullOrWhiteSpace(parameters.Region) ? desktop : _session.ParseRegion(parameters.Region);
        var search = _session.FindText(parameters.Text, region);
        var where = ComputerUseToolSupport.DescribeSearchArea(region, desktop);

        var lines = ComputerUseToolSupport.DescribeSearch(search, where, desktop, suggestClick: true);

        if (search.Targets.Count > 0)
        {
            _session.Look(search.Targets[0].Point, search.Targets[0].Label);
        }

        var result = new ToolResult
        {
            Title = "Find text",
            Output = string.Join(Environment.NewLine, lines),
            Metadata = new Dictionary<string, object>
            {
                ["matches"] = search.TotalMatches,
                ["approximate"] = search.Approximate,
                ["linesRead"] = search.Lines.Count,
                ["retried"] = search.Retried,
                ["elapsedMs"] = (long)search.Elapsed.TotalMilliseconds
            }
        };

        if (parameters.Show && search.Targets.Count > 0)
        {
            var view = _session.RenderTextTargets(search.Targets);
            result.Attachments.Add(new ToolAttachment
            {
                Filename = $"cu-text-{DateTime.UtcNow:HHmmss}.jpg",
                Content = Convert.ToBase64String(view.ImageBytes),
                ContentType = view.MediaType
            });
        }

        AutomatonDebug.Log($"ComputerUseFindTextTool.ExecuteInternalAsync: Exit - {search.TotalMatches} match(es), approximate={search.Approximate}, retried={search.Retried}, {search.Elapsed.TotalMilliseconds:0}ms");
        return Task.FromResult(result);
    }
}

/// <summary>
/// <summary>
/// Brings a window to the front by name.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseFocusWindowTool : BaseTool<ComputerUseFocusWindowParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUseFocusWindowTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["title"] = Property("string", "The window's title or a distinctive part of it, e.g. \"Steam\" or \"Properties\". A process name such as \"notepad\" also works. Matching ignores case; an exact title beats a partial one, and ties go to the window nearest the front.")
            },
            new List<string> { "title" });
    }

    public override string Id => "cu_focus_window";

    public override string Description =>
        "Bring a window to the front and give it keyboard focus, by its title. Use this instead of clicking a partly covered window to raise it, and before typing into an app that may not have focus. " +
        "Restores the window if it is minimised. If no window matches, the result lists the open windows so you can pick a real title. Your grid labels are stale afterwards — call cu_grid.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseFocusWindowParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseFocusWindowTool.ExecuteInternalAsync: Entry - title='{parameters?.Title ?? "null"}'");

        ArgumentNullException.ThrowIfNull(parameters);

        if (string.IsNullOrWhiteSpace(parameters.Title))
        {
            throw new ArgumentException("title is required.");
        }

        _session.EnsureUsable();

        var result = _session.FocusWindow(parameters.Title);

        var what = result.Focused
            ? $"Brought {result.Target.Describe()} to the front."
            : $"Windows refused to bring {result.Target.Describe()} to the front. It may belong to an elevated app, or Windows is blocking focus changes right now — click a visible part of that window instead.";

        if (result.OtherMatches.Count > 0)
        {
            var others = string.Join("; ", result.OtherMatches.Take(5).Select(w => w.Describe()));
            what += $" '{parameters.Title.Trim()}' also matched: {others}. If you meant one of those, call again with more of its title.";
        }

        AutomatonDebug.Log($"ComputerUseFocusWindowTool.ExecuteInternalAsync: Exit - {what}");
        return Task.FromResult(ComputerUseToolSupport.BuildActionResult("Focus window", what, _session, result.Report, "focusing the window", actionPoint: null));
    }
}

/// <summary>
/// A small, cheap look at what one label is sitting on.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUsePeekTool : BaseTool<ComputerUsePeekParameters>
{
    private readonly ComputerUseSession _session;

    public ComputerUsePeekTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["label"] = Property("string", "The label to look at: a grid label from the current view such as \"C3\", or a cu_find_text match such as \"T1\"."),
                ["size"] = Property("integer", $"Width and height of the square shown, in screen pixels, 40-400. Default {ComputerUseSession.DefaultPeekSize}. Use a larger size to see what a small thing belongs to.")
            },
            new List<string> { "label" });
    }

    public override string Id => "cu_peek";

    public override string Description =>
        "A small, magnified crop around one label with a red crosshair on the exact point a click on it would land. " +
        "Costs about a tenth of a cu_grid view, so use it freely to check a marker before clicking a small target — an icon, a checkbox, a text field, a narrow button — " +
        "or to read something too small to make out in the current view. It has no labels of its own and leaves the labels in play unchanged.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUsePeekParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUsePeekTool.ExecuteInternalAsync: Entry - label='{parameters?.Label ?? "null"}', size={parameters?.Size}");

        ArgumentNullException.ThrowIfNull(parameters);

        _session.EnsureUsable();

        var label = _session.ResolveLabel(parameters.Label);
        var size = parameters.Size ?? ComputerUseSession.DefaultPeekSize;
        var view = _session.Peek(label, size);

        var stale = !ComputerUseSession.IsTextLabel(parameters.Label) && _session.IsLabelStale(parameters.Label)
            ? " Note: the screen has changed since the view these labels came from, so what the crosshair is on now may not be what you last saw there."
            : string.Empty;

        var output =
            $"Peek at {label.Label}: {view.Region.Width}x{view.Region.Height} screen pixels around ({label.Point.X},{label.Point.Y}), shown at {view.Magnification:0.#}x ({view.ImageWidth}x{view.ImageHeight} image). " +
            $"The red crosshair is exactly where cu_click \"{label.Label}\" lands.{stale}" +
            $"{Environment.NewLine}If the crosshair is on the target, click it. If the target is visible but the crosshair is beside it, do not click: " +
            $"cu_grid region=\"<the two labels either side of it>\" or around=\"{label.Label}\" puts fresh markers over it, or cu_find_text if it has a text label." +
            $"{Environment.NewLine}This image has NO labels of its own; the labels in play are unchanged.";

        AutomatonDebug.Log($"ComputerUsePeekTool.ExecuteInternalAsync: Exit - {label.Label} at {label.Point}, {view.ImageBytes.Length} bytes");

        return Task.FromResult(new ToolResult
        {
            Title = "Peek",
            Output = output,
            Metadata = new Dictionary<string, object>
            {
                ["label"] = label.Label,
                ["region"] = view.Region.ToString(),
                ["imageWidth"] = view.ImageWidth,
                ["imageHeight"] = view.ImageHeight,
                ["magnification"] = Math.Round(view.Magnification, 2)
            },
            Attachments =
            {
                new ToolAttachment
                {
                    Filename = $"cu-peek-{DateTime.UtcNow:HHmmss}-{label.Label}.jpg",
                    Content = Convert.ToBase64String(view.ImageBytes),
                    ContentType = view.MediaType
                }
            }
        });
    }
}

/// <summary>
/// Clicks a point named as a fraction of the picture the model is looking at.
/// </summary>
[SupportedOSPlatform("windows")]
public class ComputerUseClickAtTool : BaseTool<ComputerUseClickAtParameters>
{
    /// <summary>How much screen a verify peek shows around the point.</summary>
    private const int VerifySize = 160;

    private readonly ComputerUseSession _session;

    public ComputerUseClickAtTool(ComputerUseSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["at"] = Property("string", "Where to click, as \"x,y\". Fractions of the image you were last shown — \"0.42,0.71\" is 42% across and 71% down it — or percentages, \"42%,71%\". " +
                    "For pixels, say whose: \"picture 412,530\" (pixels of the image you were last shown, as you see them) or \"screen 515,662\" (real screen pixels, as tools report them). " +
                    "Bare pixels are only taken when the image you were shown is the screen at 1:1; otherwise nothing is clicked and you are asked which you meant."),
                ["region"] = Property("string", "What the fractions are of, if not the last image you were shown: \"screen\", a named area, \"monitor:N\", \"changed:N\", or two labels."),
                ["verify"] = Property("boolean", "Look at the point instead of clicking it: returns a magnified crop with a crosshair where the click would land. Use it for small targets before committing. Default false."),
                ["button"] = Property("string", "Which button.", new List<string> { "left", "right", "middle" }),
                ["count"] = Property("integer", "1 for a single click, 2 for a double click. Default 1.")
            },
            new List<string> { "at" });
    }

    public override string Id => "cu_click_at";

    public override string Description =>
        "Click a point you can see, named as a fraction of the picture you are looking at: cu_click_at at=\"0.42,0.71\". One call instead of a grid descent, for a target you can already make out. " +
        "Fractions, not pixels, so nothing depends on how the image was scaled; if you do give pixels, say \"picture x,y\" or \"screen x,y\" — the image is usually scaled, so the two differ. Use verify=true first on a small target — it shows a magnified crop with a crosshair exactly where the click would land, and costs a tenth of a grid view. " +
        "For text, cu_click text=\"…\" is more reliable still. Fall back to cu_grid when you cannot make the target out at all.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(ComputerUseClickAtParameters parameters, ToolContext context)
    {
        AutomatonDebug.Log($"ComputerUseClickAtTool.ExecuteInternalAsync: Entry - at='{parameters?.At ?? "null"}', region='{parameters?.Region ?? "null"}', verify={parameters?.Verify}");

        ArgumentNullException.ThrowIfNull(parameters);

        _session.EnsureUsable();

        var (point, frame) = ResolvePoint(parameters);
        var button = ComputerUseToolSupport.ParseButton(parameters.Button);
        var count = parameters.Count ?? 1;

        if (parameters.Verify)
        {
            var peek = _session.PeekAt(point, VerifySize);

            return Task.FromResult(new ToolResult
            {
                Title = "Verify point",
                Output =
                    $"NOT clicked — this is a look at {parameters.At!.Trim()} of {frame}, which is screen ({point.X},{point.Y}). " +
                    $"{peek.Region.Width}x{peek.Region.Height} screen pixels shown at {peek.Magnification:0.#}x; the red crosshair is exactly where a click would land. Fractions still refer to {frame}, not to this crop." +
                    $"{Environment.NewLine}On target? Call cu_click_at again with the same at= and no verify. Off to one side? Adjust the fractions — the crop is {peek.Region.Width} screen pixels wide, so a tenth of its width is {peek.Region.Width / 10} pixels.",
                Metadata = new Dictionary<string, object>
                {
                    ["clicked"] = false,
                    ["x"] = point.X,
                    ["y"] = point.Y
                },
                Attachments =
                {
                    new ToolAttachment
                    {
                        Filename = $"cu-verify-{DateTime.UtcNow:HHmmss}.jpg",
                        Content = Convert.ToBase64String(peek.ImageBytes),
                        ContentType = peek.MediaType
                    }
                }
            });
        }

        // The ghost, the change report and the label bookkeeping all run as they do for
        // a labelled click; only the way the point was named is different.
        var label = new GridLabel { Label = $"the point at {parameters.At!.Trim()}", Point = point };
        var report = _session.Click(label, button, count);

        var what = $"Clicked {parameters.At!.Trim()} of {frame} — screen ({point.X},{point.Y}) — with the {button.ToString().ToLowerInvariant()} button{(count > 1 ? $" x{count}" : string.Empty)}.";

        AutomatonDebug.Log($"ComputerUseClickAtTool.ExecuteInternalAsync: Exit - {what}");
        return Task.FromResult(ComputerUseToolSupport.BuildActionResult("Click at", what, _session, report, "the click", point));
    }

    /// <summary>
    /// Reads "x,y" and turns it into a screen point.
    ///
    /// Fractions by default, because they survive every scaling in the chain — the
    /// capture, the render, and whatever the model's own vision encoder did to the
    /// image; (0,0) and (1,1) are read as fractions, which only costs a pixel at the
    /// very corner.
    ///
    /// Pixels must say whose: "picture x,y" (the image last shown, as the model sees it)
    /// or "screen x,y". Bare pixels were once taken as screen pixels, and a model reading
    /// them off a 0.8x whole-desktop picture or a 2.4x crop clicked a fifth (or far more)
    /// off target: right-clicking the folder above the one it meant in Outlook, and
    /// opening the wrong Delete dialog. The same model passed real screen pixels at
    /// other times, so no fixed reading is right. Bare pixels are taken only where both
    /// readings land on the same spot (the picture was the screen at 1:1); otherwise
    /// nothing is clicked and both readings are offered.
    /// </summary>
    private (ScreenPoint Point, string Frame) ResolvePoint(ComputerUseClickAtParameters parameters)
    {
        if (string.IsNullOrWhiteSpace(parameters.At))
        {
            throw new ArgumentException("at is required: \"0.42,0.71\" as fractions of the image you are looking at.");
        }

        var text = parameters.At.Trim();
        var unit = PixelUnit.Unsaid;
        foreach (var (word, said) in new[] { ("screen", PixelUnit.Screen), ("picture", PixelUnit.Picture), ("image", PixelUnit.Picture) })
        {
            if (text.StartsWith(word, StringComparison.OrdinalIgnoreCase))
            {
                unit = said;
                text = text[word.Length..].TrimStart(':', ' ', '=');
                break;
            }

            if (text.EndsWith(word, StringComparison.OrdinalIgnoreCase))
            {
                unit = said;
                text = text[..^word.Length].TrimEnd(' ', ':');
                break;
            }
        }

        var parts = text.Split(new[] { ',', ' ', ';' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        if (parts.Length != 2)
        {
            throw new ArgumentException($"Could not read a point from \"{text}\". Use two numbers: \"0.42,0.71\" for fractions, \"42%,71%\" for percentages, or \"1620,1530\" for screen pixels.");
        }

        var percent = parts[0].EndsWith('%') || parts[1].EndsWith('%');
        var numbers = new double[2];

        for (var i = 0; i < 2; i++)
        {
            if (!double.TryParse(parts[i].TrimEnd('%'), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out numbers[i]))
            {
                throw new ArgumentException($"\"{parts[i]}\" is not a number. Use \"0.42,0.71\", \"42%,71%\" or \"1620,1530\".");
            }
        }

        if (percent)
        {
            numbers[0] /= 100;
            numbers[1] /= 100;
        }

        var asFraction = percent || unit == PixelUnit.Unsaid && numbers[0] >= 0 && numbers[0] <= 1 && numbers[1] >= 0 && numbers[1] <= 1;

        if (!asFraction)
        {
            return ResolvePixels(numbers[0], numbers[1], unit, parameters.Region);
        }

        ScreenRect region;
        string frame;

        if (!string.IsNullOrWhiteSpace(parameters.Region))
        {
            region = _session.ParseRegion(parameters.Region);
            frame = $"region \"{parameters.Region.Trim()}\"";
        }
        else if (_session.LastViewed is { } viewed)
        {
            region = viewed.Region;
            frame = viewed.Description;
        }
        else
        {
            region = _session.DesktopBounds;
            frame = "the whole screen (you have not looked at anything yet)";
        }

        return (ComputerUseSession.PointFromFraction(region, numbers[0], numbers[1]), $"{frame} ({GridOverlayRenderer.DescribePosition(region, _session.DesktopBounds)})");
    }

    private enum PixelUnit
    {
        Unsaid,
        Screen,
        Picture
    }

    /// <summary>How far apart (screen pixels) the two readings of bare pixels may be and still be one point.</summary>
    private const int SamePointTolerance = 4;

    /// <summary>
    /// Pixels to a screen point: screen pixels as they are; picture pixels through the last image shown
    /// (its region and the size it was sent at); bare pixels only when both readings agree.
    /// </summary>
    private (ScreenPoint Point, string Frame) ResolvePixels(double x, double y, PixelUnit unit, string? region)
    {
        AutomatonDebug.Log($"ComputerUseClickAtTool.ResolvePixels: Entry - ({x},{y}) as {unit}, region='{region ?? "null"}'");
        var desktop = _session.DesktopBounds;
        var screen = new ScreenPoint((int)Math.Round(x), (int)Math.Round(y));
        var onDesktop = screen.X >= desktop.X && screen.Y >= desktop.Y && screen.X < desktop.Right && screen.Y < desktop.Bottom;
        var viewed = _session.LastViewed;

        // A region named alongside pixels must be the picture they were read from.
        if (!string.IsNullOrWhiteSpace(region) && unit != PixelUnit.Screen)
        {
            var named = _session.ParseRegion(region);
            if (viewed is not { } last || last.Region.ToString() != named.ToString())
            {
                throw new ArgumentException(
                    $"Nothing was clicked: pixels with region=\"{region.Trim()}\" are ambiguous, as the last image you were shown was not that region. " +
                    $"Give fractions of the region instead (at=\"0.42,0.71\" with the same region), or real screen pixels (at=\"screen {screen.X},{screen.Y}\", no region).");
            }
        }

        ScreenPoint? picture = null;
        string? pictureFrame = null;
        if (viewed is { } shown && x >= 0 && y >= 0 && x <= shown.ImageWidth && y <= shown.ImageHeight)
        {
            var fx = shown.ImageWidth > 1 ? x / (shown.ImageWidth - 1) : 0;
            var fy = shown.ImageHeight > 1 ? y / (shown.ImageHeight - 1) : 0;
            picture = ComputerUseSession.PointFromFraction(shown.Region, Math.Clamp(fx, 0, 1), Math.Clamp(fy, 0, 1));
            pictureFrame = $"{shown.Description} ({GridOverlayRenderer.DescribePosition(shown.Region, desktop)}), a {shown.ImageWidth}x{shown.ImageHeight} image";
        }

        switch (unit)
        {
            case PixelUnit.Screen:
                if (!onDesktop)
                {
                    throw new ArgumentException($"Screen point ({screen.X},{screen.Y}) is outside the desktop {desktop}. Fractions of the image you are looking at are safer: \"0.42,0.71\".");
                }

                AutomatonDebug.Log($"ComputerUseClickAtTool.ResolvePixels: Exit - screen pixels ({screen.X},{screen.Y})");
                return (screen, "the screen, in pixels");

            case PixelUnit.Picture:
                if (picture is not { } inPicture)
                {
                    throw new ArgumentException(viewed is { } seen
                        ? $"Nothing was clicked: ({x:0},{y:0}) is outside the last image you were shown, which is {seen.ImageWidth}x{seen.ImageHeight} pixels. Use fractions of it: at=\"0.42,0.71\"."
                        : "Nothing was clicked: you have not been shown an image yet, so there are no picture pixels. Take a screenshot first, or give screen pixels: at=\"screen x,y\".");
                }

                AutomatonDebug.Log($"ComputerUseClickAtTool.ResolvePixels: Exit - picture pixels to screen ({inPicture.X},{inPicture.Y})");
                return (inPicture, $"{pictureFrame}, in its pixels");
        }

        // Bare pixels: fine when there's only one reading, or both readings are the same spot.
        if (picture is not { } maybe)
        {
            if (!onDesktop)
            {
                throw new ArgumentException($"Screen point ({screen.X},{screen.Y}) is outside the desktop {desktop}. Fractions of the image you are looking at are safer: \"0.42,0.71\".");
            }

            AutomatonDebug.Log($"ComputerUseClickAtTool.ResolvePixels: Exit - bare pixels outside the last image, so screen ({screen.X},{screen.Y})");
            return (screen, "the screen, in pixels");
        }

        if (!onDesktop || Math.Abs(maybe.X - screen.X) <= SamePointTolerance && Math.Abs(maybe.Y - screen.Y) <= SamePointTolerance)
        {
            AutomatonDebug.Log($"ComputerUseClickAtTool.ResolvePixels: Exit - bare pixels read in the picture, screen ({maybe.X},{maybe.Y}) (screen reading {(onDesktop ? "is the same point" : "is off the desktop")})");
            return (maybe, $"{pictureFrame}, in its pixels");
        }

        var shownNow = viewed!.Value;
        var fractionX = x / Math.Max(1, shownNow.ImageWidth - 1);
        var fractionY = y / Math.Max(1, shownNow.ImageHeight - 1);
        AutomatonDebug.Log($"ComputerUseClickAtTool.ResolvePixels: Exit - ambiguous: picture reading ({maybe.X},{maybe.Y}), screen reading ({screen.X},{screen.Y}); refused");
        throw new ArgumentException(
            $"Nothing was clicked: \"{x:0},{y:0}\" could mean two different places, because the last image you were shown ({pictureFrame}) is not the screen at 1:1. " +
            $"Read as pixels of that image, it is screen ({maybe.X},{maybe.Y}); read as screen pixels, it is ({screen.X},{screen.Y}) — {Math.Abs(maybe.X - screen.X) + Math.Abs(maybe.Y - screen.Y)} pixels apart. " +
            $"Call again with the one you meant: at=\"picture {x:0},{y:0}\" (or the same as fractions, at=\"{fractionX:0.###},{fractionY:0.###}\") if you read it off the image, or at=\"screen {screen.X},{screen.Y}\" if those are real screen pixels.");
    }
}
