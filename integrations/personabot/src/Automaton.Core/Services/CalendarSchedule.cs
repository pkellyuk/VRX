using System.Globalization;
using Automaton.Core.Models;

namespace Automaton.Core.Services;

/// <summary>
/// Shared wall-clock scheduling rules for the calendar tool, the scheduler and the UI.
/// Every caller derives "when does this task run next" from here so the three never disagree.
/// Recurring tasks always advance from the original wall-clock schedule, never from the time a
/// run happened to finish, so a 09:00 daily task stays at 09:00 however long a run takes.
/// </summary>
public static class CalendarSchedule
{
    private static readonly string[] MissedRunPolicies = ["run_once", "skip"];
    private static readonly string[] NotificationPolicies = ["changes", "all", "failures", "custom"];

    /// <summary>Throws <see cref="ArgumentException"/> with a user-facing message when the definition cannot be scheduled.</summary>
    public static void Validate(CalendarTask task)
    {
        if (task == null)
        {
            throw new ArgumentNullException(nameof(task));
        }

        AutomatonDebug.Log($"CalendarSchedule.Validate: Entry - id={task.Id}, isWatch={task.IsWatch}, type={task.TaskType}, recurrence={task.Recurrence}");

        if (string.IsNullOrWhiteSpace(task.Title))
        {
            throw new ArgumentException("Enter a task title.");
        }

        var instructions = task.IsWatch ? task.Objective : task.Description;
        if (string.IsNullOrWhiteSpace(instructions))
        {
            throw new ArgumentException("Enter instructions for the assistant.");
        }

        if (task.Schedule == null)
        {
            throw new ArgumentException("The task has no schedule options.");
        }

        // Throws TimeZoneNotFoundException / InvalidTimeZoneException for an unknown id.
        _ = TimeZoneInfo.FindSystemTimeZoneById(task.Schedule.TimeZoneId);

        if (!task.IsWatch && !TimeSpan.TryParseExact(task.DueTime, @"hh\:mm", CultureInfo.InvariantCulture, out _))
        {
            throw new ArgumentException("Time must be HH:mm.");
        }

        if (task.IsWatch && task.IntervalMinutes is not > 0)
        {
            throw new ArgumentException("Check interval must be at least one minute.");
        }

        if (task.TaskType == CalendarTaskType.Recurring && !task.IsWatch && !task.Recurrence.HasValue)
        {
            throw new ArgumentException("Choose a repeat interval.");
        }

        if (task.RecurrenceInterval < 1 || task.EndAfter is <= 0)
        {
            throw new ArgumentException("Intervals and run counts must be positive.");
        }

        if (task.EndDate?.Date < task.DueDate.Date)
        {
            throw new ArgumentException("End date must be on or after the start date.");
        }

        if (!MissedRunPolicies.Contains(task.Schedule.MissedRunPolicy))
        {
            throw new ArgumentException("Unknown missed-run policy.");
        }

        if (!NotificationPolicies.Contains(task.Schedule.NotificationPolicy))
        {
            throw new ArgumentException("Unknown notification policy.");
        }

        if (task.Schedule.WeekdaysOnly && (task.IsWatch || task.Recurrence != RecurrenceType.Daily))
        {
            throw new ArgumentException("Weekdays is available for daily repeating tasks.");
        }

        AutomatonDebug.Log($"CalendarSchedule.Validate: Exit - id={task.Id} valid");
    }

    /// <summary>The UTC instant of the first run of a freshly defined task.</summary>
    public static DateTime FirstRunUtc(CalendarTask task)
    {
        if (task == null)
        {
            throw new ArgumentNullException(nameof(task));
        }

        AutomatonDebug.Log($"CalendarSchedule.FirstRunUtc: Entry - id={task.Id}, isWatch={task.IsWatch}, dueDate={task.DueDate:yyyy-MM-dd}, dueTime={task.DueTime}");

        if (task.IsWatch)
        {
            var firstCheck = task.NextRunAt ?? task.CreatedAt.AddMinutes(task.IntervalMinutes ?? 60);
            AutomatonDebug.Log($"CalendarSchedule.FirstRunUtc: Exit - watch id={task.Id} first={firstCheck:O}");
            return firstCheck;
        }

        var local = task.DueDate.Date.Add(ParseTime(task.DueTime));
        while (task.Schedule.WeekdaysOnly && IsWeekend(local))
        {
            local = local.AddDays(1);
        }

        var utc = ToUtc(local, task.Schedule.TimeZoneId);
        AutomatonDebug.Log($"CalendarSchedule.FirstRunUtc: Exit - id={task.Id} local={local:O} utc={utc:O}");
        return utc;
    }

    /// <summary>
    /// The next run to store when a definition is created or its schedule edited. A watch starts one
    /// interval from now. A one-off keeps its requested time even if that is already past, so it runs
    /// once as soon as the scheduler sees it. A repeating task whose series started in the past picks
    /// up at the first occurrence after now instead of replaying an old one.
    /// </summary>
    public static DateTime? InitialNextRunUtc(CalendarTask task, DateTime nowUtc)
    {
        if (task == null)
        {
            throw new ArgumentNullException(nameof(task));
        }

        AutomatonDebug.Log($"CalendarSchedule.InitialNextRunUtc: Entry - id={task.Id}, now={nowUtc:O}");
        DateTime? result;
        if (task.IsWatch)
        {
            result = nowUtc.AddMinutes(task.IntervalMinutes ?? 60);
        }
        else
        {
            var first = FirstRunUtc(task);
            result = task.TaskType == CalendarTaskType.OneOff || first > nowUtc ? first : NextAfter(task, nowUtc);
        }

        AutomatonDebug.Log($"CalendarSchedule.InitialNextRunUtc: Exit - id={task.Id} next={result:O}");
        return result;
    }

    /// <summary>
    /// The UTC instant the task should next run, or null when it never runs again
    /// (not pending, run count exhausted, past its end date, or a one-off that already ran).
    /// </summary>
    public static DateTime? NextRunUtc(CalendarTask task)
    {
        if (task == null)
        {
            throw new ArgumentNullException(nameof(task));
        }

        AutomatonDebug.Log($"CalendarSchedule.NextRunUtc: Entry - id={task.Id}, status={task.Status}, nextRunAt={task.NextRunAt:O}, lastRunAt={task.LastRunAt:O}, runCount={task.RunCount}, endAfter={task.EndAfter}");

        if (task.Status != CalendarTaskStatus.Pending)
        {
            AutomatonDebug.Log($"CalendarSchedule.NextRunUtc: Exit - id={task.Id} not pending");
            return null;
        }

        if (task.EndAfter.HasValue && task.RunCount >= task.EndAfter.Value)
        {
            AutomatonDebug.Log($"CalendarSchedule.NextRunUtc: Exit - id={task.Id} run count exhausted");
            return null;
        }

        var next = task.Schedule.NextOverrideUtc ?? task.NextRunAt;
        if (!next.HasValue)
        {
            if (task.TaskType == CalendarTaskType.OneOff && !task.IsWatch && task.LastRunAt.HasValue)
            {
                AutomatonDebug.Log($"CalendarSchedule.NextRunUtc: Exit - id={task.Id} one-off already ran");
                return null;
            }

            next = task.LastRunAt.HasValue ? NextAfter(task, task.LastRunAt.Value) : FirstRunUtc(task);
        }

        if (!next.HasValue)
        {
            AutomatonDebug.Log($"CalendarSchedule.NextRunUtc: Exit - id={task.Id} no further occurrence");
            return null;
        }

        var result = WithinEnd(task, next.Value) ? next : null;
        AutomatonDebug.Log($"CalendarSchedule.NextRunUtc: Exit - id={task.Id} next={result:O}");
        return result;
    }

    /// <summary>
    /// The first scheduled occurrence strictly after <paramref name="afterUtc"/>, or null when there is none.
    /// Watches step from the given instant; everything else steps along the original wall-clock series.
    /// </summary>
    public static DateTime? NextAfter(CalendarTask task, DateTime afterUtc)
    {
        if (task == null)
        {
            throw new ArgumentNullException(nameof(task));
        }

        AutomatonDebug.Log($"CalendarSchedule.NextAfter: Entry - id={task.Id}, after={afterUtc:O}, recurrence={task.Recurrence}, interval={task.RecurrenceInterval}");

        if (task.IsWatch)
        {
            var nextCheck = afterUtc.AddMinutes(task.IntervalMinutes ?? 60);
            AutomatonDebug.Log($"CalendarSchedule.NextAfter: Exit - watch id={task.Id} next={nextCheck:O}");
            return nextCheck;
        }

        if (task.TaskType != CalendarTaskType.Recurring || !task.Recurrence.HasValue)
        {
            AutomatonDebug.Log($"CalendarSchedule.NextAfter: Exit - id={task.Id} not recurring");
            return null;
        }

        var zone = TimeZoneInfo.FindSystemTimeZoneById(task.Schedule.TimeZoneId);
        var first = task.DueDate.Date.Add(ParseTime(task.DueTime));
        var afterLocal = TimeZoneInfo.ConvertTimeFromUtc(DateTime.SpecifyKind(afterUtc, DateTimeKind.Utc), zone);
        var step = Math.Max(1, task.RecurrenceInterval);
        var unit = task.Recurrence.Value;

        // Jump straight to the occurrence index around afterUtc instead of walking from the start.
        long index = unit switch
        {
            RecurrenceType.Minutely => (long)((afterLocal - first).TotalMinutes / step),
            RecurrenceType.Hourly => (long)((afterLocal - first).TotalHours / step),
            RecurrenceType.Daily => (long)((afterLocal - first).TotalDays / step),
            RecurrenceType.Weekly => (long)((afterLocal - first).TotalDays / (7 * step)),
            RecurrenceType.Monthly => ((afterLocal.Year - first.Year) * 12 + afterLocal.Month - first.Month) / step,
            _ => 0
        };

        for (index = Math.Max(0, index); index < int.MaxValue; index++)
        {
            DateTime candidate;
            try
            {
                candidate = Occurrence(first, unit, step, index);
            }
            catch (ArgumentOutOfRangeException)
            {
                AutomatonDebug.Log($"CalendarSchedule.NextAfter: Exit - id={task.Id} ran off the end of DateTime");
                return null;
            }

            if (task.Schedule.WeekdaysOnly && IsWeekend(candidate))
            {
                continue;
            }

            var utc = ToUtc(candidate, task.Schedule.TimeZoneId);
            if (utc <= afterUtc)
            {
                continue;
            }

            var result = WithinEnd(task, utc) ? utc : (DateTime?)null;
            AutomatonDebug.Log($"CalendarSchedule.NextAfter: Exit - id={task.Id} index={index} candidate={candidate:O} next={result:O}");
            return result;
        }

        AutomatonDebug.Log($"CalendarSchedule.NextAfter: Exit - id={task.Id} exhausted");
        return null;
    }

    /// <summary>True when <paramref name="utc"/> falls on or before the task's end date in its own timezone.</summary>
    public static bool WithinEnd(CalendarTask task, DateTime utc)
    {
        if (task == null)
        {
            throw new ArgumentNullException(nameof(task));
        }

        if (!task.EndDate.HasValue)
        {
            return true;
        }

        var zone = TimeZoneInfo.FindSystemTimeZoneById(task.Schedule.TimeZoneId);
        var local = TimeZoneInfo.ConvertTimeFromUtc(DateTime.SpecifyKind(utc, DateTimeKind.Utc), zone);
        return local.Date <= task.EndDate.Value.Date;
    }

    /// <summary>
    /// Converts a wall-clock time in the given zone to UTC. A nonexistent spring-forward time
    /// executes at the first valid minute; a repeated autumn hour resolves to standard time so it runs once.
    /// </summary>
    public static DateTime ToUtc(DateTime wallTime, string zoneId)
    {
        if (string.IsNullOrWhiteSpace(zoneId))
        {
            throw new ArgumentException("A timezone id is required.", nameof(zoneId));
        }

        var zone = TimeZoneInfo.FindSystemTimeZoneById(zoneId);
        wallTime = DateTime.SpecifyKind(wallTime, DateTimeKind.Unspecified);
        var adjusted = wallTime;
        while (zone.IsInvalidTime(adjusted))
        {
            adjusted = adjusted.AddMinutes(1);
        }

        if (adjusted != wallTime)
        {
            AutomatonDebug.Log($"CalendarSchedule.ToUtc: {wallTime:O} does not exist in {zoneId}; using {adjusted:O}");
        }

        return TimeZoneInfo.ConvertTimeToUtc(adjusted, zone);
    }

    /// <summary>One-line human description of the schedule, shared by the tool output and the UI.</summary>
    public static string Describe(CalendarTask task)
    {
        if (task == null)
        {
            throw new ArgumentNullException(nameof(task));
        }

        if (task.IsWatch)
        {
            return $"Check every {task.IntervalMinutes} minutes · {task.Schedule.TimeZoneId}";
        }

        if (task.TaskType == CalendarTaskType.OneOff)
        {
            return $"Once · {task.DueDate:dd MMM yyyy} at {task.DueTime} · {task.Schedule.TimeZoneId}";
        }

        var cadence = task.Schedule.WeekdaysOnly ? "Weekdays" : $"Every {task.RecurrenceInterval} {task.Recurrence}";
        return $"{cadence} at {task.DueTime} · {task.Schedule.TimeZoneId}";
    }

    private static DateTime Occurrence(DateTime first, RecurrenceType unit, int step, long index)
    {
        return unit switch
        {
            RecurrenceType.Minutely => first.AddMinutes(index * step),
            RecurrenceType.Hourly => first.AddHours(index * step),
            RecurrenceType.Daily => first.AddDays(index * step),
            RecurrenceType.Weekly => first.AddDays(index * step * 7),
            _ => first.AddMonths(checked((int)index * step))
        };
    }

    private static TimeSpan ParseTime(string? dueTime)
    {
        return TimeSpan.TryParse(dueTime, CultureInfo.InvariantCulture, out var time) ? time : TimeSpan.Zero;
    }

    private static bool IsWeekend(DateTime date)
    {
        return date.DayOfWeek is DayOfWeek.Saturday or DayOfWeek.Sunday;
    }
}
