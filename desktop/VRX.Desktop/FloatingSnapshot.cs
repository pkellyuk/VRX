using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;

namespace VRX.Desktop;

internal static class FloatingPanelChrome
{
    internal static Grid Wrap(UIElement content)
    {
        var grid = new Grid();
        grid.Children.Add(new Border
        {
            Margin = new Thickness(16), CornerRadius = new CornerRadius(14), BorderThickness = new Thickness(6),
            BorderBrush = new SolidColorBrush(Color.FromRgb(68, 132, 145)),
            Background = new SolidColorBrush(Color.FromRgb(16, 24, 40)),
            Effect = new DropShadowEffect { Color = Color.FromRgb(100, 211, 224), BlurRadius = 10, ShadowDepth = 0, Opacity = .22 }
        });
        grid.Children.Add(content);
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
    public bool Dirty { get; private set; } = true;
    internal FloatingSnapshot(BitmapSource image, string imagesFolder)
    {
        original = image; this.imagesFolder = imagesFolder;
        var root = new DockPanel { Margin = new Thickness(48) };
        var title = new Border { Height = 56, CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Color.FromRgb(37, 50, 72)),
            Child = new TextBlock { Text = Loc.Get("SnapshotTitle"), Foreground = Brushes.White, FontSize = 25, Margin = new Thickness(12, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center } };
        DockPanel.SetDock(title, Dock.Top); root.Children.Add(title);
        var bottom = new StackPanel { Orientation = Orientation.Horizontal, Height = 56, Margin = new Thickness(0, 8, 0, 0) };
        DockPanel.SetDock(bottom, Dock.Bottom); root.Children.Add(bottom);
        AddButton(bottom, Loc.Get("SnapshotSave"), Save);
        AddButton(bottom, Loc.Get("SnapshotDelete"), Delete);
        AddButton(bottom, Loc.Get("SnapshotClose"), () => Closed = true);
        bottom.Children.Add(status);
        root.Children.Add(new Image { Source = original, Stretch = Stretch.Uniform, Margin = new Thickness(0, 8, 0, 0) });
        surface = new UserControl { Width = ControllerMenu.Width, Height = ControllerMenu.Height, Content = FloatingPanelChrome.Wrap(root), FontFamily = new FontFamily("Segoe UI") };
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
        if (kind == 4) { held = null; return; }
        var point = new Point(x * ControllerMenu.Width, y * ControllerMenu.Height);
        var hit = actions.Keys.FirstOrDefault(b => b.IsEnabled && new Rect(b.TranslatePoint(new Point(), surface), b.RenderSize).Contains(point));
        if (kind == 1) held = hit;
        if (kind == 2)
        {
            var pressed = held; held = null;
            if (pressed != null && pressed == hit) actions[pressed]();
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
    public void Published() => Dirty = false;
    public void Dispose() { held = null; }
    internal static void SmokeTest(string folder)
    {
        byte[] colours = { 0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255 };
        var image = BitmapSource.Create(2, 2, 96, 96, PixelFormats.Bgra32, null, colours, 8); image.Freeze();
        var snapshot = new FloatingSnapshot(image, Path.Combine(folder, "snapshot-images"));
        void Click(FloatingSnapshot panel, string label)
        {
            panel.Render();
            var button = panel.actions.Keys.Single(b => b.Content?.ToString() == label);
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
