using System;
using System.Collections.Generic;
using System.Linq;
using Automaton.Core.Models.Goals;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services.Goals;

/// <summary>A journal entry found by searching, with the goal it belongs to.</summary>
public sealed class GoalJournalHit
{
    public GoalJournalEntry Entry { get; set; } = new();
    public string GoalId { get; set; } = string.Empty;
    public string GoalTitle { get; set; } = string.Empty;
    public string ProjectId { get; set; } = string.Empty;

    /// <summary>How many of the query's meaningful terms the entry contains.</summary>
    public int TermsMatched { get; set; }
}

/// <summary>
/// Goals as memory (GOALSUPDATE.md §10): which conversations worked on a goal, and what was
/// decided along the way. A decision is recorded once, on purpose, in a sentence or two, so it
/// answers "why did we choose X" better than the chat around it does - and it outlives the chat.
/// </summary>
public sealed partial class GoalTrackingService
{
    /// <summary>
    /// The session ids of the conversations that have touched a goal. The goal is resolved
    /// inside the caller's project first, so a goal id from another project is "not found" and
    /// no search is run for it.
    /// </summary>
    public HashSet<string> LinkedSessionIds(GoalCaller caller, string goalId)
    {
        return ListConversations(caller, goalId)
            .Select(link => link.StateId)
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .ToHashSet(StringComparer.Ordinal);
    }

    /// <summary>
    /// Journal entries written on purpose - decisions, attempts and notes - that contain the
    /// query's meaningful terms, best match first. The system's own records (status changes,
    /// gate refusals, loop and worker rows) are not memory and are never returned.
    /// </summary>
    /// <param name="projectId">The project to search, or null for every project.</param>
    /// <param name="goalId">Optionally one goal. It must already have been resolved by the caller.</param>
    public List<GoalJournalHit> SearchJournal(string? projectId, string query, string? goalId = null, int max = 5)
    {
        var terms = ConversationSearchService.MeaningfulTerms(query ?? string.Empty)
            .Select(term => term.Trim().Trim('"', '*').ToLowerInvariant())
            // MeaningfulTerms hands back the stopwords when a query is made of nothing else. For
            // full-text search that is a fair last resort; here it would match every entry.
            .Where(term => term.Length >= 3 && !ConversationSearchService.IsStopWord(term))
            .Distinct(StringComparer.Ordinal)
            .Take(8)
            .ToList();

        if (terms.Count == 0)
        {
            AutomatonDebug.Log("GoalTrackingService.SearchJournal: Exit - the query has no meaningful terms");
            return new List<GoalJournalHit>();
        }

        var parameters = new List<SqliteParameter>();
        var matches = new List<string>();
        for (var i = 0; i < terms.Count; i++)
        {
            matches.Add($"(CASE WHEN instr(lower(j.Content), @Term{i}) > 0 THEN 1 ELSE 0 END)");
            parameters.Add(P($"@Term{i}", terms[i]));
        }

        var score = string.Join(" + ", matches);
        var sql = $@"
            SELECT j.*, g.Title AS GoalTitle, g.Description AS GoalDescription, g.ProjectId AS GoalProjectId, ({score}) AS TermsMatched
            FROM GoalJournal j INNER JOIN Goals g ON g.Id = j.GoalId
            WHERE j.Kind IN ('Decision', 'Attempt', 'Note') AND ({score}) > 0";

        if (!string.IsNullOrWhiteSpace(projectId))
        {
            sql += " AND g.ProjectId = @ProjectId";
            parameters.Add(P("@ProjectId", projectId));
        }

        if (!string.IsNullOrWhiteSpace(goalId))
        {
            sql += " AND j.GoalId = @GoalId";
            parameters.Add(P("@GoalId", goalId));
        }

        // Decisions before attempts before notes when the terms tie: a decision is the answer
        // to "why", the others are the story around it.
        sql += @"
            ORDER BY TermsMatched DESC,
                     CASE j.Kind WHEN 'Decision' THEN 0 WHEN 'Attempt' THEN 1 ELSE 2 END,
                     j.Id DESC
            LIMIT @Max";
        parameters.Add(P("@Max", Math.Clamp(max, 1, 25)));

        var hits = _db.Query(sql, parameters.ToArray()).Select(row =>
        {
            var title = row["GoalTitle"] as string;
            return new GoalJournalHit
            {
                GoalId = row["GoalId"] as string ?? string.Empty,
                GoalTitle = string.IsNullOrWhiteSpace(title) ? DeriveTitle(row["GoalDescription"] as string ?? string.Empty) : title!,
                ProjectId = row["GoalProjectId"] as string ?? string.Empty,
                TermsMatched = (int)ToLong(row["TermsMatched"]),
                Entry = new GoalJournalEntry
                {
                    Id = ToLong(row["Id"]),
                    GoalId = row["GoalId"] as string ?? string.Empty,
                    TaskId = row["TaskId"] as string,
                    Kind = row["Kind"] as string ?? GoalJournalKinds.Note,
                    Content = row["Content"] as string ?? string.Empty,
                    StateId = row["StateId"] as string,
                    Author = ParseEnum(row["Author"], GoalAuthor.System),
                    CreatedAt = ParseTimestamp(row["CreatedAt"]) ?? DateTime.UtcNow
                }
            };
        }).ToList();

        AutomatonDebug.Log($"GoalTrackingService.SearchJournal: project={projectId ?? "*"}, goal={goalId ?? "*"}, terms=[{string.Join(",", terms)}], hits={hits.Count}");
        return hits;
    }
}
