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
    private sealed record Shortcut(string Name, int Code);
    public MainWindow(bool smokeTest)
    {
        smoke = smokeTest;
        string data = smoke ? Path.Combine(EngineSession.RepositoryRoot(), "desktop", "out", "smoke-data") :
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VRX");
        store = new(data);
        InitializeComponent();
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
            sessionError = code == 0 ? "" : "VRX could not continue. " +
                (lastEngineError.Length > 0 ? lastEngineError[(lastEngineError.LastIndexOf(']') + 1)..].Trim() : "See Session details below.");
            UpdateButtons(); RefreshApps();
            Status.Text = code == 0 ? "Stopped · your settings are saved" : sessionError;
            if (code != 0) SessionDetails.IsExpanded = true;
        });
        try { string last = Path.Combine(store.Root, "last-game.txt"); if (File.Exists(last)) lastExecutable = File.ReadAllText(last); }
        catch (IOException) { }
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
            else { refreshTimer.Start(); await LoadGpusAsync(); }
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
        SaveAndApply();
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
        SteadyCheck.IsChecked = p.SteadyDepth;
        FuseCheck.IsChecked = p.FuseModels;
        ShowGpuChoice(p.DepthGpu);
        RecenterKeys.SelectedValue = p.RecenterKey; MenuKeys.SelectedValue = p.MenuKey;
        SettingsPanel.IsEnabled = true;
        loading = false; DrawPreview();
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
    private void SettingsChanged(object sender, RoutedEventArgs e)
    {
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

    // The top view shows the curve: the edges come towards the viewer (down the
    // canvas) by the sag of the arc, using the same geometry as the renderer
    // (screen_curve.h: kCurveMaxWrap 70 degrees, edges never nearer than 55 % of
    // the distance). A quadratic Bezier whose control point is one sag above the
    // ends passes exactly through the middle of the screen.
    private void DrawCurve(double centre, double y)
    {
        double fraction = CurveSlider.Value / 100.0;
        bool curved = fraction > 0;
        ScreenArc.Visibility = curved ? Visibility.Visible : Visibility.Collapsed;
        ScreenLine.Visibility = curved ? Visibility.Collapsed : Visibility.Visible;
        if (!curved) return;

        double wrap = fraction * 70 * Math.PI / 180;
        double sag = WidthSlider.Value / wrap * (1 - Math.Cos(wrap / 2));
        sag = Math.Min(sag, DistanceSlider.Value * 0.45) * 17;
        var figure = new PathFigure { StartPoint = new Point(ScreenLine.X1, y + sag) };
        figure.Segments.Add(new QuadraticBezierSegment(new Point(centre, y - sag), new Point(ScreenLine.X2, y + sag), true));
        var geometry = new PathGeometry();
        geometry.Figures.Add(figure);
        ScreenArc.Data = geometry;
    }
    private void PreviewDown(object sender, MouseButtonEventArgs e)
    {
        if (profile == null) return;
        var point = e.GetPosition(Preview);
        if (Math.Abs(point.Y - ScreenLine.Y1) > 18 || point.X < ScreenLine.X1 - 12 || point.X > ScreenLine.X2 + 12) return;
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
    private void StartClick(object sender, RoutedEventArgs e)
    {
        if (!SaveAndApply() || AppList.SelectedItem is not RunningApp app || WindowList.SelectedItem is not GameWindow window) return;
        try { sessionError = lastEngineError = ""; LogBox.Clear(); engine.Start(app, window, profile!, store.Root); Status.Text = "Starting VR · " + app.Name; }
        catch (Exception ex) { sessionError = ex.Message; Status.Text = sessionError; }
        UpdateButtons();
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
        if (!engine.Running) return;
        e.Cancel = true;
        await StopSession();
        if (engine.Running) { Status.Text = "VRX is still stopping. Close again after it stops."; return; }
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
        if (!new Profile().Control(0, 0, false).StartsWith("VRX 8 ") || !new Profile().Control(0, 0, false).TrimEnd().EndsWith(" 0 1 1 0 0 1 0 0") ||
            !original.Control(0, 0, false).TrimEnd().EndsWith(" 0 0 1 0 0 1 0 0") || !both.Control(0, 0, false).TrimEnd().EndsWith(" 1 1 1 0 1 0 0") ||
            !unsteady.Control(0, 0, false).TrimEnd().EndsWith(" 1 0 0 0 1 0 0") || !delayed.Control(0, 0, false).TrimEnd().EndsWith(" 0 1 1 0 1 1 0 0") ||
            !matchedWins.Control(0, 0, false).TrimEnd().EndsWith(" 1 1 1 0 0 1 0 0") || !wholePixel.Control(0, 0, false).TrimEnd().EndsWith(" 1 1 0 0 0 0 0") ||
            !curved.Control(0, 0, false).TrimEnd().EndsWith(" 1 65 1"))
            throw new Exception("Control snapshot must be v8 ending with the matched, fast-model, steady, fuse, delayed and sub-pixel flags, the curve percentage and the ambilight flag (see desktop_control.h)");
        if (!store.Load(one.ExecutablePath).SteadyDepth || store.Load(one.ExecutablePath).FuseModels)
            throw new Exception("Profiles saved before steady/fuse existed must load with steadying on and fusion off");
        if (!store.Load(one.ExecutablePath).SubpixelWarp)
            throw new Exception("Profiles saved before the sub-pixel warp existed must load with it on");
        if (store.Load(one.ExecutablePath).ScreenCurve != 0 || store.Load(one.ExecutablePath).Ambilight)
            throw new Exception("Profiles saved before the curve and ambilight existed must load flat, with no glow");
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
        CurveSlider.Value = 0;
        AmbilightCheck.IsChecked = false;
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
        File.WriteAllText(Path.Combine(output, "smoke-pass.txt"), "PASS: WPF loaded/rendered; process enumeration; profile round-trip and path isolation; shortcut conflicts; control snapshot emitted; session error survives save/refresh. No VR session started.");
    }
}
