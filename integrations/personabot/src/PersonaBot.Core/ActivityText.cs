using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace PersonaBot;

/// <summary>
/// The short line above the desktop that says what the persona is doing ("I'm checking tax year 2026…").
/// It comes from the persona's own narration when it gives one (the prompt asks for a short first
/// sentence before acting), otherwise from the tool it is using. Always about ten words at most.
/// </summary>
public static class ActivityText
{
    public const int MaxWords = 10;

    private static readonly Regex Markdown = new(@"[*_`#>\[\]]|\(https?://[^)]*\)", RegexOptions.CultureInvariant);
    private static readonly Regex Spaces = new(@"\s+", RegexOptions.CultureInvariant);
    private static readonly Regex SentenceEnd = new(@"(?<=[.!?…:])\s", RegexOptions.CultureInvariant);

    /// <summary>The first sentence of a reply, capped at <see cref="MaxWords"/> words; null when there is no text.</summary>
    public static string? FromReply(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return null;
        var firstLine = text.Trim().Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).FirstOrDefault(l => Markdown.Replace(l, "").Trim().Length > 0);
        if (firstLine == null) return null;
        var plain = Spaces.Replace(Markdown.Replace(firstLine, ""), " ").Trim().TrimStart('-', '•', ' ');
        var sentence = SentenceEnd.Split(plain)[0].Trim().TrimEnd(':');
        return sentence.Length == 0 ? null : Cap(sentence);
    }

    /// <summary>What a tool call looks like to someone watching, e.g. "Running: npm test" or "Clicking on the screen".</summary>
    public static string ForTool(string tool, string? arguments)
    {
        ArgumentNullException.ThrowIfNull(tool);
        string? Arg(string name)
        {
            try
            {
                using var doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(arguments) ? "{}" : arguments);
                return doc.RootElement.ValueKind == JsonValueKind.Object && doc.RootElement.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
                    ? value.GetString() : null;
            }
            catch (JsonException)
            {
                return null;
            }
        }
        var text = tool switch
        {
            "bash" => "Running: " + FirstLine(Arg("command")),
            "read" => "Reading " + FileName(Arg("path")),
            "write" => "Writing " + FileName(Arg("path")),
            "cu_begin" => Arg("reason") is { Length: > 0 } reason ? reason : "Taking the desktop",
            "cu_end" => "Handing back the desktop",
            "cu_type" => "Typing on the desktop",
            "cu_key" => "Pressing " + (Arg("keys") ?? Arg("key") ?? "keys"),
            "cu_scroll" => "Scrolling",
            "cu_mark" => "Pointing something out on the screen",
            "cu_snapshot" => "Taking a screenshot for the timeline",
            "cu_record" => Arg("action") == "stop" ? "Saving the screen recording" : "Recording the screen",
            "cu_drag" => "Dragging on the screen",
            "cu_move" => "Moving the pointer",
            "cu_find" or "cu_find_text" => Arg("text") is { Length: > 0 } find ? $"Looking for “{find}”" : "Looking for text on screen",
            _ when tool.Contains("click", StringComparison.Ordinal) => "Clicking on the screen",
            _ when tool.StartsWith("cu_", StringComparison.Ordinal) => "Looking at the screen",
            _ => "Using " + tool
        };
        return Cap(text);
    }

    /// <summary>Text cut to <see cref="MaxWords"/> words, with an ellipsis when cut.</summary>
    public static string Cap(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        var words = Spaces.Replace(text, " ").Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
        var capped = words.Length <= MaxWords ? string.Join(' ', words) : string.Join(' ', words.Take(MaxWords)).TrimEnd(',', ';', ':', '.') + "…";
        if (capped.Length > 90) capped = capped[..89].TrimEnd() + "…";
        Trace.WriteLine($"ActivityText.Cap: '{capped}'");
        return capped;
    }

    private static string FirstLine(string? text) => (text ?? "").Trim().Split('\n')[0].Trim() is { Length: > 0 } line ? line : "a command";

    private static string FileName(string? path) => string.IsNullOrWhiteSpace(path) ? "a file" : Path.GetFileName(path.TrimEnd('/')) is { Length: > 0 } name ? name : path;
}
