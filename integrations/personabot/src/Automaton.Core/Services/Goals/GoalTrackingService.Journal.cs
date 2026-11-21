using System;
using System.Collections.Generic;
using System.Linq;
using Automaton.Core.Models.Goals;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services.Goals;

public sealed partial class GoalTrackingService
{
    // ── Journal ───────────────────────────────────────────────────────────

    /// <summary>
    /// Appends a note, decision or attempt. The system kinds are written by the service
    /// itself and cannot be forged through here by a model.
    /// </summary>
    public GoalJournalEntry AppendJournal(GoalCaller caller, string goalId, string kind, string content, string? taskRef = null)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (string.IsNullOrWhiteSpace(content))
        {
            throw new GoalRuleException("A journal entry needs some content.");
        }

        var resolvedKind = GoalJournalKinds.ModelWritable.FirstOrDefault(allowed => string.Equals(allowed, kind?.Trim(), StringComparison.OrdinalIgnoreCase));
        if (resolvedKind == null && caller.Author == GoalAuthor.Model)
        {
            throw new GoalRuleException($"Journal kind must be one of: {string.Join(", ", GoalJournalKinds.ModelWritable).ToLowerInvariant()}.");
        }

        resolvedKind ??= string.IsNullOrWhiteSpace(kind) ? GoalJournalKinds.Note : kind.Trim();
        AutomatonDebug.Log($"GoalTrackingService.AppendJournal: Entry - goalId={goalId}, kind={resolvedKind}, length={content.Length}, caller={caller}");

        GoalJournalEntry? entry = null;
        Mutate(caller, goalId, null, context =>
        {
            string? taskId = null;
            if (!string.IsNullOrWhiteSpace(taskRef))
            {
                taskId = ResolveTask(context.Goal, taskRef!).Id;
            }

            var id = InsertJournal(context, resolvedKind, content, taskId);
            entry = new GoalJournalEntry
            {
                Id = id,
                GoalId = context.Goal.Id,
                TaskId = taskId,
                Kind = resolvedKind,
                Content = Cap(content, MaxJournalContent) ?? string.Empty,
                StateId = context.Caller.StateId,
                Author = context.Caller.Author,
                CreatedAt = context.Now
            };
        });

        AutomatonDebug.Log($"GoalTrackingService.AppendJournal: Exit - goalId={goalId}, entryId={entry!.Id}");
        return entry;
    }

    /// <summary>The most recent entries, oldest first. <paramref name="beforeId"/> pages backwards.</summary>
    public List<GoalJournalEntry> ReadJournal(GoalCaller caller, string goalId, int maxEntries = 15, long? beforeId = null)
    {
        var goal = GetGoal(caller, goalId);
        var limit = Math.Clamp(maxEntries, 1, 200);

        var sql = "SELECT * FROM GoalJournal WHERE GoalId = @GoalId";
        var parameters = new List<SqliteParameter> { P("@GoalId", goal.Id), P("@Limit", limit) };
        if (beforeId.HasValue)
        {
            sql += " AND Id < @BeforeId";
            parameters.Add(P("@BeforeId", beforeId.Value));
        }

        sql += " ORDER BY Id DESC LIMIT @Limit";

        var entries = _db.Query(sql, parameters.ToArray())
            .Select(row => new GoalJournalEntry
            {
                Id = ToLong(row["Id"]),
                GoalId = row["GoalId"] as string ?? string.Empty,
                TaskId = row["TaskId"] as string,
                Kind = row["Kind"] as string ?? GoalJournalKinds.Note,
                Content = row["Content"] as string ?? string.Empty,
                StateId = row["StateId"] as string,
                Author = ParseEnum(row["Author"], GoalAuthor.System),
                CreatedAt = ParseTimestamp(row["CreatedAt"]) ?? DateTime.UtcNow
            })
            .OrderBy(entry => entry.Id)
            .ToList();

        AutomatonDebug.Log($"GoalTrackingService.ReadJournal: goalId={goalId}, returned {entries.Count} of max {limit}, beforeId={beforeId?.ToString() ?? "none"}");
        return entries;
    }

    private long InsertJournal(MutationContext context, string kind, string content, string? taskId)
    {
        context.Execute(@"
            INSERT INTO GoalJournal (GoalId, TaskId, Kind, Content, StateId, Author, CreatedAt)
            VALUES (@GoalId, @TaskId, @Kind, @Content, @StateId, @Author, @CreatedAt)",
            P("@GoalId", context.Goal.Id),
            P("@TaskId", taskId),
            P("@Kind", kind),
            P("@Content", Cap(content, MaxJournalContent) ?? string.Empty),
            P("@StateId", context.Caller.StateId),
            P("@Author", context.Caller.Author.ToString()),
            P("@CreatedAt", Iso(context.Now)));

        return ToLong(context.Query("SELECT last_insert_rowid() AS Id")[0]["Id"]);
    }

    // ── History links and the working binding ─────────────────────────────

    /// <summary>
    /// Records that this conversation touched this goal. History only: it never changes which
    /// goal the chat is working on.
    /// </summary>
    public void LinkConversation(GoalCaller caller, string goalId)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (string.IsNullOrWhiteSpace(caller.StateId))
        {
            return;
        }

        var goal = GetGoal(caller, goalId);
        var now = Iso(_clock());
        _db.Execute(@"
            INSERT INTO GoalConversations (GoalId, StateId, FirstAt, LastAt) VALUES (@GoalId, @StateId, @Now, @Now)
            ON CONFLICT(GoalId, StateId) DO UPDATE SET LastAt = @Now",
            P("@GoalId", goal.Id), P("@StateId", caller.StateId), P("@Now", now));
    }

    private static void LinkConversation(MutationContext context)
    {
        if (string.IsNullOrWhiteSpace(context.Caller.StateId))
        {
            return;
        }

        context.Execute(@"
            INSERT INTO GoalConversations (GoalId, StateId, FirstAt, LastAt) VALUES (@GoalId, @StateId, @Now, @Now)
            ON CONFLICT(GoalId, StateId) DO UPDATE SET LastAt = @Now",
            P("@GoalId", context.Goal.Id), P("@StateId", context.Caller.StateId), P("@Now", Iso(context.Now)));
    }

    /// <summary>The conversations that have worked on a goal, most recent first.</summary>
    public List<(string StateId, DateTime FirstAt, DateTime LastAt)> ListConversations(GoalCaller caller, string goalId)
    {
        var goal = GetGoal(caller, goalId);
        return _db.Query("SELECT * FROM GoalConversations WHERE GoalId = @GoalId ORDER BY LastAt DESC", P("@GoalId", goal.Id))
            .Select(row => (
                row["StateId"] as string ?? string.Empty,
                ParseTimestamp(row["FirstAt"]) ?? DateTime.UtcNow,
                ParseTimestamp(row["LastAt"]) ?? DateTime.UtcNow))
            .ToList();
    }

    /// <summary>
    /// Binds a conversation to the goal it is working on. An explicit work action - reading
    /// or listing goals never does this.
    /// </summary>
    public void Bind(GoalCaller caller, string stateId, string goalId)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (string.IsNullOrWhiteSpace(stateId))
        {
            throw new ArgumentException("stateId is required", nameof(stateId));
        }

        AutomatonDebug.Log($"GoalTrackingService.Bind: Entry - stateId={stateId}, goalId={goalId}, caller={caller}");
        Mutate(caller, goalId, null, context => BindInTransaction(context, stateId, context.Goal.Id));
        AutomatonDebug.Log($"GoalTrackingService.Bind: Exit - {stateId} bound to {goalId}");
    }

    private static void BindInTransaction(MutationContext context, string stateId, string goalId)
    {
        context.Execute(@"
            INSERT INTO GoalBindings (StateId, GoalId, BoundAt, BoundBy) VALUES (@StateId, @GoalId, @Now, @By)
            ON CONFLICT(StateId) DO UPDATE SET GoalId = @GoalId, BoundAt = @Now, BoundBy = @By",
            P("@StateId", stateId), P("@GoalId", goalId), P("@Now", Iso(context.Now)), P("@By", context.Caller.Author.ToString()));
    }

    /// <summary>The goal id this conversation is bound to, or null.</summary>
    public string? GetBinding(string stateId)
    {
        if (string.IsNullOrWhiteSpace(stateId))
        {
            return null;
        }

        var rows = _db.Query("SELECT GoalId FROM GoalBindings WHERE StateId = @StateId", P("@StateId", stateId));
        return rows.Count == 0 ? null : rows[0]["GoalId"] as string;
    }

    /// <summary>
    /// The project of the goal a conversation is bound to, or null. How a chat with no project
    /// of its own knows where it is working once it has chosen a goal (GOALSUPDATE.md §24.2).
    /// </summary>
    public string? GetBoundProjectId(string stateId)
    {
        if (string.IsNullOrWhiteSpace(stateId))
        {
            return null;
        }

        var rows = _db.Query(@"
            SELECT g.ProjectId FROM GoalBindings b
            JOIN Goals g ON g.Id = b.GoalId
            WHERE b.StateId = @StateId", P("@StateId", stateId));

        var projectId = rows.Count == 0 ? null : rows[0]["ProjectId"] as string;
        AutomatonDebug.Log($"GoalTrackingService.GetBoundProjectId: stateId={stateId} -> {projectId ?? "(not bound)"}");
        return string.IsNullOrWhiteSpace(projectId) ? null : projectId;
    }

    /// <summary>
    /// The project a goal belongs to, or null when there is no such goal. Only for a chat with
    /// no project, which may already read any project by naming it; a project chat's boundary
    /// is decided by its own project, never by this.
    /// </summary>
    public string? FindProjectOfGoal(string goalId)
    {
        if (string.IsNullOrWhiteSpace(goalId))
        {
            return null;
        }

        var rows = _db.Query("SELECT ProjectId FROM Goals WHERE Id = @Id", P("@Id", goalId.Trim()));
        var projectId = rows.Count == 0 ? null : rows[0]["ProjectId"] as string;
        AutomatonDebug.Log($"GoalTrackingService.FindProjectOfGoal: goalId={goalId} -> {projectId ?? "(no such goal)"}");
        return string.IsNullOrWhiteSpace(projectId) ? null : projectId;
    }

    // ── Cross-project grants (GOALSUPDATE.md §5.0.1) ──────────────────────

    /// <summary>Whether this chat has already been allowed to change goals in this project.</summary>
    public bool HasProjectGrant(string stateId, string projectId)
    {
        if (string.IsNullOrWhiteSpace(stateId) || string.IsNullOrWhiteSpace(projectId))
        {
            return false;
        }

        return _db.Query("SELECT 1 FROM GoalProjectGrants WHERE StateId = @StateId AND ProjectId = @ProjectId",
            P("@StateId", stateId), P("@ProjectId", projectId)).Count > 0;
    }

    public void RecordProjectGrant(string stateId, string projectId)
    {
        if (string.IsNullOrWhiteSpace(stateId) || string.IsNullOrWhiteSpace(projectId))
        {
            AutomatonDebug.LogWarning("GoalTrackingService.RecordProjectGrant: Exit - stateId or projectId is blank");
            return;
        }

        _db.Execute("INSERT OR IGNORE INTO GoalProjectGrants (StateId, ProjectId, GrantedAt) VALUES (@StateId, @ProjectId, @Now)",
            P("@StateId", stateId), P("@ProjectId", projectId), P("@Now", Iso(_clock())));
        AutomatonDebug.Log($"GoalTrackingService.RecordProjectGrant: CROSS-PROJECT GRANT - chat {stateId} may change goals in project {projectId}");
    }
}
