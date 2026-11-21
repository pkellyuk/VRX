using System.Text.Json;
using Microsoft.Data.Sqlite;
using Automaton.Core.Models;

namespace Automaton.Core.Services;

/// <summary>
/// Read-only access to stored conversations, for surfaces that browse history rather than
/// resume it (the desktop conversation search dialog being the first).
///
/// The search index keys rows by SessionId, but SessionId is NOT unique in ConversationState:
/// clearing a chat archives the old row under an "archived/{guid}" Id while the live row keeps
/// the same SessionId, so one indexed conversation can map to several stored rows. Every lookup
/// here therefore resolves a SessionId to the single best candidate row rather than assuming one.
/// </summary>
public static class ConversationHistoryReader
{
    /// <summary>
    /// Loads the full transcript for an indexed conversation.
    /// When <paramref name="hitText"/> is supplied it is used to disambiguate between stored
    /// rows sharing this SessionId — the row that actually contains the matched text wins,
    /// which is what the search dialog needs to show the message the user clicked.
    /// Returns null when nothing is stored for the SessionId any more.
    /// </summary>
    public static ConversationTranscript? LoadTranscript(string sessionId, string? hitText = null)
    {
        AutomatonDebug.Log($"ConversationHistoryReader.LoadTranscript: Entry - sessionId='{sessionId}', hitTextLength={hitText?.Length ?? 0}");

        if (string.IsNullOrWhiteSpace(sessionId))
        {
            AutomatonDebug.Log("ConversationHistoryReader.LoadTranscript: Exit - blank sessionId");
            return null;
        }

        try
        {
            var candidates = LoadCandidateRows(sessionId);
            if (candidates.Count == 0)
            {
                AutomatonDebug.Log($"ConversationHistoryReader.LoadTranscript: Exit - no stored rows for sessionId='{sessionId}'");
                return null;
            }

            AutomatonDebug.Log($"ConversationHistoryReader.LoadTranscript: {candidates.Count} candidate row(s) for sessionId='{sessionId}'");

            ConversationTranscript? best = null;

            foreach (var candidate in candidates)
            {
                var transcript = BuildTranscript(sessionId, candidate);
                if (transcript.Messages.Count == 0)
                {
                    continue;
                }

                if (!string.IsNullOrWhiteSpace(hitText) && ContainsHit(transcript, hitText))
                {
                    AutomatonDebug.Log($"ConversationHistoryReader.LoadTranscript: Exiting - matched hit text in stateId='{transcript.StateId}' with {transcript.Messages.Count} message(s)");
                    return transcript;
                }

                if (best == null || transcript.Messages.Count > best.Messages.Count)
                {
                    best = transcript;
                }
            }

            if (best == null)
            {
                AutomatonDebug.Log($"ConversationHistoryReader.LoadTranscript: Exit - candidates held no messages for sessionId='{sessionId}'");
                return null;
            }

            AutomatonDebug.Log($"ConversationHistoryReader.LoadTranscript: Exiting - fell back to stateId='{best.StateId}' with {best.Messages.Count} message(s)");
            return best;
        }
        catch (Exception ex)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR,
                $"[ConversationHistoryReader] LoadTranscript failed for '{sessionId}': {ex.Message}");
            return null;
        }
    }

    /// <summary>
    /// Loads display labels for a set of indexed conversations, keyed by SessionId.
    /// Used to title the grouped hit list without loading every transcript. SessionIds with
    /// nothing stored are simply absent from the result.
    /// </summary>
    public static Dictionary<string, ConversationSummary> LoadSummaries(IReadOnlyCollection<string> sessionIds)
    {
        AutomatonDebug.Log($"ConversationHistoryReader.LoadSummaries: Entry - sessionIds={sessionIds?.Count ?? 0}");

        var summaries = new Dictionary<string, ConversationSummary>(StringComparer.Ordinal);

        if (sessionIds == null || sessionIds.Count == 0)
        {
            AutomatonDebug.Log("ConversationHistoryReader.LoadSummaries: Exit - nothing requested");
            return summaries;
        }

        try
        {
            var distinct = sessionIds
                .Where(id => !string.IsNullOrWhiteSpace(id))
                .Distinct(StringComparer.Ordinal)
                .ToList();

            if (distinct.Count == 0)
            {
                AutomatonDebug.Log("ConversationHistoryReader.LoadSummaries: Exit - all requested ids were blank");
                return summaries;
            }

            var db = ConfigDatabase.Instance;
            var (placeholders, parameters) = BuildInClause(distinct, "s");

            // One row per stored candidate, with its message count so duplicates sharing a
            // SessionId can be reduced to the row holding the real conversation.
            var rows = db.Query($@"
                SELECT
                    s.Id,
                    s.SessionId,
                    s.ProjectId,
                    s.UpdatedAt,
                    p.Name AS ProjectName,
                    MAX((SELECT COUNT(*) FROM ConversationMessages m WHERE m.StateId = s.Id),
                        (SELECT COUNT(*) FROM ConversationMessagesFull f WHERE f.StateId = s.Id)) AS MessageCount
                FROM ConversationState s
                LEFT JOIN Projects p ON p.Id = s.ProjectId
                WHERE s.SessionId IN ({placeholders})",
                parameters.ToArray());

            AutomatonDebug.Log($"ConversationHistoryReader.LoadSummaries: {rows.Count} candidate row(s) for {distinct.Count} sessionId(s)");

            foreach (var row in rows)
            {
                var sessionId = row["SessionId"] as string;
                var stateId = row["Id"] as string;

                if (string.IsNullOrWhiteSpace(sessionId) || string.IsNullOrWhiteSpace(stateId))
                {
                    continue;
                }

                var messageCount = (int)ToLong(row["MessageCount"]);

                var summary = new ConversationSummary
                {
                    SessionId = sessionId,
                    StateId = stateId,
                    ProjectId = row["ProjectId"] as string,
                    ProjectName = row["ProjectName"] as string,
                    UpdatedAt = row["UpdatedAt"] as string,
                    MessageCount = messageCount,
                    IsArchived = IsArchivedStateId(stateId)
                };

                if (!summaries.TryGetValue(sessionId, out var existing) || summary.MessageCount > existing.MessageCount)
                {
                    summaries[sessionId] = summary;
                }
            }

            ApplyTitles(db, summaries);

            AutomatonDebug.Log($"ConversationHistoryReader.LoadSummaries: Exiting - resolved {summaries.Count} summary/summaries");
            return summaries;
        }
        catch (Exception ex)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR,
                $"[ConversationHistoryReader] LoadSummaries failed: {ex.Message}");
            return summaries;
        }
    }

    /// <summary>
    /// Fills in each summary's <see cref="ConversationSummary.Title"/> from the first user
    /// message of its chosen state row, falling back to a label derived from the state Id.
    /// </summary>
    private static void ApplyTitles(ConfigDatabase db, Dictionary<string, ConversationSummary> summaries)
    {
        AutomatonDebug.Log($"ConversationHistoryReader.ApplyTitles: Entry - summaries={summaries?.Count ?? 0}");

        if (summaries == null || summaries.Count == 0)
        {
            AutomatonDebug.Log("ConversationHistoryReader.ApplyTitles: Exit - nothing to title");
            return;
        }

        var stateIds = summaries.Values
            .Select(s => s.StateId)
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .Distinct(StringComparer.Ordinal)
            .ToList();

        var firstUserMessages = new Dictionary<string, string>(StringComparer.Ordinal);

        if (stateIds.Count > 0)
        {
            try
            {
                var (placeholders, parameters) = BuildInClause(stateIds, "t");

                var rows = db.Query($@"
                    SELECT StateId, Content
                    FROM ConversationMessages
                    WHERE StateId IN ({placeholders})
                      AND Role = 'user'
                      AND MessageIndex = (
                          SELECT MIN(mi.MessageIndex)
                          FROM ConversationMessages mi
                          WHERE mi.StateId = ConversationMessages.StateId
                            AND mi.Role = 'user')",
                    parameters.ToArray());

                foreach (var row in rows)
                {
                    var stateId = row["StateId"] as string;
                    var content = row["Content"] as string;

                    if (string.IsNullOrWhiteSpace(stateId) || string.IsNullOrWhiteSpace(content))
                    {
                        continue;
                    }

                    firstUserMessages[stateId] = content;
                }

                AutomatonDebug.Log($"ConversationHistoryReader.ApplyTitles: Loaded {firstUserMessages.Count} first-user-message title(s)");
            }
            catch (Exception ex)
            {
                AutomatonDebug.Log(AutomatonDebug.DEBUG_WARNING,
                    $"[ConversationHistoryReader] ApplyTitles query failed: {ex.Message}");
            }
        }

        foreach (var summary in summaries.Values)
        {
            if (firstUserMessages.TryGetValue(summary.StateId, out var firstMessage))
            {
                summary.Title = BuildTitleFromText(firstMessage);
                continue;
            }

            summary.Title = BuildTitleFromStateId(summary.StateId);
        }

        AutomatonDebug.Log("ConversationHistoryReader.ApplyTitles: Exiting");
    }

    /// <summary>
    /// Loads the stored rows sharing a SessionId, newest first.
    /// </summary>
    private static List<ConversationCandidate> LoadCandidateRows(string sessionId)
    {
        var db = ConfigDatabase.Instance;

        var rows = db.Query(@"
            SELECT s.Id, s.ProjectId, s.UpdatedAt, s.State, p.Name AS ProjectName
            FROM ConversationState s
            LEFT JOIN Projects p ON p.Id = s.ProjectId
            WHERE s.SessionId = @SessionId
            ORDER BY s.UpdatedAt DESC",
            new SqliteParameter("@SessionId", sessionId));

        var candidates = new List<ConversationCandidate>();

        foreach (var row in rows)
        {
            var stateId = row["Id"] as string;
            if (string.IsNullOrWhiteSpace(stateId))
            {
                continue;
            }

            candidates.Add(new ConversationCandidate(
                stateId,
                row["ProjectId"] as string,
                row["ProjectName"] as string,
                row["UpdatedAt"] as string,
                row["State"] as string));
        }

        return candidates;
    }

    /// <summary>
    /// Builds a transcript from one candidate row, preferring the normalized message table
    /// and falling back to the legacy State JSON blob for rows written before the migration.
    /// </summary>
    private static ConversationTranscript BuildTranscript(string sessionId, ConversationCandidate candidate)
    {
        var transcript = new ConversationTranscript
        {
            SessionId = sessionId,
            StateId = candidate.StateId,
            ProjectId = candidate.ProjectId,
            ProjectName = candidate.ProjectName,
            UpdatedAt = candidate.UpdatedAt,
            IsArchived = IsArchivedStateId(candidate.StateId)
        };

        // The full transcript, not the model's context: every message ever saved, then every
        // compaction summary, in the order the index counts positions in. A message compaction
        // dropped is therefore still readable here, beside the summary that replaced it.
        var db = ConfigDatabase.Instance;
        var rows = db.LoadFullTranscript(candidate.StateId);

        if (rows.Count > 0)
        {
            var index = 0;
            foreach (var row in rows)
            {
                transcript.Messages.Add(new ConversationTranscriptMessage
                {
                    Index = index++,
                    MessageId = row.TryGetValue("MessageId", out var messageId) ? messageId as string ?? string.Empty : string.Empty,
                    Role = row["Role"] as string ?? string.Empty,
                    Content = row["Content"] as string ?? string.Empty,
                    Origin = Search.SearchOrigins.Normalize(row.TryGetValue("Origin", out var origin) ? origin as string : null)
                });
            }

            AutomatonDebug.Log($"ConversationHistoryReader.BuildTranscript: stateId='{candidate.StateId}' - {transcript.Messages.Count} row(s), {transcript.Messages.Count(m => m.IsCompressed)} compressed");
            return transcript;
        }

        AppendMessagesFromStateJson(transcript, candidate.StateJson);
        return transcript;
    }

    private static void AppendMessagesFromStateJson(ConversationTranscript transcript, string? stateJson)
    {
        if (string.IsNullOrWhiteSpace(stateJson))
        {
            return;
        }

        try
        {
            var state = JsonSerializer.Deserialize<ConversationState>(stateJson, new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true
            });

            if (state?.Messages == null)
            {
                return;
            }

            var index = 0;
            foreach (var message in state.Messages)
            {
                if (message == null)
                {
                    continue;
                }

                transcript.Messages.Add(new ConversationTranscriptMessage
                {
                    Index = index++,
                    MessageId = message.Id ?? string.Empty,
                    Role = message.Role ?? string.Empty,
                    Content = message.Content ?? string.Empty
                });
            }

            AutomatonDebug.Log($"ConversationHistoryReader.AppendMessagesFromStateJson: Recovered {transcript.Messages.Count} message(s) from legacy JSON for stateId='{transcript.StateId}'");
        }
        catch (Exception ex)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_WARNING,
                $"[ConversationHistoryReader] Failed to read legacy State JSON for '{transcript.StateId}': {ex.Message}");
        }
    }

    /// <summary>
    /// True when one of the transcript's messages carries the searched-for text. The index
    /// truncates very long messages, so a prefix comparison is used as well as containment.
    /// </summary>
    private static bool ContainsHit(ConversationTranscript transcript, string hitText)
    {
        if (transcript == null || string.IsNullOrWhiteSpace(hitText))
        {
            return false;
        }

        foreach (var message in transcript.Messages)
        {
            if (string.IsNullOrEmpty(message.Content))
            {
                continue;
            }

            if (message.Content.Contains(hitText, StringComparison.Ordinal))
            {
                return true;
            }
        }

        return false;
    }

    private static bool IsArchivedStateId(string stateId)
    {
        if (string.IsNullOrWhiteSpace(stateId))
        {
            return false;
        }

        return stateId.StartsWith("archived/", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Condenses message text into a one-line title.
    /// </summary>
    private static string BuildTitleFromText(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return "(empty conversation)";
        }

        var firstLine = text
            .Replace("\r", " ")
            .Replace("\n", " ")
            .Trim();

        while (firstLine.Contains("  ", StringComparison.Ordinal))
        {
            firstLine = firstLine.Replace("  ", " ", StringComparison.Ordinal);
        }

        if (firstLine.Length > 90)
        {
            firstLine = firstLine[..90].TrimEnd() + "...";
        }

        return firstLine;
    }

    /// <summary>
    /// Fallback label for conversations with no user message, derived from the state Id which
    /// encodes its origin ("director/main", "archived/{guid}", "projects\{proj}\{thread}.json").
    /// </summary>
    private static string BuildTitleFromStateId(string stateId)
    {
        if (string.IsNullOrWhiteSpace(stateId))
        {
            return "(untitled conversation)";
        }

        if (stateId.StartsWith("director/", StringComparison.OrdinalIgnoreCase))
        {
            return $"Director session ({stateId["director/".Length..]})";
        }

        if (Workers.BackgroundWorkerInfo.IsWorkerChat(stateId))
        {
            return $"Background worker ({stateId[Workers.BackgroundWorkerInfo.WorkerStatePrefix.Length..]})";
        }

        if (IsArchivedStateId(stateId))
        {
            return "Archived conversation";
        }

        return stateId;
    }

    private static (string Placeholders, List<SqliteParameter> Parameters) BuildInClause(IReadOnlyList<string> values, string prefix)
    {
        var names = new List<string>(values.Count);
        var parameters = new List<SqliteParameter>(values.Count);

        for (var i = 0; i < values.Count; i++)
        {
            var name = $"@{prefix}{i}";
            names.Add(name);
            parameters.Add(new SqliteParameter(name, values[i]));
        }

        return (string.Join(", ", names), parameters);
    }

    private static long ToLong(object? value)
    {
        return value switch
        {
            long l => l,
            int i => i,
            double d => (long)d,
            decimal m => (long)m,
            string s when long.TryParse(s, out var parsed) => parsed,
            _ => 0
        };
    }

    private readonly record struct ConversationCandidate(
        string StateId,
        string? ProjectId,
        string? ProjectName,
        string? UpdatedAt,
        string? StateJson);
}

/// <summary>
/// A stored conversation resolved for reading.
/// </summary>
public class ConversationTranscript
{
    public string SessionId { get; set; } = string.Empty;

    /// <summary>
    /// The ConversationState.Id row this transcript came from.
    /// </summary>
    public string StateId { get; set; } = string.Empty;

    public string? ProjectId { get; set; }

    public string? ProjectName { get; set; }

    public string? UpdatedAt { get; set; }

    /// <summary>
    /// True when the row is an archive left behind by clearing the chat.
    /// </summary>
    public bool IsArchived { get; set; }

    public List<ConversationTranscriptMessage> Messages { get; set; } = new();
}

/// <summary>
/// One message within a <see cref="ConversationTranscript"/>.
/// </summary>
public class ConversationTranscriptMessage
{
    /// <summary>
    /// Zero-based position within the transcript.
    /// </summary>
    public int Index { get; set; }

    /// <summary>
    /// The stored message's id, when there is one. Empty for transcripts recovered from the
    /// legacy state JSON. A search hit found by meaning carries the id of the message its chunk
    /// was cut from, and matching on that is exact where matching on a position is only as good
    /// as the index's age.
    /// </summary>
    public string MessageId { get; set; } = string.Empty;

    public string Role { get; set; } = string.Empty;

    public string Content { get; set; } = string.Empty;

    /// <summary>
    /// <see cref="Search.SearchOrigins.Raw"/> for a message as sent,
    /// <see cref="Search.SearchOrigins.Compressed"/> for a summary compaction wrote.
    /// </summary>
    public string Origin { get; set; } = Search.SearchOrigins.Raw;

    /// <summary>True when this entry is a compaction summary rather than a message.</summary>
    public bool IsCompressed => string.Equals(Origin, Search.SearchOrigins.Compressed, StringComparison.Ordinal);
}

/// <summary>
/// Display label for a stored conversation, without loading its transcript.
/// </summary>
public class ConversationSummary
{
    public string SessionId { get; set; } = string.Empty;

    public string StateId { get; set; } = string.Empty;

    public string? ProjectId { get; set; }

    public string? ProjectName { get; set; }

    public string? UpdatedAt { get; set; }

    public int MessageCount { get; set; }

    public bool IsArchived { get; set; }

    public string Title { get; set; } = string.Empty;
}
