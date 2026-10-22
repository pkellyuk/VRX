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
    // A pick never carries a stereo method: the game's saved setting decides (true geometry
    // only when it is on for that game, in Expert).
    // ReviewSteamSetup: the true-geometry section's "Review Steam launch setup..." - show the
    // launch review and Steam's setup dialog again before a Steam geometry launch (never from
    // a tile here).
    // DepthOnly: Easy mode's pick - play with estimated depth now, whatever this game's saved
    // method is (and without changing it). True geometry is only reachable in Expert.
    // Restart: the main button's "Restart" (true geometry) for a running Steam game (never from a tile here:
    // running windows only attach).
    // Settings: a tile's Settings button - select this game for the Expert sections, without
    // starting VR.
    public sealed record Choice(RunningApp App, GameWindow Window, GameHistory.Entry? History = null, SteamLibrary.Game? Steam = null, bool ReviewSteamSetup = false, bool DepthOnly = false, bool Restart = false, bool Settings = false);

    // Expert shows each tile's Settings button (the way to change a game's true-geometry
    // setting); Easy shows tiles that just play with estimated depth.
    private readonly bool expert;

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

    // Three tiles to a row at the chooser's default 1000 width, with room for the scroll bar;
    // resizing it fits more or fewer to a row.
    private const double PictureWidth = 272, PictureHeight = 153;
    private const int SnapshotWidth = 560;
    private readonly List<Tile> tiles = [];
    private nint hwnd;

    // The size it was left at, for next time (AppSettings.ChooserWindow); set when it closes.
    public WindowPlace? Place { get; private set; }

    // backendFor: a game's saved stereo method; a stopped game whose method is true geometry
    // reads "Play with true geometry" in Expert.
    public ChooserWindow(IReadOnlyList<RunningApp> apps, IReadOnlyList<ScreenSource> screens, RunningApp? screenApp, nint currentWindow, WindowPlace? place = null, Func<RunningApp, string>? backendFor = null, IReadOnlyList<GameHistory.Entry>? history = null, IReadOnlyList<SteamLibrary.Game>? steamGames = null, string? dataRoot = null, string? steamWarning = null, bool expert = true)
    {
        ArgumentNullException.ThrowIfNull(apps);
        ArgumentNullException.ThrowIfNull(screens);
        // steamGames is null when Steam is not installed: its section is hidden then.
        this.expert = expert;
        System.Diagnostics.Debug.WriteLine($"[Chooser] enter: {apps.Count} app(s), {screens.Count} display(s), current {currentWindow:X}, place {place?.ToString() ?? "default"}, expert {expert}, Steam {(steamGames == null ? "not installed" : steamGames.Count + " game(s)")}");
        InitializeComponent();
        Title = Loc.Get("ChooserTitle");
        // Expert: the tiles' Settings buttons are how a game is chosen for its settings.
        if (expert) Intro.Text = Loc.Get("ChooserIntroExpert");
        // The remembered size, never bigger than the screen it opens on (it opens centred on VRX).
        if (place is { HasSize: true })
        {
            Width = Math.Clamp(place.Width, MinWidth, Math.Max(MinWidth, SystemParameters.WorkArea.Width));
            Height = Math.Clamp(place.Height, MinHeight, Math.Max(MinHeight, SystemParameters.WorkArea.Height));
        }
        bool maximize = place?.Maximized == true;
        foreach(var entry in history ?? []) {
            var app=apps.FirstOrDefault(a=>a.CanAttach && string.Equals(a.FullPath,entry.Executable,StringComparison.OrdinalIgnoreCase)) ??
                new RunningApp(0,System.IO.Path.GetFileName(entry.Executable),entry.Executable,[],RunningApps.IconFor(entry.Executable));
            var window=app.Windows.FirstOrDefault(RunningApps.IsChooserWindow) ?? new GameWindow(0,entry.Title);
            if(app.Pid>0) continue; // Running games belong only in Attach.
            HistoryTiles.Children.Add(MakeTile(new Choice(app,window,entry),null,false,SavedGeometry(backendFor,app)));
        }
        HistoryHeader.Visibility=HistoryTiles.Children.Count>0?Visibility.Visible:Visibility.Collapsed;
        foreach(var game in steamGames ?? []) {
            string key=SteamLibrary.ProfileKey(dataRoot!,game);
            var app=SteamLibrary.Running(game,apps) ?? new RunningApp(0,game.Title,key,[],System.IO.File.Exists(key)?RunningApps.IconFor(key):null);
            if(app.Pid>0) continue;
            var window=app.Windows.FirstOrDefault(RunningApps.IsChooserWindow) ?? new GameWindow(0,game.Title);
            SteamTiles.Children.Add(MakeTile(new Choice(app,window,Steam:game),null,false,SavedGeometry(backendFor,app)));
        }
        SteamHeader.Visibility=steamGames!=null?Visibility.Visible:Visibility.Collapsed;
        SteamHint.Visibility=SteamHeader.Visibility;
        SteamHint.Text=!string.IsNullOrEmpty(steamWarning)?Loc.Get("SteamDiscoveryPartial"):SteamTiles.Children.Count==0?Loc.Get(steamGames?.Count>0?"ChooserSteamAllRunning":"SteamNoGames"):Loc.Get(expert?"SteamLibraryHint":"SteamLibraryHintEasy");
        LaunchSection.Visibility=HistoryTiles.Children.Count>0 || steamGames!=null?Visibility.Visible:Visibility.Collapsed;

        // Windows, most recently used first (Windows' z-order), each with its app's icon. A
        // running window only attaches, with the game's saved method: geometry only when VRX
        // started it with geometry, else estimated depth (MainWindow.StartSession decides).
        var order = RunningApps.ZOrder();
        var windows = apps.Where(a => a.CanAttach && !ScreenSources.IsScreen(a))
            .SelectMany(a => a.Windows.Where(RunningApps.IsChooserWindow).Select(w => new Choice(a, w)))
            .OrderBy(c => order.GetValueOrDefault(c.Window.Handle, int.MaxValue)).ToList();
        foreach (var choice in windows)
            WindowTiles.Children.Add(MakeTile(choice, null, choice.Window.Handle == currentWindow));
        LaunchSection.IsExpanded=windows.Count==0;
        NoWindows.Visibility = windows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        if(HistoryTiles.Children.Count>0 || SteamTiles.Children.Count>0) NoWindows.Text=Loc.Get("ChooserNoWindowsHistory");

        if (screenApp != null)
            foreach (var screen in screens)
            {
                var window = screenApp.Windows.FirstOrDefault(w => w.Monitor == screen.Index);
                if (window == null) continue;
                ScreenTiles.Children.Add(MakeTile(new Choice(screenApp, window), screen, window.Handle == currentWindow));
            }
        ScreensHeader.Visibility = ScreenTiles.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        ScreensHint.Visibility=ScreensHeader.Visibility;

        // The display pictures are taken now, before the chooser is on screen, so it is not in them.
        RefreshSnapshots();
        SourceInitialized += (_, _) => { Attach(); if (maximize) WindowState = WindowState.Maximized; };
        StateChanged += (_, _) => UpdateFrame();
        Closing += (_, _) => RememberPlace();
        LayoutUpdated += (_, _) => PlaceThumbnails();
        Scroll.ScrollChanged += (_, _) => PlaceThumbnails();
        Closed += (_, _) => Detach();
        KeyDown += (_, e) => { if (e.Key == Key.Escape) Close(); };
        System.Diagnostics.Debug.WriteLine($"[Chooser] exit ctor: {windows.Count} window tile(s), {ScreenTiles.Children.Count} display tile(s)");
    }

    // The number of tiles, for the smoke test.
    public int WindowTileCount => WindowTiles.Children.Count;
    public int HistoryTileCount => HistoryTiles.Children.Count;
    public int SteamTileCount => SteamTiles.Children.Count;
    public int ScreenTileCount => ScreenTiles.Children.Count;
    public bool SteamSectionShown => SteamHeader.Visibility == Visibility.Visible;

    // Whether a game's saved stereo method is true geometry (Auto and Depth are estimated depth).
    private static bool SavedGeometry(Func<RunningApp, string>? backendFor, RunningApp app)
    {
        ArgumentNullException.ThrowIfNull(app);
        if (backendFor == null) return false;
        string backend = backendFor(app) ?? StereoPaths.Auto;
        bool geometry = StereoPaths.Mode(backend) == StereoPaths.GeometryAuto;
        System.Diagnostics.Debug.WriteLine($"[Chooser] SavedGeometry: {app.FullPath} saved '{backend}' -> geometry {geometry}");
        return geometry;
    }

    // savedGeometry: the stopped game's saved setting is true geometry (its caption says so in
    // Expert; the launch itself follows the saved setting, with the warning first).
    private FrameworkElement MakeTile(Choice choice, ScreenSource? screen, bool current, bool savedGeometry = false)
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
        string title = screen != null ? screen.Title : choice.Steam?.Title ?? choice.History?.Title ?? MainWindow.GameDisplayName(choice.Window.Title, choice.App.Name);
        string detail = screen != null ? (screen.Primary ? Loc.Get("ChooserMainDisplay") : Loc.Get("WholeScreen")) :
            RunningApps.IsMinimized(choice.Window.Handle) ? Loc.Format("ChooserMinimized", choice.App.Name) : choice.App.Name;
        bool missing=choice.History!=null && !System.IO.File.Exists(choice.App.FullPath);
        // Running games appear only under Attach, so these tiles are always stopped games.
        if(choice.History!=null) detail=Loc.Get(missing?"HistoryMissing":"HistoryLaunch");
        if(choice.Steam!=null && choice.App.Pid==0) detail=Loc.Get("SteamLaunch");
        var caption = new StackPanel { Width = PictureWidth, Margin = new Thickness(0, 8, 0, 0) };
        caption.Children.Add(new TextBlock { Text = title, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis, ToolTip = title });
        caption.Children.Add(new TextBlock { Text = detail, FontSize = 12, Foreground = (Brush)FindResource("Muted"), TextTrimming = TextTrimming.CharacterEllipsis });
        bool launches=screen==null && choice.App.Pid==0 && (choice.History!=null || choice.Steam!=null);
        // Expert: a stopped game whose saved method is true geometry launches with it.
        bool launchesGeometry=launches && expert && savedGeometry;
        string action=Loc.Get(screen!=null?"ChooserAttachDisplay":launchesGeometry?"ChooserLaunchGeometryAction":launches?"ChooserLaunchAction":"ChooserAttachAction");
        caption.Children.Add(new TextBlock {Text=action,Foreground=(Brush)FindResource("Accent"),FontWeight=FontWeights.SemiBold,Margin=new Thickness(0,8,0,0)});
        var content = new StackPanel();
        content.Children.Add(picture);
        content.Children.Add(caption);
        var button = new Button { Style = (Style)FindResource("Tile"), Content = content, Tag = current ? "Current" : null };
        if(missing) { button.IsEnabled=false; button.ToolTip=Loc.Get("ErrorHistoryMissing"); }
        System.Windows.Automation.AutomationProperties.SetName(button, action+": "+title);
        // Easy: a game or window plays with estimated depth now. Expert: a running window
        // attaches and a stopped game launches the way it is set up - true geometry only when
        // its saved setting says so (StartFromHistory / StartFromSteam check for anti-cheat and
        // show its warning, else the red warning, first), else estimated depth. The tile's ⚙
        // changes that.
        if (!expert)
            button.Click += (_, _) => Pick(screen==null?choice with {DepthOnly=true}:choice);
        else
            button.Click += (_, _) => Pick(choice);
        tiles.Add(new Tile { Choice = choice, Picture = picture, Screen = screen, Snapshot = snapshot });
        System.Diagnostics.Debug.WriteLine($"[Chooser] MakeTile: '{title}', launches {launches}, geometry caption {launchesGeometry}, missing {missing}, expert {expert}");
        if (!expert) return button;
        // Separate sibling buttons: input controls must never sit inside a tile Button. The
        // Settings button sits over the tile's caption (a window's live picture is drawn by DWM
        // above everything in the picture area).
        var holder = new Grid { Margin = button.Margin };
        button.Margin = new Thickness(0);
        holder.Children.Add(button);
        holder.Children.Add(SettingsButton(choice, title));
        return holder;
    }

    // Expert: a tile's small Settings (gear) button. It selects the tile's game, window or
    // display for VRX's settings without starting VR (MainWindow.ShowSettingsFor).
    private Button SettingsButton(Choice choice, string title)
    {
        ArgumentNullException.ThrowIfNull(choice);
        ArgumentNullException.ThrowIfNull(title);
        var gear = new Button
        {
            Content = Loc.Get("ChooserSettingsSymbol"), Tag = SettingsTag, ToolTip = Loc.Get("ChooserSettingsTip"), FontSize = 15,
            Padding = new Thickness(8, 1, 8, 1), MinHeight = 0, MinWidth = 0, Cursor = Cursors.Hand,
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 10, 10),
        };
        System.Windows.Automation.AutomationProperties.SetName(gear, Loc.Format("ChooserSettingsFor", title));
        gear.Click += (_, _) => Pick(choice with { ReviewSteamSetup = false, DepthOnly = false, Restart = false, Settings = true });
        System.Diagnostics.Debug.WriteLine($"[Chooser] SettingsButton: {choice.App.FullPath} '{title}'");
        return gear;
    }

    // The Settings buttons' Tag (tests find them by it; never a stereo method).
    public const string SettingsTag = "Settings";

    private void Pick(Choice choice)
    {
        System.Diagnostics.Debug.WriteLine($"[Chooser] picked {choice.App.Name} '{choice.Window.Title}' monitor {choice.Window.Monitor}, recent {choice.History != null}, Steam {choice.Steam?.AppId ?? "-"}, depth only {choice.DepthOnly}, settings only {choice.Settings}");
        Picked = choice;
        DialogResult = true;
    }

    private void CloseClick(object sender, RoutedEventArgs e) => Close();

    private void MaximizeClick(object sender, RoutedEventArgs e)
    {
        if (WindowState == WindowState.Maximized) SystemCommands.RestoreWindow(this);
        else SystemCommands.MaximizeWindow(this);
    }

    // Maximized, Windows pushes the frame past the screen's edges: pad the content back in,
    // drop the edge line, and turn the maximize glyph into restore.
    private void UpdateFrame()
    {
        bool maximized = WindowState == WindowState.Maximized;
        Frame.BorderThickness = new Thickness(maximized ? 0 : 1);
        Frame.Padding = maximized ? WindowPlacement.MaximizedOverhang(this) : new Thickness(0);
        MaxGlyph.Data = maximized ? MainWindow.RestoreGlyph : MainWindow.MaximizeGlyph;
        string name = Loc.Get(maximized ? "TitleRestore" : "TitleMaximize");
        MaxButton.ToolTip = name;
        System.Windows.Automation.AutomationProperties.SetName(MaxButton, name);
        System.Diagnostics.Debug.WriteLine($"[Chooser] UpdateFrame: maximized {maximized}, padding {Frame.Padding}");
    }

    private void RememberPlace()
    {
        Rect bounds = WindowState == WindowState.Normal ? new Rect(Left, Top, ActualWidth, ActualHeight) : RestoreBounds;
        Place = AppSettings.ValidSize(new WindowPlace { Width = bounds.Width, Height = bounds.Height, Maximized = WindowState == WindowState.Maximized });
        System.Diagnostics.Debug.WriteLine($"[Chooser] RememberPlace: {Place?.ToString() ?? "none"}");
    }

    // Once the window exists: ask Windows for each window's live picture.
    private void Attach()
    {
        hwnd = new WindowInteropHelper(this).Handle;
        System.Diagnostics.Debug.WriteLine($"[Chooser] Attach enter: hwnd {hwnd:X}");
        foreach (var tile in tiles.Where(t => t.Screen == null))
        {
            if(tile.Choice.Window.Handle==0) continue;
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
            if(!tile.Picture.IsVisible) {
                if(tile.Visible) {
                    var hidden=new Native.ThumbnailProperties {Flags=Native.ThumbVisible,Visible=false};
                    Native.DwmUpdateThumbnailProperties(tile.Thumbnail,ref hidden);tile.Visible=false;
                }
                continue;
            }
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
