using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Automaton.Core.Models.Goals;

namespace Automaton.Core.Services.Goals;

/// <summary>How one requested check ended.</summary>
public enum GoalCheckResult
{
    Met,
    NotMet,
    TimedOut,
    Cancelled,

    /// <summary>The plan, the command or the folder changed while it ran, so the result proves nothing.</summary>
    Discarded,

    /// <summary>Never started, because an earlier check in the same call was cancelled.</summary>
    NotRun
}

public sealed class GoalCheckOutcome
{
    public string ShortId { get; set; } = string.Empty;
    public string Description { get; set; } = string.Empty;
    public string Command { get; set; } = string.Empty;
    public GoalCheckResult Result { get; set; }
    public int? ExitCode { get; set; }
    public string EvidenceTail { get; set; } = string.Empty;
    public TimeSpan Duration { get; set; }
    public string? Explanation { get; set; }
}

/// <summary>
/// Running a criterion's check command (GOALSUPDATE.md §5.5). Freshness is by version, not by
/// clock: a run records the goal's WorkRevision when it starts and its result is accepted only
/// if that, the command and the working folder are all still the same when it finishes. So a
/// check that finishes late can never produce a pass for work it did not see.
/// </summary>
public sealed partial class GoalTrackingService
{
    public const string CheckTimeoutSetting = "goals.checkTimeoutSeconds";

    private static readonly HashSet<string> CheckRunsReconciled = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// Runs the check of one criterion, or of every criterion that has one. Sequential, each
    /// accepted or discarded on its own; a failure does not stop the rest, a cancellation does.
    /// </summary>
    /// <param name="approve">
    /// Asked once per command before anything runs, with the command and its folder. Throwing
    /// refuses the whole call. A tool passes the chat's shell permission path here; a human
    /// pressing a button passes null, because the press is the approval.
    /// </param>
    public async Task<List<GoalCheckOutcome>> RunChecksAsync(
        GoalCaller caller,
        string goalId,
        string? criterionRef,
        IGoalCheckRunner runner,
        Func<string, string, Task>? approve,
        CancellationToken cancellationToken = default)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (runner == null)
        {
            throw new ArgumentNullException(nameof(runner));
        }

        AutomatonDebug.Log($"GoalTrackingService.RunChecksAsync: Entry - goalId={goalId}, criterion={criterionRef ?? "(all)"}, caller={caller}");

        var goal = GetGoal(caller, goalId);
        List<GoalCriterion> wanted;
        if (string.IsNullOrWhiteSpace(criterionRef))
        {
            wanted = goal.Criteria.Where(criterion => criterion.HasCheckCommand).OrderBy(criterion => criterion.Ordinal).ToList();
            if (wanted.Count == 0)
            {
                throw new GoalRuleException("None of this goal's criteria has a check command, so there is nothing to run.");
            }
        }
        else
        {
            var one = ResolveCriterion(goal, criterionRef!);
            if (!one.HasCheckCommand)
            {
                throw new GoalRuleException($"{one.ShortId} has no check command. Give it one, or record where it stands with your evidence.");
            }

            wanted = new List<GoalCriterion> { one };
        }

        // Every command is approved before any of them runs, so a refusal leaves no half-run batch.
        if (approve != null)
        {
            foreach (var criterion in wanted)
            {
                await approve(criterion.CheckCommand!, goal.WorkingFolder).ConfigureAwait(false);
            }
        }

        var timeout = CheckTimeout(caller.ProjectId);
        var outcomes = new List<GoalCheckOutcome>();
        var cancelled = false;

        foreach (var criterion in wanted)
        {
            if (cancelled || cancellationToken.IsCancellationRequested)
            {
                cancelled = true;
                outcomes.Add(new GoalCheckOutcome
                {
                    ShortId = criterion.ShortId,
                    Description = criterion.Description,
                    Command = criterion.CheckCommand!,
                    Result = GoalCheckResult.NotRun,
                    Explanation = "not run, because the checks were cancelled"
                });
                continue;
            }

            var outcome = await RunOneCheckAsync(caller, goalId, criterion.ShortId, runner, timeout, cancellationToken).ConfigureAwait(false);
            outcomes.Add(outcome);
            cancelled = outcome.Result == GoalCheckResult.Cancelled;
        }

        AutomatonDebug.Log($"GoalTrackingService.RunChecksAsync: Exit - goalId={goalId}, {string.Join(", ", outcomes.Select(outcome => $"{outcome.ShortId}={outcome.Result}"))}");
        return outcomes;
    }

    private async Task<GoalCheckOutcome> RunOneCheckAsync(GoalCaller caller, string goalId, string criterionRef, IGoalCheckRunner runner, TimeSpan timeout, CancellationToken cancellationToken)
    {
        var runId = Guid.NewGuid().ToString();
        var command = string.Empty;
        var folder = string.Empty;
        long workRevision = 0;
        var outcome = new GoalCheckOutcome();

        // 1. Record what is about to be checked, and against which version of the work.
        Mutate(caller, goalId, null, context =>
        {
            var criterion = ResolveCriterion(context.Goal, criterionRef);
            if (!criterion.HasCheckCommand)
            {
                throw new GoalRuleException($"{criterion.ShortId} no longer has a check command.");
            }

            command = criterion.CheckCommand!;
            folder = context.Goal.WorkingFolder;
            workRevision = context.Goal.WorkRevision;
            outcome.ShortId = criterion.ShortId;
            outcome.Description = criterion.Description;
            outcome.Command = command;

            context.Execute(@"
                INSERT INTO GoalCheckRuns (Id, GoalId, CriterionId, WorkRevision, Command, WorkingFolder, StartedAt, Result, StartedBy)
                VALUES (@Id, @GoalId, @CriterionId, @WorkRevision, @Command, @WorkingFolder, @StartedAt, 'Running', @StartedBy)",
                P("@Id", runId),
                P("@GoalId", context.Goal.Id),
                P("@CriterionId", criterion.Id),
                P("@WorkRevision", workRevision),
                P("@Command", command),
                P("@WorkingFolder", folder),
                P("@StartedAt", Iso(context.Now)),
                P("@StartedBy", context.Caller.ClaimantId ?? context.Caller.Author.ToString()));
        });

        AutomatonDebug.Log($"GoalTrackingService.RunOneCheckAsync: run {runId} started - {outcome.ShortId}, workRevision={workRevision}, command={command}");

        // 2. Run it. No database lock is held while it runs.
        GoalCheckExecution execution;
        try
        {
            execution = await runner.RunAsync(command, folder, timeout, cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            execution = new GoalCheckExecution { Cancelled = true };
        }
        catch (Exception ex)
        {
            execution = new GoalCheckExecution { StartError = ex.Message };
        }

        outcome.ExitCode = execution.ExitCode;
        outcome.Duration = execution.Duration;
        outcome.EvidenceTail = execution.OutputTail;

        // 3. Accept the result only for the work it actually saw.
        Mutate(caller, goalId, null, context =>
        {
            var criterion = context.Goal.Criteria.FirstOrDefault(candidate => string.Equals(candidate.ShortId, outcome.ShortId, StringComparison.OrdinalIgnoreCase));
            var stillOpen = context.Query("SELECT 1 FROM GoalCheckRuns WHERE Id = @Id AND Result = 'Running'", P("@Id", runId)).Count > 0;

            string? discardedBecause = null;
            if (criterion == null)
            {
                discardedBecause = "the criterion was removed while its check ran";
            }
            else if (!stillOpen)
            {
                discardedBecause = "the run was closed as abandoned while it was still going";
            }
            else if (context.Goal.WorkRevision != workRevision)
            {
                discardedBecause = "the plan changed while the check ran";
            }
            else if (!string.Equals(criterion.CheckCommand, command, StringComparison.Ordinal))
            {
                discardedBecause = "the check command was changed while it ran";
            }
            else if (!string.Equals(context.Goal.WorkingFolder, folder, StringComparison.Ordinal))
            {
                discardedBecause = "the working folder was changed while the check ran";
            }

            if (execution.Cancelled)
            {
                outcome.Result = GoalCheckResult.Cancelled;
                outcome.Explanation = "cancelled; the criterion is as it was";
            }
            else if (discardedBecause != null)
            {
                outcome.Result = GoalCheckResult.Discarded;
                outcome.Explanation = $"{discardedBecause}, so this result proves nothing; run it again";
            }
            else if (execution.TimedOut)
            {
                outcome.Result = GoalCheckResult.TimedOut;
                outcome.Explanation = $"did not finish within {timeout.TotalSeconds:0} seconds and was stopped";
            }
            else if (execution.StartError != null)
            {
                outcome.Result = GoalCheckResult.NotMet;
                outcome.Explanation = execution.StartError;
            }
            else
            {
                outcome.Result = execution.ExitCode == 0 ? GoalCheckResult.Met : GoalCheckResult.NotMet;
            }

            var evidence = BuildCheckEvidence(outcome, execution);
            context.Execute(@"
                UPDATE GoalCheckRuns SET FinishedAt = @FinishedAt, ExitCode = @ExitCode, Result = @Result, EvidenceTail = @Evidence
                WHERE Id = @Id",
                P("@FinishedAt", Iso(context.Now)),
                P("@ExitCode", execution.ExitCode),
                P("@Result", outcome.Result.ToString()),
                P("@Evidence", evidence),
                P("@Id", runId));

            context.Journal(GoalJournalKinds.Check, $"{outcome.ShortId} check {outcome.Result}{(outcome.ExitCode.HasValue ? $" (exit {outcome.ExitCode})" : string.Empty)} in {outcome.Duration.TotalSeconds:0.0}s{(outcome.Explanation == null ? string.Empty : $" - {outcome.Explanation}")}");

            // Cancelled and discarded runs leave the criterion exactly as it was.
            if (criterion == null || outcome.Result == GoalCheckResult.Cancelled || outcome.Result == GoalCheckResult.Discarded)
            {
                return;
            }

            var wasSatisfied = criterion.IsSatisfied;
            criterion.Status = outcome.Result == GoalCheckResult.Met ? GoalCriterionStatus.Met : GoalCriterionStatus.NotMet;
            criterion.MetBy = outcome.Result == GoalCheckResult.Met ? GoalProofSource.Check : null;
            criterion.Evidence = evidence;
            criterion.CheckedAt = context.Now;
            criterion.CheckedWorkRevision = workRevision;
            criterion.LastCheckRunId = runId;
            WriteCriterion(context, criterion);
            context.Execute("UPDATE GoalCriteria SET LastCheckRunId = @RunId WHERE Id = @Id", P("@RunId", runId), P("@Id", criterion.Id));

            if (wasSatisfied && !criterion.IsSatisfied)
            {
                ReopenIfClosed(context, $"the check for {criterion.ShortId} no longer passes");
            }
        });

        AutomatonDebug.Log($"GoalTrackingService.RunOneCheckAsync: run {runId} - {outcome.ShortId} {outcome.Result}, exit={outcome.ExitCode?.ToString() ?? "-"}, seconds={outcome.Duration.TotalSeconds:0.0}, evidenceChars={outcome.EvidenceTail.Length}{(outcome.Explanation == null ? string.Empty : $", {outcome.Explanation}")}");
        return outcome;
    }

    private static string BuildCheckEvidence(GoalCheckOutcome outcome, GoalCheckExecution execution)
    {
        var header = outcome.Result switch
        {
            GoalCheckResult.TimedOut => $"timed out: {outcome.Explanation}",
            GoalCheckResult.Cancelled => "cancelled",
            GoalCheckResult.Discarded => $"discarded: {outcome.Explanation}",
            _ when execution.StartError != null => $"could not run: {execution.StartError}",
            _ => $"exit code {execution.ExitCode}"
        };

        var evidence = $"{outcome.Command}\n{header}";
        if (!string.IsNullOrWhiteSpace(execution.OutputTail))
        {
            evidence += "\n" + execution.OutputTail;
        }

        return Cap(evidence, MaxEvidence) ?? header;
    }

    /// <summary>The recorded runs of a goal's checks, newest first, for the review sheet.</summary>
    public List<(string CriterionId, string Result, int? ExitCode, DateTime StartedAt, string Command)> ListCheckRuns(GoalCaller caller, string goalId, int max = 20)
    {
        var goal = GetGoal(caller, goalId);
        return _db.Query("SELECT * FROM GoalCheckRuns WHERE GoalId = @GoalId ORDER BY StartedAt DESC LIMIT @Max", P("@GoalId", goal.Id), P("@Max", Math.Clamp(max, 1, 200)))
            .Select(row => (
                row["CriterionId"] as string ?? string.Empty,
                row["Result"] as string ?? string.Empty,
                row["ExitCode"] == null ? (int?)null : (int)ToLong(row["ExitCode"]),
                ParseTimestamp(row["StartedAt"]) ?? DateTime.UtcNow,
                row["Command"] as string ?? string.Empty))
            .ToList();
    }

    /// <summary>
    /// A run still marked Running long after it could have finished belongs to a process that
    /// died. Closed as Cancelled, once per process. Only runs older than the timeout plus a
    /// margin are touched, because another process may be running a check right now; and a run
    /// closed here that does finish after all is discarded, not believed (step 3 above).
    /// </summary>
    private void ReconcileAbandonedCheckRuns()
    {
        lock (CheckRunsReconciled)
        {
            if (!CheckRunsReconciled.Add(_db.DatabasePath))
            {
                return;
            }
        }

        CloseAbandonedCheckRuns();
    }

    /// <summary>Closes runs left Running past any possible finish. Returns how many.</summary>
    public int CloseAbandonedCheckRuns()
    {
        var cutoff = _clock() - CheckTimeout(null) - TimeSpan.FromMinutes(5);
        var closed = _db.Execute(
            "UPDATE GoalCheckRuns SET Result = 'Cancelled', FinishedAt = @Now, EvidenceTail = 'abandoned: the process running it ended' WHERE Result = 'Running' AND StartedAt < @Cutoff",
            P("@Now", Iso(_clock())), P("@Cutoff", Iso(cutoff)));

        if (closed > 0)
        {
            AutomatonDebug.LogWarning($"GoalTrackingService.CloseAbandonedCheckRuns: closed {closed} check run(s) left Running by a process that ended");
        }

        return closed;
    }

    private TimeSpan CheckTimeout(string? projectId)
    {
        var value = Setting(CheckTimeoutSetting, projectId);
        if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var seconds) && seconds > 0)
        {
            return TimeSpan.FromSeconds(seconds);
        }

        return TimeSpan.FromMinutes(10);
    }
}
