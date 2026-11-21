using System.Globalization;
using Automaton.Core.Interfaces;

namespace Automaton.Core.Services;

/// <summary>
/// Reduces large tool results before they are stored in conversation history.
/// The UI can still render the full tool output separately, but the model only
/// needs a compact working copy to avoid context blow-ups on large listings.
/// </summary>
public static class ToolResultContextSanitizer
{
    public const int MaxOutputLines = 200;
    public const int MaxOutputCharacters = 12_000;

    public static ToolResult SanitizeForConversation(string? toolName, ToolResult? result)
    {
        if (result == null)
        {
            return new ToolResult();
        }

        var output = result.Output ?? string.Empty;
        if (string.IsNullOrEmpty(output))
        {
            return CloneWithoutAttachments(result, output);
        }

        var normalized = output.Replace("\r\n", "\n");
        var lines = normalized.Split('\n');
        var displayedLines = lines.Length > MaxOutputLines
            ? lines.Take(MaxOutputLines).ToArray()
            : lines;

        var truncatedOutput = string.Join(Environment.NewLine, displayedLines);
        var wasLineTruncated = displayedLines.Length < lines.Length;
        var wasCharacterTruncated = truncatedOutput.Length > MaxOutputCharacters;

        if (wasCharacterTruncated)
        {
            truncatedOutput = CompressionSupport.Truncate(truncatedOutput, MaxOutputCharacters);
        }

        if (!wasLineTruncated && !wasCharacterTruncated)
        {
            return CloneWithoutAttachments(result, output);
        }

        var summary = BuildSummary(
            toolName,
            result.Metadata,
            originalCharacters: output.Length,
            originalLines: lines.Length,
            displayedLines: displayedLines.Length);

        var compactOutput = string.Concat(summary, Environment.NewLine, Environment.NewLine, truncatedOutput);
        return CloneWithoutAttachments(result, compactOutput);
    }

    private static ToolResult CloneWithoutAttachments(ToolResult result, string output) =>
        new()
        {
            Title = result.Title,
            Output = output,
            Metadata = new Dictionary<string, object>(result.Metadata),
            Attachments = new List<ToolAttachment>()
        };

    private static string BuildSummary(
        string? toolName,
        Dictionary<string, object> metadata,
        int originalCharacters,
        int originalLines,
        int displayedLines)
    {
        var parts = new List<string>
        {
            $"Output truncated for conversation context from {FormatNumber(originalLines)} lines / {FormatNumber(originalCharacters)} chars.",
            $"Showing first {FormatNumber(displayedLines)} lines and up to {FormatNumber(MaxOutputCharacters)} chars."
        };

        if (string.Equals(toolName, "glob", StringComparison.OrdinalIgnoreCase))
        {
            var matchCount = TryGetMetadataValue(metadata, "count");
            var pattern = TryGetMetadataValue(metadata, "pattern");
            var path = TryGetMetadataValue(metadata, "path");

            if (!string.IsNullOrWhiteSpace(matchCount) || !string.IsNullOrWhiteSpace(pattern) || !string.IsNullOrWhiteSpace(path))
            {
                parts.Add(
                    $"Glob summary: matches={matchCount ?? "?"}, pattern={pattern ?? "?"}, path={path ?? "?"}.");
            }
        }

        return string.Join(" ", parts);
    }

    private static string? TryGetMetadataValue(Dictionary<string, object> metadata, string key)
    {
        if (!metadata.TryGetValue(key, out var value) || value == null)
        {
            return null;
        }

        return value switch
        {
            string text when !string.IsNullOrWhiteSpace(text) => text,
            IFormattable formattable => formattable.ToString(null, CultureInfo.InvariantCulture),
            _ => value.ToString()
        };
    }

    private static string FormatNumber(int value) => value.ToString("N0", CultureInfo.InvariantCulture);
}
