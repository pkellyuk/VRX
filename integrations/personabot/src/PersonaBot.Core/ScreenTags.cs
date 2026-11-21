using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using Automaton.Core.Interfaces;
using Automaton.Core.Services.ComputerUse;
using SkiaSharp;

namespace PersonaBot;

/// <summary>
/// One thing a local detection model found on the desktop (xidentify): <see cref="Kind"/> "ui" for a control
/// (button, icon, field; <see cref="Label"/> "control") or "object" for an everyday object (its COCO class,
/// "person", "dog", "car"...). Coordinates are screen pixels; <see cref="Id"/> numbers it within its frame.
/// </summary>
public sealed record ScreenTag(int Id, string Kind, string Label, float Score, int X, int Y, int Width, int Height)
{
    [System.Text.Json.Serialization.JsonIgnore] public int CenterX => X + Width / 2;
    [System.Text.Json.Serialization.JsonIgnore] public int CenterY => Y + Height / 2;
}

/// <summary>What to look for, and how sure the models must be.</summary>
public sealed record ScreenTagRequest(bool Ui = true, bool Objects = true, int UiSize = 1280, float UiMinScore = 0.25f, float ObjectMinScore = 0.5f);

/// <summary>The tags in one frame, how long tagging took in all, and the models' own timings.</summary>
public sealed record ScreenTagResult(IReadOnlyList<ScreenTag> Tags, double Milliseconds, string Timings);

/// <summary>
/// Tags desktop frames with a local model on this computer's GPU. Implemented by the host (Windows: the
/// personabot-vision helper); hosts without one have none.
/// </summary>
public interface IScreenTagger : IAsyncDisposable
{
    /// <summary>Where it runs, e.g. "DirectML · NVIDIA GeForce RTX 3090"; known after the first frame.</summary>
    string Description { get; }

    /// <summary>Tags one BGRA frame of <paramref name="width"/> x <paramref name="height"/> (stride width × 4).</summary>
    Task<ScreenTagResult> TagAsync(ReadOnlyMemory<byte> bgra, int width, int height, ScreenTagRequest request, CancellationToken token);
}

public static class ScreenTags
{
    /// <summary>Tags a PNG screenshot (bot tools and recordings work from PNGs and JPEGs).</summary>
    public static async Task<ScreenTagResult> TagImageAsync(IScreenTagger tagger, byte[] image, ScreenTagRequest request, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(tagger);
        ArgumentNullException.ThrowIfNull(image);
        ArgumentNullException.ThrowIfNull(request);
        using var decoded = SkiaImages.Decode(image);
        using var bgra = decoded.ColorType == SKColorType.Bgra8888 && decoded.RowBytes == decoded.Width * 4 ? decoded.Copy() : decoded.Copy(SKColorType.Bgra8888);
        return await tagger.TagAsync(bgra.Bytes, bgra.Width, bgra.Height, request, token);
    }

    /// <summary>The tags as the bot reads them: one line each, biggest objects first, then controls top to bottom.</summary>
    public static string Describe(ScreenTagResult result, string? find = null)
    {
        ArgumentNullException.ThrowIfNull(result);
        var tags = Filter(result.Tags, find);
        var text = new StringBuilder();
        text.Append(CultureInfo.InvariantCulture, $"{tags.Count} tag{(tags.Count == 1 ? "" : "s")}{(string.IsNullOrWhiteSpace(find) ? "" : $" matching \"{find}\"")} of {result.Tags.Count} found in {result.Milliseconds:F0} ms. ");
        text.AppendLine("Positions are screen pixels; click one with cu_click_at at=\"cx,cy\".");
        foreach (var tag in tags.OrderBy(t => t.Kind == "ui").ThenBy(t => t.Kind == "ui" ? t.Y : -t.Width * t.Height).ThenBy(t => t.X))
        {
            text.AppendLine(CultureInfo.InvariantCulture,
                $"#{tag.Id} {(tag.Kind == "ui" ? "control" : tag.Label)} {tag.Score * 100:F0}% box {tag.X},{tag.Y} {tag.Width}x{tag.Height} center {tag.CenterX},{tag.CenterY}");
        }
        return text.ToString().TrimEnd();
    }

    /// <summary>Tags whose label or kind contains <paramref name="find"/> (case-insensitive; "ui" or "control" for controls); all when empty.</summary>
    public static List<ScreenTag> Filter(IEnumerable<ScreenTag> tags, string? find)
    {
        ArgumentNullException.ThrowIfNull(tags);
        if (string.IsNullOrWhiteSpace(find)) return tags.ToList();
        var words = find.Split([',', ' '], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        return tags.Where(t => words.Any(w => t.Label.Contains(w, StringComparison.OrdinalIgnoreCase)
            || t.Kind.Equals(w, StringComparison.OrdinalIgnoreCase)
            || (w.Equals("control", StringComparison.OrdinalIgnoreCase) && t.Kind == "ui"))).ToList();
    }

    /// <summary>The tags as marks on the person's view: controls cyan, objects orange with their name.</summary>
    public static IReadOnlyList<ScreenMark> AsMarks(IEnumerable<ScreenTag> tags)
    {
        ArgumentNullException.ThrowIfNull(tags);
        return tags.Select(t => new ScreenMark(ScreenMarkKind.Box,
            [((double)t.X, (double)t.Y), ((double)(t.X + t.Width), (double)(t.Y + t.Height))],
            t.Kind == "ui" ? $"#{t.Id}" : $"#{t.Id} {t.Label}",
            t.Kind == "ui" ? "#22D3EE" : "#F59E0B")).ToList();
    }
}

/// <summary>
/// cu_tags: the bot asks the local model what is on the screen right now, gets controls and objects with their
/// boxes in screen pixels, and (by default) sees them drawn on the person's view, so the person watches what it found.
/// </summary>
public sealed class ScreenTagTool(Func<IScreenTagger?> tagger, Func<CancellationToken, Task<byte[]>> capture, Action<IReadOnlyList<ScreenMark>, bool> show) : ITool
{
    public string Id => "cu_tags";

    public string Description =>
        "Find controls (buttons, icons, fields, links) and everyday objects (people, animals, vehicles, food...: the 80 COCO classes) on the desktop right now, " +
        "using a fast local vision model on this computer's GPU (tens of milliseconds, no screenshot sent to you). " +
        "Returns each tag's box and center in screen pixels, ready for cu_click_at. Controls are unnamed boxes: use cu_find_text or cu_peek to read them. " +
        "find narrows the list (\"person\", \"dog, cat\", \"control\"). Tags are also drawn on the person's view unless show=false. Works on videos too: call it again to see the current frame.";

    public JsonSchema Parameters { get; } = new()
    {
        Properties = new Dictionary<string, JsonSchemaProperty>
        {
            ["find"] = new() { Type = "string", Description = "Only tags whose name contains one of these words (comma or space separated), or \"control\" for controls. Empty for everything." },
            ["kinds"] = new() { Type = "string", Description = "\"all\" (default), \"ui\" for controls only, or \"objects\" for objects only." },
            ["show"] = new() { Type = "boolean", Description = "Draw the tags on the person's view (default true)." }
        }
    };

    public async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        ArgumentNullException.ThrowIfNull(args);
        ArgumentNullException.ThrowIfNull(context);
        Trace.WriteLine($"ScreenTagTool.ExecuteAsync enter: {args}");
        if (tagger() is not { } model)
        {
            Trace.WriteLine("ScreenTagTool.ExecuteAsync exit: no local vision model on this computer");
            return new ToolResult { Title = "Tags unavailable", Output = "This computer has no local vision model for tagging. Use cu_grid, cu_find_text and cu_peek instead." };
        }
        using var doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(args) ? "{}" : args);
        var root = doc.RootElement;
        var find = root.TryGetProperty("find", out var f) && f.ValueKind == JsonValueKind.String ? f.GetString() : null;
        var kinds = root.TryGetProperty("kinds", out var k) && k.ValueKind == JsonValueKind.String ? k.GetString()?.Trim().ToLowerInvariant() : "all";
        var draw = !root.TryGetProperty("show", out var s) || s.ValueKind != JsonValueKind.False;
        var request = new ScreenTagRequest(Ui: kinds is "all" or "ui" or "controls" or null or "", Objects: kinds is "all" or "objects" or "object" or null or "");
        var image = await capture(context.CancellationToken);
        Trace.WriteLine($"ScreenTagTool.ExecuteAsync: captured {image.Length} bytes; tagging ui={request.Ui}, objects={request.Objects} with {model.Description}");
        var result = await ScreenTags.TagImageAsync(model, image, request, context.CancellationToken);
        var matching = ScreenTags.Filter(result.Tags, find);
        if (draw) show(ScreenTags.AsMarks(matching), true);
        Trace.WriteLine($"ScreenTagTool.ExecuteAsync exit: {matching.Count} of {result.Tags.Count} tags in {result.Milliseconds:F0} ms ({result.Timings})");
        return new ToolResult { Title = $"{matching.Count} tags", Output = ScreenTags.Describe(result, find) };
    }
}
