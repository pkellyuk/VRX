using System;
using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

/// <summary>
/// Where a goal is in its life. Only <see cref="Queued"/> goals are candidates for
/// dispatch, and only <see cref="Running"/> ones have a live worker.
/// </summary>
public enum DirectorGoalStatus
{
    /// <summary>Waiting for a free worker slot.</summary>
    Queued,

    /// <summary>A worker is executing it now.</summary>
    Running,

    /// <summary>The worker needs an answer from the human before it can continue.</summary>
    Blocked,

    /// <summary>The worker finished and reported an outcome.</summary>
    Done,

    /// <summary>The worker stopped on an error.</summary>
    Failed,

    /// <summary>Stopped by the human before it finished, or dropped from the queue.</summary>
    Cancelled
}

/// <summary>
/// One unit of work the Voice Director manages: a goal the human asked for, plus the
/// dispatch record of the worker executing it.
///
/// Deliberately not the same thing as <c>ProjectGoal</c> (the per-project backlog
/// edited by hand and by <c>goal_tool</c>). A ProjectGoal is planning state with no
/// notion of execution; a DirectorGoal has a worker session and a lifecycle. Merging
/// them would make every hand-written backlog item look like something the Director
/// ought to be running.
///
/// Persisted in the DirectorGoals table in config.db so the queue survives a restart.
/// </summary>
public class DirectorGoal
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = Guid.NewGuid().ToString();

    /// <summary>Short name, the way the human said it. Spoken back to them verbatim.</summary>
    [JsonPropertyName("title")]
    public string Title { get; set; } = string.Empty;

    /// <summary>
    /// The fuller brief handed to the worker. May be empty when the title says it all.
    /// </summary>
    [JsonPropertyName("detail")]
    public string? Detail { get; set; }

    /// <summary>
    /// Where the worker runs. Passed as the conversation's fallback working directory.
    /// </summary>
    [JsonPropertyName("workingDirectory")]
    public string WorkingDirectory { get; set; } = string.Empty;

    /// <summary>Optional project association, for grouping in the UI.</summary>
    [JsonPropertyName("projectId")]
    public string? ProjectId { get; set; }

    /// <summary>The chat thread that explicitly queued this goal, when known.</summary>
    [JsonPropertyName("originatingThreadId")]
    public string? OriginatingThreadId { get; set; }

    /// <summary>
    /// Stable local session key used to route visible lifecycle records back to the
    /// exact chat that queued the work. Null for global or legacy Director goals.
    /// </summary>
    [JsonPropertyName("originatingSessionKey")]
    public string? OriginatingSessionKey { get; set; }

    /// <summary>
    /// Queue order. Lower runs first. Contiguous within the queued set, renumbered on
    /// reorder - see <c>DirectorGoalService.ReprioritiseAsync</c>.
    /// </summary>
    [JsonPropertyName("position")]
    public int Position { get; set; }

    [JsonPropertyName("status")]
    public DirectorGoalStatus Status { get; set; } = DirectorGoalStatus.Queued;

    /// <summary>
    /// The conversation this goal's worker runs in - a stateId, so the session is an
    /// ordinary chat session that can be opened and read like any other. Assigned when
    /// the goal is first dispatched and kept afterwards, so a finished goal can still
    /// be opened to see what the worker actually did.
    /// </summary>
    [JsonPropertyName("workerStateId")]
    public string? WorkerStateId { get; set; }

    /// <summary>
    /// The worker's most recent one-line status ("Running read…", "Finished bash").
    /// Overwritten constantly while running; it is a progress indicator, not a log.
    /// </summary>
    [JsonPropertyName("lastStatus")]
    public string? LastStatus { get; set; }

    /// <summary>The worker's closing summary, for "what did it do?".</summary>
    [JsonPropertyName("outcome")]
    public string? Outcome { get; set; }

    /// <summary>
    /// What the worker needs answered while <see cref="DirectorGoalStatus.Blocked"/>.
    /// The Director asks this in its own words rather than reading it aloud.
    /// </summary>
    [JsonPropertyName("blockedQuestion")]
    public string? BlockedQuestion { get; set; }

    /// <summary>
    /// The human's answer to <see cref="BlockedQuestion"/>, waiting to be handed to the
    /// worker. Included in the brief on the next dispatch and cleared once delivered -
    /// otherwise the worker would be told the answer again every time it restarted.
    /// </summary>
    [JsonPropertyName("pendingAnswer")]
    public string? PendingAnswer { get; set; }

    /// <summary>
    /// The project goal this work belongs to, when it is one task of a tracked goal. The
    /// Director's queue and the project's goals stay separate things; this is the link.
    /// </summary>
    [JsonPropertyName("projectGoalId")]
    public string? ProjectGoalId { get; set; }

    /// <summary>The task of <see cref="ProjectGoalId"/> the worker is executing (its GUID).</summary>
    [JsonPropertyName("goalTaskId")]
    public string? GoalTaskId { get; set; }

    /// <summary>
    /// When true, finishing this task queues the project goal's next runnable task, and so
    /// on until none is left or one blocks.
    /// </summary>
    [JsonPropertyName("chainProjectGoal")]
    public bool ChainProjectGoal { get; set; }

    /// <summary>Identifies one attempt. A new one is minted on every dispatch.</summary>
    [JsonPropertyName("runId")]
    public string? RunId { get; set; }

    /// <summary>The session id of the worker's conversation, which is how a tool call finds its goal.</summary>
    [JsonPropertyName("workerSessionId")]
    public string? WorkerSessionId { get; set; }

    /// <summary>Done, Failed, Blocked or Unreported: what the worker itself said, through report_outcome.</summary>
    [JsonPropertyName("reportedOutcome")]
    public string? ReportedOutcome { get; set; }

    [JsonPropertyName("reportedSummary")]
    public string? ReportedSummary { get; set; }

    /// <summary>The run that reported. A report only counts for the run that is current.</summary>
    [JsonPropertyName("reportedRunId")]
    public string? ReportedRunId { get; set; }

    [JsonPropertyName("reportedAt")]
    public DateTime? ReportedAt { get; set; }

    /// <summary>True when the current run has said how it ended.</summary>
    [JsonIgnore]
    public bool HasReportForCurrentRun =>
        !string.IsNullOrWhiteSpace(RunId) &&
        !string.IsNullOrWhiteSpace(ReportedOutcome) &&
        string.Equals(RunId, ReportedRunId, StringComparison.Ordinal);

    [JsonPropertyName("createdAt")]
    public DateTime CreatedAt { get; set; } = DateTime.UtcNow;

    [JsonPropertyName("startedAt")]
    public DateTime? StartedAt { get; set; }

    [JsonPropertyName("finishedAt")]
    public DateTime? FinishedAt { get; set; }

    /// <summary>True for the statuses that mean the goal will not run again.</summary>
    [JsonIgnore]
    public bool IsTerminal =>
        Status == DirectorGoalStatus.Done ||
        Status == DirectorGoalStatus.Failed ||
        Status == DirectorGoalStatus.Cancelled;

    /// <summary>
    /// How long the worker ran, or has been running. Null before dispatch. Spoken as
    /// "took about four minutes", so callers should round it.
    /// </summary>
    [JsonIgnore]
    public TimeSpan? Elapsed
    {
        get
        {
            if (StartedAt == null)
            {
                return null;
            }

            return (FinishedAt ?? DateTime.UtcNow) - StartedAt.Value;
        }
    }
}
