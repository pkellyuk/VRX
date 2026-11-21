using System.Globalization;
using System.Text.Json;
using Automaton.Core.Interfaces;
using PersonaBot;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal sealed partial class VrexSession
{
    private readonly Lazy<PersonaCalendar> calendar = new(() => new PersonaCalendar(() => [Vrex]));

    private async Task ReportCalendarAsync(string? startText)
    {
        DateTime start = DateTime.TryParseExact(startText, "yyyy-MM-dd", CultureInfo.InvariantCulture, DateTimeStyles.None, out var date)
            ? date : DateTime.Today;
        var bookings = await calendar.Value.ListAsync(start, start.AddDays(7));
        emit(new("calendar-start", start.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture)));
        foreach (var booking in bookings.Where(booking => booking.PersonaId == VrexProtocol.PersonaId).Take(250))
            emit(new("calendar-item", Booking: CalendarEntry(booking)));
        emit(new("calendar-ready"));
    }

    private static VrexBooking CalendarEntry(Booking booking) =>
        new(booking.TaskId, Trim(booking.Title, 120), booking.Start, booking.DurationMinutes, Trim(booking.Instructions, 1600),
            booking.Repeat == BookingRepeat.None ? null : booking.Repeat.ToString());

    private Task<Booking> CreateBookingAsync(string enteredTitle, DateTime start, int minutes, string? enteredNotes, string? enteredRepeat)
    {
        var title = enteredTitle.Trim();
        if (title is not { Length: > 0 and <= 120 }) throw new ArgumentException("Enter a title of up to 120 characters.");
        if (TimeZoneInfo.Local.IsInvalidTime(start)) throw new ArgumentException("That time does not exist on this date. Choose a different time.");
        var repeat = enteredRepeat switch
        {
            null or "none" => BookingRepeat.None, "daily" => BookingRepeat.Daily,
            "weekdays" => BookingRepeat.Weekdays, "weekly" => BookingRepeat.Weekly,
            _ => throw new ArgumentException("Choose none, daily, weekdays or weekly.")
        };
        return calendar.Value.CreateAsync(new Booking
        {
            PersonaId = VrexProtocol.PersonaId, Title = title, Start = start, DurationMinutes = minutes,
            Instructions = Trim(enteredNotes?.Trim() is { Length: > 0 } notes ? notes : title, 1600), Repeat = repeat
        });
    }

    private static string BookingClashMessage(BookingClashException ex)
    {
        var alternatives = string.Join(", ", ex.FreeSlots.Select(time => time.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture)));
        return ex.Message + (alternatives.Length == 0 ? "" : " Available times: " + alternatives);
    }

    private static JsonSchema CalendarSchema() => new()
    {
        Description = "Read or update Vrex’s local planning calendar at the user's request. Bookings reserve time and do not start a task automatically. Use the person's local time and confirm ambiguous details before booking.",
        Properties = new()
        {
            ["action"] = new() { Type = "string", Enum = ["list", "create", "move", "delete"] },
            ["id"] = new() { Type = "string", Description = "Booking identifier from list, for move or delete." },
            ["from"] = new() { Type = "string", Description = "First date for list as yyyy-MM-dd, defaults to today. Lists one week." },
            ["start"] = new() { Type = "string", Description = "Local start as yyyy-MM-dd HH:mm for create or move." },
            ["title"] = new() { Type = "string", Description = "A short booking title." },
            ["notes"] = new() { Type = "string", Description = "What the user plans to work on." },
            ["minutes"] = new() { Type = "integer", Minimum = 5, Maximum = 1440, Description = "Booking duration, default 30 minutes." },
            ["repeat"] = new() { Type = "string", Enum = ["none", "daily", "weekdays", "weekly"], Description = "Optional recurrence for a new booking." }
        },
        Required = ["action"]
    };

    private async Task<ToolResult> UseCalendarAsync(string arguments, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        if (!chatOpen || performingGoalTurn)
            return Result("Calendar changes are available from a regular Vrex chat.");
        using var document = JsonDocument.Parse(arguments);
        var args = document.RootElement;
        string? Text(string name) => args.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()?.Trim() : null;
        try
        {
            var diary = calendar.Value;
            string? action = Text("action");
            if (action == "list")
            {
                var from = Date(Text("from"), "yyyy-MM-dd") ?? DateTime.Today;
                var entries = await diary.ListAsync(from, from.AddDays(7));
                return Result(JsonSerializer.Serialize(entries.Where(item => item.PersonaId == VrexProtocol.PersonaId)
                    .Take(25).Select(CalendarEntry)));
            }
            var existing = action is "move" or "delete" && Text("id") is { Length: > 0 } id ? await diary.GetAsync(id) : null;
            if (action is "move" or "delete" && existing?.PersonaId != VrexProtocol.PersonaId)
                return Result("That booking was not found in Vrex’s calendar.");
            token.ThrowIfCancellationRequested();
            if (action == "delete")
            {
                await diary.DeleteAsync(existing!.TaskId);
                emit(new("calendar-changed"));
                return Result("Removed the booking.");
            }
            var start = Date(Text("start"), "yyyy-MM-dd HH:mm") ?? throw new ArgumentException("Specify the booking’s local date and time.");
            Booking saved;
            if (action == "move")
                saved = await diary.UpdateAsync(existing! with { Start = start });
            else if (action == "create")
            {
                int minutes = args.TryGetProperty("minutes", out var duration) && duration.TryGetInt32(out int parsed) ? parsed : 30;
                saved = await CreateBookingAsync(Text("title") ?? "", start, minutes, Text("notes"), Text("repeat"));
            }
            else return Result("Choose list, create, move or delete.");
            emit(new("calendar-changed"));
            return Result(JsonSerializer.Serialize(CalendarEntry(saved)));
        }
        catch (BookingClashException ex) { return Result(BookingClashMessage(ex)); }
        catch (ArgumentException ex) { return Result(ex.Message); }

        static ToolResult Result(string output) => new() { Title = "Vrex calendar", Output = output };
        static DateTime? Date(string? value, string format) => value == null ? null :
            DateTime.TryParseExact(value, format, CultureInfo.InvariantCulture, DateTimeStyles.None, out var parsed) ? parsed :
            throw new ArgumentException("Use the calendar’s local date and time format.");
    }
}
