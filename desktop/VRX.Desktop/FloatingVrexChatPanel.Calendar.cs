using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using VRX.VrexBridge;

namespace VRX.Desktop;

internal sealed partial class FloatingVrexChatPanel
{
    private readonly StackPanel calendarPage = new() { Margin = new(12) };
    private readonly StackPanel calendarRows = new();
    private readonly TextBlock calendarWeekLabel = new() { FontSize = 23, FontWeight = FontWeights.SemiBold, Margin = new(4, 10, 4, 8) };
    private DateTime calendarWeek = WeekOf(DateTime.Today);
    private readonly List<VrexBooking> calendarBookings = [];
    private void BuildCalendarPage()
    {
        var controls = new WrapPanel();
        controls.Children.Add(Button("Back to chat", () => ShowPage(chatContent, "Vrex chat")));
        controls.Children.Add(Button("Previous week", () => MoveCalendar(-7)));
        controls.Children.Add(Button("This week", () => { calendarWeek = WeekOf(DateTime.Today); LoadCalendar(); }));
        controls.Children.Add(Button("Next week", () => MoveCalendar(7)));
        calendarPage.Children.Add(controls);
        calendarPage.Children.Add(calendarWeekLabel);
        calendarPage.Children.Add(calendarRows);
    }

    private static DateTime WeekOf(DateTime date) => date.Date.AddDays(-((int)date.DayOfWeek + 6) % 7);

    private void MoveCalendar(int days)
    {
        calendarWeek = calendarWeek.AddDays(days);
        LoadCalendar();
    }

    private void LoadCalendar()
    {
        calendarWeekLabel.Text = $"{calendarWeek:d MMM} – {calendarWeek.AddDays(6):d MMM yyyy}";
        calendarRows.Children.Clear();
        calendarRows.Children.Add(new TextBlock { Text = "Loading bookings…", FontSize = 20, Margin = new(4) });
        client.Send(new("calendar", calendarWeek.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture)));
        dirty = true;
    }

    private bool HandleCalendarEvent(VrexEvent message)
    {
        switch (message.Type)
        {
            case "calendar-start":
                calendarBookings.Clear();
                return true;
            case "calendar-item":
                if (message.Booking is { } booking) calendarBookings.Add(booking);
                return true;
            case "calendar-ready":
                calendarRows.Children.Clear();
                foreach (var group in calendarBookings.Where(booking => booking.Start >= calendarWeek && booking.Start < calendarWeek.AddDays(7))
                    .OrderBy(booking => booking.Start).GroupBy(booking => booking.Start.Date))
                {
                    calendarRows.Children.Add(new TextBlock { Text = group.Key.ToString("dddd d MMM"), FontSize = 21,
                        FontWeight = FontWeights.SemiBold, Margin = new(4, 12, 4, 4) });
                    foreach (var item in group)
                    {
                        string row = $"{item.Start:t} – {item.Start.AddMinutes(item.Minutes):t}  ·  {item.Title}";
                        if (item.Repeat != null) row += $"  ·  {item.Repeat}";
                        calendarRows.Children.Add(new TextBlock { Text = row, FontSize = 20, TextWrapping = TextWrapping.Wrap, Margin = new(4) });
                        if (!string.IsNullOrWhiteSpace(item.Notes) && item.Notes != item.Title)
                            calendarRows.Children.Add(new TextBlock { Text = item.Notes, FontSize = 18, TextWrapping = TextWrapping.Wrap, Margin = new(16, 0, 4, 6) });
                    }
                }
                if (calendarRows.Children.Count == 0)
                    calendarRows.Children.Add(new TextBlock { Text = "No bookings this week.", FontSize = 20, Margin = new(4) });
                return true;
            case "calendar-changed":
                if (ReferenceEquals(transcript.Content, calendarPage)) LoadCalendar();
                return true;
            default:
                return false;
        }
    }
}
