using System.Globalization;
using System.Text.Json;
using Automaton.Core.Models;

namespace Automaton.Core.Services;

public partial class CalendarService
{
    // Occurrence completion is stored as a wall-clock date, shared by all
    // calendar surfaces. Automatic scheduling must honour that same date.
    internal DateTime? ExcludeCompletedOccurrences(CalendarTask task, DateTime? candidate)
    {
        if (candidate == null || task.IsWatch || task.TaskType != CalendarTaskType.Recurring) return candidate;
        var completed = Database.Query("SELECT OccurrenceDate FROM CalendarTaskOccurrences WHERE TaskId=@Id AND Status=@Status",
            P("Id", task.Id), P("Status", CalendarTaskStatus.Completed.ToString()))
            .Select(row => (string)row["OccurrenceDate"]!).ToHashSet(StringComparer.Ordinal);
        if (completed.Count == 0) return candidate;
        var zone = TimeZoneInfo.FindSystemTimeZoneById(task.Schedule.TimeZoneId);
        while (candidate.HasValue)
        {
            var localDate = TimeZoneInfo.ConvertTimeFromUtc(DateTime.SpecifyKind(candidate.Value, DateTimeKind.Utc), zone).Date;
            if (!completed.Contains(localDate.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture))) break;
            if (localDate == DateTime.MaxValue.Date) return null;
            // A stored occurrence represents the whole local date. Jump past
            // it once, including hourly/minutely series and timezone offsets.
            var endOfDate = CalendarSchedule.ToUtc(localDate.AddDays(1), task.Schedule.TimeZoneId).AddTicks(-1);
            candidate = CalendarSchedule.NextAfter(task, endOfDate);
        }
        return candidate;
    }

    private CalendarTask? ReconcileCompletedOccurrences(string id, out bool changed)
    {
        changed = false;
        var row = Database.Query("SELECT * FROM CalendarTasks WHERE Id=@Id", P("Id", id)).FirstOrDefault();
        if (row == null) return null;
        var task = RowToTask(row);
        if (task.Status != CalendarTaskStatus.Pending || task.IsWatch || task.TaskType != CalendarTaskType.Recurring) return task;
        var candidate = CalendarSchedule.NextRunUtc(task);
        var next = ExcludeCompletedOccurrences(task, candidate);
        if (next == candidate) return task;
        task.NextRunAt = next;
        task.Schedule.NextOverrideUtc = null;
        task.UpdatedAt = DateTime.UtcNow;
        if (next == null) { task.Status = CalendarTaskStatus.Completed; task.CompletedAt = task.UpdatedAt; }
        Database.Execute("UPDATE CalendarTasks SET NextRunAt=@Next, ScheduleJson=@Schedule, Status=@Status, CompletedAt=@Completed, UpdatedAt=@Now WHERE Id=@Id",
            P("Next", next?.ToString("O")), P("Schedule", JsonSerializer.Serialize(task.Schedule)),
            P("Status", task.Status.ToString()), P("Completed", task.CompletedAt?.ToString("O")),
            P("Now", task.UpdatedAt.ToString("O")), P("Id", id));
        changed = true;
        return task;
    }

    internal Task<CalendarTask?> ReconcileCompletedOccurrencesAsync(string id) => Task.Run(() =>
    {
        CalendarTask? task = null;
        var changed = false;
        Database.ExecuteInTransaction((_, _) => task = ReconcileCompletedOccurrences(id, out changed));
        if (changed) RaiseChanged();
        return task;
    });
}
