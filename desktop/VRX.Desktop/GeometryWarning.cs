using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace VRX.Desktop;

// What the player answered to the true-geometry warning.
public enum GeometryConsent { Proceed, UseDepth, Cancel }

// The red true-geometry warning, shown every time geometry is turned on for a game and
// before every geometry launch or restart (Steam launches included): offline games are
// recommended; online games with anti-cheat may flag it and the account can be banned.
// When anti-cheat is found in the game, geometry is refused outright and depth offered.
internal static class GeometryWarning
{
    // A red panel with the warning (also used inline in the main window).
    public static Border Panel(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        return new Border
        {
            Background = (Brush)Application.Current.FindResource("WarningBack"),
            BorderBrush = (Brush)Application.Current.FindResource("WarningEdge"),
            BorderThickness = new Thickness(2), CornerRadius = new CornerRadius(6), Padding = new Thickness(14, 10, 14, 10),
            Child = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, FontWeight = FontWeights.SemiBold, Foreground = (Brush)Application.Current.FindResource("WarningInk") },
        };
    }

    // Asks before true geometry is used for `game`. `antiCheat`: markers found (AntiCheat);
    // any marker refuses geometry and only offers depth.
    public static GeometryConsent Ask(Window? owner, string game, IReadOnlyList<AntiCheat.Marker> antiCheat, bool launch)
    {
        ArgumentNullException.ThrowIfNull(game);
        ArgumentNullException.ThrowIfNull(antiCheat);
        Debug.WriteLine($"[Geometry warning] Ask enter: {game}, launch {launch}, anti-cheat {antiCheat.Count}");
        bool refused = antiCheat.Count > 0;
        var window = new Window
        {
            Title = Loc.Get(refused ? "GeometryAntiCheatTitle" : "GeometryWarningTitle"), Owner = owner, Width = 580, SizeToContent = SizeToContent.Height,
            ResizeMode = ResizeMode.NoResize, WindowStartupLocation = owner != null ? WindowStartupLocation.CenterOwner : WindowStartupLocation.CenterScreen, ShowInTaskbar = owner == null,
            Background = (Brush)Application.Current.FindResource("Page"), Foreground = (Brush)Application.Current.FindResource("Ink"),
        };
        var body = new StackPanel { Margin = new Thickness(24) };
        window.Content = body;
        body.Children.Add(new TextBlock { Text = game, FontSize = 18, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 12) });
        body.Children.Add(Panel(refused ? Loc.Format("GeometryAntiCheatFound", AntiCheat.Products(antiCheat)) : Loc.Get("GeometryWarningText")));
        if (refused)
        {
            body.Children.Add(new TextBlock { Text = string.Join("\n", antiCheat.Take(4).Select(m => m.Where)), FontSize = 12, Foreground = (Brush)Application.Current.FindResource("Muted"),
                TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 10, 0, 0) });
        }
        var answer = GeometryConsent.Cancel;
        var buttons = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 18, 0, 0) };
        Button Add(string text, GeometryConsent value, bool primary = false)
        {
            var button = new Button { Content = text, Padding = new Thickness(14, 8, 14, 8), Margin = new Thickness(8, 0, 0, 0) };
            if (primary && Application.Current.TryFindResource("Primary") is Style style) button.Style = style;
            button.Click += (_, _) => { answer = value; window.DialogResult = value != GeometryConsent.Cancel; };
            buttons.Children.Add(button);
            return button;
        }
        Add(Loc.Get("GeometryCancel"), GeometryConsent.Cancel).IsCancel = true;
        // Depth is the default button: Enter never turns geometry on.
        Add(Loc.Get("GeometryUseDepth"), GeometryConsent.UseDepth, primary: refused).IsDefault = true;
        if (!refused) Add(Loc.Get(launch ? "GeometryAcceptLaunch" : "GeometryAccept"), GeometryConsent.Proceed);
        body.Children.Add(buttons);
        window.ShowDialog();
        Debug.WriteLine($"[Geometry warning] Ask exit: {answer}");
        return answer;
    }
}
