using System.Text.Json;
using Automaton.Core.Models;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services;

/// <summary>
/// Schedule management: whole-definition saves, status changes, run history, run claims and
/// thread lookup. Every write raises <see cref="Changed"/> so open UIs refresh without polling.
/// </summary>
public partial class CalendarService
{
    /// <summary>Raised after any calendar write in this process.</summary>
    public static event Action? Changed;

    public static void RaiseChanged()
    {
        Changed?.Invoke();
    }

    public Task<List<CalendarTask>> ListAllAsync()
    {
        return Task.Run(() =>
        {
            AutomatonDebug.Log("CalendarService.ListAllAsync: Entry");
            var tasks = Database.Query("SELECT * FROM CalendarTasks ORDER BY DueDate, DueTime").Select(RowToTask).ToList();
            AutomatonDebug.Log($"CalendarService.ListAllAsync: Exit - {tasks.Count} tasks");
            return tasks;
        });
    }

    /// <summary>
    /// Writes the complete definition of an existing task. The caller loads the current row and
    /// edits it first, so there are no defaults masquerading as patches (the bug the old
    /// null-means-skip update had). With <paramref name="reschedule"/> the next run is recomputed
    /// from the new schedule; without it a cosmetic edit keeps the pending run where it was.
    /// </summary>
    public Task SaveDefinitionAsync(CalendarTask task, bool reschedule = true)
        => SaveDefinitionAsync(task, reschedule, expectedUpdatedAt: null);

    /// <summary>Opt-in compare-and-save for editors. A timestamp mismatch or active run refuses the write.</summary>
    public Task SaveDefinitionAsync(CalendarTask task, bool reschedule, string? expectedUpdatedAt)
    {
        if (task == null)
        {
            throw new ArgumentNullException(nameof(task));
        }

        if (string.IsNullOrWhiteSpace(task.Id))
        {
            throw new ArgumentException("task.Id is required", nameof(task));
        }

        AutomatonDebug.Log($"CalendarService.SaveDefinitionAsync: Entry - id={task.Id}, title={task.Title}, reschedule={reschedule}");
        CalendarSchedule.Validate(task);

        if (reschedule)
        {
            task.Schedule.NextOverrideUtc = null;
            task.NextRunAt = ExcludeCompletedOccurrences(task, CalendarSchedule.InitialNextRunUtc(task, DateTime.UtcNow));

            AutomatonDebug.Log($"CalendarService.SaveDefinitionAsync: id={task.Id} rescheduled nextRunAt={task.NextRunAt:O}");
        }

        return Task.Run(() =>
        {
            var changed = Database.Execute(@"UPDATE CalendarTasks SET Title=@Title, Description=@Description,
                TaskType=@TaskType, DueDate=@DueDate, DueTime=@DueTime, Recurrence=@Recurrence,
                RecurrenceInterval=@RecurrenceInterval, EndAfter=@EndAfter, EndDate=@EndDate,
                Tags=@Tags, SourceSessionId=@SourceSessionId, IsWatch=@IsWatch, Objective=@Objective,
                IntervalMinutes=@IntervalMinutes, NotifyCriteria=@NotifyCriteria, StopCondition=@StopCondition,
                LastObservation=@LastObservation, ScheduleJson=@ScheduleJson, NextRunAt=@NextRunAt,
                RunWithoutAsking=@RunWithoutAsking, UpdatedAt=@UpdatedAt WHERE Id=@Id
                AND (@ExpectedUpdatedAt IS NULL OR (UpdatedAt=@ExpectedUpdatedAt AND ClaimedRunId IS NULL))",
                P("ExpectedUpdatedAt", expectedUpdatedAt),
                P("Id", task.Id),
                P("Title", task.Title),
                P("Description", task.Description),
                P("TaskType", task.TaskType.ToString()),
                P("DueDate", task.DueDate.ToString("yyyy-MM-dd")),
                P("DueTime", task.DueTime),
                P("Recurrence", task.Recurrence?.ToString()),
                P("RecurrenceInterval", task.RecurrenceInterval),
                P("EndAfter", task.EndAfter),
                P("EndDate", task.EndDate?.ToString("yyyy-MM-dd")),
                P("Tags", JsonSerializer.Serialize(task.Tags)),
                P("SourceSessionId", task.SourceSessionId),
                P("IsWatch", task.IsWatch ? 1 : 0),
                P("Objective", task.Objective),
                P("IntervalMinutes", task.IntervalMinutes),
                P("NotifyCriteria", task.NotifyCriteria),
                P("StopCondition", task.StopCondition),
                P("LastObservation", task.LastObservation),
                P("ScheduleJson", JsonSerializer.Serialize(task.Schedule)),
                P("NextRunAt", task.NextRunAt?.ToString("O")),
                P("RunWithoutAsking", task.RunWithoutAsking ? 1 : 0),
                P("UpdatedAt", DateTime.UtcNow.ToString("O")));
            if (expectedUpdatedAt != null && changed != 1)
                throw new InvalidOperationException("The calendar entry changed or is running. Refresh before saving; no definition was overwritten.");
            RaiseChanged();
            AutomatonDebug.Log($"CalendarService.SaveDefinitionAsync: Exit - id={task.Id}");
        });
    }

    public Task SetStatusAsync(string id, CalendarTaskStatus status)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("id is required", nameof(id));
        }

        return Task.Run(() =>
        {
            AutomatonDebug.Log($"CalendarService.SetStatusAsync: Entry - id={id}, status={status}");
            Database.Execute("UPDATE CalendarTasks SET Status=@Status, UpdatedAt=@Now WHERE Id=@Id",
                P("Status", status.ToString()), P("Now", DateTime.UtcNow.ToString("O")), P("Id", id));
            RaiseChanged();
            AutomatonDebug.Log($"CalendarService.SetStatusAsync: Exit - id={id}");
        });
    }

    /// <summary>
    /// Puts a paused schedule back to pending and moves its next run to the first occurrence
    /// after now, so missed occurrences are not replayed. Exhausted series complete;
    /// an overdue or previously attempted one-off remains eligible for immediate retry.
    /// </summary>
    public async Task ResumeAsync(string id)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("id is required", nameof(id));
        }

        AutomatonDebug.Log($"CalendarService.ResumeAsync: Entry - id={id}");
        var task = await GetTaskAsync(id).ConfigureAwait(false);
        if (task == null)
        {
            AutomatonDebug.LogWarning($"CalendarService.ResumeAsync: Exit - id={id} not found");
            throw new ArgumentException("Task not found.");
        }

        if (task.Status != CalendarTaskStatus.Paused)
        {
            AutomatonDebug.LogWarning($"CalendarService.ResumeAsync: Exit - id={id} is {task.Status}, not Paused");
            throw new ArgumentException("Only paused schedules can be resumed.");
        }

        // Next run first, then the status: a UI refreshing on the status change reads a schedule
        // that has already been moved past the missed occurrences rather than the stale one.
        var now = DateTime.UtcNow;
        DateTime? next = CalendarSchedule.NextAfter(task, now);
        if (!task.IsWatch && task.TaskType == CalendarTaskType.OneOff)
        {
            next = now;
            // NextAfter intentionally returns no recurrence for a one-off.
            // An unrun future occurrence must keep its deadline when resumed.
            var pending = task.Schedule.NextOverrideUtc ?? task.NextRunAt ?? CalendarSchedule.FirstRunUtc(task);
            if (!task.LastRunAt.HasValue && task.RunCount == 0 && pending > now) next = pending;
        }
        next = ExcludeCompletedOccurrences(task, next);
        if (next.HasValue)
        {
            // Reuse the scheduler's end-date/count eligibility rules, including
            // interval checks. SetNextAsync consumes the old one-time override.
            task.Status = CalendarTaskStatus.Pending;
            task.NextRunAt = next;
            task.Schedule.NextOverrideUtc = null;
            next = CalendarSchedule.NextRunUtc(task);
        }
        await SetNextAsync(id, next).ConfigureAwait(false);
        await SetStatusAsync(id, next == null ? CalendarTaskStatus.Completed : CalendarTaskStatus.Pending).ConfigureAwait(false);
        AutomatonDebug.Log($"CalendarService.ResumeAsync: Exit - id={id} next={next:O}");
    }

    /// <summary>Records the pending occurrence as skipped and moves the schedule past it.</summary>
    public async Task SkipNextAsync(string id)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("id is required", nameof(id));
        }

        AutomatonDebug.Log($"CalendarService.SkipNextAsync: Entry - id={id}");
        var task = await GetTaskAsync(id).ConfigureAwait(false);
        if (task == null)
        {
            throw new ArgumentException("Task not found.");
        }

        var due = CalendarSchedule.NextRunUtc(task);
        if (due == null)
        {
            throw new ArgumentException("No scheduled run to skip.");
        }

        var run = new CalendarRun
        {
            TaskId = id,
            ScheduledAt = due.Value,
            Outcome = "Skipped",
            Summary = "Skipped by request.",
            FinishedAt = DateTime.UtcNow
        };
        await SaveRunAsync(run).ConfigureAwait(false);

        var next = ExcludeCompletedOccurrences(task, CalendarSchedule.NextAfter(task, due.Value));
        await SetNextAsync(id, next).ConfigureAwait(false);
        if (next == null)
        {
            AutomatonDebug.Log($"CalendarService.SkipNextAsync: id={id} has no further occurrence; completing");
            await SetStatusAsync(id, CalendarTaskStatus.Completed).ConfigureAwait(false);
        }

        AutomatonDebug.Log($"CalendarService.SkipNextAsync: Exit - id={id} skipped={due:O} next={next:O}");
    }

    public Task SetNextAsync(string id, DateTime? next)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("id is required", nameof(id));
        }

        return Task.Run(() =>
        {
            AutomatonDebug.Log($"CalendarService.SetNextAsync: Entry - id={id}, next={next:O}");
            // Advancing or explicitly replacing the pending run consumes its
            // one-time override; leaving it behind replays the same occurrence.
            Database.Execute("""
                UPDATE CalendarTasks SET NextRunAt=@Next,
                    ScheduleJson=CASE WHEN json_valid(ScheduleJson) THEN json_remove(ScheduleJson,'$.NextOverrideUtc') ELSE ScheduleJson END,
                    UpdatedAt=@Now WHERE Id=@Id
                """,
                P("Next", next?.ToString("O")), P("Now", DateTime.UtcNow.ToString("O")), P("Id", id));
            RaiseChanged();
            AutomatonDebug.Log($"CalendarService.SetNextAsync: Exit - id={id}");
        });
    }

    // ── Run claims ────────────────────────────────────────────────────

    /// <summary>
    /// Atomically claims the task for one run. Returns false when another process (or this one)
    /// already holds a claim. The desktop, its headless child hosts and the CLI all share config.db
    /// and all run a scheduler, so this is what stops a task firing twice.
    /// </summary>
    public Task<bool> TryClaimRunAsync(string taskId, string runId, string owner)
    {
        if (string.IsNullOrWhiteSpace(taskId))
        {
            throw new ArgumentException("taskId is required", nameof(taskId));
        }

        if (string.IsNullOrWhiteSpace(runId))
        {
            throw new ArgumentException("runId is required", nameof(runId));
        }

        if (string.IsNullOrWhiteSpace(owner))
        {
            throw new ArgumentException("owner is required", nameof(owner));
        }

        return Task.Run(() =>
        {
            AutomatonDebug.Log($"CalendarService.TryClaimRunAsync: Entry - taskId={taskId}, runId={runId}, owner={owner}");
            var affected = Database.Execute(@"UPDATE CalendarTasks
                SET ClaimedRunId=@Run, ClaimedBy=@By, ClaimedAt=@Now
                WHERE Id=@Id AND ClaimedRunId IS NULL",
                P("Run", runId), P("By", owner), P("Now", DateTime.UtcNow.ToString("O")), P("Id", taskId));
            var claimed = affected == 1;
            AutomatonDebug.Log($"CalendarService.TryClaimRunAsync: Exit - taskId={taskId} claimed={claimed}");
            return claimed;
        });
    }

    /// <summary>Releases a claim, but only if <paramref name="runId"/> still holds it.</summary>
    public Task ReleaseRunClaimAsync(string taskId, string runId)
    {
        if (string.IsNullOrWhiteSpace(taskId))
        {
            throw new ArgumentException("taskId is required", nameof(taskId));
        }

        if (string.IsNullOrWhiteSpace(runId))
        {
            throw new ArgumentException("runId is required", nameof(runId));
        }

        return Task.Run(() =>
        {
            AutomatonDebug.Log($"CalendarService.ReleaseRunClaimAsync: Entry - taskId={taskId}, runId={runId}");
            var affected = Database.Execute(@"UPDATE CalendarTasks
                SET ClaimedRunId=NULL, ClaimedBy=NULL, ClaimedAt=NULL
                WHERE Id=@Id AND ClaimedRunId=@Run",
                P("Id", taskId), P("Run", runId));
            if (affected == 0)
            {
                AutomatonDebug.LogWarning($"CalendarService.ReleaseRunClaimAsync: taskId={taskId} runId={runId} no longer held the claim");
            }

            RaiseChanged();
            AutomatonDebug.Log($"CalendarService.ReleaseRunClaimAsync: Exit - taskId={taskId} released={affected == 1}");
        });
    }

    /// <summary>
    /// Clears claims left behind by processes on this machine that are no longer running (a crash
    /// or a kill mid-run) and closes their run rows as failed. Claims held by live processes,
    /// including other hosts sharing the database, are left alone.
    /// </summary>
    public Task<int> ReleaseDeadClaimsAsync(string machineName, Func<int, bool> isProcessAlive)
    {
        if (string.IsNullOrWhiteSpace(machineName))
        {
            throw new ArgumentException("machineName is required", nameof(machineName));
        }

        if (isProcessAlive == null)
        {
            throw new ArgumentNullException(nameof(isProcessAlive));
        }

        return Task.Run(() =>
        {
            AutomatonDebug.Log($"CalendarService.ReleaseDeadClaimsAsync: Entry - machine={machineName}");
            var rows = Database.Query("SELECT Id, ClaimedRunId, ClaimedBy FROM CalendarTasks WHERE ClaimedRunId IS NOT NULL");
            var released = 0;
            var prefix = machineName + ":";
            foreach (var row in rows)
            {
                var taskId = row["Id"] as string;
                var runId = row["ClaimedRunId"] as string;
                var owner = row["ClaimedBy"] as string;
                if (taskId == null || runId == null || owner == null)
                {
                    continue;
                }

                if (!owner.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                {
                    AutomatonDebug.Log($"CalendarService.ReleaseDeadClaimsAsync: taskId={taskId} claimed by {owner} on another machine; leaving");
                    continue;
                }

                if (!int.TryParse(owner.AsSpan(prefix.Length), out var pid))
                {
                    AutomatonDebug.LogWarning($"CalendarService.ReleaseDeadClaimsAsync: taskId={taskId} has unparseable owner {owner}; releasing");
                }
                else if (isProcessAlive(pid))
                {
                    AutomatonDebug.Log($"CalendarService.ReleaseDeadClaimsAsync: taskId={taskId} claimed by live process {pid}; leaving");
                    continue;
                }

                Database.Execute(@"UPDATE CalendarRuns SET Outcome='Failed', FinishedAt=@Now,
                    Summary='The host process ended before this run finished.' WHERE Id=@Run AND FinishedAt IS NULL",
                    P("Now", DateTime.UtcNow.ToString("O")), P("Run", runId));
                Database.Execute(@"UPDATE CalendarTasks SET ClaimedRunId=NULL, ClaimedBy=NULL, ClaimedAt=NULL
                    WHERE Id=@Id AND ClaimedRunId=@Run", P("Id", taskId), P("Run", runId));
                AutomatonDebug.LogWarning($"CalendarService.ReleaseDeadClaimsAsync: released taskId={taskId} runId={runId} held by dead process {owner}");
                released++;
            }

            if (released > 0)
            {
                RaiseChanged();
            }

            AutomatonDebug.Log($"CalendarService.ReleaseDeadClaimsAsync: Exit - released={released}");
            return released;
        });
    }

    // ── Run history ───────────────────────────────────────────────────

    /// <summary>Inserts a run row, or updates its outcome fields when the id already exists.</summary>
    public Task SaveRunAsync(CalendarRun run)
    {
        if (run == null)
        {
            throw new ArgumentNullException(nameof(run));
        }

        if (string.IsNullOrWhiteSpace(run.TaskId))
        {
            throw new ArgumentException("run.TaskId is required", nameof(run));
        }

        return Task.Run(() =>
        {
            AutomatonDebug.Log($"CalendarService.SaveRunAsync: Entry - runId={run.Id}, taskId={run.TaskId}, outcome={run.Outcome}");
            Database.Execute(@"INSERT INTO CalendarRuns(Id,TaskId,SessionId,ScheduledAt,StartedAt,FinishedAt,Outcome,Summary,MessageId)
                VALUES(@Id,@TaskId,@SessionId,@ScheduledAt,@StartedAt,@FinishedAt,@Outcome,@Summary,@MessageId)
                ON CONFLICT(Id) DO UPDATE SET FinishedAt=@FinishedAt,Outcome=@Outcome,Summary=@Summary,MessageId=@MessageId",
                P("Id", run.Id),
                P("TaskId", run.TaskId),
                P("SessionId", run.SessionId),
                P("ScheduledAt", run.ScheduledAt.ToString("O")),
                P("StartedAt", run.StartedAt.ToString("O")),
                P("FinishedAt", run.FinishedAt?.ToString("O")),
                P("Outcome", run.Outcome),
                P("Summary", run.Summary),
                P("MessageId", run.MessageId));
            RaiseChanged();
            AutomatonDebug.Log($"CalendarService.SaveRunAsync: Exit - runId={run.Id}");
        });
    }

    public Task<List<CalendarRun>> ListRunsAsync(string id)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("id is required", nameof(id));
        }

        return Task.Run(() =>
        {
            AutomatonDebug.Log($"CalendarService.ListRunsAsync: Entry - id={id}");
            var runs = Database.Query(
                "SELECT * FROM CalendarRuns WHERE TaskId=@Id ORDER BY StartedAt DESC LIMIT 100", P("Id", id))
                .Select(r => new CalendarRun
                {
                    Id = (string)r["Id"]!,
                    TaskId = id,
                    SessionId = r["SessionId"] as string,
                    ScheduledAt = ParseTimestamp(r["ScheduledAt"] as string) ?? DateTime.MinValue,
                    StartedAt = ParseTimestamp(r["StartedAt"] as string) ?? DateTime.MinValue,
                    FinishedAt = ParseTimestamp(r["FinishedAt"] as string),
                    Outcome = r["Outcome"] as string ?? "Unknown",
                    Summary = r["Summary"] as string ?? string.Empty,
                    MessageId = r["MessageId"] as string
                }).ToList();
            AutomatonDebug.Log($"CalendarService.ListRunsAsync: Exit - id={id} runs={runs.Count}");
            return runs;
        });
    }

    /// <summary>
    /// Removes a run row that never became a run: the scheduler claimed the occurrence, found the
    /// target conversation mid-turn and backed off. Deleting the row rather than failing it keeps
    /// the history honest about what actually executed.
    /// </summary>
    public Task DeleteRunAsync(string runId)
    {
        if (string.IsNullOrWhiteSpace(runId))
        {
            throw new ArgumentException("runId is required", nameof(runId));
        }

        return Task.Run(() =>
        {
            AutomatonDebug.Log($"CalendarService.DeleteRunAsync: Entry - runId={runId}");
            var removed = Database.Execute("DELETE FROM CalendarRuns WHERE Id=@Id", P("Id", runId));
            RaiseChanged();
            AutomatonDebug.Log($"CalendarService.DeleteRunAsync: Exit - runId={runId} removed={removed}");
        });
    }

    // ── Thread lookup ─────────────────────────────────────────────────

    /// <summary>Every chat a task can target: project threads plus the workspace chat.</summary>
    public Task<List<CalendarThreadChoice>> ListThreadsAsync()
    {
        return Task.Run(() =>
        {
            AutomatonDebug.Log("CalendarService.ListThreadsAsync: Entry");
            var threads = Database.Query(@"
                SELECT t.StateFilePath AS StateId,t.Name,COALESCE(p.Name,'') AS ProjectName FROM Threads t
                LEFT JOIN Projects p ON p.Id=t.ProjectId WHERE t.IsArchived=0
                UNION SELECT c.Id AS StateId,'Workspace chat' AS Name,'' AS ProjectName FROM ConversationState c
                WHERE c.Id='desktop_default'
                ORDER BY ProjectName,Name")
                .Select(r => new CalendarThreadChoice((string)r["StateId"]!, (string)r["Name"]!, (string)r["ProjectName"]!))
                .ToList();
            AutomatonDebug.Log($"CalendarService.ListThreadsAsync: Exit - {threads.Count} threads");
            return threads;
        });
    }

    /// <summary>
    /// Maps either a persistence state id or a conversation SessionId to the state id, preferring an
    /// exact state id match. The tool historically stored SessionId, which is not the persistence key.
    /// </summary>
    public string? ResolveStateId(string? sessionOrStateId)
    {
        if (string.IsNullOrWhiteSpace(sessionOrStateId))
        {
            return null;
        }

        var rows = Database.Query(
            "SELECT Id FROM ConversationState WHERE Id=@Id OR SessionId=@Id ORDER BY CASE WHEN Id=@Id THEN 0 ELSE 1 END LIMIT 1",
            P("Id", sessionOrStateId));
        var resolved = rows.FirstOrDefault()?["Id"] as string;
        if (resolved == null)
        {
            AutomatonDebug.Log($"CalendarService.ResolveStateId: {sessionOrStateId} matches no conversation");
        }

        return resolved;
    }

    /// <summary>The saved conversation of a project chat thread (Threads.StateFilePath), or null if it has none.</summary>
    public string? ResolveThreadStateId(string? threadId)
    {
        if (string.IsNullOrWhiteSpace(threadId))
        {
            return null;
        }

        var rows = Database.Query("SELECT StateFilePath FROM Threads WHERE Id=@Id LIMIT 1", P("Id", threadId));
        var stateId = rows.FirstOrDefault()?["StateFilePath"] as string;
        AutomatonDebug.Log($"CalendarService.ResolveThreadStateId: thread {threadId} -> {stateId ?? "(none)"}");
        return string.IsNullOrWhiteSpace(stateId) ? null : ResolveStateId(stateId) ?? stateId;
    }

    private static SqliteParameter P(string name, object? value)
    {
        return new SqliteParameter("@" + name, value ?? DBNull.Value);
    }
}
