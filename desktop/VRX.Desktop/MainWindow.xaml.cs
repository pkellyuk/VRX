using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Automation;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shell;
using System.Windows.Threading;

namespace VRX.Desktop;
public partial class MainWindow : Window
{
    private readonly ProfileStore store;
    private readonly EngineSession engine = new();
    private readonly DispatcherTimer saveTimer = new() { Interval = TimeSpan.FromMilliseconds(180) };
    private readonly DispatcherTimer refreshTimer = new() { Interval = TimeSpan.FromSeconds(5) };
    private Profile? profile;
    private bool ready, loading, refreshing, stopping, closing, dragging;
    private string lastExecutable = "";
    private string sessionError = "", lastEngineError = "";
    private readonly bool smoke;
    // Auto-attach (app-wide, AutoAttach.cs): polls the window in front twice a second.
    private readonly DispatcherTimer autoTimer = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private readonly AutoAttachMachine autoMachine = new();
    private readonly System.Diagnostics.Stopwatch autoClock = System.Diagnostics.Stopwatch.StartNew();
    private AppSettings appSettings = new();
    private AutoAttachOverlay? autoOverlay;
    private ForegroundSnapshot? autoTarget;
    private nint sessionWindow;                    // the window of the running session, for "spent"
    private string autoSavedStatus = "", autoStatus = "";
    private bool autoSettingsLoading;
    // Easy | Expert and the sections' open/closed state (app-wide, AppSettings).
    private bool modeLoading, sectionsLoading;
    private bool playing;                          // the engine has entered its frame loop
    private IReadOnlyList<string> gpuLines = [];   // the engine's --list-gpus output, re-labelled when the strings change
    // The window: which mode it is laid out for (-1 before the first), whether Windows 11
    // draws its border, and whether it goes back to maximized after being minimized.
    private int placedMode = -1;
    private bool dwmFrame, restoreMaximized;
    private sealed record Shortcut(string Name, int Code);
    public MainWindow(bool smokeTest)
    {
        smoke = smokeTest;
        string data = smoke ? Path.Combine(EngineSession.RepositoryRoot(), "desktop", "out", "smoke-data") :
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VRX");
        store = new(data);
        // Bound numbers ("5.70 m") in the user's format; profiles and the engine stay invariant.
        Language = XmlLanguage.GetLanguage(CultureInfo.CurrentCulture.IetfLanguageTag);
        InitializeComponent();
        System.Diagnostics.Debug.WriteLine($"[MainWindow] UI culture {CultureInfo.CurrentUICulture.Name}, format culture {CultureInfo.CurrentCulture.Name}, pseudo {Loc.Pseudo}");
        FillWorldList();
        FillLightColourList(Profile.DefaultRoomLightColor);
        WorldHex.Text = WorldPresets[0].Hex;           // data, not text: kept out of the XAML
        WorldList.SelectedIndex = 0;
        ShowGpuChoice(Gpus.Same);
        // From <Version> in the project file, so the UI always matches the build.
        VersionText.Text = DisplayVersion();
        TitleVersion.Text = VersionText.Text.TrimStart('v');
        Title = Loc.Format("WindowTitle", VersionText.Text);
        if (smoke)
        {
            ShowActivated = false; ShowInTaskbar = false;
            WindowStartupLocation = WindowStartupLocation.Manual; Left = -20000; Top = -20000;
        }
        FillShortcutKeys();
        RecenterKeys.SelectedValue = 0xBB; MenuKeys.SelectedValue = 0x77;
        saveTimer.Tick += (_, _) => { saveTimer.Stop(); SaveAndApply(); };
        refreshTimer.Tick += (_, _) => RefreshApps();
        engine.Log += line => Dispatcher.InvokeAsync(() =>
        {
            LogBox.AppendText(line + Environment.NewLine);
            if (LogBox.Text.Length > 24000) LogBox.Text = LogBox.Text[^16000..];
            LogBox.ScrollToEnd();
            if (line.Contains("FAIL") || line.Contains("expected one game window") || line.Contains("main: failure"))
                lastEngineError = line;
            if (line.Contains("RunFrameLoop: enter"))
            {
                playing = true;
                Status.Text = Loc.Format("StatusPlaying", Path.GetFileName(profile?.ExecutablePath));
                UpdateGameCard();
            }
        });
        engine.Exited += code => Dispatcher.InvokeAsync(() =>
        {
            // No loops: the session's window is not auto-attached again until it has left
            // the foreground and come back.
            autoMachine.MarkSpent(sessionWindow);
            sessionWindow = 0;
            playing = false;
            // The renderer's own reason stays English (engine logs are not translated).
            sessionError = code == 0 ? "" : Loc.Format("StatusEngineFailed",
                lastEngineError.Length > 0 ? lastEngineError[(lastEngineError.LastIndexOf(']') + 1)..].Trim() : Loc.Get("SeeSessionDetails"));
            UpdateButtons(); RefreshApps();
            Status.Text = code == 0 ? Loc.Get("StatusStopped") : sessionError;
            if (code != 0) ShowSessionDetails();
        });
        try { string last = Path.Combine(store.Root, "last-game.txt"); if (File.Exists(last)) lastExecutable = File.ReadAllText(last); }
        catch (IOException) { }
        appSettings = store.LoadAppSettings();
        PutAppSettings();
        autoTimer.Tick += (_, _) => AutoAttachTick();
        Closed += (_, _) => { autoTimer.Stop(); autoOverlay?.Close(); autoOverlay = null; };
        // A remembered place on a monitor that has gone (or changed) is brought back on screen.
        ContentRendered += (_, _) => KeepOnScreenSoon();
        ready = true;
        Loaded += async (_, _) =>
        {
            RefreshApps();
            if (smoke)
            {
                try { await SmokeTest(); Application.Current.Shutdown(0); }
                catch (Exception ex)
                {
                    Directory.CreateDirectory(Path.Combine(EngineSession.RepositoryRoot(), "desktop", "out"));
                    File.WriteAllText(Path.Combine(EngineSession.RepositoryRoot(), "desktop", "out", "smoke-error.txt"), ex.ToString());
                    Application.Current.Shutdown(1);
                }
            }
            else { refreshTimer.Start(); UpdateAutoTimer(); await LoadGpusAsync(); }
        };
    }
    private void RefreshApps()
    {
        if (!ready || engine.Running || refreshing) return;
        var old = AppList.SelectedItem as RunningApp;
        nint oldWindow = (WindowList.SelectedItem as GameWindow)?.Handle ?? 0;
        try
        {
            refreshing = true;
            var list = RunningApps.List(ShowAll.IsChecked == true);
            AppList.ItemsSource = list;
            var selected = list.FirstOrDefault(a => old != null && a.Pid == old.Pid && a.FullPath == old.FullPath) ??
                (old == null ? list.FirstOrDefault(a => string.Equals(a.FullPath, lastExecutable, StringComparison.OrdinalIgnoreCase)) : null);
            AppList.SelectedItem = selected;
            if (selected != null && old != null && selected.Pid == old.Pid && selected.FullPath == old.FullPath)
            {
                WindowList.ItemsSource = selected.Windows;
                WindowList.SelectedItem = selected.Windows.FirstOrDefault(w => w.Handle == oldWindow) ??
                    (selected.Windows.Count == 1 ? selected.Windows[0] : null);
            }
            else { refreshing = false; LoadSelected(); }
        }
        catch (Exception ex) { Status.Text = Loc.Format("StatusRefreshFailed", ex.Message); }
        finally { refreshing = false; UpdateButtons(); if (sessionError.Length > 0) Status.Text = sessionError; }
    }
    private void AppSelected(object sender, SelectionChangedEventArgs e)
    {
        if (ready && !refreshing) { sessionError = ""; LoadSelected(); }
    }
    private void LoadSelected()
    {
        // Only a pending change is saved: just looking at a game must not create a profile
        // for it (auto-attach treats a game with saved settings as one set up before).
        if (saveTimer.IsEnabled) SaveAndApply();
        profile = null; SetSettingsEnabled(false);
        var app = AppList.SelectedItem as RunningApp;
        WindowList.ItemsSource = app?.Windows;
        WindowList.SelectedIndex = app?.Windows.Count > 0 ? 0 : -1;
        PathLabel.Text = app?.FullPath.Length > 0 ? app.FullPath : Loc.Get("PathNoAccess");
        if (app?.CanAttach == true)
        {
            try
            {
                profile = store.Load(app.FullPath);
                PutProfile(profile);
                loading = true;
                WindowList.SelectedItem = app.Windows.FirstOrDefault(w => profile.PreferredWindowTitle.Length > 0 && w.Title == profile.PreferredWindowTitle) ??
                    (app.Windows.Count == 1 ? app.Windows[0] : null);
                loading = false;
                lastExecutable = app.FullPath;
                ProfileStore.AtomicWrite(Path.Combine(store.Root, "last-game.txt"), lastExecutable);
                Status.Text = Loc.Format(WindowList.SelectedItem == null ? "StatusChooseWindow" : "StatusReadyFor", app.Name);
            }
            catch (Exception ex) { Status.Text = ex.Message; profile = null; SetSettingsEnabled(false); }
        }
        UpdateButtons();
    }
    // "v1.2" for 1.2.0, "v1.2.3" for a patch release.
    public static string DisplayVersion()
    {
        var v = typeof(MainWindow).Assembly.GetName().Version;
        if (v == null) return "";
        return v.Build > 0 ? $"v{v.Major}.{v.Minor}.{v.Build}" : $"v{v.Major}.{v.Minor}";
    }

    private IReadOnlyList<GpuChoice> gpuChoices = Gpus.Parse([]);

    // Replaces the Depth GPU list from the engine's --list-gpus lines (e.g. once the engine
    // has listed the GPUs, or when the strings change), keeping the current selection.
    private void SetGpuLines(IEnumerable<string> lines)
    {
        ArgumentNullException.ThrowIfNull(lines);
        gpuLines = lines.ToArray();
        string current = DepthGpuList.SelectedValue as string ?? profile?.DepthGpu ?? Gpus.Same;
        gpuChoices = Gpus.Parse(gpuLines);
        System.Diagnostics.Debug.WriteLine($"[MainWindow] SetGpuLines: {gpuChoices.Count} choice(s), keeping {current}");
        ShowGpuChoice(current);
    }

    // Selects `id`. A saved GPU that is not in this PC gets a "Not found" entry, so the
    // choice stays visible rather than silently changing.
    private void ShowGpuChoice(string id)
    {
        if (!Gpus.ValidId(id)) id = Gpus.Same;
        var list = gpuChoices.ToList();
        if (!list.Any(c => c.Id == id)) list.Add(new GpuChoice(id, Loc.Format("GpuNotFound", Gpus.NameOf(id))));
        bool wasLoading = loading;
        loading = true;
        DepthGpuList.ItemsSource = list;
        DepthGpuList.SelectedValue = id;
        loading = wasLoading;
    }

    private async Task LoadGpusAsync()
    {
        var lines = await Task.Run(Gpus.EngineLines);
        SetGpuLines(lines);
    }

    private void PutProfile(Profile p)
    {
        loading = true;
        WidthSlider.Value = p.Width; DistanceSlider.Value = p.Distance; HeightSlider.Value = p.Height;
        HorizontalSlider.Value = p.Horizontal; StrengthSlider.Value = p.Strength;
        FollowCheck.IsChecked = p.Follow; StereoCheck.IsChecked = p.Stereo; DismissCheck.IsChecked = p.AutoDismiss;
        ForegroundCheck.IsChecked = p.ForegroundRefinement;
        TimingList.SelectedIndex = p.MatchFrameToDepth ? 2 : p.DelayToDepth ? 1 : 0;
        FastModelCheck.IsChecked = p.FastDepthModel;
        SubpixelCheck.IsChecked = p.SubpixelWarp;
        CurveSlider.Value = p.ScreenCurve;
        AmbilightCheck.IsChecked = p.Ambilight;
        AmbiStrengthSlider.Value = p.AmbilightStrength;
        RoomSlider.Value = p.Room;
        GlassSlider.Value = p.RoomGlass;
        ReflectSlider.Value = p.RoomReflections;
        LightSlider.Value = p.RoomLight;
        FillLightColourList(p.RoomLightColor);
        WorldHex.Text = p.WorldColor;
        SteadyCheck.IsChecked = p.SteadyDepth;
        FuseCheck.IsChecked = p.FuseModels;
        ShowGpuChoice(p.DepthGpu);
        RecenterKeys.SelectedValue = p.RecenterKey; MenuKeys.SelectedValue = p.MenuKey;
        SetSettingsEnabled(true);
        loading = false; UpdateRoomControls(); DrawPreview();
    }
    private Profile ReadProfile()
    {
        if (profile == null) throw new InvalidOperationException(Loc.Get("ErrorSelectGame"));
        var p = new Profile { ExecutablePath = profile.ExecutablePath,
            PreferredWindowTitle = (WindowList.SelectedItem as GameWindow)?.Title ?? profile.PreferredWindowTitle, Width = WidthSlider.Value,
            Distance = DistanceSlider.Value, Height = HeightSlider.Value, Horizontal = HorizontalSlider.Value,
            Strength = StrengthSlider.Value, Follow = FollowCheck.IsChecked == true, Stereo = StereoCheck.IsChecked == true,
            ForegroundRefinement = ForegroundCheck.IsChecked == true,
            MatchFrameToDepth = TimingList.SelectedIndex == 2,
            DelayToDepth = TimingList.SelectedIndex == 1,
            FastDepthModel = FastModelCheck.IsChecked == true,
            SubpixelWarp = SubpixelCheck.IsChecked == true,
            ScreenCurve = (int)Math.Round(CurveSlider.Value),
            Ambilight = AmbilightCheck.IsChecked == true,
            AmbilightStrength = (int)Math.Round(AmbiStrengthSlider.Value),
            Room = (int)Math.Round(RoomSlider.Value),
            RoomGlass = (int)Math.Round(GlassSlider.Value),
            RoomReflections = (int)Math.Round(ReflectSlider.Value),
            RoomLight = (int)Math.Round(LightSlider.Value),
            RoomLightColor = SelectedLightColour(),
            WorldColor = Profile.TryParseColor(WorldHex.Text, out int world) ? Profile.FormatColor(world) :
                throw new InvalidDataException(Loc.Get("ErrorWorldColour")),
            SteadyDepth = SteadyCheck.IsChecked == true,
            FuseModels = FuseCheck.IsChecked == true,
            DepthGpu = DepthGpuList.SelectedValue as string ?? Gpus.Same,
            AutoDismiss = DismissCheck.IsChecked == true, RecenterKey = (int)(RecenterKeys.SelectedValue ?? 0),
            MenuKey = (int)(MenuKeys.SelectedValue ?? 0) };
        if (!p.Valid()) throw new InvalidDataException(Loc.Get("ErrorShortcutsConflict"));
        return p;
    }
    private bool SaveAndApply()
    {
        saveTimer.Stop();
        if (!ready || loading || stopping || profile == null) return false;
        try
        {
            var p = ReadProfile(); store.Save(p); profile = p;
            if (engine.Running) engine.Update(p);
            if (sessionError.Length == 0)
                Status.Text = Loc.Format(engine.Running ? "StatusSavedPlaying" : "StatusSaved", Path.GetFileName(p.ExecutablePath));
            return true;
        }
        catch (Exception ex) { Status.Text = ex.Message; return false; }
    }
    // The room needs the fixed screen: its slider rests while the screen follows the head.
    // Glass, reflections and the room light also need the room itself; the light colour
    // also needs the light. Resting controls keep their saved values.
    private void UpdateRoomControls()
    {
        if (RoomSlider == null || RoomValue == null || FollowCheck == null) return;
        bool follows = FollowCheck.IsChecked == true;
        RoomSlider.IsEnabled = !follows;
        int room = (int)Math.Round(RoomSlider.Value);
        RoomValue.Text = follows ? Loc.Get("NeedsFixedScreen") : room == 0 ? Loc.Get("ValueOff") : Loc.Format("ValuePercent", room);
        if (GlassSlider == null || GlassValue == null || ReflectSlider == null || ReflectValue == null ||
            LightSlider == null || LightValue == null || LightColourList == null) return;
        bool withRoom = !follows && room > 0;
        string resting = Loc.Get(follows ? "NeedsFixedScreen" : "NeedsRoom");
        GlassSlider.IsEnabled = withRoom;
        ReflectSlider.IsEnabled = withRoom;
        LightSlider.IsEnabled = withRoom;
        int glass = (int)Math.Round(GlassSlider.Value), reflect = (int)Math.Round(ReflectSlider.Value), light = (int)Math.Round(LightSlider.Value);
        GlassValue.Text = !withRoom ? resting : glass == 0 ? Loc.Get("ValueSolid") : Loc.Format("ValueClearPercent", glass);
        ReflectValue.Text = !withRoom ? resting : reflect == 0 ? Loc.Get("ValueOff") : Loc.Format("ValuePercent", reflect);
        LightValue.Text = !withRoom ? resting : light == 0 ? Loc.Get("ValueOff") : Loc.Format("ValuePercent", light);
        LightColourList.IsEnabled = withRoom && light > 0;
    }

    // The settings sections (2-7) follow whether a game is selected; their headers stay
    // usable, so sections can be opened and closed at any time.
    private void SetSettingsEnabled(bool on)
    {
        foreach (var content in new UIElement?[] { ScreenContent, AroundContent, RoomContent, DepthContent, SteamVrContent, ProfileContent })
        {
            if (content == null) continue;
            content.IsEnabled = on;
        }
    }

    // True when the settings sections accept input.
    private bool SettingsEnabled => ScreenContent?.IsEnabled == true;
    private void SettingsChanged(object sender, RoutedEventArgs e)
    {
        UpdateRoomControls();
        if (!ready || loading || profile == null) return;
        DrawPreview();
        // Coalesce rapid drag events, but keep delivering while the mouse is
        // moving rather than waiting until the player releases the slider.
        if (!saveTimer.IsEnabled) saveTimer.Start();
    }
    private void DrawPreview()
    {
        double centre = 270 + HorizontalSlider.Value * 20, y = 150 - DistanceSlider.Value * 17;
        ScreenLine.X1 = centre - WidthSlider.Value * 10; ScreenLine.X2 = centre + WidthSlider.Value * 10;
        ScreenLine.Y1 = ScreenLine.Y2 = y;
        DrawCurve(centre, y);
    }

    // World colour presets (Key = its name in Strings.resx); anything else is "Custom" and
    // typed as #RRGGBB. The list's items are the names in list order, Custom last.
    private static readonly (string Key, string Hex)[] WorldPresets =
    [
        ("WorldBlack", "#000000"), ("WorldCharcoal", "#1C1C1E"), ("WorldSlate", "#2A3441"), ("WorldMidnight", "#0B1530"),
        ("WorldPurple", "#1E0F2E"), ("WorldForest", "#0F2418"), ("WorldWarm", "#2A1E14"), ("WorldCinemaRed", "#2B0A0A"), ("WorldGrey", "#4A4A4A"),
        ("WorldDusk", "#33415C"), ("WorldOvercast", "#5A6270"),
    ];
    private static int CustomWorldIndex => WorldPresets.Length;
    private bool worldSyncing;
    private string lastWorldHex = "#000000";      // the last whole colour in the box

    // Fills (or re-labels) the list, keeping the selected entry.
    private void FillWorldList()
    {
        int selected = WorldList.SelectedIndex;
        worldSyncing = true;
        WorldList.Items.Clear();
        foreach (var preset in WorldPresets) WorldList.Items.Add(Loc.Get(preset.Key));
        WorldList.Items.Add(Loc.Get("WorldCustom"));
        WorldList.SelectedIndex = selected;
        worldSyncing = false;
    }

    // A preset picked: its colour goes into the box (Custom keeps whatever is there).
    private void WorldListChanged(object sender, SelectionChangedEventArgs e)
    {
        int index = WorldList.SelectedIndex;
        if (worldSyncing || index < 0 || index >= CustomWorldIndex) return;
        WorldHex.Text = WorldPresets[index].Hex;
    }

    // The box changed: show the colour, point the list at its preset (or Custom) and
    // save - but only once it is a whole colour, so typing does not save half of one.
    private void WorldHexChanged(object sender, TextChangedEventArgs e)
    {
        if (WorldSwatch == null || WorldList == null) return;
        if (!Profile.TryParseColor(WorldHex.Text, out int rgb))
        {
            // Half a colour: show it as Custom, so picking any preset - even the one
            // that was showing - fires and puts a whole colour back.
            WorldSwatch.Fill = Brushes.Transparent;
            worldSyncing = true;
            WorldList.SelectedIndex = CustomWorldIndex;
            worldSyncing = false;
            return;
        }
        WorldSwatch.Fill = new SolidColorBrush(Color.FromRgb((byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb));
        string hex = Profile.FormatColor(rgb);
        lastWorldHex = hex;
        int match = Array.FindIndex(WorldPresets, preset => preset.Hex == hex);
        worldSyncing = true;
        WorldList.SelectedIndex = match >= 0 ? match : CustomWorldIndex;
        worldSyncing = false;
        SettingsChanged(sender, e);
    }

    // Room light colours: blackbody sRGB values from the published Kelvin table. A saved
    // colour that is no preset (an edited profile) shows as one extra "Custom (#RRGGBB)"
    // item and is kept when saving. `lightHexes` holds each list item's colour.
    private static readonly (string Key, string Hex)[] LightPresets =
    [
        ("LightWarm", "#FFA957"), ("LightSoftWhite", Profile.DefaultRoomLightColor), ("LightNeutral", "#FFD1A3"),
        ("LightDaylight", "#FFF9FD"),
    ];
    private readonly List<string> lightHexes = [];

    // Lists the presets (plus the custom colour, if `hex` is none of them) and selects `hex`.
    // An unreadable colour selects the default.
    private void FillLightColourList(string? hex)
    {
        if (LightColourList == null) return;
        string wanted = Profile.TryParseColor(hex, out int rgb) ? Profile.FormatColor(rgb) : Profile.DefaultRoomLightColor;
        bool wasLoading = loading;
        loading = true;                            // filling the list is not a change to save
        LightColourList.Items.Clear();
        lightHexes.Clear();
        foreach (var preset in LightPresets)
        {
            LightColourList.Items.Add(Loc.Get(preset.Key));
            lightHexes.Add(preset.Hex);
        }
        int select = lightHexes.IndexOf(wanted);
        if (select < 0)
        {
            LightColourList.Items.Add(Loc.Format("LightCustom", wanted));
            lightHexes.Add(wanted);
            select = lightHexes.Count - 1;
        }
        LightColourList.SelectedIndex = select;
        loading = wasLoading;
    }

    // The selected preset's colour, or the kept custom one; the default if nothing is selected.
    private string SelectedLightColour()
    {
        int index = LightColourList?.SelectedIndex ?? -1;
        if (index < 0 || index >= lightHexes.Count) return Profile.DefaultRoomLightColor;
        return lightHexes[index];
    }

    // Leaving the box with half a colour in it puts the last whole one back, so the
    // other settings are never left unable to save.
    private void WorldHexLostFocus(object sender, RoutedEventArgs e)
    {
        if (Profile.TryParseColor(WorldHex.Text, out _)) return;
        WorldHex.Text = lastWorldHex;
    }

    // The top view shows the curve: the edges come towards the viewer (down the
    // canvas) by the sag of the arc, using the same geometry as the renderer
    // (screen_curve.h: kCurveMaxWrap 70 degrees, edges never nearer than 55 % of
    // the distance). A quadratic Bezier whose control point is one sag above the
    // ends passes exactly through the middle of the screen.
    private double arcSagPx;                       // how far below the straight line the arc's ends are drawn

    private void DrawCurve(double centre, double y)
    {
        double fraction = CurveSlider.Value / 100.0;
        bool curved = fraction > 0;
        ScreenArc.Visibility = curved ? Visibility.Visible : Visibility.Collapsed;
        ScreenLine.Visibility = curved ? Visibility.Collapsed : Visibility.Visible;
        arcSagPx = 0;
        if (!curved) return;

        double wrap = fraction * 70 * Math.PI / 180;
        double sag = WidthSlider.Value / wrap * (1 - Math.Cos(wrap / 2));
        sag = Math.Min(sag, DistanceSlider.Value * 0.45) * 17;
        arcSagPx = sag;
        var figure = new PathFigure { StartPoint = new Point(ScreenLine.X1, y + sag) };
        figure.Segments.Add(new QuadraticBezierSegment(new Point(centre, y - sag), new Point(ScreenLine.X2, y + sag), true));
        var geometry = new PathGeometry();
        geometry.Figures.Add(figure);
        ScreenArc.Data = geometry;
    }
    // Close enough to the drawn screen to drag it: the straight line, or anywhere
    // between the middle of a curved screen and its ends, which are drawn lower.
    private bool OverScreen(Point point) =>
        point.X >= ScreenLine.X1 - 12 && point.X <= ScreenLine.X2 + 12 &&
        point.Y >= ScreenLine.Y1 - 18 && point.Y <= ScreenLine.Y1 + arcSagPx + 18;
    private void PreviewDown(object sender, MouseButtonEventArgs e)
    {
        if (profile == null) return;
        if (!OverScreen(e.GetPosition(Preview))) return;
        dragging = true; Preview.CaptureMouse(); e.Handled = true;
    }
    private void PreviewMove(object sender, MouseEventArgs e)
    {
        if (!dragging) return;
        var point = e.GetPosition(Preview);
        HorizontalSlider.Value = Math.Round(Math.Clamp((point.X - 270) / 20, -3, 3), 2);
        DistanceSlider.Value = Math.Round(Math.Clamp((150 - point.Y) / 17, 1, 8), 2);
    }
    private void PreviewUp(object sender, MouseButtonEventArgs e) { dragging = false; Preview.ReleaseMouseCapture(); }
    private void UpdateButtons()
    {
        bool running = engine.Running;
        SetSettingsEnabled(profile != null && !stopping);
        StartButton.IsEnabled = !running && profile != null && WindowList.SelectedItem is GameWindow;
        StopButton.IsEnabled = running && !stopping;
        RecenterButton.IsEnabled = EasyRecenterButton.IsEnabled = DismissButton.IsEnabled = running && !stopping;
        AppList.IsEnabled = WindowList.IsEnabled = RefreshButton.IsEnabled = ShowAll.IsEnabled = !running;
        UpdateGameCard();
    }

    // The game card at the top (both modes): the game in front or attached, with its icon
    // and what is happening - waiting, attaching in N, starting, playing.
    private void UpdateGameCard()
    {
        if (CardName == null || CardState == null || CardIcon == null) return;
        var app = AppList.SelectedItem as RunningApp;
        string title = (WindowList.SelectedItem as GameWindow)?.Title ?? "";
        string name = app != null ? GameDisplayName(title, app.Name) : Loc.Get("CardNoGame");
        ImageSource? icon = app?.Icon;
        string state;
        if (engine.Running)
        {
            state = Loc.Get(stopping ? "CardStopping" : playing ? "CardPlaying" : "CardStarting");
        }
        else if (autoTarget != null)
        {
            name = GameDisplayName(autoTarget.Title, autoTarget.ProcessName);
            icon = RunningApps.IconFor(autoTarget.FullPath);
            state = Loc.Format("CardCountdown", autoMachine.Remaining);
        }
        else if (appSettings.AutoAttach)
        {
            state = Loc.Get(autoMachine.Phase == AutoAttachPhase.Spent ? "CardSpent" : "CardWaiting");
        }
        else if (app == null || !app.CanAttach)
        {
            name = Loc.Get("CardNoGame");
            icon = null;
            state = Loc.Get("CardPickGame");
        }
        else
        {
            state = Loc.Get(WindowList.SelectedItem is GameWindow ? "CardReady" : "CardChooseWindow");
        }
        CardName.Text = name;
        CardState.Text = state;
        CardIcon.Source = icon;
    }

    // A game's name for people: its window title, else its executable without ".exe";
    // "the game" when there is neither. Long titles are shortened.
    public static string GameDisplayName(string? title, string? processName)
    {
        string name = !string.IsNullOrWhiteSpace(title) ? title.Trim() : AutoAttachRules.BaseName(processName);
        if (name.Length == 0) name = Loc.Get("AutoTheGame");
        if (name.Length > 60) name = name[..57] + "...";
        return name;
    }
    private void StartClick(object sender, RoutedEventArgs e) => StartSession();
    // Attach / Play, for the button and for auto-attach alike. True when the engine started.
    private bool StartSession()
    {
        if (!SaveAndApply() || AppList.SelectedItem is not RunningApp app || WindowList.SelectedItem is not GameWindow window) return false;
        bool started = false;
        try
        {
            sessionError = lastEngineError = ""; LogBox.Clear();
            playing = false;
            engine.Start(app, window, profile!, store.Root);
            sessionWindow = window.Handle; started = true;
            Status.Text = Loc.Format("StatusStarting", app.Name);
        }
        catch (Exception ex)
        {
            sessionError = ex.Message; Status.Text = sessionError;
            autoMachine.MarkSpent(window.Handle);      // not retried until it leaves the foreground and comes back
        }
        UpdateButtons();
        return started;
    }

    // ---- Auto-attach -------------------------------------------------------------------

    // Shows the app settings: auto-attach, the sections' open/closed state and the mode
    // (Easy turns auto-attach on).
    private void PutAppSettings()
    {
        System.Diagnostics.Debug.WriteLine($"[AppSettings] PutAppSettings enter: mode {appSettings.Mode}, auto {appSettings.AutoAttach}, {appSettings.AutoAttachSeconds} s");
        autoSettingsLoading = true;
        AutoAttachCheck.IsChecked = appSettings.AutoAttach;
        AutoAttachSlider.Value = AppSettings.ClampSeconds(appSettings.AutoAttachSeconds);
        AutoAttachValue.Text = Loc.Format("ValueSeconds", (int)AutoAttachSlider.Value);
        autoMachine.Seconds = appSettings.AutoAttachSeconds;
        autoSettingsLoading = false;
        PutSections();
        SetMode(appSettings.IsExpert);
        System.Diagnostics.Debug.WriteLine("[AppSettings] PutAppSettings exit");
    }

    private void SaveAppSettingsFromUi()
    {
        if (!ready || autoSettingsLoading || AutoAttachCheck == null || AutoAttachSlider == null || AutoAttachValue == null) return;
        appSettings.AutoAttach = AutoAttachCheck.IsChecked == true;
        appSettings.AutoAttachSeconds = AppSettings.ClampSeconds((int)Math.Round(AutoAttachSlider.Value));
        AutoAttachValue.Text = Loc.Format("ValueSeconds", appSettings.AutoAttachSeconds);
        autoMachine.Seconds = appSettings.AutoAttachSeconds;
        System.Diagnostics.Debug.WriteLine($"[AutoAttach] settings: on {appSettings.AutoAttach}, {appSettings.AutoAttachSeconds} s");
        SaveAppSettings();
        UpdateAutoTimer();
        UpdateGameCard();
    }

    // Writes app-settings.json; a failure shows in the status line.
    private void SaveAppSettings()
    {
        System.Diagnostics.Debug.WriteLine($"[AppSettings] save: mode {appSettings.Mode}, auto {appSettings.AutoAttach}, {appSettings.AutoAttachSeconds} s");
        try { store.SaveAppSettings(appSettings); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            System.Diagnostics.Debug.WriteLine("[AppSettings] save failed: " + ex.Message);
            Status.Text = Loc.Format("StatusAppSettingsFailed", ex.Message);
        }
    }

    // ---- Easy | Expert -----------------------------------------------------------------

    private void ModeChecked(object sender, RoutedEventArgs e)
    {
        if (!ready || modeLoading) return;
        SetMode(ReferenceEquals(sender, ExpertMode));
    }

    // Easy shows the game card, Attach / Play, Stop VR and Recenter, and keeps auto-attach on;
    // Expert shows every section. Going back to Expert keeps auto-attach as it is. The
    // mode is remembered app-wide. Easy never changes a game's settings.
    private void SetMode(bool expert)
    {
        System.Diagnostics.Debug.WriteLine($"[Mode] SetMode enter: expert {expert}, was {appSettings.Mode}, auto-attach {appSettings.AutoAttach}");
        if (EasyMode == null || ExpertMode == null || SettingsScroll == null) return;
        modeLoading = true;
        EasyMode.IsChecked = !expert;
        ExpertMode.IsChecked = expert;
        modeLoading = false;
        var easyOnly = expert ? Visibility.Collapsed : Visibility.Visible;
        EasyPanel.Visibility = easyOnly;
        EasyIntro.Visibility = easyOnly;
        EasyRecenterButton.Visibility = easyOnly;
        SettingsScroll.Visibility = expert ? Visibility.Visible : Visibility.Collapsed;
        Subtitle.Text = Loc.Get(expert ? "SubtitleExpert" : "SubtitleEasy");
        bool remembered = ApplyWindowMode(expert);

        string mode = expert ? AppSettings.ExpertMode : AppSettings.EasyMode;
        bool changed = appSettings.Mode != mode || remembered;
        appSettings.Mode = mode;
        if (!expert && !appSettings.AutoAttach)
        {
            System.Diagnostics.Debug.WriteLine("[Mode] Easy turns auto-attach on");
            appSettings.AutoAttach = true;
            autoSettingsLoading = true;
            AutoAttachCheck.IsChecked = true;
            autoSettingsLoading = false;
            changed = true;
        }
        if (changed) SaveAppSettings();
        UpdateAutoTimer();
        UpdateGameCard();
        System.Diagnostics.Debug.WriteLine($"[Mode] SetMode exit: {appSettings.Mode}, auto-attach {appSettings.AutoAttach}, saved {changed}");
    }

    // ---- The window: compact Easy, remembered places, the title bar ----------------------

    private const double ExpertDefaultWidth = 1080, ExpertDefaultHeight = 900, ExpertMinWidth = 800, ExpertMinHeight = 640;
    // Easy's width: at least EasyMinContentWidth, and wide enough for the game card's
    // buttons beside a readable game name, and for the switch beside the subtitle (a longer
    // translation makes the window wider instead of squeezing them).
    private const double EasyMinContentWidth = 560, EasyNameMinWidth = 250, EasySubtitleMinWidth = 240, EasyButtonsMaxWidth = 560;
    private const double CardIconWidth = 44 + 14, CardNameGap = 12, HeaderGap = 16;
    private const double ExpertResizeBorder = 6;
    private static readonly Thickness ExpertMargin = new(24, 20, 24, 18), EasyMargin = new(20, 14, 20, 14);
    private static readonly Geometry MaximizeGlyph = Frozen("M 0.5,0.5 L 9.5,0.5 L 9.5,9.5 L 0.5,9.5 Z");
    private static readonly Geometry RestoreGlyph = Frozen("M 2.5,2.5 L 2.5,0.5 L 9.5,0.5 L 9.5,7.5 L 7.5,7.5 M 0.5,2.5 L 7.5,2.5 L 7.5,9.5 L 0.5,9.5 Z");

    private static Geometry Frozen(string data)
    {
        var geometry = Geometry.Parse(data);
        geometry.Freeze();
        return geometry;
    }

    // Lays the window out for a mode. Easy: compact, its height following its content, not
    // resizable or maximizable, at its remembered position. Expert: resizable, at its
    // remembered bounds (maximized if it was). The place of the mode being left is
    // remembered first; true when that changed the app settings.
    private bool ApplyWindowMode(bool expert)
    {
        System.Diagnostics.Debug.WriteLine($"[Window] ApplyWindowMode enter: expert {expert}, placed {placedMode}");
        int mode = expert ? 1 : 0;
        if (placedMode == mode)
        {
            if (!expert) FitEasyWidth();
            return false;
        }
        bool remembered = placedMode >= 0 && RememberPlace(placedMode == 1);
        placedMode = mode;
        NameRow.Visibility = expert ? Visibility.Visible : Visibility.Collapsed;
        RootGrid.Margin = expert ? ExpertMargin : EasyMargin;
        HeaderGrid.Margin = new Thickness(0, 0, 0, expert ? 16 : 12);
        if (expert) PlaceExpert();
        else PlaceEasy();
        UpdateCaptionButtons();
        System.Diagnostics.Debug.WriteLine($"[Window] ApplyWindowMode exit: {Width:F0} x {(SizeToContent == SizeToContent.Height ? "content" : Height.ToString("F0", CultureInfo.InvariantCulture))}, remembered {remembered}");
        return remembered;
    }

    private void PlaceEasy()
    {
        var chrome = WindowChrome.GetWindowChrome(this);
        if (WindowState == WindowState.Maximized) WindowState = WindowState.Normal;
        ResizeMode = ResizeMode.CanMinimize;
        if (chrome != null) chrome.ResizeBorderThickness = new Thickness(0);
        MinWidth = 0;
        MinHeight = 0;
        FitEasyWidth();
        SizeToContent = SizeToContent.Height;
        var place = appSettings.EasyWindow;
        System.Diagnostics.Debug.WriteLine($"[Window] PlaceEasy: saved {place?.ToString() ?? "none"}, smoke {smoke}");
        if (smoke) return;                             // the smoke test's window stays off screen
        if (place != null)
        {
            Left = place.Left;
            Top = place.Top;
        }
        else if (!IsLoaded)
        {
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
        }
        KeepOnScreenSoon();
    }

    private void PlaceExpert()
    {
        var chrome = WindowChrome.GetWindowChrome(this);
        SizeToContent = SizeToContent.Manual;
        ResizeMode = ResizeMode.CanResize;
        if (chrome != null) chrome.ResizeBorderThickness = new Thickness(ExpertResizeBorder);
        MinWidth = ExpertMinWidth;
        MinHeight = ExpertMinHeight;
        var place = appSettings.ExpertWindow;
        System.Diagnostics.Debug.WriteLine($"[Window] PlaceExpert: saved {place?.ToString() ?? "none"}, smoke {smoke}");
        Width = place != null ? Math.Max(place.Width, ExpertMinWidth) : ExpertDefaultWidth;
        Height = place != null ? Math.Max(place.Height, ExpertMinHeight) : ExpertDefaultHeight;
        if (smoke) return;                             // the smoke test's window stays off screen
        if (place != null)
        {
            Left = place.Left;
            Top = place.Top;
        }
        else if (!IsLoaded)
        {
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
        }
        if (place?.Maximized != true)
        {
            KeepOnScreenSoon();
            return;
        }
        // Maximized: on screen first, so it maximizes on a monitor that exists.
        if (!IsLoaded)
        {
            WindowState = WindowState.Maximized;
            return;
        }
        KeepOnScreenNow();
        WindowState = WindowState.Maximized;
    }

    // Easy's width, from what its content needs (see EasyMinContentWidth), within the screen.
    private void FitEasyWidth()
    {
        if (StartButton == null || ModeSwitch == null || GameCard == null) return;
        var unlimited = new Size(double.PositiveInfinity, double.PositiveInfinity);
        double buttons = 0;
        foreach (var button in new[] { StartButton, StopButton, EasyRecenterButton })
        {
            if (button.Visibility == Visibility.Collapsed) continue;
            button.Measure(unlimited);
            buttons += button.DesiredSize.Width;
        }
        ModeSwitch.Measure(unlimited);
        double card = GameCard.BorderThickness.Left + GameCard.BorderThickness.Right + GameCard.Padding.Left + GameCard.Padding.Right +
            CardIconWidth + EasyNameMinWidth + CardNameGap + Math.Min(buttons, EasyButtonsMaxWidth);
        double header = ModeSwitch.DesiredSize.Width + HeaderGap + EasySubtitleMinWidth;
        double content = Math.Max(EasyMinContentWidth, Math.Max(card, header));
        double width = Math.Ceiling(content + EasyMargin.Left + EasyMargin.Right + WindowFrame.BorderThickness.Left + WindowFrame.BorderThickness.Right);
        double screen = SystemParameters.WorkArea.Width;
        if (screen > 0) width = Math.Min(width, screen);
        System.Diagnostics.Debug.WriteLine($"[Window] FitEasyWidth: buttons {buttons:F0}, card {card:F0}, header {header:F0} -> {width:F0}");
        Width = width;
    }

    // Remembers where the window is for a mode (Expert: bounds and maximized; Easy: position).
    // False when there is nothing to remember yet.
    private bool RememberPlace(bool expert)
    {
        Rect bounds = WindowState == WindowState.Normal ? new Rect(Left, Top, Width, Height) : RestoreBounds;
        if (bounds.IsEmpty || !double.IsFinite(bounds.Width) || !double.IsFinite(bounds.Height))
        {
            System.Diagnostics.Debug.WriteLine($"[Window] RememberPlace: nothing to remember ({WindowState})");
            return false;
        }
        bool maximized = WindowState == WindowState.Maximized || (WindowState == WindowState.Minimized && restoreMaximized);
        if (expert)
        {
            appSettings.ExpertWindow = AppSettings.ValidBounds(new WindowPlace { Left = bounds.Left, Top = bounds.Top, Width = bounds.Width, Height = bounds.Height, Maximized = maximized });
        }
        else
        {
            appSettings.EasyWindow = AppSettings.ValidPosition(new WindowPlace { Left = bounds.Left, Top = bounds.Top });
        }
        System.Diagnostics.Debug.WriteLine($"[Window] RememberPlace: {(expert ? "Expert " + appSettings.ExpertWindow : "Easy " + appSettings.EasyWindow)}");
        return true;
    }

    // Brings the window back onto a monitor that exists, once the layout has settled.
    private void KeepOnScreenSoon()
    {
        if (smoke || !IsLoaded) return;
        Dispatcher.BeginInvoke(KeepOnScreenNow, DispatcherPriority.Loaded);
    }

    private void KeepOnScreenNow()
    {
        if (smoke || WindowState != WindowState.Normal) return;
        WindowPlacement.KeepOnScreen(new WindowInteropHelper(this).Handle, placedMode == 1);
    }

    // The skin's native parts: Windows 11's rounded corners and border in the skin's colour.
    private void WindowSourceInitialized(object? sender, EventArgs e)
    {
        nint hwnd = new WindowInteropHelper(this).Handle;
        System.Diagnostics.Debug.WriteLine($"[Window] SourceInitialized: {hwnd:X}");
        var edge = TryFindResource("WindowEdgeColor") is Color colour ? colour : Color.FromRgb(0x2A, 0x3B, 0x57);
        dwmFrame = WindowPlacement.ApplyDwmSkin(hwnd, edge);
        UpdateFrame();
    }

    private void WindowStateChanged(object? sender, EventArgs e)
    {
        if (WindowState != WindowState.Minimized) restoreMaximized = WindowState == WindowState.Maximized;
        System.Diagnostics.Debug.WriteLine($"[Window] StateChanged: {WindowState}");
        UpdateFrame();
        UpdateCaptionButtons();
    }

    // The window's own border (only where Windows draws none), and, when maximized, room for
    // the frame Windows pushes past the screen's edges.
    private void UpdateFrame()
    {
        if (WindowFrame == null) return;
        bool maximized = WindowState == WindowState.Maximized;
        WindowFrame.BorderThickness = new Thickness(maximized || dwmFrame ? 0 : 1);
        WindowFrame.Padding = maximized ? MaximizedOverhang() : new Thickness(0);
    }

    // How far a maximized window reaches past its monitor's work area, in WPF units.
    private Thickness MaximizedOverhang()
    {
        nint hwnd = new WindowInteropHelper(this).Handle;
        var work = WindowPlacement.WorkAreaOf(hwnd);
        if (work.Area <= 0 || !WindowPlacement.TryGetBounds(hwnd, out var bounds)) return new Thickness(0);
        var dpi = VisualTreeHelper.GetDpi(this);
        var overhang = new Thickness(Math.Max(0, work.Left - bounds.Left) / dpi.DpiScaleX, Math.Max(0, work.Top - bounds.Top) / dpi.DpiScaleY,
            Math.Max(0, bounds.Right - work.Right) / dpi.DpiScaleX, Math.Max(0, bounds.Bottom - work.Bottom) / dpi.DpiScaleY);
        System.Diagnostics.Debug.WriteLine($"[Window] MaximizedOverhang: window {bounds}, work {work} -> {overhang}");
        return overhang;
    }

    // Maximize / Restore Down only in Expert; its glyph and name follow the window's state.
    private void UpdateCaptionButtons()
    {
        if (MaxButton == null || MaxGlyph == null) return;
        MaxButton.Visibility = placedMode == 1 ? Visibility.Visible : Visibility.Collapsed;
        bool maximized = WindowState == WindowState.Maximized;
        MaxGlyph.Data = maximized ? RestoreGlyph : MaximizeGlyph;
        string name = Loc.Get(maximized ? "TitleRestore" : "TitleMaximize");
        MaxButton.ToolTip = name;
        AutomationProperties.SetName(MaxButton, name);
    }

    // The logo opens the window menu (as Alt+Space does) just below itself.
    private void MenuClick(object sender, RoutedEventArgs e)
    {
        var source = PresentationSource.FromVisual(MenuButton);
        if (source?.CompositionTarget == null) return;
        var corner = MenuButton.PointToScreen(new Point(0, MenuButton.ActualHeight));
        SystemCommands.ShowSystemMenu(this, source.CompositionTarget.TransformFromDevice.Transform(corner));
    }

    private void MinimizeClick(object sender, RoutedEventArgs e) => SystemCommands.MinimizeWindow(this);

    private void MaximizeClick(object sender, RoutedEventArgs e)
    {
        if (placedMode != 1) return;
        if (WindowState == WindowState.Maximized) SystemCommands.RestoreWindow(this);
        else SystemCommands.MaximizeWindow(this);
    }

    private void CloseClick(object sender, RoutedEventArgs e) => SystemCommands.CloseWindow(this);

    // ---- Sections ----------------------------------------------------------------------

    // Each collapsible section and its name in app-settings.json.
    private (string Name, Expander Section)[] SectionList() =>
    [
        (AppSettings.SectionGame, GameSection), (AppSettings.SectionScreen, ScreenSection), (AppSettings.SectionAround, AroundSection),
        (AppSettings.SectionRoom, RoomSection), (AppSettings.SectionDepth, DepthSection), (AppSettings.SectionSteamVr, SteamVrSection),
        (AppSettings.SectionProfile, ProfileSection), (AppSettings.SectionSession, SessionDetails),
    ];

    private void PutSections()
    {
        sectionsLoading = true;
        foreach (var (name, section) in SectionList()) section.IsExpanded = appSettings.SectionOpen(name);
        sectionsLoading = false;
    }

    // A section opened or closed by the user: remembered app-wide.
    private void SectionToggled(object sender, RoutedEventArgs e)
    {
        if (!ReferenceEquals(sender, e.OriginalSource)) return;
        if (!ready || sectionsLoading) return;
        var open = new Dictionary<string, bool>(StringComparer.Ordinal);
        foreach (var (name, section) in SectionList()) open[name] = section.IsExpanded;
        appSettings.Sections = open;
        System.Diagnostics.Debug.WriteLine($"[Sections] {((Expander)sender).Name} {(((Expander)sender).IsExpanded ? "opened" : "closed")}");
        SaveAppSettings();
    }

    // After a failed session: open the log (without changing the remembered state) and show it.
    private void ShowSessionDetails()
    {
        sectionsLoading = true;
        SessionDetails.IsExpanded = true;
        sectionsLoading = false;
        if (appSettings.IsExpert) SessionDetails.BringIntoView();
    }

    // ---- Strings -----------------------------------------------------------------------

    // The shortcut key choices; key names come from the strings, keeping the selections.
    private void FillShortcutKeys()
    {
        object? recenter = RecenterKeys.SelectedValue, menu = MenuKeys.SelectedValue;
        bool wasLoading = loading;
        loading = true;
        var keys = new List<Shortcut> { new("= / +", 0xBB), new(Loc.Get("KeyHome"), 0x24), new(Loc.Get("KeyEnd"), 0x23),
            new(Loc.Get("KeyInsert"), 0x2D), new(Loc.Get("KeyPause"), 0x13) };
        for (int i = 1; i <= 24; i++) keys.Add(new("F" + i.ToString(CultureInfo.InvariantCulture), 0x6F + i));
        for (int i = 0; i < 26; i++) keys.Add(new(((char)('A' + i)).ToString(), 'A' + i));
        RecenterKeys.ItemsSource = keys; MenuKeys.ItemsSource = keys;
        if (recenter != null) RecenterKeys.SelectedValue = recenter;
        if (menu != null) MenuKeys.SelectedValue = menu;
        loading = wasLoading;
    }

    // Re-labels everything set from code after the strings change (the pseudo-locale in
    // the smoke test); XAML text follows by itself.
    private void RefreshLanguage()
    {
        System.Diagnostics.Debug.WriteLine($"[MainWindow] RefreshLanguage enter: pseudo {Loc.Pseudo}");
        Title = Loc.Format("WindowTitle", VersionText.Text);
        Subtitle.Text = Loc.Get(appSettings.IsExpert ? "SubtitleExpert" : "SubtitleEasy");
        FillWorldList();
        FillLightColourList(SelectedLightColour());
        FillShortcutKeys();
        SetGpuLines(gpuLines);
        // A selected ComboBoxItem's text is copied when it is selected: select it again.
        bool wasLoading = loading;
        loading = true;
        int timing = TimingList.SelectedIndex;
        TimingList.SelectedIndex = -1;
        TimingList.SelectedIndex = timing;
        loading = wasLoading;
        AppList.Items.Refresh();
        WindowList.Items.Refresh();
        AutoAttachValue.Text = Loc.Format("ValueSeconds", appSettings.AutoAttachSeconds);
        if (profile == null) PathLabel.Text = Loc.Get(AppList.SelectedItem == null ? "PathChoose" : "PathNoAccess");
        UpdateRoomControls();
        UpdateGameCard();
        UpdateCaptionButtons();
        if (placedMode == 0) FitEasyWidth();
        System.Diagnostics.Debug.WriteLine("[MainWindow] RefreshLanguage exit");
    }

    private void AutoAttachChanged(object sender, RoutedEventArgs e) => SaveAppSettingsFromUi();
    private void AutoAttachSecondsChanged(object sender, RoutedPropertyChangedEventArgs<double> e) => SaveAppSettingsFromUi();

    // Polls only while auto-attach is on. Turning it off cancels a countdown. The smoke
    // test never polls (it must not touch real windows or attach).
    private void UpdateAutoTimer()
    {
        if (smoke || !ready) return;
        if (appSettings.AutoAttach)
        {
            if (!autoTimer.IsEnabled) autoTimer.Start();
            return;
        }
        autoTimer.Stop();
        ApplyAutoAction(autoMachine.Step(new AutoAttachInput(false, engine.Running || stopping, 0, false, false, autoClock.Elapsed.TotalSeconds)), null);
    }

    private void AutoAttachTick()
    {
        if (!ready || closing || smoke) return;
        ForegroundSnapshot? snapshot = null;
        try { snapshot = ForegroundWindow.Read(store.HasProfile); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine("[AutoAttach] could not read the window in front: " + ex.Message); }
        bool qualifies = AutoAttachRules.Evaluate(snapshot, out string reason);
        var input = new AutoAttachInput(appSettings.AutoAttach, engine.Running || stopping, snapshot?.Handle ?? 0,
            snapshot?.IsVrx == true, qualifies, autoClock.Elapsed.TotalSeconds);
        var action = autoMachine.Step(input);
        if (action != AutoAttachAction.None)
            System.Diagnostics.Debug.WriteLine($"[AutoAttach] {action}: {snapshot?.ProcessName} '{snapshot?.Title}' ({reason}), {autoMachine.Remaining} s left");
        ApplyAutoAction(action, snapshot);
    }

    // "Attaching VRX to <game> in 5 - switch window to cancel"
    public static string CountdownText(string? title, string? processName, int seconds) =>
        Loc.Format("AutoCountdown", GameDisplayName(title, processName), Math.Max(0, seconds));

    private void ApplyAutoAction(AutoAttachAction action, ForegroundSnapshot? snapshot)
    {
        switch (action)
        {
            case AutoAttachAction.Countdown:
            {
                if (snapshot == null) return;
                if (autoTarget == null) autoSavedStatus = Status.Text;
                autoTarget = snapshot;
                autoStatus = CountdownText(snapshot.Title, snapshot.ProcessName, autoMachine.Remaining);
                Status.Text = autoStatus;
                try
                {
                    autoOverlay ??= new AutoAttachOverlay();
                    autoOverlay.ShowOn(snapshot.Monitor, autoStatus);
                }
                catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception)
                {
                    System.Diagnostics.Debug.WriteLine("[AutoAttach] overlay failed: " + ex.Message);
                }
                UpdateGameCard();
                return;
            }
            case AutoAttachAction.Cancel:
            {
                HideAutoOverlay();
                if (Status.Text == autoStatus) Status.Text = autoSavedStatus;
                autoTarget = null;
                autoStatus = "";
                UpdateGameCard();
                return;
            }
            case AutoAttachAction.Attach:
            {
                HideAutoOverlay();
                var target = snapshot ?? autoTarget;
                autoTarget = null;
                autoStatus = "";
                AutoAttachNow(target);
                return;
            }
            default:
                return;
        }
    }

    private void HideAutoOverlay()
    {
        if (autoOverlay == null || !autoOverlay.IsVisible) return;
        autoOverlay.Hide();
    }

    // At zero: pick the game and its window exactly as a manual pick would (its own profile,
    // or the base settings for a new game), then start it through Attach / Play's path.
    private void AutoAttachNow(ForegroundSnapshot? target)
    {
        System.Diagnostics.Debug.WriteLine($"[AutoAttach] attach to {target?.ProcessName} pid {target?.Pid} window {target?.Handle:X}");
        if (target == null || target.Handle == 0) return;
        if (engine.Running || stopping) return;
        List<RunningApp> list;
        try { list = RunningApps.List(ShowAll.IsChecked == true); }
        catch (Exception ex)
        {
            Status.Text = Loc.Format("StatusAutoListFailed", ex.Message);
            autoMachine.MarkSpent(target.Handle);
            return;
        }
        var app = list.FirstOrDefault(a => a.Pid == target.Pid && a.CanAttach && a.Windows.Any(w => w.Handle == target.Handle));
        if (app == null)
        {
            Status.Text = Loc.Format("StatusAutoGone", AutoAttachRules.BaseName(target.ProcessName));
            autoMachine.MarkSpent(target.Handle);
            return;
        }
        refreshing = true;
        AppList.ItemsSource = list;
        AppList.SelectedItem = app;
        refreshing = false;
        sessionError = "";
        LoadSelected();                                 // as AppSelected does for a manual pick
        WindowList.SelectedItem = app.Windows.First(w => w.Handle == target.Handle);
        if (profile == null || !StartSession())
        {
            System.Diagnostics.Debug.WriteLine($"[AutoAttach] attach to {app.Name} failed: {Status.Text}");
            autoMachine.MarkSpent(target.Handle);
            return;
        }
        Status.Text = Loc.Format("StatusAutoStarted", app.Name);
        System.Diagnostics.Debug.WriteLine($"[AutoAttach] started {app.Name}");
    }
    private async void StopClick(object sender, RoutedEventArgs e) => await StopSession();
    private async Task StopSession()
    {
        if (!engine.Running || profile == null || stopping) return;
        SaveAndApply(); saveTimer.Stop();
        stopping = true; UpdateButtons(); Status.Text = Loc.Get("StatusStopping");
        try { await engine.Stop(profile); }
        catch (Exception ex) { Status.Text = Loc.Format("StatusStopFailed", ex.Message); }
        finally { stopping = false; UpdateButtons(); }
    }
    private void RecenterClick(object sender, RoutedEventArgs e)
    { if (SaveAndApply() && engine.Running) { engine.Update(profile!, reset: true); Status.Text = Loc.Get("StatusRecenter"); } }
    private void DismissClick(object sender, RoutedEventArgs e)
    { if (SaveAndApply() && engine.Running) { engine.Update(profile!, dismiss: true); Status.Text = Loc.Get("StatusDismiss"); } }
    private void ResetClick(object sender, RoutedEventArgs e)
    {
        if (profile == null) return;
        var start = store.BaseSettings();
        start.ExecutablePath = profile.ExecutablePath;
        start.PreferredWindowTitle = profile.PreferredWindowTitle;
        PutProfile(start);
        SaveAndApply();
        Status.Text = Loc.Get(store.HasBase ? "StatusResetBase" : "StatusResetDefault");
    }
    // Keeps the settings shown here as the starting point for games not yet set up.
    private void MakeBaseClick(object sender, RoutedEventArgs e)
    {
        if (profile == null) { Status.Text = Loc.Get("StatusBaseSelectFirst"); return; }
        if (!SaveAndApply()) return;
        try
        {
            store.SaveBase(profile);
            Status.Text = Loc.Get("StatusBaseSaved");
        }
        catch (Exception ex) { Status.Text = Loc.Format("StatusBaseFailed", ex.Message); }
    }

    // The Apply to all warning for `count` saved games.
    public static string ApplyAllQuestion(int count) => Loc.Format(count == 1 ? "ApplyAllQuestionOne" : "ApplyAllQuestionOther", count);

    // The status after Apply to all: the result, then any skipped or failed profiles, as
    // separate messages joined by the status separator.
    public static string ApplyAllResult(int applied, int skipped, int failed)
    {
        var parts = new List<string>
        {
            skipped == 0 && failed == 0 ? Loc.Format(applied == 1 ? "AppliedAllOne" : "AppliedAllOther", applied) :
                Loc.Format(applied == 1 ? "AppliedSomeOne" : "AppliedSomeOther", applied),
        };
        if (skipped > 0) parts.Add(Loc.Format(skipped == 1 ? "SkippedOne" : "SkippedOther", skipped));
        if (failed > 0) parts.Add(Loc.Format("FailedWrite", failed));
        return string.Join(Loc.Get("StatusSeparator"), parts);
    }

    // Overwrites every saved game, so it asks first; Cancel is the default.
    private void ApplyAllClick(object sender, RoutedEventArgs e)
    {
        if (profile == null) { Status.Text = Loc.Get("StatusApplyAllSelectFirst"); return; }
        if (!SaveAndApply()) return;

        int count = store.SavedProfileFiles().Count;
        var answer = MessageBox.Show(this, ApplyAllQuestion(count), Loc.Get("ApplyAllTitle"), MessageBoxButton.OKCancel, MessageBoxImage.Warning, MessageBoxResult.Cancel);
        if (answer != MessageBoxResult.OK) { Status.Text = Loc.Get("StatusApplyAllCancelled"); return; }
        try
        {
            int applied = store.ApplyToAll(profile, out int skipped, out int failed);
            Status.Text = ApplyAllResult(applied, skipped, failed);
        }
        catch (Exception ex) { Status.Text = Loc.Format("StatusApplyAllFailed", ex.Message); }
    }
    private void RefreshClick(object sender, RoutedEventArgs e) => RefreshApps();
    private void ShowAllChanged(object sender, RoutedEventArgs e) { if (ready) RefreshApps(); }
    private void WindowSelected(object sender, SelectionChangedEventArgs e)
    {
        if (!ready) return;
        UpdateButtons();
        if (!loading && !refreshing && profile != null) { saveTimer.Stop(); saveTimer.Start(); }
    }
    private async void WindowClosing(object? sender, CancelEventArgs e)
    {
        if (closing) return;
        if (placedMode >= 0 && RememberPlace(placedMode == 1)) SaveAppSettings();
        SaveAndApply(); refreshTimer.Stop(); saveTimer.Stop();
        autoTimer.Stop(); HideAutoOverlay();
        if (!engine.Running) return;
        e.Cancel = true;
        await StopSession();
        if (engine.Running) { Status.Text = Loc.Get("StatusStillStopping"); UpdateAutoTimer(); return; }
        closing = true; Close();
    }
    private async Task SmokeTest()
    {
        string root = EngineSession.RepositoryRoot(), output = Path.Combine(root, "desktop", "out");
        Directory.CreateDirectory(output);
        var observed = RunningApps.List(false);
        File.WriteAllLines(Path.Combine(output, "enumerated-apps.txt"), observed.Select(a => $"{a.Pid} | {a.FullPath} | {string.Join("; ", a.Windows)}"));
        var one = new Profile { ExecutablePath = Path.Combine(output, "one", "game.exe"), Width = 6.25, Distance = 3.5, Height = .2, Horizontal = .4, Strength = .8, MenuKey = 0x78 };
        var two = new Profile { ExecutablePath = Path.Combine(output, "two", "game.exe"), Width = 4, ForegroundRefinement = false, MatchFrameToDepth = true, FastDepthModel = false };
        store.Save(one); store.Save(two);
        if (store.Load(one.ExecutablePath).Width != 6.25 || store.Load(two.ExecutablePath).Width != 4 || store.FileFor(one.ExecutablePath) == store.FileFor(two.ExecutablePath))
            throw new Exception("Executable profile isolation failed");
        if (!store.Load(one.ExecutablePath).ForegroundRefinement || store.Load(two.ExecutablePath).ForegroundRefinement)
            throw new Exception("Foreground refinement default/opt-out did not round-trip per executable");
        var legacy = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(store.FileFor(one.ExecutablePath)))!.AsObject();
        legacy.Remove("ForegroundRefinement");
        legacy.Remove("MatchFrameToDepth");
        legacy.Remove("DelayToDepth");
        legacy.Remove("FastDepthModel");
        legacy.Remove("SteadyDepth");
        legacy.Remove("SubpixelWarp");
        legacy.Remove("ScreenCurve");
        legacy.Remove("Ambilight");
        legacy.Remove("AmbilightStrength");
        legacy.Remove("WorldColor");
        legacy.Remove("Room");
        legacy.Remove("RoomGlass");
        legacy.Remove("RoomReflections");
        legacy.Remove("RoomLight");
        legacy.Remove("RoomLightColor");
        legacy.Remove("FuseModels");
        File.WriteAllText(store.FileFor(one.ExecutablePath), legacy.ToJsonString());
        if (!store.Load(one.ExecutablePath).FastDepthModel || store.Load(two.ExecutablePath).FastDepthModel)
            throw new Exception("Fast depth model must default on for old profiles and keep a per-game opt-out");
        if (!store.Load(one.ExecutablePath).ForegroundRefinement) throw new Exception("Old profiles must default foreground refinement on");
        if (store.Load(one.ExecutablePath).DepthGpu != Gpus.Same) throw new Exception("Profiles without a depth GPU must use the game's GPU");
        var three = new Profile { ExecutablePath = Path.Combine(output, "three", "game.exe") };
        store.Save(three);
        var earlier = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(store.FileFor(three.ExecutablePath)))!.AsObject();
        earlier.Remove("DepthGpu");
        earlier["DepthOnSecondGpu"] = true;
        File.WriteAllText(store.FileFor(three.ExecutablePath), earlier.ToJsonString());
        if (store.Load(three.ExecutablePath).DepthGpu != Gpus.Auto) throw new Exception("The earlier second-GPU checkbox must migrate to automatic");
        if (store.Load(one.ExecutablePath).MatchFrameToDepth || !store.Load(two.ExecutablePath).MatchFrameToDepth)
            throw new Exception("Frame matching default/migration/per-game round-trip failed");
        var conflict = new Profile { RecenterKey = 0x77, MenuKey = 0x77 };
        if (conflict.Valid()) throw new Exception("Shortcut conflict was accepted");
        one.MatchFrameToDepth = true;
        File.WriteAllText(Path.Combine(output, "control-contract.txt"), one.Control(4, 7, false));
        var original = new Profile { FastDepthModel = false };
        var both = new Profile { FuseModels = true };
        var unsteady = new Profile { SteadyDepth = false };
        var delayed = new Profile { DelayToDepth = true };
        var matchedWins = new Profile { DelayToDepth = true, MatchFrameToDepth = true };
        var wholePixel = new Profile { SubpixelWarp = false };
        var curved = new Profile { ScreenCurve = 65, Ambilight = true };
        var coloured = new Profile { Ambilight = true, AmbilightStrength = 40, WorldColor = "#2A3441" };
        var roomy = new Profile { Room = 45 };
        var glassy = new Profile { Room = 45, RoomGlass = 60, RoomReflections = 25, RoomLight = 40, RoomLightColor = "#FFD1A3" };
        if (!new Profile().Control(0, 0, false).StartsWith("VRX 11 ") || !new Profile().Control(0, 0, false).TrimEnd().EndsWith(" 0 1 1 0 0 1 0 0 85 0 0 0 0 0 16757867") ||
            !original.Control(0, 0, false).TrimEnd().EndsWith(" 0 0 1 0 0 1 0 0 85 0 0 0 0 0 16757867") || !both.Control(0, 0, false).TrimEnd().EndsWith(" 1 1 1 0 1 0 0 85 0 0 0 0 0 16757867") ||
            !unsteady.Control(0, 0, false).TrimEnd().EndsWith(" 1 0 0 0 1 0 0 85 0 0 0 0 0 16757867") || !delayed.Control(0, 0, false).TrimEnd().EndsWith(" 0 1 1 0 1 1 0 0 85 0 0 0 0 0 16757867") ||
            !matchedWins.Control(0, 0, false).TrimEnd().EndsWith(" 1 1 1 0 0 1 0 0 85 0 0 0 0 0 16757867") || !wholePixel.Control(0, 0, false).TrimEnd().EndsWith(" 1 1 0 0 0 0 0 85 0 0 0 0 0 16757867") ||
            !curved.Control(0, 0, false).TrimEnd().EndsWith(" 1 65 1 85 0 0 0 0 0 16757867") || !coloured.Control(0, 0, false).TrimEnd().EndsWith(" 0 1 40 2765889 0 0 0 0 16757867") ||
            !roomy.Control(0, 0, false).TrimEnd().EndsWith(" 0 0 85 0 45 0 0 0 16757867") ||
            !glassy.Control(0, 0, false).TrimEnd().EndsWith(" 0 0 85 0 45 60 25 40 16765347"))
            throw new Exception("Control snapshot must be v11 ending with the matched, fast-model, steady, fuse, delayed and sub-pixel flags, the curve percentage, the ambilight flag and strength, the world colour, the room level and the room's glass, reflections, light and light colour (see desktop_control.h)");
        if (!store.Load(one.ExecutablePath).SteadyDepth || store.Load(one.ExecutablePath).FuseModels)
            throw new Exception("Profiles saved before steady/fuse existed must load with steadying on and fusion off");
        if (!store.Load(one.ExecutablePath).SubpixelWarp)
            throw new Exception("Profiles saved before the sub-pixel warp existed must load with it on");
        if (store.Load(one.ExecutablePath).ScreenCurve != 0 || store.Load(one.ExecutablePath).Ambilight)
            throw new Exception("Profiles saved before the curve and ambilight existed must load flat, with no glow");
        if (store.Load(one.ExecutablePath).AmbilightStrength != 85 || store.Load(one.ExecutablePath).WorldColor != "#000000")
            throw new Exception("Profiles saved before the glow strength and world colour existed must load at 85 % and black");
        if (store.Load(one.ExecutablePath).Room != 0) throw new Exception("Profiles saved before the room existed must load with it off");
        // A fresh install starts with the room set up; nothing else changes, and base
        // settings saved before the room existed keep it off.
        string freshRoot = Path.Combine(output, "newInstall-install");
        if (Directory.Exists(freshRoot)) Directory.Delete(freshRoot, true);
        var newInstall = new ProfileStore(freshRoot);
        var freshStart = newInstall.BaseSettings();
        if (freshStart.Room != Profile.NewInstallRoom || freshStart.RoomGlass != Profile.NewInstallRoomGlass ||
            freshStart.RoomReflections != Profile.NewInstallRoomReflections || freshStart.RoomLight != Profile.NewInstallRoomLight ||
            freshStart.RoomLightColor != Profile.DefaultRoomLightColor)
            throw new Exception("A newInstall install must start with the room set up (20 / 14 / 15 / 15, warm light)");
        var plain = new Profile();
        if (freshStart.FuseModels != plain.FuseModels || freshStart.DepthGpu != plain.DepthGpu || freshStart.ScreenCurve != plain.ScreenCurve ||
            freshStart.Ambilight != plain.Ambilight || freshStart.DelayToDepth != plain.DelayToDepth || freshStart.Width != plain.Width)
            throw new Exception("A newInstall install must change only the room's defaults");
        if (newInstall.Load(Path.Combine(freshRoot, "game.exe")).Room != Profile.NewInstallRoom)
            throw new Exception("A new game on a newInstall install must start with the room set up");
        Directory.CreateDirectory(freshRoot);
        File.WriteAllText(newInstall.BaseFile, "{\"Width\":5.7}");
        if (newInstall.BaseSettings().Room != 0) throw new Exception("Base settings saved before the room existed must keep the room off");
        if (new Profile { Room = 101 }.Valid() || new Profile { Room = -1 }.Valid() || !new Profile { Room = 100 }.Valid())
            throw new Exception("Room level validation");
        var oldRoom = store.Load(one.ExecutablePath);
        if (oldRoom.RoomGlass != 0 || oldRoom.RoomReflections != 0 || oldRoom.RoomLight != 0 || oldRoom.RoomLightColor != "#FFB46B")
            throw new Exception("Profiles saved before glass, reflections and the room light existed must load with them off and a soft white light colour");
        if (new Profile { RoomGlass = 101 }.Valid() || new Profile { RoomGlass = -1 }.Valid() || !new Profile { RoomGlass = 100 }.Valid() ||
            new Profile { RoomReflections = 101 }.Valid() || new Profile { RoomReflections = -1 }.Valid() || !new Profile { RoomReflections = 100 }.Valid() ||
            new Profile { RoomLight = 101 }.Valid() || new Profile { RoomLight = -1 }.Valid() || !new Profile { RoomLight = 100 }.Valid() ||
            new Profile { RoomLightColor = "#12" }.Valid() || new Profile { RoomLightColor = "#GGGGGG" }.Valid() || !new Profile { RoomLightColor = "#FFF9FD" }.Valid())
            throw new Exception("Room glass, reflections, light and light colour validation");
        var nullColour = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(store.FileFor(one.ExecutablePath)))!.AsObject();
        nullColour["WorldColor"] = null;
        File.WriteAllText(store.FileFor(one.ExecutablePath), nullColour.ToJsonString());
        if (store.Load(one.ExecutablePath).WorldColor != "#000000") throw new Exception("A missing world colour must load as black");
        var nullLight = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(store.FileFor(one.ExecutablePath)))!.AsObject();
        nullLight["RoomLightColor"] = null;
        File.WriteAllText(store.FileFor(one.ExecutablePath), nullLight.ToJsonString());
        if (store.Load(one.ExecutablePath).RoomLightColor != "#FFB46B") throw new Exception("A missing room light colour must load as soft white");
        if (new Profile { WorldColor = "#12" }.Valid() || new Profile { WorldColor = "#GGGGGG" }.Valid() || new Profile { AmbilightStrength = 101 }.Valid() ||
            !new Profile { WorldColor = "2a3441" }.Valid())
            throw new Exception("Glow strength and world colour validation");
        if (!Profile.TryParseColor("#2A3441", out int slate) || slate != 0x2A3441 || Profile.FormatColor(slate) != "#2A3441")
            throw new Exception("World colour parsing");
        one.MatchFrameToDepth = false;
        var sample = new RunningApp(1234, "game.exe", one.ExecutablePath, [new GameWindow(42, "Example game window")], null);
        refreshing = true; AppList.ItemsSource = new[] { sample }; AppList.SelectedItem = sample; refreshing = false;
        profile = one; PutProfile(one); WindowList.ItemsSource = sample.Windows; WindowList.SelectedIndex = 0;
        if (ForegroundCheck.IsChecked != true) throw new Exception("Foreground checkbox default is not on");
        if (CurveSlider.Value != 0 || AmbilightCheck.IsChecked == true) throw new Exception("The screen curve and the ambilight glow should default off");
        if (ScreenArc.Visibility != Visibility.Collapsed || ScreenLine.Visibility != Visibility.Visible)
            throw new Exception("A flat screen must be drawn as the straight line in the top view");
        CurveSlider.Value = 40;
        AmbilightCheck.IsChecked = true;
        if (ReadProfile().ScreenCurve != 40 || !ReadProfile().Ambilight) throw new Exception("The curve slider and ambilight checkbox are not mapped to settings");
        if (ScreenArc.Visibility != Visibility.Visible || ScreenLine.Visibility != Visibility.Collapsed || ScreenArc.Data == null)
            throw new Exception("A curved screen must be drawn as the arc in the top view");
        double savedWidth = WidthSlider.Value, savedCurve = CurveSlider.Value;
        WidthSlider.Value = 10; CurveSlider.Value = 100;
        if (arcSagPx < 20 || !OverScreen(new Point(ScreenLine.X1 + 2, ScreenLine.Y1 + arcSagPx)) || OverScreen(new Point(ScreenLine.X1, ScreenLine.Y1 + arcSagPx + 40)))
            throw new Exception("The ends of a curved screen must be draggable in the top view");
        WidthSlider.Value = savedWidth; CurveSlider.Value = savedCurve;
        CurveSlider.Value = 0;
        AmbilightCheck.IsChecked = false;
        if (RoomSlider.Value != 0 || RoomValue.Text != Loc.Get("ValueOff") || !RoomSlider.IsEnabled) throw new Exception("The room should default off, and be available");
        RoomSlider.Value = 45;
        if (ReadProfile().Room != 45 || RoomValue.Text != Loc.Format("ValuePercent", 45)) throw new Exception("The room slider is not mapped to settings");
        FollowCheck.IsChecked = true;
        if (RoomSlider.IsEnabled || RoomValue.Text != Loc.Get("NeedsFixedScreen")) throw new Exception("The room must rest while the screen follows the head");
        FollowCheck.IsChecked = false;
        if (!RoomSlider.IsEnabled) throw new Exception("The room must come back with the fixed screen");
        RoomSlider.Value = 0;
        // Glass, reflections and the room light: they need the room and the fixed screen, and the
        // light colour also needs the light. Resting controls keep their values.
        if (GlassSlider.Value != 0 || ReflectSlider.Value != 0 || LightSlider.Value != 0 || GlassSlider.IsEnabled || ReflectSlider.IsEnabled ||
            LightSlider.IsEnabled || LightColourList.IsEnabled || GlassValue.Text != Loc.Get("NeedsRoom") || ReflectValue.Text != Loc.Get("NeedsRoom") ||
            LightValue.Text != Loc.Get("NeedsRoom"))
            throw new Exception("Glass, reflections and the room light must default off and rest without the room");
        if (LightColourList.SelectedItem as string != Loc.Get("LightSoftWhite") || ReadProfile().RoomLightColor != "#FFB46B")
            throw new Exception("The room light colour should default to soft white");
        RoomSlider.Value = 45;
        if (!GlassSlider.IsEnabled || !ReflectSlider.IsEnabled || !LightSlider.IsEnabled || LightColourList.IsEnabled ||
            GlassValue.Text != Loc.Get("ValueSolid") || ReflectValue.Text != Loc.Get("ValueOff") || LightValue.Text != Loc.Get("ValueOff"))
            throw new Exception("Glass, reflections and the room light must be available with the room, the light colour only with the light");
        GlassSlider.Value = 60; ReflectSlider.Value = 25; LightSlider.Value = 50;
        if (GlassValue.Text != Loc.Format("ValueClearPercent", 60) || ReflectValue.Text != Loc.Format("ValuePercent", 25) || LightValue.Text != Loc.Format("ValuePercent", 50) || !LightColourList.IsEnabled)
            throw new Exception("Glass, reflections and room light value texts");
        var roomLook = ReadProfile();
        if (roomLook.RoomGlass != 60 || roomLook.RoomReflections != 25 || roomLook.RoomLight != 50)
            throw new Exception("The glass, reflections and room light sliders are not mapped to settings");
        LightColourList.SelectedItem = Loc.Get("LightNeutral");
        if (ReadProfile().RoomLightColor != "#FFD1A3") throw new Exception("A room light colour preset is not mapped to settings");
        LightColourList.SelectedItem = Loc.Get("LightWarm");
        if (ReadProfile().RoomLightColor != "#FFA957") throw new Exception("The warm room light preset is not mapped to settings");
        LightColourList.SelectedItem = Loc.Get("LightDaylight");
        if (ReadProfile().RoomLightColor != "#FFF9FD") throw new Exception("The daylight room light preset is not mapped to settings");
        FollowCheck.IsChecked = true;
        if (GlassSlider.IsEnabled || ReflectSlider.IsEnabled || LightSlider.IsEnabled || LightColourList.IsEnabled ||
            GlassValue.Text != Loc.Get("NeedsFixedScreen") || ReflectValue.Text != Loc.Get("NeedsFixedScreen") || LightValue.Text != Loc.Get("NeedsFixedScreen"))
            throw new Exception("Glass, reflections and the room light must rest while the screen follows the head");
        if (ReadProfile().RoomGlass != 60 || ReadProfile().RoomLight != 50 || ReadProfile().RoomLightColor != "#FFF9FD")
            throw new Exception("Resting room controls must keep their values");
        FollowCheck.IsChecked = false;
        RoomSlider.Value = 0;
        if (GlassSlider.IsEnabled || GlassValue.Text != Loc.Get("NeedsRoom") || ReadProfile().RoomReflections != 25)
            throw new Exception("Glass, reflections and the room light must rest, keeping their values, with the room off");
        var customLight = ReadProfile();
        customLight.Room = 45; customLight.RoomLightColor = "#123456";
        PutProfile(customLight);
        if (LightColourList.SelectedItem as string != Loc.Format("LightCustom", "#123456") || !LightColourList.IsEnabled || ReadProfile().RoomLightColor != "#123456")
            throw new Exception("A saved room light colour that is no preset must show as Custom and be kept");
        LightColourList.SelectedItem = Loc.Get("LightSoftWhite");
        if (ReadProfile().RoomLightColor != "#FFB46B") throw new Exception("Picking a preset after a custom room light colour");
        PutProfile(one);
        if (GlassSlider.Value != 0 || LightSlider.Value != 0 || LightColourList.SelectedItem as string != Loc.Get("LightSoftWhite") ||
            LightColourList.Items.Contains(Loc.Format("LightCustom", "#123456")))
            throw new Exception("Loading another profile must put back its room look and drop the custom light colour");
        if (AmbiStrengthSlider.Value != 85 || AmbiStrengthSlider.IsEnabled) throw new Exception("Glow strength should default to 85 % and follow the ambilight checkbox");
        AmbilightCheck.IsChecked = true;
        if (!AmbiStrengthSlider.IsEnabled) throw new Exception("Glow strength must be adjustable with the ambilight on");
        AmbiStrengthSlider.Value = 40;
        if (ReadProfile().AmbilightStrength != 40) throw new Exception("The glow strength slider is not mapped to settings");
        AmbiStrengthSlider.Value = 85;
        AmbilightCheck.IsChecked = false;
        if (WorldHex.Text != "#000000" || WorldList.SelectedItem as string != Loc.Get("WorldBlack")) throw new Exception("The world should default to black");
        WorldList.SelectedItem = Loc.Get("WorldSlate");
        if (WorldHex.Text != "#2A3441" || ReadProfile().WorldColor != "#2A3441") throw new Exception("A world colour preset is not mapped to settings");
        WorldHex.Text = "#123456";
        if (WorldList.SelectedItem as string != Loc.Get("WorldCustom") || ReadProfile().WorldColor != "#123456") throw new Exception("A typed world colour must show as Custom and save");
        WorldHex.Text = "#12";
        bool rejected = false;
        try { ReadProfile(); } catch (InvalidDataException) { rejected = true; }
        if (!rejected) throw new Exception("A half-typed world colour must not be saved");
        if (WorldList.SelectedItem as string != Loc.Get("WorldCustom")) throw new Exception("A half-typed world colour must show as Custom");
        WorldList.SelectedItem = Loc.Get("WorldCharcoal");
        if (WorldHex.Text != "#1C1C1E") throw new Exception("Picking a preset after a half-typed colour must put a whole colour back");
        WorldHex.Text = "#1C";
        WorldHexLostFocus(WorldHex, new RoutedEventArgs());
        if (WorldHex.Text != "#1C1C1E") throw new Exception("Leaving the box with half a colour must restore the last whole one");
        WorldHex.Text = "#000000";
        if (WorldList.SelectedItem as string != Loc.Get("WorldBlack")) throw new Exception("Typing a preset's colour must select the preset");
        if (SubpixelCheck.IsChecked != true) throw new Exception("The sub-pixel warp should default on");
        SubpixelCheck.IsChecked = false;
        if (ReadProfile().SubpixelWarp) throw new Exception("Sub-pixel warp checkbox is not mapped to settings");
        SubpixelCheck.IsChecked = true;
        if (TimingList.SelectedIndex != 0) throw new Exception("Game frame timing should default to the latest frame");
        if (store.Load(one.ExecutablePath).DelayToDepth) throw new Exception("Profiles saved before delayed timing existed must load with it off");
        TimingList.SelectedIndex = 1;
        if (!ReadProfile().DelayToDepth || ReadProfile().MatchFrameToDepth) throw new Exception("Delayed timing is not mapped to settings");
        TimingList.SelectedIndex = 2;
        if (ReadProfile().DelayToDepth || !ReadProfile().MatchFrameToDepth) throw new Exception("Matched timing is not mapped to settings");
        PutProfile(two);
        if (TimingList.SelectedIndex != 2) throw new Exception("A saved frame-matching profile must show as matched to depth");
        PutProfile(one);
        TimingList.SelectedIndex = 2;
        ForegroundCheck.IsChecked = false;
        if (ReadProfile().ForegroundRefinement) throw new Exception("Foreground checkbox not mapped to saved settings");
        if (FastModelCheck.IsChecked != true) throw new Exception("Fast depth model should default on");
        // GPU list: two identical cards stay distinguishable, software adapters are hidden,
        // a saved card that is no longer present is shown rather than silently replaced.
        string[] gpuSample = ["ParseArgs noise", "GPU|0|0|24325|0|NVIDIA GeForce RTX 3090", "GPU|1|0|12115|0|NVIDIA GeForce RTX 3060",
            "GPU|2|1|12115|0|NVIDIA GeForce RTX 3060", "GPU|3|0|0|1|Microsoft Basic Render Driver"];
        var gpus = Gpus.Parse(gpuSample);
        string card2 = Loc.Format("GpuLabelCard", "NVIDIA GeForce RTX 3060", Loc.Format("GpuSizeGB", 12115 / 1024.0), 2);
        if (gpus.Count != 5 || gpus[3].Id != "name:NVIDIA GeForce RTX 3060#0" || gpus[4].Id != "name:NVIDIA GeForce RTX 3060#1" ||
            gpus[4].Label != card2 || !Loc.NeutralStrings()["GpuLabelCard"].Contains("card {2}") || gpus.Any(g => g.Label.Contains("Basic Render")) ||
            gpus[0].Label != Loc.Get("GpuSame") || gpus[2].Label != Loc.Format("GpuLabel", "NVIDIA GeForce RTX 3090", Loc.Format("GpuSizeGB", 24325 / 1024.0)))
            throw new Exception("GPU list parsing failed");
        SetGpuLines(gpuSample);
        if (DepthGpuList.SelectedValue as string != Gpus.Same) throw new Exception("Depth GPU should default to the game's GPU");
        DepthGpuList.SelectedValue = "name:NVIDIA GeForce RTX 3060#1";
        if (ReadProfile().DepthGpu != "name:NVIDIA GeForce RTX 3060#1") throw new Exception("Depth GPU list is not mapped to settings");
        ShowGpuChoice("name:Old Card#0");
        if (DepthGpuList.SelectedValue as string != "name:Old Card#0" || ((GpuChoice)DepthGpuList.SelectedItem).Label != Loc.Format("GpuNotFound", "Old Card"))
            throw new Exception("A missing saved GPU must stay selected and be marked not found");
        DepthGpuList.SelectedValue = Gpus.Same;
        if (VersionText.Text != DisplayVersion() || !VersionText.Text.StartsWith("v1.") || !Title.Contains(VersionText.Text))
            throw new Exception("Version is not shown in the header and window title");
        var real = Gpus.List();                                    // the engine's --list-gpus on this PC
        File.WriteAllLines(Path.Combine(output, "gpus.txt"), real.Select(g => $"{g.Id} | {g.Label}"));
        if (real.Count < 2 || real[0].Id != Gpus.Same || real[1].Id != Gpus.Auto) throw new Exception("Engine GPU listing failed");
        if (Gpus.ValidId("name:#0") || Gpus.ValidId("3") || !Gpus.ValidId("name:NVIDIA GeForce RTX 3060#1")) throw new Exception("Depth GPU id validation failed");
        if (SteadyCheck.IsChecked != true || FuseCheck.IsChecked != false) throw new Exception("Steady depth should default on and fusion off");
        SteadyCheck.IsChecked = false;
        if (ReadProfile().SteadyDepth || ReadProfile().FuseModels) throw new Exception("Steady depth checkbox is not mapped to settings");
        SteadyCheck.IsChecked = true;
        if (!ReadProfile().SteadyDepth) throw new Exception("Steady depth checkbox is not mapped to settings");
        FuseCheck.IsChecked = true;
        if (!ReadProfile().FuseModels) throw new Exception("Fusion checkbox is not mapped to settings");
        if (!FuseCheck.IsEnabled) throw new Exception("Fusion must be available with the fast depth model");
        FastModelCheck.IsChecked = false;
        if (ReadProfile().FastDepthModel) throw new Exception("Fast depth model checkbox is not mapped to settings");
        if (FuseCheck.IsEnabled) throw new Exception("Fusion needs ZipDepth: its checkbox must be disabled without the fast depth model");
        // Base settings: new games start from them, existing profiles are untouched,
        // and the game's own path/window never leak into the base.
        var baseSource = new Profile { ExecutablePath = one.ExecutablePath, PreferredWindowTitle = "Example game window",
            Width = 7.25, Distance = 2.5, Strength = 1.4, SubpixelWarp = false, DelayToDepth = true, FuseModels = true, MenuKey = 0x79 };
        store.SaveBase(baseSource);
        var fresh = store.Load(Path.Combine(output, "fresh", "game.exe"));
        if (fresh.Width != 7.25 || fresh.Strength != 1.4 || fresh.SubpixelWarp || !fresh.DelayToDepth || !fresh.FuseModels || fresh.MenuKey != 0x79)
            throw new Exception("A new game must start from the saved base settings");
        if (fresh.ExecutablePath != Path.Combine(output, "fresh", "game.exe") || fresh.PreferredWindowTitle.Length != 0)
            throw new Exception("Base settings must not carry another game's path or window");
        if (store.Load(two.ExecutablePath).Width != 4) throw new Exception("Base settings must not change games that already have a profile");
        File.WriteAllText(store.BaseFile, "{ not json");
        if (store.Load(Path.Combine(output, "broken", "game.exe")).Width != new Profile().Width)
            throw new Exception("An unreadable base must fall back to VRX's defaults");
        File.Delete(store.BaseFile);
        if (store.HasBase || store.Load(Path.Combine(output, "none", "game.exe")).Width != new Profile().Width)
            throw new Exception("With no base saved, new games must use VRX's defaults");

        // Apply to all: every saved game takes the settings and keeps its own path
        // and window; an unreadable profile is skipped and left as it was.
        var allStore = new ProfileStore(Path.Combine(output, "apply-all"));
        if (Directory.Exists(allStore.Root)) Directory.Delete(allStore.Root, true);
        var gameA = new Profile { ExecutablePath = Path.Combine(output, "a", "a.exe"), PreferredWindowTitle = "Game A", Width = 3 };
        var gameB = new Profile { ExecutablePath = Path.Combine(output, "b", "b.exe"), PreferredWindowTitle = "Game B", Width = 4, FuseModels = true };
        allStore.Save(gameA); allStore.Save(gameB);
        string broken = Path.Combine(allStore.Root, "profiles", "broken.json");
        File.WriteAllText(broken, "{ not json");
        var chosen = new Profile { ExecutablePath = Path.Combine(output, "c", "c.exe"), Width = 8.5, ScreenCurve = 30, Ambilight = true, SteadyDepth = false };
        if (allStore.ApplyToAll(chosen, out int skippedAll, out int failedAll) != 2 || skippedAll != 1 || failedAll != 0)
            throw new Exception("Apply to all must update both saved games and skip the unreadable one");
        var afterA = allStore.Load(gameA.ExecutablePath);
        var afterB = allStore.Load(gameB.ExecutablePath);
        if (afterA.Width != 8.5 || afterB.Width != 8.5 || afterA.ScreenCurve != 30 || !afterB.Ambilight || afterB.SteadyDepth || afterB.FuseModels)
            throw new Exception("Apply to all did not copy the settings to every saved game");
        if (afterA.PreferredWindowTitle != "Game A" || afterB.PreferredWindowTitle != "Game B")
            throw new Exception("Apply to all must keep each game's own window");
        if (File.ReadAllText(broken) != "{ not json") throw new Exception("Apply to all must leave an unreadable profile untouched");
        if (File.Exists(allStore.FileFor(chosen.ExecutablePath))) throw new Exception("Apply to all must not create profiles for games that were never set up");
        // A game whose file cannot be written (read-only) keeps its settings; the others still change.
        var lockedFile = allStore.FileFor(gameA.ExecutablePath);
        var otherGame = new Profile { ExecutablePath = gameB.ExecutablePath, PreferredWindowTitle = "Game B", Width = 2 };
        allStore.Save(otherGame);
        File.SetAttributes(lockedFile, File.GetAttributes(lockedFile) | FileAttributes.ReadOnly);
        int appliedLocked, skippedLocked, failedLocked;
        try { appliedLocked = allStore.ApplyToAll(new Profile { ExecutablePath = chosen.ExecutablePath, Width = 9 }, out skippedLocked, out failedLocked); }
        finally { File.SetAttributes(lockedFile, File.GetAttributes(lockedFile) & ~FileAttributes.ReadOnly); }
        if (appliedLocked != 1 || failedLocked != 1 || allStore.Load(gameB.ExecutablePath).Width != 9 || allStore.Load(gameA.ExecutablePath).Width != 8.5)
            throw new Exception($"Apply to all must carry on past a locked profile (applied {appliedLocked}, skipped {skippedLocked}, failed {failedLocked})");
        if (ApplyAllButton == null) throw new Exception("The Apply to all button is missing");

        SmokeTestAutoAttach(output, sample, one);
        SmokeTestModes(output, sample, one);
        await SmokeTestWindow(output);
        SmokeTestStrings(root);

        PutProfile(one);
        PathLabel.Text = one.ExecutablePath; Status.Text = "Preview · saved settings are isolated by executable path";
        UpdateButtons();
        await Dispatcher.InvokeAsync(() => { }, DispatcherPriority.ApplicationIdle);
        UpdateLayout();
        await SmokeTestScreenshots(output);
        SetMode(true);
        PutSections();
        await Dispatcher.InvokeAsync(() => { }, DispatcherPriority.ApplicationIdle);
        UpdateLayout();
        var bitmap = new RenderTargetBitmap((int)ActualWidth, (int)ActualHeight, 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(this);
        var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using (var file = File.Create(Path.Combine(output, "desktop-preview.png"))) encoder.Save(file);
        // The curved screen in the top view, so a glance at the smoke output shows it.
        CurveSlider.Value = 70; UpdateLayout();
        var curvedShot = new RenderTargetBitmap((int)ActualWidth, (int)ActualHeight, 96, 96, PixelFormats.Pbgra32);
        curvedShot.Render(this);
        var curvedEncoder = new PngBitmapEncoder(); curvedEncoder.Frames.Add(BitmapFrame.Create(curvedShot));
        using (var file = File.Create(Path.Combine(output, "desktop-curved.png"))) curvedEncoder.Save(file);
        CurveSlider.Value = 0;
        SettingsScroll.ScrollToEnd(); UpdateLayout();
        var bottom = new RenderTargetBitmap((int)ActualWidth, (int)ActualHeight, 96, 96, PixelFormats.Pbgra32);
        bottom.Render(this);
        var bottomEncoder = new PngBitmapEncoder(); bottomEncoder.Frames.Add(BitmapFrame.Create(bottom));
        using (var file = File.Create(Path.Combine(output, "desktop-controls.png"))) bottomEncoder.Save(file);
        sessionError = "Startup failure must stay visible"; Status.Text = sessionError;
        if (!SaveAndApply() || Status.Text != sessionError) throw new Exception("Saving hid the session error");
        RefreshApps();
        if (Status.Text != sessionError) throw new Exception("Refreshing hid the session error");
        if (Loc.Missing.Count > 0) throw new Exception("Strings asked for but missing from Strings.resx: " + string.Join(", ", Loc.Missing));
        File.WriteAllText(Path.Combine(output, "smoke-pass.txt"), "PASS: WPF loaded/rendered; process enumeration; profile round-trip and path isolation; shortcut conflicts; control snapshot emitted; session error survives save/refresh; auto-attach settings, rules and state machine (no real windows, no attach); Easy/Expert mode defaults, memory and auto-attach; sections round-trip; window: places remembered culture-independently and kept on screen, Easy compact and growing to its content, Expert back at its size, title bar buttons named; strings: " +
            Loc.NeutralStrings().Count.ToString(CultureInfo.InvariantCulture) + " keys, all referenced keys present, placeholders consistent, no literal text in MainWindow.xaml, pseudo-locale transforms every string; screenshots ui-easy-skin, ui-easy-skin-pseudo, ui-expert-skin, ui-expert-all-open, ui-expert-pseudo. No VR session started.");
    }

    // Easy | Expert and the sections: defaults for new and existing users, memory, Easy
    // turning auto-attach on, what each mode shows. No window is polled and nothing attaches.
    private void SmokeTestModes(string output, RunningApp sample, Profile one)
    {
        ArgumentNullException.ThrowIfNull(output);
        ArgumentNullException.ThrowIfNull(sample);
        ArgumentNullException.ThrowIfNull(one);
        System.Diagnostics.Debug.WriteLine("[Smoke] SmokeTestModes enter");

        ProfileStore FreshStore(string name)
        {
            var created = new ProfileStore(Path.Combine(output, name));
            if (Directory.Exists(created.Root)) Directory.Delete(created.Root, true);
            return created;
        }
        // A new install starts in Easy; the choice is remembered either way.
        var fresh = FreshStore("mode-fresh");
        if (fresh.ExistingUser() || fresh.LoadAppSettings().Mode != AppSettings.EasyMode || fresh.LoadAppSettings().IsExpert)
            throw new Exception("A new install must start in Easy");
        fresh.SaveAppSettings(new AppSettings { Mode = AppSettings.ExpertMode });
        if (fresh.LoadAppSettings().Mode != AppSettings.ExpertMode) throw new Exception("Expert must be remembered");
        fresh.SaveAppSettings(new AppSettings { Mode = AppSettings.EasyMode });
        if (fresh.LoadAppSettings().Mode != AppSettings.EasyMode) throw new Exception("Easy must be remembered, even though app settings now exist");
        if (File.ReadAllText(fresh.AppSettingsFile).Contains("IsExpert")) throw new Exception("IsExpert is derived and must not be saved");
        // Anyone who has used VRX before starts in Expert, so nothing disappears on them.
        var withProfile = FreshStore("mode-profile");
        withProfile.Save(new Profile { ExecutablePath = Path.Combine(output, "mode", "game.exe") });
        if (withProfile.LoadAppSettings().Mode != AppSettings.ExpertMode) throw new Exception("An existing user with a saved game must start in Expert");
        var withSettings = FreshStore("mode-settings");
        Directory.CreateDirectory(withSettings.Root);
        File.WriteAllText(withSettings.AppSettingsFile, "{\"AutoAttach\":false,\"AutoAttachSeconds\":7}");
        var before = withSettings.LoadAppSettings();
        if (before.Mode != AppSettings.ExpertMode || before.AutoAttach || before.AutoAttachSeconds != 7)
            throw new Exception("App settings from before the modes must open in Expert, keeping auto-attach as it was");
        var withBase = FreshStore("mode-base");
        withBase.SaveBase(new Profile());
        if (withBase.LoadAppSettings().Mode != AppSettings.ExpertMode) throw new Exception("An existing user with base settings must start in Expert");
        var withLast = FreshStore("mode-last");
        ProfileStore.AtomicWrite(Path.Combine(withLast.Root, "last-game.txt"), one.ExecutablePath);
        if (withLast.LoadAppSettings().Mode != AppSettings.ExpertMode) throw new Exception("An existing user with a last game must start in Expert");
        var corruptMode = FreshStore("mode-corrupt");
        Directory.CreateDirectory(corruptMode.Root);
        File.WriteAllText(corruptMode.AppSettingsFile, "{\"Mode\":\"Banana\"}");
        if (corruptMode.LoadAppSettings().Mode != AppSettings.ExpertMode) throw new Exception("An unknown mode in existing settings must open in Expert");
        corruptMode.SaveAppSettings(new AppSettings { Mode = "Banana" });
        if (File.ReadAllText(corruptMode.AppSettingsFile).Contains("Banana")) throw new Exception("An unknown mode must not be saved");

        // Sections: Screen, Around the screen and Room open by default; the rest closed;
        // saved states round-trip; unknown or missing names use the defaults.
        var sectionDefaults = new AppSettings();
        foreach (var (name, _) in SectionList())
        {
            bool wanted = name is AppSettings.SectionScreen or AppSettings.SectionAround or AppSettings.SectionRoom;
            if (sectionDefaults.SectionOpen(name) != wanted) throw new Exception($"Section {name} must default {(wanted ? "open" : "closed")}");
        }
        if (sectionDefaults.SectionOpen("Nonsense") || sectionDefaults.SectionOpen("")) throw new Exception("Unknown sections must be closed");
        fresh.SaveAppSettings(new AppSettings { Mode = AppSettings.ExpertMode, Sections = new() { [AppSettings.SectionGame] = true, [AppSettings.SectionScreen] = false, [AppSettings.SectionSession] = true } });
        var sectionsBack = fresh.LoadAppSettings();
        if (!sectionsBack.SectionOpen(AppSettings.SectionGame) || sectionsBack.SectionOpen(AppSettings.SectionScreen) || !sectionsBack.SectionOpen(AppSettings.SectionSession) ||
            !sectionsBack.SectionOpen(AppSettings.SectionRoom) || sectionsBack.SectionOpen(AppSettings.SectionDepth))
            throw new Exception("Section states must round-trip, with unsaved sections at their defaults");

        // The window: Easy hides the sections and turns auto-attach on; Expert shows them and
        // keeps auto-attach as it is. Both are saved.
        ExpertMode.IsChecked = true;
        AutoAttachCheck.IsChecked = false;
        UpdateLayout();
        if (!appSettings.IsExpert || store.LoadAppSettings().Mode != AppSettings.ExpertMode || SettingsScroll.Visibility != Visibility.Visible ||
            EasyPanel.Visibility == Visibility.Visible || EasyIntro.Visibility == Visibility.Visible || EasyRecenterButton.Visibility == Visibility.Visible)
            throw new Exception("Expert must show the sections and hide the Easy panel");
        if (store.LoadAppSettings().AutoAttach) throw new Exception("Auto-attach should be off before switching to Easy");
        EasyMode.IsChecked = true;
        UpdateLayout();
        var easySaved = store.LoadAppSettings();
        if (appSettings.IsExpert || easySaved.Mode != AppSettings.EasyMode || !easySaved.AutoAttach || AutoAttachCheck.IsChecked != true || !appSettings.AutoAttach)
            throw new Exception("Easy must turn auto-attach on and be remembered");
        if (SettingsScroll.Visibility == Visibility.Visible || SectionList().Any(s => s.Section.IsVisible) || AppList.IsVisible || WidthSlider.IsVisible)
            throw new Exception("Easy must hide the settings sections");
        if (!StartButton.IsVisible || !StopButton.IsVisible || !EasyRecenterButton.IsVisible || !GameCard.IsVisible || !EasyIntro.IsVisible || !Status.IsVisible)
            throw new Exception("Easy must show the game card, Attach / Play, Stop VR, Recenter, the explanation and the status");
        if (autoTimer.IsEnabled) throw new Exception("The smoke test must never poll the window in front, even in Easy");
        // The game card: waiting, then a countdown (the state machine only; nothing is shown
        // on screen and nothing attaches), then cancelled.
        if (CardState.Text != Loc.Get("CardWaiting")) throw new Exception($"The Easy card must be waiting for a game (shows '{CardState.Text}')");
        var inFront = new ForegroundSnapshot(0x4242, 4321, @"C:\Games\game.exe", "The Game", false, true, true, true, false, false, default, default, default, false);
        if (autoMachine.Step(new AutoAttachInput(true, false, inFront.Handle, false, true, 1000)) != AutoAttachAction.Countdown) throw new Exception("Card countdown setup");
        autoTarget = inFront;
        UpdateGameCard();
        if (CardName.Text != "The Game" || CardState.Text != Loc.Format("CardCountdown", autoMachine.Remaining) || autoMachine.Remaining != autoMachine.Seconds)
            throw new Exception("The card must show the game in front and the countdown");
        if (autoMachine.Step(new AutoAttachInput(false, false, inFront.Handle, false, true, 1001)) != AutoAttachAction.Cancel) throw new Exception("Card countdown cancel");
        autoTarget = null;
        UpdateGameCard();
        if (CardState.Text != Loc.Get("CardWaiting")) throw new Exception("After a cancelled countdown the card must be waiting again");
        ExpertMode.IsChecked = true;
        UpdateLayout();
        if (!appSettings.IsExpert || store.LoadAppSettings().Mode != AppSettings.ExpertMode || AutoAttachCheck.IsChecked != true || !store.LoadAppSettings().AutoAttach)
            throw new Exception("Back in Expert, auto-attach must stay as the checkbox says (on)");
        if (!SettingsScroll.IsVisible || !ScreenSection.IsVisible || EasyRecenterButton.IsVisible) throw new Exception("Expert must show the sections again");
        AutoAttachCheck.IsChecked = false;
        if (store.LoadAppSettings().AutoAttach || !appSettings.IsExpert) throw new Exception("Turning auto-attach off in Expert must be saved and keep Expert");
        UpdateGameCard();
        if (CardState.Text != Loc.Get("CardReady")) throw new Exception($"With a game and window chosen, the card must be ready (shows '{CardState.Text}')");

        // Sections in the window: each toggle is saved; PutSections puts saved states back;
        // opening the log after a failure is not remembered.
        foreach (var (name, section) in SectionList())
        {
            bool was = section.IsExpanded;
            section.IsExpanded = !was;
            if (store.LoadAppSettings().SectionOpen(name) != !was) throw new Exception($"Opening/closing section {name} must be saved");
            section.IsExpanded = was;
            if (store.LoadAppSettings().SectionOpen(name) != was) throw new Exception($"Section {name} must be saved back (status: {Status.Text})");
        }
        foreach (var (_, section) in SectionList()) section.IsExpanded = true;
        appSettings = store.LoadAppSettings();
        sectionsLoading = true;
        foreach (var (_, section) in SectionList()) section.IsExpanded = false;
        sectionsLoading = false;
        PutSections();
        if (SectionList().Any(s => !s.Section.IsExpanded)) throw new Exception("Saved section states must be put back");
        appSettings.Sections = null;
        SaveAppSettings();
        PutSections();
        foreach (var (name, section) in SectionList())
            if (section.IsExpanded != AppSettings.DefaultSections[name]) throw new Exception($"Section {name} must start at its default");
        ShowSessionDetails();
        if (!SessionDetails.IsExpanded || store.LoadAppSettings().SectionOpen(AppSettings.SectionSession))
            throw new Exception("Opening the log after a failure must show it without remembering it");
        PutSections();
        System.Diagnostics.Debug.WriteLine("[Smoke] SmokeTestModes exit");
    }

    // The window: remembered places (culture-independent, nonsense dropped), keeping on a
    // monitor that exists, Easy compact and Expert back at its size, and the title bar.
    // The window itself stays off screen; nothing is maximized.
    private async Task SmokeTestWindow(string output)
    {
        ArgumentNullException.ThrowIfNull(output);
        System.Diagnostics.Debug.WriteLine("[Smoke] SmokeTestWindow enter");
        async Task Settle()
        {
            await Dispatcher.InvokeAsync(() => { }, DispatcherPriority.ApplicationIdle);
            UpdateLayout();
        }

        // Places round-trip in any culture; a size or position that makes no sense is dropped.
        var places = new ProfileStore(Path.Combine(output, "window-places"));
        if (Directory.Exists(places.Root)) Directory.Delete(places.Root, true);
        var culture = CultureInfo.CurrentCulture;
        try
        {
            CultureInfo.CurrentCulture = new CultureInfo("de-DE");
            places.SaveAppSettings(new AppSettings
            {
                Mode = AppSettings.ExpertMode,
                ExpertWindow = new WindowPlace { Left = -1500.5, Top = 20.25, Width = 1000.5, Height = 820, Maximized = true },
                EasyWindow = new WindowPlace { Left = 300.5, Top = 200, Width = 5, Height = 5 },
            });
        }
        finally
        {
            CultureInfo.CurrentCulture = culture;
        }
        string json = File.ReadAllText(places.AppSettingsFile);
        if (!json.Contains("1000.5") || !json.Contains("-1500.5") || json.Contains("1000,5")) throw new Exception("Window places must be saved culture-independently: " + json);
        var placesBack = places.LoadAppSettings();
        if (placesBack.ExpertWindow is not { Left: -1500.5, Top: 20.25, Width: 1000.5, Height: 820, Maximized: true } ||
            placesBack.EasyWindow is not { Left: 300.5, Top: 200, Width: 0, Height: 0, Maximized: false })
            throw new Exception("Window places must round-trip (Expert: bounds and maximized; Easy: position only)");
        File.WriteAllText(places.AppSettingsFile, "{\"Mode\":\"Expert\",\"ExpertWindow\":{\"Left\":10,\"Top\":10,\"Width\":-5,\"Height\":400},\"EasyWindow\":{\"Left\":1e9,\"Top\":0}}");
        var nonsense = places.LoadAppSettings();
        if (nonsense.ExpertWindow != null || nonsense.EasyWindow != null || !nonsense.IsExpert) throw new Exception("Nonsense window places must be dropped, keeping the rest");

        // Keeping on screen (physical pixels): a primary monitor and one to its left.
        var primary = new ScreenRect(0, 0, 1920, 1040);
        var leftMonitor = new ScreenRect(-2560, -200, 0, 1240);
        ScreenRect[] monitors = [primary, leftMonitor];
        void ExpectClamp(ScreenRect window, bool resize, ScreenRect wanted, string what)
        {
            var got = WindowPlacement.Clamp(window, monitors, resize);
            if (got != wanted) throw new Exception($"Keep on screen, {what}: {window} -> {got}, wanted {wanted}");
        }
        ExpectClamp(new ScreenRect(100, 100, 1100, 900), true, new ScreenRect(100, 100, 1100, 900), "inside stays");
        ExpectClamp(new ScreenRect(-2000, 100, -1000, 900), true, new ScreenRect(-2000, 100, -1000, 900), "on the other monitor stays");
        ExpectClamp(new ScreenRect(5000, 100, 6000, 900), true, new ScreenRect(920, 100, 1920, 900), "monitor gone: onto the nearest");
        ExpectClamp(new ScreenRect(1800, 500, 2800, 1300), true, new ScreenRect(920, 240, 1920, 1040), "half off the edge: moved in");
        ExpectClamp(new ScreenRect(-100, -50, 2100, 1300), true, new ScreenRect(0, 0, 1920, 1040), "too big: shrunk to the work area");
        ExpectClamp(new ScreenRect(-100, -50, 2100, 1300), false, new ScreenRect(0, 0, 2200, 1350), "too big, no resizing: top-left on screen");
        ExpectClamp(new ScreenRect(-2000, -300, -1000, 500), true, new ScreenRect(-2000, -200, -1000, 600), "above the other monitor: moved down");
        if (WindowPlacement.Clamp(new ScreenRect(5, 5, 50, 50), [], true) != new ScreenRect(5, 5, 50, 50)) throw new Exception("No monitors: the window stays");
        if (!WindowPlacement.WorkAreas().Any(a => a.Area > 0)) throw new Exception("No monitor work area found");

        // The window: Expert at a size of the user's; Easy remembers it and is compact; back
        // in Expert that size returns.
        SetMode(true);
        Width = 1000;
        Height = 820;
        await Settle();
        SetMode(false);
        await Settle();
        if (appSettings.ExpertWindow is not { Width: 1000, Height: 820 } || store.LoadAppSettings().ExpertWindow is not { Width: 1000, Height: 820 })
            throw new Exception($"Leaving Expert must remember its bounds (got {appSettings.ExpertWindow})");
        if (SizeToContent != SizeToContent.Height || ResizeMode != ResizeMode.CanMinimize || MaxButton.IsVisible || WindowChrome.GetWindowChrome(this)?.ResizeBorderThickness != new Thickness(0))
            throw new Exception("Easy must size to its content and not be resizable or maximizable");
        if (ActualHeight > 0.6 * ExpertDefaultHeight || ActualWidth >= ExpertMinWidth)
            throw new Exception($"Easy must be compact: {ActualWidth:F0} x {ActualHeight:F0}");
        CheckNothingClipped("Easy", GameCard, StartButton, StopButton, EasyRecenterButton, EasyPanel, ModeSwitch, Subtitle, EasyIntro, Status, TitleBar, CloseButton);
        double easyWidth = ActualWidth, easyHeight = ActualHeight;
        SetMode(true);
        await Settle();
        if (Width != 1000 || Height != 820 || SizeToContent != SizeToContent.Manual || ResizeMode != ResizeMode.CanResize || !MaxButton.IsVisible || MinWidth != ExpertMinWidth ||
            WindowChrome.GetWindowChrome(this)?.ResizeBorderThickness != new Thickness(ExpertResizeBorder))
            throw new Exception($"Expert must come back resizable at its remembered size (got {Width} x {Height})");
        if (store.LoadAppSettings().EasyWindow is not { } easyPlace || easyPlace.Left != Left || easyPlace.Top != Top)
            throw new Exception("Leaving Easy must remember its position");

        // The title bar: VRX's own, with named, keyboard-reachable buttons usable in the chrome.
        var chrome = WindowChrome.GetWindowChrome(this);
        if (chrome == null || Math.Abs(chrome.CaptionHeight - TitleBar.ActualHeight) > 0.5 || TitleBar.ActualHeight > 40 || TitleVersion.Text.Length == 0)
            throw new Exception("The window must have VRX's own slim title bar with the version");
        foreach (var (button, key) in new[] { (MenuButton, "TitleMenu"), (MinButton, "TitleMinimize"), (MaxButton, "TitleMaximize"), (CloseButton, "TitleClose") })
        {
            if (AutomationProperties.GetName(button) != Loc.Get(key) || button.ToolTip as string != Loc.Get(key))
                throw new Exception($"Title bar button {button.Name} must be named '{Loc.Get(key)}' (is '{AutomationProperties.GetName(button)}')");
            if (!WindowChrome.GetIsHitTestVisibleInChrome(button) || !button.Focusable || !button.IsTabStop || !button.IsVisible)
                throw new Exception($"Title bar button {button.Name} must be clickable in the title bar and reachable by keyboard");
        }
        if (MaxGlyph.Data != MaximizeGlyph) throw new Exception("A window that is not maximized shows the maximize glyph");
        Width = ExpertDefaultWidth;
        Height = ExpertDefaultHeight;
        await Settle();
        File.WriteAllText(Path.Combine(output, "smoke-window.txt"), FormattableString.Invariant($"Easy {easyWidth:F0} x {easyHeight:F0}; Expert default {ExpertDefaultWidth:F0} x {ExpertDefaultHeight:F0} (WPF units = pixels at 100%)"));
        System.Diagnostics.Debug.WriteLine($"[Smoke] SmokeTestWindow exit: Easy {easyWidth:F0} x {easyHeight:F0}");
    }

    // Every element lies wholly inside the window and its own content fits (nothing cut off).
    private void CheckNothingClipped(string what, params FrameworkElement[] elements)
    {
        ArgumentNullException.ThrowIfNull(elements);
        var window = new Rect(0, 0, ActualWidth, ActualHeight);
        if (RootGrid.DesiredSize.Height > RootGrid.ActualHeight + RootGrid.Margin.Top + RootGrid.Margin.Bottom + 0.5 ||
            RootGrid.DesiredSize.Width > RootGrid.ActualWidth + RootGrid.Margin.Left + RootGrid.Margin.Right + 0.5)
            throw new Exception($"{what}: the content needs {RootGrid.DesiredSize} but has {RootGrid.ActualWidth:F0} x {RootGrid.ActualHeight:F0}");
        foreach (var element in elements)
        {
            if (!element.IsVisible) throw new Exception($"{what}: {element.Name} is not visible");
            var bounds = element.TransformToAncestor(this).TransformBounds(new Rect(element.RenderSize));
            if (bounds.Left < -0.5 || bounds.Top < -0.5 || bounds.Right > window.Right + 0.5 || bounds.Bottom > window.Bottom + 0.5)
                throw new Exception($"{what}: {element.Name} at {bounds} is outside the window {window.Width:F0} x {window.Height:F0}");
            if (element.DesiredSize.Width > element.RenderSize.Width + element.Margin.Left + element.Margin.Right + 0.5 ||
                element.DesiredSize.Height > element.RenderSize.Height + element.Margin.Top + element.Margin.Bottom + 0.5)
                throw new Exception($"{what}: {element.Name} needs {element.DesiredSize} but has {element.RenderSize}");
        }
    }

    // Strings: every key used exists, placeholders match the arguments, MainWindow.xaml has
    // no literal text, and the pseudo-locale transforms every string. Reads the sources, so
    // it runs from the repository.
    private static void SmokeTestStrings(string root)
    {
        ArgumentNullException.ThrowIfNull(root);
        System.Diagnostics.Debug.WriteLine("[Smoke] SmokeTestStrings enter");
        var strings = Loc.NeutralStrings();
        if (strings.Count < 150) throw new Exception($"Strings.resx has only {strings.Count} strings");
        var problems = new List<string>();
        var used = new HashSet<string>(StringComparer.Ordinal);
        int PlaceholderCount(string key) => Loc.Placeholders(strings[key]).DefaultIfEmpty(-1).Max() + 1;

        // Every string's placeholders are {0}..{n-1}, it formats with n arguments, and its
        // pseudo form keeps them, is bracketed, accented where possible and ~35 % longer.
        foreach (var (key, value) in strings)
        {
            var indexes = Loc.Placeholders(value);
            int count = indexes.DefaultIfEmpty(-1).Max() + 1;
            if (Enumerable.Range(0, count).Any(i => !indexes.Contains(i))) problems.Add($"{key}: placeholders not 0..{count - 1}");
            try { _ = string.Format(CultureInfo.InvariantCulture, value, Enumerable.Repeat((object)1.5, count).ToArray()); }
            catch (FormatException) { problems.Add($"{key}: does not format with {count} argument(s)"); }
            string pseudo = Loc.PseudoTransform(value);
            if (!pseudo.StartsWith('[') || !pseudo.EndsWith(']') || pseudo == value || pseudo.Length < value.Length * 1.3 ||
                !Loc.Placeholders(pseudo).OrderBy(i => i).SequenceEqual(indexes.OrderBy(i => i)))
                problems.Add($"{key}: pseudo form '{pseudo}'");
            if (value.Any(char.IsAsciiLetterLower) && pseudo[1..^1].Replace("·", "").Trim() == value) problems.Add($"{key}: pseudo form is not accented");
        }

        // MainWindow.xaml: {l:Tr Key} (no placeholders) and {l:TrValue Key, ...} (one).
        string source = Path.Combine(root, "desktop", "VRX.Desktop");
        string xaml = File.ReadAllText(Path.Combine(source, "MainWindow.xaml"));
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(xaml, @"\{l:(Tr|TrValue)\s+(\w+)"))
        {
            string key = m.Groups[2].Value;
            used.Add(key);
            if (!strings.ContainsKey(key)) { problems.Add($"MainWindow.xaml: missing key {key}"); continue; }
            int wanted = m.Groups[1].Value == "TrValue" ? 1 : 0;
            if (PlaceholderCount(key) != wanted) problems.Add($"MainWindow.xaml: {key} has {PlaceholderCount(key)} placeholder(s), wanted {wanted}");
        }
        // No literal text: every text-bearing attribute is a markup extension, and no element
        // has text content. (No symbols are exempt; even ↻ is in Strings.resx.)
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(xaml, @"\s(Content|Text|Header|ToolTip|Title)=""([^""]*)"""))
            if (!m.Groups[2].Value.StartsWith('{')) problems.Add($"MainWindow.xaml: literal {m.Groups[1].Value}=\"{m.Groups[2].Value}\"");
        string withoutComments = System.Text.RegularExpressions.Regex.Replace(xaml, "<!--.*?-->", "", System.Text.RegularExpressions.RegexOptions.Singleline);
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(withoutComments, @">\s*([^<\s][^<]*)<"))
            problems.Add($"MainWindow.xaml: literal element text '{m.Groups[1].Value.Trim()}'");

        // Code: Loc.Get with a literal key (no placeholders) and Loc.Format with a literal key
        // and as many arguments as placeholders; a conditional key (cond ? A : B) checks each.
        foreach (string file in Directory.GetFiles(source, "*.cs"))
        {
            string code = File.ReadAllText(file);
            string name = Path.GetFileName(file);
            foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(code, @"Loc\.(Get|Format)\("))
            {
                var args = SplitArguments(code, m.Index + m.Length);
                if (args.Count == 0) { problems.Add($"{name}: unreadable Loc call at {m.Index}"); continue; }
                var keys = System.Text.RegularExpressions.Regex.Matches(args[0], "\"(\\w+)\"").Select(k => k.Groups[1].Value).ToList();
                if (keys.Count == 0) continue;                     // a key from a variable (checked below)
                foreach (string key in keys)
                {
                    used.Add(key);
                    if (!strings.ContainsKey(key)) { problems.Add($"{name}: missing key {key}"); continue; }
                    int wanted = m.Groups[1].Value == "Get" ? 0 : args.Count - 1;
                    if (PlaceholderCount(key) != wanted) problems.Add($"{name}: {key} has {PlaceholderCount(key)} placeholder(s) but {wanted} argument(s)");
                }
            }
        }
        // Keys held in tables (the colour presets).
        foreach (var (key, _) in WorldPresets.Concat(LightPresets))
        {
            used.Add(key);
            if (!strings.ContainsKey(key) || PlaceholderCount(key) != 0) problems.Add($"Preset key {key} missing or has placeholders");
        }
        foreach (string key in strings.Keys)
            if (!used.Contains(key)) problems.Add($"Strings.resx: {key} is not used");
        if (problems.Count > 0) throw new Exception("Strings: " + string.Join("; ", problems.Take(25)) + (problems.Count > 25 ? $" (+{problems.Count - 25} more)" : ""));
        System.Diagnostics.Debug.WriteLine($"[Smoke] SmokeTestStrings exit: {strings.Count} strings, {used.Count} used");
    }

    // The arguments of a call whose "(" ends just before `start`, split at top-level commas
    // (strings, chars, brackets and nested calls respected). Empty when unreadable.
    private static List<string> SplitArguments(string code, int start)
    {
        var args = new List<string>();
        if (string.IsNullOrEmpty(code) || start < 0 || start > code.Length) return args;
        int depth = 0, from = start;
        for (int i = start; i < code.Length; i++)
        {
            char c = code[i];
            if (c == '"' || c == '\'')
            {
                bool verbatim = c == '"' && i > 0 && code[i - 1] == '@';
                for (i++; i < code.Length && code[i] != c; i++)
                    if (code[i] == '\\' && !verbatim) i++;
                continue;
            }
            if (c is '(' or '[' or '{') { depth++; continue; }
            if (c is ')' or ']' or '}')
            {
                if (depth > 0) { depth--; continue; }
                string last = code[from..i].Trim();
                if (last.Length > 0 || args.Count > 0) args.Add(last);
                return args;
            }
            if (c == ',' && depth == 0) { args.Add(code[from..i].Trim()); from = i + 1; }
        }
        return [];
    }

    // Screenshots for a look at the layout: Easy; Expert with the default sections; Expert
    // with every section open (the whole scrolling list); and the same in the pseudo-locale,
    // with every visible text checked for being transformed.
    private async Task SmokeTestScreenshots(string output)
    {
        ArgumentNullException.ThrowIfNull(output);
        System.Diagnostics.Debug.WriteLine("[Smoke] SmokeTestScreenshots enter");
        async Task Settle()
        {
            await Dispatcher.InvokeAsync(() => { }, DispatcherPriority.ApplicationIdle);
            UpdateLayout();
        }
        bool savedAuto = appSettings.AutoAttach;
        SetMode(false);
        Status.Text = Loc.Get("StatusReady");
        await Settle();
        SaveWindowShot(Path.Combine(output, "ui-easy-skin.png"));
        // Easy in the pseudo-locale: the compact window grows to fit the longer text.
        double plainWidth = ActualWidth, plainHeight = ActualHeight;
        Loc.SetPseudo(true);
        try
        {
            RefreshLanguage();
            Status.Text = Loc.Get("StatusReady");
            await Settle();
            SaveWindowShot(Path.Combine(output, "ui-easy-skin-pseudo.png"));
            CheckNothingClipped("Easy, pseudo-locale", GameCard, StartButton, StopButton, EasyRecenterButton, EasyPanel, ModeSwitch, Subtitle, EasyIntro, Status, TitleBar, CloseButton);
            if (ActualWidth < plainWidth || ActualHeight < plainHeight)
                throw new Exception($"Easy must grow for longer text: {plainWidth:F0} x {plainHeight:F0} -> {ActualWidth:F0} x {ActualHeight:F0}");
        }
        finally
        {
            Loc.SetPseudo(false);
            RefreshLanguage();
        }
        Status.Text = Loc.Get("StatusReady");
        await Settle();
        if (Math.Abs(ActualWidth - plainWidth) > 0.5 || Math.Abs(ActualHeight - plainHeight) > 0.5)
            throw new Exception($"Easy must shrink back after the pseudo-locale: {ActualWidth:F0} x {ActualHeight:F0}, was {plainWidth:F0} x {plainHeight:F0}");
        SetMode(true);
        appSettings.AutoAttach = savedAuto;
        autoSettingsLoading = true; AutoAttachCheck.IsChecked = savedAuto; autoSettingsLoading = false;
        SaveAppSettings();
        UpdateGameCard();
        appSettings.Sections = null;
        PutSections();
        SettingsScroll.ScrollToTop();
        await Settle();
        SaveWindowShot(Path.Combine(output, "ui-expert-skin.png"));
        sectionsLoading = true;
        foreach (var (_, section) in SectionList()) section.IsExpanded = true;
        sectionsLoading = false;
        await Settle();
        SaveStackedShot(Path.Combine(output, "ui-expert-all-open.png"));

        string savedPath = PathLabel.Text;
        Loc.SetPseudo(true);
        try
        {
            RefreshLanguage();
            Status.Text = Loc.Get("StatusReady");
            PathLabel.Text = Loc.Get("PathChoose");
            await Settle();
            SaveStackedShot(Path.Combine(output, "ui-expert-pseudo.png"));
            CheckPseudoTexts();
        }
        finally
        {
            Loc.SetPseudo(false);
            RefreshLanguage();
            PathLabel.Text = savedPath;
        }
        await Settle();
        System.Diagnostics.Debug.WriteLine("[Smoke] SmokeTestScreenshots exit");
    }

    // In the pseudo-locale every text a person can read must be transformed ("[...]"),
    // except data: game and window names, paths, the version and key names.
    private void CheckPseudoTexts()
    {
        var problems = new List<string>();
        var dataOwners = new DependencyObject[] { AppList, WindowList, RecenterKeys, MenuKeys, VersionText, TitleVersion, CardName, WorldHex, LogBox };
        void Walk(DependencyObject node)
        {
            if (dataOwners.Contains(node)) return;
            if (node is FrameworkElement element && element.ToolTip is string tip && !tip.StartsWith('['))
                problems.Add($"tooltip of {element.Name}: {tip[..Math.Min(40, tip.Length)]}");
            if (node is TextBlock block && !string.IsNullOrWhiteSpace(block.Text) && !block.Text.StartsWith('['))
                problems.Add($"text '{block.Text}'");
            for (int i = 0; i < VisualTreeHelper.GetChildrenCount(node); i++) Walk(VisualTreeHelper.GetChild(node, i));
        }
        Walk(this);
        foreach (var list in new[] { WorldList, LightColourList, DepthGpuList, TimingList })
            foreach (var item in list.Items)
            {
                string text = item is GpuChoice gpu ? gpu.Label : item is ComboBoxItem box ? box.Content as string ?? "" : item as string ?? "";
                if (!text.StartsWith('[')) problems.Add($"{list.Name} item '{text}'");
            }
        if (!Title.StartsWith('[')) problems.Add("window title");
        if (problems.Count > 0) throw new Exception("Pseudo-locale left text untransformed: " + string.Join("; ", problems.Distinct().Take(20)));
    }

    // The whole window: with the skin's title bar it is all client area.
    private void SaveWindowShot(string file)
    {
        ArgumentNullException.ThrowIfNull(file);
        int width = (int)Math.Ceiling(ActualWidth);
        int height = (int)Math.Ceiling(ActualHeight);
        var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(this);
        SavePng(bitmap, file);
    }

    // The header, the game card, the whole list of sections (not just what the scroll
    // viewer shows) and the status line, stacked.
    private void SaveStackedShot(string file)
    {
        var parts = new FrameworkElement[] { HeaderGrid, TopArea, SectionsPanel, Status };
        const double gap = 12, margin = 24;
        double width = parts.Max(p => p.ActualWidth) + 2 * margin;
        double height = parts.Sum(p => p.ActualHeight + gap) + 2 * margin;
        var drawing = new DrawingVisual();
        using (var dc = drawing.RenderOpen())
        {
            dc.DrawRectangle((Brush)FindResource("Page"), null, new Rect(0, 0, width, height));
            double y = margin;
            foreach (var part in parts)
            {
                if (part.ActualWidth <= 0 || part.ActualHeight <= 0) continue;
                // A VisualBrush maps the visual's drawn bounds onto the rectangle: draw it at
                // those bounds, 1:1.
                var bounds = VisualTreeHelper.GetDescendantBounds(part);
                if (!bounds.IsEmpty && bounds.Width > 0 && bounds.Height > 0)
                    dc.DrawRectangle(new VisualBrush(part), null, new Rect(margin + bounds.X, y + bounds.Y, bounds.Width, bounds.Height));
                y += part.ActualHeight + gap;
            }
        }
        var bitmap = new RenderTargetBitmap((int)Math.Ceiling(width), (int)Math.Ceiling(height), 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(drawing);
        SavePng(bitmap, file);
    }

    private static void SavePng(BitmapSource bitmap, string file)
    {
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var stream = File.Create(file);
        encoder.Save(stream);
    }

    // Auto-attach: the app settings file, the rules and the state machine, all on plain data.
    // Nothing here reads or drives a real window, and nothing attaches.
    private void SmokeTestAutoAttach(string output, RunningApp sample, Profile one)
    {
        ArgumentNullException.ThrowIfNull(output);
        ArgumentNullException.ThrowIfNull(sample);
        ArgumentNullException.ThrowIfNull(one);

        // The settings file: default off / 5 s, round-trip, clamping, missing or corrupt file.
        var appStore = new ProfileStore(Path.Combine(output, "app-settings"));
        if (Directory.Exists(appStore.Root)) Directory.Delete(appStore.Root, true);
        var defaults = appStore.LoadAppSettings();
        if (defaults.AutoAttach || defaults.AutoAttachSeconds != 5) throw new Exception("Auto-attach must default off, with a 5 s countdown");
        if (Path.GetFileName(appStore.AppSettingsFile) != "app-settings.json" || Path.GetDirectoryName(appStore.AppSettingsFile) != appStore.Root)
            throw new Exception("App settings must live in app-settings.json in the store root");
        appStore.SaveAppSettings(new AppSettings { AutoAttach = true, AutoAttachSeconds = 12 });
        var back = appStore.LoadAppSettings();
        if (!back.AutoAttach || back.AutoAttachSeconds != 12) throw new Exception("App settings did not round-trip");
        appStore.SaveAppSettings(new AppSettings { AutoAttach = true, AutoAttachSeconds = 99 });
        if (appStore.LoadAppSettings().AutoAttachSeconds != 30) throw new Exception("Saving must clamp the countdown to 30 s");
        File.WriteAllText(appStore.AppSettingsFile, "{\"AutoAttach\":true,\"AutoAttachSeconds\":1}");
        if (appStore.LoadAppSettings().AutoAttachSeconds != 3 || !appStore.LoadAppSettings().AutoAttach) throw new Exception("Loading must clamp the countdown to 3 s");
        File.WriteAllText(appStore.AppSettingsFile, "{ not json");
        var corrupt = appStore.LoadAppSettings();
        if (corrupt.AutoAttach || corrupt.AutoAttachSeconds != 5) throw new Exception("A corrupt app settings file must load as the defaults");
        File.WriteAllText(appStore.AppSettingsFile, "null");
        if (appStore.LoadAppSettings().AutoAttach) throw new Exception("A null app settings file must load as the defaults");
        File.WriteAllText(appStore.AppSettingsFile, "{\"AutoAttach\":true}");
        if (appStore.LoadAppSettings().AutoAttachSeconds != 5) throw new Exception("A missing countdown must load as 5 s");
        if (appStore.HasBase) throw new Exception("App settings must not be the base settings");
        if (appStore.HasProfile("") || appStore.HasProfile(Path.Combine(output, "nobody", "game.exe"))) throw new Exception("HasProfile without a profile");
        appStore.Save(new Profile { ExecutablePath = Path.Combine(output, "somebody", "game.exe") });
        if (!appStore.HasProfile(Path.Combine(output, "somebody", "game.exe"))) throw new Exception("HasProfile with a profile");

        // Qualifies: full screen / borderless, or a saved non-browser game in a window.
        var mon = new ScreenRect(0, 0, 1920, 1080);
        var mon2 = new ScreenRect(1920, -200, 4480, 1240);
        var windowed = new ScreenRect(100, 100, 1380, 820);
        var windowedClient = new ScreenRect(108, 131, 1372, 812);
        if (!AutoAttachRules.Qualifies("Game.exe", false, mon, mon, mon, true, false)) throw new Exception("A full-screen game must qualify");
        if (!AutoAttachRules.Qualifies("game", false, mon2, mon2, mon2, true, false)) throw new Exception("A borderless game on a second monitor must qualify");
        if (!AutoAttachRules.Qualifies("game.exe", false, new ScreenRect(-1, -1, 1921, 1081), new ScreenRect(0, 0, 1920, 1080), mon, true, false))
            throw new Exception("A borderless window slightly larger than its monitor must qualify");
        if (!AutoAttachRules.Qualifies("game.exe", false, new ScreenRect(-8, -31, 1928, 1088), new ScreenRect(0, 0, 1920, 1080), mon, true, false))
            throw new Exception("A window whose client area covers the monitor must qualify");
        if (AutoAttachRules.Qualifies("game.exe", false, windowed, windowedClient, mon, true, false)) throw new Exception("A windowed game with no profile must not qualify");
        if (!AutoAttachRules.Qualifies("game.exe", true, windowed, windowedClient, mon, true, false, false, out string savedReason) || savedReason != "saved profile")
            throw new Exception("A windowed game with a saved profile must qualify");
        foreach (string browser in new[] { "chrome.exe", "msedge.exe", "firefox.exe", "opera.exe", "brave.exe", "vivaldi.exe", "iexplore.exe" })
        {
            if (AutoAttachRules.Qualifies(browser, true, windowed, windowedClient, mon, true, false)) throw new Exception($"A windowed browser ({browser}) with a profile must not qualify");
            if (!AutoAttachRules.Qualifies(browser, false, mon, mon, mon, true, false)) throw new Exception($"A full-screen browser ({browser}) must qualify");
        }
        foreach (string excluded in new[] { "VRX.Desktop.exe", "xrplayer.exe", "xrapp5.exe", "steam.exe", "steamwebhelper.exe", "vrmonitor.exe", "vrserver.exe",
            "vrcompositor.exe", "vrdashboard.exe", "vrwebhelper.exe", "vrstartup.exe", "explorer.exe", @"C:\Windows\explorer.exe", "ShellExperienceHost.exe",
            "StartMenuExperienceHost.exe", "SearchHost.exe", "WindowsTerminal.exe", "cmd.exe", "powershell.exe", "pwsh.exe", "conhost.exe", "", "  " })
        {
            if (AutoAttachRules.Qualifies(excluded, true, mon, mon, mon, true, false)) throw new Exception($"An excluded process ({excluded}) must never qualify");
        }
        if (AutoAttachRules.Qualifies(null, true, mon, mon, mon, true, false)) throw new Exception("No process name must not qualify");
        if (AutoAttachRules.Qualifies("game.exe", true, mon, mon, mon, true, true)) throw new Exception("A minimized window must not qualify");
        if (AutoAttachRules.Qualifies("game.exe", true, mon, mon, mon, false, false)) throw new Exception("An invisible window must not qualify");
        // A normal captioned window that is maximized is not "full screen", even when it
        // reaches over an auto-hidden taskbar; its client area below the caption is < 98 %.
        var maxWindow = new ScreenRect(-8, -8, 1928, 1088);
        var maxClient = new ScreenRect(0, 31, 1920, 1080);
        if (AutoAttachRules.Qualifies("notepad.exe", false, maxWindow, maxClient, mon, true, false, true)) throw new Exception("A maximized captioned window must not count as full screen");
        if (AutoAttachRules.Qualifies("notepad.exe", false, new ScreenRect(0, 0, 1920, 1040), new ScreenRect(0, 31, 1920, 1040), mon, true, false))
            throw new Exception("A window above the taskbar must not count as full screen");
        if (!AutoAttachRules.Qualifies("game.exe", true, maxWindow, maxClient, mon, true, false, true)) throw new Exception("A maximized saved game must qualify");
        if (AutoAttachRules.CoversMonitor(mon, default) || AutoAttachRules.CoversMonitor(default, mon) || AutoAttachRules.CoversMonitor(mon2, mon))
            throw new Exception("CoversMonitor edge cases");
        var good = new ForegroundSnapshot(0x100, 4321, @"C:\Games\game.exe", "The Game", false, true, true, true, false, false, mon, mon, mon, false);
        if (!AutoAttachRules.Evaluate(good, out _)) throw new Exception("A full-screen game in front must qualify");
        if (AutoAttachRules.Evaluate(null, out _) || AutoAttachRules.Evaluate(good with { Handle = 0 }, out _) ||
            AutoAttachRules.Evaluate(good with { IsVrx = true }, out string vrxReason) || vrxReason != "VRX is in front" ||
            AutoAttachRules.Evaluate(good with { TopLevel = false }, out _) || AutoAttachRules.Evaluate(good with { Offerable = false }, out _) ||
            AutoAttachRules.Evaluate(good with { Minimized = true }, out _) || AutoAttachRules.Evaluate(good with { FullPath = @"C:\Program Files\Steam\steam.exe" }, out _))
            throw new Exception("Evaluate must refuse no window, VRX, child windows, windows RunningApps would not offer, minimized and excluded");
        if (!RunningApps.IsExcludedExecutable("XRPLAYER.EXE") || RunningApps.IsExcludedExecutable("game.exe") || RunningApps.IsExcludedExecutable(null) ||
            !RunningApps.IsCaptureWindow(true, 10, 10, "UnityWndClass") || RunningApps.IsCaptureWindow(true, 10, 10, "ConsoleWindowClass") ||
            RunningApps.IsCaptureWindow(false, 10, 10, "X") || RunningApps.IsCaptureWindow(true, 0, 10, "X"))
            throw new Exception("RunningApps' shared exclusions");

        // The state machine: idle -> counting -> attach -> attached -> spent.
        static AutoAttachInput At(nint window, double now, bool qualifies = true, bool vrx = false, bool session = false, bool enabled = true) =>
            new(enabled, session, window, vrx, qualifies, now);
        var m = new AutoAttachMachine { Seconds = 5 };
        void Expect(AutoAttachAction got, AutoAttachAction wanted, AutoAttachPhase phase, string what)
        {
            if (got != wanted || m.Phase != phase) throw new Exception($"Auto-attach state machine, {what}: got {got}/{m.Phase}, wanted {wanted}/{phase}");
        }
        Expect(m.Step(At(0x100, 0, enabled: false)), AutoAttachAction.None, AutoAttachPhase.Idle, "off");
        Expect(m.Step(At(0x100, 0)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "start");
        if (m.Target != 0x100 || m.Remaining != 5) throw new Exception("The countdown must start at 5 for the window in front");
        Expect(m.Step(At(0x100, 1.2)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "1.2 s");
        if (m.Remaining != 4) throw new Exception($"After 1.2 s the countdown must show 4 (shows {m.Remaining})");
        Expect(m.Step(At(0x100, 4.9)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "4.9 s");
        if (m.Remaining != 1) throw new Exception("Just before zero the countdown must show 1");
        Expect(m.Step(At(0x100, 5.0)), AutoAttachAction.Attach, AutoAttachPhase.Attached, "zero");
        Expect(m.Step(At(0x100, 6, session: true)), AutoAttachAction.None, AutoAttachPhase.Attached, "session running");
        m.MarkSpent(0x100);                                    // the session ended
        Expect(m.Step(At(0x100, 7)), AutoAttachAction.None, AutoAttachPhase.Spent, "spent, still in front");
        Expect(m.Step(At(0x999, 8, qualifies: false, vrx: true)), AutoAttachAction.None, AutoAttachPhase.Spent, "VRX in front");
        Expect(m.Step(At(0, 8.5, qualifies: false)), AutoAttachAction.None, AutoAttachPhase.Spent, "no window in front");
        Expect(m.Step(At(0x100, 9)), AutoAttachAction.None, AutoAttachPhase.Spent, "back from VRX: still spent");
        Expect(m.Step(At(0x200, 10, qualifies: false)), AutoAttachAction.None, AutoAttachPhase.Idle, "another window in front");
        Expect(m.Step(At(0x100, 11)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "refocused");
        Expect(m.Step(At(0x200, 12, qualifies: false)), AutoAttachAction.Cancel, AutoAttachPhase.Idle, "focus change");
        if (m.Target != 0) throw new Exception("A cancelled countdown must forget its window");
        Expect(m.Step(At(0x100, 13)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "again");
        Expect(m.Step(At(0x300, 14)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "another game in front");
        if (m.Target != 0x300 || m.Remaining != 5) throw new Exception("Switching to another qualifying game must restart the countdown for it");
        Expect(m.Step(At(0x300, 15, qualifies: false)), AutoAttachAction.Cancel, AutoAttachPhase.Idle, "minimized / closed / stops qualifying");
        Expect(m.Step(At(0x400, 16, vrx: true)), AutoAttachAction.None, AutoAttachPhase.Idle, "VRX in front never counts");
        Expect(m.Step(At(0x100, 17)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "counting before VRX comes to front");
        Expect(m.Step(At(0x400, 17.5, vrx: true)), AutoAttachAction.Cancel, AutoAttachPhase.Idle, "VRX to front cancels");
        Expect(m.Step(At(0x100, 18)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "counting before turning off");
        Expect(m.Step(At(0x100, 19, enabled: false)), AutoAttachAction.Cancel, AutoAttachPhase.Idle, "turned off");
        Expect(m.Step(At(0x100, 20)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "counting before a manual attach");
        Expect(m.Step(At(0x100, 21, session: true)), AutoAttachAction.Cancel, AutoAttachPhase.Attached, "a manual attach");
        Expect(m.Step(At(0x100, 22)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "counting before a failed attach");
        Expect(m.Step(At(0x100, 27)), AutoAttachAction.Attach, AutoAttachPhase.Attached, "attach that fails");
        m.MarkSpent(0x100);
        Expect(m.Step(At(0x100, 28)), AutoAttachAction.None, AutoAttachPhase.Spent, "no retry after a failed attach");
        Expect(m.Step(At(0x100, 60)), AutoAttachAction.None, AutoAttachPhase.Spent, "no retry later either");
        Expect(m.Step(At(0x500, 61)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "another game while one is spent");
        Expect(m.Step(At(0x100, 62)), AutoAttachAction.Countdown, AutoAttachPhase.Counting, "the failed one after it left the foreground");
        m.MarkSpent(0);
        if (m.Phase != AutoAttachPhase.Counting) throw new Exception("MarkSpent(0) must be ignored");
        m.Seconds = 1;
        if (m.Seconds != 3) throw new Exception("The countdown must be at least 3 s");
        m.Seconds = 100;
        if (m.Seconds != 30) throw new Exception("The countdown must be at most 30 s");

        if (Loc.NeutralStrings()["AutoCountdown"] != "Attaching VRX to {0} in {1} - switch window to cancel" ||
            CountdownText("Half-Life 2", "hl2.exe", 5) != Loc.Format("AutoCountdown", "Half-Life 2", 5) ||
            CountdownText("", "hl2.exe", 3) != Loc.Format("AutoCountdown", "hl2", 3) || CountdownText(null, null, 1) != Loc.Format("AutoCountdown", Loc.Get("AutoTheGame"), 1))
            throw new Exception("Countdown text");

        // The controls: on/off and the seconds, saved app-wide; the smoke test never polls.
        var shown = store.LoadAppSettings();
        if (AutoAttachCheck.IsChecked != shown.AutoAttach || (int)AutoAttachSlider.Value != shown.AutoAttachSeconds || AutoAttachValue.Text != Loc.Format("ValueSeconds", shown.AutoAttachSeconds))
            throw new Exception("The auto-attach controls must show the saved app settings");
        if (AutoAttachPanel.ToolTip is not string tip || tip != Loc.Get("AutoAttachTip") ||
            !Loc.NeutralStrings()["AutoAttachTip"].Contains("full screen") || !Loc.NeutralStrings()["AutoAttachTip"].Contains("cancel"))
            throw new Exception("The auto-attach controls need their explanation");
        AutoAttachCheck.IsChecked = true;
        AutoAttachSlider.Value = 12;
        var saved = store.LoadAppSettings();
        if (!saved.AutoAttach || saved.AutoAttachSeconds != 12 || AutoAttachValue.Text != Loc.Format("ValueSeconds", 12) || autoMachine.Seconds != 12)
            throw new Exception("The auto-attach controls are not saved");
        if (autoTimer.IsEnabled) throw new Exception("The smoke test must never poll the window in front");
        AutoAttachSlider.Value = 5;
        AutoAttachCheck.IsChecked = false;
        if (store.LoadAppSettings().AutoAttach || store.LoadAppSettings().AutoAttachSeconds != 5) throw new Exception("Turning auto-attach off is not saved");

        // Just looking at games in the list must not save profiles for them (a saved profile
        // makes a windowed game auto-attachable); changing a setting still saves.
        var look1 = new RunningApp(2001, "look1.exe", Path.Combine(output, "unsaved", "look1.exe"), [new GameWindow(51, "Look 1")], null);
        var look2 = new RunningApp(2002, "look2.exe", Path.Combine(output, "unsaved", "look2.exe"), [new GameWindow(52, "Look 2")], null);
        foreach (var look in new[] { look1, look2 })
            if (File.Exists(store.FileFor(look.FullPath))) File.Delete(store.FileFor(look.FullPath));
        refreshing = true; AppList.ItemsSource = new[] { look1, look2 }; refreshing = false;
        AppList.SelectedItem = look1;
        AppList.SelectedItem = look2;
        AppList.SelectedItem = look1;
        if (store.HasProfile(look1.FullPath) || store.HasProfile(look2.FullPath))
            throw new Exception("Selecting a game in the list must not save a profile for it");
        WidthSlider.Value = 6.5;
        if (!SaveAndApply() || !store.HasProfile(look1.FullPath) || store.HasProfile(look2.FullPath))
            throw new Exception("Changing a setting must save the game's profile");
        refreshing = true; AppList.ItemsSource = new[] { sample }; AppList.SelectedItem = sample; refreshing = false;
        profile = one; PutProfile(one); WindowList.ItemsSource = sample.Windows; WindowList.SelectedIndex = 0;
    }
}
