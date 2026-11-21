using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using Automaton.Core.Models;
using Automaton.Core.Models.Goals;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services.Goals;

/// <summary>
/// The one place project goals are read and written. See GOALSUPDATE.md §5.
///
/// <para>
/// Every mutation is one transaction that begins by bumping the goal's <c>Revision</c>. That
/// first write takes SQLite's write lock, so everything validated afterwards is validated
/// against state no other writer can change: the completion gate cannot pass while a task is
/// being added, and two claims on one task cannot both succeed.
/// </para>
/// <para>
/// Every goal id is resolved together with the caller's project. A goal in another project is
/// reported exactly like one that does not exist.
/// </para>
/// </summary>
public sealed partial class GoalTrackingService
{
    public const string RequireEvidenceSetting = "goals.requireEvidence";
    public const string DefaultProofSetting = "goals.defaultProof";
    public const string ReviewModeSetting = "goals.reviewMode";
    public const string ClaimStaleMinutesSetting = "goals.claimStaleMinutes";

    private const int MaxJournalContent = 2000;
    private const int MaxEvidence = 4000;
    private const int MaxTitle = 80;

    private readonly ConfigDatabase _db;
    private readonly Func<DateTime> _clock;

    /// <summary>
    /// Raised after a committed change, with the goal id. Fired on the caller's thread;
    /// UI subscribers must marshal.
    /// </summary>
    public static event Action<string>? GoalChanged;

    public GoalTrackingService(ConfigDatabase? database = null, Func<DateTime>? clock = null)
    {
        _db = database ?? ConfigDatabase.Instance;
        _clock = clock ?? (() => DateTime.UtcNow);
        MigratePending();
        ReconcileAbandonedCheckRuns();
    }

    /// <summary>The database this service reads and writes, for tool metadata and logs.</summary>
    public string DatabasePath => _db.DatabasePath;

    /// <summary>
    /// Whether check commands can be run here. True since check execution shipped
    /// (GOALSUPDATE.md 14, phase 4): an unmet criterion with a check command is then
    /// outstanding until its check passes. A head with no way to run commands can set this
    /// false, which restores the interim rule - such a criterion is confirmed by the human at
    /// the review, like a Human one.
    /// </summary>
    public bool ChecksExecutable { get; set; } = true;

    // ── Reads ─────────────────────────────────────────────────────────────

    /// <summary>The goal with its tasks and criteria. Throws <see cref="GoalNotFoundException"/>.</summary>
    public TrackedGoal GetGoal(GoalCaller caller, string goalId)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (string.IsNullOrWhiteSpace(goalId))
        {
            throw new ArgumentException("goalId is required", nameof(goalId));
        }

        AutomatonDebug.Log($"GoalTrackingService.GetGoal: Entry - goalId={goalId}, caller={caller}");

        var rows = _db.Query(GoalSelect(caller), GoalParameters(caller, goalId));
        if (rows.Count == 0)
        {
            LogBoundary("GetGoal", caller, goalId);
            throw new GoalNotFoundException(goalId);
        }

        var goal = RowToGoal(rows[0]);
        goal.Tasks = _db.Query("SELECT * FROM GoalTasks WHERE GoalId = @GoalId ORDER BY Ordinal ASC", P("@GoalId", goal.Id))
            .Select(RowToTask)
            .ToList();
        goal.Criteria = _db.Query("SELECT * FROM GoalCriteria WHERE GoalId = @GoalId ORDER BY Ordinal ASC", P("@GoalId", goal.Id))
            .Select(RowToCriterion)
            .ToList();

        AutomatonDebug.Log($"GoalTrackingService.GetGoal: Exit - goalId={goal.Id}, revision={goal.Revision}, tasks={goal.Tasks.Count}, criteria={goal.Criteria.Count}");
        return goal;
    }

    /// <summary>
    /// The project's goals with their progress, in the deterministic order of §4.1:
    /// priority, then creation time, then id.
    /// </summary>
    public List<GoalHeader> ListGoals(GoalCaller caller, GoalStatus? status = null)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (string.IsNullOrWhiteSpace(caller.ProjectId))
        {
            throw new ArgumentException("Listing goals needs a project.", nameof(caller));
        }

        AutomatonDebug.Log($"GoalTrackingService.ListGoals: Entry - caller={caller}, status={status?.ToString() ?? "any"}");

        var sql = "SELECT Id FROM Goals WHERE ProjectId = @ProjectId";
        var parameters = new List<SqliteParameter> { P("@ProjectId", caller.ProjectId) };
        if (status.HasValue)
        {
            sql += " AND Status = @Status";
            parameters.Add(P("@Status", (int)status.Value));
        }

        sql += " ORDER BY Priority ASC, CreatedAt ASC, Id ASC";

        var headers = new List<GoalHeader>();
        foreach (var row in _db.Query(sql, parameters.ToArray()))
        {
            var goal = GetGoal(caller, row["Id"] as string ?? string.Empty);
            headers.Add(new GoalHeader { Goal = goal, Progress = GoalProgress.For(goal) });
        }

        AutomatonDebug.Log($"GoalTrackingService.ListGoals: Exit - {headers.Count} goal(s)");
        return headers;
    }

    // ── Goal-level writes ─────────────────────────────────────────────────

    public TrackedGoal CreateGoal(GoalCaller caller, GoalDraft draft)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (draft == null)
        {
            throw new ArgumentNullException(nameof(draft));
        }

        if (string.IsNullOrWhiteSpace(caller.ProjectId))
        {
            throw new ArgumentException("Creating a goal needs a project.", nameof(caller));
        }

        if (string.IsNullOrWhiteSpace(draft.Description))
        {
            throw new GoalRuleException("A goal needs a description.");
        }

        if (draft.Status == GoalStatus.Completed || draft.Status == GoalStatus.AwaitingReview)
        {
            throw new GoalRuleException("A goal cannot be created already completed. Create it, then complete it.");
        }

        AutomatonDebug.Log($"GoalTrackingService.CreateGoal: Entry - caller={caller}, tasks={draft.Tasks.Count}, criteria={draft.Criteria.Count}");

        var defaultProof = DefaultProofFor(caller.ProjectId);
        var goalId = string.IsNullOrWhiteSpace(draft.Id) ? Guid.NewGuid().ToString() : draft.Id!.Trim();
        var now = _clock();
        var description = draft.Description.Trim();
        var title = string.IsNullOrWhiteSpace(draft.Title) ? DeriveTitle(description) : Truncate(draft.Title!.Trim(), MaxTitle);

        _db.ExecuteInTransaction((connection, transaction) =>
        {
            ConfigDatabase.ExecuteOnConnection(connection, transaction, @"
                INSERT INTO Goals
                    (Id, ProjectId, Description, WorkingFolder, DownloadedDocs, SuccessCriteria, Milestones, Plan,
                     Status, CreatedAt, UpdatedAt, Title, Priority, Revision, WorkRevision,
                     NextTaskNumber, NextCriterionNumber, MigrationVersion)
                VALUES
                    (@Id, @ProjectId, @Description, @WorkingFolder, @DownloadedDocs, '[]', '[]', '[]',
                     @Status, @CreatedAt, @UpdatedAt, @Title, 0, 1, 1, 1, 1, @MigrationVersion)",
                P("@Id", goalId),
                P("@ProjectId", caller.ProjectId),
                P("@Description", description),
                P("@WorkingFolder", draft.WorkingFolder?.Trim() ?? string.Empty),
                P("@DownloadedDocs", string.IsNullOrWhiteSpace(draft.DownloadedDocsJson) ? "[]" : draft.DownloadedDocsJson),
                P("@Status", (int)draft.Status),
                P("@CreatedAt", Iso(draft.CreatedAt ?? now)),
                P("@UpdatedAt", Iso(now)),
                P("@Title", title),
                P("@MigrationVersion", CurrentMigrationVersion));

            var context = new MutationContext(this, connection, transaction, caller, now)
            {
                Goal = new TrackedGoal { Id = goalId, ProjectId = caller.ProjectId! }
            };

            var ordinal = 0;
            var insertedByDraft = new Dictionary<int, GoalTask>();
            for (var index = 0; index < draft.Tasks.Count; index++)
            {
                var task = draft.Tasks[index];
                if (task == null || string.IsNullOrWhiteSpace(task.Description))
                {
                    continue;
                }

                ordinal++;
                var inserted = InsertTask(context, task.Description, ordinal, new List<string>());
                context.Goal.Tasks.Add(inserted);
                insertedByDraft[index] = inserted;
            }

            // Dependencies are validated once every task exists, so a draft may refer forwards.
            ApplyDraftDependencies(context, draft.Tasks, insertedByDraft);

            var criterionOrdinal = 0;
            foreach (var criterion in draft.Criteria)
            {
                if (criterion == null || string.IsNullOrWhiteSpace(criterion.Description))
                {
                    continue;
                }

                criterionOrdinal++;
                InsertCriterion(context, criterion, criterionOrdinal, defaultProof, caller.Author);
            }

            context.Journal(GoalJournalKinds.StatusChange, $"goal created as {draft.Status} with {ordinal} task(s) and {criterionOrdinal} criteria");
            LinkConversation(context);
        });

        RaiseChanged(goalId);
        var created = GetGoal(caller, goalId);
        AutomatonDebug.Log($"GoalTrackingService.CreateGoal: Exit - goalId={goalId}, tasks={created.Tasks.Count}, criteria={created.Criteria.Count}");
        return created;
    }

    /// <summary>
    /// Writes only the fields that are set. With <paramref name="expectedRevision"/>, a goal
    /// that has moved on is a conflict rather than an overwrite.
    /// </summary>
    public TrackedGoal UpdateGoalFields(GoalCaller caller, string goalId, GoalFieldChanges changes, long? expectedRevision = null)
    {
        if (changes == null)
        {
            throw new ArgumentNullException(nameof(changes));
        }

        if (changes.IsEmpty)
        {
            AutomatonDebug.Log($"GoalTrackingService.UpdateGoalFields: Exit - nothing to change on {goalId}");
            return GetGoal(caller, goalId);
        }

        AutomatonDebug.Log($"GoalTrackingService.UpdateGoalFields: Entry - goalId={goalId}, caller={caller}, expectedRevision={expectedRevision?.ToString() ?? "none"}");

        Mutate(caller, goalId, expectedRevision, context =>
        {
            var written = new List<string>();

            if (changes.Description != null)
            {
                if (string.IsNullOrWhiteSpace(changes.Description))
                {
                    throw new GoalRuleException("A goal needs a description.");
                }

                context.Execute("UPDATE Goals SET Description = @Value WHERE Id = @Id", P("@Value", changes.Description.Trim()), P("@Id", context.Goal.Id));
                written.Add("description");

                // A title nobody set follows the description it was derived from.
                if (changes.Title == null && string.Equals(context.Goal.Title, DeriveTitle(context.Goal.Description), StringComparison.Ordinal))
                {
                    context.Execute("UPDATE Goals SET Title = @Value WHERE Id = @Id", P("@Value", DeriveTitle(changes.Description.Trim())), P("@Id", context.Goal.Id));
                }
            }

            if (changes.Title != null)
            {
                context.Execute("UPDATE Goals SET Title = @Value WHERE Id = @Id", P("@Value", Truncate(changes.Title.Trim(), MaxTitle)), P("@Id", context.Goal.Id));
                written.Add("title");
            }

            if (changes.WorkingFolder != null && !string.Equals(changes.WorkingFolder.Trim(), context.Goal.WorkingFolder, StringComparison.Ordinal))
            {
                context.Execute("UPDATE Goals SET WorkingFolder = @Value WHERE Id = @Id", P("@Value", changes.WorkingFolder.Trim()), P("@Id", context.Goal.Id));
                context.BumpWork();
                written.Add("workingFolder");
            }

            if (changes.DownloadedDocsJson != null)
            {
                context.Execute("UPDATE Goals SET DownloadedDocs = @Value WHERE Id = @Id", P("@Value", changes.DownloadedDocsJson), P("@Id", context.Goal.Id));
                written.Add("downloadedDocs");
            }

            if (changes.Priority.HasValue)
            {
                context.Execute("UPDATE Goals SET Priority = @Value WHERE Id = @Id", P("@Value", changes.Priority.Value), P("@Id", context.Goal.Id));
                written.Add("priority");
            }

            AutomatonDebug.Log($"GoalTrackingService.UpdateGoalFields: wrote [{string.Join(", ", written)}] on {context.Goal.Id}");
        });

        var updated = GetGoal(caller, goalId);
        AutomatonDebug.Log($"GoalTrackingService.UpdateGoalFields: Exit - goalId={goalId}, revision={updated.Revision}");
        return updated;
    }

    /// <summary>Deletes the goal and, by cascade, everything that hangs off it.</summary>
    public void DeleteGoal(GoalCaller caller, string goalId)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (string.IsNullOrWhiteSpace(goalId))
        {
            throw new ArgumentException("goalId is required", nameof(goalId));
        }

        AutomatonDebug.Log($"GoalTrackingService.DeleteGoal: Entry - goalId={goalId}, caller={caller}");

        var deleted = 0;
        _db.ExecuteInTransaction((connection, transaction) =>
        {
            var sql = string.IsNullOrWhiteSpace(caller.ProjectId)
                ? "DELETE FROM Goals WHERE Id = @Id"
                : "DELETE FROM Goals WHERE Id = @Id AND ProjectId = @ProjectId";
            deleted = ConfigDatabase.ExecuteOnConnection(connection, transaction, sql, GoalParameters(caller, goalId));
        });

        if (deleted == 0)
        {
            LogBoundary("DeleteGoal", caller, goalId);
            throw new GoalNotFoundException(goalId);
        }

        RaiseChanged(goalId);
        AutomatonDebug.Log($"GoalTrackingService.DeleteGoal: Exit - goalId={goalId} deleted");
    }

    // ── The mutation frame ────────────────────────────────────────────────

    /// <summary>
    /// Runs <paramref name="body"/> inside one transaction against a goal that has been
    /// resolved within the caller's project and locked by the revision bump.
    /// A refusal that must leave a journal row behind is set on the context and thrown
    /// after the commit; any other exception rolls the whole operation back.
    /// </summary>
    private void Mutate(GoalCaller caller, string goalId, long? expectedRevision, Action<MutationContext> body)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (string.IsNullOrWhiteSpace(goalId))
        {
            throw new ArgumentException("goalId is required", nameof(goalId));
        }

        if (body == null)
        {
            throw new ArgumentNullException(nameof(body));
        }

        Exception? deferred = null;
        var now = _clock();

        _db.ExecuteInTransaction((connection, transaction) =>
        {
            // First statement is a write: it takes the write lock before anything is read.
            var bumpSql = string.IsNullOrWhiteSpace(caller.ProjectId)
                ? "UPDATE Goals SET Revision = Revision + 1, UpdatedAt = @Now WHERE Id = @Id"
                : "UPDATE Goals SET Revision = Revision + 1, UpdatedAt = @Now WHERE Id = @Id AND ProjectId = @ProjectId";
            var bumpParameters = GoalParameters(caller, goalId).Append(P("@Now", Iso(now))).ToArray();

            if (ConfigDatabase.ExecuteOnConnection(connection, transaction, bumpSql, bumpParameters) == 0)
            {
                // Same connection on purpose: config.db uses a shared cache, and a second
                // connection opened here would wait out the command timeout on its own lock.
                var owner = ConfigDatabase.QueryOnConnection(connection, transaction, "SELECT ProjectId FROM Goals WHERE Id = @Id", P("@Id", goalId));
                LogBoundary("Mutate", caller, goalId, owner);
                throw new GoalNotFoundException(goalId);
            }

            var context = new MutationContext(this, connection, transaction, caller, now);
            context.Goal = LoadGoal(context, goalId);

            if (expectedRevision.HasValue && context.Goal.Revision - 1 != expectedRevision.Value)
            {
                AutomatonDebug.LogWarning($"GoalTrackingService.Mutate: conflict on {goalId} - expected revision {expectedRevision.Value}, found {context.Goal.Revision - 1}");
                throw new GoalConflictException($"Goal {goalId} was changed by someone else (expected revision {expectedRevision.Value}, found {context.Goal.Revision - 1}). Reload it and try again.");
            }

            body(context);
            LinkConversation(context);
            deferred = context.DeferredRefusal;
        });

        RaiseChanged(goalId);

        if (deferred != null)
        {
            throw deferred;
        }
    }

    private TrackedGoal LoadGoal(MutationContext context, string goalId)
    {
        var rows = context.Query("SELECT * FROM Goals WHERE Id = @Id", P("@Id", goalId));
        if (rows.Count == 0)
        {
            throw new GoalNotFoundException(goalId);
        }

        var goal = RowToGoal(rows[0]);
        goal.Tasks = context.Query("SELECT * FROM GoalTasks WHERE GoalId = @GoalId ORDER BY Ordinal ASC", P("@GoalId", goalId)).Select(RowToTask).ToList();
        goal.Criteria = context.Query("SELECT * FROM GoalCriteria WHERE GoalId = @GoalId ORDER BY Ordinal ASC", P("@GoalId", goalId)).Select(RowToCriterion).ToList();
        return goal;
    }

    /// <summary>State shared by the statements of one mutation.</summary>
    private sealed class MutationContext
    {
        private readonly GoalTrackingService _owner;
        private bool _workBumped;

        public MutationContext(GoalTrackingService owner, SqliteConnection connection, SqliteTransaction transaction, GoalCaller caller, DateTime now)
        {
            _owner = owner;
            Connection = connection;
            Transaction = transaction;
            Caller = caller;
            Now = now;
        }

        public SqliteConnection Connection { get; }
        public SqliteTransaction Transaction { get; }
        public GoalCaller Caller { get; }
        public DateTime Now { get; }
        public TrackedGoal Goal { get; set; } = new();

        /// <summary>A refusal to throw once the transaction, and its journal row, has committed.</summary>
        public Exception? DeferredRefusal { get; set; }

        public int Execute(string sql, params SqliteParameter[] parameters)
        {
            return ConfigDatabase.ExecuteOnConnection(Connection, Transaction, sql, parameters);
        }

        public List<Dictionary<string, object?>> Query(string sql, params SqliteParameter[] parameters)
        {
            return ConfigDatabase.QueryOnConnection(Connection, Transaction, sql, parameters);
        }

        /// <summary>Marks that the work itself changed. At most one increment per operation.</summary>
        public void BumpWork()
        {
            if (_workBumped)
            {
                return;
            }

            _workBumped = true;
            Execute("UPDATE Goals SET WorkRevision = WorkRevision + 1 WHERE Id = @Id", P("@Id", Goal.Id));
            Goal.WorkRevision++;
        }

        public void Journal(string kind, string content, string? taskId = null)
        {
            _owner.InsertJournal(this, kind, content, taskId);
        }
    }

    // ── Shared helpers ────────────────────────────────────────────────────

    private static string GoalSelect(GoalCaller caller)
    {
        return string.IsNullOrWhiteSpace(caller.ProjectId)
            ? "SELECT * FROM Goals WHERE Id = @Id"
            : "SELECT * FROM Goals WHERE Id = @Id AND ProjectId = @ProjectId";
    }

    private static SqliteParameter[] GoalParameters(GoalCaller caller, string goalId)
    {
        if (string.IsNullOrWhiteSpace(caller.ProjectId))
        {
            return new[] { P("@Id", goalId) };
        }

        return new[] { P("@Id", goalId), P("@ProjectId", caller.ProjectId) };
    }

    /// <summary>
    /// A goal id that did not resolve. If it exists in another project that is worth knowing
    /// in the log; it is never told to the caller.
    /// </summary>
    private void LogBoundary(string operation, GoalCaller caller, string goalId, List<Dictionary<string, object?>>? ownerRows = null)
    {
        var rows = ownerRows ?? _db.Query("SELECT ProjectId FROM Goals WHERE Id = @Id", P("@Id", goalId));
        if (rows.Count == 0)
        {
            AutomatonDebug.LogWarning($"GoalTrackingService.{operation}: goal {goalId} does not exist (caller={caller})");
            return;
        }

        AutomatonDebug.LogWarning($"GoalTrackingService.{operation}: PROJECT BOUNDARY - goal {goalId} belongs to project {rows[0]["ProjectId"]}, caller={caller}; reported as not found");
    }

    private void RaiseChanged(string goalId)
    {
        try
        {
            GoalChanged?.Invoke(goalId);
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"GoalTrackingService.RaiseChanged: a subscriber threw for {goalId} - {ex.Message}");
        }
    }

    private static SqliteParameter P(string name, object? value)
    {
        return new SqliteParameter(name, value ?? DBNull.Value);
    }

    private static string Iso(DateTime value)
    {
        var utc = value.Kind == DateTimeKind.Local ? value.ToUniversalTime() : DateTime.SpecifyKind(value, DateTimeKind.Utc);
        return utc.ToString("O", CultureInfo.InvariantCulture);
    }

    /// <summary>
    /// Parses an ISO-8601 timestamp back to UTC. Plain DateTime.Parse shifts a 'Z' value by
    /// the local offset (the D9 defect).
    /// </summary>
    private static DateTime? ParseTimestamp(object? value)
    {
        if (value is not string text || string.IsNullOrWhiteSpace(text))
        {
            return null;
        }

        if (!DateTime.TryParse(text, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind, out var parsed))
        {
            return null;
        }

        if (parsed.Kind == DateTimeKind.Local)
        {
            return parsed.ToUniversalTime();
        }

        return DateTime.SpecifyKind(parsed, DateTimeKind.Utc);
    }

    private static string Truncate(string value, int max)
    {
        if (string.IsNullOrEmpty(value) || value.Length <= max)
        {
            return value;
        }

        return value.Substring(0, max - 1).TrimEnd() + "…";
    }

    private static string? Cap(string? value, int max)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }

        var trimmed = value.Trim();
        if (trimmed.Length <= max)
        {
            return trimmed;
        }

        return trimmed.Substring(0, max) + " …[truncated]";
    }

    public static string DeriveTitle(string description)
    {
        if (string.IsNullOrWhiteSpace(description))
        {
            return string.Empty;
        }

        var firstLine = description.Trim().Split('\n')[0].Trim();
        return Truncate(firstLine, MaxTitle);
    }

    private static List<string> NormalizeRefs(IEnumerable<string>? refs)
    {
        if (refs == null)
        {
            return new List<string>();
        }

        return refs
            .Where(value => !string.IsNullOrWhiteSpace(value))
            .Select(value => value.Trim().ToUpperInvariant())
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    private static long ToLong(object? value)
    {
        return value == null ? 0 : Convert.ToInt64(value, CultureInfo.InvariantCulture);
    }

    private static TEnum ParseEnum<TEnum>(object? value, TEnum fallback) where TEnum : struct, Enum
    {
        if (value is string text && Enum.TryParse<TEnum>(text, ignoreCase: true, out var parsed))
        {
            return parsed;
        }

        return fallback;
    }

    private static TrackedGoal RowToGoal(Dictionary<string, object?> row)
    {
        var goal = new TrackedGoal
        {
            Id = row["Id"] as string ?? string.Empty,
            ProjectId = row["ProjectId"] as string ?? string.Empty,
            Description = row["Description"] as string ?? string.Empty,
            WorkingFolder = row["WorkingFolder"] as string ?? string.Empty,
            DownloadedDocsJson = row["DownloadedDocs"] as string ?? "[]",
            Status = (GoalStatus)(int)ToLong(row["Status"]),
            Priority = (int)ToLong(row.GetValueOrDefault("Priority")),
            Revision = ToLong(row.GetValueOrDefault("Revision")),
            WorkRevision = ToLong(row.GetValueOrDefault("WorkRevision")),
            MigrationVersion = (int)ToLong(row.GetValueOrDefault("MigrationVersion")),
            CreatedAt = ParseTimestamp(row["CreatedAt"]) ?? DateTime.UtcNow,
            UpdatedAt = ParseTimestamp(row["UpdatedAt"]) ?? DateTime.UtcNow,
            CompletedAt = ParseTimestamp(row.GetValueOrDefault("CompletedAt")),
            ReviewSummary = row.GetValueOrDefault("ReviewSummary") as string,
            ReviewRequestedAt = ParseTimestamp(row.GetValueOrDefault("ReviewRequestedAt"))
        };

        var title = row.GetValueOrDefault("Title") as string;
        goal.Title = string.IsNullOrWhiteSpace(title) ? DeriveTitle(goal.Description) : title!;
        goal.PendingScopeChanges = ParsePending(row.GetValueOrDefault("PendingScopeChange") as string);
        return goal;
    }

    private static List<PendingScopeChange> ParsePending(string? json)
    {
        if (string.IsNullOrWhiteSpace(json))
        {
            return new List<PendingScopeChange>();
        }

        try
        {
            return JsonSerializer.Deserialize<List<PendingScopeChange>>(json) ?? new List<PendingScopeChange>();
        }
        catch (JsonException ex)
        {
            AutomatonDebug.LogError($"GoalTrackingService.ParsePending: unreadable PendingScopeChange - {ex.Message}");
            return new List<PendingScopeChange>();
        }
    }

    private static GoalTask RowToTask(Dictionary<string, object?> row)
    {
        var task = new GoalTask
        {
            Id = row["Id"] as string ?? string.Empty,
            GoalId = row["GoalId"] as string ?? string.Empty,
            Ordinal = (int)ToLong(row["Ordinal"]),
            ShortId = row["ShortId"] as string ?? string.Empty,
            Description = row["Description"] as string ?? string.Empty,
            Status = ParseEnum(row["Status"], GoalTaskStatus.Todo),
            Note = row["Note"] as string,
            BlockedReason = row["BlockedReason"] as string,
            Evidence = row["Evidence"] as string,
            EvidenceBy = row["EvidenceBy"] as string,
            NeedsReview = ToLong(row["NeedsReview"]) != 0,
            ClaimedBy = row["ClaimedBy"] as string,
            ClaimedAt = ParseTimestamp(row["ClaimedAt"]),
            ClaimTouchedAt = ParseTimestamp(row["ClaimTouchedAt"]),
            CreatedAt = ParseTimestamp(row["CreatedAt"]) ?? DateTime.UtcNow,
            UpdatedAt = ParseTimestamp(row["UpdatedAt"]) ?? DateTime.UtcNow,
            CompletedAt = ParseTimestamp(row["CompletedAt"])
        };

        if (row["ClaimKind"] is string claimKind && Enum.TryParse<GoalClaimKind>(claimKind, true, out var parsedKind))
        {
            task.ClaimKind = parsedKind;
        }

        if (row["DependsOn"] is string dependsOn && !string.IsNullOrWhiteSpace(dependsOn))
        {
            try
            {
                task.DependsOn = NormalizeRefs(JsonSerializer.Deserialize<List<string>>(dependsOn));
            }
            catch (JsonException ex)
            {
                AutomatonDebug.LogError($"GoalTrackingService.RowToTask: unreadable DependsOn on task {task.Id} - {ex.Message}");
            }
        }

        return task;
    }

    private static GoalCriterion RowToCriterion(Dictionary<string, object?> row)
    {
        var criterion = new GoalCriterion
        {
            Id = row["Id"] as string ?? string.Empty,
            GoalId = row["GoalId"] as string ?? string.Empty,
            Ordinal = (int)ToLong(row["Ordinal"]),
            ShortId = row["ShortId"] as string ?? string.Empty,
            Description = row["Description"] as string ?? string.Empty,
            CheckCommand = row["CheckCommand"] as string,
            Status = ParseEnum(row["Status"], GoalCriterionStatus.Unverified),
            RequiredProof = ParseEnum(row["RequiredProof"], GoalProofKind.Human),
            Evidence = row["Evidence"] as string,
            CheckedAt = ParseTimestamp(row["CheckedAt"]),
            LastCheckRunId = row["LastCheckRunId"] as string,
            Author = ParseEnum(row["Author"], GoalAuthor.Human)
        };

        if (row["MetBy"] is string metBy && Enum.TryParse<GoalProofSource>(metBy, true, out var parsedMetBy))
        {
            criterion.MetBy = parsedMetBy;
        }

        if (row["CheckedWorkRevision"] != null)
        {
            criterion.CheckedWorkRevision = ToLong(row["CheckedWorkRevision"]);
        }

        return criterion;
    }

    // ── Settings ──────────────────────────────────────────────────────────

    private string? Setting(string key, string? projectId)
    {
        if (!string.IsNullOrWhiteSpace(projectId))
        {
            var scoped = _db.GetSettingValue($"{key}/{projectId}");
            if (!string.IsNullOrWhiteSpace(scoped))
            {
                return scoped;
            }
        }

        return _db.GetSettingValue(key);
    }

    /// <summary>
    /// Whether the goal digest goes into this chat's requests. On by default; the most
    /// specific setting wins: the chat, then the project, then everywhere.
    /// </summary>
    public bool IsDigestEnabled(string? projectId, string? sessionId)
    {
        string? value = null;
        if (!string.IsNullOrWhiteSpace(sessionId))
        {
            value = _db.GetSettingValue($"{GoalDigestBuilder.InjectSetting}/chat/{sessionId}");
        }

        if (string.IsNullOrWhiteSpace(value))
        {
            value = Setting(GoalDigestBuilder.InjectSetting, projectId);
        }

        return !string.Equals(value, "false", StringComparison.OrdinalIgnoreCase) &&
               !string.Equals(value, "0", StringComparison.Ordinal);
    }

    /// <summary>Turns the goal digest on or off for every chat of a project.</summary>
    public void SetDigestEnabled(string projectId, bool enabled)
    {
        if (string.IsNullOrWhiteSpace(projectId))
        {
            throw new ArgumentException("projectId is required", nameof(projectId));
        }

        _db.SetSettingValue($"{GoalDigestBuilder.InjectSetting}/{projectId}", enabled ? "true" : "false");
        AutomatonDebug.Log($"GoalTrackingService.SetDigestEnabled: project={projectId}, enabled={enabled}");
    }

    private bool RequireEvidence(string? projectId)
    {
        var value = Setting(RequireEvidenceSetting, projectId);
        return !string.Equals(value, "false", StringComparison.OrdinalIgnoreCase) &&
               !string.Equals(value, "0", StringComparison.Ordinal);
    }

    /// <summary>
    /// The proof a new criterion without a check command asks for. Human by default;
    /// <c>goals.reviewMode = none</c> turns review off for the project and makes it Any.
    /// </summary>
    private GoalProofKind DefaultProofFor(string? projectId)
    {
        var reviewMode = Setting(ReviewModeSetting, projectId);
        if (string.Equals(reviewMode, "none", StringComparison.OrdinalIgnoreCase))
        {
            return GoalProofKind.Any;
        }

        var configured = Setting(DefaultProofSetting, projectId);
        if (Enum.TryParse<GoalProofKind>(configured, ignoreCase: true, out var parsed) && parsed != GoalProofKind.Check)
        {
            return parsed;
        }

        return GoalProofKind.Human;
    }

    private TimeSpan ClaimStaleAfter(string? projectId)
    {
        var value = Setting(ClaimStaleMinutesSetting, projectId);
        if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var minutes) && minutes > 0)
        {
            return TimeSpan.FromMinutes(minutes);
        }

        return TimeSpan.FromMinutes(120);
    }
}
