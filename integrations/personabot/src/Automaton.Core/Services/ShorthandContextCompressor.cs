using System.Text;
using Automaton.Core.Interfaces;

namespace Automaton.Core.Services;

public static class ShorthandContextCompressor
{
    private const string SummaryHeader = "[Compressed shorthand context]";

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

    public static Task<CompressionOperationResult> CompressIfNeededAsync(
        ToolContext context,
        ConversationCompressionSettings settings,
        Func<IReadOnlyList<Message>, ConversationCompressionSettings, int, int, CancellationToken, Task<string?>> generateShorthandAsync,
        CancellationToken cancellationToken = default)
    {
        return CompressionSupport.CompressAsync(
            context,
            settings,
            force: false,
            BuildSummary,
            async (messages, compressionSettings, estimatedTokens, triggerTokens, token) =>
            {
                var shorthandBody = await generateShorthandAsync(messages, compressionSettings, estimatedTokens, triggerTokens, token);
                return WrapGeneratedSummary(shorthandBody, estimatedTokens, triggerTokens);
            },
            cancellationToken);
    }

    public static Task<CompressionOperationResult> ForceCompactAsync(
        ToolContext context,
        ConversationCompressionSettings settings,
        Func<IReadOnlyList<Message>, ConversationCompressionSettings, int, int, CancellationToken, Task<string?>> generateShorthandAsync,
        CancellationToken cancellationToken = default)
    {
        return CompressionSupport.CompressAsync(
            context,
            settings,
            force: true,
            BuildSummary,
            async (messages, compressionSettings, estimatedTokens, triggerTokens, token) =>
            {
                var shorthandBody = await generateShorthandAsync(messages, compressionSettings, estimatedTokens, triggerTokens, token);
                return WrapGeneratedSummary(shorthandBody, estimatedTokens, triggerTokens);
            },
            cancellationToken);
    }

    private static string BuildSummary(
        IReadOnlyList<Message> messages,
        ConversationCompressionSettings settings,
        int estimatedTokens,
        int triggerTokens)
    {
        var summaryLimit = settings.GetAdaptiveShorthandCharacterLimit();
        var builder = new StringBuilder();
        builder.AppendLine(SummaryHeader);
        builder.AppendLine($"TOK {estimatedTokens}>{triggerTokens}; older ctx compacted.");

        foreach (var message in messages)
        {
            var line = BuildShorthandLine(message);
            if (string.IsNullOrWhiteSpace(line))
            {
                continue;
            }

            if (builder.Length + line.Length + Environment.NewLine.Length > summaryLimit)
            {
                builder.AppendLine("MORE earlier ctx omitted.");
                break;
            }

            builder.AppendLine(line);
        }

        return builder.ToString().TrimEnd();
    }

    private static string WrapGeneratedSummary(string? shorthandBody, int estimatedTokens, int triggerTokens)
    {
        if (string.IsNullOrWhiteSpace(shorthandBody))
        {
            return string.Empty;
        }

        if (shorthandBody.StartsWith(SummaryHeader, StringComparison.Ordinal))
        {
            return shorthandBody.Trim();
        }

        return CompressionSupport.MergeSummaryBodyWithHeader(SummaryHeader, shorthandBody, estimatedTokens, triggerTokens);
    }

    private static string BuildShorthandLine(Message message)
    {
        if (message.Role == "system" && CompressionSupport.IsCompressedSummary(message.Content))
        {
            return "SYS prior-summary";
        }

        return message.Role switch
        {
            "user" => BuildGenericLine("USR", message, 150),
            "assistant" => BuildAssistantLine(message),
            "tool" => BuildToolLine(message),
            "system" => BuildGenericLine("SYS", message, 140),
            _ => BuildGenericLine("MSG", message, 140)
        };
    }

    private static string BuildGenericLine(string prefix, Message message, int textLimit)
    {
        var text = CompressionSupport.NormalizeInlineText(message.Content, textLimit);
        return string.IsNullOrWhiteSpace(text)
            ? prefix
            : $"{prefix} {text}";
    }

    private static string BuildAssistantLine(Message message)
    {
        var text = CompressionSupport.NormalizeInlineText(message.Content, 140);
        var toolNames = CompressionSupport.SummarizeToolNames(message);
        if (string.IsNullOrWhiteSpace(toolNames))
        {
            return string.IsNullOrWhiteSpace(text) ? "AST" : $"AST {text}";
        }

        return string.IsNullOrWhiteSpace(text)
            ? $"AST tc={toolNames}"
            : $"AST {text} | tc={toolNames}";
    }

    private static string BuildToolLine(Message message)
    {
        var toolPart = (message.Parts ?? []).FirstOrDefault(part => part.Type == "tool");
        if (toolPart == null)
        {
            return BuildGenericLine("TOL", message, 120);
        }

        var segments = new List<string>
        {
            "TOL",
            string.IsNullOrWhiteSpace(toolPart.Tool) ? "unknown" : toolPart.Tool!
        };

        if (!string.IsNullOrWhiteSpace(toolPart.State?.Status))
        {
            segments.Add(toolPart.State.Status switch
            {
                "completed" => "done",
                "error" => "err",
                "pending" => "pend",
                _ => toolPart.State.Status
            });
        }

        var inputSummary = CompressionSupport.SummarizeToolInput(toolPart.State?.Input);
        if (!string.IsNullOrWhiteSpace(inputSummary))
        {
            segments.Add(inputSummary);
        }

        if (!string.IsNullOrWhiteSpace(toolPart.State?.Title))
        {
            segments.Add($"title={CompressionSupport.Truncate(toolPart.State.Title, 36)}");
        }

        var detailText = !string.IsNullOrWhiteSpace(toolPart.State?.Error)
            ? toolPart.State.Error
            : !string.IsNullOrWhiteSpace(toolPart.State?.Output)
                ? toolPart.State.Output
                : message.Content;

        var detail = CompressionSupport.NormalizeInlineText(detailText, 96);
        if (!string.IsNullOrWhiteSpace(detail))
        {
            segments.Add($"out={detail}");
        }

        return string.Join(" | ", segments);
    }
}
