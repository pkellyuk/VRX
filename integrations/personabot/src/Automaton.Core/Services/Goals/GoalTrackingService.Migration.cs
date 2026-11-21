using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Automaton.Core.Models;
using Automaton.Core.Models.Goals;

namespace Automaton.Core.Services.Goals;

public sealed partial class GoalTrackingService
{
    /// <summary>
    /// 0 = the legacy Plan / Milestones / SuccessCriteria columns have not been converted.
    /// 1 = converted. Recorded on the goal, in the converting transaction, so that removing
    /// every migrated task can never bring the legacy plan back.
    /// </summary>
    public const int CurrentMigrationVersion = 1;

    // Goals whose legacy columns could not be read. Retried at the next process start, not
    // on every service construction.
    private static readonly Dictionary<string, HashSet<string>> FailedMigrationsByDatabase = new(StringComparer.OrdinalIgnoreCase);
    private static readonly object MigrationLock = new();

    private HashSet<string> FailedMigrations
    {
        get
        {
            if (!FailedMigrationsByDatabase.TryGetValue(_db.DatabasePath, out var failed))
            {
                failed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                FailedMigrationsByDatabase[_db.DatabasePath] = failed;
            }

            return failed;
        }
    }

    /// <summary>Goal ids that could not be migrated in this process, for the UI's marker.</summary>
    public IReadOnlyCollection<string> GoalsThatCouldNotMigrate
    {
        get
        {
            lock (MigrationLock)
            {
                return FailedMigrations.ToList();
            }
        }
    }

    /// <summary>Forgets earlier failures so a repaired goal is retried without a restart.</summary>
    public void ResetMigrationFailures()
    {
        lock (MigrationLock)
        {
            FailedMigrations.Clear();
        }
    }

    /// <summary>
    /// Converts every goal still at version 0. One transaction per goal: a goal that fails
    /// is left exactly as it was and does not stop the others. Returns how many converted.
    /// </summary>
    public int MigratePending()
    {
        var pending = _db.Query("SELECT Id FROM Goals WHERE MigrationVersion = 0")
            .Select(row => row["Id"] as string ?? string.Empty)
            .Where(id => id.Length > 0)
            .ToList();

        lock (MigrationLock)
        {
            pending = pending.Where(id => !FailedMigrations.Contains(id)).ToList();
        }

        if (pending.Count == 0)
        {
            return 0;
        }

        AutomatonDebug.Log($"GoalTrackingService.MigratePending: Entry - {pending.Count} goal(s) at version 0");

        var migrated = 0;
        foreach (var goalId in pending)
        {
            try
            {
                if (MigrateOne(goalId))
                {
                    migrated++;
                }
            }
            catch (Exception ex)
            {
                lock (MigrationLock)
                {
                    FailedMigrations.Add(goalId);
                }

                AutomatonDebug.LogError($"GoalTrackingService.MigratePending: goal {goalId} could not be migrated and was left untouched - {ex.Message}");
            }
        }

        AutomatonDebug.Log($"GoalTrackingService.MigratePending: Exit - migrated {migrated} of {pending.Count}");
        return migrated;
    }

    private bool MigrateOne(string goalId)
    {
        var converted = false;
        var now = _clock();

        _db.ExecuteInTransaction((connection, transaction) =>
        {
            // Claim the goal first: the write takes the lock, and a goal another process has
            // already migrated matches no row.
            var claimed = ConfigDatabase.ExecuteOnConnection(connection, transaction,
                "UPDATE Goals SET MigrationVersion = @Version, Revision = Revision + 1 WHERE Id = @Id AND MigrationVersion = 0",
                P("@Version", CurrentMigrationVersion), P("@Id", goalId));
            if (claimed == 0)
            {
                AutomatonDebug.Log($"GoalTrackingService.MigrateOne: goal {goalId} was already migrated elsewhere");
                return;
            }

            var context = new MutationContext(this, connection, transaction, GoalCaller.ForSystem(), now);
            var row = context.Query("SELECT * FROM Goals WHERE Id = @Id", P("@Id", goalId))[0];
            context.Goal = RowToGoal(row);

            // JsonException from any of these aborts the transaction and leaves version 0.
            var milestones = ParseMilestones(row["Milestones"] as string, goalId);
            var plan = ParseStrings(row["Plan"] as string, "Plan", goalId);
            var criteria = ParseStrings(row["SuccessCriteria"] as string, "SuccessCriteria", goalId);

            var alreadyCompleted = context.Goal.Status == GoalStatus.Completed;
            var dropped = new List<string>();
            var shortIdByMilestone = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            var milestoneTasks = new List<(GoalTask Task, StoredMilestone Source)>();
            var ordinal = 0;

            foreach (var milestone in milestones)
            {
                if (string.IsNullOrWhiteSpace(milestone.Description))
                {
                    continue;
                }

                ordinal++;
                var description = milestone.Description.Trim();
                if (milestone.Criteria.Count > 0)
                {
                    description += "\n" + string.Join("\n", milestone.Criteria.Select(item => $"- {item}"));
                }

                var task = InsertTask(context, description, ordinal, new List<string>());
                context.Goal.Tasks.Add(task);
                milestoneTasks.Add((task, milestone));
                if (!string.IsNullOrWhiteSpace(milestone.Id))
                {
                    shortIdByMilestone[milestone.Id] = task.ShortId;
                }
            }

            foreach (var (task, source) in milestoneTasks)
            {
                var dependsOn = new List<string>();
                foreach (var legacyId in source.DependsOn)
                {
                    if (shortIdByMilestone.TryGetValue(legacyId, out var shortId) && !string.Equals(shortId, task.ShortId, StringComparison.OrdinalIgnoreCase))
                    {
                        dependsOn.Add(shortId);
                        continue;
                    }

                    dropped.Add($"{task.ShortId} depended on unknown milestone '{legacyId}'");
                }

                if (dependsOn.Count == 0)
                {
                    continue;
                }

                try
                {
                    ValidateDependencies(context.Goal, task.ShortId, dependsOn);
                }
                catch (GoalRuleException ex)
                {
                    dropped.Add($"{task.ShortId} dependencies dropped: {ex.Message}");
                    continue;
                }

                task.DependsOn = dependsOn;
                context.Execute("UPDATE GoalTasks SET DependsOn = @DependsOn WHERE Id = @Id",
                    P("@DependsOn", JsonSerializer.Serialize(dependsOn)), P("@Id", task.Id));
            }

            foreach (var step in plan)
            {
                ordinal++;
                context.Goal.Tasks.Add(InsertTask(context, step, ordinal, new List<string>()));
            }

            var criterionOrdinal = 0;
            foreach (var text in criteria)
            {
                criterionOrdinal++;

                // They predate authorship. Human-authored is the safe side: a model cannot then
                // weaken them without approval.
                var criterion = InsertCriterion(context, new GoalCriterionDraft { Description = text, RequiredProof = GoalProofKind.Human }, criterionOrdinal, GoalProofKind.Human, GoalAuthor.Human);
                context.Goal.Criteria.Add(criterion);
            }

            if (alreadyCompleted)
            {
                const string MigratedEvidence = "migrated from a goal already marked completed";
                foreach (var task in context.Goal.Tasks)
                {
                    task.Status = GoalTaskStatus.Done;
                    task.Evidence = MigratedEvidence;
                    task.EvidenceBy = "Human";
                    task.CompletedAt = now;
                    WriteTask(context, task);
                }

                foreach (var criterion in context.Goal.Criteria)
                {
                    criterion.Status = GoalCriterionStatus.Met;
                    criterion.MetBy = GoalProofSource.Human;
                    criterion.Evidence = MigratedEvidence;
                    criterion.CheckedAt = now;
                    WriteCriterion(context, criterion);
                }

                context.Execute("UPDATE Goals SET CompletedAt = COALESCE(CompletedAt, UpdatedAt) WHERE Id = @Id", P("@Id", goalId));
            }

            if (string.IsNullOrWhiteSpace(row.GetValueOrDefault("Title") as string))
            {
                context.Execute("UPDATE Goals SET Title = @Title WHERE Id = @Id", P("@Title", DeriveTitle(context.Goal.Description)), P("@Id", goalId));
            }

            context.Execute("UPDATE Goals SET WorkRevision = WorkRevision + 1 WHERE Id = @Id", P("@Id", goalId));

            var summary = $"migrated: {milestoneTasks.Count} milestone(s) and {plan.Count} plan step(s) became {context.Goal.Tasks.Count} task(s); {criterionOrdinal} criteria" +
                          (alreadyCompleted ? "; goal was already completed, so all marked done" : string.Empty) +
                          (dropped.Count == 0 ? string.Empty : $"; dropped: {string.Join("; ", dropped)}");
            context.Journal(GoalJournalKinds.Migration, summary);
            AutomatonDebug.Log($"GoalTrackingService.MigrateOne: goal {goalId} - {summary}");
            converted = true;
        });

        return converted;
    }

    private sealed class StoredMilestone
    {
        public string Id { get; set; } = string.Empty;
        public string Description { get; set; } = string.Empty;
        public List<string> Criteria { get; set; } = new();
        public List<string> DependsOn { get; set; } = new();
    }

    private static List<string> ParseStrings(string? json, string column, string goalId)
    {
        if (string.IsNullOrWhiteSpace(json))
        {
            return new List<string>();
        }

        try
        {
            var values = JsonSerializer.Deserialize<List<string?>>(json) ?? new List<string?>();
            return values.Where(value => !string.IsNullOrWhiteSpace(value)).Select(value => value!.Trim()).ToList();
        }
        catch (JsonException ex)
        {
            throw new JsonException($"column {column} of goal {goalId} is not a JSON list of strings: {ex.Message}", ex);
        }
    }

    private static List<StoredMilestone> ParseMilestones(string? json, string goalId)
    {
        var milestones = new List<StoredMilestone>();
        if (string.IsNullOrWhiteSpace(json))
        {
            return milestones;
        }

        try
        {
            using var document = JsonDocument.Parse(json);
            if (document.RootElement.ValueKind == JsonValueKind.Null)
            {
                return milestones;
            }

            if (document.RootElement.ValueKind != JsonValueKind.Array)
            {
                throw new JsonException("expected a JSON array");
            }

            foreach (var element in document.RootElement.EnumerateArray())
            {
                if (element.ValueKind != JsonValueKind.Object)
                {
                    continue;
                }

                milestones.Add(new StoredMilestone
                {
                    Id = ReadString(element, "id"),
                    Description = ReadString(element, "description"),
                    Criteria = ReadStringList(element, "criteria"),
                    DependsOn = ReadStringList(element, "dependsOn")
                });
            }
        }
        catch (JsonException ex)
        {
            throw new JsonException($"column Milestones of goal {goalId} is not readable: {ex.Message}", ex);
        }

        return milestones;
    }

    private static bool TryGetProperty(JsonElement element, string name, out JsonElement value)
    {
        foreach (var property in element.EnumerateObject())
        {
            if (string.Equals(property.Name, name, StringComparison.OrdinalIgnoreCase))
            {
                value = property.Value;
                return true;
            }
        }

        value = default;
        return false;
    }

    private static string ReadString(JsonElement element, string name)
    {
        if (!TryGetProperty(element, name, out var value) || value.ValueKind != JsonValueKind.String)
        {
            return string.Empty;
        }

        return value.GetString() ?? string.Empty;
    }

    private static List<string> ReadStringList(JsonElement element, string name)
    {
        var result = new List<string>();
        if (!TryGetProperty(element, name, out var value) || value.ValueKind != JsonValueKind.Array)
        {
            return result;
        }

        foreach (var item in value.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(item.GetString()))
            {
                result.Add(item.GetString()!.Trim());
            }
        }

        return result;
    }
}
