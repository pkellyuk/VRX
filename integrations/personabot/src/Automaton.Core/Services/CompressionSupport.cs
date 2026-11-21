using System.Text;
using System.Text.Json;
using Automaton.Core.Interfaces;

namespace Automaton.Core.Services;

public enum ConversationCompressionMode
{
    Summary,
    Shorthand
}

public readonly record struct CompressionOperationResult(
    bool Applied,
    int EstimatedTokens,
    int TriggerTokens,
    bool UsedFallback = false,
    string? SummaryError = null);

public static class ContextCompressor
{
    public static bool CompressIfNeeded(
        ToolContext context,
        ConversationCompressionSettings settings,
        ConversationCompressionMode mode,
        out int estimatedTokens,
        out int triggerTokens)
    {
        if (context == null)
        {
            throw new ArgumentNullException(nameof(context));
        }

        AutomatonDebug.Log($"ContextCompressor.CompressIfNeeded: Entry - mode={mode}, messageCount={context.Messages?.Count ?? 0}");
        var result = mode switch
        {
            ConversationCompressionMode.Shorthand => ShorthandContextCompressor.CompressIfNeeded(context, settings, out estimatedTokens, out triggerTokens),
            _ => ConversationContextCompressor.CompressIfNeeded(context, settings, out estimatedTokens, out triggerTokens)
        };
        AutomatonDebug.Log($"ContextCompressor.CompressIfNeeded: Exiting - result={result}, estimatedTokens={estimatedTokens}, triggerTokens={triggerTokens}");
        return result;
    }

    public static bool ForceCompact(
        ToolContext context,
        ConversationCompressionSettings settings,
        ConversationCompressionMode mode,
        out int estimatedTokens,
        out int triggerTokens)
    {
        if (context == null)
        {
            throw new ArgumentNullException(nameof(context));
        }

        AutomatonDebug.Log($"ContextCompressor.ForceCompact: Entry - mode={mode}, messageCount={context.Messages?.Count ?? 0}");
        var result = mode switch
        {
            ConversationCompressionMode.Shorthand => ShorthandContextCompressor.ForceCompact(context, settings, out estimatedTokens, out triggerTokens),
            _ => ConversationContextCompressor.ForceCompact(context, settings, out estimatedTokens, out triggerTokens)
        };
        AutomatonDebug.Log($"ContextCompressor.ForceCompact: Exiting - result={result}");
        return result;
    }

    public static string GetDisplayName(ConversationCompressionMode mode)
    {
        AutomatonDebug.Log($"ContextCompressor.GetDisplayName: Entry - mode={mode}");
        var result = mode switch
        {
            ConversationCompressionMode.Shorthand => "shorthand",
            _ => "summary"
        };
        AutomatonDebug.Log($"ContextCompressor.GetDisplayName: Exiting - result={result}");
        return result;
    }
}

public static class CompressionSupport
{
    private static readonly string[] SummaryHeaders =
    [
        "[Compressed conversation summary]",
        "[Compressed shorthand context]"
    ];
    internal static IReadOnlyList<string> RecognizedSummaryHeaders => SummaryHeaders;

    /// <summary>
    /// Room kept for text that is attached to a request without being part of the conversation
    /// (the goal digest, at most 1,500 characters). It is not in <c>context.Messages</c>, so it
    /// is not in the estimate; without this the request could cross the limit the trigger is
    /// there to protect.
    /// </summary>
    public const int TransientContextReserveTokens = 500;

    public static bool Compress(
        ToolContext context,
        ConversationCompressionSettings settings,
        bool force,
        Func<IReadOnlyList<Message>, ConversationCompressionSettings, int, int, string> buildSummary,
        out int estimatedTokens,
        out int triggerTokens)
    {
        estimatedTokens = EstimateTokens(context.Messages);
        triggerTokens = settings.GetCompressionTriggerTokens();

        if (!force && estimatedTokens + TransientContextReserveTokens <= triggerTokens)
        {
            return false;
        }

        var slices = CreateCompressionSlices(context.Messages, settings, estimatedTokens, triggerTokens);
        if (slices == null)
        {
            return false;
        }

        var summary = buildSummary(slices.Compressible, settings, estimatedTokens, triggerTokens);
        if (string.IsNullOrWhiteSpace(summary))
        {
            return false;
        }

        context.Messages = ApplySummary(slices, summary, context.SessionId, OffersChapterTool(context));
        return true;
    }

    public static async Task<CompressionOperationResult> CompressAsync(
        ToolContext context,
        ConversationCompressionSettings settings,
        bool force,
        Func<IReadOnlyList<Message>, ConversationCompressionSettings, int, int, string> buildFallbackSummary,
        Func<IReadOnlyList<Message>, ConversationCompressionSettings, int, int, CancellationToken, Task<string?>> buildSummaryAsync,
        CancellationToken cancellationToken = default)
    {
        var estimatedTokens = EstimateTokens(context.Messages);
        var triggerTokens = settings.GetCompressionTriggerTokens();

        if (!force && estimatedTokens + TransientContextReserveTokens <= triggerTokens)
        {
            return new CompressionOperationResult(false, estimatedTokens, triggerTokens);
        }

        var slices = CreateCompressionSlices(context.Messages, settings, estimatedTokens, triggerTokens);
        if (slices == null)
        {
            return new CompressionOperationResult(false, estimatedTokens, triggerTokens);
        }

        var usedFallback = false;
        string? summary = null;

        // Why there is no real summary, when there is none. It travels with the result so
        // whoever shows the compaction can say so: a stub that passes for a summary is the
        // loss nobody sees.
        string? summaryError = null;

        try
        {
            summary = await buildSummaryAsync(
                slices.Compressible,
                settings,
                estimatedTokens,
                triggerTokens,
                cancellationToken);

            if (string.IsNullOrWhiteSpace(summary))
            {
                summaryError = "the model returned no summary text";
            }
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"CompressionSupport.CompressAsync: AI summary generation failed: {ex.Message}. Falling back to simple summary.");
            summaryError = ex.Message;
            // Fall through to use fallback summary
        }

        if (string.IsNullOrWhiteSpace(summary))
        {
            summary = buildFallbackSummary(slices.Compressible, settings, estimatedTokens, triggerTokens);
            usedFallback = !string.IsNullOrWhiteSpace(summary);
        }

        if (string.IsNullOrWhiteSpace(summary))
        {
            return new CompressionOperationResult(false, estimatedTokens, triggerTokens, usedFallback, summaryError);
        }

        context.Messages = ApplySummary(slices, summary, context.SessionId, OffersChapterTool(context));
        return new CompressionOperationResult(true, estimatedTokens, triggerTokens, usedFallback, summaryError);
    }

    public static int EstimateTokens(IEnumerable<Message> messages)
    {
        var total = 0;
        foreach (var message in messages ?? [])
        {
            total += EstimateTokens(message);
        }

        return total;
    }

    public static bool IsCompressedSummary(string? content)
    {
        if (string.IsNullOrWhiteSpace(content))
        {
            return false;
        }

        return SummaryHeaders.Any(header => content.StartsWith(header, StringComparison.Ordinal));
    }

    public static string MergeSummary(string existingContent, string summary)
    {
        var baseContent = RemoveExistingSummary(existingContent);
        if (string.IsNullOrWhiteSpace(baseContent))
        {
            return summary;
        }

        return $"{baseContent.TrimEnd()}{Environment.NewLine}{Environment.NewLine}{summary}";
    }

    public static string NormalizeInlineText(string? text, int maxLength)
    {
        var normalized = (text ?? string.Empty).Replace("\r\n", " ").Replace('\n', ' ').Trim();
        return Truncate(normalized, maxLength);
    }

    public static string Truncate(string? text, int maxLength)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return string.Empty;
        }

        if (text.Length <= maxLength)
        {
            return text;
        }

        return text[..Math.Max(maxLength - 3, 0)] + "...";
    }

    public static string SummarizeToolNames(Message message)
    {
        return string.Join(
            ",",
            (message.Parts ?? [])
                .Where(part => !string.IsNullOrWhiteSpace(part.Tool))
                .Select(part => part.Tool!)
                .Distinct(StringComparer.OrdinalIgnoreCase));
    }

    /// <summary>
    /// The stretch about to be compacted, written out for the model that will summarise it.
    /// </summary>
    /// <remarks>
    /// The prompt has room for only so much - half the model's context, see
    /// <see cref="ConversationCompressionSettings.GetSummaryTranscriptCharacterBudget"/> -
    /// and what does not fit has to be left out somewhere. It used to be everything after
    /// the first stretch that fitted, so the summary of a long stretch described how it began
    /// and nothing of how it ended - and the end is what the conversation carries on from.
    /// Now the opening keeps a quarter of the room and the close three quarters, with the
    /// gap marked.
    /// </remarks>
    public static string BuildCompressionTranscript(IReadOnlyList<Message> messages, ConversationCompressionSettings settings)
    {
        var maxCharacters = settings.GetSummaryTranscriptCharacterBudget();
        var blocks = (messages ?? []).Where(message => message != null).Select(DescribeForTranscript).ToList();

        if (blocks.Sum(block => block.Length) <= maxCharacters)
        {
            return string.Concat(blocks).Trim();
        }

        var headBudget = maxCharacters / 4;
        var tailBudget = maxCharacters - headBudget;

        var head = 0;
        var headLength = 0;
        while (head < blocks.Count && headLength + blocks[head].Length <= headBudget)
        {
            headLength += blocks[head].Length;
            head++;
        }

        var tail = blocks.Count;
        var tailLength = 0;
        while (tail > head && tailLength + blocks[tail - 1].Length <= tailBudget)
        {
            tailLength += blocks[tail - 1].Length;
            tail--;
        }

        // One message larger than its whole share would otherwise leave that end empty.
        var builder = new StringBuilder();
        if (head == 0 && blocks.Count > 0)
        {
            builder.AppendLine(Truncate(blocks[0], headBudget));
            head = 1;
            tail = Math.Max(tail, head);
        }
        else
        {
            builder.Append(string.Concat(blocks.Take(head)));
        }

        if (tail > head)
        {
            builder.AppendLine($"[... {tail - head} message(s) from the middle of this stretch are not shown ...]");
            builder.AppendLine();
        }

        if (tail == blocks.Count && tail > head)
        {
            var last = blocks[^1];
            builder.AppendLine(last.Length <= tailBudget ? last : "..." + last[^Math.Max(tailBudget - 3, 0)..]);
        }
        else
        {
            builder.Append(string.Concat(blocks.Skip(tail)));
        }

        AutomatonDebug.Log($"CompressionSupport.BuildCompressionTranscript: {blocks.Count} message(s) over the {maxCharacters}-character budget; kept the first {head} and the last {blocks.Count - tail}");
        return builder.ToString().Trim();
    }

    private static string DescribeForTranscript(Message message)
    {
        var builder = new StringBuilder();
        builder.Append(string.IsNullOrWhiteSpace(message.Role) ? "message" : message.Role);
        builder.AppendLine(":");

        if (!string.IsNullOrWhiteSpace(message.Content))
        {
            builder.AppendLine(message.Content.Trim());
        }

        foreach (var part in message.Parts ?? [])
        {
            if (!string.IsNullOrWhiteSpace(part.Text))
            {
                builder.Append('[');
                builder.Append(part.Type ?? "part");
                builder.Append("] ");
                builder.AppendLine(part.Text.Trim());
            }

            if (!string.IsNullOrWhiteSpace(part.Tool))
            {
                builder.Append("[tool] ");
                builder.Append(part.Tool);

                var inputSummary = SummarizeToolInput(part.State?.Input);
                if (!string.IsNullOrWhiteSpace(inputSummary))
                {
                    builder.Append(' ');
                    builder.Append(inputSummary);
                }

                if (!string.IsNullOrWhiteSpace(part.State?.Title))
                {
                    builder.Append(" title=");
                    builder.Append(Truncate(part.State.Title, 80));
                }

                builder.AppendLine();

                if (!string.IsNullOrWhiteSpace(part.State?.Output))
                {
                    builder.AppendLine(Truncate(part.State.Output, 800));
                }

                if (!string.IsNullOrWhiteSpace(part.State?.Error))
                {
                    builder.Append("[tool-error] ");
                    builder.AppendLine(Truncate(part.State.Error, 400));
                }
            }
        }

        builder.AppendLine();
        return builder.ToString();
    }

    public static List<Message> CompressMessages(
        IReadOnlyList<Message> messages,
        ConversationCompressionSettings settings,
        int estimatedTokens,
        int triggerTokens,
        Func<IReadOnlyList<Message>, ConversationCompressionSettings, int, int, string> buildSummary)
    {
        var slices = CreateCompressionSlices(messages, settings, estimatedTokens, triggerTokens);
        if (slices == null)
        {
            return messages.ToList();
        }

        var summary = buildSummary(slices.Compressible, settings, estimatedTokens, triggerTokens);
        if (string.IsNullOrWhiteSpace(summary))
        {
            return messages.ToList();
        }

        return ApplySummary(slices, summary, sessionId: null, toolOffered: true);
    }

    public static string MergeSummaryBodyWithHeader(string header, string body, int estimatedTokens, int triggerTokens)
    {
        if (string.IsNullOrWhiteSpace(body))
        {
            return string.Empty;
        }

        return $"{header}{Environment.NewLine}TOK {estimatedTokens}>{triggerTokens}; older ctx compacted.{Environment.NewLine}{body.Trim()}";
    }

    public static string SummarizeToolInput(Dictionary<string, object>? input, int maxValueLength = 40)
    {
        if (input == null || input.Count == 0)
        {
            return string.Empty;
        }

        string[] preferredKeys =
        [
            "filePath", "path", "offset", "limit", "pattern", "command", "url",
            "workingFolder", "goalId", "question", "query", "name", "id"
        ];

        var emitted = new List<string>();
        foreach (var key in preferredKeys)
        {
            if (!input.TryGetValue(key, out var rawValue))
            {
                continue;
            }

            var value = ExtractInlineValue(rawValue);
            if (string.IsNullOrWhiteSpace(value))
            {
                continue;
            }

            emitted.Add($"{ShortenKey(key)}={Truncate(value, maxValueLength)}");
            if (emitted.Count >= 4)
            {
                break;
            }
        }

        return string.Join(" ", emitted);
    }

    private static CompressionSlices? CreateCompressionSlices(
        IReadOnlyList<Message> messages,
        ConversationCompressionSettings settings,
        int estimatedTokens,
        int triggerTokens)
    {
        if (messages == null)
        {
            AutomatonDebug.LogWarning("CompressionSupport.CreateCompressionSlices: Entry - messages is null, nothing to slice");
            return null;
        }

        if (settings == null)
        {
            AutomatonDebug.LogWarning("CompressionSupport.CreateCompressionSlices: Entry - settings is null, nothing to slice");
            return null;
        }

        AutomatonDebug.Log($"CompressionSupport.CreateCompressionSlices: Entry - messageCount={messages.Count}, estimatedTokens={estimatedTokens}, triggerTokens={triggerTokens}");

        if (messages.Count <= 2)
        {
            AutomatonDebug.Log("CompressionSupport.CreateCompressionSlices: Exiting - too few messages to slice");
            return null;
        }

        var leadingSystemCount = messages.TakeWhile(message => message.Role == "system").Count();
        var nonSystemCount = messages.Count - leadingSystemCount;
        if (nonSystemCount <= 1)
        {
            AutomatonDebug.Log($"CompressionSupport.CreateCompressionSlices: Exiting - nonSystemCount={nonSystemCount} leaves nothing to compress");
            return null;
        }

        var recentToKeep = DetermineRecentMessagesToKeep(
            messages,
            settings,
            leadingSystemCount,
            nonSystemCount,
            estimatedTokens,
            triggerTokens);

        var compressibleCount = nonSystemCount - recentToKeep;
        AutomatonDebug.Log($"CompressionSupport.CreateCompressionSlices: leadingSystemCount={leadingSystemCount}, nonSystemCount={nonSystemCount}, recentToKeep={recentToKeep}, compressibleCount={compressibleCount}");
        if (compressibleCount <= 0)
        {
            AutomatonDebug.Log("CompressionSupport.CreateCompressionSlices: Exiting - token budget keeps every non-system message");
            return null;
        }

        var preservedPrefix = messages.Take(leadingSystemCount).ToList();
        var compressible = messages.Skip(leadingSystemCount).Take(compressibleCount).ToList();
        var preservedSuffix = messages.Skip(leadingSystemCount + compressibleCount).ToList();

        // A tool result whose assistant call has been summarised away is an orphan the provider
        // rejects, so the boundary walks forward until the kept tail opens on a real turn.
        while (preservedSuffix.Count > 1 &&
               string.Equals(preservedSuffix[0].Role, "tool", StringComparison.OrdinalIgnoreCase))
        {
            compressible.Add(preservedSuffix[0]);
            preservedSuffix.RemoveAt(0);
        }

        // Provider templates expect a user turn in the request, and a long agentic turn ends in
        // assistant/tool traffic only. Carry the last user message over verbatim - that message
        // alone, never the whole tail sitting behind it. Preserving the tail is what stopped a
        // long turn from ever shrinking: everything the turn had produced counted as "recent",
        // so pass after pass freed nothing and the context ran past the window.
        if (!preservedSuffix.Any(message => string.Equals(message.Role, "user", StringComparison.OrdinalIgnoreCase)))
        {
            var anchorIndex = compressible.FindLastIndex(message => string.Equals(message.Role, "user", StringComparison.OrdinalIgnoreCase));
            if (anchorIndex >= 0)
            {
                var anchor = compressible[anchorIndex];
                compressible.RemoveAt(anchorIndex);
                preservedSuffix.Insert(0, anchor);
                AutomatonDebug.Log($"CompressionSupport.CreateCompressionSlices: carried user anchor from compressible index {anchorIndex} into the preserved tail");
            }
        }

        if (compressible.Count == 0)
        {
            AutomatonDebug.Log("CompressionSupport.CreateCompressionSlices: Exiting - nothing left to compress after boundary adjustments");
            return null;
        }

        AutomatonDebug.Log($"CompressionSupport.CreateCompressionSlices: Exiting - prefix={preservedPrefix.Count}, compressible={compressible.Count}, suffix={preservedSuffix.Count}");
        return new CompressionSlices(preservedPrefix, compressible, preservedSuffix);
    }

    private static int DetermineRecentMessagesToKeep(
        IReadOnlyList<Message> messages,
        ConversationCompressionSettings settings,
        int leadingSystemCount,
        int nonSystemCount,
        int estimatedTokens,
        int triggerTokens)
    {
        var configuredRecentToKeep = Math.Clamp(settings.RecentMessagesToKeep, 1, Math.Max(nonSystemCount - 1, 1));
        if (configuredRecentToKeep <= 1)
        {
            return 1;
        }

        if (triggerTokens == int.MaxValue || estimatedTokens <= triggerTokens)
        {
            return configuredRecentToKeep;
        }

        var targetPostCompressionTokens = settings.GetDesiredPostCompressionTokens();
        if (targetPostCompressionTokens == int.MaxValue)
        {
            return configuredRecentToKeep;
        }

        var prefixTokens = 0;
        for (var index = 0; index < leadingSystemCount; index++)
        {
            prefixTokens += EstimateTokens(messages[index]);
        }

        var summaryBudgetTokens = EstimateSummaryBudgetTokens(settings);
        var availableRecentTokens = targetPostCompressionTokens - prefixTokens - summaryBudgetTokens;
        if (availableRecentTokens <= 0)
        {
            return 1;
        }

        var recentToKeep = 0;
        var recentTokenTotal = 0;
        for (var index = messages.Count - 1; index >= leadingSystemCount && recentToKeep < configuredRecentToKeep; index--)
        {
            var messageTokens = EstimateTokens(messages[index]);
            if (recentToKeep > 0 && recentTokenTotal + messageTokens > availableRecentTokens)
            {
                break;
            }

            recentTokenTotal += messageTokens;
            recentToKeep++;
        }

        return Math.Max(1, recentToKeep);
    }

    /// <summary>Unknown counts as offered: only a turn that has said what it offers can say it does not.</summary>
    private static bool OffersChapterTool(ToolContext context) =>
        context.AvailableToolIds == null || context.AvailableToolIds.Contains(ConversationChapters.ToolId, StringComparer.OrdinalIgnoreCase);

    private static List<Message> ApplySummary(CompressionSlices slices, string summary, string? sessionId, bool toolOffered)
    {
        var result = new List<Message>(slices.PreservedPrefix.Count + slices.PreservedSuffix.Count);
        if (slices.PreservedPrefix.Count > 0)
        {
            result.AddRange(slices.PreservedPrefix.Take(slices.PreservedPrefix.Count - 1));
            var summaryHost = slices.PreservedPrefix[^1];

            // The summary this one replaces is not thrown away: it stays stored as a chapter,
            // and the new summary carries a contents page pointing back at it and the ones
            // before it. See ConversationChapters.
            var earlier = ConversationChapters.EarlierTitles(summaryHost.Content, sessionId);
            AutomatonDebug.Log($"CompressionSupport.ApplySummary: writing chapter {earlier.Count + 1} with {earlier.Count} earlier chapter(s) on its contents page");

            // After the earlier titles are read, or this chapter would list itself among them.
            ConversationChapters.TryStore(sessionId, summary);

            result.Add(new Message
            {
                Id = summaryHost.Id,
                Role = summaryHost.Role,
                Content = MergeSummary(summaryHost.Content, ConversationChapters.WithIndex(summary, earlier, toolOffered)),
                Parts = summaryHost.Parts
            });
        }
        else
        {
            result.Add(new Message
            {
                Id = Guid.NewGuid().ToString(),
                Role = "system",
                Content = summary
            });
        }

        result.AddRange(slices.PreservedSuffix);
        return result;
    }

    private static string RemoveExistingSummary(string content)
    {
        if (string.IsNullOrWhiteSpace(content))
        {
            return string.Empty;
        }

        var index = FindFirstSummaryIndex(content);
        return index >= 0
            ? content[..index].TrimEnd()
            : content;
    }

    /// <summary>
    /// The message content with any compaction summary removed: the system prompt as it was
    /// before compaction merged a summary into it, or the content unchanged when it carries no
    /// summary. This is what the full-transcript store keeps for a summary-hosting message, so
    /// that store never holds anything the compressor wrote.
    /// </summary>
    public static string StripSummary(string? content)
    {
        if (string.IsNullOrWhiteSpace(content))
        {
            return content ?? string.Empty;
        }

        var index = FindFirstSummaryIndex(content);
        if (index < 0)
        {
            return content;
        }

        var stripped = content[..index].TrimEnd();
        AutomatonDebug.Log($"CompressionSupport.StripSummary: removed a summary of {content.Length - index} characters, {stripped.Length} characters of original content remain");
        return stripped;
    }

    /// <summary>
    /// Everything compaction merged into a message - the summary and the contents page under
    /// it - exactly as merged, or null when the message carries none.
    /// </summary>
    public static string? ExtractSummaryBlock(string? content)
    {
        if (string.IsNullOrWhiteSpace(content))
        {
            return null;
        }

        var index = FindFirstSummaryIndex(content);
        if (index < 0)
        {
            return null;
        }

        var block = content[index..].Trim();
        return block.Length == 0 ? null : block;
    }

    /// <summary>
    /// A system prompt with the compaction summary of the messages it stands in for.
    /// </summary>
    /// <remarks>
    /// The summary lives inside the system message, so anything that rewrites the system
    /// message - a refreshed prompt, a settings update, one copy of the conversation being
    /// laid over another - deletes the summary unless it carries it across. That happened
    /// silently for a long time: a conversation was compacted, and a few seconds later the
    /// only record of what had been compacted was gone. Every place that replaces a system
    /// message goes through here.
    ///
    /// <paramref name="authoritative"/> wins over a summary already on
    /// <paramref name="prompt"/>: it is the newer state, and a copy that kept its own older
    /// summary would quietly roll the conversation back a chapter.
    /// </remarks>
    public static string AdoptSummary(string? prompt, IEnumerable<string?> authoritative)
    {
        var block = (authoritative ?? []).Select(ExtractSummaryBlock).LastOrDefault(found => found != null);
        if (block == null)
        {
            return prompt ?? string.Empty;
        }

        return MergeSummary(prompt ?? string.Empty, block);
    }

    /// <summary>
    /// The compaction summary carried by a message, or null when the message carries none. The
    /// summary starts at its header and runs to the end of the content, which is how
    /// <see cref="MergeSummary"/> lays it out.
    /// </summary>
    public static string? ExtractSummary(string? content)
    {
        if (string.IsNullOrWhiteSpace(content))
        {
            return null;
        }

        var index = FindFirstSummaryIndex(content);
        if (index < 0)
        {
            return null;
        }

        // The contents page of earlier chapters rides under the summary but is not part of
        // it: what is stored and searched is the chapter's own text.
        var summary = ConversationChapters.StripIndex(content[index..]).Trim();
        if (summary.Length == 0)
        {
            return null;
        }

        AutomatonDebug.Log($"CompressionSupport.ExtractSummary: found a summary of {summary.Length} characters at offset {index}");
        return summary;
    }

    /// <summary>
    /// Where the first summary starts, or -1. A summary is merged after a blank line, and the
    /// blank line is whatever the platform that compacted wrote - a conversation compacted on
    /// the Android or Linux side and synced here separates with a bare newline - so both forms
    /// are recognised rather than only this platform's.
    /// </summary>
    private static int FindFirstSummaryIndex(string content)
    {
        var bestIndex = -1;
        foreach (var header in SummaryHeaders)
        {
            var leadingIndex = content.StartsWith(header, StringComparison.Ordinal) ? 0 : -1;
            if (leadingIndex < 0)
            {
                leadingIndex = EarliestIndex(
                    content.IndexOf($"\r\n\r\n{header}", StringComparison.Ordinal),
                    content.IndexOf($"\n\n{header}", StringComparison.Ordinal));
            }

            if (leadingIndex < 0)
            {
                continue;
            }

            if (bestIndex < 0 || leadingIndex < bestIndex)
            {
                bestIndex = leadingIndex;
            }
        }

        return bestIndex;
    }

    private static int EarliestIndex(int first, int second)
    {
        if (first < 0)
        {
            return second;
        }

        if (second < 0)
        {
            return first;
        }

        return Math.Min(first, second);
    }

    private static int EstimateSerializedTokens(object? value)
    {
        if (value == null)
        {
            return 0;
        }

        try
        {
            return EstimateTextTokens(JsonSerializer.Serialize(value));
        }
        catch
        {
            return 0;
        }
    }

    /// <summary>
    /// The estimate for a loose piece of text, on the same four-characters-per-token rule
    /// the message estimate uses. Public so a reply still streaming in can be counted with
    /// the same ruler as the messages already settled in the context.
    /// </summary>
    public static int EstimateTextTokens(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return 0;
        }

        return (int)Math.Ceiling(value.Length / 4.0);
    }

    private static int EstimateSummaryBudgetTokens(ConversationCompressionSettings settings)
    {
        return EstimateTextTokens(new string('x', settings.GetAdaptiveSummaryCharacterLimit())) + 48;
    }

    /// <summary>
    /// The flat per-message charge in the estimate, on top of role and text. Named so a
    /// live count of an unsettled message can charge the same amount.
    /// </summary>
    public const int MessageOverheadTokens = 12;

    private static int EstimateTokens(Message message)
    {
        if (message == null)
        {
            return 0;
        }

        var total = MessageOverheadTokens;
        total += EstimateTextTokens(message.Role);
        total += EstimateTextTokens(message.Content);

        foreach (var part in message.Parts ?? [])
        {
            total += 6;
            total += EstimateTextTokens(part.Type);
            total += EstimateTextTokens(part.Text);
            total += EstimateTextTokens(part.Tool);
            total += EstimateTextTokens(part.CallId);

            if (part.State != null)
            {
                total += EstimateTextTokens(part.State.Status);
                total += EstimateSerializedTokens(part.State.Input);
                total += EstimateTextTokens(part.State.Output);
                total += EstimateTextTokens(part.State.Error);
                total += EstimateTextTokens(part.State.Title);
            }
        }

        return total;
    }

    private static string ExtractInlineValue(object? value)
    {
        if (value == null)
        {
            return string.Empty;
        }

        if (value is JsonElement element)
        {
            return element.ValueKind switch
            {
                JsonValueKind.String => element.GetString() ?? string.Empty,
                JsonValueKind.Number or JsonValueKind.True or JsonValueKind.False => element.GetRawText(),
                _ => CompressionSupport.NormalizeInlineText(element.GetRawText(), 40)
            };
        }

        return value switch
        {
            string text => text,
            bool boolean => boolean ? "true" : "false",
            _ => value.ToString() ?? string.Empty
        };
    }

    private static string ShortenKey(string key)
    {
        return key switch
        {
            "filePath" => "file",
            "path" => "path",
            "offset" => "off",
            "limit" => "lim",
            "pattern" => "pat",
            "command" => "cmd",
            "workingFolder" => "wd",
            "goalId" => "goal",
            "question" => "q",
            "query" => "qry",
            _ => key
        };
    }

    private sealed record CompressionSlices(
        List<Message> PreservedPrefix,
        List<Message> Compressible,
        List<Message> PreservedSuffix);
}
