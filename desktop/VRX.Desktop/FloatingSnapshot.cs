using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace VRX.Desktop;

internal static class FloatingPanelChrome
{
    internal static Grid Scrollable(ScrollViewer scroll)
    {
        var grid = new Grid();
        scroll.Margin = new Thickness(0, 0, 18, 0); grid.Children.Add(scroll);
        var track = new Canvas { Width = 12, HorizontalAlignment = HorizontalAlignment.Right, IsHitTestVisible = false, ClipToBounds = true };
        var thumb = new Border { Tag = "ScrollPosition", Width = 4, CornerRadius = new CornerRadius(2), Opacity = .7 };
        thumb.SetResourceReference(Border.BackgroundProperty, "Accent");
        var up = new TextBlock { Text = "⌃", FontSize = 16, Opacity = .7 };
        var down = new TextBlock { Text = "⌄", FontSize = 16, Opacity = .7 };
        up.SetResourceReference(TextBlock.ForegroundProperty, "Accent"); down.SetResourceReference(TextBlock.ForegroundProperty, "Accent");
        track.Children.Add(thumb); track.Children.Add(up); track.Children.Add(down); grid.Children.Add(track);
        void Update()
        {
            track.Visibility = scroll.ScrollableHeight > 1 ? Visibility.Visible : Visibility.Hidden;
            double length = Math.Max(0, scroll.ActualHeight - 40);
            thumb.Height = Math.Min(length, Math.Max(24, length * scroll.ViewportHeight / Math.Max(1, scroll.ExtentHeight)));
            Canvas.SetTop(thumb, 20 + Math.Max(0, length - thumb.Height) * scroll.VerticalOffset / Math.Max(1, scroll.ScrollableHeight));
            Canvas.SetLeft(thumb, 4); Canvas.SetTop(up, 0); Canvas.SetTop(down, Math.Max(0, scroll.ActualHeight - 20));
            up.Visibility = scroll.VerticalOffset > 1 ? Visibility.Visible : Visibility.Hidden;
            down.Visibility = scroll.VerticalOffset < scroll.ScrollableHeight - 1 ? Visibility.Visible : Visibility.Hidden;
        }
        scroll.ScrollChanged += (_, _) => Update(); scroll.SizeChanged += (_, _) => Update();
        return grid;
    }
    internal static UIElement Title(UIElement heading, Button close)
    {
        var label = close.Content?.ToString() ?? Loc.Get("SnapshotClose");
        close.ToolTip = label; System.Windows.Automation.AutomationProperties.SetName(close, label);
        close.Content = "×"; close.Tag = ""; close.FontSize = 28; close.Padding = new Thickness(0);
        close.HorizontalContentAlignment = HorizontalAlignment.Center;
        close.Width = 80; close.Height = 48; close.Margin = new Thickness(4);
        var row = new DockPanel(); DockPanel.SetDock(close, Dock.Right);
        row.Children.Add(close); row.Children.Add(heading);
        return row;
    }
    internal static Grid Wrap(UIElement content)
    {
        var grid = new Grid();
        foreach (double margin in new[] { 12.0, 14.0 })
            grid.Children.Add(new Border { Tag = "GlassGlow", Margin = new Thickness(margin), CornerRadius = new CornerRadius(18),
                BorderThickness = new Thickness(2), BorderBrush = new SolidColorBrush(Color.FromArgb(30, 100, 211, 224)), IsHitTestVisible = false });
        grid.Children.Add(new Border
        {
            Margin = new Thickness(16), CornerRadius = new CornerRadius(14), BorderThickness = new Thickness(1.5),
            Tag = "GlassFrame",
            BorderBrush = new SolidColorBrush(Color.FromRgb(68, 132, 145)),
            Background = new SolidColorBrush(Color.FromRgb(16, 24, 40))
        });
        var decoration = new Canvas { IsHitTestVisible = false, ClipToBounds = true };
        foreach (var at in new[] { new Point(764, 570), new Point(30, 640) })
        {
            var glyph = new TextBlock { Tag = "ThemeDecoration", FontFamily = new FontFamily("Segoe UI Emoji"), FontSize = 88 };
            Canvas.SetLeft(glyph, at.X); Canvas.SetTop(glyph, at.Y); decoration.Children.Add(glyph);
        }
        grid.Children.Add(decoration);
        grid.Children.Add(content);
        Appearance.Register(grid);
        return grid;
    }
}

internal sealed class FloatingSnapshot : IFloatingPanel
{
    private readonly BitmapSource original;
    private readonly string imagesFolder;
    private readonly UserControl surface;
    private readonly TextBlock status = new() { Foreground = Brushes.White, FontSize = 15, Margin = new Thickness(8), TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly Dictionary<Button, Action> actions = new();
    private readonly RenderTargetBitmap bitmap = new(ControllerMenu.PixelWidth, ControllerMenu.PixelHeight, 192, 192, PixelFormats.Pbgra32);
    private Button? held;
    internal string? SavedPath { get; private set; }
    public bool Closed { get; private set; }
    private int appearanceRevision = -1;
    private bool dirty = true;
    public bool Dirty { get => dirty || appearanceRevision != Appearance.Revision; private set => dirty = value; }
    internal FloatingSnapshot(BitmapSource image, string imagesFolder)
    {
        original = image; this.imagesFolder = imagesFolder;
        var root = new DockPanel { Margin = new Thickness(48) };
        var title = new Border { Height = 56, CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Color.FromRgb(37, 50, 72)),
            Child = FloatingPanelChrome.Title(new TextBlock { Text = Loc.Get("SnapshotTitle"), Foreground = Brushes.White, FontSize = 25, Margin = new Thickness(12, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center }, CloseButton()) };
        DockPanel.SetDock(title, Dock.Top); root.Children.Add(title);
        var bottom = new StackPanel { Orientation = Orientation.Horizontal, Height = 56, Margin = new Thickness(0, 8, 0, 0) };
        DockPanel.SetDock(bottom, Dock.Bottom); root.Children.Add(bottom);
        AddButton(bottom, Loc.Get("SnapshotSave"), Save);
        AddButton(bottom, Loc.Get("SnapshotDelete"), Delete);
        bottom.Children.Add(status);
        root.Children.Add(new Image { Source = original, Stretch = Stretch.Uniform, Margin = new Thickness(0, 8, 0, 0) });
        surface = new UserControl { Width = ControllerMenu.Width, Height = ControllerMenu.Height, Content = FloatingPanelChrome.Wrap(root), FontFamily = new FontFamily("Segoe UI") };
    }
    private Button CloseButton()
    {
        var button = new Button { Content = Loc.Get("SnapshotClose"), FontSize = 20 };
        actions[button] = () => Closed = true; return button;
    }
    private void AddButton(Panel panel, string label, Action action)
    {
        var button = new Button { Content = label, FontSize = 21, MinWidth = 130, Height = 48, Margin = new Thickness(0, 0, 8, 0) };
        panel.Children.Add(button); actions[button] = action;
    }
    private void Save()
    {
        try
        {
            if (SavedPath == null)
            {
                Directory.CreateDirectory(imagesFolder);
                string path = Path.Combine(imagesFolder, $"VRX-{DateTime.Now:yyyyMMdd-HHmmss-fff}-{Guid.NewGuid():N}.png");
                bool created = false;
                try
                {
                    using var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                    created = true;
                    var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(original)); encoder.Save(file);
                }
                catch { if (created && File.Exists(path)) File.Delete(path); throw; }
                SavedPath = path;
            }
            status.Text = Loc.Get("SnapshotSaved");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or NotSupportedException)
        { status.Text = Loc.Format("SnapshotFileError", ex.Message); }
        Dirty = true;
    }
    private void Delete()
    {
        try { if (SavedPath != null) File.Delete(SavedPath); Closed = true; }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        { status.Text = Loc.Format("SnapshotFileError", ex.Message); Dirty = true; }
    }
    public void Pointer(int kind, float x, float y, float value = 0)
    {
        if (kind == 4) { PanelFeedback.Cancel(held, () => Dirty = true); held = null; return; }
        var point = new Point(x * ControllerMenu.Width, y * ControllerMenu.Height);
        var hit = actions.Keys.FirstOrDefault(b => b.IsEnabled && new Rect(b.TranslatePoint(new Point(), surface), b.RenderSize).Contains(point));
        if (kind == 1) { held = hit; PanelFeedback.Down(held, () => Dirty = true); }
        if (kind == 2)
        {
            var pressed = held; held = null;
            if (pressed != null && pressed == hit) { PanelFeedback.Up(pressed, () => Dirty = true); actions[pressed](); } else PanelFeedback.Cancel(pressed, () => Dirty = true);
        }
    }
    public byte[] Render()
    {
        surface.Measure(new Size(ControllerMenu.Width, ControllerMenu.Height));
        surface.Arrange(new Rect(0, 0, ControllerMenu.Width, ControllerMenu.Height)); surface.UpdateLayout();
        bitmap.Clear(); bitmap.Render(surface);
        var pixels = new byte[ControllerMenu.PixelBytes];
        bitmap.CopyPixels(pixels, ControllerMenu.PixelWidth * 4, 0);
        return pixels;
    }
    public void Published() { Dirty = false; appearanceRevision = Appearance.Revision; }
    public void Dispose() { held = null; }
    internal static void SmokeTest(string folder)
    {
        byte[] colours = { 0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255 };
        var image = BitmapSource.Create(2, 2, 96, 96, PixelFormats.Bgra32, null, colours, 8); image.Freeze();
        var snapshot = new FloatingSnapshot(image, Path.Combine(folder, "snapshot-images"));
        void Click(FloatingSnapshot panel, string label)
        {
            panel.Render();
            var button = panel.actions.Keys.Single(b => b.Content?.ToString() == label || b.ToolTip?.ToString() == label);
            var point = button.TranslatePoint(new Point(button.ActualWidth / 2, button.ActualHeight / 2), panel.surface);
            panel.Pointer(1, (float)(point.X / ControllerMenu.Width), (float)(point.Y / ControllerMenu.Height));
            panel.Pointer(2, (float)(point.X / ControllerMenu.Width), (float)(point.Y / ControllerMenu.Height));
        }
        Click(snapshot, Loc.Get("SnapshotSave"));
        if (snapshot.SavedPath == null || !File.Exists(snapshot.SavedPath)) throw new Exception("Snapshot Save did not create an image");
        var decoded = new BitmapImage(); decoded.BeginInit(); decoded.CacheOption = BitmapCacheOption.OnLoad;
        decoded.UriSource = new Uri(snapshot.SavedPath); decoded.EndInit();
        if (decoded.PixelWidth != 2 || decoded.PixelHeight != 2) throw new Exception("Snapshot must save the original full-resolution image");
        var png = new PngBitmapEncoder(); png.Frames.Add(BitmapFrame.Create(snapshot.bitmap));
        using (var file = File.Create(Path.Combine(folder, "floating-snapshot.png"))) png.Save(file);
        var saved = snapshot.SavedPath; Click(snapshot, Loc.Get("SnapshotClose"));
        if (!snapshot.Closed || !File.Exists(saved)) throw new Exception("Close must preserve a saved snapshot");
        var delete = new FloatingSnapshot(image, snapshot.imagesFolder); Click(delete, Loc.Get("SnapshotSave"));
        var deleted = delete.SavedPath!; Click(delete, Loc.Get("SnapshotDelete"));
        if (!delete.Closed || File.Exists(deleted)) throw new Exception("Delete must remove the saved image and close its panel");
        File.Delete(saved);
    }
}
