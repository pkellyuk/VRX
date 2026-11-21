using System;
using System.Collections.Generic;
using System.Linq;
using Automaton.Core.Models;
using Automaton.Core.Models.Goals;

namespace Automaton.Core.Services.Goals;

public sealed partial class GoalTrackingService
{
    /// <summary>What the completion gate found. See GOALSUPDATE.md §5.4.</summary>
    public sealed class GateReport
    {
        /// <summary>Things that must be dealt with before anyone is asked to review.</summary>
        public List<string> Outstanding { get; } = new();

        /// <summary>Criteria that only the human's review can satisfy.</summary>
        public List<GoalCriterion> ForReview { get; } = new();

        /// <summary>
        /// The goal has no success criteria. Tasks alone are entirely under the model's
        /// control, so such a goal is never completed on the model's word: it is handed to the
        /// human like any other review - and "no plan yet" can never pass vacuously.
        /// </summary>
        public bool NoCriteria { get; set; }

        public bool NeedsHuman => ForReview.Count > 0 || NoCriteria;

        public bool Clear => Outstanding.Count == 0 && !NeedsHuman;
    }

    /// <summary>Evaluates the gate without changing anything.</summary>
    public GateReport EvaluateGate(TrackedGoal goal)
    {
        if (goal == null)
        {
            throw new ArgumentNullException(nameof(goal));
        }

        var report = new GateReport();

        report.NoCriteria = goal.Criteria.Count == 0;

        foreach (var task in goal.Tasks.OrderBy(task => task.Ordinal))
        {
            if (!task.IsFinished)
            {
                report.Outstanding.Add($"{task.ShortId} is {task.Status}: {Truncate(task.Description, 80)}");
                continue;
            }

            if (task.NeedsReview)
            {
                report.Outstanding.Add($"{task.ShortId} needs review because a task it depends on was reopened: mark it Done again with fresh evidence");
            }
        }

        if (goal.Tasks.Count > 0 && goal.Tasks.All(task => task.Status == GoalTaskStatus.Skipped))
        {
            report.Outstanding.Add("every task was skipped: at least one must actually be done");
        }

        foreach (var criterion in goal.Criteria.OrderBy(criterion => criterion.Ordinal))
        {
            var fresh = criterion.MetBy != GoalProofSource.Check || criterion.CheckedWorkRevision == goal.WorkRevision;
            if (criterion.IsSatisfied && fresh)
            {
                continue;
            }

            if (criterion.IsSatisfied && !fresh)
            {
                report.Outstanding.Add($"{criterion.ShortId} was checked before the work last changed: run its check again");
                continue;
            }

            if (criterion.RequiredProof == GoalProofKind.Human)
            {
                report.ForReview.Add(criterion);
                continue;
            }

            if (criterion.HasCheckCommand)
            {
                // Until checks can be run, a command-backed criterion is confirmed by the human.
                if (!ChecksExecutable)
                {
                    report.ForReview.Add(criterion);
                    continue;
                }

                report.Outstanding.Add($"{criterion.ShortId} needs its check to pass ({Truncate(criterion.CheckCommand!, 60)}): {Truncate(criterion.Description, 80)}");
                continue;
            }

            if (criterion.Status == GoalCriterionStatus.Met)
            {
                report.Outstanding.Add($"{criterion.ShortId} was met by {criterion.MetBy}, but now needs {criterion.RequiredProof} proof");
                continue;
            }

            report.Outstanding.Add($"{criterion.ShortId} is {criterion.Status}: {Truncate(criterion.Description, 80)}");
        }

        foreach (var pending in goal.PendingScopeChanges)
        {
            report.Outstanding.Add($"awaiting human approval: {pending.Describe()}");
        }

        return report;
    }

    /// <summary>
    /// Asks for the goal to be completed. Completes it, hands it to the human for their one
    /// review, or refuses with what is outstanding. A refusal is journalled and then thrown.
    /// </summary>
    /// <param name="overrideGate">Honoured only for a human caller.</param>
    public GoalCompletionResult RequestCompletion(GoalCaller caller, string goalId, string? summary, bool overrideGate = false)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        AutomatonDebug.Log($"GoalTrackingService.RequestCompletion: Entry - goalId={goalId}, overrideGate={overrideGate}, caller={caller}");

        var reviewOff = string.Equals(Setting(ReviewModeSetting, caller.ProjectId), "none", StringComparison.OrdinalIgnoreCase);
        var result = new GoalCompletionResult();
        Mutate(caller, goalId, null, context =>
        {
            var goal = context.Goal;
            if (goal.Status == GoalStatus.Completed)
            {
                result.Outcome = GoalCompletionOutcome.Completed;
                return;
            }

            if (goal.Status == GoalStatus.Cancelled)
            {
                throw new GoalRuleException("This goal was cancelled. Make it active again before completing it.");
            }

            var report = EvaluateGate(goal);
            var isHuman = context.Caller.IsHuman;

            if (report.Outstanding.Count > 0 && !(isHuman && overrideGate))
            {
                context.Journal(GoalJournalKinds.Gate, $"completion refused for {context.Caller.Author}: {string.Join("; ", report.Outstanding)}");
                AutomatonDebug.Log($"GoalTrackingService.RequestCompletion: refused - goal {goal.Id}, {report.Outstanding.Count} outstanding: {string.Join("; ", report.Outstanding)}");
                context.DeferredRefusal = new GoalGateRefusedException(report.Outstanding);
                return;
            }

            if (isHuman)
            {
                // A person completing the goal is the review: their criteria are confirmed by it.
                foreach (var criterion in report.ForReview)
                {
                    ConfirmByHuman(context, criterion);
                }

                if (report.Outstanding.Count > 0)
                {
                    context.Journal(GoalJournalKinds.Gate, $"human completed the goal over the gate. Outstanding at the time: {string.Join("; ", report.Outstanding)}");
                    AutomatonDebug.LogWarning($"GoalTrackingService.RequestCompletion: OVERRIDE - goal {goal.Id} completed by human with {report.Outstanding.Count} outstanding");
                }

                Complete(context, summary);
                result.Outcome = GoalCompletionOutcome.Completed;
                return;
            }

            // With review switched off for the project, a goal without criteria is the
            // model's to close; criteria that ask for a human still wait for one.
            var needsHuman = report.ForReview.Count > 0 || (report.NoCriteria && !reviewOff);
            if (needsHuman)
            {
                HandOver(context, summary, report.ForReview);
                result.Outcome = GoalCompletionOutcome.AwaitingReview;
                result.ForReview = report.ForReview.Select(criterion => $"{criterion.ShortId} {Truncate(criterion.Description, 80)}").ToList();
                if (report.NoCriteria)
                {
                    result.ForReview.Add("the goal as a whole, because it has no success criteria");
                }

                return;
            }

            Complete(context, summary);
            result.Outcome = GoalCompletionOutcome.Completed;
        });

        result.Goal = GetGoal(caller, goalId);
        AutomatonDebug.Log($"GoalTrackingService.RequestCompletion: Exit - goalId={goalId}, outcome={result.Outcome}, status={result.Goal.Status}");
        return result;
    }

    /// <summary>
    /// Moves a goal between statuses. Completed goes through the gate; AwaitingReview cannot be
    /// set directly because it is only ever the result of asking to complete.
    /// </summary>
    public TrackedGoal SetGoalStatus(GoalCaller caller, string goalId, GoalStatus status, string? reason = null, bool overrideGate = false)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        AutomatonDebug.Log($"GoalTrackingService.SetGoalStatus: Entry - goalId={goalId}, status={status}, caller={caller}");

        if (status == GoalStatus.Completed)
        {
            var completion = RequestCompletion(caller, goalId, reason, overrideGate);
            AutomatonDebug.Log($"GoalTrackingService.SetGoalStatus: Exit - goalId={goalId}, completion outcome={completion.Outcome}");
            return completion.Goal;
        }

        if (status == GoalStatus.AwaitingReview)
        {
            throw new GoalRuleException("A goal is handed over for review by asking to complete it, not by setting that status.");
        }

        Mutate(caller, goalId, null, context =>
        {
            var previous = context.Goal.Status;
            if (previous == status)
            {
                return;
            }

            context.Goal.Status = status;
            context.Execute("UPDATE Goals SET Status = @Status, CompletedAt = NULL, ReviewRequestedAt = NULL WHERE Id = @Id",
                P("@Status", (int)status), P("@Id", context.Goal.Id));
            context.Journal(GoalJournalKinds.StatusChange, $"goal {previous} -> {status}{(string.IsNullOrWhiteSpace(reason) ? string.Empty : $" | {Truncate(reason!, 300)}")}");
        });

        var goal = GetGoal(caller, goalId);
        AutomatonDebug.Log($"GoalTrackingService.SetGoalStatus: Exit - goalId={goalId}, status={goal.Status}");
        return goal;
    }

    // ── The human's one review (GOALSUPDATE.md §5.6) ──────────────────────

    /// <summary>Accepts the whole goal: every criterion waiting on the human is confirmed at once.</summary>
    public TrackedGoal AcceptReview(GoalCaller caller, string goalId)
    {
        RequireHumanReviewer(caller, "accept");
        AutomatonDebug.Log($"GoalTrackingService.AcceptReview: Entry - goalId={goalId}");

        Mutate(caller, goalId, null, context =>
        {
            if (context.Goal.Status != GoalStatus.AwaitingReview)
            {
                throw new GoalRuleException($"This goal is {context.Goal.Status}, not awaiting review.");
            }

            var report = EvaluateGate(context.Goal);
            if (report.Outstanding.Count > 0)
            {
                throw new GoalGateRefusedException(report.Outstanding);
            }

            foreach (var criterion in report.ForReview)
            {
                ConfirmByHuman(context, criterion);
            }

            context.Journal(GoalJournalKinds.Review, $"human accepted the goal, confirming {report.ForReview.Count} criteria");
            Complete(context, null);
        });

        var goal = GetGoal(caller, goalId);
        AutomatonDebug.Log($"GoalTrackingService.AcceptReview: Exit - goalId={goalId}, status={goal.Status}");
        return goal;
    }

    /// <summary>
    /// Sends the goal back with a comment. The named criteria become NotMet, one task carrying
    /// the comment becomes the next runnable work, and the goal is active again - so the chat
    /// working on it sees what to do without the human explaining it twice.
    /// </summary>
    public TrackedGoal SendBackReview(GoalCaller caller, string goalId, string comment, IEnumerable<string>? criterionRefs = null)
    {
        RequireHumanReviewer(caller, "send back");
        if (string.IsNullOrWhiteSpace(comment))
        {
            throw new GoalRuleException("Say what needs to change when sending a goal back.");
        }

        AutomatonDebug.Log($"GoalTrackingService.SendBackReview: Entry - goalId={goalId}");

        Mutate(caller, goalId, null, context =>
        {
            if (context.Goal.Status != GoalStatus.AwaitingReview)
            {
                throw new GoalRuleException($"This goal is {context.Goal.Status}, not awaiting review.");
            }

            var trimmed = comment.Trim();
            var named = new List<string>();
            foreach (var reference in NormalizeRefs(criterionRefs))
            {
                var criterion = ResolveCriterion(context.Goal, reference);
                criterion.Status = GoalCriterionStatus.NotMet;
                criterion.MetBy = null;
                criterion.Evidence = Cap($"Review: {trimmed}", MaxEvidence);
                WriteCriterion(context, criterion);
                named.Add(criterion.ShortId);
            }

            var description = named.Count == 0
                ? $"Address review: {trimmed}"
                : $"Address review ({string.Join(", ", named)}): {trimmed}";
            var task = InsertTask(context, description, context.Goal.Tasks.Count + 1, new List<string>());

            // Ahead of any other open work, so it is what the digest offers next.
            var firstOpen = context.Goal.Tasks.FindIndex(existing => !existing.IsFinished);
            context.Goal.Tasks.Insert(firstOpen < 0 ? context.Goal.Tasks.Count : firstOpen, task);
            Renumber(context);
            context.BumpWork();

            context.Goal.Status = GoalStatus.Active;
            context.Execute("UPDATE Goals SET Status = @Status, ReviewRequestedAt = NULL, CompletedAt = NULL WHERE Id = @Id",
                P("@Status", (int)GoalStatus.Active), P("@Id", context.Goal.Id));
            context.Journal(GoalJournalKinds.Review, $"human sent the goal back{(named.Count == 0 ? string.Empty : $" against {string.Join(", ", named)}")}: {trimmed}", task.Id);
        });

        var goal = GetGoal(caller, goalId);
        AutomatonDebug.Log($"GoalTrackingService.SendBackReview: Exit - goalId={goalId}, status={goal.Status}, tasks={goal.Tasks.Count}");
        return goal;
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    private static void RequireHumanReviewer(GoalCaller caller, string verb)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (caller.IsHuman)
        {
            return;
        }

        AutomatonDebug.LogWarning($"GoalTrackingService.RequireHumanReviewer: refused - {caller} tried to {verb} a review");
        throw new GoalRuleException("Only the human can review a goal.");
    }

    private void ConfirmByHuman(MutationContext context, GoalCriterion criterion)
    {
        var previous = criterion.Status;
        criterion.Status = GoalCriterionStatus.Met;
        criterion.MetBy = GoalProofSource.Human;
        criterion.CheckedAt = context.Now;
        WriteCriterion(context, criterion);
        context.Journal(GoalJournalKinds.StatusChange, $"{criterion.ShortId} {previous} -> Met (by Human)");
    }

    private void Complete(MutationContext context, string? summary)
    {
        var previous = context.Goal.Status;
        context.Goal.Status = GoalStatus.Completed;
        context.Goal.CompletedAt = context.Now;
        context.Execute(@"
            UPDATE Goals SET Status = @Status, CompletedAt = @CompletedAt, ReviewRequestedAt = NULL,
                   ReviewSummary = COALESCE(@Summary, ReviewSummary)
            WHERE Id = @Id",
            P("@Status", (int)GoalStatus.Completed),
            P("@CompletedAt", Iso(context.Now)),
            P("@Summary", Cap(summary, MaxEvidence)),
            P("@Id", context.Goal.Id));
        context.Journal(GoalJournalKinds.StatusChange, $"goal {previous} -> Completed{(string.IsNullOrWhiteSpace(summary) ? string.Empty : $" | {Truncate(summary!.Trim(), 300)}")}");
    }

    private void HandOver(MutationContext context, string? summary, List<GoalCriterion> forReview)
    {
        var previous = context.Goal.Status;
        context.Goal.Status = GoalStatus.AwaitingReview;
        context.Goal.ReviewRequestedAt = context.Now;
        context.Execute("UPDATE Goals SET Status = @Status, ReviewSummary = @Summary, ReviewRequestedAt = @Now WHERE Id = @Id",
            P("@Status", (int)GoalStatus.AwaitingReview),
            P("@Summary", Cap(summary, MaxEvidence)),
            P("@Now", Iso(context.Now)),
            P("@Id", context.Goal.Id));
        var toConfirm = forReview.Count == 0 ? "no criteria, so the goal as a whole" : string.Join(", ", forReview.Select(criterion => criterion.ShortId));
        context.Journal(GoalJournalKinds.Review, $"goal {previous} -> AwaitingReview, handed to the human with {toConfirm} to confirm");
        AutomatonDebug.Log($"GoalTrackingService.HandOver: goal {context.Goal.Id} handed over for review with {forReview.Count} criteria");
    }
}
