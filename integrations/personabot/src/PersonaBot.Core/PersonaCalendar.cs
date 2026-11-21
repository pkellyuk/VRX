using System.Diagnostics;
using System.Globalization;
using Automaton.Core.Models;
using Automaton.Core.Services;

namespace PersonaBot;

/// <summary>How often a booking repeats.</summary>
public enum BookingRepeat
{
    None,
    Daily,
    Weekdays,
    Weekly,
    Interval
}

/// <summary>
/// One persona's calendar entry (PERSONAS.md §4): when it works and on what. <see cref="Start"/> is local
/// time. A recurring entry appears once per occurrence, each with the same <see cref="TaskId"/>.
/// </summary>
public sealed record Booking
{
    public string TaskId { get; init; } = "";
    public required string PersonaId { get; init; }
    public required string Title { get; init; }

    /// <summary>What the persona should do; Automaton's "instructions for the assistant".</summary>
    public required string Instructions { get; init; }

    public required DateTime Start { get; init; }
    public int DurationMinutes { get; init; } = PersonaCalendar.DefaultDurationMinutes;

    /// <summary>Whether this work needs the one shared desktop; such bookings may not overlap (§5).</summary>
    public bool NeedsDesktop { get; init; } = true;

    public BookingRepeat Repeat { get; init; } = BookingRepeat.None;
    public int RepeatMinutes { get; init; } = 30;

    public DateTime End => Start.AddMinutes(DurationMinutes);

    public bool Overlaps(Booking other)
    {
        ArgumentNullException.ThrowIfNull(other);
        return Start < other.End && other.Start < End;
    }
}

/// <summary>A refused booking: it would share the desktop with <see cref="Clash"/>. <see cref="FreeSlots"/> are the next times that fit.</summary>
public sealed class BookingClashException(Booking clash, string holder, IReadOnlyList<DateTime> freeSlots)
    : InvalidOperationException($"{holder} has the desktop for “{clash.Title}” from {clash.Start:ddd d MMM HH:mm} to {clash.End:HH:mm}.")
{
    public Booking Clash { get; } = clash;
    public string Holder { get; } = holder;
    public IReadOnlyList<DateTime> FreeSlots { get; } = freeSlots;
}

/// <summary>
/// The personas' calendar, stored as Automaton calendar tasks (CalendarTasks, through
/// <see cref="CalendarService"/>) so Automaton's scheduling, recurrence and run history apply. Each
/// task targets one of its persona's chats (Schedule.TargetSessionId, as Automaton routes scheduled
/// runs); what Automaton has no column for (the persona, the duration, whether the desktop is needed)
/// is in PersonaBotBookings beside it. Desktop bookings never overlap: a clash is refused and the
/// next free times are offered, as people do with a meeting room (PERSONAS.md §5, decision 1).
/// </summary>
public sealed class PersonaCalendar
{
    public const int DefaultDurationMinutes = 30;
    public const string Tag = "personabot";

    /// <summary>How far ahead a repeating booking is checked for clashes.</summary>
    public static readonly TimeSpan RepeatCheckWindow = TimeSpan.FromDays(56);

    private readonly CalendarService _calendar = new();
    private readonly Func<IReadOnlyList<Persona>> _personas;
    private readonly Automaton.Core.Services.ConfigDatabase _db;

    public PersonaCalendar(PersonaStore personas) : this(() => personas.All)
    {
        ArgumentNullException.ThrowIfNull(personas);
    }

    public PersonaCalendar(Func<IReadOnlyList<Persona>> personas)
    {
        _personas = personas ?? throw new ArgumentNullException(nameof(personas));
        _db = PersonaBotDatabase.EnsureReady();
        Trace.WriteLine("PersonaCalendar ctor: ready");
    }

    /// <summary>Every booking (each occurrence of a repeating one) that starts between <paramref name="from"/> and <paramref name="to"/>.</summary>
    public async Task<IReadOnlyList<Booking>> ListAsync(DateTime from, DateTime to)
    {
        if (to <= from)
        {
            Trace.TraceWarning($"PersonaCalendar.ListAsync: refused range {from:yyyy-MM-dd HH:mm} to {to:yyyy-MM-dd HH:mm}");
            throw new ArgumentException("The end of the range must be after its start.");
        }
        Trace.WriteLine($"PersonaCalendar.ListAsync enter: {from:yyyy-MM-dd HH:mm} to {to:yyyy-MM-dd HH:mm}");
        var extras = _db.Query("SELECT TaskId, PersonaId, DurationMinutes, NeedsDesktop FROM PersonaBotBookings")
            .ToDictionary(r => (string)r["TaskId"]!, r => r);
        // Automaton filters by due date; a day either side catches entries that cross midnight.
        var tasks = await _calendar.ListTasksAsync(from.Date.AddDays(-1), to.Date.AddDays(1));
        var result = new List<Booking>();
        var badTimes = 0;
        foreach (var task in tasks)
        {
            if (task.IsWatch || task.Status is CalendarTaskStatus.Cancelled || !extras.TryGetValue(task.Id, out var extra)) continue;
            if (!TimeSpan.TryParseExact(task.DueTime, @"hh\:mm", CultureInfo.InvariantCulture, out var time))
            {
                badTimes++;
                continue;
            }
            var booking = new Booking
            {
                TaskId = task.Id, PersonaId = (string)extra["PersonaId"]!, Title = task.Title, Instructions = task.Description ?? "",
                Start = task.DueDate.Date + time, DurationMinutes = Convert.ToInt32(extra["DurationMinutes"]),
                NeedsDesktop = Convert.ToInt64(extra["NeedsDesktop"]) != 0, Repeat = RepeatOf(task), RepeatMinutes = task.Recurrence == RecurrenceType.Minutely ? task.RecurrenceInterval : 30
            };
            if (booking.Repeat == BookingRepeat.Interval)
            {
                var skip = Math.Max(0L, (long)Math.Floor(((from - booking.Start).TotalMinutes - booking.DurationMinutes) / booking.RepeatMinutes));
                var first = booking with { Start = booking.Start.AddMinutes(skip * booking.RepeatMinutes) };
                foreach (var occurrence in Occurrences(first, to))
                {
                    if (occurrence.End > from && occurrence.Start < to) result.Add(occurrence);
                    if (result.Count > 100000) throw new ArgumentException("Choose a shorter calendar range for minute-based repeats.");
                }
            }
            else if (booking.End > from && booking.Start < to) result.Add(booking);
        }
        if (badTimes > 0) Trace.TraceWarning($"PersonaCalendar.ListAsync: skipped {badTimes} task(s) with an unreadable due time");
        Trace.WriteLine($"PersonaCalendar.ListAsync exit: {result.Count} bookings of {tasks.Count} calendar tasks ({extras.Count} PersonaBot bookings)");
        return result.OrderBy(b => b.Start).ToList();
    }

    /// <summary>One booking by its id, as it was set up (a repeating one at its first time); null when there's no such booking.</summary>
    public async Task<Booking?> GetAsync(string taskId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(taskId);
        Trace.WriteLine($"PersonaCalendar.GetAsync enter: {taskId}");
        var extra = _db.Query("SELECT TaskId, PersonaId, DurationMinutes, NeedsDesktop FROM PersonaBotBookings WHERE TaskId = @Id", ChatStore.P("@Id", taskId)).FirstOrDefault();
        var task = extra == null ? null : await _calendar.GetTaskAsync(taskId);
        if (extra == null || task == null || task.Status is CalendarTaskStatus.Cancelled ||
            !TimeSpan.TryParseExact(task.DueTime, @"hh\:mm", CultureInfo.InvariantCulture, out var time))
        {
            Trace.WriteLine($"PersonaCalendar.GetAsync exit: {taskId} not found (extras={extra != null}, task={task != null})");
            return null;
        }
        var booking = new Booking
        {
            TaskId = task.Id, PersonaId = (string)extra["PersonaId"]!, Title = task.Title, Instructions = task.Description ?? "",
            Start = task.DueDate.Date + time, DurationMinutes = Convert.ToInt32(extra["DurationMinutes"]),
            NeedsDesktop = Convert.ToInt64(extra["NeedsDesktop"]) != 0, Repeat = RepeatOf(task), RepeatMinutes = task.Recurrence == RecurrenceType.Minutely ? task.RecurrenceInterval : 30
        };
        Trace.WriteLine($"PersonaCalendar.GetAsync exit: {taskId} at {booking.Start:yyyy-MM-dd HH:mm}");
        return booking;
    }

    /// <summary>Books time for a persona; refused with <see cref="BookingClashException"/> when it would share the desktop.</summary>
    public async Task<Booking> CreateAsync(Booking booking)
    {
        ArgumentNullException.ThrowIfNull(booking);
        Trace.WriteLine($"PersonaCalendar.CreateAsync enter: {booking.PersonaId} '{booking.Title}' {booking.Start:yyyy-MM-dd HH:mm} {booking.DurationMinutes}m");
        var persona = Validate(booking);
        await RefuseClashAsync(booking, ignoreTaskId: null);
        var task = ToTask(booking, persona, new CalendarTask());
        CalendarSchedule.Validate(task);
        await _calendar.CreateTaskAsync(task);
        await _calendar.SaveDefinitionAsync(task, reschedule: true);
        SaveExtras(task.Id, booking);
        Trace.WriteLine($"PersonaCalendar.CreateAsync exit: {task.Id}");
        return booking with { TaskId = task.Id };
    }

    /// <summary>Changes a booking (all its occurrences when it repeats); the same clash rule applies.</summary>
    public async Task<Booking> UpdateAsync(Booking booking)
    {
        ArgumentNullException.ThrowIfNull(booking);
        ArgumentException.ThrowIfNullOrWhiteSpace(booking.TaskId);
        Trace.WriteLine($"PersonaCalendar.UpdateAsync enter: {booking.TaskId}");
        var persona = Validate(booking);
        var existing = await _calendar.GetTaskAsync(booking.TaskId);
        if (existing == null)
        {
            Trace.TraceWarning($"PersonaCalendar.UpdateAsync: {booking.TaskId} no longer exists");
            throw new ArgumentException("That calendar entry no longer exists.");
        }
        await RefuseClashAsync(booking, ignoreTaskId: booking.TaskId);
        var task = ToTask(booking, persona, existing);
        await _calendar.SaveDefinitionAsync(task, reschedule: true);
        SaveExtras(task.Id, booking);
        Trace.WriteLine($"PersonaCalendar.UpdateAsync exit: {booking.TaskId} {booking.Start:yyyy-MM-dd HH:mm} {booking.DurationMinutes}m, repeat {booking.Repeat}");
        return booking;
    }

    public async Task DeleteAsync(string taskId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(taskId);
        Trace.WriteLine($"PersonaCalendar.DeleteAsync enter: {taskId}");
        await _calendar.DeleteTaskAsync(taskId, onlyIfIdle: true);
        _db.Execute("DELETE FROM PersonaBotBookings WHERE TaskId = @Id", ChatStore.P("@Id", taskId));
        Trace.WriteLine($"PersonaCalendar.DeleteAsync exit: {taskId}");
    }

    /// <summary>The next <paramref name="count"/> start times after <paramref name="after"/> when the desktop is free for <paramref name="minutes"/>, on the quarter hour.</summary>
    public async Task<IReadOnlyList<DateTime>> FreeSlotsAsync(DateTime after, int minutes, int count = 3, string? ignoreTaskId = null)
    {
        if (minutes <= 0)
        {
            Trace.TraceWarning($"PersonaCalendar.FreeSlotsAsync: refused {minutes} minutes");
            throw new ArgumentException("The duration must be positive.");
        }
        Trace.WriteLine($"PersonaCalendar.FreeSlotsAsync enter: after {after:yyyy-MM-dd HH:mm}, {minutes}m, {count} wanted, ignoring {ignoreTaskId ?? "none"}");
        var start = RoundUpToQuarter(after);
        var taken = (await ListAsync(start, start.AddDays(14))).Where(b => b.NeedsDesktop && b.TaskId != ignoreTaskId).ToList();
        var slots = new List<DateTime>();
        for (var candidate = start; slots.Count < count && candidate < start.AddDays(14); candidate = candidate.AddMinutes(15))
        {
            var end = candidate.AddMinutes(minutes);
            var blocker = taken.FirstOrDefault(b => candidate < b.End && b.Start < end);
            if (blocker != null)
            {
                candidate = RoundUpToQuarter(blocker.End).AddMinutes(-15);
                continue;
            }
            slots.Add(candidate);
            candidate = end.AddMinutes(-15);
        }
        Trace.WriteLine($"PersonaCalendar.FreeSlotsAsync exit: {slots.Count} free slot(s) around {taken.Count} desktop booking(s){(slots.Count > 0 ? $", first {slots[0]:yyyy-MM-dd HH:mm}" : "")}");
        return slots;
    }

    private async Task RefuseClashAsync(Booking booking, string? ignoreTaskId)
    {
        if (!booking.NeedsDesktop)
        {
            Trace.WriteLine($"PersonaCalendar.RefuseClashAsync: {booking.PersonaId}'s booking doesn't need the desktop");
            return;
        }
        var window = booking.Repeat == BookingRepeat.None ? booking.DurationMinutes : RepeatCheckWindow.TotalMinutes;
        var others = (await ListAsync(booking.Start.AddDays(-1), booking.Start.AddMinutes(window).AddDays(1)))
            .Where(b => b.NeedsDesktop && b.TaskId != ignoreTaskId).OrderBy(b => b.Start).ToList();
        var index = 0;
        foreach (var occurrence in Occurrences(booking, booking.Start.AddMinutes(window)))
        {
            while (index < others.Count && others[index].End <= occurrence.Start) index++;
            var clash = others.Skip(index).TakeWhile(b => b.Start < occurrence.End).FirstOrDefault(occurrence.Overlaps);
            if (clash == null) continue;
            var holder = _personas().FirstOrDefault(p => p.Id == clash.PersonaId)?.Name ?? "Another bot";
            var free = await FreeSlotsAsync(occurrence.Start, booking.DurationMinutes, 3, ignoreTaskId);
            Trace.WriteLine($"PersonaCalendar: refused, clashes with {clash.TaskId} ({holder})");
            throw new BookingClashException(clash, holder, free);
        }
        Trace.WriteLine($"PersonaCalendar.RefuseClashAsync: no clash with {others.Count} other desktop booking(s)");
    }

    /// <summary>The occurrences of a booking starting before <paramref name="until"/>.</summary>
    public static IEnumerable<Booking> Occurrences(Booking booking, DateTime until)
    {
        ArgumentNullException.ThrowIfNull(booking);
        if (booking.Repeat == BookingRepeat.None)
        {
            yield return booking;
            yield break;
        }
        if (booking.Repeat == BookingRepeat.Interval && booking.RepeatMinutes is < 1 or > 10080) throw new ArgumentException("Repeat every 1 to 10,080 minutes.");
        for (var start = booking.Start; start < until; start = booking.Repeat == BookingRepeat.Interval ? start.AddMinutes(booking.RepeatMinutes) : start.AddDays(booking.Repeat == BookingRepeat.Weekly ? 7 : 1))
        {
            if (booking.Repeat == BookingRepeat.Weekdays && start.DayOfWeek is DayOfWeek.Saturday or DayOfWeek.Sunday) continue;
            yield return booking with { Start = start };
        }
    }

    private Persona Validate(Booking booking)
    {
        if (!Enum.IsDefined(booking.Repeat)) throw new ArgumentException("Choose a valid recurrence.");
        if (booking.Repeat == BookingRepeat.Interval)
        {
            if (booking.RepeatMinutes is < 1 or > 10080) throw new ArgumentException("Repeat every 1 to 10,080 minutes.");
            if (booking.NeedsDesktop && booking.DurationMinutes > booking.RepeatMinutes)
                throw new ArgumentException("The desktop booking's duration cannot exceed its repeat interval.");
        }
        if (string.IsNullOrWhiteSpace(booking.Title))
        {
            Trace.TraceWarning($"PersonaCalendar.Validate: refused {booking.PersonaId}'s booking: no title");
            throw new ArgumentException("Enter what the booking is for.");
        }
        if (string.IsNullOrWhiteSpace(booking.Instructions))
        {
            Trace.TraceWarning($"PersonaCalendar.Validate: refused {booking.PersonaId}'s booking: no instructions");
            throw new ArgumentException("Enter what the bot should do.");
        }
        if (booking.DurationMinutes is < 5 or > 24 * 60)
        {
            Trace.TraceWarning($"PersonaCalendar.Validate: refused {booking.PersonaId}'s booking: {booking.DurationMinutes} minutes");
            throw new ArgumentException("A booking lasts from 5 minutes to 24 hours.");
        }
        if (booking.Start.Second != 0 || booking.Start.Millisecond != 0)
        {
            Trace.TraceWarning($"PersonaCalendar.Validate: refused {booking.PersonaId}'s booking: start {booking.Start:HH:mm:ss.fff} isn't on the minute");
            throw new ArgumentException("Bookings start on the minute.");
        }
        return _personas().FirstOrDefault(persona => persona.Id == booking.PersonaId)
            ?? throw new ArgumentException("The booking’s companion does not exist.");
    }

    private static CalendarTask ToTask(Booking booking, Persona persona, CalendarTask task)
    {
        task.Title = booking.Title.Trim();
        task.Description = booking.Instructions.Trim();
        task.DueDate = booking.Start.Date;
        task.DueTime = booking.Start.ToString("HH:mm", CultureInfo.InvariantCulture);
        task.TaskType = booking.Repeat == BookingRepeat.None ? CalendarTaskType.OneOff : CalendarTaskType.Recurring;
        task.Recurrence = booking.Repeat switch
        {
            BookingRepeat.Daily or BookingRepeat.Weekdays => RecurrenceType.Daily,
            BookingRepeat.Weekly => RecurrenceType.Weekly,
            BookingRepeat.Interval => RecurrenceType.Minutely,
            _ => null
        };
        task.RecurrenceInterval = booking.Repeat == BookingRepeat.Interval ? booking.RepeatMinutes : 1;
        task.Schedule.WeekdaysOnly = booking.Repeat == BookingRepeat.Weekdays;
        task.Schedule.TimeZoneId = TimeZoneInfo.Local.Id;
        // Automaton routes a scheduled run to its target chat: the persona's open chat.
        var chats = new ChatStore(persona.ProjectId, projectName: persona.Name);
        task.Schedule.TargetSessionId = chats.StateId(chats.ActiveId);
        Trace.WriteLine($"PersonaCalendar.ToTask: {persona.Id} {task.TaskType} at {task.DueDate:yyyy-MM-dd} {task.DueTime}, repeat {booking.Repeat}, target chat {task.Schedule.TargetSessionId}");
        if (!task.Tags.Contains(Tag)) task.Tags.Add(Tag);
        if (!task.Tags.Contains("persona:" + persona.Id))
        {
            task.Tags.RemoveAll(t => t.StartsWith("persona:", StringComparison.Ordinal));
            task.Tags.Add("persona:" + persona.Id);
        }
        return task;
    }

    private void SaveExtras(string taskId, Booking booking)
    {
        Trace.WriteLine($"PersonaCalendar.SaveExtras: {taskId} for {booking.PersonaId}, {booking.DurationMinutes}m, needs desktop={booking.NeedsDesktop}");
        _db.Execute(@"INSERT INTO PersonaBotBookings (TaskId, PersonaId, DurationMinutes, NeedsDesktop) VALUES (@Id, @Persona, @Minutes, @Desktop)
                ON CONFLICT(TaskId) DO UPDATE SET PersonaId = excluded.PersonaId, DurationMinutes = excluded.DurationMinutes, NeedsDesktop = excluded.NeedsDesktop",
            ChatStore.P("@Id", taskId), ChatStore.P("@Persona", booking.PersonaId), ChatStore.P("@Minutes", booking.DurationMinutes), ChatStore.P("@Desktop", booking.NeedsDesktop ? 1 : 0));
    }

    private static BookingRepeat RepeatOf(CalendarTask task) => task.TaskType != CalendarTaskType.Recurring ? BookingRepeat.None : task.Recurrence switch
    {
        RecurrenceType.Minutely => BookingRepeat.Interval,
        RecurrenceType.Weekly => BookingRepeat.Weekly,
        RecurrenceType.Daily when task.Schedule.WeekdaysOnly => BookingRepeat.Weekdays,
        RecurrenceType.Daily => BookingRepeat.Daily,
        _ => BookingRepeat.None
    };

    private static DateTime RoundUpToQuarter(DateTime time)
    {
        var trimmed = new DateTime(time.Year, time.Month, time.Day, time.Hour, time.Minute, 0, time.Kind);
        var remainder = trimmed.Minute % 15;
        var rounded = remainder == 0 ? trimmed : trimmed.AddMinutes(15 - remainder);
        return rounded < time ? rounded.AddMinutes(15) : rounded;
    }
}
