using System.Globalization;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace VRX.Desktop;

// An off-screen WPF view, not a second desktop window. Every editable control refers to
// the real settings control, so its existing validation, profile save and live update
// handlers remain authoritative. No mouse/keyboard injection or foreground changes.
public sealed class ControllerMenu : IDisposable
{
    // Versioned, same-user, session-specific IPC; see controller_menu_protocol.h.
    public const int Width = 960, Height = 768, HeaderBytes = 64, EventCount = 64;
    public const int PixelsAt = HeaderBytes + EventCount * 16;
    public const int Capacity = PixelsAt + Width * Height * 4;
    public const int Magic = 0x58524356;
    public const int TitleLeft = 24, TitleTop = 24, TitleHeight = 56, TitleRight = Width - 24;
    private readonly MemoryMappedFile map;
    private readonly MemoryMappedViewAccessor memory;
    private readonly Mutex mutex;
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(50) };
    private readonly UserControl surface;
    private readonly ScrollViewer scroll = new() { VerticalScrollBarVisibility = ScrollBarVisibility.Hidden };
    private readonly StackPanel body = new();
    private readonly TextBlock heading = new() { FontSize = 26, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock interactionHint = new() { FontSize = 16, Margin = new(0, 0, 0, 10), TextWrapping = TextWrapping.Wrap };
    private readonly Border titleBar = new() { Height = TitleHeight, Background = new SolidColorBrush(Color.FromRgb(37, 50, 72)),
        CornerRadius = new(8), Padding = new(12, 0, 12, 0), Margin = new(0, 0, 0, 8) };
    private readonly TextBlock status = new() { FontSize = 16, TextWrapping = TextWrapping.Wrap, MaxHeight = 56 };
    private readonly Dictionary<FrameworkElement, Action<Point, bool>> targets = new();
    private readonly List<Action> refresh = new();
    private readonly IReadOnlyList<Expander> sections;
    private readonly Action applyAll;
    private readonly RenderTargetBitmap bitmap = new(Width, Height, 96, 96, PixelFormats.Pbgra32);
    private readonly byte[] pixels = new byte[Width * Height * 4];
    private FrameworkElement? held;
    private bool enabled, disposed;
    private bool controlScreen;
    private Button controlScreenButton = null!;
    private int generation;
    private int pageGeneration;
    private Action? back;

    public ControllerMenu(string name, IReadOnlyList<Expander> sections, TextBlock desktopStatus,
        Button recenter, Action applyAll)
    {
        this.sections = sections;
        this.applyAll = applyAll;
        mutex = new(false, name + ".Lock");
        map = MemoryMappedFile.CreateOrOpen(name, Capacity, MemoryMappedFileAccess.ReadWrite);
        memory = map.CreateViewAccessor(0, Capacity, MemoryMappedFileAccess.ReadWrite);
        if (!Lock()) throw new InvalidOperationException("Controller menu IPC is busy");
        try
        {
            memory.WriteArray(0, new byte[HeaderBytes], 0, HeaderBytes);
            memory.Write(0, Magic); memory.Write(4, 1);
            memory.Write(20, -1.0f); memory.Write(24, -1.0f);
        }
        finally { mutex.ReleaseMutex(); }

        var root = new DockPanel { Margin = new(TitleLeft, TitleTop, Width - TitleRight, 24) };
        var top = new StackPanel();
        DockPanel.SetDock(top, Dock.Top);
        var title = new DockPanel();
        var dragHint = new TextBlock { Text = Loc.Get("ControllerMenuDrag"), FontSize = 16,
            VerticalAlignment = VerticalAlignment.Center, Margin = new(12, 0, 0, 0) };
        DockPanel.SetDock(dragHint, Dock.Right); title.Children.Add(dragHint); title.Children.Add(heading);
        titleBar.Child = title; top.Children.Add(titleBar);
        interactionHint.Text = Loc.Get("ControllerMenuHint"); top.Children.Add(interactionHint);
        var toolbar = new WrapPanel();
        top.Children.Add(toolbar);
        toolbar.Children.Add(ActionButton(Loc.Get("ControllerMenuBack"), () => { if (back != null) back(); else Home(); }));
        var recenterCopy = ActionButton(Loc.Get("RecenterNow"), () => recenter.RaiseEvent(new RoutedEventArgs(Button.ClickEvent)));
        Bind(recenterCopy, UIElement.IsEnabledProperty, recenter, nameof(recenter.IsEnabled));
        toolbar.Children.Add(recenterCopy);
        controlScreenButton = ActionButton(Loc.Get("ControllerMenuControlScreen"), () =>
        {
            controlScreen = !controlScreen;
            controlScreenButton.Content = Loc.Get(controlScreen ? "ControllerMenuStopControlScreen" : "ControllerMenuControlScreen");
            interactionHint.Text = Loc.Get(controlScreen ? "ControllerMenuScreenHint" : "ControllerMenuHint");
        });
        toolbar.Children.Add(controlScreenButton);
        root.Children.Add(top);
        DockPanel.SetDock(status, Dock.Bottom);
        Bind(status, TextBlock.TextProperty, desktopStatus, nameof(TextBlock.Text));
        root.Children.Add(status);
        scroll.Content = body;
        root.Children.Add(scroll);
        surface = new UserControl
        {
            Width = Width, Height = Height, FontSize = 21, FontFamily = new FontFamily("Segoe UI"),
            Foreground = Brushes.White, Content = new Border { Background = new SolidColorBrush(Color.FromRgb(16, 24, 40)), Child = root },
            Language = System.Windows.Markup.XmlLanguage.GetLanguage(CultureInfo.CurrentCulture.IetfLanguageTag)
        };
        Home();
        timer.Tick += (_, _) => Tick();
        timer.Start();
    }

    public void Enable(bool value)
    {
        enabled = value;
        if (!value)
        {
            controlScreen = false;
            controlScreenButton.Content = Loc.Get("ControllerMenuControlScreen");
            interactionHint.Text = Loc.Get("ControllerMenuHint");
        }
        held = null;
        if (!Lock()) return; // Tick retries; a busy renderer must not block WPF.
        try { memory.Write(8, value ? 1 : 0); memory.Write(36, controlScreen ? 1 : 0); }
        finally { mutex.ReleaseMutex(); }
    }

    private bool Lock()
    {
        try { return mutex.WaitOne(0); }
        catch (AbandonedMutexException) { return true; }
    }

    private void Page(string title, Action? parent)
    {
        pageGeneration++;
        held = null; refresh.Clear();
        // Keep toolbar targets; detached page targets must never receive late releases.
        foreach (var key in targets.Keys.Where(k => IsUnder(k, body)).ToArray()) targets.Remove(key);
        body.Children.Clear();
        heading.Text = title; back = parent; scroll.ScrollToTop();
    }

    private void Home()
    {
        Page(Loc.Get("ControllerMenuTitle"), null);
        foreach (var section in sections)
            body.Children.Add(ActionButton(section.Header?.ToString() ?? "", () => Section(section)));
    }

    private void Section(Expander section)
    {
        Page(section.Header?.ToString() ?? "", Home);
        if (section.Content is FrameworkElement content && Project(content) is { } copy) body.Children.Add(copy);
    }

    private static void Bind(DependencyObject target, DependencyProperty property, object source, string path,
        BindingMode mode = BindingMode.OneWay) => BindingOperations.SetBinding(target, property,
            new Binding(path) { Source = source, Mode = mode, UpdateSourceTrigger = UpdateSourceTrigger.PropertyChanged });

    private FrameworkElement? Project(FrameworkElement source)
    {
        FrameworkElement? copy;
        switch (source)
        {
            case Canvas: return null; // The desktop's mouse-drag preview is replaced by the screen sliders.
            case TextBlock text:
                var label = new TextBlock { TextWrapping = TextWrapping.Wrap, FontSize = 19, Margin = new(0, 7, 0, 5) };
                Bind(label, TextBlock.TextProperty, text, nameof(text.Text)); copy = label; break;
            case Slider slider:
                var track = new Slider { Minimum = slider.Minimum, Maximum = slider.Maximum, Height = 48,
                    TickFrequency = slider.TickFrequency, IsSnapToTickEnabled = slider.IsSnapToTickEnabled, Margin = new(12, 2, 12, 12) };
                Bind(track, RangeBase.ValueProperty, slider, nameof(slider.Value), BindingMode.TwoWay);
                targets[track] = (p, _) => track.SetCurrentValue(RangeBase.ValueProperty,
                    ControllerMenuInput.SliderValue(p.X, track.ActualWidth, track.Minimum, track.Maximum, track.IsSnapToTickEnabled ? track.TickFrequency : 0));
                copy = track; break;
            case CheckBox check:
                var box = new CheckBox { FontSize = 21, Margin = new(0, 10, 0, 10), MinHeight = 40 };
                Bind(box, ContentControl.ContentProperty, check, nameof(check.Content));
                Bind(box, ToggleButton.IsCheckedProperty, check, nameof(check.IsChecked), BindingMode.TwoWay);
                targets[box] = (_, down) => { if (!down) box.SetCurrentValue(ToggleButton.IsCheckedProperty, box.IsChecked != true); };
                copy = box; break;
            case Selector selector:
                var pick = ActionButton("", () => Choices(selector));
                void UpdateChoice() => pick.Content = ItemLabel(selector, selector.SelectedItem);
                UpdateChoice(); refresh.Add(UpdateChoice); copy = pick; break;
            case Button button:
                copy = ActionButton(button.Content?.ToString() ?? "", () =>
                {
                    if (button.Name == "ApplyAllButton") ConfirmApply();
                    else button.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                }); break;
            case TextBox text when text.IsReadOnly:
                var read = new TextBlock { FontSize = 16, TextWrapping = TextWrapping.Wrap };
                Bind(read, TextBlock.TextProperty, text, nameof(text.Text)); copy = read; break;
            case TextBox text:
                var edit = ActionButton(text.Text, () => HexEditor(text));
                refresh.Add(() => edit.Content = text.Text); copy = edit; break;
            case DockPanel dock:
                var dockCopy = new DockPanel { LastChildFill = dock.LastChildFill };
                foreach (UIElement child in dock.Children)
                    if (child is FrameworkElement element && Project(element) is { } item)
                    { DockPanel.SetDock(item, DockPanel.GetDock(child)); dockCopy.Children.Add(item); }
                copy = dockCopy; break;
            case Grid grid:
                var gridCopy = new Grid();
                foreach (var column in grid.ColumnDefinitions) gridCopy.ColumnDefinitions.Add(new ColumnDefinition { Width = column.Width });
                foreach (var row in grid.RowDefinitions) gridCopy.RowDefinitions.Add(new RowDefinition { Height = row.Height });
                foreach (UIElement child in grid.Children)
                    if (child is FrameworkElement element && Project(element) is { } item)
                    {
                        Grid.SetColumn(item, Grid.GetColumn(child)); Grid.SetRow(item, Grid.GetRow(child));
                        Grid.SetColumnSpan(item, Grid.GetColumnSpan(child)); Grid.SetRowSpan(item, Grid.GetRowSpan(child));
                        gridCopy.Children.Add(item);
                    }
                copy = gridCopy; break;
            case WrapPanel wrap:
                var wrapCopy = new WrapPanel { Orientation = wrap.Orientation };
                foreach (UIElement child in wrap.Children)
                    if (child is FrameworkElement element && Project(element) is { } item) wrapCopy.Children.Add(item);
                copy = wrapCopy; break;
            default:
                var stack = new StackPanel();
                foreach (object child in LogicalTreeHelper.GetChildren(source))
                    if (child is FrameworkElement element && Project(element) is { } item) stack.Children.Add(item);
                if (stack.Children.Count == 0) return null;
                copy = stack; break;
        }
        Bind(copy, UIElement.IsEnabledProperty, source, nameof(source.IsEnabled));
        Bind(copy, UIElement.VisibilityProperty, source, nameof(source.Visibility));
        return copy;
    }

    private string ItemLabel(Selector selector, object? item)
    {
        if (item is ContentControl content) return content.Content?.ToString() ?? "";
        string path = selector is ItemsControl items ? items.DisplayMemberPath : "";
        if (string.IsNullOrEmpty(path) && item is RunningApp) path = nameof(RunningApp.Name);
        return (string.IsNullOrEmpty(path) ? item : item?.GetType().GetProperty(path)?.GetValue(item))?.ToString() ?? Loc.Get("ControllerMenuSelect");
    }

    private void Choices(Selector source)
    {
        string title = heading.Text;
        // Reconstruct the owning section on return, rather than reparenting live controls.
        Action parent = () => { var section = sections.FirstOrDefault(s => s.Header?.ToString() == title); if (section != null) Section(section); else Home(); };
        Page(Loc.Get("ControllerMenuSelect"), parent);
        foreach (object item in source.Items)
        {
            if (item is FrameworkElement element && (element.Visibility != Visibility.Visible || !element.IsEnabled)) continue;
            body.Children.Add(ActionButton(ItemLabel(source, item), () =>
            {
                source.SetCurrentValue(Selector.SelectedItemProperty, item);
                parent();
            }));
        }
    }

    private void HexEditor(TextBox source)
    {
        string value = source.Text.TrimStart('#');
        Page(Loc.Get("WorldHexTip"), Home);
        var display = new TextBlock { FontSize = 32, Text = "#" + value, Margin = new(0, 12, 0, 12) };
        body.Children.Add(display);
        var keys = new WrapPanel(); body.Children.Add(keys);
        foreach (char key in "0123456789ABCDEF")
        {
            var button = ActionButton(key.ToString(), () => { if (value.Length < 6) value += key; display.Text = "#" + value; });
            button.Width = 90; keys.Children.Add(button);
        }
        body.Children.Add(ActionButton(Loc.Get("ControllerMenuDelete"), () => { if (value.Length > 0) value = value[..^1]; display.Text = "#" + value; }));
        body.Children.Add(ActionButton(Loc.Get("ControllerMenuApply"), () =>
        {
            if (value.Length != 6) return;
            source.SetCurrentValue(TextBox.TextProperty, "#" + value);
            source.RaiseEvent(new RoutedEventArgs(UIElement.LostFocusEvent));
            Home();
        }));
    }

    private void ConfirmApply()
    {
        Page(Loc.Get("ApplyAllTitle"), Home);
        body.Children.Add(new TextBlock { Text = Loc.Get("ControllerMenuApplyAllQuestion"), TextWrapping = TextWrapping.Wrap, Margin = new(0, 20, 0, 20) });
        body.Children.Add(ActionButton(Loc.Get("ControllerMenuCancel"), Home));
        body.Children.Add(ActionButton(Loc.Get("ControllerMenuApply"), () => { applyAll(); Home(); }));
    }

    private Button ActionButton(string label, Action action)
    {
        var button = new Button { Content = label, FontSize = 21, MinHeight = 48, Margin = new(0, 4, 8, 4),
            HorizontalContentAlignment = HorizontalAlignment.Left, Padding = new(16, 8, 16, 8) };
        targets[button] = (_, down) => { if (!down) action(); };
        return button;
    }

    private static bool IsUnder(DependencyObject element, DependencyObject parent)
    {
        for (DependencyObject? p = element; p != null; p = VisualTreeHelper.GetParent(p)) if (p == parent) return true;
        return false;
    }

    private FrameworkElement? Hit(Point point)
    {
        // Use visual hit testing: this tree deliberately has no HWND/presentation
        // source, and text hits must not hand a non-Visual Run to GetParent.
        DependencyObject? hit = VisualTreeHelper.HitTest(surface, point)?.VisualHit;
        while (hit != null)
        {
            if (hit is FrameworkElement element && element.IsEnabled && targets.ContainsKey(element)) return element;
            hit = VisualTreeHelper.GetParent(hit);
        }
        return null;
    }

    private void Pointer(int kind, float x, float y, float value)
    {
        if (kind == 4) { held = null; return; }
        if (!float.IsFinite(x) || !float.IsFinite(y) || !float.IsFinite(value)) return;
        var point = new Point(x * Width, y * Height);
        if (kind == 3) { scroll.ScrollToVerticalOffset(scroll.VerticalOffset - Math.Clamp(value, -1000, 1000)); return; }
        if (kind == 1)
        {
            held = Hit(point);
            if (held != null) targets[held](surface.TranslatePoint(point, held), true);
        }
        else if (kind == 0 && held is Slider) targets[held](surface.TranslatePoint(point, held), true);
        else if (kind == 2)
        {
            var target = held; held = null;
            if (target != null && target.IsEnabled && targets.TryGetValue(target, out var action) &&
                (target is Slider || Hit(point) == target)) action(surface.TranslatePoint(point, target), false);
        }
    }

    private void Tick()
    {
        if (disposed || !Lock()) return;
        var events = new List<(int Kind, float X, float Y, float Value)>();
        bool visible;
        try
        {
            memory.Write(8, enabled ? 1 : 0);
            memory.Write(36, enabled && controlScreen ? 1 : 0);
            memory.Write(40, unchecked((ulong)Environment.TickCount64));
            visible = enabled && memory.ReadInt32(12) != 0;
            uint write = memory.ReadUInt32(28), read = memory.ReadUInt32(32);
            // A producer overflow cancels a drag; never turn a dropped release into a stuck slider.
            if (unchecked(write - read) > EventCount) { held = null; read = write; }
            while (read != write)
            {
                int offset = HeaderBytes + (int)(read % EventCount) * 16;
                events.Add((memory.ReadInt32(offset), memory.ReadSingle(offset + 4), memory.ReadSingle(offset + 8), memory.ReadSingle(offset + 12)));
                read++;
            }
            memory.Write(32, read);
        }
        finally { mutex.ReleaseMutex(); }
        if (!visible) { held = null; return; }
        surface.Measure(new Size(Width, Height)); surface.Arrange(new Rect(0, 0, Width, Height)); surface.UpdateLayout();
        int page = pageGeneration;
        foreach (var e in events)
        {
            Pointer(e.Kind, e.X, e.Y, e.Value);
            if (page != pageGeneration) break; // remaining events describe the old bitmap
        }
        foreach (var update in refresh.ToArray()) update();
        surface.UpdateLayout();
        bitmap.Clear(); bitmap.Render(surface); bitmap.CopyPixels(pixels, Width * 4, 0);
        // A solid white texel region for the renderer's low-latency beams and pointer.
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        { int p = (y * Width + x) * 4; pixels[p] = pixels[p + 1] = pixels[p + 2] = pixels[p + 3] = 255; }
        if (!Lock()) return;
        try { memory.WriteArray(PixelsAt, pixels, 0, pixels.Length); memory.Write(16, ++generation); }
        finally { mutex.ReleaseMutex(); }
    }

    public void Dispose()
    {
        if (disposed) return;
        Enable(false); disposed = true; timer.Stop();
        memory.Dispose(); map.Dispose(); mutex.Dispose();
    }

    // Runs on Windows as part of --smoke-test. Exercises the actual off-screen
    // visual tree and IPC, without a headset, OS input injection or a second window.
    internal void SmokeTest(string output)
    {
        Enable(true);
        if (!Lock()) throw new Exception("Menu test could not lock IPC");
        try { memory.Write(12, 1); }
        finally { mutex.ReleaseMutex(); }
        Tick();
        if (memory.ReadInt32(16) == 0) throw new Exception("Visible controller menu did not publish a bitmap");
        var bar = titleBar.TranslatePoint(new Point(0, 0), surface);
        if (bar.X != TitleLeft || bar.Y != TitleTop || titleBar.ActualHeight != TitleHeight || titleBar.ActualWidth != TitleRight - TitleLeft)
            throw new Exception("The visible title bar must match the renderer's drag region");
        if (pixels.Take(4).Any(b => b != 255)) throw new Exception("Controller beams must use solid white texels");
        ClickButton(Loc.Get("ControllerMenuControlScreen")); Tick();
        if (memory.ReadInt32(36) != 1) throw new Exception("Control screen button did not enable mouse mode through IPC");
        ClickButton(Loc.Get("ControllerMenuStopControlScreen")); Tick();
        if (memory.ReadInt32(36) != 0) throw new Exception("Stop controlling screen did not disable mouse mode");
        ClickButton(Loc.Get("ControllerMenuControlScreen")); Tick();
        Enable(false); Tick();
        if (memory.ReadInt32(36) != 0) throw new Exception("Controllers off must disable mouse mode");
        if (memory.ReadInt32(12) != 1) throw new Exception("Desktop must leave wall-menu visibility to the renderer");
        Enable(true); Tick();
        var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using (var file = File.Create(System.IO.Path.Combine(output, "controller-menu-sections.png"))) encoder.Save(file);

        Section(sections[1]); Tick(); // Screen controls, including an editable slider.
        var track = targets.Keys.OfType<Slider>().First();
        var source = (Slider)BindingOperations.GetBinding(track, RangeBase.ValueProperty)!.Source;
        double before = source.Value;
        var point = track.TranslatePoint(new Point(track.ActualWidth - 12, track.ActualHeight / 2), surface);
        float x = (float)(point.X / Width), y = (float)(point.Y / Height);
        // Feed an ordered Down/Up pair through the same shared event ring as OpenXR.
        if (!Lock()) throw new Exception("Menu test could not lock IPC");
        try
        {
            uint write = memory.ReadUInt32(28);
            foreach (int kind in new[] { 1, 2 })
            {
                int offset = HeaderBytes + (int)(write++ % EventCount) * 16;
                memory.Write(offset, kind); memory.Write(offset + 4, x); memory.Write(offset + 8, y); memory.Write(offset + 12, 0f);
            }
            memory.Write(28, write);
        }
        finally { mutex.ReleaseMutex(); }
        Tick();
        if (Math.Abs(source.Value - source.Maximum) > .001) throw new Exception("Controller slider did not update the desktop setting through IPC");
        source.Value = before;
        Tick();
        if (track.Value != before) throw new Exception("Desktop setting did not update the controller slider");
        var box = targets.Keys.OfType<CheckBox>().First();
        var original = (CheckBox)BindingOperations.GetBinding(box, ToggleButton.IsCheckedProperty)!.Source;
        bool? old = original.IsChecked;
        point = box.TranslatePoint(new Point(8, 8), surface);
        Pointer(1, (float)(point.X / Width), (float)(point.Y / Height), 0);
        Pointer(2, (float)(point.X / Width), (float)(point.Y / Height), 0);
        if (original.IsChecked == old) throw new Exception("Controller checkbox did not update the desktop setting");
        original.IsChecked = old;
        Pointer(1, x, y, 0); Pointer(4, -1, -1, 0);
        if (held != null) throw new Exception("Focus loss must cancel a controller drag");

        // Render every settings section so missing widgets/resources fail the smoke test.
        for (int i = 0; i < sections.Count; i++)
        {
            Section(sections[i]); Tick();
            if (i == 1)
            {
                var image = new PngBitmapEncoder(); image.Frames.Add(BitmapFrame.Create(bitmap));
                using var file = File.Create(System.IO.Path.Combine(output, "controller-menu-screen.png")); image.Save(file);
            }
        }
        if (!targets.Keys.OfType<Button>().Any()) throw new Exception("Controller section navigation lost its buttons");

        void ClickButton(string label)
        {
            Tick();
            var button = targets.Keys.OfType<Button>().Single(b => b.Content?.ToString() == label);
            var p = button.TranslatePoint(new Point(button.ActualWidth / 2, button.ActualHeight / 2), surface);
            Pointer(1, (float)(p.X / Width), (float)(p.Y / Height), 0);
            Pointer(2, (float)(p.X / Width), (float)(p.Y / Height), 0);
        }

        // Exercise the nested pages through real hit testing. Isolated source controls
        // let us check validation/events without changing the user's active profile.
        var choices = new ComboBox();
        for (int i = 0; i < 25; i++) choices.Items.Add(new ComboBoxItem { Content = $"Option {i}" });
        choices.Items.Add(new ComboBoxItem { Content = "Hidden option", Visibility = Visibility.Collapsed });
        choices.SelectedIndex = 0;
        Choices(choices); Tick();
        if (targets.Keys.OfType<Button>().Any(b => b.Content?.ToString() == "Hidden option"))
            throw new Exception("Hidden desktop options must stay hidden in the controller menu");
        Pointer(3, .5f, .5f, -1000); Tick();
        if (scroll.VerticalOffset <= 0) throw new Exception("Thumbstick input did not scroll the option list");
        scroll.ScrollToTop(); Tick();
        ClickButton("Option 1");
        if (choices.SelectedIndex != 1) throw new Exception("Controller option selection did not update its desktop source");

        var hex = new TextBox { Text = "#123456" };
        int committed = 0;
        hex.LostFocus += (_, _) => committed++;
        HexEditor(hex);
        ClickButton(Loc.Get("ControllerMenuDelete"));
        ClickButton("A");
        ClickButton(Loc.Get("ControllerMenuApply"));
        if (hex.Text != "#12345A" || committed != 1)
            throw new Exception("Controller colour keypad did not commit through desktop validation");
        ConfirmApply(); Tick();
        if (heading.Text != Loc.Get("ApplyAllTitle")) throw new Exception("Apply to all needs an in-headset confirmation");
        ClickButton(Loc.Get("ControllerMenuCancel"));
        if (heading.Text != Loc.Get("ControllerMenuTitle")) throw new Exception("Cancelling Apply to all must return to the menu");
        Enable(false); Tick();
        if (memory.ReadInt32(8) != 0 || memory.ReadInt32(36) != 0 || held != null)
            throw new Exception("Disabling controllers must stop menu and mouse input; the renderer owns wall visibility");
    }
}

public static class ControllerMenuInput
{
    public static double SliderValue(double x, double width, double minimum, double maximum, double tick)
    {
        double value = minimum + Math.Clamp((x - 12) / Math.Max(1, width - 24), 0, 1) * (maximum - minimum);
        if (tick > 0) value = minimum + Math.Round((value - minimum) / tick) * tick;
        return Math.Clamp(value, minimum, maximum);
    }
}
