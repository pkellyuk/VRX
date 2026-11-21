using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using VRX.VrexBridge;

namespace VRX.Desktop;

internal sealed class FloatingVrexGridPanel : IFloatingPanel
{
    private readonly IVrexClient client;
    private readonly UserControl surface;
    private readonly Button close = new() { Content = Loc.Get("SnapshotClose") };
    private readonly Grid viewport = new();
    private readonly Image image = new() { Stretch = Stretch.Uniform };
    private readonly Canvas overlay = new() { IsHitTestVisible = false };
    private readonly Ellipse marker = new() { Width = 34, Height = 34, StrokeThickness = 4,
        Fill = new SolidColorBrush(Color.FromArgb(90, 0, 0, 0)), Visibility = Visibility.Collapsed };
    private readonly TextBlock detail = new() { Text = "Waiting for Vrex to inspect a screen.", FontSize = 19, Margin = new(6), TextWrapping = TextWrapping.Wrap };
    private readonly RenderTargetBitmap bitmap = new(ControllerMenu.PixelWidth, ControllerMenu.PixelHeight, 192, 192, PixelFormats.Pbgra32);
    private readonly byte[] pixels = new byte[ControllerMenu.PixelBytes];
    private readonly PanelFeedback.Hover hover = new();
    private bool held, dirty = true;
    private int appearance = -1;
    private string? source;
    private string viewKind = "";
    private VrexMarkPoint? point;
    public bool Closed { get; private set; }
    public bool Dirty => dirty || appearance != Appearance.Revision;

    internal FloatingVrexGridPanel(IVrexClient client)
    {
        this.client = client;
        var root = new DockPanel { Margin = new Thickness(48) };
        var title = new Border { Height = 56, Child = FloatingPanelChrome.Title(new TextBlock {
            Text = "Vrex zoom grid", FontSize = 25, Margin = new Thickness(12, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center
        }, close) };
        DockPanel.SetDock(title, Dock.Top);
        DockPanel.SetDock(detail, Dock.Bottom);
        root.Children.Add(title);
        root.Children.Add(detail);
        viewport.Children.Add(image);
        overlay.Children.Add(marker);
        viewport.Children.Add(overlay);
        root.Children.Add(viewport);
        surface = new UserControl { Width = ControllerMenu.Width, Height = ControllerMenu.Height, Content = FloatingPanelChrome.Wrap(root) };
        client.Changed += Changed;
        client.Send(new("grid-viewer", Key: 1));
    }

    private void Changed(VrexEvent message)
    {
        if (Closed) return;
        switch (message.Type)
        {
            case "grid-view":
                point = null;
                source = message.Text;
                image.Source = null;
                if (message.Grid is { } grid)
                {
                    try
                    {
                        using var stream = new MemoryStream(grid.Image, writable: false);
                        image.Source = BitmapFrame.Create(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
                    }
                    catch (Exception ex) when (ex is IOException or NotSupportedException or ArgumentException)
                    {
                        detail.Text = "This view could not be displayed.";
                        break;
                    }
                    viewKind = (source == "pc" ? "Main PC screen" : "Ubuntu") + " · " + grid.Kind;
                    detail.Text = viewKind;
                }
                else detail.Text = "Waiting for Vrex to inspect a screen.";
                break;
            case "grid-marker" when source == message.Text && image.Source != null:
                point = message.Point;
                bool click = message.X == 1;
                marker.Stroke = click ? Brushes.Gold : Brushes.DeepSkyBlue;
                detail.Text = viewKind + (click ? " · Click" : " · Vrex target") + (point == null ? " outside this view" : "");
                break;
            case "pc-mode" when source == "pc" && client is VrexClient { AllowPcScreen: false }:
            case "stopped":
                Changed(new("grid-view"));
                return;
            case "ubuntu-stopped" when source == "ubuntu":
                Changed(new("grid-view"));
                return;
            default: return;
        }
        dirty = true;
    }

    private bool IsClose(float x, float y) => new Rect(close.TranslatePoint(new Point(), surface), close.RenderSize)
        .Contains(new Point(x * ControllerMenu.Width, y * ControllerMenu.Height));

    public void Pointer(int kind, float x, float y, float value = 0)
    {
        if (Closed) return;
        bool hit = IsClose(x, y);
        if (kind is 0 or 4 or PanelFeedback.Leave) hover.Set(kind == 0 && hit ? close : null, () => dirty = true);
        if (kind == 1 && hit) { held = true; PanelFeedback.Down(close, () => dirty = true); }
        if (kind is not (2 or 4) || !held) return;
        held = false;
        if (kind == 2 && hit) Dispose();
        else PanelFeedback.Cancel(close, () => dirty = true);
    }

    public byte[] Render()
    {
        var size = new Size(ControllerMenu.Width, ControllerMenu.Height);
        surface.Measure(size); surface.Arrange(new Rect(size)); surface.UpdateLayout();
        marker.Visibility = Visibility.Collapsed;
        if (image.Source is BitmapSource frame && point is { } at)
        {
            double scale = Math.Min(viewport.ActualWidth / frame.PixelWidth, viewport.ActualHeight / frame.PixelHeight);
            double width = frame.PixelWidth * scale, height = frame.PixelHeight * scale;
            Canvas.SetLeft(marker, (viewport.ActualWidth - width) * .5 + at.X * width - marker.Width * .5);
            Canvas.SetTop(marker, (viewport.ActualHeight - height) * .5 + at.Y * height - marker.Height * .5);
            marker.Visibility = Visibility.Visible;
        }
        overlay.UpdateLayout();
        bitmap.Clear(); bitmap.Render(surface);
        bitmap.CopyPixels(pixels, ControllerMenu.PixelWidth * 4, 0);
        return pixels;
    }

    public void Published() { dirty = false; appearance = Appearance.Revision; }
    public void Dispose()
    {
        if (Closed) return;
        Closed = true;
        client.Changed -= Changed;
        image.Source = null;
        if (client is not VrexClient { Running: false }) client.Send(new("grid-viewer", Key: 0));
    }

    internal static void SmokeTest(IVrexClient client, Action<VrexEvent> emit)
    {
        using var panel = new FloatingVrexGridPanel(client);
        using var stream = new MemoryStream();
        var encoder = new PngBitmapEncoder();
        byte[] pixels = new byte[480 * 160 * 4];
        new Random(41).NextBytes(pixels);
        var frame = BitmapSource.Create(480, 160, 96, 96, PixelFormats.Bgra32, null, pixels, 480 * 4);
        encoder.Frames.Add(BitmapFrame.Create(frame));
        encoder.Save(stream);
        string line = JsonSerializer.Serialize(new VrexEvent("grid-view", "pc", Grid: new("grid", stream.ToArray())));
        if (line.Length <= VrexProtocol.MaxEventChars || VrexClient.DecodeWorkerEvent(line) is not { } decoded ||
            VrexClient.DecodeWorkerEvent(JsonSerializer.Serialize(new VrexEvent("reply", new string('x', VrexProtocol.MaxEventChars)))) != null)
            throw new InvalidOperationException("Large grid images need their own event allowance.");
        emit(decoded);
        emit(new("grid-marker", "ubuntu", X: 1, Point: new(.5, .25)));
        if (panel.point != null) throw new InvalidOperationException("A click from a different screen must not appear on this grid.");
        emit(new("grid-marker", "pc", X: 1, Point: new(.5, .25)));
        panel.Render();
        double expectedY = (panel.viewport.ActualHeight - panel.viewport.ActualWidth / 3) / 2 + panel.viewport.ActualWidth / 12;
        if (panel.marker.Visibility != Visibility.Visible || Math.Abs(Canvas.GetTop(panel.marker) + panel.marker.Height / 2 - expectedY) > .01)
            throw new InvalidOperationException("Grid clicks must line up with the letterboxed image.");
    }
}
