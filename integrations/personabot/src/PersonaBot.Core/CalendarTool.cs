using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using Automaton.Core.Interfaces;

namespace PersonaBot;

/// <summary>
/// calendar: the bots read and change the personas' calendar (the Calendar window), through <see cref="PersonaCalendar"/>,
/// so a bot's booking is the same as one made by hand: stored as an Automaton calendar task that runs in the persona's
/// chat at its time, and refused when it would share the desktop with another, with the next free times offered.
/// Automaton's own calendar_control isn't used: it knows nothing of personas, durations or the desktop rule.
/// </summary>
/// <param name="calendar">The calendar, or null where there is none yet.</param>
/// <param name="personas">The personas, to book by name and to show who has each booking.</param>
/// <param name="callerPersona">The persona of the chat the call comes from (chat id → persona id): bookings default to it.</param>
/// <param name="changed">Called after a booking is made, changed or removed, so an open Calendar window refreshes.</param>
public sealed class CalendarTool(Func<PersonaCalendar?> calendar, Func<PersonaStore?> personas, Func<string?, string?> callerPersona, Action? changed = null) : ITool
{
    public const string ToolId = "calendar";

    public string Id => ToolId;

    public string Description =>
        "The bots' calendar (what the person sees in the Calendar window): bookings of a bot's time, each with instructions the bot carries out " +
        "in its own chat when the time comes. operation: list (bookings between from and to, default the next 7 days), free (the next free times " +
        "for duration minutes after after), create, update (only the fields given change; a repeating booking changes every time), delete, personas " +
        "(who can be booked). Times are the person's local time, \"yyyy-MM-dd HH:mm\". A booking that needs the desktop can't overlap another: " +
        "if it would, it's refused with the next free times, so offer those. Bookings are for you unless persona names another bot. " +
        "Confirm the details with the person (the question tool, with options) before booking on their behalf if they haven't said exactly.";

    public JsonSchema Parameters { get; } = new()
    {
        Properties = new Dictionary<string, JsonSchemaProperty>
        {
            ["operation"] = new() { Type = "string", Description = "What to do.", Enum = ["list", "free", "create", "update", "delete", "personas"] },
            ["id"] = new() { Type = "string", Description = "The booking's id (from list), for update and delete." },
            ["title"] = new() { Type = "string", Description = "What the booking is for, short (\"Sort the inbox\")." },
            ["instructions"] = new() { Type = "string", Description = "What the bot should do when the time comes, as you'd tell it." },
            ["start"] = new() { Type = "string", Description = "\"yyyy-MM-dd HH:mm\", local time." },
            ["duration"] = new() { Type = "integer", Description = "Minutes, 5 to 1440 (default 30)." },
            ["repeat"] = new() { Type = "string", Description = "How often it repeats (default none). Use interval for every N minutes.", Enum = ["none", "daily", "weekdays", "weekly", "interval"] },
            ["repeat_minutes"] = new() { Type = "integer", Description = "For repeat=interval: repeat every N minutes, 1 to 10080 (default 30). Desktop duration must fit within the interval." },
            ["needs_desktop"] = new() { Type = "boolean", Description = "Whether the work uses the desktop (default true); such bookings can't overlap." },
            ["persona"] = new() { Type = "string", Description = "Which bot (name or id); default you." },
            ["from"] = new() { Type = "string", Description = "list: \"yyyy-MM-dd\" (default today)." },
            ["to"] = new() { Type = "string", Description = "list: \"yyyy-MM-dd\", inclusive (default a week after from)." },
            ["after"] = new() { Type = "string", Description = "free: \"yyyy-MM-dd HH:mm\" (default now)." }
        },
        Required = ["operation"]
    };

    public async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        ArgumentNullException.ThrowIfNull(args);
        ArgumentNullException.ThrowIfNull(context);
        using var doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(args) ? "{}" : args);
        var root = doc.RootElement;
        var operation = Text(root, "operation")?.ToLowerInvariant() ?? throw new ArgumentException("Say which operation: list, free, create, update, delete or personas.");
        Trace.WriteLine($"CalendarTool.ExecuteAsync enter: {operation} from chat {context.SessionId}");
        if (calendar() is not { } diary || personas() is not { } people)
        {
            Trace.TraceWarning("CalendarTool.ExecuteAsync: no calendar yet");
            return new ToolResult { Title = "No calendar", Output = "The calendar isn't available yet (PersonaBot is still starting). Try again shortly." };
        }
        var result = operation switch
        {
            "list" => await ListAsync(diary, people, root),
            "free" => await FreeAsync(diary, root),
            "create" => await CreateAsync(diary, people, root, context),
            "update" => await UpdateAsync(diary, people, root),
            "delete" => await DeleteAsync(diary, root),
            "personas" => Personas(people),
            _ => throw new ArgumentException($"Unknown operation \"{operation}\": use list, free, create, update, delete or personas.")
        };
        Trace.WriteLine($"CalendarTool.ExecuteAsync exit: {operation}: {result.Title}");
        return result;
    }

    private static async Task<ToolResult> ListAsync(PersonaCalendar diary, PersonaStore people, JsonElement root)
    {
        var from = Date(root, "from") ?? DateTime.Today;
        var to = (Date(root, "to") ?? from.AddDays(6)).AddDays(1);
        if (to <= from) throw new ArgumentException("to must be on or after from.");
        var bookings = await diary.ListAsync(from, to);
        var output = new StringBuilder();
        output.AppendLine(bookings.Count == 0
            ? $"No bookings from {from:ddd d MMM} to {to.AddDays(-1):ddd d MMM}."
            : $"{bookings.Count} booking(s) from {from:ddd d MMM} to {to.AddDays(-1):ddd d MMM} (a repeating booking shows each time):");
        foreach (var b in bookings) output.AppendLine(Describe(b, people));
        return new ToolResult { Title = $"Calendar: {bookings.Count} booking(s)", Output = output.ToString().TrimEnd() };
    }

    private static async Task<ToolResult> FreeAsync(PersonaCalendar diary, JsonElement root)
    {
        var minutes = Int(root, "duration") ?? PersonaCalendar.DefaultDurationMinutes;
        var after = Time(root, "after") ?? DateTime.Now;
        var slots = await diary.FreeSlotsAsync(after, minutes, 5);
        var output = slots.Count == 0
            ? $"The desktop has no free {minutes}-minute slot in the next two weeks."
            : $"The desktop is free for {minutes} minutes at: " + string.Join(", ", slots.Select(s => s.ToString("ddd d MMM HH:mm", CultureInfo.InvariantCulture))) + ".";
        return new ToolResult { Title = $"Free times: {slots.Count}", Output = output };
    }

    private async Task<ToolResult> CreateAsync(PersonaCalendar diary, PersonaStore people, JsonElement root, ToolContext context)
    {
        var personaId = Persona(people, Text(root, "persona")) ?? callerPersona(context.SessionId)
            ?? throw new ArgumentException("Say which bot the booking is for (persona).");
        var booking = new Booking
        {
            PersonaId = personaId,
            Title = Text(root, "title") ?? throw new ArgumentException("Give the booking a title."),
            Instructions = Text(root, "instructions") ?? throw new ArgumentException("Say what the bot should do (instructions)."),
            Start = Time(root, "start") ?? throw new ArgumentException("Say when it starts: start \"yyyy-MM-dd HH:mm\"."),
            DurationMinutes = Int(root, "duration") ?? PersonaCalendar.DefaultDurationMinutes,
            Repeat = Repeat(root) ?? BookingRepeat.None,
            RepeatMinutes = Int(root, "repeat_minutes") ?? 30,
            NeedsDesktop = Bool(root, "needs_desktop") ?? true
        };
        if (booking.Repeat == BookingRepeat.None && booking.Start < DateTime.Now.AddMinutes(-1))
            throw new ArgumentException($"{booking.Start:ddd d MMM HH:mm} has passed: pick a time from now on.");
        try
        {
            var made = await diary.CreateAsync(booking);
            changed?.Invoke();
            return new ToolResult { Title = "Booked", Output = "Booked: " + Describe(made, people) };
        }
        catch (BookingClashException clash)
        {
            return Clash(clash);
        }
    }

    private async Task<ToolResult> UpdateAsync(PersonaCalendar diary, PersonaStore people, JsonElement root)
    {
        var id = Text(root, "id") ?? throw new ArgumentException("Give the booking's id (from list).");
        // The booking as it was set up (a repeating one from its first time), as the base for the changes.
        var existing = await diary.GetAsync(id) ?? throw new ArgumentException($"There's no booking {id} (list shows their ids).");
        var updated = existing with
        {
            PersonaId = Persona(people, Text(root, "persona")) ?? existing.PersonaId,
            Title = Text(root, "title") ?? existing.Title,
            Instructions = Text(root, "instructions") ?? existing.Instructions,
            Start = Time(root, "start") ?? existing.Start,
            DurationMinutes = Int(root, "duration") ?? existing.DurationMinutes,
            Repeat = Repeat(root) ?? existing.Repeat,
            RepeatMinutes = Int(root, "repeat_minutes") ?? existing.RepeatMinutes,
            NeedsDesktop = Bool(root, "needs_desktop") ?? existing.NeedsDesktop
        };
        try
        {
            var saved = await diary.UpdateAsync(updated);
            changed?.Invoke();
            return new ToolResult { Title = "Booking changed", Output = "Changed: " + Describe(saved, people) };
        }
        catch (BookingClashException clash)
        {
            return Clash(clash);
        }
    }

    private async Task<ToolResult> DeleteAsync(PersonaCalendar diary, JsonElement root)
    {
        var id = Text(root, "id") ?? throw new ArgumentException("Give the booking's id (from list).");
        await diary.DeleteAsync(id);
        changed?.Invoke();
        return new ToolResult { Title = "Booking removed", Output = $"Removed booking {id} (every time, if it repeated)." };
    }

    private static ToolResult Personas(PersonaStore people) => new()
    {
        Title = "Personas",
        Output = "Bots that can be booked: " + string.Join("; ", people.All.Select(p => $"{p.Name} (id {p.Id})")) + "."
    };

    private static ToolResult Clash(BookingClashException clash) => new()
    {
        Title = "Clash: not booked",
        Output = $"Not booked: {clash.Message} Only one bot can use the desktop at a time. " +
                 (clash.FreeSlots.Count == 0 ? "There's no free time for it in the next two weeks." :
                     "Free times that fit: " + string.Join(", ", clash.FreeSlots.Select(s => s.ToString("ddd d MMM HH:mm", CultureInfo.InvariantCulture))) +
                     ". Offer these to the person (with the question tool), or set needs_desktop false if the work doesn't use the desktop.")
    };

    /// <summary>One booking in a line: its id, when, who, what, how often, and whether it uses the desktop.</summary>
    internal static string Describe(Booking b, PersonaStore people)
    {
        var who = people.All.FirstOrDefault(p => p.Id == b.PersonaId)?.Name ?? b.PersonaId;
        var repeat = b.Repeat == BookingRepeat.Interval ? $", repeats every {b.RepeatMinutes} minutes" : b.Repeat == BookingRepeat.None ? "" : $", repeats {b.Repeat.ToString().ToLowerInvariant()}";
        return string.Create(CultureInfo.InvariantCulture,
            $"[{b.TaskId}] {b.Start:ddd d MMM HH:mm}–{b.End:HH:mm} · {who} · \"{b.Title}\"{repeat}{(b.NeedsDesktop ? "" : ", no desktop")} · does: {Short(b.Instructions)}");
    }

    private static string Short(string text) => text.Length <= 120 ? text : text[..119] + "…";

    /// <summary>A persona by name or id (any case); null when <paramref name="name"/> is blank; refused when there's no such bot.</summary>
    internal static string? Persona(PersonaStore people, string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return null;
        var found = people.All.FirstOrDefault(p => string.Equals(p.Id, name, StringComparison.OrdinalIgnoreCase) || string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase))
            ?? people.All.FirstOrDefault(p => p.Name.Contains(name, StringComparison.OrdinalIgnoreCase));
        return found?.Id ?? throw new ArgumentException($"There's no bot called \"{name}\": " + string.Join(", ", people.All.Select(p => p.Name)) + ".");
    }

    private static string? Text(JsonElement root, string key) =>
        root.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(v.GetString()) ? v.GetString()!.Trim() : null;

    private static int? Int(JsonElement root, string key)
    {
        if (!root.TryGetProperty(key, out var v)) return null;
        if (v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n)) return n;
        if (v.ValueKind == JsonValueKind.String && int.TryParse(v.GetString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var s)) return s;
        throw new ArgumentException($"{key} must be a whole number of minutes.");
    }

    private static bool? Bool(JsonElement root, string key) =>
        root.TryGetProperty(key, out var v) ? v.ValueKind switch { JsonValueKind.True => true, JsonValueKind.False => false, _ => null } : null;

    private static BookingRepeat? Repeat(JsonElement root) => Text(root, "repeat")?.ToLowerInvariant() switch
    {
        null => null,
        "none" or "once" => BookingRepeat.None,
        "daily" => BookingRepeat.Daily,
        "weekdays" => BookingRepeat.Weekdays,
        "weekly" => BookingRepeat.Weekly,
        "interval" => BookingRepeat.Interval,
        var other => throw new ArgumentException($"repeat \"{other}\" isn't one of none, daily, weekdays, weekly, interval.")
    };

    /// <summary>"yyyy-MM-dd HH:mm" (or ISO "yyyy-MM-ddTHH:mm"), local time, on the minute.</summary>
    internal static DateTime? Time(JsonElement root, string key)
    {
        if (Text(root, key) is not { } text) return null;
        string[] formats = ["yyyy-MM-dd HH:mm", "yyyy-MM-ddTHH:mm", "yyyy-MM-dd H:mm", "yyyy-MM-ddTHH:mm:ss", "yyyy-MM-dd HH:mm:ss"];
        if (!DateTime.TryParseExact(text, formats, CultureInfo.InvariantCulture, DateTimeStyles.None, out var time))
            throw new ArgumentException($"{key} \"{text}\" isn't \"yyyy-MM-dd HH:mm\".");
        return new DateTime(time.Year, time.Month, time.Day, time.Hour, time.Minute, 0, DateTimeKind.Unspecified);
    }

    private static DateTime? Date(JsonElement root, string key)
    {
        if (Text(root, key) is not { } text) return null;
        if (!DateTime.TryParseExact(text, ["yyyy-MM-dd", "yyyy-MM-dd HH:mm"], CultureInfo.InvariantCulture, DateTimeStyles.None, out var date))
            throw new ArgumentException($"{key} \"{text}\" isn't \"yyyy-MM-dd\".");
        return date.Date;
    }
}
