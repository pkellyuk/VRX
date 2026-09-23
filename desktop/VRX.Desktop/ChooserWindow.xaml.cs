using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;

namespace VRX.Desktop;

// "Choose...": every window VRX can play and every whole display, as tiles with live
// pictures - the window's own, drawn live by Windows (DWM thumbnails, as in Alt+Tab), or a
// snapshot of the display taken as the chooser opens. Clicking a tile picks it.
public sealed partial class ChooserWindow : Window
{
    public sealed record Choice(RunningApp App, GameWindow Window);

    // What was picked; null when the chooser was closed without a pick.
    public Choice? Picked { get; private set; }

    private sealed class Tile
    {
        public required Choice Choice;
        public required FrameworkElement Picture;   // where the window's picture is drawn
        public ScreenSource? Screen;                 // a display: a snapshot instead
        public Image? Snapshot;
        public nint Thumbnail;                       // the window's DWM thumbnail (0: none)
        public int SourceWidth, SourceHeight;
        public Native.Rect Shown;                    // last destination given to DWM
        public bool Visible;
    }

    // Three tiles to a row in the 1000-wide chooser, with room for the scroll bar.
    private const double PictureWidth = 272, PictureHeight = 153;
    private const int SnapshotWidth = 560;
    private readonly List<Tile> tiles = [];
    private nint hwnd;

    public ChooserWindow(IReadOnlyList<RunningApp> apps, IReadOnlyList<ScreenSource> screens, RunningApp? screenApp, nint currentWindow)
    {
        ArgumentNullException.ThrowIfNull(apps);
        ArgumentNullException.ThrowIfNull(screens);
        System.Diagnostics.Debug.WriteLine($"[Chooser] enter: {apps.Count} app(s), {screens.Count} display(s), current {currentWindow:X}");
        InitializeComponent();
        Title = Loc.Get("ChooserTitle");

        // Windows, most recently used first (Windows' z-order), each with its app's icon.
        var order = RunningApps.ZOrder();
        var windows = apps.Where(a => a.CanAttach && !ScreenSources.IsScreen(a))
            .SelectMany(a => a.Windows.Where(RunningApps.IsChooserWindow).Select(w => new Choice(a, w)))
            .OrderBy(c => order.GetValueOrDefault(c.Window.Handle, int.MaxValue)).ToList();
        foreach (var choice in windows)
            WindowTiles.Children.Add(MakeTile(choice, null, choice.Window.Handle == currentWindow));
        NoWindows.Visibility = windows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

        if (screenApp != null)
            foreach (var screen in screens)
            {
                var window = screenApp.Windows.FirstOrDefault(w => w.Monitor == screen.Index);
                if (window == null) continue;
                ScreenTiles.Children.Add(MakeTile(new Choice(screenApp, window), screen, window.Handle == currentWindow));
            }
        ScreensHeader.Visibility = ScreenTiles.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;

        // The display pictures are taken now, before the chooser is on screen, so it is not in them.
        RefreshSnapshots();
        SourceInitialized += (_, _) => Attach();
        LayoutUpdated += (_, _) => PlaceThumbnails();
        Scroll.ScrollChanged += (_, _) => PlaceThumbnails();
        Closed += (_, _) => Detach();
        KeyDown += (_, e) => { if (e.Key == Key.Escape) Close(); };
        System.Diagnostics.Debug.WriteLine($"[Chooser] exit ctor: {windows.Count} window tile(s), {ScreenTiles.Children.Count} display tile(s)");
    }

    // The number of tiles, for the smoke test.
    public int WindowTileCount => WindowTiles.Children.Count;
    public int ScreenTileCount => ScreenTiles.Children.Count;

    private Button MakeTile(Choice choice, ScreenSource? screen, bool current)
    {
        ArgumentNullException.ThrowIfNull(choice);
        var picture = new Grid { Width = PictureWidth, Height = PictureHeight, Background = (Brush)FindResource("Well") };
        // Behind the live picture (and in its place when there is none, such as a minimized
        // window): the app's icon, large.
        var icon = choice.App.Icon ?? (screen != null ? ScreenSources.Icon : null);
        if (icon != null) picture.Children.Add(new Image { Source = icon, Width = 48, Height = 48, Opacity = 0.55 });
        Image? snapshot = null;
        if (screen != null)
        {
            snapshot = new Image { Stretch = Stretch.Uniform };
            RenderOptions.SetBitmapScalingMode(snapshot, BitmapScalingMode.HighQuality);
            picture.Children.Add(snapshot);
        }
        string title = screen != null ? screen.Title : MainWindow.GameDisplayName(choice.Window.Title, choice.App.Name);
        string detail = screen != null ? (screen.Primary ? Loc.Get("ChooserMainDisplay") : Loc.Get("WholeScreen")) :
            RunningApps.IsMinimized(choice.Window.Handle) ? Loc.Format("ChooserMinimized", choice.App.Name) : choice.App.Name;
        var caption = new StackPanel { Width = PictureWidth, Margin = new Thickness(0, 8, 0, 0) };
        caption.Children.Add(new TextBlock { Text = title, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis, ToolTip = title });
        caption.Children.Add(new TextBlock { Text = detail, FontSize = 12, Foreground = (Brush)FindResource("Muted"), TextTrimming = TextTrimming.CharacterEllipsis });
        var content = new StackPanel();
        content.Children.Add(picture);
        content.Children.Add(caption);
        var button = new Button { Style = (Style)FindResource("Tile"), Content = content, Tag = current ? "Current" : null };
        System.Windows.Automation.AutomationProperties.SetName(button, title);
        button.Click += (_, _) => Pick(choice);
        tiles.Add(new Tile { Choice = choice, Picture = picture, Screen = screen, Snapshot = snapshot });
        return button;
    }

    private void Pick(Choice choice)
    {
        System.Diagnostics.Debug.WriteLine($"[Chooser] picked {choice.App.Name} '{choice.Window.Title}' monitor {choice.Window.Monitor}");
        Picked = choice;
        DialogResult = true;
    }

    private void CloseClick(object sender, RoutedEventArgs e) => Close();

    // Once the window exists: ask Windows for each window's live picture.
    private void Attach()
    {
        hwnd = new WindowInteropHelper(this).Handle;
        System.Diagnostics.Debug.WriteLine($"[Chooser] Attach enter: hwnd {hwnd:X}");
        foreach (var tile in tiles.Where(t => t.Screen == null))
        {
            if (RunningApps.IsMinimized(tile.Choice.Window.Handle)) continue;   // no picture; the icon stays
            if (Native.DwmRegisterThumbnail(hwnd, tile.Choice.Window.Handle, out tile.Thumbnail) != 0) { tile.Thumbnail = 0; continue; }
            if (Native.DwmQueryThumbnailSourceSize(tile.Thumbnail, out var size) != 0 || size.X <= 0 || size.Y <= 0)
            {
                Native.DwmUnregisterThumbnail(tile.Thumbnail); tile.Thumbnail = 0; continue;
            }
            tile.SourceWidth = size.X; tile.SourceHeight = size.Y;
        }
        System.Diagnostics.Debug.WriteLine($"[Chooser] Attach exit: {tiles.Count(t => t.Thumbnail != 0)} live window picture(s)");
    }

    private void Detach()
    {
        foreach (var tile in tiles.Where(t => t.Thumbnail != 0)) { Native.DwmUnregisterThumbnail(tile.Thumbnail); tile.Thumbnail = 0; }
        System.Diagnostics.Debug.WriteLine("[Chooser] closed");
    }

    private void RefreshSnapshots()
    {
        foreach (var tile in tiles.Where(t => t.Screen != null && t.Snapshot != null))
            tile.Snapshot!.Source = ScreenSources.Snapshot(tile.Screen!, SnapshotWidth);
    }

    // Windows draws each thumbnail over the chooser at a rectangle in device pixels: fit the
    // window's picture into its tile, and cut it to the scrolling area so it scrolls away
    // under the title and edges like the tile itself.
    private void PlaceThumbnails()
    {
        if (hwnd == 0 || !IsLoaded) return;
        var source = PresentationSource.FromVisual(this);
        if (source?.CompositionTarget == null) return;
        var toDevice = source.CompositionTarget.TransformToDevice;
        Rect viewport = Scroll.TransformToAncestor(this).TransformBounds(new Rect(0, 0, Scroll.ViewportWidth, Scroll.ViewportHeight));
        viewport = Rect.Transform(viewport, toDevice);
        foreach (var tile in tiles.Where(t => t.Thumbnail != 0))
        {
            Rect box = Rect.Transform(tile.Picture.TransformToAncestor(this).TransformBounds(new Rect(0, 0, PictureWidth, PictureHeight)), toDevice);
            double scale = Math.Min(box.Width / tile.SourceWidth, box.Height / tile.SourceHeight);
            double width = tile.SourceWidth * scale, height = tile.SourceHeight * scale;
            var fitted = new Rect(box.X + (box.Width - width) / 2, box.Y + (box.Height - height) / 2, width, height);
            Rect shown = Rect.Intersect(fitted, viewport);
            var props = new Native.ThumbnailProperties { Flags = Native.ThumbVisible };
            if (shown.IsEmpty || shown.Width < 1 || shown.Height < 1)
            {
                if (!tile.Visible) continue;
                props.Visible = false; tile.Visible = false;
                Native.DwmUpdateThumbnailProperties(tile.Thumbnail, ref props);
                continue;
            }
            var destination = Native.Rect.From(shown);
            if (tile.Visible && destination.Equals(tile.Shown)) continue;
            // The part of the window that is still showing, in the window's own pixels.
            var cut = new Rect((shown.X - fitted.X) / scale, (shown.Y - fitted.Y) / scale, shown.Width / scale, shown.Height / scale);
            props.Flags = Native.ThumbVisible | Native.ThumbDestination | Native.ThumbSource | Native.ThumbOpacity;
            props.Visible = true;
            props.Opacity = 255;
            props.Destination = destination;
            props.Source = Native.Rect.From(cut);
            if (Native.DwmUpdateThumbnailProperties(tile.Thumbnail, ref props) == 0) { tile.Shown = destination; tile.Visible = true; }
        }
    }

    private static class Native
    {
        [StructLayout(LayoutKind.Sequential)] public struct Size { public int X, Y; }
        [StructLayout(LayoutKind.Sequential)] public record struct Rect(int Left, int Top, int Right, int Bottom)
        {
            public static Rect From(System.Windows.Rect r) => new((int)Math.Round(r.Left), (int)Math.Round(r.Top), (int)Math.Round(r.Right), (int)Math.Round(r.Bottom));
        }
        [StructLayout(LayoutKind.Sequential)] public struct ThumbnailProperties
        {
            public uint Flags; public Rect Destination; public Rect Source; public byte Opacity;
            [MarshalAs(UnmanagedType.Bool)] public bool Visible;
            [MarshalAs(UnmanagedType.Bool)] public bool SourceClientAreaOnly;
        }
        public const uint ThumbDestination = 0x1, ThumbSource = 0x2, ThumbOpacity = 0x4, ThumbVisible = 0x8;
        [DllImport("dwmapi.dll")] public static extern int DwmRegisterThumbnail(nint destination, nint source, out nint thumbnail);
        [DllImport("dwmapi.dll")] public static extern int DwmUnregisterThumbnail(nint thumbnail);
        [DllImport("dwmapi.dll")] public static extern int DwmQueryThumbnailSourceSize(nint thumbnail, out Size size);
        [DllImport("dwmapi.dll")] public static extern int DwmUpdateThumbnailProperties(nint thumbnail, ref ThumbnailProperties properties);
    }
}
