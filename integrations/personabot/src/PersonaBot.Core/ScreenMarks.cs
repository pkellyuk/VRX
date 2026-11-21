using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using Automaton.Core.Interfaces;
using Automaton.Core.Services.ComputerUse;
using SkiaSharp;

namespace PersonaBot;

/// <summary>A shape a bot draws over the person's view of the desktop to show them something.</summary>
public enum ScreenMarkKind
{
    Box,
    Circle,
    Arrow,
    Highlight,
    Underline,
    Pen,
    Label
}

/// <summary>
/// One mark, in guest screen pixels. Box, circle, highlight and underline use two corners; an arrow runs
/// from its first point to its tip; a pen stroke is all its points; a label sits at its first point.
/// </summary>
public sealed record ScreenMark(ScreenMarkKind Kind, IReadOnlyList<(double X, double Y)> Points, string? Text, string Colour);

/// <summary>
/// Reads the cu_mark tool's arguments. Positions are "x,y": fractions of the whole screen ("0.42,0.71" is
/// 42% across and 71% down), percentages ("42%,71%"), or, when both numbers are above 1, screen pixels,
/// as Automaton's cu_click_at reads them.
/// </summary>
public static class ScreenMarkParser
{
    public const int MaxMarks = 20;
    public const int MaxPenPoints = 200;

    public static readonly IReadOnlyDictionary<string, string> Colours = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
    {
        ["red"] = "#F43F5E", ["yellow"] = "#FACC15", ["green"] = "#22C55E", ["blue"] = "#3B82F6", ["orange"] = "#F97316", ["purple"] = "#A855F7"
    };

    public static IReadOnlyList<ScreenMark> Parse(JsonElement marks, ScreenRect screen,
        Func<string, (double X, double Y)>? resolve = null)
    {
        if (marks.ValueKind != JsonValueKind.Array) throw new ArgumentException("marks must be a list of shapes.");
        if (screen.Width <= 0 || screen.Height <= 0) throw new ArgumentException("The screen has no size yet.");
        Trace.WriteLine($"ScreenMarkParser.Parse enter: {marks.GetArrayLength()} item(s) on a {screen.Width}x{screen.Height} screen");
        resolve ??= text => Point(text, screen);
        var result = new List<ScreenMark>();
        foreach (var item in marks.EnumerateArray())
        {
            if (result.Count == MaxMarks) throw new ArgumentException($"Draw at most {MaxMarks} marks at a time.");
            if (item.ValueKind != JsonValueKind.Object) throw new ArgumentException("Each mark must be an object with a kind.");
            var kindText = Text(item, "kind") ?? throw new ArgumentException("Each mark needs a kind: box, circle, arrow, highlight, underline, pen or label.");
            if (!Enum.TryParse<ScreenMarkKind>(kindText, ignoreCase: true, out var kind) || !Enum.IsDefined(kind))
                throw new ArgumentException($"Unknown mark kind '{kindText}'. Use box, circle, arrow, highlight, underline, pen or label.");
            var colourName = Text(item, "color") ?? Text(item, "colour") ?? (kind == ScreenMarkKind.Highlight ? "yellow" : "red");
            if (!Colours.TryGetValue(colourName, out var colour)) throw new ArgumentException($"Unknown colour '{colourName}'. Use {string.Join(", ", Colours.Keys)}.");
            var label = Text(item, "text");
            if (label is { Length: > 120 }) throw new ArgumentException("Keep a mark's text to 120 characters.");
            List<(double, double)> points;
            switch (kind)
            {
                case ScreenMarkKind.Pen:
                    var path = Text(item, "points") ?? throw new ArgumentException("A pen mark needs points: \"x,y x,y …\".");
                    points = path.Split(' ', StringSplitOptions.RemoveEmptyEntries).Select(resolve).ToList();
                    if (points.Count is < 2 or > MaxPenPoints) throw new ArgumentException($"A pen mark needs 2 to {MaxPenPoints} points.");
                    break;
                case ScreenMarkKind.Label:
                    points = [resolve(Text(item, "at") ?? throw new ArgumentException("A label needs at: where it goes."))];
                    if (string.IsNullOrWhiteSpace(label)) throw new ArgumentException("A label needs text.");
                    break;
                default:
                    var from = Text(item, "at") ?? throw new ArgumentException($"A {kindText} needs at (its {(kind == ScreenMarkKind.Arrow ? "tail" : "top-left corner")}).");
                    var to = Text(item, "to") ?? throw new ArgumentException($"A {kindText} needs to (its {(kind == ScreenMarkKind.Arrow ? "tip" : "bottom-right corner")}).");
                    points = [resolve(from), resolve(to)];
                    break;
            }
            result.Add(new ScreenMark(kind, points, string.IsNullOrWhiteSpace(label) ? null : label.Trim(), colour));
        }
        Trace.WriteLine($"ScreenMarkParser.Parse exit: {string.Join(", ", result.GroupBy(m => m.Kind).Select(g => $"{g.Count()} {g.Key}"))}");
        return result;
    }

    /// <summary>An "x,y" position as screen pixels, clamped to the screen.</summary>
    public static (double X, double Y) Point(string text, ScreenRect screen)
    {
        ArgumentNullException.ThrowIfNull(text);
        var parts = text.Split(',', StringSplitOptions.TrimEntries);
        if (parts.Length != 2) throw new ArgumentException($"'{text}' isn't a position; use \"x,y\", e.g. \"0.42,0.71\".");
        var percent = parts.All(p => p.EndsWith('%'));
        if (!double.TryParse(parts[0].TrimEnd('%'), NumberStyles.Float, CultureInfo.InvariantCulture, out var x)
            || !double.TryParse(parts[1].TrimEnd('%'), NumberStyles.Float, CultureInfo.InvariantCulture, out var y)
            || !double.IsFinite(x) || !double.IsFinite(y))
            throw new ArgumentException($"'{text}' isn't a position; use \"x,y\", e.g. \"0.42,0.71\".");
        if (percent) { x /= 100; y /= 100; }
        var pixels = !percent && (x > 1 || y > 1);
        var px = pixels ? x : x * (screen.Width - 1);
        var py = pixels ? y : y * (screen.Height - 1);
        return (Math.Clamp(px, 0, screen.Width - 1), Math.Clamp(py, 0, screen.Height - 1));
    }

    private static string? Text(JsonElement item, string name) =>
        item.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;
}

/// <summary>
/// cu_mark: lets a bot point things out to the person by drawing on their view of the desktop. The marks
/// are an overlay in PersonaBot, not part of the Linux screen: they never change the guest, never show in
/// the bot's own screenshots, and stay until the person clears them or the bot draws new ones.
/// </summary>
public sealed class MarkTool : ITool
{
    private readonly Func<ScreenRect> screen;
    private readonly Action<IReadOnlyList<ScreenMark>, bool> show;
    private readonly string target;
    private readonly Func<SKBitmap>? capture;
    private GridView? reference;
    private ScreenRect referenceScreen;
    public event Action<GridView>? ReferenceShown;

    public MarkTool(Func<ScreenRect> screen, Action<IReadOnlyList<ScreenMark>, bool> show,
        string target = "guest screen", Func<SKBitmap>? capture = null)
    {
        this.screen = screen;
        this.show = show;
        this.target = target;
        this.capture = capture;
        if (capture == null) return;
        Parameters.Required = [];
        Parameters.Properties["inspect"] = new() { Type = "boolean", Description = "Get a fresh labelled view to locate the marks. Use this before drawing." };
        Parameters.Properties["zoom_from"] = new() { Type = "string", Description = "With inspect, the top-left grid label of a smaller area." };
        Parameters.Properties["zoom_to"] = new() { Type = "string", Description = "With inspect, the bottom-right grid label of a smaller area." };
    }

    public string Id => "cu_mark";

    public string Description =>
        "Point something out to the person watching: draw boxes, circles, arrows, highlights, underlines, pen strokes or labels over their view of the desktop. " +
        "Use it whenever you want them to look at something specific (a result, an error, a field to fill, where to click). " +
        (capture == null
            ? "Positions are \"x,y\" fractions of the whole screen (\"0.42,0.71\"), percentages, or screen pixels. "
            : "First call with inspect=true. Draw using labels from the returned view (for example B2), or fractions within that view only. " +
              "For a small target, inspect again with zoom_from and zoom_to grid labels that surround it, then use the new view. ") +
        "The marks are an overlay; they don't change any windows or their contents. " +
        "New marks replace the old ones unless keep=true; marks=[] with keep=false clears them.";

    public JsonSchema Parameters { get; } = new()
    {
        Properties = new Dictionary<string, JsonSchemaProperty>
        {
            ["marks"] = new()
            {
                Type = "array", Description = "The shapes to draw, at most 20.",
                Items = new JsonSchema
                {
                    Properties = new Dictionary<string, JsonSchemaProperty>
                    {
                        ["kind"] = new() { Type = "string", Description = "What to draw.", Enum = ["box", "circle", "arrow", "highlight", "underline", "pen", "label"] },
                        ["at"] = new() { Type = "string", Description = "\"x,y\": top-left corner (box, circle, highlight, underline), the arrow's tail, or where a label goes." },
                        ["to"] = new() { Type = "string", Description = "\"x,y\": bottom-right corner, or the arrow's tip (what it points at)." },
                        ["points"] = new() { Type = "string", Description = "For pen: \"x,y x,y …\", 2 to 200 points." },
                        ["text"] = new() { Type = "string", Description = "A short caption shown with the mark (required for label)." },
                        ["color"] = new() { Type = "string", Description = "Default red (yellow for highlight).", Enum = ["red", "yellow", "green", "blue", "orange", "purple"] }
                    },
                    Required = ["kind"]
                }
            },
            ["keep"] = new() { Type = "boolean", Description = "Add to the marks already shown instead of replacing them. Default false." }
        },
        Required = ["marks"]
    };

    public Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        ArgumentNullException.ThrowIfNull(args);
        using var doc = JsonDocument.Parse(args);
        if (capture != null && doc.RootElement.TryGetProperty("inspect", out var inspect) && inspect.ValueKind == JsonValueKind.True)
            return Task.FromResult(Inspect(doc.RootElement, context.CancellationToken));
        if (!doc.RootElement.TryGetProperty("marks", out var marksElement))
        {
            Trace.TraceWarning("MarkTool: refused: no marks argument");
            throw new ArgumentException("marks is required (use [] to clear).");
        }
        var keep = doc.RootElement.TryGetProperty("keep", out var keepElement) && keepElement.ValueKind == JsonValueKind.True;
        var bounds = screen();
        if (capture != null && marksElement.ValueKind == JsonValueKind.Array && marksElement.GetArrayLength() != 0 &&
            (reference == null || !referenceScreen.Equals(bounds)))
            return Task.FromResult(new ToolResult { Title = "Inspect before marking", Output = "Call this mark tool with inspect=true to locate the target on the correct display first." });
        var marks = ScreenMarkParser.Parse(marksElement, bounds,
            reference is { } view ? text => ResolveMarkPoint(view, bounds, text) : null);
        context.CancellationToken.ThrowIfCancellationRequested();
        Trace.WriteLine($"MarkTool: {marks.Count} marks, keep={keep}");
        show(marks, !keep);
        var output = marks.Count == 0 && !keep
            ? "Cleared the marks from the person's view."
            : $"Showed {marks.Count} mark(s) on the person's view of the {target}. They stay until the person clears them or you draw new ones.";
        return Task.FromResult(new ToolResult { Title = "Marked the screen", Output = output });
    }

    public void ResetReference() => reference = null;
    public bool HasZoomedReference => reference is { } view && !view.Region.Equals(referenceScreen);

    public static bool IsInspection(string arguments)
    {
        using var document = JsonDocument.Parse(arguments);
        return document.RootElement.ValueKind == JsonValueKind.Object &&
            document.RootElement.TryGetProperty("inspect", out var inspect) && inspect.ValueKind == JsonValueKind.True;
    }

    private ToolResult Inspect(JsonElement args, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        var bounds = screen();
        var region = bounds;
        string? notice = null;
        if (args.TryGetProperty("zoom_from", out var from) || args.TryGetProperty("zoom_to", out _))
        {
            if (reference is not { } current || !referenceScreen.Equals(bounds))
                notice = "The earlier labels belong to a different view. Start with this fresh full-screen grid and choose the surrounding labels from it. ";
            else if (from.ValueKind != JsonValueKind.String || !args.TryGetProperty("zoom_to", out var to) ||
                to.ValueKind != JsonValueKind.String || current.Resolve(from.GetString()) is not { } first ||
                current.Resolve(to.GetString()) is not { } last || last.Point.X - first.Point.X < 16 || last.Point.Y - first.Point.Y < 16)
                return ReferenceResult(current, bounds, "Choose valid top-left and bottom-right labels that surround the target in this grid. ");
            else
                region = new ScreenRect(first.Point.X, first.Point.Y, last.Point.X - first.Point.X + 1, last.Point.Y - first.Point.Y + 1);
        }
        using var bitmap = capture!();
        if (bitmap.Width != bounds.Width || bitmap.Height != bounds.Height)
            throw new InvalidOperationException("The display changed size. Inspect it again before drawing.");
        using var crop = SkiaImages.Crop(bitmap, new ScreenRect(region.X - bounds.X, region.Y - bounds.Y, region.Width, region.Height));
        var view = new GridOverlayRenderer().Render(crop, region);
        token.ThrowIfCancellationRequested();
        reference = view;
        referenceScreen = bounds;
        ReferenceShown?.Invoke(view);
        return ReferenceResult(view, bounds, notice);
    }

    private ToolResult ReferenceResult(GridView view, ScreenRect bounds, string? notice) =>
        new()
        {
            Title = "View for annotations",
            Output = notice + (view.Region.Equals(bounds)
                ? $"This is the full {target}. "
                : $"This is only a magnified crop of the {target}, at ({view.Region.X},{view.Region.Y}), {view.Region.Width} by {view.Region.Height} screen pixels. ") +
                "Use the labels or x,y fractions from 0 to 1 within this image for at/to. " +
                "Use zoom_from and zoom_to on this grid to zoom further, or inspect=true alone for the full screen.",
            Attachments = { new ToolAttachment { Filename = "annotation-view.jpg", Content = Convert.ToBase64String(view.ImageBytes), ContentType = view.MediaType } }
        };

    private static (double X, double Y) ResolveMarkPoint(GridView view, ScreenRect bounds, string text)
    {
        if (view.Resolve(text) is { } label)
            return (label.Point.X - bounds.X, label.Point.Y - bounds.Y);
        var parts = text.Split(',', StringSplitOptions.TrimEntries);
        if (parts.Length != 2 ||
            !double.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out double x) || !(x is >= 0 and <= 1) ||
            !double.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out double y) || !(y is >= 0 and <= 1))
            throw new ArgumentException("Use a label from the current grid or x,y fractions from 0 to 1 within that view.");
        return (view.Region.X - bounds.X + x * (view.Region.Width - 1), view.Region.Y - bounds.Y + y * (view.Region.Height - 1));
    }
}
