using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Automaton.Core.Models;
using Automaton.Core.Models.Goals;

namespace Automaton.Core.Services.Goals;

public sealed partial class GoalTrackingService
{
    // ── Task structure ────────────────────────────────────────────────────

    /// <summary>
    /// Adds a task after <paramref name="afterRef"/>, or at the end. Adding work to a finished
    /// goal reopens it.
    /// </summary>
    public GoalTask AddTask(GoalCaller caller, string goalId, string description, string? afterRef = null, IEnumerable<string>? dependsOn = null)
    {
        if (string.IsNullOrWhiteSpace(description))
        {
            throw new GoalRuleException("A task needs a description.");
        }

        AutomatonDebug.Log($"GoalTrackingService.AddTask: Entry - goalId={goalId}, after={afterRef ?? "end"}, caller={caller}");

        GoalTask? added = null;
        Mutate(caller, goalId, null, context =>
        {
            var position = context.Goal.Tasks.Count;
            if (!string.IsNullOrWhiteSpace(afterRef))
            {
                var after = ResolveTask(context.Goal, afterRef!);
                position = context.Goal.Tasks.IndexOf(after) + 1;
            }

            added = InsertTask(context, description, context.Goal.Tasks.Count + 1, new List<string>());
            context.Goal.Tasks.Insert(position, added);

            var dependencies = NormalizeRefs(dependsOn);
            if (dependencies.Count > 0)
            {
                ValidateDependencies(context.Goal, added.ShortId, dependencies);
                added.DependsOn = dependencies;
                context.Execute("UPDATE GoalTasks SET DependsOn = @DependsOn WHERE Id = @Id",
                    P("@DependsOn", JsonSerializer.Serialize(dependencies)),
                    P("@Id", added.Id));
            }

            Renumber(context);
            context.BumpWork();
            context.Journal(GoalJournalKinds.Scope, $"added {added.ShortId}: {Truncate(added.Description, 200)}", added.Id);
            ReopenIfClosed(context, $"task {added.ShortId} added");
        });

        AutomatonDebug.Log($"GoalTrackingService.AddTask: Exit - goalId={goalId}, task={added!.ShortId}, ordinal={added.Ordinal}");
        return added;
    }

    /// <summary>Rewords a task or changes what it depends on. Null leaves a part alone.</summary>
    public GoalTask UpdateTask(GoalCaller caller, string goalId, string taskRef, string? description, IEnumerable<string>? dependsOn)
    {
        if (string.IsNullOrWhiteSpace(taskRef))
        {
            throw new ArgumentException("taskRef is required", nameof(taskRef));
        }

        AutomatonDebug.Log($"GoalTrackingService.UpdateTask: Entry - goalId={goalId}, task={taskRef}, caller={caller}");

        GoalTask? updated = null;
        Mutate(caller, goalId, null, context =>
        {
            var task = ResolveTask(context.Goal, taskRef);
            RequireHolderOrHuman(context, task, "edit");

            if (description != null)
            {
                if (string.IsNullOrWhiteSpace(description))
                {
                    throw new GoalRuleException("A task needs a description.");
                }

                var before = task.Description;
                task.Description = description.Trim();
                context.Execute("UPDATE GoalTasks SET Description = @Description, UpdatedAt = @Now WHERE Id = @Id",
                    P("@Description", task.Description), P("@Now", Iso(context.Now)), P("@Id", task.Id));
                context.Journal(GoalJournalKinds.Scope, $"reworded {task.ShortId}: \"{Truncate(before, 150)}\" -> \"{Truncate(task.Description, 150)}\"", task.Id);
            }

            if (dependsOn != null)
            {
                var dependencies = NormalizeRefs(dependsOn);
                ValidateDependencies(context.Goal, task.ShortId, dependencies);
                task.DependsOn = dependencies;
                context.Execute("UPDATE GoalTasks SET DependsOn = @DependsOn, UpdatedAt = @Now WHERE Id = @Id",
                    P("@DependsOn", JsonSerializer.Serialize(dependencies)), P("@Now", Iso(context.Now)), P("@Id", task.Id));
            }

            context.BumpWork();
            updated = task;
        });

        AutomatonDebug.Log($"GoalTrackingService.UpdateTask: Exit - goalId={goalId}, task={updated!.ShortId}");
        return updated;
    }

    /// <summary>Moves a task to just after <paramref name="afterRef"/>, or to the front when null.</summary>
    public GoalTask MoveTask(GoalCaller caller, string goalId, string taskRef, string? afterRef)
    {
        if (string.IsNullOrWhiteSpace(taskRef))
        {
            throw new ArgumentException("taskRef is required", nameof(taskRef));
        }

        AutomatonDebug.Log($"GoalTrackingService.MoveTask: Entry - goalId={goalId}, task={taskRef}, after={afterRef ?? "front"}");

        GoalTask? moved = null;
        Mutate(caller, goalId, null, context =>
        {
            var task = ResolveTask(context.Goal, taskRef);
            GoalTask? after = null;
            if (!string.IsNullOrWhiteSpace(afterRef))
            {
                after = ResolveTask(context.Goal, afterRef!);
                if (ReferenceEquals(after, task))
                {
                    throw new GoalRuleException($"{task.ShortId} cannot be moved after itself.");
                }
            }

            context.Goal.Tasks.Remove(task);
            var index = after == null ? 0 : context.Goal.Tasks.IndexOf(after) + 1;
            context.Goal.Tasks.Insert(index, task);
            Renumber(context);
            context.BumpWork();
            moved = task;
        });

        AutomatonDebug.Log($"GoalTrackingService.MoveTask: Exit - {moved!.ShortId} now at ordinal {moved.Ordinal}");
        return moved;
    }

    /// <summary>
    /// Removes a task. Refused while other tasks depend on it. Its ShortId is never reused.
    /// </summary>
    public void RemoveTask(GoalCaller caller, string goalId, string taskRef)
    {
        if (string.IsNullOrWhiteSpace(taskRef))
        {
            throw new ArgumentException("taskRef is required", nameof(taskRef));
        }

        AutomatonDebug.Log($"GoalTrackingService.RemoveTask: Entry - goalId={goalId}, task={taskRef}, caller={caller}");

        Mutate(caller, goalId, null, context =>
        {
            var task = ResolveTask(context.Goal, taskRef);
            RequireHolderOrHuman(context, task, "remove");

            var dependants = context.Goal.Tasks
                .Where(other => other.DependsOn.Contains(task.ShortId, StringComparer.OrdinalIgnoreCase))
                .Select(other => other.ShortId)
                .ToList();
            if (dependants.Count > 0)
            {
                throw new GoalRuleException($"{task.ShortId} cannot be removed because {string.Join(", ", dependants)} depend on it. Remove that dependency first.");
            }

            context.Execute("DELETE FROM GoalTasks WHERE Id = @Id", P("@Id", task.Id));
            context.Goal.Tasks.Remove(task);
            Renumber(context);
            context.BumpWork();
            context.Journal(GoalJournalKinds.Scope, $"removed {task.ShortId} ({task.Status}): {Truncate(task.Description, 200)}");
        });

        AutomatonDebug.Log($"GoalTrackingService.RemoveTask: Exit - goalId={goalId}, task={taskRef} removed");
    }

    /// <summary>
    /// Replaces the whole task list. Only allowed while no task has left Todo, because
    /// replacing a list that has progress would wipe it. The legacy tools' route.
    /// </summary>
    public List<GoalTask> ReplaceTaskList(GoalCaller caller, string goalId, IEnumerable<GoalTaskDraft> drafts)
    {
        if (drafts == null)
        {
            throw new ArgumentNullException(nameof(drafts));
        }

        var all = drafts.ToList();
        var wanted = all.Where(draft => draft != null && !string.IsNullOrWhiteSpace(draft.Description)).ToList();
        AutomatonDebug.Log($"GoalTrackingService.ReplaceTaskList: Entry - goalId={goalId}, tasks={wanted.Count}, caller={caller}");

        var result = new List<GoalTask>();
        Mutate(caller, goalId, null, context =>
        {
            var progressed = context.Goal.Tasks.Where(task => task.Status != GoalTaskStatus.Todo).Select(task => task.ShortId).ToList();
            if (progressed.Count > 0)
            {
                // Only ever name a tool the calling chat really has.
                var route = "Tasks that have progress can only be changed one at a time. If you have no tool for that, leave the list as it is and tell the human what should change.";
                if (caller.AvailableToolIds != null)
                {
                    route = caller.Lacks("goal_task_edit")
                        ? "Task-level tracking is not available in this chat because its tool list was fixed before the goals update. Start a new chat in this project to continue this goal; its progress is stored and will be shown there."
                        : "Use goal_task_edit to add, change or remove individual tasks.";
                }

                throw new GoalRuleException($"The task list cannot be replaced because {string.Join(", ", progressed)} already have progress, and replacing the list would wipe it. {route}");
            }

            context.Execute("DELETE FROM GoalTasks WHERE GoalId = @GoalId", P("@GoalId", context.Goal.Id));
            context.Goal.Tasks.Clear();

            var ordinal = 0;
            var insertedByDraft = new Dictionary<int, GoalTask>();
            for (var index = 0; index < all.Count; index++)
            {
                var draft = all[index];
                if (draft == null || string.IsNullOrWhiteSpace(draft.Description))
                {
                    continue;
                }

                ordinal++;
                var inserted = InsertTask(context, draft.Description, ordinal, new List<string>());
                context.Goal.Tasks.Add(inserted);
                insertedByDraft[index] = inserted;
                result.Add(inserted);
            }

            // Before this, a replaced list lost every dependency it was given (GOALSUPDATE.md §24.1).
            ApplyDraftDependencies(context, all, insertedByDraft);
            context.BumpWork();
            context.Journal(GoalJournalKinds.Scope, $"task list replaced with {ordinal} task(s)");
            if (ordinal > 0)
            {
                ReopenIfClosed(context, "task list replaced");
            }
        });

        AutomatonDebug.Log($"GoalTrackingService.ReplaceTaskList: Exit - goalId={goalId}, tasks={result.Count}");
        return result;
    }

    // ── Task status, and the claim that goes with Doing ───────────────────

    public GoalTaskChangeResult SetTaskStatus(
        GoalCaller caller,
        string goalId,
        string taskRef,
        GoalTaskStatus status,
        string? note = null,
        string? evidence = null,
        string? blockedReason = null)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (string.IsNullOrWhiteSpace(taskRef))
        {
            throw new ArgumentException("taskRef is required", nameof(taskRef));
        }

        AutomatonDebug.Log($"GoalTrackingService.SetTaskStatus: Entry - goalId={goalId}, task={taskRef}, status={status}, caller={caller}");

        var requireEvidence = RequireEvidence(caller.ProjectId);
        var staleAfter = ClaimStaleAfter(caller.ProjectId);
        var result = new GoalTaskChangeResult();

        Mutate(caller, goalId, null, context => ApplyTaskStatus(context, taskRef, status, note, evidence, blockedReason, requireEvidence, staleAfter, result));

        AutomatonDebug.Log($"GoalTrackingService.SetTaskStatus: Exit - goalId={goalId}, task={result.Task.ShortId}, status={result.Task.Status}, progress={result.Progress.Describe()}, sideEffects={result.SideEffects.Count}");
        return result;
    }

    /// <summary>
    /// The rules of a task status change, against a goal already locked by the caller's
    /// transaction. Separate from <see cref="SetTaskStatus"/> so that a change to a task and a
    /// change to something else - a Director goal's dispatch record, say - can be one transaction.
    /// </summary>
    private void ApplyTaskStatus(
        MutationContext context,
        string taskRef,
        GoalTaskStatus status,
        string? note,
        string? evidence,
        string? blockedReason,
        bool requireEvidence,
        TimeSpan staleAfter,
        GoalTaskChangeResult result)
    {
        var task = ResolveTask(context.Goal, taskRef);
        var previous = task.Status;
        var holder = task.ClaimedBy;
        var callerHolds = holder != null && string.Equals(holder, context.Caller.ClaimantId, StringComparison.Ordinal);

        if (holder != null && !callerHolds && !context.Caller.IsHuman && context.Caller.Author != GoalAuthor.System)
        {
            var touched = task.ClaimTouchedAt ?? task.ClaimedAt ?? context.Now;
            var idle = context.Now - touched;
            var canTakeOver = status == GoalTaskStatus.Doing && task.ClaimKind == GoalClaimKind.Chat && idle >= staleAfter;

            if (!canTakeOver)
            {
                var heldFor = context.Now - (task.ClaimedAt ?? context.Now);
                AutomatonDebug.LogWarning($"GoalTrackingService.SetTaskStatus: claim refused - {task.ShortId} held by {holder} for {heldFor.TotalMinutes:F0} min, wanted by {context.Caller.ClaimantId}");
                throw new GoalRuleException($"{task.ShortId} is being worked on by {holder} (for {Math.Max(1, (int)heldFor.TotalMinutes)} min). Pick another task; this one was not changed.");
            }

            result.SideEffects.Add($"took over {task.ShortId} from {holder}, which had been idle for {(int)idle.TotalMinutes} min");
            context.Journal(GoalJournalKinds.Claim, $"{context.Caller.ClaimantId} took over {task.ShortId} from {holder}, idle {(int)idle.TotalMinutes} min", task.Id);
            AutomatonDebug.Log($"GoalTrackingService.SetTaskStatus: stale claim on {task.ShortId} taken over from {holder} by {context.Caller.ClaimantId}");
        }

        switch (status)
        {
            case GoalTaskStatus.Doing:
                StartTask(context, task, result);
                break;

            case GoalTaskStatus.Done:
                RequireDependenciesFinished(context.Goal, task, "finished");
                if (requireEvidence && context.Caller.Author == GoalAuthor.Model && string.IsNullOrWhiteSpace(evidence) && string.IsNullOrWhiteSpace(task.Evidence))
                {
                    throw new GoalRuleException($"{task.ShortId} cannot be marked Done without evidence. Say what shows it is finished: the command you ran and its result, or the file you changed.");
                }

                break;

            case GoalTaskStatus.Skipped:
                if (string.IsNullOrWhiteSpace(note))
                {
                    throw new GoalRuleException($"{task.ShortId} cannot be skipped without a note saying why.");
                }

                break;

            case GoalTaskStatus.Blocked:
                if (string.IsNullOrWhiteSpace(blockedReason) && string.IsNullOrWhiteSpace(note))
                {
                    throw new GoalRuleException($"{task.ShortId} cannot be marked Blocked without saying what it is blocked on.");
                }

                break;
        }

        task.Status = status;
        if (note != null)
        {
            task.Note = Cap(note, 500);
        }

        if (!string.IsNullOrWhiteSpace(evidence))
        {
            task.Evidence = Cap(evidence, MaxEvidence);
            task.EvidenceBy = context.Caller.Author == GoalAuthor.Human ? "Human" : context.Caller.ClaimKind == GoalClaimKind.Worker ? "Worker" : "Model";
        }

        task.BlockedReason = status == GoalTaskStatus.Blocked ? Cap(blockedReason ?? note, 500) : null;

        if (status == GoalTaskStatus.Doing)
        {
            task.ClaimedBy = context.Caller.ClaimantId;
            task.ClaimKind = context.Caller.ClaimKind;
            task.ClaimedAt = callerHolds ? task.ClaimedAt ?? context.Now : context.Now;
            task.ClaimTouchedAt = context.Now;
        }
        else
        {
            task.ClaimedBy = null;
            task.ClaimKind = null;
            task.ClaimedAt = null;
            task.ClaimTouchedAt = null;
        }

        if (status == GoalTaskStatus.Done)
        {
            task.NeedsReview = false;
        }

        task.CompletedAt = task.IsFinished ? context.Now : null;
        WriteTask(context, task);

        var wasFinished = previous == GoalTaskStatus.Done || previous == GoalTaskStatus.Skipped;
        if (wasFinished && !task.IsFinished)
        {
            ReopenPrerequisite(context, task, result);
            ReopenIfClosed(context, $"task {task.ShortId} reopened");
            result.GoalReopened = context.Goal.Status == GoalStatus.Active;
        }

        if (status == GoalTaskStatus.Skipped)
        {
            context.Journal(GoalJournalKinds.Scope, $"skipped {task.ShortId}: {task.Note}", task.Id);
        }

        if (previous != status)
        {
            context.BumpWork();
            context.Journal(GoalJournalKinds.StatusChange, $"{task.ShortId} {previous} -> {status}{(string.IsNullOrWhiteSpace(task.Note) ? string.Empty : $" | {task.Note}")}", task.Id);
        }

        result.Task = task;
        result.Progress = GoalProgress.For(context.Goal);
    }

    /// <summary>Gives up a claim without finishing: the task goes back to Todo.</summary>
    public GoalTask ReleaseClaim(GoalCaller caller, string goalId, string taskRef)
    {
        AutomatonDebug.Log($"GoalTrackingService.ReleaseClaim: Entry - goalId={goalId}, task={taskRef}, caller={caller}");
        var result = SetTaskStatus(caller, goalId, taskRef, GoalTaskStatus.Todo, note: null);
        AutomatonDebug.Log($"GoalTrackingService.ReleaseClaim: Exit - {result.Task.ShortId} is {result.Task.Status}");
        return result.Task;
    }

    private void StartTask(MutationContext context, GoalTask task, GoalTaskChangeResult result)
    {
        var caller = context.Caller;
        if (string.IsNullOrWhiteSpace(caller.ClaimantId))
        {
            throw new GoalRuleException("This caller cannot hold a task, so it cannot mark one Doing.");
        }

        RequireDependenciesFinished(context.Goal, task, "started");

        // One Doing task per claimant per goal: the caller's previous one goes back to Todo.
        foreach (var other in context.Goal.Tasks)
        {
            if (ReferenceEquals(other, task) || other.Status != GoalTaskStatus.Doing)
            {
                continue;
            }

            if (!string.Equals(other.ClaimedBy, caller.ClaimantId, StringComparison.Ordinal))
            {
                continue;
            }

            other.Status = GoalTaskStatus.Todo;
            other.ClaimedBy = null;
            other.ClaimKind = null;
            other.ClaimedAt = null;
            other.ClaimTouchedAt = null;
            WriteTask(context, other);
            result.SideEffects.Add($"{other.ShortId} went back to Todo because you can work on one task at a time");
            context.Journal(GoalJournalKinds.StatusChange, $"{other.ShortId} Doing -> Todo (its holder started {task.ShortId})", other.Id);
        }

        if (string.IsNullOrWhiteSpace(caller.StateId) || caller.Author != GoalAuthor.Model)
        {
            return;
        }

        BindInTransaction(context, caller.StateId!, context.Goal.Id);
    }

    /// <summary>
    /// A finished task has been reopened. Dependants that were being worked on go back to
    /// Todo; dependants already Done keep their status but are flagged for another look.
    /// </summary>
    private void ReopenPrerequisite(MutationContext context, GoalTask reopened, GoalTaskChangeResult result)
    {
        foreach (var dependant in context.Goal.Tasks)
        {
            if (!dependant.DependsOn.Contains(reopened.ShortId, StringComparer.OrdinalIgnoreCase))
            {
                continue;
            }

            if (dependant.Status == GoalTaskStatus.Doing)
            {
                dependant.Status = GoalTaskStatus.Todo;
                dependant.ClaimedBy = null;
                dependant.ClaimKind = null;
                dependant.ClaimedAt = null;
                dependant.ClaimTouchedAt = null;
                WriteTask(context, dependant);
                result.SideEffects.Add($"{dependant.ShortId} went back to Todo because {reopened.ShortId} was reopened");
                context.Journal(GoalJournalKinds.StatusChange, $"{dependant.ShortId} Doing -> Todo ({reopened.ShortId} reopened)", dependant.Id);
                continue;
            }

            if (dependant.Status == GoalTaskStatus.Done && !dependant.NeedsReview)
            {
                dependant.NeedsReview = true;
                WriteTask(context, dependant);
                result.SideEffects.Add($"{dependant.ShortId} needs review because {reopened.ShortId} was reopened; mark it Done again with fresh evidence");
                context.Journal(GoalJournalKinds.StatusChange, $"{dependant.ShortId} flagged for review ({reopened.ShortId} reopened)", dependant.Id);
            }
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    private static void RequireDependenciesFinished(TrackedGoal goal, GoalTask task, string verb)
    {
        var open = task.DependsOn
            .Select(id => goal.Tasks.FirstOrDefault(other => string.Equals(other.ShortId, id, StringComparison.OrdinalIgnoreCase)))
            .Where(other => other != null && !other.IsFinished)
            .Select(other => $"{other!.ShortId} ({other.Status})")
            .ToList();

        if (open.Count == 0)
        {
            return;
        }

        throw new GoalRuleException($"{task.ShortId} cannot be {verb} until {string.Join(", ", open)} {(open.Count == 1 ? "is" : "are")} done.");
    }

    private static void RequireHolderOrHuman(MutationContext context, GoalTask task, string verb)
    {
        if (string.IsNullOrEmpty(task.ClaimedBy) || context.Caller.IsHuman || context.Caller.Author == GoalAuthor.System)
        {
            return;
        }

        if (string.Equals(task.ClaimedBy, context.Caller.ClaimantId, StringComparison.Ordinal))
        {
            return;
        }

        throw new GoalRuleException($"You cannot {verb} {task.ShortId}: it is being worked on by {task.ClaimedBy}.");
    }

    /// <summary>
    /// Rejects a dependency on itself, on a task that is not in the goal, and any set that
    /// would close a cycle.
    /// </summary>
    private static void ValidateDependencies(TrackedGoal goal, string taskShortId, List<string> dependsOn)
    {
        foreach (var reference in dependsOn)
        {
            if (string.Equals(reference, taskShortId, StringComparison.OrdinalIgnoreCase))
            {
                throw new GoalRuleException($"{taskShortId} cannot depend on itself.");
            }

            if (!goal.Tasks.Any(task => string.Equals(task.ShortId, reference, StringComparison.OrdinalIgnoreCase)))
            {
                throw new GoalRuleException($"{taskShortId} cannot depend on {reference}: there is no such task in this goal.");
            }
        }

        var edges = goal.Tasks.ToDictionary(task => task.ShortId, task => task.DependsOn, StringComparer.OrdinalIgnoreCase);
        edges[taskShortId] = dependsOn;

        var visiting = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var finished = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        bool HasCycle(string node)
        {
            if (finished.Contains(node))
            {
                return false;
            }

            if (!visiting.Add(node))
            {
                return true;
            }

            if (edges.TryGetValue(node, out var next))
            {
                foreach (var dependency in next)
                {
                    if (HasCycle(dependency))
                    {
                        return true;
                    }
                }
            }

            visiting.Remove(node);
            finished.Add(node);
            return false;
        }

        if (HasCycle(taskShortId))
        {
            throw new GoalRuleException($"{taskShortId} cannot depend on {string.Join(", ", dependsOn)}: that would make the tasks depend on each other in a circle.");
        }
    }

    /// <summary>Accepts "T3", "t3" or the task's GUID, and only ever within this goal.</summary>
    private static GoalTask ResolveTask(TrackedGoal goal, string taskRef)
    {
        var wanted = taskRef.Trim();
        var task = goal.Tasks.FirstOrDefault(candidate =>
            string.Equals(candidate.ShortId, wanted, StringComparison.OrdinalIgnoreCase) ||
            string.Equals(candidate.Id, wanted, StringComparison.OrdinalIgnoreCase));

        if (task == null)
        {
            throw new GoalRuleException($"There is no task {wanted} in this goal. Its tasks are: {(goal.Tasks.Count == 0 ? "(none)" : string.Join(", ", goal.Tasks.Select(candidate => candidate.ShortId)))}.");
        }

        return task;
    }

    /// <summary>
    /// Inserts a task with the next ShortId from the goal's counter, which only ever
    /// increases - deleting T8 never lets a later task become T8.
    /// </summary>
    private GoalTask InsertTask(MutationContext context, string description, int ordinal, List<string> dependsOn)
    {
        var number = ToLong(context.Query("SELECT NextTaskNumber FROM Goals WHERE Id = @Id", P("@Id", context.Goal.Id))[0]["NextTaskNumber"]);
        if (number < 1)
        {
            number = 1;
        }

        context.Execute("UPDATE Goals SET NextTaskNumber = @Next WHERE Id = @Id", P("@Next", number + 1), P("@Id", context.Goal.Id));

        var task = new GoalTask
        {
            Id = Guid.NewGuid().ToString(),
            GoalId = context.Goal.Id,
            Ordinal = ordinal,
            ShortId = $"T{number}",
            Description = description.Trim(),
            Status = GoalTaskStatus.Todo,
            DependsOn = dependsOn,
            CreatedAt = context.Now,
            UpdatedAt = context.Now
        };

        context.Execute(@"
            INSERT INTO GoalTasks (Id, GoalId, Ordinal, ShortId, Description, Status, DependsOn, NeedsReview, CreatedAt, UpdatedAt)
            VALUES (@Id, @GoalId, @Ordinal, @ShortId, @Description, @Status, @DependsOn, 0, @CreatedAt, @UpdatedAt)",
            P("@Id", task.Id),
            P("@GoalId", task.GoalId),
            P("@Ordinal", task.Ordinal),
            P("@ShortId", task.ShortId),
            P("@Description", task.Description),
            P("@Status", task.Status.ToString()),
            P("@DependsOn", JsonSerializer.Serialize(dependsOn)),
            P("@CreatedAt", Iso(context.Now)),
            P("@UpdatedAt", Iso(context.Now)));

        return task;
    }

    private static void WriteTask(MutationContext context, GoalTask task)
    {
        task.UpdatedAt = context.Now;
        context.Execute(@"
            UPDATE GoalTasks SET
                Status = @Status, Note = @Note, BlockedReason = @BlockedReason, Evidence = @Evidence,
                EvidenceBy = @EvidenceBy, NeedsReview = @NeedsReview, ClaimedBy = @ClaimedBy,
                ClaimKind = @ClaimKind, ClaimedAt = @ClaimedAt, ClaimTouchedAt = @ClaimTouchedAt,
                UpdatedAt = @UpdatedAt, CompletedAt = @CompletedAt
            WHERE Id = @Id",
            P("@Status", task.Status.ToString()),
            P("@Note", task.Note),
            P("@BlockedReason", task.BlockedReason),
            P("@Evidence", task.Evidence),
            P("@EvidenceBy", task.EvidenceBy),
            P("@NeedsReview", task.NeedsReview ? 1 : 0),
            P("@ClaimedBy", task.ClaimedBy),
            P("@ClaimKind", task.ClaimKind?.ToString()),
            P("@ClaimedAt", task.ClaimedAt.HasValue ? Iso(task.ClaimedAt.Value) : null),
            P("@ClaimTouchedAt", task.ClaimTouchedAt.HasValue ? Iso(task.ClaimTouchedAt.Value) : null),
            P("@UpdatedAt", Iso(task.UpdatedAt)),
            P("@CompletedAt", task.CompletedAt.HasValue ? Iso(task.CompletedAt.Value) : null),
            P("@Id", task.Id));
    }

    /// <summary>Rewrites ordinals to match list order, contiguous from 1.</summary>
    private static void Renumber(MutationContext context)
    {
        for (var i = 0; i < context.Goal.Tasks.Count; i++)
        {
            var task = context.Goal.Tasks[i];
            if (task.Ordinal == i + 1)
            {
                continue;
            }

            task.Ordinal = i + 1;
            context.Execute("UPDATE GoalTasks SET Ordinal = @Ordinal WHERE Id = @Id", P("@Ordinal", task.Ordinal), P("@Id", task.Id));
        }
    }

    /// <summary>
    /// Work has been added to a goal that was finished or handed over for review: it is
    /// active again, in the same transaction as the change that reopened it.
    /// </summary>
    private static void ReopenIfClosed(MutationContext context, string reason)
    {
        if (context.Goal.Status != GoalStatus.Completed && context.Goal.Status != GoalStatus.AwaitingReview)
        {
            return;
        }

        var previous = context.Goal.Status;
        context.Goal.Status = GoalStatus.Active;
        context.Goal.CompletedAt = null;
        context.Goal.ReviewRequestedAt = null;
        context.Execute("UPDATE Goals SET Status = @Status, CompletedAt = NULL, ReviewRequestedAt = NULL WHERE Id = @Id",
            P("@Status", (int)GoalStatus.Active), P("@Id", context.Goal.Id));
        context.Journal(GoalJournalKinds.StatusChange, $"goal {previous} -> Active, reopened: {reason}");
        AutomatonDebug.Log($"GoalTrackingService.ReopenIfClosed: goal {context.Goal.Id} reopened from {previous} - {reason}");
    }
}
