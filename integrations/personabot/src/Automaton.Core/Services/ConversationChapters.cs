using System.Text;
using System.Text.RegularExpressions;

namespace Automaton.Core.Services;

/// <summary>One compaction summary of a conversation, as it was written.</summary>
/// <param name="Number">1 for the first stretch compacted, counting up.</param>
public sealed record ConversationChapter(int Number, string Title, string Content, string CreatedAt);

/// <summary>
/// A conversation's compaction summaries read as a book: each compaction writes one chapter,
/// every chapter is kept in full, and the context carries the newest chapter plus a contents
/// page of the ones before it.
///
/// Before this the newest summary simply replaced the last one, so after a second compaction
/// the model knew the latest stretch and nothing of what came before it. Folding the old
/// summary into the new one would have kept something, but a summary of a summary fades a
/// little more each time; a chapter stays as sharp as when it was written, and costs nothing
/// in the context until it is read.
///
/// The chapters themselves are the rows of <c>ConversationSummaries</c>, which the
/// full-transcript store has always kept (FULLTRANSCRIPT.md). This class adds the contents
/// page and the way back to them.
/// </summary>
public static partial class ConversationChapters
{
    /// <summary>Opens the contents page inside a compacted system message.</summary>
    public const string IndexHeader = "[Earlier summaries of this conversation]";

    /// <summary>The tool that lists and reads chapters.</summary>
    public const string ToolId = "conversation_summaries";

    /// <summary>How many titles the contents page shows; the tool lists the rest.</summary>
    private const int MaxListed = 40;

    private const int MaxTitleLength = 110;

    [GeneratedRegex(@"^\s*(\d+)\.\s+(.+?)\s*$", RegexOptions.Compiled)]
    private static partial Regex IndexLineRegex();

    /// <summary>
    /// The titles of every chapter before the one about to be written, numbered.
    /// </summary>
    /// <param name="hostContent">The system message as it stands, old summary and old contents page included.</param>
    /// <param name="sessionId">The conversation's session, when the caller has one.</param>
    /// <remarks>
    /// The stored chapters are the authority when they can be reached: they number a
    /// conversation compacted before the contents page existed correctly, where counting the
    /// page's own lines would start again from one. Without a store - a unit test, a host with
    /// no database - the page carries itself forward.
    /// </remarks>
    public static IReadOnlyList<(int Number, string Title)> EarlierTitles(string? hostContent, string? sessionId) =>
        EarlierTitles(hostContent, TryLoadStored(sessionId));

    /// <summary>As above, from chapters already loaded; null means there is no store to ask.</summary>
    public static IReadOnlyList<(int Number, string Title)> EarlierTitles(string? hostContent, IReadOnlyList<ConversationChapter>? stored)
    {
        var replaced = CompressionSupport.ExtractSummary(hostContent);

        if (stored != null)
        {
            var contents = stored.Select(chapter => chapter.Content).ToList();

            // The summary being replaced is normally the last one stored, written when the
            // conversation was saved after its compaction. A turn that compacts twice between
            // saves has not stored it yet.
            if (replaced != null && (contents.Count == 0 || !string.Equals(contents[^1].Trim(), replaced.Trim(), StringComparison.Ordinal)))
            {
                contents.Add(replaced);
            }

            return contents.Select((content, index) => (index + 1, TitleOf(content))).ToList();
        }

        var carried = ParseIndex(hostContent).ToList();
        if (replaced != null)
        {
            carried.Add(((carried.Count == 0 ? 0 : carried[^1].Number) + 1, TitleOf(replaced)));
        }

        return carried;
    }

    /// <summary>The new summary with the contents page of earlier chapters under it.</summary>
    /// <param name="toolOffered">
    /// Whether this chat can call <see cref="ToolId"/>. A contents page that tells the model
    /// to use a tool it was never given sends it looking for something that is not there.
    /// </param>
    public static string WithIndex(string summary, IReadOnlyList<(int Number, string Title)> earlier, bool toolOffered = true)
    {
        if (earlier == null || earlier.Count == 0)
        {
            return summary;
        }

        var builder = new StringBuilder(summary.TrimEnd());
        builder.AppendLine();
        builder.AppendLine();
        builder.AppendLine(IndexHeader);
        builder.AppendLine(
            $"The summary above is chapter {earlier[^1].Number + 1}: it covers only the stretch compacted most recently. " +
            $"Each earlier stretch has its own summary, kept in full outside this context. " +
            (toolOffered
                ? $"Read one with the {ToolId} tool (action \"read\" and its chapter number) when you need what happened then."
                : "This chat has no tool for reading them; the person can open them under Summaries beside the chat, so say so rather than guessing at what they hold."));

        var shown = earlier.Skip(Math.Max(0, earlier.Count - MaxListed)).ToList();
        if (shown.Count < earlier.Count)
        {
            builder.AppendLine($"(chapters {earlier[0].Number}-{shown[0].Number - 1} are not listed here; the tool's \"list\" action shows them)");
        }

        foreach (var (number, title) in shown)
        {
            builder.AppendLine($"{number}. {title}");
        }

        return builder.ToString().TrimEnd();
    }

    /// <summary>A summary block with any contents page cut off it: the chapter's own text.</summary>
    public static string StripIndex(string summaryBlock)
    {
        if (string.IsNullOrEmpty(summaryBlock))
        {
            return summaryBlock ?? string.Empty;
        }

        var index = summaryBlock.IndexOf(IndexHeader, StringComparison.Ordinal);
        return index < 0 ? summaryBlock : summaryBlock[..index].TrimEnd();
    }

    /// <summary>The numbered titles on a contents page, or none when the content has no page.</summary>
    public static IEnumerable<(int Number, string Title)> ParseIndex(string? content)
    {
        if (string.IsNullOrEmpty(content))
        {
            yield break;
        }

        var start = content.IndexOf(IndexHeader, StringComparison.Ordinal);
        if (start < 0)
        {
            yield break;
        }

        foreach (var line in content[(start + IndexHeader.Length)..].Split('\n'))
        {
            var match = IndexLineRegex().Match(line.TrimEnd('\r'));
            if (match.Success && int.TryParse(match.Groups[1].Value, out var number))
            {
                yield return (number, match.Groups[2].Value);
            }
        }
    }

    /// <summary>
    /// What a chapter is about, in one line: its first line that says something. The header
    /// and the token-count line every summary opens with say nothing about its content.
    /// </summary>
    public static string TitleOf(string? summary)
    {
        foreach (var raw in (summary ?? string.Empty).Split('\n'))
        {
            var line = raw.Trim().TrimStart('-', '*', '•', ' ').Trim();
            if (line.Length == 0
                || line.StartsWith('[')
                || line.StartsWith("Estimated tokens", StringComparison.OrdinalIgnoreCase)
                || line.StartsWith("TOK ", StringComparison.Ordinal)
                || line.StartsWith("Recent messages", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            return CompressionSupport.Truncate(line, MaxTitleLength);
        }

        return "(no text)";
    }

    /// <summary>Every chapter stored for a conversation, oldest first.</summary>
    public static IReadOnlyList<ConversationChapter> LoadForState(string stateId, ConfigDatabase? database = null)
    {
        var rows = (database ?? ConfigDatabase.Instance).LoadConversationSummaries(stateId);
        return rows
            .Select((row, index) =>
            {
                var content = row["Content"] as string ?? string.Empty;
                return new ConversationChapter(index + 1, TitleOf(content), content, row["CreatedAt"] as string ?? string.Empty);
            })
            .ToList();
    }

    /// <summary>Every chapter stored for the conversation a session belongs to, or null when the session has no stored conversation.</summary>
    public static IReadOnlyList<ConversationChapter>? LoadForSession(string? sessionId, ConfigDatabase? database = null)
    {
        if (string.IsNullOrWhiteSpace(sessionId))
        {
            return null;
        }

        var source = database ?? ConfigDatabase.Instance;
        var stateId = source.FindStateIdBySession(sessionId);
        return stateId == null ? null : LoadForState(stateId, source);
    }

    /// <summary>
    /// Stores a chapter the moment it is written, rather than leaving it to the next save.
    /// </summary>
    /// <remarks>
    /// A chapter used to reach the store only when the whole conversation was next saved by
    /// whoever was running the turn, and that is at the end of the turn. A long agentic turn
    /// compacts in its first minutes and runs for another hour; until it ended there was no
    /// chapter to list, no node in the tree, and - if the turn was stopped or the app closed
    /// first - no chapter ever. The later save finds the same text already stored and adds
    /// nothing.
    /// </remarks>
    /// <returns>True when a new chapter was stored.</returns>
    public static bool TryStore(string? sessionId, string? summary, ConfigDatabase? database = null)
    {
        if (string.IsNullOrWhiteSpace(sessionId) || string.IsNullOrWhiteSpace(summary))
        {
            return false;
        }

        try
        {
            var source = database ?? ConfigDatabase.Instance;
            var stateId = source.FindStateIdBySession(sessionId);
            if (stateId == null)
            {
                AutomatonDebug.Log($"ConversationChapters.TryStore: session {sessionId} has no stored conversation yet; the chapter will be stored with its first save");
                return false;
            }

            // Trimmed the way the save path reads it back out of the system message, so the
            // two are the same text and the save does not store it a second time.
            var stored = source.RecordConversationSummary(stateId, StripIndex(summary).Trim());
            AutomatonDebug.Log($"ConversationChapters.TryStore: stateId={stateId}, stored={stored}");
            return stored;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ConversationChapters.TryStore: could not store the chapter for session {sessionId}: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// The store, read without letting it break a compaction: a summary with a contents page
    /// that carried itself forward is far better than a turn that failed on a database lock.
    /// </summary>
    private static IReadOnlyList<ConversationChapter>? TryLoadStored(string? sessionId)
    {
        try
        {
            return LoadForSession(sessionId);
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ConversationChapters.TryLoadStored: could not read the stored summaries for session {sessionId}: {ex.Message}");
            return null;
        }
    }
}
