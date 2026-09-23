using System.Globalization;
using System.Reflection;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;

namespace Vrx.Linux;

public partial class MainWindow : Window
{
    // Room light colours: the WPF app's blackbody presets. A profile colour that is none of
    // them shows as one extra "Custom (#RRGGBB)" item and is kept when saving.
    static readonly (string Name, string Hex)[] LightPresets =
    {
        ("Warm (2700 K)", "#FFA957"), ("Soft white (3000 K)", "#FFB46B"),
        ("Neutral (4000 K)", "#FFD1A3"), ("Daylight (6500 K)", "#FFF9FD"),
    };

    const double ExpertWidth = 1080, ExpertHeight = 900, ExpertMinWidth = 800, ExpertMinHeight = 640, EasyWidth = 760;
    const int MaxLogLines = 500;

    readonly EnginePaths paths = EnginePaths.Find();
    readonly string settingsPath;
    readonly DirectoryInfo sessionDirectory;
    readonly AppSettings appSettings;
    SortedDictionary<string, LinuxProfile> profiles;
    bool profilesWritable = true;             // false when the file could not be read
    string current;
    readonly List<string> lightHexes = new();
    bool loading = true;
    EngineSession? engine;
    bool stopping, playing;
    uint recenterCount;                       // Recenter requests in this session
    string lastError = "";
    readonly Queue<string> logLines = new();
    readonly DispatcherTimer liveTimer = new() { Interval = TimeSpan.FromMilliseconds(100) };
    readonly DispatcherTimer saveTimer = new() { Interval = TimeSpan.FromMilliseconds(400) };
    readonly DispatcherTimer killTimer = new() { Interval = TimeSpan.FromSeconds(5) };
    bool dragging;
    Action? confirmed;

    public MainWindow()
    {
        InitializeComponent();
        sessionDirectory = Directory.CreateTempSubdirectory("vrx-linux-");
        settingsPath = Path.Combine(sessionDirectory.FullName, "settings");
        appSettings = AppSettings.Load(AppSettings.DefaultPath);
        try
        {
            profiles = ProfileStore.Load(ProfileStore.DefaultPath);
        }
        catch (InvalidDataException error)
        {
            // Keep the file as it is; show the defaults until the user fixes or moves it.
            profiles = new(StringComparer.Ordinal) { ["Default"] = LinuxProfile.Default };
            profilesWritable = false;
            lastError = error.Message;
        }
        current = profiles.ContainsKey(appSettings.Profile) ? appSettings.Profile : profiles.Keys.First();

        var version = Assembly.GetExecutingAssembly().GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
        VersionText.Text = "Linux" + (string.IsNullOrEmpty(version) ? "" : " " + version.Split('+')[0]);
        foreach (var slider in new[] { WidthSlider, DistanceSlider, HeightSlider, HorizontalSlider, StrengthSlider })
        {
            slider.TickFrequency = 0.05;
            slider.IsSnapToTickEnabled = true;
        }
        foreach (var slider in new[] { RoomSlider, GlassSlider, ReflectSlider, LightSlider })
        {
            slider.TickFrequency = 1;
            slider.IsSnapToTickEnabled = true;
        }

        FillProfileLists();
        LoadProfile(current);
        (appSettings.Source == AppSettings.SourceWindow ? SourceWindow :
         appSettings.Source == AppSettings.SourceScreen ? SourceScreen : SourceAny).IsChecked = true;
        foreach (var section in Sections())
        {
            section.IsExpanded = appSettings.OpenSections.Contains(section.Name!);
            section.PropertyChanged += (_, e) => { if (e.Property == Section.IsExpandedProperty) SaveOpenSections(); };
        }
        SetMode(appSettings.IsExpert, save: false);
        loading = false;
        UpdateButtons();
        Status.Text = profilesWritable ? "Ready · choose a profile and press Attach / Play"
                                       : $"{lastError}. Showing defaults; profiles will not be saved.";

        EasyMode.IsCheckedChanged += (_, _) => { if (!loading && EasyMode.IsChecked == true) SetMode(false); };
        ExpertMode.IsCheckedChanged += (_, _) => { if (!loading && ExpertMode.IsChecked == true) SetMode(true); };
        foreach (var source in new[] { SourceWindow, SourceScreen, SourceAny })
            source.IsCheckedChanged += (_, _) => SourceChanged();
        foreach (var slider in new[] { WidthSlider, DistanceSlider, HeightSlider, HorizontalSlider, StrengthSlider,
                                       RoomSlider, GlassSlider, ReflectSlider, LightSlider })
            slider.ValueChanged += (_, _) => SettingsChanged();
        CudaCheck.IsCheckedChanged += (_, _) => SettingsChanged();
        LightColourList.SelectionChanged += (_, _) => SettingsChanged();
        CardProfile.SelectionChanged += (_, _) => ProfileChosen(CardProfile.SelectedItem as string);
        ProfileList.SelectionChanged += (_, _) => ProfileChosen(ProfileList.SelectedItem as string);
        StartButton.Click += (_, _) => Start();
        StopButton.Click += (_, _) => Stop();
        EasyRecenterButton.Click += (_, _) => Recenter();
        RecenterButton.Click += (_, _) => Recenter();
        NewProfileButton.Click += (_, _) => NewProfile();
        RenameButton.Click += (_, _) => RenameProfile();
        DeleteButton.Click += (_, _) => Confirm($"Delete the profile \"{current}\"?", DeleteProfile);
        ResetButton.Click += (_, _) => ResetProfile();
        BaseButton.Click += (_, _) => MakeBase();
        ApplyAllButton.Click += (_, _) => Confirm(
            $"Copy the settings shown here to all {profiles.Count} profiles? Each keeps its own name.", ApplyToAll);
        ConfirmYes.Click += (_, _) => { ConfirmLayer.IsVisible = false; confirmed?.Invoke(); confirmed = null; };
        ConfirmNo.Click += (_, _) => { ConfirmLayer.IsVisible = false; confirmed = null; };
        Preview.PointerPressed += PreviewDown;
        Preview.PointerMoved += PreviewMove;
        Preview.PointerReleased += (_, e) => { dragging = false; e.Pointer.Capture(null); };
        liveTimer.Tick += (_, _) => { liveTimer.Stop(); WriteLiveSettings(); };
        saveTimer.Tick += (_, _) => { saveTimer.Stop(); SaveCurrentProfile(); };
        killTimer.Tick += (_, _) =>
        {
            killTimer.Stop();
            AppendLog("Engine did not stop after SIGTERM; forcing shutdown.");
            engine?.ForceStop();
        };
        Closing += WindowClosing;
        // Developer aid: VRX_LINUX_SCREENSHOT=<directory> renders Easy and Expert to PNGs and exits.
        var screenshots = Environment.GetEnvironmentVariable("VRX_LINUX_SCREENSHOT");
        if (!string.IsNullOrEmpty(screenshots)) Opened += async (_, _) => await Screenshots(screenshots);
    }

    async Task Screenshots(string directory)
    {
        Directory.CreateDirectory(directory);
        var expert = appSettings.IsExpert;
        foreach (var (mode, file) in new[] { (false, "easy.png"), (true, "expert.png") })
        {
            SetMode(mode, save: false);
            await Task.Delay(1000);
            var size = new PixelSize((int)Math.Ceiling(Bounds.Width), (int)Math.Ceiling(Bounds.Height));
            using var bitmap = new Avalonia.Media.Imaging.RenderTargetBitmap(size);
            bitmap.Render(this);
            using var output = File.Create(Path.Combine(directory, file));
            bitmap.Save(output, new Avalonia.Media.Imaging.PngBitmapEncoderOptions());
        }
        // Every section open, at its full height.
        var open = Sections().ToDictionary(section => section, section => section.IsExpanded);
        foreach (var section in open.Keys) section.IsExpanded = true;
        await Task.Delay(1000);
        var panel = new PixelSize((int)Math.Ceiling(SectionsPanel.Bounds.Width), (int)Math.Ceiling(SectionsPanel.Bounds.Height));
        using (var bitmap = new Avalonia.Media.Imaging.RenderTargetBitmap(panel))
        {
            bitmap.Render(SectionsPanel);
            using var output = File.Create(Path.Combine(directory, "sections.png"));
            bitmap.Save(output, new Avalonia.Media.Imaging.PngBitmapEncoderOptions());
        }
        foreach (var (section, wasOpen) in open) section.IsExpanded = wasOpen;
        SetMode(expert, save: false);
        Close();
    }

    IEnumerable<Section> Sections() => SectionsPanel.Children.OfType<Section>();

    // ---- Easy | Expert -----------------------------------------------------------------

    // Easy shows the card, Attach / Play, Stop VR and Recenter in a compact window;
    // Expert shows every section in a large, resizable one. The mode is remembered.
    void SetMode(bool expert, bool save = true)
    {
        var wasLoading = loading;
        loading = true;
        EasyMode.IsChecked = !expert;
        ExpertMode.IsChecked = expert;
        loading = wasLoading;
        EasyPanel.IsVisible = EasyIntro.IsVisible = EasyRecenterButton.IsVisible = !expert;
        SettingsScroll.IsVisible = expert;
        // Easy sizes the window to its content, so the settings row must not fill it.
        RootGrid.RowDefinitions[2].Height = expert ? new GridLength(1, GridUnitType.Star) : GridLength.Auto;
        NameRow.IsVisible = expert;
        Subtitle.Text = expert ? "Choose a profile. Set up your screen." : "Play your game in VR. VRX does the setting up.";
        RootGrid.Margin = expert ? new Thickness(24, 20, 24, 18) : new Thickness(20, 16, 20, 14);
        HeaderGrid.Margin = new Thickness(0, 0, 0, expert ? 16 : 12);
        if (expert)
        {
            SizeToContent = SizeToContent.Manual;
            CanResize = true;
            MinWidth = ExpertMinWidth;
            MinHeight = ExpertMinHeight;
            Width = ExpertWidth;
            Height = ExpertHeight;
        }
        else
        {
            if (WindowState == WindowState.Maximized) WindowState = WindowState.Normal;
            MinWidth = 0;
            MinHeight = 0;
            CanResize = false;
            Width = EasyWidth;
            SizeToContent = SizeToContent.Height;
        }
        appSettings.Mode = expert ? AppSettings.ExpertMode : AppSettings.EasyMode;
        if (save) SaveAppSettings();
    }

    // ---- Profiles ----------------------------------------------------------------------

    void FillProfileLists()
    {
        var wasLoading = loading;
        loading = true;
        var names = profiles.Keys.ToList();
        CardProfile.ItemsSource = names;
        ProfileList.ItemsSource = names;
        CardProfile.SelectedItem = current;
        ProfileList.SelectedItem = current;
        loading = wasLoading;
    }

    void ProfileChosen(string? name)
    {
        if (loading || name == null || name == current || !profiles.ContainsKey(name)) return;
        FlushProfile();
        current = name;
        appSettings.Profile = name;
        SaveAppSettings();
        FillProfileLists();
        LoadProfile(name);
        if (engine != null) ScheduleLive();
        Status.Text = engine != null ? $"Playing · {name}" : $"Ready · {name}";
    }

    void LoadProfile(string name)
    {
        var p = profiles[name];
        var wasLoading = loading;
        loading = true;
        WidthSlider.Value = p.Width;
        DistanceSlider.Value = p.Distance;
        HeightSlider.Value = p.Height;
        HorizontalSlider.Value = p.Horizontal;
        StrengthSlider.Value = p.Strength;
        RoomSlider.Value = p.Room;
        GlassSlider.Value = p.Glass;
        ReflectSlider.Value = p.Reflect;
        LightSlider.Value = p.Light;
        CudaCheck.IsChecked = p.Cuda;
        FillLightColourList(p.LightColor);
        ProfileName.Text = name;
        loading = wasLoading;
        UpdateValues();
    }

    // Lists the presets (plus the custom colour, if `hex` is none of them) and selects `hex`.
    void FillLightColourList(string hex)
    {
        string wanted;
        try { wanted = LinuxProfile.NormalizeColor(hex); } catch (ArgumentException) { wanted = LinuxProfile.Default.LightColor; }
        var names = new List<string>();
        lightHexes.Clear();
        foreach (var preset in LightPresets)
        {
            names.Add(preset.Name);
            lightHexes.Add(preset.Hex);
        }
        if (!lightHexes.Contains(wanted))
        {
            names.Add($"Custom ({wanted})");
            lightHexes.Add(wanted);
        }
        LightColourList.ItemsSource = names;
        LightColourList.SelectedIndex = lightHexes.IndexOf(wanted);
    }

    LinuxProfile ReadProfile() => new(
        CudaCheck.IsChecked == true,
        Math.Round(WidthSlider.Value, 2), Math.Round(DistanceSlider.Value, 2), Math.Round(HeightSlider.Value, 2),
        Math.Round(HorizontalSlider.Value, 2), Math.Round(StrengthSlider.Value, 2),
        (int)Math.Round(RoomSlider.Value), (int)Math.Round(GlassSlider.Value), (int)Math.Round(ReflectSlider.Value),
        (int)Math.Round(LightSlider.Value),
        LightColourList.SelectedIndex >= 0 ? lightHexes[LightColourList.SelectedIndex] : LinuxProfile.Default.LightColor);

    void SettingsChanged()
    {
        UpdateValues();
        if (loading) return;
        saveTimer.Stop();
        saveTimer.Start();
        if (engine != null) ScheduleLive();
        UpdateButtons();
    }

    // Settings are saved in the selected profile as they change, as the WPF app saves them per game.
    void SaveCurrentProfile()
    {
        if (!profilesWritable) return;
        var profile = ReadProfile().Normalized();
        if (profiles.TryGetValue(current, out var saved) && saved == profile) return;
        profiles[current] = profile;
        SaveProfiles(engine != null ? $"Playing · saved for {current}" : $"Saved for {current}");
    }

    void FlushProfile()
    {
        if (!saveTimer.IsEnabled) return;
        saveTimer.Stop();
        SaveCurrentProfile();
    }

    bool SaveProfiles(string done)
    {
        if (!profilesWritable) { Status.Text = "Profiles could not be read, so they are not saved."; return false; }
        try
        {
            ProfileStore.Save(ProfileStore.DefaultPath, profiles);
            Status.Text = done;
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            Status.Text = $"Could not save profiles: {error.Message}";
            return false;
        }
    }

    string? EnteredName()
    {
        var name = (ProfileName.Text ?? "").Trim();
        if (name.Length is 0 or > 80) { Status.Text = "Enter a profile name of 1 to 80 characters."; return null; }
        return name;
    }

    void NewProfile()
    {
        if (EnteredName() is not string name) return;
        if (profiles.ContainsKey(name)) { Status.Text = $"There is already a profile called {name}."; return; }
        FlushProfile();
        profiles[name] = appSettings.Base ?? LinuxProfile.Default;
        current = name;
        appSettings.Profile = name;
        SaveAppSettings();
        FillProfileLists();
        LoadProfile(name);
        if (engine != null) ScheduleLive();
        SaveProfiles($"Made the profile {name} from the {(appSettings.Base != null ? "base" : "default")} settings");
    }

    void RenameProfile()
    {
        if (EnteredName() is not string name || name == current) return;
        if (profiles.ContainsKey(name)) { Status.Text = $"There is already a profile called {name}."; return; }
        FlushProfile();
        var old = current;
        profiles[name] = profiles[old];
        profiles.Remove(old);
        current = name;
        appSettings.Profile = name;
        SaveAppSettings();
        FillProfileLists();
        SaveProfiles($"Renamed {old} to {name}");
    }

    void DeleteProfile()
    {
        if (profiles.Count == 1) { Status.Text = "The last profile cannot be deleted."; return; }
        saveTimer.Stop();
        var old = current;
        profiles.Remove(old);
        current = profiles.Keys.First();
        appSettings.Profile = current;
        SaveAppSettings();
        FillProfileLists();
        LoadProfile(current);
        if (engine != null) ScheduleLive();
        SaveProfiles($"Deleted {old}");
    }

    void ResetProfile()
    {
        profiles[current] = appSettings.Base ?? LinuxProfile.Default;
        LoadProfile(current);
        if (engine != null) ScheduleLive();
        SaveProfiles(appSettings.Base != null ? "Reset to your base settings" : "Reset to VRX's default settings");
    }

    void MakeBase()
    {
        appSettings.Base = ReadProfile().Normalized();
        Status.Text = SaveAppSettings()
            ? "Base settings saved · new profiles will start from these"
            : "Could not save base settings";
    }

    void ApplyToAll()
    {
        var settings = ReadProfile().Normalized();
        foreach (var name in profiles.Keys.ToList()) profiles[name] = settings;
        SaveProfiles($"Applied these settings to all {profiles.Count} profiles");
    }

    void Confirm(string question, Action action)
    {
        ConfirmText.Text = question;
        confirmed = action;
        ConfirmLayer.IsVisible = true;
    }

    // ---- App settings --------------------------------------------------------------------

    void SourceChanged()
    {
        if (loading) return;
        appSettings.Source = SourceWindow.IsChecked == true ? AppSettings.SourceWindow :
                             SourceScreen.IsChecked == true ? AppSettings.SourceScreen : AppSettings.SourceAny;
        SaveAppSettings();
        UpdateButtons();
    }

    void SaveOpenSections()
    {
        if (loading) return;
        appSettings.OpenSections = Sections().Where(s => s.IsExpanded).Select(s => s.Name!).ToList();
        SaveAppSettings();
    }

    bool SaveAppSettings()
    {
        try { appSettings.Save(AppSettings.DefaultPath); return true; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    // ---- Values and the top view ---------------------------------------------------------

    void UpdateValues()
    {
        string Metres(double value) => value.ToString("F2", CultureInfo.CurrentCulture) + " m";
        string Percent(double value) => value.ToString("F0", CultureInfo.CurrentCulture) + " %";
        WidthValue.Text = Metres(WidthSlider.Value);
        DistanceValue.Text = Metres(DistanceSlider.Value);
        HeightValue.Text = Metres(HeightSlider.Value);
        HorizontalValue.Text = Metres(HorizontalSlider.Value);
        StrengthValue.Text = StrengthSlider.Value.ToString("F2", CultureInfo.CurrentCulture);
        RoomValue.Text = RoomSlider.Value < 0.5 ? "Off" : Percent(RoomSlider.Value);
        GlassValue.Text = Percent(GlassSlider.Value);
        ReflectValue.Text = Percent(ReflectSlider.Value);
        LightValue.Text = LightSlider.Value < 0.5 ? "Off" : Percent(LightSlider.Value);
        GlassSlider.IsEnabled = ReflectSlider.IsEnabled = LightSlider.IsEnabled = LightColourList.IsEnabled = RoomSlider.Value >= 0.5;
        DrawPreview();
    }

    // The WPF app's top view: the viewer at the bottom, the screen as a line.
    void DrawPreview()
    {
        double centre = 270 + HorizontalSlider.Value * 20, y = 150 - DistanceSlider.Value * 17;
        ScreenLine.StartPoint = new Point(centre - WidthSlider.Value * 10, y);
        ScreenLine.EndPoint = new Point(centre + WidthSlider.Value * 10, y);
    }

    void PreviewDown(object? sender, PointerPressedEventArgs e)
    {
        dragging = true;
        e.Pointer.Capture(Preview);
        PreviewMove(sender, e);
    }

    void PreviewMove(object? sender, PointerEventArgs e)
    {
        if (!dragging) return;
        var point = e.GetPosition(Preview);
        HorizontalSlider.Value = Math.Round(Math.Clamp((point.X - 270) / 20, -3, 3), 2);
        DistanceSlider.Value = Math.Round(Math.Clamp((150 - point.Y) / 17, 0.5, 8), 2);
    }

    // ---- The engine ----------------------------------------------------------------------

    void ScheduleLive()
    {
        liveTimer.Stop();
        liveTimer.Start();
    }

    bool WriteLiveSettings()
    {
        try
        {
            LiveSettings.Write(settingsPath, ReadProfile(), recenterCount);
            return true;
        }
        catch (Exception error) when (error is IOException or ArgumentException or UnauthorizedAccessException)
        {
            Status.Text = $"Cannot apply live settings: {error.Message}";
            AppendLog(Status.Text);
            return false;
        }
    }

    string SourceArgument => appSettings.Source switch
    {
        AppSettings.SourceWindow => "--source=window",
        AppSettings.SourceScreen => "--source=screen",
        _ => "--source=any",
    };

    void Start()
    {
        if (engine != null) return;
        if (!File.Exists(paths.Engine)) { Status.Text = $"Build the Linux engine first: {paths.Engine}"; return; }
        var cuda = CudaCheck.IsChecked == true;
        if (cuda && !File.Exists(paths.Model)) { Status.Text = $"Fetch the checked ZipDepth model first: {paths.Model}"; return; }
        FlushProfile();
        if (!WriteLiveSettings()) return;
        engine = new EngineSession(paths, settingsPath, cuda, SourceArgument);
        engine.Output += line => Dispatcher.UIThread.Post(() => EngineOutput(line));
        engine.Exited += code => Dispatcher.UIThread.Post(() => EngineExited(code));
        playing = stopping = false;
        lastError = "";
        AppendLog("Starting: " + engine.CommandLine);
        try
        {
            engine.Start();
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            AppendLog($"Engine process error: {error.Message}");
            engine.Dispose();
            engine = null;
            Status.Text = "Could not start VR. See Session details.";
            UpdateButtons();
            return;
        }
        CardState.Text = "Starting VR…";
        Status.Text = $"Starting VR · {current}";
        UpdateButtons();
    }

    // Puts the screen straight in front of where the headset is looking now: the
    // next snapshot carries a new counter, which the engine acts on once.
    void Recenter()
    {
        if (engine == null || stopping) return;
        liveTimer.Stop();
        ++recenterCount;
        if (WriteLiveSettings()) Status.Text = "Recenter requested · uses the current headset direction";
    }

    void Stop()
    {
        if (engine == null || stopping) return;
        stopping = true;
        Status.Text = "Stopping VR…";
        if (engine.RequestStop()) killTimer.Start();
        UpdateButtons();
    }

    // Follows the engine's log for the card: the chooser, then SteamVR's session states.
    void EngineOutput(string line)
    {
        AppendLog(line);
        if (line.StartsWith("Select a", StringComparison.Ordinal))
            CardState.Text = "Choose your game's window or screen in the sharing dialog";
        else if (line.StartsWith("First live source frame", StringComparison.Ordinal))
            CardState.Text = "Starting VR…";
        else if (line is "OpenXR session state: 4" or "OpenXR session state: 5")
        {
            if (!playing) Status.Text = $"Playing · {current}";
            playing = true;
        }
        else if (line.Contains("failed", StringComparison.OrdinalIgnoreCase) || line.Contains("select a source again") ||
                 line.StartsWith("No live capture") || line.Contains("not built") || line.Contains("lost"))
            lastError = line;
        UpdateButtons();
    }

    void EngineExited(int code)
    {
        killTimer.Stop();
        liveTimer.Stop();
        engine?.Dispose();
        engine = null;
        var wasStopping = stopping;
        stopping = playing = false;
        Status.Text = code == 0 || wasStopping ? "Stopped · your settings are saved"
            : $"VRX could not continue. {(lastError.Length > 0 ? lastError : $"The engine exited with status {code}.")}";
        AppendLog($"Engine exited with status {code}.");
        UpdateButtons();
    }

    void UpdateButtons()
    {
        var running = engine != null;
        StartButton.IsEnabled = !running;
        StopButton.IsEnabled = running && !stopping;
        RecenterButton.IsEnabled = EasyRecenterButton.IsEnabled = running && !stopping;
        CudaCheck.IsEnabled = !running;
        foreach (var source in new[] { SourceWindow, SourceScreen, SourceAny }) source.IsEnabled = !running;
        CardState.Text = running
            ? (stopping ? "Stopping VR…" : playing ? "Playing" : CardState.Text)
            : appSettings.Source switch
            {
                AppSettings.SourceWindow => "Ready · press Attach / Play and choose your game's window",
                AppSettings.SourceScreen => "Ready · press Attach / Play and choose the screen your game is on",
                _ => "Ready · press Attach / Play and choose a window or screen",
            };
    }

    void AppendLog(string line)
    {
        logLines.Enqueue(line);
        while (logLines.Count > MaxLogLines) logLines.Dequeue();
        var text = new StringBuilder();
        foreach (var entry in logLines) text.Append(entry).Append('\n');
        LogBox.Text = text.ToString();
        LogBox.CaretIndex = LogBox.Text.Length;
    }

    // Closing stops VR cleanly (SIGTERM, then a kill after 3 s) and removes the session files.
    void WindowClosing(object? sender, WindowClosingEventArgs e)
    {
        FlushProfile();
        if (engine != null)
        {
            if (engine.RequestStop() && !engine.WaitForExit(TimeSpan.FromSeconds(3))) engine.ForceStop();
            engine.WaitForExit(TimeSpan.FromSeconds(1));
            engine.Dispose();
            engine = null;
        }
        try { sessionDirectory.Delete(recursive: true); } catch (IOException) { }
    }
}
