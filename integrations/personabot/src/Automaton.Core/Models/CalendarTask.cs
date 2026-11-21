using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

/// <summary>
/// Status of a calendar task or occurrence.
/// </summary>
public enum CalendarTaskStatus
{
    Pending,
    Completed,
    Cancelled,
    Paused
}

/// <summary>
/// Task type for calendar entries.
/// </summary>
public enum CalendarTaskType
{
    OneOff,
    Recurring
}

/// <summary>
/// Recurrence unit for recurring calendar tasks. The actual period is this unit multiplied
/// by <see cref="CalendarTask.RecurrenceInterval"/> (e.g. unit=Minutely, interval=10 => every 10 minutes).
/// Order matches the editor's recurrence dropdown so the selected index maps directly to the enum value.
/// </summary>
public enum RecurrenceType
{
    Minutely,
    Hourly,
    Daily,
    Weekly,
    Monthly
}

/// <summary>
/// Represents a calendar task stored in config.db.
/// </summary>
public class CalendarTask
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = Guid.NewGuid().ToString();

    [JsonPropertyName("taskType")]
    public CalendarTaskType TaskType { get; set; } = CalendarTaskType.OneOff;

    [JsonPropertyName("title")]
    public string Title { get; set; } = string.Empty;

    [JsonPropertyName("description")]
    public string? Description { get; set; }

    [JsonPropertyName("dueDate")]
    public DateTime DueDate { get; set; } = DateTime.Today;

    [JsonPropertyName("dueTime")]
    public string? DueTime { get; set; }

    [JsonPropertyName("recurrence")]
    public RecurrenceType? Recurrence { get; set; }

    /// <summary>
    /// Multiplier applied to the <see cref="Recurrence"/> unit (e.g. unit=Minutely, interval=10 => every
    /// 10 minutes; unit=Daily, interval=2 => every other day). Defaults to 1. Ignored for non-recurring tasks.
    /// </summary>
    [JsonPropertyName("recurrenceInterval")]
    public int RecurrenceInterval { get; set; } = 1;

    [JsonPropertyName("endAfter")]
    public int? EndAfter { get; set; }

    [JsonPropertyName("endDate")]
    public DateTime? EndDate { get; set; }

    [JsonPropertyName("tags")]
    public List<string> Tags { get; set; } = new();

    [JsonPropertyName("status")]
    public CalendarTaskStatus Status { get; set; } = CalendarTaskStatus.Pending;

    [JsonPropertyName("completedAt")]
    public DateTime? CompletedAt { get; set; }

    [JsonPropertyName("createdAt")]
    public DateTime CreatedAt { get; set; } = DateTime.UtcNow;

    [JsonPropertyName("updatedAt")]
    public DateTime UpdatedAt { get; set; } = DateTime.UtcNow;

    // ── Watch fields ───────────────────────────────────────────────────
    // A "watch" is an AI-managed entry that the scheduler fires on an interval,
    // running a headless check in its own dedicated child conversation and only
    // alerting the human (via the Inbox) when its notify criteria is met.
    // These are all null/false/zero for ordinary human calendar tasks.

    /// <summary>
    /// Marks this entry as an AI-managed watch. The scheduler only fires entries
    /// where this is true; human tasks are ignored by the scheduler.
    /// </summary>
    [JsonPropertyName("isWatch")]
    public bool IsWatch { get; set; }

    /// <summary>
    /// Scheduled runs of this task may use tools without asking, as a chat in FULL permission
    /// mode does. A run is unattended, so in an ASK-mode chat any tool that needs permission
    /// stops it as Needs input. Only a person sets this, from the task editor: the
    /// calendar_control tool has no parameter for it, so a model cannot grant itself tools.
    /// </summary>
    [JsonPropertyName("runWithoutAsking")]
    public bool RunWithoutAsking { get; set; }

    /// <summary>
    /// The originating chat conversation (stateId) this watch was created from.
    /// Used by the executing AI to pull deeper context via get_conversation/remember.
    /// </summary>
    [JsonPropertyName("sourceSessionId")]
    public string? SourceSessionId { get; set; }

    /// <summary>
    /// The dedicated child conversation (stateId, by convention "task/{taskId}")
    /// created on first run that this task executes against. Null until
    /// the first fire creates it. Never registered in the project thread tree.
    /// </summary>
    [JsonPropertyName("taskSessionId")]
    public string? TaskSessionId { get; set; }

    /// <summary>
    /// The dedicated child conversation (stateId, by convention "watch/{taskId}")
    /// created on first run that this watch's checks execute against. Null until
    /// the first fire creates it. Never registered in the project thread tree.
    /// </summary>
    [JsonPropertyName("watchSessionId")]
    public string? WatchSessionId { get; set; }

    /// <summary>
    /// What this watch is trying to accomplish — the instruction the executing AI follows.
    /// </summary>
    [JsonPropertyName("objective")]
    public string? Objective { get; set; }

    /// <summary>
    /// Fire interval in minutes (e.g. 240 = every 4 hours). Drives NextRunAt for watches,
    /// superseding the daily/weekly/monthly Recurrence used by human tasks.
    /// </summary>
    [JsonPropertyName("intervalMinutes")]
    public int? IntervalMinutes { get; set; }

    /// <summary>
    /// Natural-language condition that, when met, should trigger an inbox alert.
    /// </summary>
    [JsonPropertyName("notifyCriteria")]
    public string? NotifyCriteria { get; set; }

    /// <summary>
    /// Natural-language condition that, when met, should stop the watch (mark complete).
    /// Set by the AI per-watch from the user's phrasing. EndAfter/EndDate act as optional
    /// hard guards alongside this.
    /// </summary>
    [JsonPropertyName("stopCondition")]
    public string? StopCondition { get; set; }

    /// <summary>
    /// AI-maintained snapshot of the last observed state. The basis for change detection:
    /// each fire compares the current observation against this and updates it.
    /// </summary>
    [JsonPropertyName("lastObservation")]
    public string? LastObservation { get; set; }

    /// <summary>
    /// When the scheduler should next fire this watch. Null for non-watch tasks.
    /// </summary>
    [JsonPropertyName("nextRunAt")]
    public DateTime? NextRunAt { get; set; }

    /// <summary>
    /// When this watch last fired.
    /// </summary>
    [JsonPropertyName("lastRunAt")]
    public DateTime? LastRunAt { get; set; }

    /// <summary>
    /// Number of times this watch has fired.
    /// </summary>
    [JsonPropertyName("runCount")]
    public int RunCount { get; set; }

    /// <summary>
    /// Error from the most recent fire, if it failed; null on success.
    /// </summary>
    [JsonPropertyName("lastRunError")]
    public string? LastRunError { get; set; }

    [JsonPropertyName("schedule")]
    public CalendarScheduleOptions Schedule { get; set; } = new();

    /// <summary>
    /// Id of the <see cref="CalendarRun"/> currently executing this task, or null when idle.
    /// The claim is taken atomically in the database so that two scheduler processes sharing
    /// config.db (the desktop and its headless child, or the CLI) never run the same task twice.
    /// </summary>
    [JsonPropertyName("claimedRunId")]
    public string? ClaimedRunId { get; set; }

    /// <summary>"machine:pid" of the process holding <see cref="ClaimedRunId"/>.</summary>
    [JsonPropertyName("claimedBy")]
    public string? ClaimedBy { get; set; }

    [JsonPropertyName("claimedAt")]
    public DateTime? ClaimedAt { get; set; }

    /// <summary>True while some scheduler process holds a run claim on this task.</summary>
    [JsonIgnore]
    public bool IsClaimed => !string.IsNullOrWhiteSpace(ClaimedRunId);
}

public class CalendarScheduleOptions
{
    public string? TargetSessionId { get; set; }
    public string TimeZoneId { get; set; } = TimeZoneInfo.Local.Id;
    public bool WeekdaysOnly { get; set; }
    public string MissedRunPolicy { get; set; } = "run_once";
    public string NotificationPolicy { get; set; } = "changes";
    public DateTime? NextOverrideUtc { get; set; }
}

public class CalendarRun
{
    public string Id { get; set; } = Guid.NewGuid().ToString();
    public string TaskId { get; set; } = "";
    public string? SessionId { get; set; }
    public DateTime ScheduledAt { get; set; }
    public DateTime StartedAt { get; set; } = DateTime.UtcNow;
    public DateTime? FinishedAt { get; set; }
    public string Outcome { get; set; } = "Running";
    public string Summary { get; set; } = "";
    public string? MessageId { get; set; }
    public string DurationText => FinishedAt.HasValue ? $"{(FinishedAt.Value - StartedAt).TotalSeconds:0}s" : "In progress";
    public string StartedText => StartedAt.ToLocalTime().ToString("dd MMM HH:mm");
}

public record CalendarThreadChoice(string StateId, string Name, string ProjectName)
{
    public string Label => string.IsNullOrWhiteSpace(ProjectName) ? Name : $"{ProjectName} / {Name}";
    public override string ToString() => Label;
}

/// <summary>
/// Tracks completion/cancellation of individual recurring task occurrences.
/// </summary>
public class CalendarTaskOccurrence
{
    [JsonPropertyName("taskId")]
    public string TaskId { get; set; } = string.Empty;

    [JsonPropertyName("occurrenceDate")]
    public DateTime OccurrenceDate { get; set; }

    [JsonPropertyName("status")]
    public CalendarTaskStatus Status { get; set; } = CalendarTaskStatus.Completed;

    [JsonPropertyName("completedAt")]
    public DateTime? CompletedAt { get; set; }
}
