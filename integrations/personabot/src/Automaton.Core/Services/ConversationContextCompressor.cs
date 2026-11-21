using System.Text;
using Automaton.Core.Interfaces;

namespace Automaton.Core.Services;

public sealed class ConversationCompressionSettings
{
    public int ContextLimitTokens { get; init; }
    public int RecentMessagesToKeep { get; init; } = 500;
    public int SummaryMaxCharacters { get; init; } = 6000;

    public int GetAdaptiveSummaryCharacterLimit()
    {
        if (SummaryMaxCharacters <= 0)
        {
            return 1400;
        }

        return Math.Clamp(SummaryMaxCharacters / 3, 1200, 2400);
    }

    public int GetAdaptiveShorthandCharacterLimit()
    {
        if (SummaryMaxCharacters <= 0)
        {
            return 1000;
        }

        return Math.Clamp(SummaryMaxCharacters / 4, 800, 1600);
    }

    /// <summary>
    /// How much of the stretch being compacted the summarising model is shown, in characters.
    /// </summary>
    /// <remarks>
    /// Half the model's own context, counted at four characters a token. The summary is
    /// written by the same model, so its context is the room there is; half of it leaves the
    /// other half for the prompt, the reply, and text that runs denser than four characters
    /// a token (code does). This was a fixed 24,000 characters whatever the model, which on
    /// a million-token context showed the summariser 13 of the 703 messages it was
    /// summarising. The fixed figure is kept as the floor, and as the answer when the
    /// context size is not known.
    /// </remarks>
    public int GetSummaryTranscriptCharacterBudget()
    {
        var floor = Math.Max(SummaryMaxCharacters * 4, 12000);
        if (ContextLimitTokens <= 0)
        {
            return floor;
        }

        var halfTheContext = Math.Min((long)ContextLimitTokens / 2 * 4, int.MaxValue);
        return (int)Math.Max(floor, halfTheContext);
    }

    public int GetCompressionTriggerTokens()
    {
        if (ContextLimitTokens <= 0)
        {
            return int.MaxValue;
        }

        var conservativeReserve = Math.Max(10000, ContextLimitTokens / 10);
        var reserveBasedTrigger = ContextLimitTokens - conservativeReserve;
        var percentageBasedFloor = (int)Math.Floor(ContextLimitTokens * 0.8);
        return Math.Max(1024, Math.Max(reserveBasedTrigger, percentageBasedFloor));
    }

    public int GetDesiredPostCompressionTokens()
    {
        var triggerTokens = GetCompressionTriggerTokens();
        if (triggerTokens == int.MaxValue)
        {
            return int.MaxValue;
        }

        var headroomTokens = ContextLimitTokens <= 0
            ? 2048
            : Math.Max(2048, Math.Min(32768, ContextLimitTokens / 20));

        return Math.Max(1024, triggerTokens - headroomTokens);
    }
}

public static class ConversationContextCompressor
{
    private const string SummaryHeader = "[Compressed conversation summary]";

    public static bool CompressIfNeeded(
        ToolContext context,
        ConversationCompressionSettings settings,
        out int estimatedTokens,
        out int triggerTokens)
    {
        return CompressionSupport.Compress(
            context,
            settings,
            force: false,
            BuildSummary,
            out estimatedTokens,
            out triggerTokens);
    }

    public static Task<CompressionOperationResult> CompressIfNeededAsync(
        ToolContext context,
        ConversationCompressionSettings settings,
        Func<IReadOnlyList<Message>, ConversationCompressionSettings, int, int, CancellationToken, Task<string?>> generateSummaryAsync,
        CancellationToken cancellationToken = default)
    {
        return CompressionSupport.CompressAsync(
            context,
            settings,
            force: false,
            BuildSummary,
            async (messages, compressionSettings, estimatedTokens, triggerTokens, token) =>
            {
                var summaryBody = await generateSummaryAsync(messages, compressionSettings, estimatedTokens, triggerTokens, token);
                return WrapGeneratedSummary(summaryBody, estimatedTokens, triggerTokens);
            },
            cancellationToken);
    }

    public static bool ForceCompact(
        ToolContext context,
        ConversationCompressionSettings settings,
        out int estimatedTokens,
        out int triggerTokens)
    {
        return CompressionSupport.Compress(
            context,
            settings,
            force: true,
            BuildSummary,
            out estimatedTokens,
            out triggerTokens);
    }

    public static Task<CompressionOperationResult> ForceCompactAsync(
        ToolContext context,
        ConversationCompressionSettings settings,
        Func<IReadOnlyList<Message>, ConversationCompressionSettings, int, int, CancellationToken, Task<string?>> generateSummaryAsync,
        CancellationToken cancellationToken = default)
    {
        return CompressionSupport.CompressAsync(
            context,
            settings,
            force: true,
            BuildSummary,
            async (messages, compressionSettings, estimatedTokens, triggerTokens, token) =>
            {
                var summaryBody = await generateSummaryAsync(messages, compressionSettings, estimatedTokens, triggerTokens, token);
                return WrapGeneratedSummary(summaryBody, estimatedTokens, triggerTokens);
            },
            cancellationToken);
    }

    public static int EstimateTokens(IEnumerable<Message> messages)
    {
        return CompressionSupport.EstimateTokens(messages);
    }

    private static string BuildSummary(
        IReadOnlyList<Message> messages,
        ConversationCompressionSettings settings,
        int estimatedTokens,
        int triggerTokens)
    {
        var summaryLimit = settings.GetAdaptiveSummaryCharacterLimit();
        var builder = new StringBuilder();
        builder.AppendLine(SummaryHeader);
        builder.AppendLine($"Estimated tokens at compaction: {estimatedTokens}. Safe threshold: {triggerTokens}. Older messages were compressed locally.");

        foreach (var message in messages)
        {
            var line = BuildSummaryLine(message);
            if (string.IsNullOrWhiteSpace(line))
            {
                continue;
            }

            if (builder.Length + line.Length + Environment.NewLine.Length > summaryLimit)
            {
                builder.AppendLine("... additional earlier context omitted for brevity ...");
                break;
            }

            builder.AppendLine(line);
        }

        return builder.ToString().TrimEnd();
    }

    private static string WrapGeneratedSummary(string? summaryBody, int estimatedTokens, int triggerTokens)
    {
        if (string.IsNullOrWhiteSpace(summaryBody))
        {
            return string.Empty;
        }

        var trimmed = summaryBody.Trim();
        if (trimmed.StartsWith(SummaryHeader, StringComparison.Ordinal))
        {
            return trimmed;
        }

        return string.Join(
            Environment.NewLine,
            SummaryHeader,
            $"Estimated tokens at compaction: {estimatedTokens}. Safe threshold: {triggerTokens}. Older messages were compacted with LLM summary.",
            trimmed);
    }

    private static string BuildSummaryLine(Message message)
    {
        if (message.Role == "system" && CompressionSupport.IsCompressedSummary(message.Content))
        {
            return "- prior summary retained";
        }

        var label = message.Role switch
        {
            "user" => "user",
            "assistant" => "assistant",
            "tool" => "tool",
            "system" => "system",
            _ => "message"
        };

        var text = message.Content ?? string.Empty;
        var toolNames = CompressionSupport.SummarizeToolNames(message);
        if (!string.IsNullOrWhiteSpace(toolNames))
        {
            text = $"{text} [tools: {toolNames}]".Trim();
        }

        text = CompressionSupport.NormalizeInlineText(text, 240);
        return string.IsNullOrWhiteSpace(text)
            ? $"- {label}"
            : $"- {label}: {text}";
    }
}
