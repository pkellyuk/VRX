using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace VRX.Desktop;

// What the player answered to the true-geometry warning.
public enum GeometryConsent { Proceed, UseDepth, Cancel }

// The red true-geometry warning, shown before every geometry launch or restart (the main
// button's Start and Restart, chooser tiles, Steam launches; never for moving the Depth |
// Geometry switch, which is only a setting): offline games are
// recommended; online games with anti-cheat may flag it and the account can be banned.
// When anti-cheat is found in the game, the anti-cheat warning replaces it: the products
// and files found, and the choice of estimated depth (the default) or "I understand the
// risk", which unlocks only after a visible countdown that restarts every time it opens.
internal static class GeometryWarning
{
    // How long "I understand the risk" stays locked in the anti-cheat warning.
    public const int RiskCountdownSeconds = 5;

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

    // One warning dialog, built but not shown (Ask shows it; the smoke test inspects it and
    // drives its countdown). Answer is Cancel until a button is used.
    internal sealed class Prompt
    {
        public required Window Window { get; init; }
        public required Button CancelButton { get; init; }
        public required Button DepthButton { get; init; }
        // "I understand..." without anti-cheat; null with anti-cheat (RiskButton instead).
        public Button? AcceptButton { get; init; }
        // "I understand the risk": anti-cheat only, locked until the countdown ends.
        public Button? RiskButton { get; init; }
        public required bool AntiCheat { get; init; }
        public GeometryConsent Answer { get; internal set; } = GeometryConsent.Cancel;
        public int SecondsLeft { get; internal set; }
        internal DispatcherTimer? Timer { get; init; }

        // The risk button's text: with the seconds left while locked, plain once unlocked.
        public string RiskLabel => RiskButton?.Content is TextBlock label ? label.Text : "";

        // One second of the countdown (the timer calls it; the smoke test may too).
        public void Tick()
        {
            Debug.WriteLine($"[Geometry warning] Tick enter: {SecondsLeft} s left, risk button {(RiskButton == null ? "none" : RiskButton.IsEnabled ? "enabled" : "locked")}");
            if (RiskButton == null || SecondsLeft <= 0)
            {
                Timer?.Stop();
                return;
            }
            SecondsLeft--;
            ShowRiskState();
            if (SecondsLeft <= 0) Timer?.Stop();
            Debug.WriteLine($"[Geometry warning] Tick exit: {SecondsLeft} s left, enabled {RiskButton.IsEnabled}");
        }

        // Locked with "(n)" while seconds remain; enabled with the plain label at zero.
        internal void ShowRiskState()
        {
            if (RiskButton == null || RiskButton.Content is not TextBlock label) return;
            bool unlocked = SecondsLeft <= 0;
            label.Text = unlocked ? Loc.Get("GeometryRiskAccept") : Loc.Format("GeometryRiskAcceptCountdown", SecondsLeft);
            RiskButton.IsEnabled = unlocked;
        }
    }

    // Asks before true geometry is used for `game`. `antiCheat`: markers found (AntiCheat);
    // any marker shows the anti-cheat warning instead of the generic one.
    public static GeometryConsent Ask(Window? owner, string game, IReadOnlyList<AntiCheat.Marker> antiCheat)
    {
        ArgumentNullException.ThrowIfNull(game);
        ArgumentNullException.ThrowIfNull(antiCheat);
        Debug.WriteLine($"[Geometry warning] Ask enter: {game}, anti-cheat {antiCheat.Count}");
        var prompt = Build(owner, game, antiCheat);
        prompt.Window.ShowDialog();
        Debug.WriteLine($"[Geometry warning] Ask exit: {prompt.Answer}");
        return prompt.Answer;
    }

    // The dialog for Ask. With anti-cheat: its title, the red explanation, the files found,
    // and Cancel / Use estimated depth (default) / I understand the risk, the last locked for
    // `countdownSeconds` counted down every `tick` (one second) from when the dialog is first
    // drawn. Without: the red warning and Cancel / Use estimated depth (default) / I understand.
    internal static Prompt Build(Window? owner, string game, IReadOnlyList<AntiCheat.Marker> antiCheat,
        int countdownSeconds = RiskCountdownSeconds, TimeSpan? tick = null)
    {
        ArgumentNullException.ThrowIfNull(game);
        ArgumentNullException.ThrowIfNull(antiCheat);
        ArgumentOutOfRangeException.ThrowIfNegative(countdownSeconds);
        Debug.WriteLine($"[Geometry warning] Build enter: {game}, anti-cheat {antiCheat.Count}, countdown {countdownSeconds} s, tick {tick?.TotalMilliseconds ?? 1000} ms");
        bool protectedGame = antiCheat.Count > 0;
        var window = new Window
        {
            Title = Loc.Get(protectedGame ? "GeometryAntiCheatTitle" : "GeometryWarningTitle"), Owner = owner, Width = 580, SizeToContent = SizeToContent.Height,
            ResizeMode = ResizeMode.NoResize, WindowStartupLocation = owner != null ? WindowStartupLocation.CenterOwner : WindowStartupLocation.CenterScreen, ShowInTaskbar = owner == null,
            Background = (Brush)Application.Current.FindResource("Page"), Foreground = (Brush)Application.Current.FindResource("Ink"),
        };
        var body = new StackPanel { Margin = new Thickness(24) };
        window.Content = body;
        body.Children.Add(new TextBlock { Text = game, FontSize = 18, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 12) });
        body.Children.Add(Panel(protectedGame ? Loc.Format("GeometryAntiCheatFound", AntiCheat.Products(antiCheat)) : Loc.Get("GeometryWarningText")));
        if (protectedGame)
        {
            body.Children.Add(new TextBlock { Text = string.Join("\n", antiCheat.Take(4).Select(m => m.Where)), FontSize = 12, Foreground = (Brush)Application.Current.FindResource("Muted"),
                TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 10, 0, 0) });
        }
        var buttons = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 18, 0, 0) };
        Prompt? prompt = null;
        Button Add(object content, GeometryConsent value, bool primary = false)
        {
            var button = new Button { Content = content, Padding = new Thickness(14, 8, 14, 8), Margin = new Thickness(8, 0, 0, 0) };
            if (primary && Application.Current.TryFindResource("Primary") is Style style) button.Style = style;
            button.Click += (_, _) =>
            {
                // A locked button never answers, however the click arrived.
                if (prompt == null || !button.IsEnabled)
                {
                    Debug.WriteLine($"[Geometry warning] {value} ignored: the button is locked");
                    return;
                }
                prompt.Answer = value;
                Debug.WriteLine($"[Geometry warning] answered {value}");
                window.Close();
            };
            buttons.Children.Add(button);
            return button;
        }
        var cancel = Add(Loc.Get("GeometryCancel"), GeometryConsent.Cancel);
        cancel.IsCancel = true;
        // Depth is the default button: Enter never turns geometry on.
        var depth = Add(Loc.Get("GeometryUseDepth"), GeometryConsent.UseDepth, primary: protectedGame);
        depth.IsDefault = true;
        Button? accept = null, risk = null;
        DispatcherTimer? timer = null;
        if (protectedGame)
        {
            // A TextBlock, not a string: a translation's "_" never becomes an access key.
            risk = Add(new TextBlock(), GeometryConsent.Proceed);
            risk.IsEnabled = false;
            timer = new DispatcherTimer(DispatcherPriority.Normal, window.Dispatcher) { Interval = tick ?? TimeSpan.FromSeconds(1) };
        }
        else
        {
            accept = Add(Loc.Get("GeometryAcceptLaunch"), GeometryConsent.Proceed);
        }
        body.Children.Add(buttons);
        prompt = new Prompt { Window = window, CancelButton = cancel, DepthButton = depth, AcceptButton = accept, RiskButton = risk, AntiCheat = protectedGame, Timer = timer, SecondsLeft = countdownSeconds };
        prompt.ShowRiskState();
        if (timer != null)
        {
            timer.Tick += (_, _) => prompt.Tick();
            // The countdown starts when the dialog is first drawn, and ends with it.
            window.ContentRendered += (_, _) =>
            {
                Debug.WriteLine($"[Geometry warning] countdown starts: {prompt.SecondsLeft} s");
                if (prompt.SecondsLeft > 0) timer.Start();
            };
            window.Closed += (_, _) => timer.Stop();
        }
        Debug.WriteLine($"[Geometry warning] Build exit: anti-cheat {protectedGame}, risk '{prompt.RiskLabel}' enabled {risk?.IsEnabled}");
        return prompt;
    }
}
