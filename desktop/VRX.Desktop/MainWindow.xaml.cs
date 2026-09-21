using System.ComponentModel;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
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
    private sealed record Shortcut(string Name, int Code);
    public MainWindow(bool smokeTest)
    {
        smoke = smokeTest;
        string data = smoke ? Path.Combine(EngineSession.RepositoryRoot(), "desktop", "out", "smoke-data") :
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VRX");
        store = new(data);
        InitializeComponent();
        FillWorldList();
        FillLightColourList(Profile.DefaultRoomLightColor);
        WorldList.SelectedIndex = 0;
        ShowGpuChoice(Gpus.Same);
        // From <Version> in the project file, so the UI always matches the build.
        VersionText.Text = DisplayVersion();
        Title = $"VRX {VersionText.Text} — Desktop setup";
        if (smoke)
        {
            ShowActivated = false; ShowInTaskbar = false;
            WindowStartupLocation = WindowStartupLocation.Manual; Left = -20000; Top = -20000;
        }
        var keys = new List<Shortcut> { new("= / +", 0xBB), new("Home", 0x24), new("End", 0x23), new("Insert", 0x2D), new("Pause", 0x13) };
        for (int i = 1; i <= 24; i++) keys.Add(new("F" + i, 0x6F + i));
        for (int i = 0; i < 26; i++) keys.Add(new(((char)('A' + i)).ToString(), 'A' + i));
        RecenterKeys.ItemsSource = keys; MenuKeys.ItemsSource = keys;
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
            if (line.Contains("RunFrameLoop: enter")) Status.Text = "Playing · " + Path.GetFileName(profile?.ExecutablePath);
        });
        engine.Exited += code => Dispatcher.InvokeAsync(() =>
        {
            // No loops: the session's window is not auto-attached again until it has left
            // the foreground and come back.
            autoMachine.MarkSpent(sessionWindow);
            sessionWindow = 0;
            sessionError = code == 0 ? "" : "VRX could not continue. " +
                (lastEngineError.Length > 0 ? lastEngineError[(lastEngineError.LastIndexOf(']') + 1)..].Trim() : "See Session details below.");
            UpdateButtons(); RefreshApps();
            Status.Text = code == 0 ? "Stopped · your settings are saved" : sessionError;
            if (code != 0) SessionDetails.IsExpanded = true;
        });
        try { string last = Path.Combine(store.Root, "last-game.txt"); if (File.Exists(last)) lastExecutable = File.ReadAllText(last); }
        catch (IOException) { }
        appSettings = store.LoadAppSettings();
        PutAppSettings();
        autoTimer.Tick += (_, _) => AutoAttachTick();
        Closed += (_, _) => { autoTimer.Stop(); autoOverlay?.Close(); autoOverlay = null; };
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
        catch (Exception ex) { Status.Text = "Could not refresh applications: " + ex.Message; }
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
        profile = null; SettingsPanel.IsEnabled = false;
        var app = AppList.SelectedItem as RunningApp;
        WindowList.ItemsSource = app?.Windows;
        WindowList.SelectedIndex = app?.Windows.Count > 0 ? 0 : -1;
        PathLabel.Text = app?.FullPath.Length > 0 ? app.FullPath : "No accessible executable/window. Select a running game.";
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
                Status.Text = WindowList.SelectedItem == null ? "Choose which window to capture for " + app.Name : "Ready · settings for " + app.Name;
            }
            catch (Exception ex) { Status.Text = ex.Message; profile = null; SettingsPanel.IsEnabled = false; }
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

    // Replaces the Depth GPU list (e.g. once the engine has listed the GPUs), keeping
    // the current selection.
    private void SetGpuChoices(IReadOnlyList<GpuChoice> choices)
    {
        string current = DepthGpuList.SelectedValue as string ?? profile?.DepthGpu ?? Gpus.Same;
        gpuChoices = choices;
        ShowGpuChoice(current);
    }

    // Selects `id`. A saved GPU that is not in this PC gets a "Not found" entry, so the
    // choice stays visible rather than silently changing.
    private void ShowGpuChoice(string id)
    {
        if (!Gpus.ValidId(id)) id = Gpus.Same;
        var list = gpuChoices.ToList();
        if (!list.Any(c => c.Id == id)) list.Add(new GpuChoice(id, "Not found: " + Gpus.NameOf(id) + " (the game's GPU is used)"));
        bool wasLoading = loading;
        loading = true;
        DepthGpuList.ItemsSource = list;
        DepthGpuList.SelectedValue = id;
        loading = wasLoading;
    }

    private async Task LoadGpusAsync()
    {
        var list = await Task.Run(Gpus.List);
        SetGpuChoices(list);
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
        SettingsPanel.IsEnabled = true;
        loading = false; UpdateRoomControls(); DrawPreview();
    }
    private Profile ReadProfile()
    {
        if (profile == null) throw new InvalidOperationException("Select a game first.");
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
                throw new InvalidDataException("World colour must be six hex digits, like #1C1C1E. Changes are not saved until it is."),
            SteadyDepth = SteadyCheck.IsChecked == true,
            FuseModels = FuseCheck.IsChecked == true,
            DepthGpu = DepthGpuList.SelectedValue as string ?? Gpus.Same,
            AutoDismiss = DismissCheck.IsChecked == true, RecenterKey = (int)(RecenterKeys.SelectedValue ?? 0),
            MenuKey = (int)(MenuKeys.SelectedValue ?? 0) };
        if (!p.Valid()) throw new InvalidDataException("Choose two different shortcut keys. Changes are not saved until the settings are valid.");
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
                Status.Text = (engine.Running ? "Playing · saved for " : "Saved for ") + Path.GetFileName(p.ExecutablePath);
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
        RoomValue.Text = follows ? "Needs the fixed screen" : room == 0 ? "Off" : $"{room} %";
        if (GlassSlider == null || GlassValue == null || ReflectSlider == null || ReflectValue == null ||
            LightSlider == null || LightValue == null || LightColourList == null) return;
        bool withRoom = !follows && room > 0;
        string resting = follows ? "Needs the fixed screen" : "Needs the room";
        GlassSlider.IsEnabled = withRoom;
        ReflectSlider.IsEnabled = withRoom;
        LightSlider.IsEnabled = withRoom;
        int glass = (int)Math.Round(GlassSlider.Value), reflect = (int)Math.Round(ReflectSlider.Value), light = (int)Math.Round(LightSlider.Value);
        GlassValue.Text = !withRoom ? resting : glass == 0 ? "Solid" : $"{glass} % clear";
        ReflectValue.Text = !withRoom ? resting : reflect == 0 ? "Off" : $"{reflect} %";
        LightValue.Text = !withRoom ? resting : light == 0 ? "Off" : $"{light} %";
        LightColourList.IsEnabled = withRoom && light > 0;
    }
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

    // World colour presets; anything else is "Custom" and typed as #RRGGBB.
    private static readonly (string Name, string Hex)[] WorldPresets =
    [
        ("Black (default)", "#000000"), ("Charcoal", "#1C1C1E"), ("Slate", "#2A3441"), ("Midnight blue", "#0B1530"),
        ("Deep purple", "#1E0F2E"), ("Forest", "#0F2418"), ("Warm dark", "#2A1E14"), ("Cinema red", "#2B0A0A"), ("Grey", "#4A4A4A"),
        ("Dusk", "#33415C"), ("Overcast", "#5A6270"),
    ];
    private const string CustomColour = "Custom";
    private bool worldSyncing;
    private string lastWorldHex = "#000000";      // the last whole colour in the box

    private void FillWorldList()
    {
        WorldList.Items.Clear();
        foreach (var preset in WorldPresets) WorldList.Items.Add(preset.Name);
        WorldList.Items.Add(CustomColour);
    }

    // A preset picked: its colour goes into the box (Custom keeps whatever is there).
    private void WorldListChanged(object sender, SelectionChangedEventArgs e)
    {
        if (worldSyncing || WorldList.SelectedItem is not string name || name == CustomColour) return;
        foreach (var preset in WorldPresets)
            if (preset.Name == name) { WorldHex.Text = preset.Hex; return; }
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
            WorldList.SelectedItem = CustomColour;
            worldSyncing = false;
            return;
        }
        WorldSwatch.Fill = new SolidColorBrush(Color.FromRgb((byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb));
        string hex = Profile.FormatColor(rgb);
        lastWorldHex = hex;
        string match = CustomColour;
        foreach (var preset in WorldPresets) if (preset.Hex == hex) match = preset.Name;
        worldSyncing = true;
        WorldList.SelectedItem = match;
        worldSyncing = false;
        SettingsChanged(sender, e);
    }

    // Room light colours: blackbody sRGB values from the published Kelvin table. A saved
    // colour that is no preset (an edited profile) shows as one extra "Custom (#RRGGBB)"
    // item and is kept when saving.
    private static readonly (string Name, string Hex)[] LightPresets =
    [
        ("Warm (2700 K)", "#FFA957"), ("Soft white (3000 K)", Profile.DefaultRoomLightColor), ("Neutral (4000 K)", "#FFD1A3"),
        ("Daylight (6500 K)", "#FFF9FD"),
    ];
    private string? customLightHex;                // the kept non-preset colour, or null

    // Lists the presets (plus the custom colour, if `hex` is none of them) and selects `hex`.
    // An unreadable colour selects the default.
    private void FillLightColourList(string? hex)
    {
        if (LightColourList == null) return;
        string wanted = Profile.TryParseColor(hex, out int rgb) ? Profile.FormatColor(rgb) : Profile.DefaultRoomLightColor;
        bool wasLoading = loading;
        loading = true;                            // filling the list is not a change to save
        LightColourList.Items.Clear();
        customLightHex = null;
        string? select = null;
        foreach (var preset in LightPresets)
        {
            LightColourList.Items.Add(preset.Name);
            if (preset.Hex == wanted) select = preset.Name;
        }
        if (select == null)
        {
            customLightHex = wanted;
            select = $"Custom ({wanted})";
            LightColourList.Items.Add(select);
        }
        LightColourList.SelectedItem = select;
        loading = wasLoading;
    }

    // The selected preset's colour, or the kept custom one; the default if nothing is selected.
    private string SelectedLightColour()
    {
        if (LightColourList?.SelectedItem is not string name) return Profile.DefaultRoomLightColor;
        foreach (var preset in LightPresets)
            if (preset.Name == name) return preset.Hex;
        return customLightHex ?? Profile.DefaultRoomLightColor;
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
        SettingsPanel.IsEnabled = profile != null && !stopping;
        StartButton.IsEnabled = !running && profile != null && WindowList.SelectedItem is GameWindow;
        StopButton.IsEnabled = running && !stopping;
        RecenterButton.IsEnabled = DismissButton.IsEnabled = running && !stopping;
        AppList.IsEnabled = WindowList.IsEnabled = RefreshButton.IsEnabled = ShowAll.IsEnabled = !running;
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
            engine.Start(app, window, profile!, store.Root);
            sessionWindow = window.Handle; started = true;
            Status.Text = "Starting VR · " + app.Name;
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

    private void PutAppSettings()
    {
        autoSettingsLoading = true;
        AutoAttachCheck.IsChecked = appSettings.AutoAttach;
        AutoAttachSlider.Value = AppSettings.ClampSeconds(appSettings.AutoAttachSeconds);
        AutoAttachValue.Text = $"{(int)AutoAttachSlider.Value} s";
        autoMachine.Seconds = appSettings.AutoAttachSeconds;
        autoSettingsLoading = false;
    }

    private void SaveAppSettingsFromUi()
    {
        if (!ready || autoSettingsLoading || AutoAttachCheck == null || AutoAttachSlider == null || AutoAttachValue == null) return;
        appSettings.AutoAttach = AutoAttachCheck.IsChecked == true;
        appSettings.AutoAttachSeconds = AppSettings.ClampSeconds((int)Math.Round(AutoAttachSlider.Value));
        AutoAttachValue.Text = $"{appSettings.AutoAttachSeconds} s";
        autoMachine.Seconds = appSettings.AutoAttachSeconds;
        System.Diagnostics.Debug.WriteLine($"[AutoAttach] settings: on {appSettings.AutoAttach}, {appSettings.AutoAttachSeconds} s");
        try { store.SaveAppSettings(appSettings); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { Status.Text = "Could not save the auto-attach setting: " + ex.Message; }
        UpdateAutoTimer();
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
    public static string CountdownText(string? title, string? processName, int seconds)
    {
        string name = !string.IsNullOrWhiteSpace(title) ? title.Trim() : AutoAttachRules.BaseName(processName);
        if (name.Length == 0) name = "the game";
        if (name.Length > 60) name = name[..57] + "...";
        return $"Attaching VRX to {name} in {Math.Max(0, seconds)} - switch window to cancel";
    }

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
                return;
            }
            case AutoAttachAction.Cancel:
            {
                HideAutoOverlay();
                if (Status.Text == autoStatus) Status.Text = autoSavedStatus;
                autoTarget = null;
                autoStatus = "";
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
            Status.Text = "Auto-attach could not list applications: " + ex.Message;
            autoMachine.MarkSpent(target.Handle);
            return;
        }
        var app = list.FirstOrDefault(a => a.Pid == target.Pid && a.CanAttach && a.Windows.Any(w => w.Handle == target.Handle));
        if (app == null)
        {
            Status.Text = $"Auto-attach: {AutoAttachRules.BaseName(target.ProcessName)} is no longer available";
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
        Status.Text = "Auto-attached · starting VR · " + app.Name;
        System.Diagnostics.Debug.WriteLine($"[AutoAttach] started {app.Name}");
    }
    private async void StopClick(object sender, RoutedEventArgs e) => await StopSession();
    private async Task StopSession()
    {
        if (!engine.Running || profile == null || stopping) return;
        SaveAndApply(); saveTimer.Stop();
        stopping = true; UpdateButtons(); Status.Text = "Stopping VR…";
        try { await engine.Stop(profile); }
        catch (Exception ex) { Status.Text = "Could not stop VR: " + ex.Message; }
        finally { stopping = false; UpdateButtons(); }
    }
    private void RecenterClick(object sender, RoutedEventArgs e)
    { if (SaveAndApply() && engine.Running) { engine.Update(profile!, reset: true); Status.Text = "Recenter requested · uses the current headset direction"; } }
    private void DismissClick(object sender, RoutedEventArgs e)
    { if (SaveAndApply() && engine.Running) { engine.Update(profile!, dismiss: true); Status.Text = "SteamVR menu dismissal requested"; } }
    private void ResetClick(object sender, RoutedEventArgs e)
    {
        if (profile == null) return;
        var start = store.BaseSettings();
        start.ExecutablePath = profile.ExecutablePath;
        start.PreferredWindowTitle = profile.PreferredWindowTitle;
        PutProfile(start);
        SaveAndApply();
        Status.Text = store.HasBase ? "Reset to your base settings" : "Reset to VRX's default settings";
    }
    // Keeps the settings shown here as the starting point for games not yet set up.
    private void MakeBaseClick(object sender, RoutedEventArgs e)
    {
        if (profile == null) { Status.Text = "Select a game first, then save its settings as the base."; return; }
        if (!SaveAndApply()) return;
        try
        {
            store.SaveBase(profile);
            Status.Text = "Base settings saved · games you have not set up yet will start from these";
        }
        catch (Exception ex) { Status.Text = "Could not save base settings: " + ex.Message; }
    }
    // Overwrites every saved game, so it asks first; Cancel is the default.
    private void ApplyAllClick(object sender, RoutedEventArgs e)
    {
        if (profile == null) { Status.Text = "Select a game first, then apply its settings to all games."; return; }
        if (!SaveAndApply()) return;

        int count = store.SavedProfileFiles().Count;
        string question = $"Apply the settings shown here to all {count} saved game{(count == 1 ? "" : "s")}?\n\n" +
            "Every game's screen placement, 3D strength, depth options, depth GPU and shortcut keys will be replaced " +
            "by these. Each game keeps its own path and window.\n\nThis cannot be undone. " +
            "Games you have not set up yet still start from the base settings.";
        var answer = MessageBox.Show(this, question, "Apply to all games", MessageBoxButton.OKCancel, MessageBoxImage.Warning, MessageBoxResult.Cancel);
        if (answer != MessageBoxResult.OK) { Status.Text = "Apply to all cancelled · nothing was changed"; return; }
        try
        {
            int applied = store.ApplyToAll(profile, out int skipped, out int failed);
            string text = skipped == 0 && failed == 0 ? $"Settings applied to all {applied} saved game{(applied == 1 ? "" : "s")}" :
                $"Settings applied to {applied} saved game{(applied == 1 ? "" : "s")}";
            if (skipped > 0) text += $" · {skipped} unreadable profile{(skipped == 1 ? "" : "s")} left unchanged";
            if (failed > 0) text += $" · {failed} could not be written (read-only or in use) and keep their old settings";
            Status.Text = text;
        }
        catch (Exception ex) { Status.Text = "Could not apply to all games: " + ex.Message; }
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
        SaveAndApply(); refreshTimer.Stop(); saveTimer.Stop();
        autoTimer.Stop(); HideAutoOverlay();
        if (!engine.Running) return;
        e.Cancel = true;
        await StopSession();
        if (engine.Running) { Status.Text = "VRX is still stopping. Close again after it stops."; UpdateAutoTimer(); return; }
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
        if (RoomSlider.Value != 0 || RoomValue.Text != "Off" || !RoomSlider.IsEnabled) throw new Exception("The room should default off, and be available");
        RoomSlider.Value = 45;
        if (ReadProfile().Room != 45 || RoomValue.Text != "45 %") throw new Exception("The room slider is not mapped to settings");
        FollowCheck.IsChecked = true;
        if (RoomSlider.IsEnabled || RoomValue.Text != "Needs the fixed screen") throw new Exception("The room must rest while the screen follows the head");
        FollowCheck.IsChecked = false;
        if (!RoomSlider.IsEnabled) throw new Exception("The room must come back with the fixed screen");
        RoomSlider.Value = 0;
        // Glass, reflections and the room light: they need the room and the fixed screen, and the
        // light colour also needs the light. Resting controls keep their values.
        if (GlassSlider.Value != 0 || ReflectSlider.Value != 0 || LightSlider.Value != 0 || GlassSlider.IsEnabled || ReflectSlider.IsEnabled ||
            LightSlider.IsEnabled || LightColourList.IsEnabled || GlassValue.Text != "Needs the room" || ReflectValue.Text != "Needs the room" ||
            LightValue.Text != "Needs the room")
            throw new Exception("Glass, reflections and the room light must default off and rest without the room");
        if (LightColourList.SelectedItem as string != "Soft white (3000 K)" || ReadProfile().RoomLightColor != "#FFB46B")
            throw new Exception("The room light colour should default to soft white");
        RoomSlider.Value = 45;
        if (!GlassSlider.IsEnabled || !ReflectSlider.IsEnabled || !LightSlider.IsEnabled || LightColourList.IsEnabled ||
            GlassValue.Text != "Solid" || ReflectValue.Text != "Off" || LightValue.Text != "Off")
            throw new Exception("Glass, reflections and the room light must be available with the room, the light colour only with the light");
        GlassSlider.Value = 60; ReflectSlider.Value = 25; LightSlider.Value = 50;
        if (GlassValue.Text != "60 % clear" || ReflectValue.Text != "25 %" || LightValue.Text != "50 %" || !LightColourList.IsEnabled)
            throw new Exception("Glass, reflections and room light value texts");
        var roomLook = ReadProfile();
        if (roomLook.RoomGlass != 60 || roomLook.RoomReflections != 25 || roomLook.RoomLight != 50)
            throw new Exception("The glass, reflections and room light sliders are not mapped to settings");
        LightColourList.SelectedItem = "Neutral (4000 K)";
        if (ReadProfile().RoomLightColor != "#FFD1A3") throw new Exception("A room light colour preset is not mapped to settings");
        LightColourList.SelectedItem = "Warm (2700 K)";
        if (ReadProfile().RoomLightColor != "#FFA957") throw new Exception("The warm room light preset is not mapped to settings");
        LightColourList.SelectedItem = "Daylight (6500 K)";
        if (ReadProfile().RoomLightColor != "#FFF9FD") throw new Exception("The daylight room light preset is not mapped to settings");
        FollowCheck.IsChecked = true;
        if (GlassSlider.IsEnabled || ReflectSlider.IsEnabled || LightSlider.IsEnabled || LightColourList.IsEnabled ||
            GlassValue.Text != "Needs the fixed screen" || ReflectValue.Text != "Needs the fixed screen" || LightValue.Text != "Needs the fixed screen")
            throw new Exception("Glass, reflections and the room light must rest while the screen follows the head");
        if (ReadProfile().RoomGlass != 60 || ReadProfile().RoomLight != 50 || ReadProfile().RoomLightColor != "#FFF9FD")
            throw new Exception("Resting room controls must keep their values");
        FollowCheck.IsChecked = false;
        RoomSlider.Value = 0;
        if (GlassSlider.IsEnabled || GlassValue.Text != "Needs the room" || ReadProfile().RoomReflections != 25)
            throw new Exception("Glass, reflections and the room light must rest, keeping their values, with the room off");
        var customLight = ReadProfile();
        customLight.Room = 45; customLight.RoomLightColor = "#123456";
        PutProfile(customLight);
        if (LightColourList.SelectedItem as string != "Custom (#123456)" || !LightColourList.IsEnabled || ReadProfile().RoomLightColor != "#123456")
            throw new Exception("A saved room light colour that is no preset must show as Custom and be kept");
        LightColourList.SelectedItem = "Soft white (3000 K)";
        if (ReadProfile().RoomLightColor != "#FFB46B") throw new Exception("Picking a preset after a custom room light colour");
        PutProfile(one);
        if (GlassSlider.Value != 0 || LightSlider.Value != 0 || LightColourList.SelectedItem as string != "Soft white (3000 K)" ||
            LightColourList.Items.Contains("Custom (#123456)"))
            throw new Exception("Loading another profile must put back its room look and drop the custom light colour");
        if (AmbiStrengthSlider.Value != 85 || AmbiStrengthSlider.IsEnabled) throw new Exception("Glow strength should default to 85 % and follow the ambilight checkbox");
        AmbilightCheck.IsChecked = true;
        if (!AmbiStrengthSlider.IsEnabled) throw new Exception("Glow strength must be adjustable with the ambilight on");
        AmbiStrengthSlider.Value = 40;
        if (ReadProfile().AmbilightStrength != 40) throw new Exception("The glow strength slider is not mapped to settings");
        AmbiStrengthSlider.Value = 85;
        AmbilightCheck.IsChecked = false;
        if (WorldHex.Text != "#000000" || WorldList.SelectedItem as string != "Black (default)") throw new Exception("The world should default to black");
        WorldList.SelectedItem = "Slate";
        if (WorldHex.Text != "#2A3441" || ReadProfile().WorldColor != "#2A3441") throw new Exception("A world colour preset is not mapped to settings");
        WorldHex.Text = "#123456";
        if (WorldList.SelectedItem as string != "Custom" || ReadProfile().WorldColor != "#123456") throw new Exception("A typed world colour must show as Custom and save");
        WorldHex.Text = "#12";
        bool rejected = false;
        try { ReadProfile(); } catch (InvalidDataException) { rejected = true; }
        if (!rejected) throw new Exception("A half-typed world colour must not be saved");
        if (WorldList.SelectedItem as string != "Custom") throw new Exception("A half-typed world colour must show as Custom");
        WorldList.SelectedItem = "Charcoal";
        if (WorldHex.Text != "#1C1C1E") throw new Exception("Picking a preset after a half-typed colour must put a whole colour back");
        WorldHex.Text = "#1C";
        WorldHexLostFocus(WorldHex, new RoutedEventArgs());
        if (WorldHex.Text != "#1C1C1E") throw new Exception("Leaving the box with half a colour must restore the last whole one");
        WorldHex.Text = "#000000";
        if (WorldList.SelectedItem as string != "Black (default)") throw new Exception("Typing a preset's colour must select the preset");
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
        var gpus = Gpus.Parse(["ParseArgs noise", "GPU|0|0|24325|0|NVIDIA GeForce RTX 3090", "GPU|1|0|12115|0|NVIDIA GeForce RTX 3060",
            "GPU|2|1|12115|0|NVIDIA GeForce RTX 3060", "GPU|3|0|0|1|Microsoft Basic Render Driver"]);
        if (gpus.Count != 5 || gpus[3].Id != "name:NVIDIA GeForce RTX 3060#0" || gpus[4].Id != "name:NVIDIA GeForce RTX 3060#1" ||
            !gpus[4].Label.Contains("card 2") || gpus.Any(g => g.Label.Contains("Basic Render")))
            throw new Exception("GPU list parsing failed");
        SetGpuChoices(gpus);
        if (DepthGpuList.SelectedValue as string != Gpus.Same) throw new Exception("Depth GPU should default to the game's GPU");
        DepthGpuList.SelectedValue = "name:NVIDIA GeForce RTX 3060#1";
        if (ReadProfile().DepthGpu != "name:NVIDIA GeForce RTX 3060#1") throw new Exception("Depth GPU list is not mapped to settings");
        ShowGpuChoice("name:Old Card#0");
        if (DepthGpuList.SelectedValue as string != "name:Old Card#0" || !((GpuChoice)DepthGpuList.SelectedItem).Label.StartsWith("Not found"))
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

        PutProfile(one);
        PathLabel.Text = one.ExecutablePath; Status.Text = "Preview · saved settings are isolated by executable path";
        UpdateButtons();
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
        File.WriteAllText(Path.Combine(output, "smoke-pass.txt"), "PASS: WPF loaded/rendered; process enumeration; profile round-trip and path isolation; shortcut conflicts; control snapshot emitted; session error survives save/refresh; auto-attach settings, rules and state machine (no real windows, no attach). No VR session started.");
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

        if (CountdownText("Half-Life 2", "hl2.exe", 5) != "Attaching VRX to Half-Life 2 in 5 - switch window to cancel" ||
            CountdownText("", "hl2.exe", 3) != "Attaching VRX to hl2 in 3 - switch window to cancel" || !CountdownText(null, null, 1).Contains("the game"))
            throw new Exception("Countdown text");

        // The controls: on/off and the seconds, saved app-wide; the smoke test never polls.
        var shown = store.LoadAppSettings();
        if (AutoAttachCheck.IsChecked != shown.AutoAttach || (int)AutoAttachSlider.Value != shown.AutoAttachSeconds || AutoAttachValue.Text != $"{shown.AutoAttachSeconds} s")
            throw new Exception("The auto-attach controls must show the saved app settings");
        if (AutoAttachPanel.ToolTip is not string tip || !tip.Contains("full screen") || !tip.Contains("cancel"))
            throw new Exception("The auto-attach controls need their explanation");
        AutoAttachCheck.IsChecked = true;
        AutoAttachSlider.Value = 12;
        var saved = store.LoadAppSettings();
        if (!saved.AutoAttach || saved.AutoAttachSeconds != 12 || AutoAttachValue.Text != "12 s" || autoMachine.Seconds != 12)
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
