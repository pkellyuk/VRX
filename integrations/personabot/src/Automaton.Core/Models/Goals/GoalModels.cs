using System;
using System.Collections.Generic;
using System.Linq;

namespace Automaton.Core.Models.Goals;

/// <summary>Where one task of a goal is. See GOALSUPDATE.md §4.2.</summary>
public enum GoalTaskStatus
{
    Todo,
    Doing,
    Done,
    Blocked,
    Skipped
}

/// <summary>Whether a success criterion has been shown to hold.</summary>
public enum GoalCriterionStatus
{
    Unverified,
    Met,
    NotMet
}

/// <summary>
/// What kind of proof a criterion will accept. Ordered weakest to strongest, so
/// "lowering" a requirement is a numeric comparison.
/// </summary>
public enum GoalProofKind
{
    Any = 0,
    Check = 1,
    Human = 2
}

/// <summary>
/// How a criterion actually became Met. Assigned by the path the write came through,
/// never supplied by the model.
/// </summary>
public enum GoalProofSource
{
    Model,
    Check,
    Human
}

/// <summary>Who made a change. Comes from <c>GoalCaller</c>, never from a tool argument.</summary>
public enum GoalAuthor
{
    Model,
    Human,
    System
}

public enum GoalClaimKind
{
    Chat,
    Worker
}

/// <summary>Kinds of journal row. System kinds are never counted as progress.</summary>
public static class GoalJournalKinds
{
    public const string Note = "Note";
    public const string Decision = "Decision";
    public const string Attempt = "Attempt";
    public const string StatusChange = "StatusChange";
    public const string Scope = "Scope";
    public const string Check = "Check";
    public const string Gate = "Gate";
    public const string Claim = "Claim";
    public const string Worker = "Worker";
    public const string Loop = "Loop";
    public const string Migration = "Migration";
    public const string Review = "Review";

    /// <summary>The kinds a model may write through a tool.</summary>
    public static readonly IReadOnlyList<string> ModelWritable = new[] { Note, Decision, Attempt };
}

/// <summary>
/// A project goal with its tasks and criteria: the aggregate the goal tracking service
/// reads and writes. The legacy <c>ProjectGoal</c> in Automaton.Tools is a flattened view
/// of this.
/// </summary>
public sealed class TrackedGoal
{
    public string Id { get; set; } = string.Empty;
    public string ProjectId { get; set; } = string.Empty;
    public string Title { get; set; } = string.Empty;
    public string Description { get; set; } = string.Empty;
    public string WorkingFolder { get; set; } = string.Empty;

    /// <summary>The DownloadedDocs column, passed through untouched as JSON.</summary>
    public string DownloadedDocsJson { get; set; } = "[]";

    public GoalStatus Status { get; set; }
    public int Priority { get; set; }

    /// <summary>Concurrency token for the whole aggregate.</summary>
    public long Revision { get; set; }

    /// <summary>Version of what the work is and how far it has got.</summary>
    public long WorkRevision { get; set; }

    public int MigrationVersion { get; set; }
    public DateTime CreatedAt { get; set; }
    public DateTime UpdatedAt { get; set; }
    public DateTime? CompletedAt { get; set; }
    public string? ReviewSummary { get; set; }
    public DateTime? ReviewRequestedAt { get; set; }

    public List<GoalTask> Tasks { get; set; } = new();
    public List<GoalCriterion> Criteria { get; set; } = new();
    public List<PendingScopeChange> PendingScopeChanges { get; set; } = new();

    /// <summary>Active and Blocked goals are the ones a model can work on.</summary>
    public bool IsActive => Status == GoalStatus.Active || Status == GoalStatus.Blocked;
}

public sealed class GoalTask
{
    public string Id { get; set; } = string.Empty;
    public string GoalId { get; set; } = string.Empty;
    public int Ordinal { get; set; }
    public string ShortId { get; set; } = string.Empty;
    public string Description { get; set; } = string.Empty;
    public GoalTaskStatus Status { get; set; }
    public string? Note { get; set; }
    public string? BlockedReason { get; set; }
    public List<string> DependsOn { get; set; } = new();
    public string? Evidence { get; set; }
    public string? EvidenceBy { get; set; }
    public bool NeedsReview { get; set; }
    public string? ClaimedBy { get; set; }
    public GoalClaimKind? ClaimKind { get; set; }
    public DateTime? ClaimedAt { get; set; }
    public DateTime? ClaimTouchedAt { get; set; }
    public DateTime CreatedAt { get; set; }
    public DateTime UpdatedAt { get; set; }
    public DateTime? CompletedAt { get; set; }

    public bool IsFinished => Status == GoalTaskStatus.Done || Status == GoalTaskStatus.Skipped;
}

public sealed class GoalCriterion
{
    public string Id { get; set; } = string.Empty;
    public string GoalId { get; set; } = string.Empty;
    public int Ordinal { get; set; }
    public string ShortId { get; set; } = string.Empty;
    public string Description { get; set; } = string.Empty;
    public string? CheckCommand { get; set; }
    public GoalCriterionStatus Status { get; set; }
    public GoalProofKind RequiredProof { get; set; }
    public GoalProofSource? MetBy { get; set; }
    public string? Evidence { get; set; }
    public DateTime? CheckedAt { get; set; }
    public long? CheckedWorkRevision { get; set; }
    public string? LastCheckRunId { get; set; }
    public GoalAuthor Author { get; set; }

    public bool HasCheckCommand => !string.IsNullOrWhiteSpace(CheckCommand);

    /// <summary>True when the criterion is Met by a kind of proof it accepts.</summary>
    public bool IsSatisfied
    {
        get
        {
            if (Status != GoalCriterionStatus.Met || MetBy == null)
            {
                return false;
            }

            return GoalProof.Satisfies(MetBy.Value, RequiredProof);
        }
    }
}

public static class GoalProof
{
    /// <summary>
    /// Human proof satisfies anything, a check satisfies Check and Any, and a model's say-so
    /// satisfies only Any.
    /// </summary>
    public static bool Satisfies(GoalProofSource source, GoalProofKind required)
    {
        if (source == GoalProofSource.Human)
        {
            return true;
        }

        if (source == GoalProofSource.Check)
        {
            return required != GoalProofKind.Human;
        }

        return required == GoalProofKind.Any;
    }
}

/// <summary>
/// A change a model proposed to a human-authored criterion. Parked until a human approves
/// or rejects it. See GOALSUPDATE.md §5.4.
/// </summary>
public sealed class PendingScopeChange
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");

    /// <summary>update | remove</summary>
    public string Kind { get; set; } = "update";

    public string CriterionId { get; set; } = string.Empty;
    public string CriterionShortId { get; set; } = string.Empty;
    public string? OldDescription { get; set; }
    public string? NewDescription { get; set; }
    public string? OldCheckCommand { get; set; }
    public string? NewCheckCommand { get; set; }
    public bool ChangesCheckCommand { get; set; }
    public GoalProofKind? OldRequiredProof { get; set; }
    public GoalProofKind? NewRequiredProof { get; set; }
    public string? StateId { get; set; }
    public DateTime RequestedAt { get; set; } = DateTime.UtcNow;

    public string Describe()
    {
        if (string.Equals(Kind, "remove", StringComparison.OrdinalIgnoreCase))
        {
            return $"remove {CriterionShortId} \"{OldDescription}\"";
        }

        var parts = new List<string>();
        if (NewDescription != null)
        {
            parts.Add($"reword to \"{NewDescription}\"");
        }

        if (ChangesCheckCommand)
        {
            parts.Add(string.IsNullOrWhiteSpace(NewCheckCommand) ? "remove its check command" : $"check command \"{NewCheckCommand}\"");
        }

        if (NewRequiredProof != null)
        {
            parts.Add($"proof {OldRequiredProof} -> {NewRequiredProof}");
        }

        return $"change {CriterionShortId}: {string.Join(", ", parts)}";
    }
}

public sealed class GoalJournalEntry
{
    public long Id { get; set; }
    public string GoalId { get; set; } = string.Empty;
    public string? TaskId { get; set; }
    public string Kind { get; set; } = GoalJournalKinds.Note;
    public string Content { get; set; } = string.Empty;
    public string? StateId { get; set; }
    public GoalAuthor Author { get; set; }
    public DateTime CreatedAt { get; set; }
}

/// <summary>Computed, never stored.</summary>
public sealed class GoalProgress
{
    public int Done { get; set; }
    public int Total { get; set; }
    public int CriteriaMet { get; set; }
    public int CriteriaTotal { get; set; }

    /// <summary>Null when the goal has no tasks: "no plan yet", never 0% or 100%.</summary>
    public int? Percent { get; set; }

    public GoalTask? NextRunnable { get; set; }

    /// <summary>Set when tasks remain but none can be started, saying what they wait on.</summary>
    public string? WhyNothingRunnable { get; set; }

    public string Describe()
    {
        var tasks = Total == 0 ? "no plan yet" : $"{Done}/{Total} tasks";
        return $"{tasks}, {CriteriaMet}/{CriteriaTotal} criteria met";
    }

    public static GoalProgress For(TrackedGoal goal)
    {
        if (goal == null)
        {
            throw new ArgumentNullException(nameof(goal));
        }

        var progress = new GoalProgress
        {
            Total = goal.Tasks.Count,
            Done = goal.Tasks.Count(task => task.IsFinished),
            CriteriaTotal = goal.Criteria.Count,
            CriteriaMet = goal.Criteria.Count(criterion => criterion.IsSatisfied)
        };

        if (progress.Total > 0)
        {
            progress.Percent = (int)Math.Floor(progress.Done * 100.0 / progress.Total);
        }

        var finished = goal.Tasks
            .Where(task => task.IsFinished)
            .Select(task => task.ShortId)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);

        progress.NextRunnable = goal.Tasks
            .OrderBy(task => task.Ordinal)
            .FirstOrDefault(task =>
                task.Status == GoalTaskStatus.Todo &&
                string.IsNullOrEmpty(task.ClaimedBy) &&
                task.DependsOn.All(finished.Contains));

        if (progress.NextRunnable != null || progress.Done == progress.Total)
        {
            return progress;
        }

        // Tasks remain and none can start: say what the first waiting one waits on.
        var waiting = goal.Tasks
            .OrderBy(task => task.Ordinal)
            .FirstOrDefault(task => task.Status == GoalTaskStatus.Todo && task.DependsOn.Any(id => !finished.Contains(id)));

        if (waiting != null)
        {
            var blockers = waiting.DependsOn
                .Where(id => !finished.Contains(id))
                .Select(id =>
                {
                    var blocker = goal.Tasks.FirstOrDefault(task => string.Equals(task.ShortId, id, StringComparison.OrdinalIgnoreCase));
                    return blocker == null ? id : $"{id}, which is {blocker.Status}";
                });
            progress.WhyNothingRunnable = $"{waiting.ShortId} waits on {string.Join(" and ", blockers)}";
            return progress;
        }

        var open = goal.Tasks.Where(task => !task.IsFinished).OrderBy(task => task.Ordinal).ToList();
        progress.WhyNothingRunnable = string.Join(", ", open.Select(task =>
            string.IsNullOrEmpty(task.ClaimedBy) ? $"{task.ShortId} is {task.Status}" : $"{task.ShortId} is {task.Status} (held by {task.ClaimedBy})"));
        return progress;
    }
}

/// <summary>A goal without its children, for listings.</summary>
public sealed class GoalHeader
{
    public TrackedGoal Goal { get; set; } = new();
    public GoalProgress Progress { get; set; } = new();
}

public sealed class GoalTaskDraft
{
    public string Description { get; set; } = string.Empty;

    /// <summary>ShortIds of tasks in the same goal, e.g. "T1".</summary>
    public List<string> DependsOn { get; set; } = new();

    /// <summary>
    /// Zero-based positions of other drafts in the same list, resolved to task ids once they
    /// exist. Task numbers are never reused, so a list that replaces another cannot know its
    /// ids in advance.
    /// </summary>
    public List<int> DependsOnDrafts { get; set; } = new();
}

public sealed class GoalCriterionDraft
{
    public string Description { get; set; } = string.Empty;
    public string? CheckCommand { get; set; }

    /// <summary>Null means "use the default for this project".</summary>
    public GoalProofKind? RequiredProof { get; set; }
}

public sealed class GoalDraft
{
    /// <summary>Optional. A caller that already minted an id (the legacy store, the remote protocol).</summary>
    public string? Id { get; set; }

    public string? Title { get; set; }
    public string Description { get; set; } = string.Empty;
    public string WorkingFolder { get; set; } = string.Empty;
    public string? DownloadedDocsJson { get; set; }
    public GoalStatus Status { get; set; } = GoalStatus.Active;
    public DateTime? CreatedAt { get; set; }
    public List<GoalTaskDraft> Tasks { get; set; } = new();
    public List<GoalCriterionDraft> Criteria { get; set; } = new();
}

/// <summary>Only the non-null members are written.</summary>
public sealed class GoalFieldChanges
{
    public string? Title { get; set; }
    public string? Description { get; set; }
    public string? WorkingFolder { get; set; }
    public string? DownloadedDocsJson { get; set; }
    public int? Priority { get; set; }

    public bool IsEmpty =>
        Title == null && Description == null && WorkingFolder == null && DownloadedDocsJson == null && Priority == null;
}

/// <summary>What a task status change did, including the things it did on the side.</summary>
public sealed class GoalTaskChangeResult
{
    public GoalTask Task { get; set; } = new();
    public List<string> SideEffects { get; set; } = new();
    public GoalProgress Progress { get; set; } = new();
    public bool GoalReopened { get; set; }
}

public enum GoalCompletionOutcome
{
    Completed,
    AwaitingReview
}

public sealed class GoalCompletionResult
{
    public GoalCompletionOutcome Outcome { get; set; }
    public TrackedGoal Goal { get; set; } = new();

    /// <summary>The criteria handed to the human, when the outcome is AwaitingReview.</summary>
    public List<string> ForReview { get; set; } = new();
}

/// <summary>What happened to a criterion edit: applied now, or parked for a human.</summary>
public sealed class GoalCriterionEditResult
{
    public GoalCriterion? Criterion { get; set; }
    public bool Parked { get; set; }
    public string Message { get; set; } = string.Empty;
}
