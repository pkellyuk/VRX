using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Automaton.Core.Models;
using Automaton.Core.Models.Goals;

namespace Automaton.Core.Services.Goals;

/// <summary>What the loop should do next, and what to tell the human if it is stopping.</summary>
public sealed class GoalLoopDecision
{
    public bool Proceed { get; set; }

    /// <summary>The text to inject as the next turn, when proceeding.</summary>
    public string? Prompt { get; set; }

    /// <summary>Why the loop is not proceeding. Written for the human.</summary>
    public string? StopReason { get; set; }

    public string? GoalId { get; set; }

    public static GoalLoopDecision Go(string prompt, string? goalId)
    {
        return new GoalLoopDecision { Proceed = true, Prompt = prompt, GoalId = goalId };
    }

    public static GoalLoopDecision Halt(string reason, string? goalId = null)
    {
        return new GoalLoopDecision { Proceed = false, StopReason = reason, GoalId = goalId };
    }
}

/// <summary>The stored state of one chat's automaton loop.</summary>
public sealed class GoalLoopState
{
    public string SessionId { get; set; } = string.Empty;
    public string? ProjectId { get; set; }
    public string? GoalId { get; set; }
    public string Directory { get; set; } = string.Empty;
    public bool IsRunning { get; set; }
    public DateTime? StartedAt { get; set; }
    public DateTime? LastIterationAt { get; set; }
    public int Iterations { get; set; }
    public DateTime? BudgetStartedAt { get; set; }
    public string? LastProgressKey { get; set; }
    public int StalledIterations { get; set; }
    public string? LastError { get; set; }
}

/// <summary>
/// The rules of the automaton loop (GOALSUPDATE.md §8.3): it belongs to one chat and one goal,
/// it stops when there is nothing left for a model to do, it notices when turns go by without
/// recorded progress, and it has a budget that a model cannot refill by stopping and starting.
/// Timing - when the next turn is due, whether one is in flight - stays with the caller.
/// </summary>
public sealed partial class GoalTrackingService
{
    public const string LoopStallLimitSetting = "goals.loopStallLimit";
    public const string LoopMaxIterationsSetting = "goals.loopMaxIterations";
    public const string LoopMaxHoursSetting = "goals.loopMaxHours";

    private const string LoopMarker = "[AUTOMATION] This prompt was injected by the automaton scheduler, not typed by the human user.";

    /// <summary>The prompt for a chat that has no project, and so no goal to steer by.</summary>
    public const string LegacyLoopPrompt =
        LoopMarker + " Please continue to implement the goals, then update each goal's status until we reach 100% completion.";

    // No tool is named here: the goal status attached to the request says which tools this
    // chat has, and a chat created before the update has none of the new ones.
    private const string GoalLoopPrompt =
        LoopMarker + " Continue with the task marked now in the goal status below. If none is marked, take the next runnable task. " +
        "Record progress as you go, and when every task is finished record where the criteria stand and finish the goal properly. " +
        "Do not ask the human to confirm anything; what needs them is reviewed once, at the end.";

    /// <summary>Raised when a loop stops for any reason other than being asked to: (sessionId, reason).</summary>
    public static event Action<string, string>? LoopStopped;

    /// <summary>
    /// Starts, or resumes, the loop for a chat. A human starting it gets a fresh budget; a
    /// model starting it again does not, or stopping and starting would be a way round the cap.
    /// </summary>
    public GoalLoopDecision LoopStart(string sessionId, string? projectId, string? directory, bool startedByHuman)
    {
        if (string.IsNullOrWhiteSpace(sessionId))
        {
            throw new ArgumentException("sessionId is required", nameof(sessionId));
        }

        AutomatonDebug.Log($"GoalTrackingService.LoopStart: Entry - session={sessionId}, project={projectId ?? "(none)"}, byHuman={startedByHuman}");

        string? goalId = null;
        if (!string.IsNullOrWhiteSpace(projectId))
        {
            var choice = ChooseLoopGoal(sessionId, projectId!);
            if (!choice.Proceed)
            {
                AutomatonDebug.Log($"GoalTrackingService.LoopStart: Exit - refused: {choice.StopReason}");
                return choice;
            }

            goalId = choice.GoalId;
        }

        var now = _clock();
        var existing = GetLoop(sessionId);
        var freshBudget = existing == null || startedByHuman || !string.Equals(existing.GoalId, goalId, StringComparison.OrdinalIgnoreCase);
        var progressKey = goalId == null ? null : ProgressKey(GetGoal(GoalCaller.ForSystem(projectId), goalId));

        _db.Execute(@"
            INSERT INTO AutomatonLoops
                (SessionId, ProjectId, GoalId, Directory, IsRunning, StartedAt, LastIterationAt, Iterations, BudgetStartedAt, LastProgressKey, StalledIterations, LastError)
            VALUES
                (@SessionId, @ProjectId, @GoalId, @Directory, 1, @Now, NULL, 0, @Now, @ProgressKey, 0, NULL)
            ON CONFLICT(SessionId) DO UPDATE SET
                ProjectId = @ProjectId, GoalId = @GoalId, Directory = @Directory, IsRunning = 1, StartedAt = @Now,
                LastError = NULL, LastProgressKey = @ProgressKey, StalledIterations = 0,
                Iterations = CASE WHEN @FreshBudget = 1 THEN 0 ELSE Iterations END,
                BudgetStartedAt = CASE WHEN @FreshBudget = 1 THEN @Now ELSE COALESCE(BudgetStartedAt, @Now) END",
            P("@SessionId", sessionId),
            P("@ProjectId", projectId),
            P("@GoalId", goalId),
            P("@Directory", directory ?? string.Empty),
            P("@Now", Iso(now)),
            P("@ProgressKey", progressKey),
            P("@FreshBudget", freshBudget ? 1 : 0));

        if (goalId != null)
        {
            JournalLoop(projectId!, goalId, sessionId, $"automaton loop started by {(startedByHuman ? "the human" : "the model")}{(freshBudget ? string.Empty : ", budget carried over")}");
        }

        AutomatonDebug.Log($"GoalTrackingService.LoopStart: Exit - session={sessionId}, goal={goalId ?? "(none)"}, freshBudget={freshBudget}");
        return GoalLoopDecision.Go(goalId == null ? LegacyLoopPrompt : GoalLoopPrompt, goalId);
    }

    /// <summary>Asked before each injected turn. Stops the loop itself when there is nothing to do.</summary>
    public GoalLoopDecision LoopBeforeIteration(string sessionId)
    {
        if (string.IsNullOrWhiteSpace(sessionId))
        {
            throw new ArgumentException("sessionId is required", nameof(sessionId));
        }

        var loop = GetLoop(sessionId);
        if (loop == null || !loop.IsRunning)
        {
            return GoalLoopDecision.Halt("The automaton loop is not running for this chat.");
        }

        var limits = LoopLimits(loop.ProjectId);
        if (loop.Iterations >= limits.MaxIterations)
        {
            return StopLoop(loop, $"The automaton loop has used its budget of {limits.MaxIterations} turns. Start it again to give it more.");
        }

        var budgetStarted = loop.BudgetStartedAt ?? loop.StartedAt ?? _clock();
        if (_clock() - budgetStarted >= limits.MaxDuration)
        {
            return StopLoop(loop, $"The automaton loop has run for its limit of {limits.MaxDuration.TotalHours:0.#} hours. Start it again to give it more.");
        }

        if (string.IsNullOrWhiteSpace(loop.ProjectId))
        {
            AutomatonDebug.Log($"GoalTrackingService.LoopBeforeIteration: session={sessionId} has no project - legacy prompt, iteration {loop.Iterations + 1}/{limits.MaxIterations}");
            return GoalLoopDecision.Go(LegacyLoopPrompt, null);
        }

        // The chat may have chosen another goal since the last turn: follow it.
        var choice = ChooseLoopGoal(sessionId, loop.ProjectId!);
        if (!choice.Proceed)
        {
            return StopLoop(loop, choice.StopReason ?? "There is no goal for the automaton loop to work on.");
        }

        if (!string.Equals(choice.GoalId, loop.GoalId, StringComparison.OrdinalIgnoreCase))
        {
            AutomatonDebug.Log($"GoalTrackingService.LoopBeforeIteration: session={sessionId} moved from goal {loop.GoalId ?? "-"} to {choice.GoalId}; budget restarts");
            var moved = GetGoal(GoalCaller.ForSystem(loop.ProjectId), choice.GoalId!);
            _db.Execute(@"
                UPDATE AutomatonLoops SET GoalId = @GoalId, Iterations = 0, BudgetStartedAt = @Now, StalledIterations = 0, LastProgressKey = @Key
                WHERE SessionId = @SessionId",
                P("@GoalId", choice.GoalId), P("@Now", Iso(_clock())), P("@Key", ProgressKey(moved)), P("@SessionId", sessionId));
        }

        AutomatonDebug.Log($"GoalTrackingService.LoopBeforeIteration: session={sessionId}, goal={choice.GoalId}, iteration {loop.Iterations + 1}/{limits.MaxIterations}, stalled={loop.StalledIterations}/{limits.StallLimit}");
        return GoalLoopDecision.Go(GoalLoopPrompt, choice.GoalId);
    }

    /// <summary>
    /// Asked after each injected turn. Measures progress, not activity: only a change to the
    /// work itself, or one more criterion met, counts. Notes, refused completions and the
    /// loop's own records do not, so a model that writes a note every turn, or asks to finish
    /// and is refused every turn, is correctly seen as going nowhere.
    /// </summary>
    public GoalLoopDecision LoopAfterIteration(string sessionId, string? error)
    {
        if (string.IsNullOrWhiteSpace(sessionId))
        {
            throw new ArgumentException("sessionId is required", nameof(sessionId));
        }

        var loop = GetLoop(sessionId);
        if (loop == null)
        {
            return GoalLoopDecision.Halt("The automaton loop is not running for this chat.");
        }

        var now = _clock();
        var limits = LoopLimits(loop.ProjectId);
        string? progressKey = null;
        TrackedGoal? goal = null;

        if (!string.IsNullOrWhiteSpace(loop.ProjectId) && !string.IsNullOrWhiteSpace(loop.GoalId))
        {
            try
            {
                goal = GetGoal(GoalCaller.ForSystem(loop.ProjectId), loop.GoalId!);
                progressKey = ProgressKey(goal);
            }
            catch (GoalNotFoundException)
            {
                goal = null;
            }
        }

        var progressed = goal != null && !string.Equals(progressKey, loop.LastProgressKey, StringComparison.Ordinal);
        var stalled = goal == null || progressed ? 0 : loop.StalledIterations + 1;

        _db.Execute(@"
            UPDATE AutomatonLoops SET Iterations = Iterations + 1, LastIterationAt = @Now, LastProgressKey = COALESCE(@Key, LastProgressKey),
                   StalledIterations = @Stalled, LastError = @Error
            WHERE SessionId = @SessionId",
            P("@Now", Iso(now)), P("@Key", progressKey), P("@Stalled", stalled), P("@Error", Cap(error, 500)), P("@SessionId", sessionId));

        loop.Iterations++;
        loop.StalledIterations = stalled;
        AutomatonDebug.Log($"GoalTrackingService.LoopAfterIteration: session={sessionId}, goal={loop.GoalId ?? "-"}, iteration={loop.Iterations}, key={progressKey ?? "-"}, progressed={progressed}, stalled={stalled}/{limits.StallLimit}, error={(error == null ? "none" : "yes")}");

        if (!loop.IsRunning)
        {
            return GoalLoopDecision.Halt("The automaton loop was stopped.", loop.GoalId);
        }

        if (string.IsNullOrWhiteSpace(loop.GoalId))
        {
            return GoalLoopDecision.Go(LegacyLoopPrompt, null);
        }

        if (goal == null)
        {
            return StopLoop(loop, "The goal the automaton loop was working on no longer exists.");
        }

        if (goal.Status == GoalStatus.AwaitingReview)
        {
            return StopLoop(loop, $"\"{Truncate(goal.Title, 60)}\" is finished and handed over for your review. The automaton loop has stopped.");
        }

        if (goal.Status == GoalStatus.Completed)
        {
            return StopLoop(loop, $"\"{Truncate(goal.Title, 60)}\" is complete. The automaton loop has stopped.");
        }

        if (!goal.IsActive)
        {
            return StopLoop(loop, $"\"{Truncate(goal.Title, 60)}\" is now {goal.Status}, so the automaton loop has stopped.");
        }

        // Everything a model can do is done and the rest is the human's decision: that is
        // waiting, not stalling, and nothing gets blocked for it.
        var gate = EvaluateGate(goal);
        var onlyApprovalLeft = gate.Outstanding.Count > 0 && gate.Outstanding.All(item => item.StartsWith("awaiting human approval", StringComparison.Ordinal));
        if (onlyApprovalLeft)
        {
            return StopLoop(loop, $"\"{Truncate(goal.Title, 60)}\" is waiting for you to approve a change to its criteria. The automaton loop has stopped until you decide.");
        }

        if (stalled < limits.StallLimit)
        {
            return GoalLoopDecision.Go(GoalLoopPrompt, loop.GoalId);
        }

        var reason = $"no recorded progress in {stalled} automated turns";
        var blocked = BlockStalledTask(loop, goal, reason);
        return StopLoop(loop, $"The automaton loop stopped: {reason} on \"{Truncate(goal.Title, 60)}\"{(blocked == null ? string.Empty : $", so {blocked} was marked blocked")}. A turn that does work but does not record it counts as no progress.");
    }

    /// <summary>Stops the loop because someone asked. Does not raise <see cref="LoopStopped"/>.</summary>
    public void LoopStop(string sessionId, string reason)
    {
        if (string.IsNullOrWhiteSpace(sessionId))
        {
            return;
        }

        var loop = GetLoop(sessionId);
        if (loop == null || !loop.IsRunning)
        {
            return;
        }

        _db.Execute("UPDATE AutomatonLoops SET IsRunning = 0, LastError = @Reason WHERE SessionId = @SessionId", P("@Reason", Cap(reason, 500)), P("@SessionId", sessionId));
        if (!string.IsNullOrWhiteSpace(loop.ProjectId) && !string.IsNullOrWhiteSpace(loop.GoalId))
        {
            JournalLoop(loop.ProjectId!, loop.GoalId!, sessionId, $"automaton loop stopped: {reason}");
        }

        AutomatonDebug.Log($"GoalTrackingService.LoopStop: session={sessionId} - {reason}");
    }

    /// <summary>
    /// Nothing is running at startup, so a stored "running" is a lie. Loops are not resumed:
    /// a loop is only ever started by someone choosing to start it.
    /// </summary>
    public int LoopReconcileAtStartup()
    {
        var stopped = _db.Execute("UPDATE AutomatonLoops SET IsRunning = 0, LastError = 'stopped by restart' WHERE IsRunning = 1");
        AutomatonDebug.Log($"GoalTrackingService.LoopReconcileAtStartup: {stopped} loop(s) marked stopped by restart");
        return stopped;
    }

    public GoalLoopState? GetLoop(string sessionId)
    {
        if (string.IsNullOrWhiteSpace(sessionId))
        {
            return null;
        }

        var rows = _db.Query("SELECT * FROM AutomatonLoops WHERE SessionId = @SessionId", P("@SessionId", sessionId));
        if (rows.Count == 0)
        {
            return null;
        }

        var row = rows[0];
        return new GoalLoopState
        {
            SessionId = row["SessionId"] as string ?? string.Empty,
            ProjectId = row["ProjectId"] as string,
            GoalId = row["GoalId"] as string,
            Directory = row["Directory"] as string ?? string.Empty,
            IsRunning = ToLong(row["IsRunning"]) != 0,
            StartedAt = ParseTimestamp(row["StartedAt"]),
            LastIterationAt = ParseTimestamp(row["LastIterationAt"]),
            Iterations = (int)ToLong(row["Iterations"]),
            BudgetStartedAt = ParseTimestamp(row["BudgetStartedAt"]),
            LastProgressKey = row["LastProgressKey"] as string,
            StalledIterations = (int)ToLong(row["StalledIterations"]),
            LastError = row["LastError"] as string
        };
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    /// <summary>
    /// The goal a loop works on: the one the chat is bound to, or the project's only active
    /// goal, which is then bound. With several and no binding the loop will not guess.
    /// </summary>
    private GoalLoopDecision ChooseLoopGoal(string sessionId, string projectId)
    {
        var system = GoalCaller.ForSystem(projectId);
        var boundId = GetBinding(sessionId);
        if (boundId != null)
        {
            try
            {
                var bound = GetGoal(system, boundId);
                if (bound.IsActive)
                {
                    return GoalLoopDecision.Go(GoalLoopPrompt, bound.Id);
                }

                if (bound.Status == GoalStatus.AwaitingReview)
                {
                    return GoalLoopDecision.Halt($"\"{Truncate(bound.Title, 60)}\" is finished and handed over for your review, so there is nothing for the automaton loop to do.", bound.Id);
                }

                if (bound.Status == GoalStatus.Completed)
                {
                    return GoalLoopDecision.Halt($"\"{Truncate(bound.Title, 60)}\" is complete, so there is nothing for the automaton loop to do.", bound.Id);
                }
            }
            catch (GoalNotFoundException)
            {
                AutomatonDebug.Log($"GoalTrackingService.ChooseLoopGoal: session {sessionId} is bound to goal {boundId}, which is not in project {projectId}");
            }
        }

        var active = ListGoals(system, GoalStatus.Active).Concat(ListGoals(system, GoalStatus.Blocked)).ToList();
        if (active.Count == 0)
        {
            return GoalLoopDecision.Halt("This project has no active goal, so there is nothing for the automaton loop to do.");
        }

        if (active.Count > 1)
        {
            return GoalLoopDecision.Halt($"This project has {active.Count} active goals and this chat has not chosen one. Choose the goal to work on, then start the automaton loop.");
        }

        var only = active[0].Goal;
        Bind(system, sessionId, only.Id);
        AutomatonDebug.Log($"GoalTrackingService.ChooseLoopGoal: session {sessionId} bound to the project's only active goal {only.Id}");
        return GoalLoopDecision.Go(GoalLoopPrompt, only.Id);
    }

    /// <summary>What the work is and how far it has got, plus how many criteria are met. Nothing else.</summary>
    private static string ProgressKey(TrackedGoal goal)
    {
        return $"{goal.WorkRevision}:{goal.Criteria.Count(criterion => criterion.IsSatisfied)}";
    }

    private string? BlockStalledTask(GoalLoopState loop, TrackedGoal goal, string reason)
    {
        var claimant = $"chat:{loop.SessionId}";
        var task = goal.Tasks.FirstOrDefault(candidate => candidate.Status == GoalTaskStatus.Doing && string.Equals(candidate.ClaimedBy, claimant, StringComparison.Ordinal));
        if (task == null)
        {
            JournalLoop(loop.ProjectId!, goal.Id, loop.SessionId, $"automaton loop stalled: {reason}; this chat held no task");
            return null;
        }

        try
        {
            SetTaskStatus(GoalCaller.ForSystem(loop.ProjectId), goal.Id, task.ShortId, GoalTaskStatus.Blocked, blockedReason: reason);
            JournalLoop(loop.ProjectId!, goal.Id, loop.SessionId, $"automaton loop stalled: {reason}; {task.ShortId} marked blocked");
            return task.ShortId;
        }
        catch (GoalRuleException ex)
        {
            AutomatonDebug.LogWarning($"GoalTrackingService.BlockStalledTask: could not block {task.ShortId} - {ex.Message}");
            return null;
        }
    }

    private GoalLoopDecision StopLoop(GoalLoopState loop, string reason)
    {
        _db.Execute("UPDATE AutomatonLoops SET IsRunning = 0, LastError = @Reason WHERE SessionId = @SessionId", P("@Reason", Cap(reason, 500)), P("@SessionId", loop.SessionId));
        if (!string.IsNullOrWhiteSpace(loop.ProjectId) && !string.IsNullOrWhiteSpace(loop.GoalId))
        {
            try
            {
                JournalLoop(loop.ProjectId!, loop.GoalId!, loop.SessionId, $"automaton loop stopped itself: {reason}");
            }
            catch (GoalNotFoundException)
            {
                // The goal went away; the log line below is the record.
            }
        }

        AutomatonDebug.Log($"GoalTrackingService.StopLoop: session={loop.SessionId}, goal={loop.GoalId ?? "-"}, iterations={loop.Iterations} - {reason}");

        try
        {
            LoopStopped?.Invoke(loop.SessionId, reason);
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"GoalTrackingService.StopLoop: a LoopStopped subscriber threw - {ex.Message}");
        }

        return GoalLoopDecision.Halt(reason, loop.GoalId);
    }

    /// <summary>A Loop journal row. Loop rows are never counted as progress.</summary>
    private void JournalLoop(string projectId, string goalId, string sessionId, string content)
    {
        Mutate(GoalCaller.ForSystem(projectId), goalId, null, context => context.Journal(GoalJournalKinds.Loop, $"{content} (chat {sessionId})"));
    }

    private (int StallLimit, int MaxIterations, TimeSpan MaxDuration) LoopLimits(string? projectId)
    {
        return (
            PositiveSetting(LoopStallLimitSetting, projectId, 3),
            PositiveSetting(LoopMaxIterationsSetting, projectId, 50),
            TimeSpan.FromHours(PositiveSetting(LoopMaxHoursSetting, projectId, 4)));
    }

    private int PositiveSetting(string key, string? projectId, int fallback)
    {
        var value = Setting(key, projectId);
        if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed) && parsed > 0)
        {
            return parsed;
        }

        return fallback;
    }
}
