using System.Globalization;
using System.IO;
using VRX.VrexBridge;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
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
public sealed partial class ControllerMenu : IDisposable
{
    // Versioned, same-user, session-specific IPC; see controller_menu_protocol.h.
    public const int Width = 960, Height = 768, HeaderBytes = 64, EventCount = 64;
    public const int PixelWidth = Width * 2, PixelHeight = Height * 2;
    public const int PixelsAt = HeaderBytes + EventCount * 16;
    public const int MaxSnapshots = 6, SnapshotMetaBytes = 1056, SnapshotPathChars = 512;
    public const int PixelBytes = PixelWidth * PixelHeight * 4, SnapshotsAt = PixelsAt + PixelBytes, SnapshotBytes = SnapshotMetaBytes + PixelBytes;
    public const int KeyboardAt = SnapshotsAt + MaxSnapshots * SnapshotBytes, KeyboardEventsAt = KeyboardAt + 24;
    public const int VrexAt = KeyboardEventsAt + 256 * 8, PlacementAt = VrexAt + 32, PlacementBytes = 40;
    public const int AudioAt = PlacementAt + (MaxSnapshots + 1) * PlacementBytes, AudioBytes = 16;
    private const int SmoothEdgesAt = AudioAt + (MaxSnapshots + 1) * AudioBytes;
    public const int Capacity = SmoothEdgesAt + 8;
    public const int Magic = 0x58524356;
    public const int TitleLeft = 48, TitleTop = 48, TitleHeight = 56, TitleRight = Width - 48;
    private readonly MemoryMappedFile map;
    private readonly MemoryMappedViewAccessor memory;
    private readonly Mutex mutex;
    private readonly DispatcherTimer timer = new(DispatcherPriority.Input) { Interval = TimeSpan.FromMilliseconds(50) };
    private long nextMenuRender;
    private readonly PanelScrollMotion scrolling = new();
    private long lastScrollTick = Environment.TickCount64, dragSampleTime;
    private bool contentDragCandidate, contentDragging;
    private double contentDragY, contentDragOffset, dragLastY, dragSpeed;
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
    private readonly CheckBox? smoothEdges;
    private readonly Action applyAll;
    private readonly RenderTargetBitmap bitmap = new(PixelWidth, PixelHeight, 192, 192, PixelFormats.Pbgra32);
    private readonly RenderTargetBitmap motionBitmap = new(Width, Height, 96, 96, PixelFormats.Pbgra32);
    private readonly byte[] motionPixels = new byte[Width * Height * 4];
    private readonly byte[] pixels = new byte[PixelBytes];
    private FrameworkElement? held;
    private readonly PanelFeedback.Hover beamHover = new();
    private bool enabled, disposed;
    private bool controlScreen;
    private Button controlScreenButton = null!;
    private Button backButton = null!;
    private Button rollupButton = null!;
    private readonly UniformGrid toolbar = new() { Columns = 4, Margin = new Thickness(0, 0, 0, 8) };
    private int generation;
    private int pageGeneration;
    private Action? back;
    private Action? currentPage;
    private string breadcrumb = "";
    private readonly IFloatingPanel?[] snapshots = new IFloatingPanel?[MaxSnapshots];
    private readonly int[] snapshotFrames = new int[MaxSnapshots];
    private readonly int[] pendingPanelKinds = Enumerable.Repeat(-1, MaxSnapshots).ToArray();
    private uint snapshotError;
    private string imagesFolder = Path.Combine(EngineSession.RepositoryRoot(), "images");
    private readonly VrViewCapture vrCapture;
    private readonly Button captureLauncher;
    private Button? vrScreenshotButton, vrVideoButton;
    private readonly TextBlock captureStatus = new() { TextWrapping = TextWrapping.Wrap };
    private readonly SwipePredictor predictor = new(Path.Combine(VrxDataDirectory.Root, "keyboard-words.json"));
    private IFloatingTextTarget? textTarget;
    private uint keyboardFocusSerial;
    private uint pendingArrange;
    private int observedPanel = -1;
    private (int Slot, FloatingBrowser? Browser, float X, float Y, long Time)? guidePanelTarget;
    private bool settingsVisibleForGuide;
    private bool guideShortcutWasDown;
    private long panelDwellStart, panelLastPoint;
    private float panelPointX, panelPointY;

    public ControllerMenu(string name, IReadOnlyList<Expander> sections, TextBlock desktopStatus,
        Button recenter, Action applyAll, CheckBox? stereo = null, string? placementFile = null,
        CheckBox? smoothEdges = null)
    {
        this.sections = sections;
        this.smoothEdges = smoothEdges;
        this.applyAll = applyAll;
        this.placementFile = placementFile;
        mutex = new(false, name + ".Lock");
        map = MemoryMappedFile.CreateOrOpen(name, Capacity, MemoryMappedFileAccess.ReadWrite);
        memory = map.CreateViewAccessor(0, Capacity, MemoryMappedFileAccess.ReadWrite);
        if (!Lock()) throw new InvalidOperationException("Controller menu IPC is busy");
        try
        {
            memory.WriteArray(0, new byte[HeaderBytes], 0, HeaderBytes);
            memory.Write(0, Magic); memory.Write(4, 7);
            memory.Write(20, -1.0f); memory.Write(24, -1.0f);
        }
        finally { mutex.ReleaseMutex(); }

        var root = new DockPanel { Margin = new(TitleLeft, TitleTop, Width - TitleRight, 48) };
        var top = new StackPanel();
        DockPanel.SetDock(top, Dock.Top);
        var title = new DockPanel();
        backButton = ActionButton(Loc.Get("ControllerMenuBack"), () => back?.Invoke());
        backButton.ToolTip = Loc.Get("ControllerMenuBack");
        System.Windows.Automation.AutomationProperties.SetName(backButton, Loc.Get("ControllerMenuBack"));
        FloatingPanelChrome.SetTitleIcon(backButton, "M18,3 L6,12 L18,21");
        backButton.Width = 56; backButton.Height = 48; backButton.Margin = new Thickness(4);
        DockPanel.SetDock(backButton, Dock.Left); title.Children.Add(backButton);
        var dragHint = new TextBlock { Text = Loc.Get("ControllerMenuDrag"), FontSize = 16,
            VerticalAlignment = VerticalAlignment.Center, Margin = new(12, 0, 0, 0) };
        DockPanel.SetDock(dragHint, Dock.Right); title.Children.Add(dragHint); title.Children.Add(heading);
        titleBar.Child = FloatingPanelChrome.Title(title, ActionButton(Loc.Get("SnapshotClose"), () =>
        {
            if (!Lock()) return;
            try { memory.Write(12, 0); } finally { mutex.ReleaseMutex(); }
        })); top.Children.Add(titleBar);
        interactionHint.ToolTip = Loc.Get("ControllerMenuHint");
        var breadcrumbRow = new DockPanel();
        rollupButton = ActionButton(Loc.Get("ControllerMenuCollapse"), () =>
        {
            bool collapse = toolbar.Visibility == Visibility.Visible;
            toolbar.Visibility = collapse ? Visibility.Collapsed : Visibility.Visible;
            FloatingPanelChrome.SetTitleIcon(rollupButton, collapse ? "M3,7 L12,17 L21,7" : "M3,17 L12,7 L21,17");
            rollupButton.ToolTip = Loc.Get(collapse ? "ControllerMenuExpand" : "ControllerMenuCollapse");
            System.Windows.Automation.AutomationProperties.SetName(rollupButton, rollupButton.ToolTip.ToString());
            restoreOffset = scroll.VerticalOffset;
        });
        FloatingPanelChrome.SetTitleIcon(rollupButton, "M3,17 L12,7 L21,17");
        rollupButton.ToolTip = Loc.Get("ControllerMenuCollapse");
        System.Windows.Automation.AutomationProperties.SetName(rollupButton, Loc.Get("ControllerMenuCollapse"));
        rollupButton.Width = 56; rollupButton.MinHeight = 36; rollupButton.Margin = new Thickness(4, 0, 4, 4);
        DockPanel.SetDock(rollupButton, Dock.Right); breadcrumbRow.Children.Add(rollupButton); breadcrumbRow.Children.Add(interactionHint);
        top.Children.Add(breadcrumbRow);
        top.Children.Add(toolbar);
        stereo ??= Window.GetWindow(sections.FirstOrDefault())?.FindName("StereoCheck") as CheckBox;
        if (stereo != null)
        {
            var indicator = new CheckBox { Content = Loc.Get("Quick3D"), IsHitTestVisible = false, Focusable = false, Margin = new(0) };
            Bind(indicator, ToggleButton.IsCheckedProperty, stereo, nameof(stereo.IsChecked));
            var quick3d = ActionButton(Loc.Get("Quick3D"),
                () => stereo.SetCurrentValue(ToggleButton.IsCheckedProperty, stereo.IsChecked != true));
            quick3d.Content = indicator;
            System.Windows.Automation.AutomationProperties.SetName(quick3d, Loc.Get("Quick3D"));
            Bind(quick3d, UIElement.IsEnabledProperty, stereo, nameof(stereo.IsEnabled));
            toolbar.Children.Add(quick3d);
        }
        var recenterCopy = ActionButton(Loc.Get("RecenterNow"), () => recenter.RaiseEvent(new RoutedEventArgs(Button.ClickEvent)));
        Bind(recenterCopy, UIElement.IsEnabledProperty, recenter, nameof(recenter.IsEnabled));
        toolbar.Children.Add(recenterCopy);
        controlScreenButton = ActionButton(Loc.Get("ControllerMenuControlScreen"), () =>
        {
            controlScreen = !controlScreen;
            controlScreenButton.Content = Loc.Get(controlScreen ? "ControllerMenuStopControlScreen" : "ControllerMenuControlScreen");
            interactionHint.Text = breadcrumb;
            if (controlScreen) { textTarget = null; SetKeyboardTarget(); }
        });
        controlScreenButton.ToolTip = Loc.Get("ControllerMenuScreenHint");
        toolbar.Children.Add(controlScreenButton);
        captureLauncher = ActionButton("Capture", ShowCapture);
        captureLauncher.Tag = "▧";
        toolbar.Children.Add(captureLauncher);
        vrCapture = new(System.Windows.Threading.Dispatcher.CurrentDispatcher, UpdateCaptureButtons);
        toolbar.Children.Add(ActionButton(Loc.Get("BrowserOpen"), () => OpenBrowser("https://www.google.com")));
        toolbar.Children.Add(ActionButton("Webcam", OpenWebcam));
        toolbar.Children.Add(ActionButton(Loc.Get("KeyboardOpen"), OpenKeyboard));
        toolbar.Children.Add(ActionButton(Loc.Get("ArrangeWindows"), () => pendingArrange++));
        toolbar.Children.Add(ActionButton(Loc.Get("GameHelpStart"), BeginGameHelp));
        var vrexLauncher = ActionButton("", OpenVrexChat);
        vrexLauncher.Tag = null;
        vrexLauncher.HorizontalContentAlignment = HorizontalAlignment.Center;
        vrexLauncher.ToolTip = "Open Vrex chat";
        System.Windows.Automation.AutomationProperties.SetName(vrexLauncher, "Open Vrex chat");
        vrexLauncher.Content = new Image { Width = 32, Height = 32,
            Source = new BitmapImage(new Uri("pack://application:,,,/Assets/vrex-badge.png")) };
        toolbar.Children.Add(vrexLauncher);
        toolbar.Children.Add(ActionButton("Ubuntu", OpenVrexUbuntu));
        var text = new FrameworkElementFactory(typeof(TextBlock));
        text.SetBinding(TextBlock.TextProperty, new Binding());
        text.SetValue(TextBlock.TextWrappingProperty, TextWrapping.Wrap);
        var toolbarLabel = new DataTemplate { VisualTree = text };
        foreach (Control control in toolbar.Children)
        {
            control.MinHeight = 68; control.Margin = new Thickness(4);
            control.FontSize = 18; control.Padding = new Thickness(12, 8, 12, 8);
            control.HorizontalAlignment = HorizontalAlignment.Stretch;
            if (control is Button { Content: string } button) button.ContentTemplate = toolbarLabel;
        }
        root.Children.Add(top);
        DockPanel.SetDock(status, Dock.Bottom);
        Bind(status, TextBlock.TextProperty, desktopStatus, nameof(TextBlock.Text));
        root.Children.Add(status);
        scroll.Content = body;
        root.Children.Add(FloatingPanelChrome.Scrollable(scroll));
        surface = new UserControl
        {
            Width = Width, Height = Height, FontSize = 21, FontFamily = new FontFamily("Segoe UI"),
            Foreground = Brushes.White, Content = FloatingPanelChrome.Wrap(root),
            Language = System.Windows.Markup.XmlLanguage.GetLanguage(CultureInfo.CurrentCulture.IetfLanguageTag)
        };
        Home();
        RestoreWorkspace();
        timer.Tick += (_, _) => Tick();
        timer.Start();
        VrexClient.Current.Changed+=VrexChanged;
    }

    private VRX.VrexBridge.VrexEvent? vrexPointing;
    private long vrexPointingUntil;

    private void VrexChanged(VRX.VrexBridge.VrexEvent message)
    {
        if (message.Type == "screen-point")
        {
            vrexPointing = message;
            vrexPointingUntil = Environment.TickCount64 + 4200;
            return;
        }
        if (message.Type == "pc-mode")
        {
            vrexPointing = null;
            foreach (var bubble in snapshots.OfType<FloatingVrexReply>().Where(bubble => bubble.Interactive && !bubble.WaitingForChoice))
                bubble.Dismiss();
            return;
        }
        if (message.Type is "stopped" or "chat-changed")
        {
            vrexPointing = null;
            foreach (var bubble in snapshots.OfType<FloatingVrexReply>()) bubble.Dismiss();
            return;
        }
        if (!snapshots.OfType<FloatingVrexChatPanel>().Any(panel => !panel.Closed)) return;
        if (message.Type == "suggestion" && !string.IsNullOrWhiteSpace(message.Text))
        {
            if (snapshots.OfType<FloatingVrexReply>().Any(panel => panel.WaitingForChoice)) return;
            DismissVrexPreviews();
            var client = VrexClient.Current;
            string context = message.Text;
            var options = (message.Choices is { Length: > 0 } ? message.Choices.Take(2) : ["Guide Me"])
                .Select(choice => (choice, (Action)(() =>
                {
                    if (message.ObservedSurface is "settings" or "browser")
                        client.Send(new("chat", $"Explain this in words: {choice}\nYour earlier observation was: {context}"));
                    else
                        _ = client.CapturePcAsync($"Guide me with this request: {choice}\nThe suggestion I selected was: {context}");
                }))).ToList();
            options.Add(("Not now", () => { }));
            var existing = snapshots.OfType<FloatingVrexReply>().FirstOrDefault(p => p.Interactive && !p.Closed);
            if (existing != null) existing.Show(context, options);
            else AddPanel(5, () => { var bubble = new FloatingVrexReply(); bubble.Show(context, options); return bubble; });
            return;
        }
        if(message.Type!="reply" || string.IsNullOrWhiteSpace(message.Text))return;
        if (message.Choices is { Length: > 0 } labels)
        {
            DismissVrexPreviews();
            var options = labels.Select(label => (label, (Action)(() => VrexClient.Current.ChooseReply(label)))).ToList();
            options.Add(("Not now", () => VrexClient.Current.Send(new("dismiss-choice"))));
            var dialogue = snapshots.OfType<FloatingVrexReply>().FirstOrDefault(panel => panel.Interactive && !panel.Closed);
            if (dialogue != null) dialogue.Show(message.Text, options, waitForChoice: true);
            else AddPanel(5, () => { var panel = new FloatingVrexReply(); panel.Show(message.Text, options, waitForChoice: true); return panel; });
            return;
        }
        if (snapshots.OfType<FloatingVrexReply>().Any(panel => panel.Interactive && !panel.Closed)) return;
        var caption=snapshots.OfType<FloatingVrexReply>().FirstOrDefault(p=>!p.Interactive && !p.Closed);
        if(caption!=null) {caption.Show(message.Text);return;}
        AddPanel(4,()=>{var panel=new FloatingVrexReply();panel.Show(message.Text);return panel;});

        void DismissVrexPreviews()
        {
            foreach (var panel in snapshots.OfType<FloatingVrexReply>().Where(panel => !panel.Interactive))
                panel.Dismiss();
        }
    }

    public void Enable(bool value)
    {
        enabled = value;
        if (!value)
        {
            foreach (var keyboard in snapshots.OfType<FloatingKeyboard>()) keyboard.StopVoice();
            controlScreen = false;
            controlScreenButton.Content = Loc.Get("ControllerMenuControlScreen");
            interactionHint.Text = breadcrumb;
        }
        held = null;
        if (!value) foreach (var snapshot in snapshots) snapshot?.Pointer(4, -1, -1);
        if (!Lock()) return; // Tick retries; a busy renderer must not block WPF.
        try { memory.Write(8, value ? 1 : 0); memory.Write(36, controlScreen ? 1 : 0); }
        finally { mutex.ReleaseMutex(); }
    }

    private bool Lock()
    {
        try { return mutex.WaitOne(0); }
        catch (AbandonedMutexException) { return true; }
    }

    private int AddPanel(int kind, Func<IFloatingPanel> create, int preferredSlot = -1)
    {
        if (!Lock()) return -1;
        int slot = -1;
        try
        {
            for (int i = 0; i < MaxSnapshots; i++)
                if ((preferredSlot < 0 || preferredSlot == i) && snapshots[i] == null && memory.ReadInt32(SnapshotsAt+i*SnapshotBytes+8) == 0 && memory.ReadInt32(SnapshotsAt+i*SnapshotBytes+12) == 0)
                {
                    slot = i;
                    if (preferredSlot < 0) memory.Write(PlacementAt + (i + 1) * PlacementBytes, 0);
                    memory.Write(SnapshotsAt+i*SnapshotBytes+12, 3);
                    break;
                }
        }
        finally { mutex.ReleaseMutex(); }
        if (slot < 0) { interactionHint.Text = Loc.Get("SnapshotLimit"); return -1; }
        try
        {
            snapshots[slot] = create(); snapshotFrames[slot] = 0;
            pendingPanelKinds[slot] = kind; // Tick publishes the bitmap and metadata together.
            return slot;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or
            System.ComponentModel.Win32Exception or FormatException or NotSupportedException)
        {
            snapshots[slot]?.Dispose(); snapshots[slot] = null;
            pendingPanelKinds[slot] = -2; // Retry releasing the reserved slot without blocking WPF.
            interactionHint.Text = ex.Message; return -1;
        }
    }
    private void OpenBrowser(string url)
    {
        int slot = AddPanel(1, () => new FloatingBrowser(FocusBrowser, OpenKeyboard, OpenBrowser, url));
        if (slot >= 0) FocusBrowser((FloatingBrowser)snapshots[slot]!);
    }
    private void OpenWebcam()
    {
        if (snapshots.OfType<FloatingBrowser>().Any(panel => panel.IsWebcam && !panel.Closed)) return;
        AddPanel(6, CreateWebcamPanel);
    }
    private static FloatingBrowser CreateWebcamPanel() =>
        new(_ => { }, () => { }, _ => { }, WebcamPage.Url, webcam: true);
    private void OpenVrexUbuntu()
    {
        for(int i=0;i<MaxSnapshots;i++) if(snapshots[i] is FloatingUbuntuPanel {Closed:false} existing)
        {
            textTarget=existing;SetKeyboardTarget();return;
        }
        int slot=AddPanel(3,CreateUbuntuPanel);
        if(slot>=0 && snapshots[slot] is FloatingUbuntuPanel panel)
        {
            textTarget=panel;SetKeyboardTarget();
        }
    }
    private FloatingUbuntuPanel CreateUbuntuPanel() => new(target => { textTarget = target; SetKeyboardTarget(); },
        openKeyboard: ShowKeyboardForInput);
    private FloatingVrexChatPanel CreateVrexChatPanel() => new(VrexClient.Current,
        target => { textTarget = target; SetKeyboardTarget(); }, ShowKeyboardForInput, CaptureGuidePanel, OpenBrowser, OpenVrexGrid);
    private bool OpenVrexGrid()
    {
        if (snapshots.OfType<FloatingVrexGridPanel>().Any(panel => !panel.Closed)) return true;
        return AddPanel(0, () => new FloatingVrexGridPanel(VrexClient.Current)) >= 0;
    }
    private void OpenVrexChat()
    {
        var existing=snapshots.OfType<FloatingVrexChatPanel>().FirstOrDefault(panel => !panel.Closed);
        if (existing == null)
        {
            int slot=AddPanel(7, CreateVrexChatPanel);
            if (slot >= 0) existing=(FloatingVrexChatPanel)snapshots[slot]!;
        }
        if (existing != null) { textTarget=existing; SetKeyboardTarget(); }
    }
    private void ShowKeyboardForInput()
    {
        if (!snapshots.OfType<FloatingKeyboard>().Any(keyboard => !keyboard.Closed)) OpenKeyboard();
        else SetKeyboardTarget();
    }
    private void OpenKeyboard()
    {
        for (int i = 0; i < MaxSnapshots; i++) if (snapshots[i] is FloatingKeyboard { Closed: false })
        {
            if (Lock())
            {
                try { long at = SnapshotsAt+i*SnapshotBytes; memory.Write(at, unchecked(memory.ReadUInt32(at)+1)); }
                finally { mutex.ReleaseMutex(); }
            }
            SetKeyboardTarget(); return;
        }
        if (AddPanel(2, () => new FloatingKeyboard(predictor, SendKeyboardEdit)) >= 0) SetKeyboardTarget();
    }
    private void FocusBrowser(FloatingBrowser browser) { textTarget = browser; SetKeyboardTarget(); }
    private void SetKeyboardTarget()
    {
        string label = textTarget is FloatingBrowser browser ? Loc.Format("KeyboardBrowserTarget", browser.Address) :
            textTarget is FloatingUbuntuPanel or FloatingVrexChatPanel ? Loc.Get("VrexKeyboardTarget") :
            controlScreen ? Loc.Get("KeyboardWindowsTarget") : Loc.Get("KeyboardChooseTarget");
        foreach (var keyboard in snapshots.OfType<FloatingKeyboard>())
            keyboard.SetTarget(label, textTarget is FloatingVrexChatPanel { IsPasswordActive: true });
    }
    private AudioDirection? ReadAudioDirection(int index)
    {
        long at = AudioAt + index * AudioBytes;
        if (memory.ReadInt32(at) == 0) return null;
        float x = memory.ReadSingle(at + 4), y = memory.ReadSingle(at + 8), z = memory.ReadSingle(at + 12);
        return float.IsFinite(x) && float.IsFinite(y) && float.IsFinite(z) ? new(x, y, z) : null;
    }
    private bool SendKeyboardEdit(KeyboardEdit edit)
    {
        if (textTarget is FloatingBrowser { Closed: false } browser) { if (!browser.CanType) return false; browser.Edit(edit); return true; }
        if (textTarget is FloatingUbuntuPanel {Closed:false} ubuntu) {ubuntu.Edit(edit);return true;}
        if (textTarget is FloatingVrexChatPanel {Closed:false} chat) {chat.Edit(edit);return true;}
        if (!Lock()) return false;
        try
        {
            if (!enabled || !controlScreen || (memory.ReadUInt32(KeyboardAt+12)&1) == 0) return false;
            uint write = memory.ReadUInt32(KeyboardAt), read = memory.ReadUInt32(KeyboardAt+4);
            int count = edit.Backspaces + edit.Text.Length + (edit.Key != 0 ? 1 : 0);
            if (unchecked(write-read)+count > 256) return false;
            void Add(int type, int code)
            { long at = KeyboardEventsAt+(write++ % 256)*8; memory.Write(at, type | ((int)edit.Modifiers << 8)); memory.Write(at+4, code); }
            for (int i = 0; i < edit.Backspaces; i++) Add(1, 8);
            foreach (char c in edit.Text) Add(0, c);
            if (edit.Key != 0) Add(1, edit.Key);
            memory.Write(KeyboardAt, write); return true;
        }
        finally { mutex.ReleaseMutex(); }
    }

    private readonly Dictionary<string, double> pageOffsets = new();
    private string pageKey = "";
    private VrexCompanionSettings? companionSettings;
    private double? restoreOffset;
    private void Page(string title, Action? parent, string? navigationKey = null, string? trail = null)
    {
        if (pageKey.Length > 0) pageOffsets[pageKey] = restoreOffset ?? scroll.VerticalOffset;
        pageKey = navigationKey ?? title;
        restoreOffset = pageOffsets.GetValueOrDefault(pageKey);
        pageGeneration++;
        held = null; contentDragCandidate = contentDragging = false; refresh.Clear();
        // Keep toolbar targets; detached page targets must never receive late releases.
        foreach (var key in targets.Keys.Where(k => IsUnder(k, body)).ToArray()) targets.Remove(key);
        companionSettings?.Dispose();
        companionSettings = null;
        body.Children.Clear();
        heading.Text = title; back = parent;
        backButton.IsEnabled = parent != null;
        breadcrumb = trail ?? (parent == null ? title : Loc.Get("ControllerMenuTitle") + " › " + title);
        interactionHint.Text = breadcrumb;
    }

    private void Home()
    {
        Page(Loc.Get("ControllerMenuTitle"), null);
        currentPage = Home;
        var cards = new UniformGrid { Columns = 2 };
        foreach (var section in sections)
        {
            var card = ActionButton(section.Header?.ToString() ?? "", () => Section(section));
            card.Tag = Appearance.Icon(section.Name); card.MinHeight = 82;
            cards.Children.Add(card);
        }
        body.Children.Add(cards);
        body.Children.Add(ActionButton(Loc.Get("GameHelpSettings"), ShowGameHelpSettings));
    }

    private void ShowCapture()
    {
        if (pageKey == "capture") return;
        var parent = currentPage ?? Home;
        OpenCapturePage(parent, breadcrumb);
    }

    private void OpenCapturePage(Action parent, string parentTrail)
    {
        Page("Capture", parent, "capture", parentTrail + " › Capture");
        currentPage = () => OpenCapturePage(parent, parentTrail);
        body.Children.Add(ActionButton(Loc.Get("SnapshotTake"), TakeGameSnapshot));
        vrScreenshotButton = ActionButton("VR screenshot", () => vrCapture.Start(imagesFolder, false));
        vrVideoButton = ActionButton("Record VR video", () =>
        {
            if (vrCapture.CanStop) vrCapture.Stop();
            else vrCapture.Start(imagesFolder, true);
        });
        body.Children.Add(vrScreenshotButton);
        body.Children.Add(vrVideoButton);
        body.Children.Add(ActionButton(Loc.Get("SnapshotOpenFolder"), OpenCaptures));
        body.Children.Add(captureStatus);
        UpdateCaptureButtons();
    }

    private void UpdateCaptureButtons()
    {
        captureLauncher.Content = vrCapture.CanStop ? "Capture · Recording" : "Capture";
        if (vrScreenshotButton != null) vrScreenshotButton.IsEnabled = !vrCapture.Busy;
        if (vrVideoButton != null)
        {
            vrVideoButton.Content = vrCapture.CanStop ? "Stop VR recording" : "Record VR video";
            vrVideoButton.IsEnabled = !vrCapture.Busy || vrCapture.CanStop;
        }
        captureStatus.Text = vrCapture.Status;
        nextMenuRender = 0;
    }

    private void TakeGameSnapshot()
    {
        if (!Lock()) return;
        try
        {
            bool free = Enumerable.Range(0, MaxSnapshots).Any(i =>
                memory.ReadInt32(SnapshotsAt + i * SnapshotBytes + 8) == 0 &&
                memory.ReadInt32(SnapshotsAt + i * SnapshotBytes + 12) == 0);
            if (!free) { interactionHint.Text = Loc.Get("SnapshotLimit"); return; }
            memory.Write(48, unchecked(memory.ReadUInt32(48) + 1));
        }
        finally { mutex.ReleaseMutex(); }
    }

    private void OpenCaptures()
    {
        try
        {
            Directory.CreateDirectory(imagesFolder);
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(imagesFolder) { UseShellExecute = true });
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        { interactionHint.Text = Loc.Format("SnapshotFileError", ex.Message); }
    }

    internal Task StopCaptureAsync() => vrCapture.StopAsync();

    private void Section(Expander section)
    {
        Page(section.Header?.ToString() ?? "", Home, "section:" + section.Name);
        currentPage = () => Section(section);
        if (section.Name == "RoomSection" && section.Content is DependencyObject room)
        {
            var cards = new UniformGrid { Columns = 2 };
            foreach (var category in Categories(room)) if (Project(category) is { } card) cards.Children.Add(card);
            body.Children.Add(cards); return;
        }
        if (section.Content is FrameworkElement content && Project(content) is { } copy) body.Children.Add(copy);
    }

    private static IEnumerable<Expander> Categories(DependencyObject owner)
    {
        foreach (var child in LogicalTreeHelper.GetChildren(owner).OfType<DependencyObject>())
            if (child is Expander category) yield return category;
            else foreach (var nested in Categories(child)) yield return nested;
    }

    private void Category(Expander category, Action parent, string parentTrail)
    {
        Page(category.Header?.ToString() ?? "", parent, "category:" + category.Name, parentTrail + " › " + category.Header);
        currentPage = () => Category(category, parent, parentTrail);
        if (category.Content is FrameworkElement content && Project(content) is { } copy) body.Children.Add(copy);
        if (category.Name == "RoomVrexCategory")
        {
            companionSettings = new VrexCompanionSettings(
                (button, action) => targets[button] = (_, down) => { if (!down) action(); },
                button => targets.Remove(button), () => nextMenuRender = 0);
            body.Children.Add(companionSettings);
            companionSettings.Connect();
        }
    }

    private static void Bind(DependencyObject target, DependencyProperty property, object source, string path,
        BindingMode mode = BindingMode.OneWay) => BindingOperations.SetBinding(target, property,
            new Binding(path) { Source = source, Mode = mode, UpdateSourceTrigger = UpdateSourceTrigger.PropertyChanged });

    private FrameworkElement? Project(FrameworkElement source)
    {
        FrameworkElement? copy;
        switch (source)
        {
            case ContentControl { Name: "VrexSetupHost" or "VrexModelSettingsHost" or "VrexCompanionSettingsHost" }: return null;
            case Button { Name: "VrexModelSettingsButton" }: return null;
            case Expander category:
                Action parent = currentPage ?? Home;
                string trail = breadcrumb;
                var card = ActionButton(category.Header?.ToString() ?? "", () => Category(category, parent, trail));
                card.Tag = Appearance.Icon(category.Name); card.MinHeight = 88;
                copy = card; break;
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
            case UniformGrid uniform:
                var uniformCopy = new UniformGrid { Columns = uniform.Columns, Rows = uniform.Rows, FirstColumn = uniform.FirstColumn };
                foreach (UIElement child in uniform.Children)
                    if (child is FrameworkElement element && Project(element) is { } item) uniformCopy.Children.Add(item);
                copy = uniformCopy; break;
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
        // Reconstruct the owning page, including nested Room categories.
        Action parent = currentPage ?? Home;
        Page(Loc.Get("ControllerMenuSelect"), parent, "choices:" + source.Name, breadcrumb + " › " + Loc.Get("ControllerMenuSelect"));
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
        Action parent = currentPage ?? Home;
        Page(Loc.Get("WorldHexTip"), parent, "hex:" + source.Name, breadcrumb + " › " + Loc.Get("WorldHexTip"));
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
            parent();
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
        var button = new Button { Content = label, Tag = Appearance.ActionIcon(label), FontSize = 21, MinHeight = 48, Margin = new(0, 4, 8, 4),
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

    private bool InScrollViewport(Point point)
    {
        var top = scroll.TranslatePoint(new Point(), surface);
        return point.X >= top.X && point.X <= top.X + scroll.ActualWidth && point.Y >= top.Y && point.Y <= top.Y + scroll.ActualHeight;
    }
    private void Pointer(int kind, float x, float y, float value)
    {
        if (kind is 4 or PanelFeedback.Leave) beamHover.Set(null, () => nextMenuRender = 0);
        if (kind == PanelFeedback.Leave) return;
        if (kind == 4)
        { PanelFeedback.Cancel(held, () => nextMenuRender = 0); held = null; contentDragCandidate = contentDragging = false; scrolling.Jump(scroll.VerticalOffset); return; }
        if (!float.IsFinite(x) || !float.IsFinite(y) || !float.IsFinite(value)) return;
        var point = new Point(x * Width, y * Height);
        if (kind == 0) beamHover.Set(Hit(point), () => nextMenuRender = 0);
        if (!scrolling.Active && !contentDragging) scrolling.Jump(scroll.VerticalOffset);
        if (kind == 3) { scrolling.Add(-Math.Clamp(value, -1000, 1000), scroll.ScrollableHeight); return; }
        if (kind == 1)
        {
            held = Hit(point);
            PanelFeedback.Down(held, () => nextMenuRender = 0);
            contentDragCandidate = held is not Slider && InScrollViewport(point) && scroll.ScrollableHeight > 0;
            contentDragging = false; contentDragY = dragLastY = point.Y; contentDragOffset = scroll.VerticalOffset;
            dragSampleTime = Environment.TickCount64; dragSpeed = 0; scrolling.Jump(scroll.VerticalOffset);
            if (held != null) targets[held](surface.TranslatePoint(point, held), true);
        }
        else if (kind == 0)
        {
            if (held is Slider) targets[held](surface.TranslatePoint(point, held), true);
            else if (contentDragCandidate)
            {
                if (Math.Abs(point.Y - contentDragY) > 7) { contentDragging = true; PanelFeedback.Cancel(held, () => nextMenuRender = 0); held = null; }
                if (contentDragging)
                {
                    long now = Environment.TickCount64;
                    double dt = Math.Max(.005, (now - dragSampleTime) / 1000.0);
                    double speed = Math.Clamp((dragLastY - point.Y) / dt, -6500, 6500);
                    dragSpeed = dragSpeed * .45 + speed * .55;
                    dragLastY = point.Y; dragSampleTime = now;
                    scrolling.Drag(contentDragOffset + contentDragY - point.Y, scroll.ScrollableHeight);
                }
            }
        }
        else if (kind == 2)
        {
            if (contentDragging)
            {
                scrolling.Release(Environment.TickCount64 - dragSampleTime < 100 ? dragSpeed : 0);
                contentDragCandidate = contentDragging = false; held = null; return;
            }
            contentDragCandidate = false;
            var target = held; held = null;
            if (target != null && (target is Slider || Hit(point) == target)) PanelFeedback.Up(target, () => nextMenuRender = 0); else PanelFeedback.Cancel(target, () => nextMenuRender = 0);
            if (target != null && target.IsEnabled && targets.TryGetValue(target, out var action) &&
                (target is Slider || Hit(point) == target)) action(surface.TranslatePoint(point, target), false);
        }
    }

    private void Tick()
    {
        if (disposed || !Lock()) return;
        var events = new List<(int Kind, float X, float Y, float Value)>();
        var ready = new List<(int Slot, string Path, uint Serial)>();
        bool visible;
        bool windowsFocus = false;
        try
        {
            memory.Write(8, enabled ? 1 : 0);
            memory.Write(36, enabled && controlScreen ? 1 : 0);
            memory.Write(40, unchecked((ulong)Environment.TickCount64));
            memory.Write(SmoothEdgesAt, smoothEdges?.IsChecked == true ? 1 : 0);
            var vrex=VrexClient.Current;
            vrex.AudioDirection = enabled ? ReadAudioDirection(0) : null;
            for (int i = 0; i < snapshots.Length; i++)
                if (snapshots[i] is FloatingBrowser browser)
                    browser.SetAudioDirection(enabled ? ReadAudioDirection(i + 1) : null);
            int vrexSlot=Array.FindIndex(snapshots,p=>p is FloatingUbuntuPanel {Closed:false});
            int chatSlot=Array.FindIndex(snapshots,p=>p is FloatingVrexChatPanel {Closed:false});
            vrex.GuestAudioDirection = enabled && vrex.VmPhase == VrexVmPhase.Running && vrexSlot >= 0
                ? ReadAudioDirection(vrexSlot + 1) : null;
            int vrexState=vrex.State switch {"responding"=>1,"working"=>2,"paused"=>3,"speaking"=>4,"error"=>5,_=>0};
            bool companionOpen = chatSlot >= 0;
            int vrexFlags = (companionOpen ? 1 : 0) | (vrex.Helpful ? 2 : 0) |
                (vrex.AllowPcScreen ? 4 : 0) | (vrex.AllowPanelScreen ? 8 : 0) |
                (snapshots.OfType<FloatingVrexReply>().Any(panel => panel.Interactive && !panel.Closed) ? 16 : 0);
            int companionSlot = vrex.Helpful && vrexSlot >= 0 ? vrexSlot : chatSlot;
            var vrexPoint=vrex.Helpful && vrexSlot>=0?((FloatingUbuntuPanel)snapshots[vrexSlot]!).BotPoint:new Point(-1,-1);
            if (!companionOpen || Environment.TickCount64 >= vrexPointingUntil) vrexPointing = null;
            if (vrexPointing is { Point: { } location, Text: var target })
            {
                if (target == "pc" && vrex.AllowPcScreen && vrex.PcTools.Contains("pc_mark"))
                {
                    vrexFlags |= 32;
                    companionSlot = -1;
                    vrexPoint = new Point(location.X, location.Y);
                }
                else if (target == "ubuntu" && vrex.AllowUbuntuMarks && vrexSlot >= 0 &&
                    ((FloatingUbuntuPanel)snapshots[vrexSlot]!).ProjectGuestPoint(location) is Point projected)
                {
                    vrexFlags |= 32;
                    companionSlot = vrexSlot;
                    vrexPoint = projected;
                }
            }
            memory.Write(VrexAt,companionOpen?vrexFlags:0);memory.Write(VrexAt+4,vrexState);memory.Write(VrexAt+8,companionSlot);
            memory.Write(VrexAt+16,(float)vrexPoint.X);memory.Write(VrexAt+20,(float)vrexPoint.Y);
            memory.Write(VrexAt+24,vrex.Avatar);memory.Write(VrexAt+28,vrex.ShellRgb);
            if (pendingArrange != 0) { memory.Write(60, unchecked(memory.ReadUInt32(60)+pendingArrange)); pendingArrange = 0; }
            visible = enabled && memory.ReadInt32(12) != 0;
            if (memory.ReadUInt32(KeyboardAt+8) != keyboardFocusSerial)
            { keyboardFocusSerial = memory.ReadUInt32(KeyboardAt+8); windowsFocus = true; }
            if (memory.ReadUInt32(56) != snapshotError)
            { snapshotError = memory.ReadUInt32(56); interactionHint.Text = Loc.Get("SnapshotFailed"); }
            for (int i = 0; i < MaxSnapshots; i++)
                if (memory.ReadInt32(SnapshotsAt + i * SnapshotBytes + 12) == 1 && snapshots[i] == null)
                {
                    var path = new byte[SnapshotPathChars * 2];
                    memory.ReadArray(SnapshotsAt + i * SnapshotBytes + 32, path, 0, path.Length);
                    ready.Add((i, System.Text.Encoding.Unicode.GetString(path).Split('\0')[0], memory.ReadUInt32(SnapshotsAt + i * SnapshotBytes + 20)));
                }
            uint write = memory.ReadUInt32(28), read = memory.ReadUInt32(32);
            // A producer overflow cancels a drag; never turn a dropped release into a stuck slider.
            if (unchecked(write - read) > EventCount)
            { held = null; observedPanel = -1; foreach (var snapshot in snapshots) snapshot?.Pointer(4, -1, -1); read = write; }
            while (read != write)
            {
                int offset = HeaderBytes + (int)(read % EventCount) * 16;
                events.Add((memory.ReadInt32(offset), memory.ReadSingle(offset + 4), memory.ReadSingle(offset + 8), memory.ReadSingle(offset + 12)));
                read++;
            }
            memory.Write(32, read);
        }
        finally { mutex.ReleaseMutex(); }
        if (windowsFocus) { textTarget = null; SetKeyboardTarget(); }
        CheckGameHelpCapture();
        foreach (var item in ready)
        {
            try
            {
                var image = new BitmapImage(); image.BeginInit(); image.CacheOption = BitmapCacheOption.OnLoad;
                using (var stream = File.OpenRead(item.Path)) { image.StreamSource = stream; image.EndInit(); }
                image.Freeze();
                if (gameHelpCaptureProvider != null && item.Serial == gameHelpCaptureSerial)
                {
                    var provider = gameHelpCaptureProvider; gameHelpCaptureProvider = null;
                    var attachment = GameHelpImage.Capture(image, provider);
                    try
                    {
                        var helper = new FloatingBrowser(FocusBrowser, OpenKeyboard, OpenBrowser, provider.Url);
                        snapshots[item.Slot] = helper; pendingPanelKinds[item.Slot] = 1;
                        helper.StartGameHelp(attachment); FocusBrowser(helper);
                    }
                    catch { attachment.Dispose(); throw; }
                }
                else snapshots[item.Slot] = new FloatingSnapshot(image, imagesFolder);
                snapshotFrames[item.Slot] = 0;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or NotSupportedException or System.IO.FileFormatException or InvalidOperationException)
            { interactionHint.Text = Loc.Format("SnapshotFileError", ex.Message); }
            finally
            {
                string full = Path.GetFullPath(item.Path);
                if (full.StartsWith(Path.GetTempPath(), StringComparison.OrdinalIgnoreCase) && Path.GetFileName(full).StartsWith("VRX", StringComparison.OrdinalIgnoreCase))
                    try { File.Delete(full); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
        }
        settingsVisibleForGuide = visible;
        foreach (var e in events.Where(e => e.Kind >= 16))
        {
            int slot = e.Kind / 16 - 1;
            if (slot >= 0 && slot < MaxSnapshots)
                snapshots[slot]?.Pointer(enabled ? e.Kind % 16 : 4, e.X, e.Y, e.Value);
        }
        TrackObservedPanel(events);
        bool guideShortcutDown = GuideShortcutIsDown();
        if (enabled && guideShortcutDown && !guideShortcutWasDown)
            snapshots.OfType<FloatingVrexChatPanel>().FirstOrDefault(panel => !panel.Closed)?.RequestGuide();
        guideShortcutWasDown = guideShortcutDown;
        timer.Interval = TimeSpan.FromMilliseconds(visible || snapshots.Any(p => p is { Closed: false }) ? 1000.0 / 90 : 50);
        RenderSettings(events, visible);
        for (int i = 0; i < MaxSnapshots; i++)
        {
            var snapshot = snapshots[i];
            if (snapshot == null && pendingPanelKinds[i] == -1 && !ready.Any(r => r.Slot == i)) continue;
            byte[]? rendered = snapshot is { Closed: false, Dirty: true } ? snapshot.Render() : null;
            if (!Lock()) continue;
            try
            {
                long at = SnapshotsAt + i * SnapshotBytes;
                if (snapshot == null || snapshot.Closed)
                {
                    if (snapshot is FloatingVrexChatPanel)
                    {
                        foreach (var bubble in snapshots.OfType<FloatingVrexReply>()) bubble.Dismiss();
                        foreach (var grid in snapshots.OfType<FloatingVrexGridPanel>()) grid.Dispose();
                    }
                    memory.Write(at + 8, 0); memory.Write(at + 12, 0);
                    if (ReferenceEquals(snapshot, textTarget)) { textTarget = null; SetKeyboardTarget(); }
                    snapshot?.Dispose(); snapshots[i] = null;
                    pendingPanelKinds[i] = -1;
                }
                else if (rendered != null)
                {
                    if (pendingPanelKinds[i] >= 0)
                    { memory.Write(at, unchecked(memory.ReadUInt32(at)+1)); memory.Write(at+16, pendingPanelKinds[i]); pendingPanelKinds[i] = -1; }
                    PublishPixels(at + SnapshotMetaBytes, rendered);
                    memory.Write(at + 4, ++snapshotFrames[i]); memory.Write(at + 8, 1); memory.Write(at + 12, 2);
                    snapshot.Published();
                }
            }
            finally { mutex.ReleaseMutex(); }
        }
        if (enabled && snapshots.OfType<FloatingVrexChatPanel>().Any(panel => !panel.Closed) &&
            !snapshots.OfType<FloatingVrexReply>().Any(panel => panel.WaitingForChoice))
            VrexClient.Current.PollPcScreen(() => CaptureObservedPanel(visible));
    }

    private static bool GuideShortcutIsDown()
    {
        const int control = 0x11, leftAlt = 0xA4, shift = 0x10, leftWindows = 0x5B, rightWindows = 0x5C;
        return IsKeyDown('G') && IsKeyDown(control) && IsKeyDown(leftAlt) &&
            !IsKeyDown(shift) && !IsKeyDown(leftWindows) && !IsKeyDown(rightWindows);
    }

    private static bool IsKeyDown(int virtualKey) => (GetAsyncKeyState(virtualKey) & 0x8000) != 0;

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int virtualKey);

    private void TrackObservedPanel(IEnumerable<(int Kind, float X, float Y, float Value)> events)
    {
        if (!enabled) { observedPanel = -1; guidePanelTarget = null; return; }
        long now = Environment.TickCount64;
        foreach (var e in events)
        {
            int panel = e.Kind / 16, kind = e.Kind % 16;
            if (kind == 0 && e.X is >= 0 and <= 1 && e.Y is >= 0 and <= 1)
            {
                if (observedPanel != panel || now - panelLastPoint > 500) panelDwellStart = now;
                observedPanel = panel;
                panelLastPoint = now;
                (panelPointX, panelPointY) = (e.X, e.Y);
                var browser = panel > 0 && panel <= snapshots.Length ? snapshots[panel - 1] as FloatingBrowser : null;
                if (now - panelDwellStart >= 900 && (panel == 0 || browser is { CanObserve: true }))
                    guidePanelTarget = (panel, browser, e.X, e.Y, now);
            }
            else if ((kind is 4 or PanelFeedback.Leave) && observedPanel == panel) observedPanel = -1;
        }
    }

    private (VrexScreenCapture.Frame Frame, string Surface)? CaptureObservedPanel(bool settingsVisible)
    {
        long now = Environment.TickCount64;
        if (observedPanel < 0 || now - panelLastPoint > 350 || now - panelDwellStart < 900) return null;
        return CapturePanel(observedPanel, panelPointX, panelPointY, settingsVisible);
    }

    private (VrexScreenCapture.Frame Frame, string Surface)? CaptureGuidePanel()
    {
        if (guidePanelTarget is not { } target || Environment.TickCount64 - target.Time > 8000 ||
            (target.Slot > 0 && !ReferenceEquals(snapshots[target.Slot - 1], target.Browser))) return null;
        return CapturePanel(target.Slot, target.X, target.Y, settingsVisibleForGuide);
    }

    private (VrexScreenCapture.Frame Frame, string Surface)? CapturePanel(int slot, float x, float y, bool settingsVisible)
    {
        byte[] frame;
        string source;
        if (slot == 0 && settingsVisible) { frame = pixels; source = "settings"; }
        else if (slot > 0 && slot <= snapshots.Length && snapshots[slot - 1] is FloatingBrowser { CanObserve: true } browser)
        { frame = browser.Render(); source = "browser"; }
        else return null;
        try
        {
            var image = BitmapSource.Create(PixelWidth, PixelHeight, 192, 192, PixelFormats.Pbgra32, null, frame, PixelWidth * 4);
            return (VrexScreenCapture.Encode(image, x, y), source);
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or ArgumentException)
        {
            System.Diagnostics.Debug.WriteLine($"[Vrex panel capture] {ex.Message}");
            return null;
        }
    }

    private void RenderSettings(IReadOnlyList<(int Kind, float X, float Y, float Value)> events, bool visible)
    {
        if (!visible) { beamHover.Set(null, () => nextMenuRender = 0); PanelFeedback.Cancel(held, () => nextMenuRender = 0); held = null; contentDragCandidate = contentDragging = false; scrolling.Jump(scroll.VerticalOffset); return; }
        surface.Measure(new Size(Width, Height)); surface.Arrange(new Rect(0, 0, Width, Height)); surface.UpdateLayout();
        if (restoreOffset is double savedOffset) { scroll.ScrollToVerticalOffset(savedOffset); surface.UpdateLayout(); scrolling.Jump(scroll.VerticalOffset); restoreOffset = null; }
        int page = pageGeneration;
        foreach (var e in events.Where(e => e.Kind < 16))
        {
            Pointer(e.Kind, e.X, e.Y, e.Value);
            if (page != pageGeneration) break; // remaining events describe the old bitmap
        }
        foreach (var update in refresh.ToArray()) update();
        long scrollNow = Environment.TickCount64;
        double scrollDt = Math.Clamp((scrollNow - lastScrollTick) / 1000.0, .001, .05); lastScrollTick = scrollNow;
        bool scrollChanged = scrolling.Step(scrollDt, scroll.ScrollableHeight, contentDragging);
        if (scrollChanged) scroll.ScrollToVerticalOffset(scrolling.Position);
        if (scrollNow < nextMenuRender && !scrollChanged && !events.Any(e => e.Kind > 0 && e.Kind < 16)) return;
        nextMenuRender = scrollNow + (scrollChanged || contentDragging || held is Slider ? 11 : 50);
        surface.UpdateLayout();
        if (restoreOffset is double pendingOffset) { scroll.ScrollToVerticalOffset(pendingOffset); surface.UpdateLayout(); scrolling.Jump(scroll.VerticalOffset); restoreOffset = null; }
        if (scrolling.Active || contentDragging || held is Slider)
        {
            // Rasterize at panel resolution while moving. Expand into the unchanged
            // IPC texture, then restore supersampled text once movement settles.
            motionBitmap.Clear(); motionBitmap.Render(surface); motionBitmap.CopyPixels(motionPixels, Width * 4, 0);
            var source = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, uint>(motionPixels.AsSpan());
            var destination = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, uint>(pixels.AsSpan());
            for (int y = 0; y < Height; y++)
            {
                int row = y * 2 * PixelWidth;
                for (int x = 0; x < Width; x++)
                { uint colour = source[y * Width + x]; destination[row + x * 2] = colour; destination[row + x * 2 + 1] = colour; }
                destination.Slice(row, PixelWidth).CopyTo(destination.Slice(row + PixelWidth, PixelWidth));
            }
        }
        else { bitmap.Clear(); bitmap.Render(surface); bitmap.CopyPixels(pixels, PixelWidth * 4, 0); }
        // A solid white texel region for the renderer's low-latency beams and pointer.
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        { int p = (y * PixelWidth + x) * 4; pixels[p] = pixels[p + 1] = pixels[p + 2] = pixels[p + 3] = 255; }
        if (!Lock()) return;
        try { PublishPixels(PixelsAt, pixels); memory.Write(16, ++generation); }
        finally { mutex.ReleaseMutex(); }
    }

    private void PublishPixels(long offset, byte[] frame)
    {
        // Accessor.WriteArray marshals each byte; bulk-copy the released frame while
        // retaining the view handle and the existing IPC mutex for its entire write.
        if (offset < 0 || offset > memory.Capacity - frame.Length) throw new ArgumentOutOfRangeException(nameof(offset));
        var handle = memory.SafeMemoryMappedViewHandle;
        bool retained = false;
        try
        {
            handle.DangerousAddRef(ref retained);
            var destination = IntPtr.Add(handle.DangerousGetHandle(), checked((int)(memory.PointerOffset + offset)));
            System.Runtime.InteropServices.Marshal.Copy(frame, 0, destination, frame.Length);
        }
        finally { if (retained) handle.DangerousRelease(); }
    }

    public void Dispose()
    {
        if (disposed) return;
        SaveWorkspace();
        Enable(false); disposed = true; timer.Stop();
        VrexClient.Current.Changed-=VrexChanged;
        companionSettings?.Dispose();
        vrCapture.Dispose();
        VrexClient.Current.AudioDirection = null;
        VrexClient.Current.GuestAudioDirection = null;
        foreach (var panel in snapshots) panel?.Dispose();
        predictor.Save();
        memory.Dispose(); map.Dispose(); mutex.Dispose();
    }

    // Runs on Windows as part of --smoke-test. Exercises the actual off-screen
    // visual tree and IPC, without a headset, OS input injection or a second window.
    internal void SmokeTest(string output)
    {
        Enable(true);
        Home(); Tick();
        if (backButton.IsEnabled || interactionHint.Text != Loc.Get("ControllerMenuTitle")) throw new Exception("Root menu must show its breadcrumb and disable Back");
        if (!Lock()) throw new Exception("Menu test could not show the panel");
        try { memory.Write(12, 1); } finally { mutex.ReleaseMutex(); }
        Tick();
        var close = targets.Keys.OfType<Button>().Single(button => button.ToolTip?.ToString() == Loc.Get("SnapshotClose"));
        foreach (var button in new[] { close, backButton, rollupButton })
        {
            if (button.Content is not Grid { Width: 24, Height: 24 } icon)
                throw new Exception("Floating settings navigation buttons must show full-size icons.");
            var iconCentre = icon.TranslatePoint(new Point(icon.ActualWidth / 2, icon.ActualHeight / 2), button);
            if (Math.Abs(iconCentre.X - button.ActualWidth / 2) > .5 || Math.Abs(iconCentre.Y - button.ActualHeight / 2) > .5)
                throw new Exception("Floating settings navigation icons must be centered.");
        }
        double expandedViewport = scroll.ViewportHeight;
        ClickButton(Loc.Get("ControllerMenuCollapse")); Tick();
        if (toolbar.Visibility != Visibility.Collapsed || scroll.ViewportHeight < expandedViewport + 100)
            throw new Exception("Rolling up quick controls must give their space to the scrolling content");
        ClickButton(Loc.Get("ControllerMenuExpand")); Tick();
        if (toolbar.Visibility != Visibility.Visible) throw new Exception("Quick controls must expand again through the roll-up icon");
        if (memory.ReadUInt32(VrexAt) != 0) throw new Exception("Vrex must be inactive while chat is closed.");
        string chatPreferences = Path.Combine(output, $"vrex-prefs-{Guid.NewGuid():N}.json");
        var settings = new VrexChatPreferences(true, true, true, true, ["pc_click", "pc_read_file", "unknown"]);
        try
        {
            settings.Save(chatPreferences);
            var restored = VrexChatPreferences.Load(chatPreferences);
            if (!restored.Helpful || !restored.SpeakReplies || !restored.AllowPcScreen || !restored.AllowPanelScreen ||
                restored.PcTools?.SequenceEqual(["pc_click", "pc_read_file"]) != true)
                throw new Exception("Reloading Vrex chat preferences must restore each setting and recognized tool permission.");
        }
        finally { File.Delete(chatPreferences); }
        var vrexLauncher = toolbar.Children.OfType<Button>().Single(button => button.ToolTip as string == "Open Vrex chat");
        if (vrexLauncher.Content is not Image { ActualWidth: > 0, Source: BitmapSource { PixelWidth: > 0 } } badge ||
            !IsUnder(badge, vrexLauncher))
            throw new Exception("The Vrex launcher must render the logo in its button.");
        if (toolbar.Children[toolbar.Children.IndexOf(vrexLauncher) + 1] is not Button { Content: "Ubuntu" })
            throw new Exception("The Ubuntu launcher must sit next to the Vrex launcher.");
        ClickButton("Capture");
        if (pageKey != "capture" || vrScreenshotButton == null || vrVideoButton == null ||
            !targets.Keys.OfType<Button>().Any(button => button.Content?.ToString() == Loc.Get("SnapshotTake")))
            throw new Exception("Capture must expose game screenshot, VR screenshot and VR video.");
        ClickButton(Loc.Get("ControllerMenuBack"));
        var stereoButton = toolbar.Children.OfType<Button>().Single(button => button.Content is CheckBox);
        var stereoIndicator = (CheckBox)stereoButton.Content;
        var stereoSource = (CheckBox)BindingOperations.GetBinding(stereoIndicator, ToggleButton.IsCheckedProperty)!.Source;
        bool? initialStereo = stereoSource.IsChecked;
        var stereoEdge = stereoButton.TranslatePoint(new Point(stereoButton.ActualWidth - 8, stereoButton.ActualHeight / 2), surface);
        float stereoX = (float)(stereoEdge.X / Width), stereoY = (float)(stereoEdge.Y / Height);
        Pointer(1, stereoX, stereoY, 0);
        Pointer(2, stereoX, stereoY, 0);
        if (stereoSource.IsChecked == initialStereo || stereoIndicator.IsChecked != stereoSource.IsChecked)
            throw new Exception("The entire 3D button must toggle the setting and update its indicator.");
        stereoSource.IsChecked = initialStereo;
        var helpPreferences = GameHelpPreferences.Current;
        helpPreferences.Permit(helpPreferences.Selected, false);
        uint oldCaptureRequests = memory.ReadUInt32(48);
        BeginGameHelp();
        if (memory.ReadUInt32(48) != oldCaptureRequests || gameHelpCaptureProvider != null || heading.Text != Loc.Get("GameHelpSettings"))
            throw new Exception("Game help must ask for screenshot consent before requesting a capture");
        if (GameHelpPreferences.Providers.Any(p => !targets.Keys.OfType<Button>().Any(b => b.Content?.ToString() == p.Name)))
            throw new Exception("Game help must expose its major-provider choices");
        helpPreferences.Permit(helpPreferences.Selected, true); BeginGameHelp();
        if (gameHelpCaptureProvider?.Id != helpPreferences.Selected.Id || gameHelpCaptureSerial != unchecked(oldCaptureRequests + 1) || memory.ReadUInt32(48) != gameHelpCaptureSerial)
            throw new Exception("Consented game help must correlate a fresh native capture with the selected provider");
        gameHelpCaptureProvider = null;
        if (!Lock()) throw new Exception("Game-help test could not clear its synthetic capture request");
        try { memory.Write(48, oldCaptureRequests); } finally { mutex.ReleaseMutex(); }
        helpPreferences.Permit(helpPreferences.Selected, false); Home(); Tick();
        imagesFolder = Path.Combine(output, "snapshot-ipc-images");
        FloatingSnapshot.SmokeTest(output);
        FloatingKeyboard.SmokeTest(output);
        if (!Lock()) throw new Exception("Menu test could not lock IPC");
        try { memory.Write(12, 1); }
        finally { mutex.ReleaseMutex(); }
        Tick();
        // A ready GPU-capture file becomes an independently rendered snapshot slot.
        string captureFile = Path.Combine(Path.GetTempPath(), "VRX-" + Guid.NewGuid().ToString("N") + ".bmp");
        var captureImage = BitmapSource.Create(2, 2, 96, 96, PixelFormats.Bgra32, null,
            new byte[] { 0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255 }, 8);
        var bmp = new BmpBitmapEncoder(); bmp.Frames.Add(BitmapFrame.Create(captureImage));
        using (var file = File.Create(captureFile)) bmp.Save(file);
        if (!Lock()) throw new Exception("Snapshot test could not lock IPC");
        try
        {
            memory.Write(SnapshotsAt, 1); memory.Write(SnapshotsAt + 12, 1);
            var path = System.Text.Encoding.Unicode.GetBytes(captureFile + '\0'); memory.WriteArray(SnapshotsAt + 32, path, 0, path.Length);
        }
        finally { mutex.ReleaseMutex(); }
        Tick();
        if (snapshots[0] == null || memory.ReadInt32(SnapshotsAt + 8) != 1 || memory.ReadInt32(SnapshotsAt + 12) != 2 || memory.ReadInt32(SnapshotsAt + 4) == 0 || File.Exists(captureFile))
            throw new Exception("Snapshot capture handoff did not publish a bitmap or clean its temporary file");
        // Route the bottom-edge Save and Delete buttons through snapshot event IDs.
        void SnapshotClick(float x)
        {
            if (!Lock()) throw new Exception("Snapshot test could not lock IPC");
            try
            {
                uint write = memory.ReadUInt32(28);
                foreach (int kind in new[] { 17, 18 })
                {
                    int at = HeaderBytes + (int)(write++ % EventCount) * 16;
                    memory.Write(at, kind); memory.Write(at + 4, x); memory.Write(at + 8, .90f); memory.Write(at + 12, 0f);
                }
                memory.Write(28, write);
            }
            finally { mutex.ReleaseMutex(); }
            Tick();
        }
        SnapshotClick(.1f);
        string? savedSnapshot = (snapshots[0] as FloatingSnapshot)?.SavedPath;
        if (savedSnapshot == null || !File.Exists(savedSnapshot)) throw new Exception("Snapshot Save did not route through IPC");
        SnapshotClick(.24f);
        if (snapshots[0] != null || memory.ReadInt32(SnapshotsAt + 8) != 0 || File.Exists(savedSnapshot))
            throw new Exception("Snapshot Delete did not close the slot and delete its image through IPC");
        if (memory.ReadInt32(16) == 0) throw new Exception("Visible controller menu did not publish a bitmap");
        var bar = titleBar.TranslatePoint(new Point(0, 0), surface);
        if (bar.X != TitleLeft || bar.Y != TitleTop || titleBar.ActualHeight != TitleHeight || titleBar.ActualWidth != TitleRight - TitleLeft)
            throw new Exception("The visible title bar must match the renderer's drag region");
        if (pixels.Take(4).Any(b => b != 255)) throw new Exception("Controller beams must use solid white texels");
        ClickButton(Loc.Get("ControllerMenuControlScreen")); Tick();
        uint beforeArrange = memory.ReadUInt32(60);
        ClickButton(Loc.Get("ArrangeWindows")); Tick();
        if (memory.ReadUInt32(60) != unchecked(beforeArrange+1)) throw new Exception("Arrange windows did not send its request through IPC");
        if (memory.ReadInt32(36) != 1) throw new Exception("Control screen button did not enable mouse mode through IPC");
        if (!Lock()) throw new Exception("Keyboard test could not lock IPC");
        try { memory.Write(KeyboardAt+12, 1); }
        finally { mutex.ReleaseMutex(); }
        if (!SendKeyboardEdit(new KeyboardEdit("hi", Key: 13, Backspaces: 1)) || memory.ReadUInt32(KeyboardAt) != 4 ||
            memory.ReadInt32(KeyboardEventsAt) != 1 || memory.ReadInt32(KeyboardEventsAt+4) != 8 ||
            memory.ReadInt32(KeyboardEventsAt+12) != 'h' || memory.ReadInt32(KeyboardEventsAt+28) != 13)
            throw new Exception("Floating keyboard did not queue Unicode text and editing keys for Windows");
        if (!SendKeyboardEdit(new KeyboardEdit("", 67, Modifiers: KeyboardModifiers.Control)) || memory.ReadUInt32(KeyboardAt) != 5 ||
            memory.ReadInt32(KeyboardEventsAt+32) != 257 || memory.ReadInt32(KeyboardEventsAt+36) != 67)
            throw new Exception("Floating keyboard did not preserve Windows shortcut modifiers through IPC");
        if (!Lock()) throw new Exception("Keyboard test could not lock IPC");
        try { memory.Write(KeyboardAt+12, 0); }
        finally { mutex.ReleaseMutex(); }
        if (SendKeyboardEdit(new KeyboardEdit("blocked")) || memory.ReadUInt32(KeyboardAt) != 5)
            throw new Exception("Keyboard must not send text while Windows target is inactive");
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
        if (!backButton.IsEnabled || !interactionHint.Text.Contains(" › ")) throw new Exception("A section must enable Back and show its breadcrumb");
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
        var panelFrameTimes = new List<double>();
        for (int sample = 0; sample < 30; sample++)
        {
            scrolling.Add(sample % 2 == 0 ? 80 : -80, scroll.ScrollableHeight);
            var clock = System.Diagnostics.Stopwatch.StartNew(); nextMenuRender = 0; Tick();
            panelFrameTimes.Add(clock.Elapsed.TotalMilliseconds);
        }
        File.WriteAllText(Path.Combine(output, "floating-panel-performance.txt"),
            $"Settings scroll layout/render/IPC: average {panelFrameTimes.Average():F1} ms; worst {panelFrameTimes.Max():F1} ms. Desktop smoke measurement, without headset contention.\n");
        Pointer(1, x, y, 0); Pointer(4, -1, -1, 0);
        if (held != null) throw new Exception("Focus loss must cancel a controller drag");

        // Room life controls use the same bound controls, including all preset choices.
        Section(sections.Single(s => s.Name == "RoomSection")); Tick();
        nextMenuRender = 0; Tick();
        var roomShot = new PngBitmapEncoder(); roomShot.Frames.Add(BitmapFrame.Create(bitmap));
        using (var file = File.Create(Path.Combine(output, "controller-room-categories.png"))) roomShot.Save(file);
        var roomWindow = Window.GetWindow(sections.Single(s => s.Name == "RoomSection"));
        var roomSlider = (Slider)roomWindow.FindName("RoomSlider");
        double oldRoom = roomSlider.Value; roomSlider.Value = 60;
        if (Categories((DependencyObject)sections.Single(s => s.Name == "RoomSection").Content).Count() != 6)
            throw new Exception("Room settings must expose six organized category pages");
        ClickButton(Loc.Get("RoomCategoryCreatures")); Tick();
        var lifeBox = targets.Keys.OfType<CheckBox>().Single(b =>
            BindingOperations.GetBinding(b, ToggleButton.IsCheckedProperty)?.Source is CheckBox source && source.Name == "CreaturesCheck");
        var lifeSource = (CheckBox)BindingOperations.GetBinding(lifeBox, ToggleButton.IsCheckedProperty)!.Source;
        bool? lifeBefore = lifeSource.IsChecked;
        targets[lifeBox](new Point(), false);
        if (lifeSource.IsChecked == lifeBefore) throw new Exception("VR creature toggle must update the desktop");
        lifeSource.IsChecked = lifeBefore;
        ClickButton(Loc.Get("ControllerMenuBack")); ClickButton(Loc.Get("RoomCategoryMusic")); Tick();
        var visualBoxes = targets.Keys.OfType<CheckBox>().Where(b =>
            BindingOperations.GetBinding(b, ToggleButton.IsCheckedProperty)?.Source is CheckBox source && source.Name.StartsWith("Visual", StringComparison.Ordinal)).ToArray();
        if (visualBoxes.Length != 12) throw new Exception("VR must expose all twelve visualizer layer checkboxes");
        var visualSources = visualBoxes.Select(b => (CheckBox)BindingOperations.GetBinding(b, ToggleButton.IsCheckedProperty)!.Source).ToArray();
        var visualBefore = visualSources.Select(b => b.IsChecked).ToArray();
        for (int i = 0; i < 2; i++) { visualSources[i].IsChecked = false; targets[visualBoxes[i]](new Point(), false); }
        if (visualSources.Take(2).Any(b => b.IsChecked != true)) throw new Exception("VR visualizer layers must be independently selectable");
        for (int i = 0; i < visualSources.Length; i++) visualSources[i].IsChecked = visualBefore[i];
        scroll.ScrollToVerticalOffset(800); Tick();
        double roomOffset = scroll.VerticalOffset;
        var environmentSource = (Selector)roomWindow.FindName("EnvironmentList");
        object currentEnvironment = environmentSource.SelectedItem;
        Choices(environmentSource); Tick();
        var sameChoice = targets.Keys.OfType<Button>().Single(b => b.Content?.ToString() == ItemLabel(environmentSource, currentEnvironment));
        targets[sameChoice](new Point(), false); Tick();
        if (roomOffset < 100 || Math.Abs(scroll.VerticalOffset - roomOffset) > 2) throw new Exception("Returning from a selector must preserve the Room page's scroll position");
        if (!breadcrumb.Contains(Loc.Get("RoomCategoryMusic"))) throw new Exception("Returning from a selector must retain its Room category breadcrumb");
        Section(sections.Single(s => s.Name == "RoomSection")); Tick();
        foreach (var category in Categories((DependencyObject)sections.Single(s => s.Name == "RoomSection").Content))
        { Category(category, () => Section(sections.Single(s => s.Name == "RoomSection")), Loc.Get("SectionRoom")); Tick(); }
        roomSlider.Value = oldRoom;

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
            var button = targets.Keys.OfType<Button>().Single(b => b.Content?.ToString() == label || b.ToolTip?.ToString() == label);
            var p = button.TranslatePoint(new Point(button.ActualWidth / 2, button.ActualHeight / 2), surface);
            Pointer(1, (float)(p.X / Width), (float)(p.Y / Height), 0);
            Pointer(2, (float)(p.X / Width), (float)(p.Y / Height), 0);
        }

        // Themes and background transparency apply only to floating chrome.
        Home(); Tick();
        byte opaqueAlpha = 0, clearAlpha = 255;
        for (int theme = 0; theme < Appearance.Themes.Length; theme++)
        {
            Appearance.Configure(theme, 100, 0); nextMenuRender = 0; Tick();
            opaqueAlpha = pixels[(60 * PixelWidth + 1000) * 4 + 3];
            var themed = new PngBitmapEncoder(); themed.Frames.Add(BitmapFrame.Create(bitmap));
            using (var themedFile = File.Create(System.IO.Path.Combine(output, $"controller-theme-{theme}.png"))) themed.Save(themedFile);
            Appearance.Configure(theme, 0, 0); nextMenuRender = 0; Tick();
            clearAlpha = pixels[(60 * PixelWidth + 1000) * 4 + 3];
            if (opaqueAlpha < 220 || clearAlpha > 80) throw new Exception("Floating background opacity must preserve alpha in every theme");
        }
        Appearance.Configure(0, 95, 0);

        var motionTest = new PanelScrollMotion();
        for (int packet = 0; packet < 4; packet++) motionTest.Add(100, 2000);
        if (motionTest.Target != 400) throw new Exception("Scroll packets in one UI frame must accumulate");
        motionTest.Step(.01, 2000);
        if (motionTest.Position <= 0 || motionTest.Position >= 400) throw new Exception("Scrolling must animate between offsets");
        for (int tick = 0; tick < 80; tick++) motionTest.Step(.01, 2000);
        if (Math.Abs(motionTest.Position - 400) > 1) throw new Exception("Smooth scrolling must reach the requested offset");

        // Exercise the nested pages through real hit testing. Isolated source controls
        // let us check validation/events without changing the user's active profile.
        var choices = new ComboBox();
        for (int i = 0; i < 25; i++) choices.Items.Add(new ComboBoxItem { Content = $"Option {i}" });
        choices.Items.Add(new ComboBoxItem { Content = "Hidden option", Visibility = Visibility.Collapsed });
        choices.SelectedIndex = 0;
        Choices(choices); Tick();
        if (targets.Keys.OfType<Button>().Any(b => b.Content?.ToString() == "Hidden option"))
            throw new Exception("Hidden desktop options must stay hidden in the controller menu");
        var dragChoice = targets.Keys.OfType<Button>().Single(b => b.Content?.ToString() == "Option 1");
        var dragPoint = dragChoice.TranslatePoint(new Point(dragChoice.ActualWidth / 2, dragChoice.ActualHeight / 2), surface);
        float dragX = (float)(dragPoint.X / Width), dragY = (float)(dragPoint.Y / Height);
        Pointer(1, dragX, dragY, 0); Pointer(0, dragX, dragY - .12f, 0); Pointer(2, dragX, dragY - .12f, 0); Tick();
        if (choices.SelectedIndex != 0 || scrolling.Target <= 0) throw new Exception("Dragging panel content must scroll without clicking a choice");
        Pointer(4, 0, 0, 0);
        Pointer(3, .5f, .5f, -1000); Tick();
        if (scroll.VerticalOffset <= 0) throw new Exception("Thumbstick input did not scroll the option list");
        scrolling.Jump(0); scroll.ScrollToTop(); Tick();
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

// Accumulate input independently of WPF's deferred layout. Multiple XR events in
// one UI tick must add together rather than overwrite the same old scroll offset.
internal sealed class PanelScrollMotion
{
    public double Position { get; private set; }
    public double Target { get; private set; }
    private double velocity;
    public bool Active => Math.Abs(Target - Position) > .1 || Math.Abs(velocity) > 5;
    public void Jump(double offset) { Position = Target = offset; velocity = 0; }
    public void Add(double delta, double maximum) { Target = Math.Clamp(Target + delta, 0, maximum); velocity = 0; }
    public void Drag(double offset, double maximum) { Target = Math.Clamp(offset, 0, maximum); velocity = 0; }
    public void Release(double speed) { velocity = Math.Clamp(speed, -6500, 6500); }
    public bool Step(double dt, double maximum, bool dragging = false)
    {
        double previous = Position;
        Target = Math.Clamp(Target + velocity * dt, 0, maximum);
        velocity *= Math.Exp(-8 * dt);
        if (Target <= 0 || Target >= maximum) velocity = 0;
        Position += (Target - Position) * (1 - Math.Exp(-dt / (dragging ? .018 : .055)));
        if (!Active) Position = Target;
        return Math.Abs(Position - previous) > .01;
    }
}
