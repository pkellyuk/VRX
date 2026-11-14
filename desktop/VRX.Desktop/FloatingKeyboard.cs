using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;

namespace VRX.Desktop;

internal sealed class FloatingKeyboard : IFloatingPanel
{
    private readonly SwipePredictor predictor;
    private readonly Func<KeyboardEdit, bool> emit;
    private readonly UserControl surface;
    private readonly Canvas keys = new() { Width = 864, Height = 288 };
    private readonly TextBlock target = new() { Foreground = Brushes.LightCyan, FontSize = 17, Height = 28, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly Dictionary<Button, Action> actions = new();
    private readonly List<(Rect Box, char Letter)> letters = new();
    private readonly List<Point> trace = new();
    private readonly Button[] candidates = new Button[3];
    private readonly Polyline ink = new() { Stroke = Brushes.LightCyan, StrokeThickness = 4, IsHitTestVisible = false };
    private readonly RenderTargetBitmap bitmap = new(ControllerMenu.PixelWidth, ControllerMenu.PixelHeight, 192, 192, PixelFormats.Pbgra32);
    private Button? heldButton;
    private char heldLetter;
    private bool shift, symbols;
    private bool addWordCandidate;
    private string prefix = "", committed = "";
    private string[] suggestions = [];
    public bool Closed { get; private set; }
    public bool Dirty { get; private set; } = true;
    internal FloatingKeyboard(SwipePredictor predictor, Func<KeyboardEdit, bool> emit)
    {
        this.predictor = predictor; this.emit = emit;
        var root = new DockPanel { Margin = new Thickness(48) };
        var title = new Border { Height = 56, CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Color.FromRgb(37, 50, 72)),
            Child = new TextBlock { Text = Loc.Get("KeyboardTitle"), Foreground = Brushes.White, FontSize = 25, Margin = new Thickness(12, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center } };
        DockPanel.SetDock(title, Dock.Top); root.Children.Add(title);
        var top = new StackPanel { Margin = new Thickness(0, 8, 0, 0) };
        top.Children.Add(target);
        var choices = new StackPanel { Orientation = Orientation.Horizontal, Height = 52 };
        for (int i = 0; i < candidates.Length; i++)
        {
            int choice = i;
            candidates[i] = Button("", 280, () =>
            {
                if (choice >= suggestions.Length) return;
                if (addWordCandidate) { predictor.Learn(suggestions[choice]); addWordCandidate = false; }
                Commit(suggestions[choice]);
            });
            choices.Children.Add(candidates[i]);
        }
        top.Children.Add(choices); top.Children.Add(keys);
        var controls = new WrapPanel { Height = 60 };
        controls.Children.Add(Button(Loc.Get("KeyboardShift"), 72, () => { shift = !shift; DrawKeys(); }));
        controls.Children.Add(Button("123/#+=", 88, () => { symbols = !symbols; DrawKeys(); }));
        controls.Children.Add(Button(Loc.Get("KeyboardSpace"), 220, () =>
        {
            if (committed.Length > 0) { ClearWord(); return; }
            if (Send(new KeyboardEdit(" "))) { if (predictor.IsKnown(prefix)) predictor.Learn(prefix); ClearWord(); }
        }));
        controls.Children.Add(Button("⌫", 90, Backspace));
        controls.Children.Add(Button(Loc.Get("KeyboardEnter"), 100, () => { if (Send(new KeyboardEdit("", 13))) ClearWord(); }));
        controls.Children.Add(Button("Tab", 64, () => { if (Send(new KeyboardEdit("", 9))) ClearWord(); }));
        controls.Children.Add(Button("←", 64, () => { if (Send(new KeyboardEdit("", 37))) ClearWord(); }));
        controls.Children.Add(Button("→", 64, () => { if (Send(new KeyboardEdit("", 39))) ClearWord(); }));
        top.Children.Add(controls);
        top.Children.Add(new TextBlock { Text = Loc.Get("KeyboardHint"), Foreground = Brushes.LightGray, FontSize = 16, Margin = new Thickness(0, 6, 0, 6), TextWrapping = TextWrapping.Wrap });
        top.Children.Add(Button(Loc.Get("SnapshotClose"), 130, () => Closed = true));
        root.Children.Add(top);
        surface = new UserControl { Width = ControllerMenu.Width, Height = ControllerMenu.Height, FontFamily = new FontFamily("Segoe UI"), Content = FloatingPanelChrome.Wrap(root) };
        DrawKeys(); SetTarget(Loc.Get("KeyboardChooseTarget"));
    }
    private Button Button(string text, double width, Action action)
    {
        var button = new Button { Content = text, Width = width, Height = 48, FontSize = 21, Margin = new Thickness(0, 0, 6, 4) };
        actions[button] = () => { action(); Dirty = true; }; return button;
    }
    internal void SetTarget(string label) { target.Text = label; ClearWord(); Dirty = true; }
    private void DrawKeys()
    {
        letters.Clear(); keys.Children.Clear();
        string[] rows = [symbols ? ":/-_?=&@.+" : shift ? "!@#$%^&*()" : "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"];
        for (int row = 0; row < rows.Length; row++) for (int col = 0; col < rows[row].Length; col++)
        {
            double inset = row < 2 ? 0 : row == 2 ? .5 : 1.5;
            var box = new Rect((col + inset) * 86.4 + 3, row * 72 + 3, 80.4, 64);
            char letter = rows[row][col];
            var key = new Border { Width = box.Width, Height = box.Height, CornerRadius = new CornerRadius(7),
                Background = new SolidColorBrush(Color.FromRgb(40, 56, 78)), BorderBrush = new SolidColorBrush(Color.FromRgb(68, 94, 120)), BorderThickness = new Thickness(1),
                Child = new TextBlock { Text = (shift && char.IsLetter(letter) ? char.ToUpperInvariant(letter) : letter).ToString(), FontSize = 29,
                    Foreground = Brushes.White, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } };
            Canvas.SetLeft(key, box.X); Canvas.SetTop(key, box.Y); keys.Children.Add(key); letters.Add((box, letter));
        }
        keys.Children.Add(ink); Dirty = true;
    }
    private bool Send(KeyboardEdit edit)
    {
        bool sent = emit(edit);
        if (!sent) target.Text = Loc.Get("KeyboardChooseTarget");
        Dirty = true; return sent;
    }
    private void ClearWord() { prefix = committed = ""; suggestions = []; addWordCandidate = false; ShowSuggestions(); }
    private void CompletePrefix()
    {
        suggestions = prefix.Length > 0 ? predictor.Prefix(prefix) : []; addWordCandidate = false;
        if (suggestions.Length == 0 && prefix.Length >= 2 && !predictor.IsKnown(prefix))
        { suggestions = [prefix]; addWordCandidate = true; }
        ShowSuggestions();
    }
    private void ShowSuggestions()
    {
        for (int i = 0; i < candidates.Length; i++)
        { candidates[i].Content = i < suggestions.Length ? addWordCandidate ? Loc.Format("KeyboardAddWord", suggestions[i]) : suggestions[i] : ""; candidates[i].IsEnabled = i < suggestions.Length; }
        Dirty = true;
    }
    private void Commit(string word)
    {
        bool capital = shift || (committed.Length > 0 && char.IsUpper(committed[0]));
        string result = capital ? char.ToUpperInvariant(word[0]) + word[1..] : word;
        int remove = committed.Length > 0 ? committed.Length + 1 : prefix.Length;
        if (!Send(new KeyboardEdit(result + " ", Backspaces: remove))) return;
        predictor.Learn(word); committed = result; prefix = ""; shift = false; DrawKeys(); ShowSuggestions();
    }
    private void Backspace()
    {
        int remove = committed.Length > 0 ? committed.Length + 1 : 1;
        if (!Send(new KeyboardEdit("", Backspaces: remove))) return;
        committed = ""; if (prefix.Length > 0) prefix = prefix[..^1];
        CompletePrefix();
    }
    private Point Normalized(Point point) => new(point.X / 86.4 - .5, (point.Y - 108) / 86.4);
    public void Pointer(int kind, float x, float y, float value = 0)
    {
        if (kind == 4) { trace.Clear(); ink.Points.Clear(); heldButton = null; heldLetter = '\0'; Dirty = true; return; }
        var point = new Point(x * ControllerMenu.Width, y * ControllerMenu.Height);
        var button = actions.Keys.FirstOrDefault(b => b.IsEnabled && new Rect(b.TranslatePoint(new Point(), surface), b.RenderSize).Contains(point));
        var local = surface.TranslatePoint(point, keys);
        var letter = letters.FirstOrDefault(k => k.Box.Contains(local)).Letter;
        if (kind == 1)
        {
            heldButton = button; heldLetter = letter; trace.Clear(); ink.Points.Clear();
            if (letter != '\0') committed = ""; // A new gesture starts a new word; candidate clicks replace the previous one.
            if (letter != '\0') { trace.Add(Normalized(local)); ink.Points.Add(local); }
        }
        if (kind == 0 && heldLetter != '\0')
        {
            var next = Normalized(local);
            if (trace.Count == 0 || (trace[^1] - next).Length > .04) { trace.Add(next); ink.Points.Add(local); }
            Dirty = true;
        }
        if (kind != 2) return;
        var pressed = heldButton; heldButton = null;
        if (pressed != null && pressed == button) actions[pressed]();
        if (heldLetter != '\0')
        {
            double length = 0;
            for (int i = 1; i < trace.Count; i++) length += (trace[i] - trace[i - 1]).Length;
            if (char.IsLetter(heldLetter) && length > .8 && trace.Count > 2)
            {
                suggestions = predictor.Predict(trace);
                addWordCandidate = false;
                if (suggestions.Length > 0) Commit(suggestions[0]);
                ShowSuggestions();
            }
            else
            {
                char typed = shift ? char.ToUpperInvariant(heldLetter) : heldLetter;
                if (Send(new KeyboardEdit(typed.ToString())))
                {
                    committed = ""; prefix = char.IsLetter(typed) ? prefix + char.ToLowerInvariant(typed) : "";
                    shift = false; DrawKeys(); CompletePrefix();
                }
            }
        }
        trace.Clear(); ink.Points.Clear(); heldLetter = '\0'; Dirty = true;
    }
    public byte[] Render()
    {
        surface.Measure(new Size(ControllerMenu.Width, ControllerMenu.Height)); surface.Arrange(new Rect(0, 0, ControllerMenu.Width, ControllerMenu.Height)); surface.UpdateLayout();
        bitmap.Clear(); bitmap.Render(surface); var pixels = new byte[ControllerMenu.PixelBytes]; bitmap.CopyPixels(pixels, ControllerMenu.PixelWidth * 4, 0); return pixels;
    }
    public void Published() => Dirty = false;
    public void Dispose() { trace.Clear(); heldButton = null; }
    internal static void SmokeTest(string output)
    {
        SwipePredictor.SmokeTest();
        var edits = new List<KeyboardEdit>();
        var keyboard = new FloatingKeyboard(new SwipePredictor(), edit => { edits.Add(edit); return true; }); keyboard.Render();
        void Point(int kind, Point key)
        {
            var p = keyboard.keys.TranslatePoint(new Point((key.X + .5) * 86.4, key.Y * 86.4 + 108), keyboard.surface);
            keyboard.Pointer(kind, (float)(p.X / ControllerMenu.Width), (float)(p.Y / ControllerMenu.Height));
        }
        var shape = SwipePredictor.Shape("hello"); Point(1, shape[0]); foreach (var p in shape) Point(0, p); Point(2, shape[^1]);
        if (!edits.Any(e => e.Text == "hello ")) throw new Exception("Floating keyboard swipe did not commit hello");
        shape = SwipePredictor.Shape("world"); Point(1, shape[0]); foreach (var p in shape) Point(0, p); Point(2, shape[^1]);
        if (edits[^1].Text != "world " || edits[^1].Backspaces != 0) throw new Exception("A second swipe must append a new word");
        Point(1, SwipePredictor.KeyPoint('a')); Point(2, SwipePredictor.KeyPoint('a'));
        if (edits[^1].Text != "a") throw new Exception("Floating keyboard tap did not type a letter");
        keyboard.Render(); var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(keyboard.bitmap));
        using var file = System.IO.File.Create(System.IO.Path.Combine(output, "floating-keyboard.png")); encoder.Save(file);
    }
}
