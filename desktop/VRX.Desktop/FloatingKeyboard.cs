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
    private readonly Canvas keys = new() { Width = 864, Height = 232 };
    private readonly TextBlock target = new() { Foreground = Brushes.LightCyan, FontSize = 17, Height = 28, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly Dictionary<Button, Action> actions = new();
    private readonly List<(Rect Box, char Letter)> letters = new();
    private readonly List<Point> trace = new();
    private readonly Button[] candidates = new Button[3];
    private readonly Polyline ink = new() { Stroke = Brushes.LightCyan, StrokeThickness = 4, IsHitTestVisible = false };
    private readonly RenderTargetBitmap bitmap = new(ControllerMenu.PixelWidth, ControllerMenu.PixelHeight, 192, 192, PixelFormats.Pbgra32);
    private readonly KeyboardDictation dictation = new();
    private readonly Button voiceButton;
    private readonly Button microphoneButton;
    private readonly TextBlock voiceStatus = new() { FontSize = 14, Height = 24, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly Border microphonePopup = new() { Visibility = Visibility.Collapsed, Width = 760, Padding = new Thickness(12), CornerRadius = new CornerRadius(16), BorderThickness = new Thickness(2), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(48, 142, 0, 0) };
    private readonly StackPanel microphoneChoices = new();
    private readonly ScrollViewer microphoneScroll = new() { MaxHeight = 250, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    private readonly HashSet<Button> microphoneButtons = new();
    private KeyboardDictation.Microphone selectedMicrophone = new(-1, Loc.Get("DictationDefaultMic"));
    private bool changingVoice;
    private int voiceGeneration;
    private Button? heldButton;
    private char heldLetter;
    private bool shift, symbols;
    private KeyboardModifiers modifiers;
    private bool addWordCandidate;
    private string prefix = "", committed = "";
    private string[] suggestions = [];
    public bool Closed { get; private set; }
    private int appearanceRevision = -1;
    private bool dirty = true;
    public bool Dirty { get => dirty || appearanceRevision != Appearance.Revision; private set => dirty = value; }
    internal FloatingKeyboard(SwipePredictor predictor, Func<KeyboardEdit, bool> emit)
    {
        this.predictor = predictor; this.emit = emit;
        var root = new DockPanel { Margin = new Thickness(48) };
        var title = new Border { Height = 56, CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Color.FromRgb(37, 50, 72)),
            Child = FloatingPanelChrome.Title(new TextBlock { Text = Loc.Get("KeyboardTitle"), Foreground = Brushes.White, FontSize = 25, Margin = new Thickness(12, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center }, Button(Loc.Get("SnapshotClose"), 80, () => { StopVoice(); Closed = true; })) };
        DockPanel.SetDock(title, Dock.Top); root.Children.Add(title);
        var top = new StackPanel { Margin = new Thickness(0, 8, 0, 0) };
        top.Children.Add(target);
        var voiceBar = new StackPanel { Orientation = Orientation.Horizontal, Height = 44 };
        voiceButton = Button(Loc.Get("DictationStart"), 180, () => _ = ToggleVoice()); voiceButton.Height = 40;
        microphoneButton = Button(Loc.Format("DictationMicrophone", selectedMicrophone.Name), 670, OpenMicrophones); microphoneButton.Height = 40;
        voiceBar.Children.Add(voiceButton); voiceBar.Children.Add(microphoneButton);
        top.Children.Add(voiceBar); top.Children.Add(voiceStatus);
        voiceStatus.Text = Loc.Get("DictationOff");
        var choices = new StackPanel { Orientation = Orientation.Horizontal, Height = 44 };
        for (int i = 0; i < candidates.Length; i++)
        {
            int choice = i;
            candidates[i] = Button("", 280, () =>
            {
                if (choice >= suggestions.Length) return;
                if (addWordCandidate) { predictor.Learn(suggestions[choice]); addWordCandidate = false; }
                Commit(suggestions[choice]);
            });
            candidates[i].Height = 40; choices.Children.Add(candidates[i]);
        }
        top.Children.Add(choices); top.Children.Add(keys);
        var controls = new WrapPanel { Height = 50 };
        controls.Children.Add(Button(Loc.Get("KeyboardShift"), 72, () => { shift = !shift; DrawKeys(); }));
        controls.Children.Add(Button("123/#+=", 88, () => { symbols = !symbols; DrawKeys(); }));
        controls.Children.Add(Button(Loc.Get("KeyboardSpace"), 410, () =>
        {
            if (committed.Length > 0) { ClearWord(); return; }
            if (Send(new KeyboardEdit(" "))) { if (predictor.IsKnown(prefix)) predictor.Learn(prefix); ClearWord(); }
        }));
        controls.Children.Add(Button("⌫", 130, Backspace));

        top.Children.Add(controls);
        var special = new WrapPanel { Height = 232, Visibility = Visibility.Collapsed };
        var actionRow = new DockPanel { Height = 56 };
        var enter = Button(Loc.Get("KeyboardEnter"), 400, () => { if (Send(new KeyboardEdit("", 13))) ClearWord(); });
        enter.Height = 52; DockPanel.SetDock(enter, Dock.Right); actionRow.Children.Add(enter);
        var leftActions = new StackPanel { Orientation = Orientation.Horizontal };
        leftActions.Children.Add(Button("Tab", 80, () => { if (Send(new KeyboardEdit("", 9))) ClearWord(); }));
        leftActions.Children.Add(Button("PC keys", 110, () => { bool pc = special.Visibility != Visibility.Visible; special.Visibility = pc ? Visibility.Visible : Visibility.Collapsed; keys.Visibility = pc ? Visibility.Collapsed : Visibility.Visible; ClearWord(); }));
        foreach (var (label, flag) in new[] { ("Ctrl", KeyboardModifiers.Control), ("Alt", KeyboardModifiers.Alt), ("Win", KeyboardModifiers.Windows) })
        {
            Button? toggle = null;
            toggle = Button(label, 78, () => { modifiers ^= flag; if (modifiers.HasFlag(flag)) toggle!.Background = Brushes.DarkCyan; else toggle!.ClearValue(Control.BackgroundProperty); });
            special.Children.Add(toggle);
        }
        foreach (var (label, key) in new[] { ("Esc",27), ("Del",46), ("Home",36), ("End",35), ("PgUp",33), ("PgDn",34), ("←",37), ("↑",38), ("↓",40), ("→",39) })
            special.Children.Add(Button(label, 70, () => { Send(new KeyboardEdit("", key)); ClearWord(); }));
        for (int i = 0; i < 12; i++)
        { int code = 112+i; special.Children.Add(Button($"F{i+1}", 64, () => { Send(new KeyboardEdit("", code)); ClearWord(); })); }
        foreach (var (label, key) in new[] { ("Copy",67), ("Paste",86), ("Cut",88), ("Select all",65), ("Undo",90), ("Redo",89) })
            special.Children.Add(Button(label, 120, () => { Send(new KeyboardEdit("", key, Modifiers: KeyboardModifiers.Control)); ClearWord(); }));
        top.Children.Add(special);
        actionRow.Children.Add(leftActions); top.Children.Add(actionRow);
        top.Children.Add(new TextBlock { Text = Loc.Get("KeyboardHint"), Foreground = Brushes.LightGray, FontSize = 16, Margin = new Thickness(0, 6, 0, 6), TextWrapping = TextWrapping.Wrap });
        root.Children.Add(top);
        var chrome = FloatingPanelChrome.Wrap(root);
        microphonePopup.SetResourceReference(Border.BackgroundProperty, "Card"); microphonePopup.SetResourceReference(Border.BorderBrushProperty, "Accent");
        microphoneScroll.Content = microphoneChoices; microphonePopup.Child = microphoneScroll; chrome.Children.Add(microphonePopup);
        surface = new UserControl { Width = ControllerMenu.Width, Height = ControllerMenu.Height, FontFamily = new FontFamily("Segoe UI"), Content = chrome };
        dictation.Recognized += text =>
        {
            int epoch = System.Threading.Volatile.Read(ref voiceGeneration);
            surface.Dispatcher.BeginInvoke(() =>
            {
                if (Closed || epoch != voiceGeneration || !dictation.Listening || changingVoice) return;
                modifiers = KeyboardModifiers.None; shift = false;
                if (Send(new KeyboardEdit(text + " "))) { ClearWord(); voiceStatus.Text = Loc.Format("DictationHeard", text); }
                else { StopVoice(); voiceStatus.Text = Loc.Get("KeyboardChooseTarget"); }
                Dirty = true;
            });
        };
        dictation.Hypothesized += text =>
        {
            int epoch = System.Threading.Volatile.Read(ref voiceGeneration);
            surface.Dispatcher.BeginInvoke(() => { if (!Closed && epoch == voiceGeneration && dictation.Listening && !changingVoice) { voiceStatus.Text = Loc.Format("DictationHearing", text); Dirty = true; } });
        };
        dictation.Failed += message =>
        {
            int epoch = System.Threading.Volatile.Read(ref voiceGeneration);
            surface.Dispatcher.BeginInvoke(() => { if (!Closed && epoch == voiceGeneration && dictation.Listening) { StopVoice(); voiceStatus.Text = Loc.Format("DictationError", message); Dirty = true; } });
        };
        DrawKeys(); SetTarget(Loc.Get("KeyboardChooseTarget"));
    }
    private Button Button(string text, double width, Action action)
    {
        var button = new Button { Content = text, Width = width, Height = 48, FontSize = 18, Padding = new Thickness(4), Margin = new Thickness(0, 0, 6, 4) };
        actions[button] = () => { action(); Dirty = true; }; return button;
    }
    internal void SetTarget(string label) { if (target.Text != label) StopVoice(); target.Text = label; ClearWord(); Dirty = true; }
    private async Task ToggleVoice()
    {
        if (changingVoice) return;
        if (dictation.Listening) { StopVoice(); return; }
        changingVoice = true; int epoch = ++voiceGeneration;
        voiceButton.IsEnabled = microphoneButton.IsEnabled = false;
        voiceStatus.Text = Loc.Get("DictationStarting"); Dirty = true;
        try
        {
            string language = await Task.Run(() => dictation.Start(selectedMicrophone));
            if (Closed || epoch != voiceGeneration) { dictation.Stop(); return; }
            voiceButton.Content = Loc.Get("DictationStop"); voiceStatus.Text = Loc.Format("DictationListening", language);
        }
        catch (Exception ex) when (ex is InvalidOperationException or System.Runtime.InteropServices.COMException or NAudio.MmException or UnauthorizedAccessException or ArgumentException or PlatformNotSupportedException)
        { voiceStatus.Text = Loc.Format("DictationError", ex.Message); }
        finally { changingVoice = false; voiceButton.IsEnabled = microphoneButton.IsEnabled = true; Dirty = true; }
    }
    internal void StopVoice()
    {
        voiceGeneration++; dictation.Stop();
        voiceButton.Content = Loc.Get("DictationStart"); voiceStatus.Text = Loc.Get("DictationOff"); Dirty = true;
    }
    private void OpenMicrophones()
    {
        if (microphonePopup.Visibility == Visibility.Visible) { microphonePopup.Visibility = Visibility.Collapsed; Dirty = true; return; }
        foreach (var button in microphoneButtons) actions.Remove(button);
        microphoneButtons.Clear(); microphoneChoices.Children.Clear();
        try
        {
            foreach (var microphone in KeyboardDictation.Microphones())
            {
                var pick = Button(microphone.Name, 710, () => _ = SelectMicrophone(microphone)); pick.Height = 44;
                microphoneButtons.Add(pick); microphoneChoices.Children.Add(pick);
            }
            microphonePopup.Visibility = Visibility.Visible;
        }
        catch (Exception ex) when (ex is NAudio.MmException or InvalidOperationException) { voiceStatus.Text = Loc.Format("DictationError", ex.Message); }
        Dirty = true;
    }
    private async Task SelectMicrophone(KeyboardDictation.Microphone microphone)
    {
        bool resume = dictation.Listening; StopVoice();
        selectedMicrophone = microphone; microphonePopup.Visibility = Visibility.Collapsed;
        microphoneButton.Content = Loc.Format("DictationMicrophone", microphone.Name); Dirty = true;
        if (resume) await ToggleVoice();
    }
    private void DrawKeys()
    {
        letters.Clear(); keys.Children.Clear();
        string[] rows = symbols
            ? shift ? ["~`|•√π÷×¶∆", "€£¥¢^°{}[]", "\\%©®™✓<>;", "_…!?\"'="] : ["1234567890", "@#$%&*()-+", "=/:;!?.,_", "[]{}<>\\"]
            : [shift ? "!@#$%^&*()" : "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"];
        for (int row = 0; row < rows.Length; row++) for (int col = 0; col < rows[row].Length; col++)
        {
            double inset = row < 2 ? 0 : row == 2 ? .5 : 1.5;
            var box = new Rect((col + inset) * 86.4 + 3, row * 58 + 3, 80.4, 51);
            char letter = rows[row][col];
            var key = new Border { Tag = letter, Width = box.Width, Height = box.Height, CornerRadius = new CornerRadius(7),
                Background = new SolidColorBrush(Color.FromRgb(40, 56, 78)), BorderBrush = new SolidColorBrush(Color.FromRgb(68, 94, 120)), BorderThickness = new Thickness(1),
                Child = new TextBlock { Text = (shift && char.IsLetter(letter) ? char.ToUpperInvariant(letter) : letter).ToString(), FontSize = 29,
                    Foreground = Brushes.White, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } };
            Canvas.SetLeft(key, box.X); Canvas.SetTop(key, box.Y); keys.Children.Add(key); letters.Add((box, letter));
        }
        keys.Children.Add(ink); Dirty = true;
    }
    private bool Send(KeyboardEdit edit)
    {
        var flags = modifiers | edit.Modifiers | (shift && (edit.Key != 0 || modifiers != KeyboardModifiers.None) ? KeyboardModifiers.Shift : KeyboardModifiers.None);
        if (flags != KeyboardModifiers.None && edit.Text.Length == 1 && char.IsAsciiLetterOrDigit(edit.Text[0]))
            edit = edit with { Key = char.ToUpperInvariant(edit.Text[0]), Text = "" };
        edit = edit with { Modifiers = flags };
        bool sent = emit(edit);
        modifiers = KeyboardModifiers.None;
        if (edit.Key != 0 && shift) { shift = false; DrawKeys(); }
        foreach (var button in actions.Keys.Where(b => b.Content is string s && s is "Ctrl" or "Alt" or "Win")) button.ClearValue(Control.BackgroundProperty);
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
        int remove = modifiers == KeyboardModifiers.None && committed.Length > 0 ? committed.Length + 1 : 1;
        if (!Send(new KeyboardEdit("", Backspaces: remove))) return;
        committed = ""; if (prefix.Length > 0) prefix = prefix[..^1];
        CompletePrefix();
    }
    private Point Normalized(Point point) => new(point.X / 86.4 - .5, (point.Y - 86.5) / 69.6);
    public void Pointer(int kind, float x, float y, float value = 0)
    {
        if (kind == 4)
        {
            PanelFeedback.Cancel(heldButton, () => Dirty = true);
            PanelFeedback.Cancel(keys.Children.OfType<Border>().FirstOrDefault(b => b.Tag is char c && c == heldLetter), () => Dirty = true);
            trace.Clear(); ink.Points.Clear(); heldButton = null; heldLetter = '\0'; Dirty = true; return;
        }
        var point = new Point(x * ControllerMenu.Width, y * ControllerMenu.Height);
        bool Drawn(Button b) { DependencyObject? p = b; while (p != null) { if (p is UIElement e && e.Visibility != Visibility.Visible) return false; p = VisualTreeHelper.GetParent(p); } return true; }
        if (kind == 3 && microphonePopup.Visibility == Visibility.Visible) { microphoneScroll.ScrollToVerticalOffset(microphoneScroll.VerticalOffset - value); Dirty = true; return; }
        var possibleButtons = microphonePopup.Visibility == Visibility.Visible ? microphoneButtons.Prepend(microphoneButton) : actions.Keys.Where(b => !microphoneButtons.Contains(b));
        var button = possibleButtons.FirstOrDefault(b => b.IsEnabled && Drawn(b) && new Rect(b.TranslatePoint(new Point(), surface), b.RenderSize).Contains(point));
        var local = surface.TranslatePoint(point, keys);
        var letter = microphonePopup.Visibility != Visibility.Visible && keys.Visibility == Visibility.Visible ? letters.FirstOrDefault(k => k.Box.Contains(local)).Letter : '\0';
        if (kind == 1)
        {
            heldButton = button; PanelFeedback.Down(heldButton, () => Dirty = true); heldLetter = letter; trace.Clear(); ink.Points.Clear();
            if (letter != '\0') committed = ""; // A new gesture starts a new word; candidate clicks replace the previous one.
            if (letter != '\0') { PanelFeedback.Down(keys.Children.OfType<Border>().FirstOrDefault(b => b.Tag is char c && c == letter), () => Dirty = true); trace.Add(Normalized(local)); ink.Points.Add(local); }
        }
        if (kind == 0 && heldLetter != '\0')
        {
            var next = Normalized(local);
            if (trace.Count == 0 || (trace[^1] - next).Length > .04) { trace.Add(next); ink.Points.Add(local); }
            Dirty = true;
        }
        if (kind != 2) return;
        var pressed = heldButton; heldButton = null;
        if (pressed != null && pressed == button) { PanelFeedback.Up(pressed, () => Dirty = true); actions[pressed](); } else PanelFeedback.Cancel(pressed, () => Dirty = true);
        if (heldLetter != '\0')
        {
            double length = 0;
            for (int i = 1; i < trace.Count; i++) length += (trace[i] - trace[i - 1]).Length;
            if (modifiers == KeyboardModifiers.None && char.IsLetter(heldLetter) && length > .8 && trace.Count > 2)
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
        if (heldLetter != '\0') PanelFeedback.Up(keys.Children.OfType<Border>().FirstOrDefault(b => b.Tag is char c && c == heldLetter), () => Dirty = true);
        trace.Clear(); ink.Points.Clear(); heldLetter = '\0'; Dirty = true;
    }
    public byte[] Render()
    {
        surface.Measure(new Size(ControllerMenu.Width, ControllerMenu.Height)); surface.Arrange(new Rect(0, 0, ControllerMenu.Width, ControllerMenu.Height)); surface.UpdateLayout();
        bitmap.Clear(); bitmap.Render(surface); var pixels = new byte[ControllerMenu.PixelBytes]; bitmap.CopyPixels(pixels, ControllerMenu.PixelWidth * 4, 0); return pixels;
    }
    public void Published() { Dirty = false; appearanceRevision = Appearance.Revision; }
    public void Dispose() { Closed = true; voiceGeneration++; dictation.Dispose(); trace.Clear(); heldButton = null; }
    internal static void SmokeTest(string output)
    {
        KeyboardDictation.SmokeTest(output);
        SwipePredictor.SmokeTest();
        var edits = new List<KeyboardEdit>();
        var keyboard = new FloatingKeyboard(new SwipePredictor(), edit => { edits.Add(edit); return true; }); keyboard.Render();
        void Point(int kind, Point key)
        {
            var p = keyboard.keys.TranslatePoint(new Point((key.X + .5) * 86.4, key.Y * 69.6 + 86.5), keyboard.surface);
            keyboard.Pointer(kind, (float)(p.X / ControllerMenu.Width), (float)(p.Y / ControllerMenu.Height));
        }
        var shape = SwipePredictor.Shape("hello"); Point(1, shape[0]); foreach (var p in shape) Point(0, p); Point(2, shape[^1]);
        if (!edits.Any(e => e.Text == "hello ")) throw new Exception("Floating keyboard swipe did not commit hello");
        shape = SwipePredictor.Shape("world"); Point(1, shape[0]); foreach (var p in shape) Point(0, p); Point(2, shape[^1]);
        if (edits[^1].Text != "world " || edits[^1].Backspaces != 0) throw new Exception("A second swipe must append a new word");
        Point(1, SwipePredictor.KeyPoint('a')); Point(2, SwipePredictor.KeyPoint('a'));
        if (edits[^1].Text != "a") throw new Exception("Floating keyboard tap did not type a letter");
        void Click(string label)
        {
            keyboard.Render(); var button = keyboard.actions.Keys.Single(b => b.Content?.ToString() == label);
            var p = button.TranslatePoint(new Point(button.ActualWidth/2,button.ActualHeight/2),keyboard.surface);
            keyboard.Pointer(1,(float)(p.X/ControllerMenu.Width),(float)(p.Y/ControllerMenu.Height));
            keyboard.Pointer(2,(float)(p.X/ControllerMenu.Width),(float)(p.Y/ControllerMenu.Height));
        }
        keyboard.Render();
        var enter = keyboard.actions.Keys.Single(b => b.Content?.ToString() == Loc.Get("KeyboardEnter"));
        var tab = keyboard.actions.Keys.Single(b => b.Content?.ToString() == "Tab");
        var pc = keyboard.actions.Keys.Single(b => b.Content?.ToString() == "PC keys");
        if (enter.Width != 400 || tab.TranslatePoint(new Point(), keyboard.surface).X >= pc.TranslatePoint(new Point(), keyboard.surface).X || pc.TranslatePoint(new Point(), keyboard.surface).X >= enter.TranslatePoint(new Point(), keyboard.surface).X)
            throw new Exception("Keyboard must put Tab and PC keys on the left and a four-width Enter on the right");
        keyboard.OpenMicrophones(); keyboard.Render();
        if (keyboard.microphoneButtons.Count == 0) throw new Exception("Keyboard microphone picker is empty");
        keyboard.microphonePopup.Visibility = Visibility.Collapsed;
        Click("PC keys"); Click("F12");
        if (edits[^1].Key != 123) throw new Exception("Keyboard function keys failed");
        Click("Copy");
        if (edits[^1].Key != 67 || edits[^1].Modifiers != KeyboardModifiers.Control) throw new Exception("Keyboard Copy shortcut failed");
        Click("Ctrl"); Click("PC keys");
        Point(1, SwipePredictor.KeyPoint('c')); Point(2, SwipePredictor.KeyPoint('c'));
        if (edits[^1].Key != 67 || edits[^1].Text != "" || edits[^1].Modifiers != KeyboardModifiers.Control) throw new Exception("Keyboard Ctrl letter chord failed");
        Point(1, SwipePredictor.KeyPoint('a')); Point(2, SwipePredictor.KeyPoint('a'));
        if (edits[^1].Text != "a" || edits[^1].Modifiers != KeyboardModifiers.None) throw new Exception("Keyboard modifier remained held after a chord");
        Click("PC keys");
        keyboard.Render(); var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(keyboard.bitmap));
        using var file = System.IO.File.Create(System.IO.Path.Combine(output, "floating-keyboard.png")); encoder.Save(file);
    }
}
