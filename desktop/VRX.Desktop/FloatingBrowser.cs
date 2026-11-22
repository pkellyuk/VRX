using CefSharp;
using CefSharp.Handler;
using CefSharp.OffScreen;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using VRX.VrexBridge;
using CefRect = CefSharp.Structs.Rect;

namespace VRX.Desktop;

internal static class FloatingBrowserRuntime
{
    private static readonly bool forceNativeAudio = Environment.GetCommandLineArgs().Contains("--browser-native-audio");
    internal static bool SpatialAudioEnabled { get; private set; } = true;
    internal static bool NativeAudio => forceNativeAudio;
    internal static BrowserAudioBackend AudioBackend { get; private set; } = BrowserAudioBackend.WASAPI;

    internal static BrowserAudioBackend ParseAudioBackend(string? value) =>
        value == nameof(BrowserAudioBackend.WaveOut) ? BrowserAudioBackend.WaveOut : BrowserAudioBackend.WASAPI;

    internal static void ConfigureAudio(bool spatial, string? backend)
    {
        SpatialAudioEnabled = spatial;
        AudioBackend = ParseAudioBackend(backend);
    }
    private static bool started;
    internal static BrowserPermissionStore Permissions { get; private set; } = null!;
    internal static BrowserBookmarks Bookmarks { get; private set; } = null!;
    internal static void Ensure()
    {
        if (started) return;
        string root = Path.Combine(VrxDataDirectory.Root, "browser");
        if (Environment.GetCommandLineArgs().Contains("--smoke-test"))
            root = Path.Combine(Path.GetTempPath(), "VRX-SmokeBrowser", Environment.ProcessId.ToString(System.Globalization.CultureInfo.InvariantCulture));
        Cef.EnableWaitForBrowsersToClose();
        var settings = new CefSettings { WindowlessRenderingEnabled = true, MultiThreadedMessageLoop = true,
            RootCachePath = root, CachePath = Path.Combine(root, "profile"), LogSeverity = LogSeverity.Error,
            LogFile = Path.Combine(root, "browser.log") };
        Directory.CreateDirectory(root);
        Permissions = new BrowserPermissionStore(Path.Combine(root, "site-permissions.json"));
        Bookmarks = new BrowserBookmarks(Path.Combine(root, "bookmarks.json"));
        settings.EnableAudio();
        settings.CefCommandLineArgs.Add("disable-background-networking");
        // Exercise real Chromium permission callbacks without opening real devices.
        // Deliberately do not enable fake UI or auto-grant media access.
        if (Environment.GetCommandLineArgs().Contains("--smoke-test")) settings.CefCommandLineArgs.Add("use-fake-device-for-media-stream");
        if (!Cef.Initialize(settings)) throw new InvalidOperationException("Chromium could not start");
        Cef.GetGlobalRequestContext().RegisterSchemeHandlerFactory("https", WebcamPage.Host, new WebcamPage());
        started = true;
    }
    internal static void Shutdown()
    {
        if (!started) return;
        Cef.WaitForBrowsersToClose(); Cef.Shutdown(); started = false;
    }
}

internal sealed partial class FloatingBrowser : IFloatingPanel, IFloatingTextTarget
{
    private const int ViewWidth = 1728, ViewHeight = 944;
    private readonly ChromiumWebBrowser browser;
    private readonly UserControl surface;
    private readonly Image picture = new() { Width = 864, Height = 472, Stretch = Stretch.Fill };
    private readonly Canvas viewport = new() { Width = 864, Height = 472, ClipToBounds = true };
    private int viewX, viewY;
    private readonly StackPanel context = new() { Width = 250, Background = new SolidColorBrush(Color.FromRgb(30, 45, 65)), Visibility = Visibility.Collapsed };
    private readonly TextBlock addressText = new() { FontSize = 19, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly TextBlock status = new() { Foreground = Brushes.LightGray, FontSize = 15, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly Dictionary<Button, Action> actions = new();
    private readonly Button zoomReset;
    private double zoom;
    private readonly object pixelsLock = new();
    private byte[]? pixels, popup;
    private int pixelsWidth, pixelsHeight, popupWidth, popupHeight;
    private CefRect popupRect;
    private WriteableBitmap? viewBitmap;
    private readonly RenderTargetBitmap bitmap = new(ControllerMenu.PixelWidth, ControllerMenu.PixelHeight, 192, 192, PixelFormats.Pbgra32);
    private readonly Action<FloatingBrowser> focus;
    private readonly Action openKeyboard;
    private readonly Action<string> newWindow;
    private Button? held;
    private long clickFlashUntil;
    private Point clickFlashPoint;
    private bool clickFlashDirty;
    private bool leftHeld, rightHeld, addressFocus, addressSelected;
    private string address = "";
    private int addressCaret;
    private CefSharp.MouseEvent lastMouse;
    private int generation = 1, renderedGeneration, publishedGeneration;
    private int paintCount;
    private bool loading, chromeDirty = true;
    private readonly string initialAddress;
    private readonly System.Diagnostics.Stopwatch loadClock = new();
    private readonly byte[] output = new byte[ControllerMenu.PixelBytes];
    public bool Closed { get; private set; }
    private int appearanceRevision = -1;
    public bool Dirty => clickFlashDirty || appearanceRevision != Appearance.Revision || loading || Volatile.Read(ref generation) != publishedGeneration;
    internal string Address => address;
    internal string LoadedAddress => browser.Address is { Length: > 0 } loaded ? loaded : initialAddress;
    internal double Zoom => zoom;
    internal string? PageTitle => pageTitle;
    internal bool IsWebcam { get; }
    internal bool CanObserve => !Closed && !IsWebcam && permissionOverlay.Visibility != Visibility.Visible &&
        browser.IsBrowserInitialized;
    private readonly BrowserSpatialAudio spatialAudio;
    private readonly PanelFeedback.Hover beamHover = new();
    internal void SetAudioDirection(AudioDirection? position) => spatialAudio.SetDirection(position);
    internal bool CanType => addressFocus || browser.IsBrowserInitialized;
    private void Changed(bool chrome = true) { if (chrome) chromeDirty = true; Interlocked.Increment(ref generation); }
    internal FloatingBrowser(Action<FloatingBrowser> focus, Action openKeyboard, Action<string> newWindow,
        string initialUrl = "https://www.google.com", bool webcam = false, Action<NAudio.Wave.ISampleProvider>? captureAudio = null,
        double initialZoom = 1)
    {
        IsWebcam = webcam;
        zoom = ClampZoom(initialZoom);
        initialAddress = initialUrl;
        spatialAudio = new BrowserSpatialAudio(FloatingBrowserRuntime.AudioBackend,
            FloatingBrowserRuntime.SpatialAudioEnabled, captureAudio);
        FloatingBrowserRuntime.Ensure(); this.focus = focus; this.openKeyboard = openKeyboard; this.newWindow = newWindow;
        var root = new Grid { Margin = new Thickness(48) };
        foreach (double height in new[] { 56.0, 64.0, 472.0, 24.0, 56.0 }) root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(height) });
        if (webcam) { root.RowDefinitions[1].Height = new GridLength(8); root.RowDefinitions[4].Height = new GridLength(0); }
        void Add(UIElement item, int row) { Grid.SetRow(item, row); root.Children.Add(item); }
        Add(new Border { Height = 56, CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Color.FromRgb(37, 50, 72)),
            Child = FloatingPanelChrome.Title(new TextBlock { Text = webcam ? "Webcam" : Loc.Get("BrowserTitle"), Foreground = Brushes.White, FontSize = 25, Margin = new Thickness(12, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center }, Button(Loc.Get("SnapshotClose"), 80, () => Closed = true)) }, 0);
        var nav = new DockPanel { Margin = new Thickness(0, 8, 0, 8) };
        nav.Children.Add(Button("←", 52, () => { if (browser is { IsBrowserInitialized: true }) browser.Back(); }));
        nav.Children.Add(Button("→", 52, () => { if (browser is { IsBrowserInitialized: true }) browser.Forward(); }));
        nav.Children.Add(Button("↻", 52, () => { if (browser is { IsBrowserInitialized: true }) browser.Reload(); }));
        var go = Button(Loc.Get("BrowserGo"), 68, Navigate); DockPanel.SetDock(go, Dock.Right); nav.Children.Add(go);
        var clearAddress = Button("×", 48, () =>
        {
            address = ""; addressCaret = 0; addressSelected = false; addressFocus = true; addressText.Text = "";
            focus(this); openKeyboard(); Changed();
        });
        clearAddress.ToolTip = Loc.Get("BrowserClearAddress");
        System.Windows.Automation.AutomationProperties.SetName(clearAddress, Loc.Get("BrowserClearAddress"));
        DockPanel.SetDock(clearAddress, Dock.Right); nav.Children.Add(clearAddress);
        bookmarksButton = Button("☆", 48, ToggleBookmarks);
        bookmarksButton.ToolTip = "Bookmarks";
        System.Windows.Automation.AutomationProperties.SetName(bookmarksButton, "Bookmarks");
        DockPanel.SetDock(bookmarksButton, Dock.Right); nav.Children.Add(bookmarksButton);
        AddGameHelpControls(nav);
        var field = Button("", double.NaN, () => { addressFocus = addressSelected = true; addressCaret = address.Length; focus(this); openKeyboard(); Changed(); });
        field.HorizontalContentAlignment = HorizontalAlignment.Left; field.Content = addressText; nav.Children.Add(field); Add(nav, 1);
        viewport.Children.Add(picture); viewport.Children.Add(context); Add(viewport, 2); Add(status, 3);
        var bottom = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 0) };
        bottom.Children.Add(Button(Loc.Get("KeyboardOpen"), 160, () => { focus(this); openKeyboard(); }));
        bottom.Children.Add(Button(Loc.Get("BrowserNew"), 190, () => newWindow("https://www.google.com")));
        bottom.Children.Add(Button(Loc.Get("BrowserLicenses"), 130, () => browser!.Load("chrome://credits")));
        bottom.Children.Add(Button("−",48,()=>SetZoom(zoom-.1)));
        zoomReset=Button($"{zoom*100:0}%",80,()=>SetZoom(1)); bottom.Children.Add(zoomReset);
        bottom.Children.Add(Button("+",48,()=>SetZoom(zoom+.1)));
        bottom.Children.Add(Button(Loc.Get("BrowserSiteAccess"),160,ShowSitePermissions));
        Add(bottom, 4);
        if (webcam) { nav.Visibility = Visibility.Collapsed; bottom.Visibility = Visibility.Collapsed; }
        surface = new UserControl { Width = ControllerMenu.Width, Height = ControllerMenu.Height, FontFamily = new FontFamily("Segoe UI"), Content = FloatingPanelChrome.Wrap(root) };
        InitializePermissions();
        InitializeBookmarks();
        browser = new ChromiumWebBrowser(initialUrl, automaticallyCreateBrowser: false, useLegacyRenderHandler: false)
        { Size = new System.Drawing.Size(ViewWidth, ViewHeight) };
        browser.RenderHandler = new RenderHandler(browser, this);
        // Native output lets Chromium use its normal Windows audio path for comparison.
        if (!webcam && (!FloatingBrowserRuntime.NativeAudio || captureAudio != null))
            browser.AudioHandler = spatialAudio;
        browser.MenuHandler = new MenuHandler(this); browser.LifeSpanHandler = new Popups(this);
        browser.PermissionHandler = webcam ? new WebcamPage.CameraPermissions() : new SitePermissionHandler(this);
        if (webcam) browser.RequestHandler = new WebcamPage.LocalNavigation();
        browser.DialogHandler = new GameHelpFileDialogs(this);
        browser.FrameLoadStart += (_, e) => { if (e.Frame.IsMain) PermissionsNavigating(); };
        browser.BrowserInitialized += (_, _) => { browser.SetZoomLevel(Math.Log(2*Volatile.Read(ref zoom)) / Math.Log(1.2)); browser.GetBrowserHost().SetAudioMuted(false); };
        browser.TitleChanged += (_, e) => surface.Dispatcher.BeginInvoke(() =>
        {
            if (!Closed) pageTitle = e.Title;
        });
        browser.AddressChanged += (_, e) => surface.Dispatcher.BeginInvoke(() =>
        {
            if (Closed) return;
            CloseBookmarks();
            UpdateBookmarkStar();
            if (!addressFocus) { address = e.Address; addressText.Text = address; }
            SetZoom(zoom);
            Changed();
        });
        browser.LoadingStateChanged += (_, e) => surface.Dispatcher.BeginInvoke(() =>
        { if (!Closed) { loading = e.IsLoading; if (loading) loadClock.Restart(); status.Text = loading ? Loc.Get("BrowserLoading") : address; Changed(); } });
        address = initialUrl; addressText.Text = initialUrl; browser.CreateBrowser(browserSettings: new BrowserSettings { WindowlessFrameRate = 60 });
    }
    private Button Button(string text, double width, Action action)
    {
        var button = new Button { Content = text, Width = width, Height = 48, FontSize = 20, Margin = new Thickness(0, 0, 6, 0), Padding = new Thickness(8, 0, 8, 0) };
        actions[button] = () => { action(); Changed(); }; return button;
    }
    private static double ClampZoom(double value) => double.IsFinite(value) ? Math.Round(Math.Clamp(value, .25, 3), 2) : 1;

    private void SetZoom(double value)
    {
        zoom=ClampZoom(value);
        zoomReset.Content=$"{zoom*100:0}%";
        if(browser.IsBrowserInitialized) browser.SetZoomLevel(Math.Log(2*zoom)/Math.Log(1.2));
        Changed();
    }
    private void Navigate()
    {
        string value = address.Trim();
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) || (uri.Scheme != "http" && uri.Scheme != "https"))
            value = value.Contains('.') && !value.Contains(' ') ? "https://" + value : "https://www.google.com/search?q=" + Uri.EscapeDataString(value);
        address = value; addressText.Text = value; addressFocus = addressSelected = false; browser.Load(value); Changed();
    }
    public void Edit(KeyboardEdit edit)
    {
        if (Closed || IsWebcam) return;
        if (permissionOverlay.Visibility == Visibility.Visible || bookmarkOverlay.Visibility == Visibility.Visible) return;
        if (edit.Modifiers.HasFlag(KeyboardModifiers.Control) && edit.Key == 76)
        { addressFocus = addressSelected = true; addressCaret = address.Length; Changed(); return; }
        if (addressFocus)
        {
            if (edit.Modifiers.HasFlag(KeyboardModifiers.Control) && edit.Key == 65) { addressSelected = true; Changed(); return; }
            if (edit.Modifiers.HasFlag(KeyboardModifiers.Control) && edit.Key is 67 or 88)
            { Clipboard.SetText(addressSelected ? address : ""); if (edit.Key == 88 && addressSelected) { address = ""; addressCaret = 0; } Changed(); return; }
            if (edit.Modifiers.HasFlag(KeyboardModifiers.Control) && edit.Key == 86) edit = new KeyboardEdit(Clipboard.ContainsText() ? Clipboard.GetText() : "");
            if (addressSelected && (edit.Text.Length > 0 || edit.Backspaces > 0 || edit.Key == 8)) { address = ""; addressCaret = 0; addressSelected = false; }
            int remove = Math.Min(addressCaret, edit.Backspaces + (edit.Key == 8 ? 1 : 0));
            address = address.Remove(addressCaret-remove, remove); addressCaret -= remove;
            address = address.Insert(addressCaret, edit.Text); addressCaret += edit.Text.Length;
            if (edit.Key == 37) { addressSelected = false; addressCaret = Math.Max(0, addressCaret-1); }
            if (edit.Key == 39) { addressSelected = false; addressCaret = Math.Min(address.Length, addressCaret+1); }
            if (edit.Key == 36) { addressSelected = false; addressCaret = 0; }
            if (edit.Key == 35) { addressSelected = false; addressCaret = address.Length; }
            if (edit.Key == 46 && addressCaret < address.Length) address = address.Remove(addressCaret, 1);
            addressText.Text = address; if (edit.Key == 13) Navigate(); Changed(); return;
        }
        if (!browser.IsBrowserInitialized) return;
        var host = browser.GetBrowserHost(); host.SendFocusEvent(true);
        var flags = (edit.Modifiers.HasFlag(KeyboardModifiers.Control) ? CefEventFlags.ControlDown : 0)
            | (edit.Modifiers.HasFlag(KeyboardModifiers.Alt) ? CefEventFlags.AltDown : 0)
            | (edit.Modifiers.HasFlag(KeyboardModifiers.Shift) ? CefEventFlags.ShiftDown : 0)
            | (edit.Modifiers.HasFlag(KeyboardModifiers.Windows) ? CefEventFlags.CommandDown : 0);
        void Key(int code)
        {
            host.SendKeyEvent(new CefSharp.KeyEvent { WindowsKeyCode = code, NativeKeyCode = code, Modifiers = flags, Type = KeyEventType.RawKeyDown });
            if (code is 8 or 9 or 13) host.SendKeyEvent(new CefSharp.KeyEvent { WindowsKeyCode = code, Modifiers = flags, Type = KeyEventType.Char });
            host.SendKeyEvent(new CefSharp.KeyEvent { WindowsKeyCode = code, NativeKeyCode = code, Modifiers = flags, Type = KeyEventType.KeyUp });
        }
        for (int i = 0; i < edit.Backspaces; i++) Key(8);
        foreach (char c in edit.Text) host.SendKeyEvent(new CefSharp.KeyEvent { WindowsKeyCode = c, Type = KeyEventType.Char });
        if (edit.Key != 0) Key(edit.Key);
    }
    private void CancelMouse()
    {
        if (!browser.IsBrowserInitialized) return;
        var host = browser.GetBrowserHost();
        var released = new CefSharp.MouseEvent(lastMouse.X, lastMouse.Y, CefEventFlags.None);
        if (leftHeld) host.SendMouseClickEvent(released, MouseButtonType.Left, true, 1);
        if (rightHeld) host.SendMouseClickEvent(released, MouseButtonType.Right, true, 1);
        lastMouse = released;
        leftHeld = rightHeld = false;
    }
    private bool IsDrawn(Button button)
    {
        // These controls are deliberately off-screen: IsVisible depends on a
        // presentation source and is false even for buttons in the bitmap.
        DependencyObject? current = button;
        while (current != null)
        {
            if (current is UIElement element && element.Visibility != Visibility.Visible) return false;
            if (ReferenceEquals(current, surface)) return true;
            current = VisualTreeHelper.GetParent(current);
        }
        return false;
    }
    public void Pointer(int kind, float x, float y, float value = 0)
    {
        if (Closed) return;
        if (kind is 4 or PanelFeedback.Leave) beamHover.Set(null, () => Changed());
        if (kind == PanelFeedback.Leave) return;
        if (kind == 4) { PanelFeedback.Cancel(held, () => Changed()); held = null; CancelMouse(); return; }
        var point = new System.Windows.Point(x * ControllerMenu.Width, y * ControllerMenu.Height);
        var button = actions.Keys.LastOrDefault(b => IsDrawn(b) && b.IsEnabled && new System.Windows.Rect(b.TranslatePoint(new System.Windows.Point(), surface), b.RenderSize).Contains(point));
        bool permissionModal = permissionOverlay.Visibility == Visibility.Visible;
        bool bookmarksOpen = bookmarkOverlay.Visibility == Visibility.Visible;
        if (permissionModal && button != null && !IsPermissionButton(button) && button.ToolTip?.ToString() != Loc.Get("SnapshotClose")) button = null;
        if (bookmarksOpen && !permissionModal && button != bookmarksButton && !bookmarkButtons.Contains(button!)) button = null;
        if (kind == 0) beamHover.Set(button, () => Changed());
        if (kind == 1) { held = button; PanelFeedback.Down(held, () => Changed()); focus(this); }
        if (kind == 2 && held != null)
        { var down = held; held = null; if (down == button) { PanelFeedback.Up(down, () => Changed()); actions[down](); } else PanelFeedback.Cancel(down, () => Changed()); return; }
        if (permissionModal)
        {
            if (kind == 3) { permissionScroll.ScrollToVerticalOffset(permissionScroll.VerticalOffset - value); Changed(); }
            return;
        }
        if (bookmarksOpen)
        {
            if (kind == 3) { bookmarkScroll.ScrollToVerticalOffset(bookmarkScroll.VerticalOffset - value); Changed(); }
            else if (kind == 1 && button == null) CloseBookmarks();
            return;
        }
        var local = surface.TranslatePoint(point, viewport);
        bool inside = local.X >= 0 && local.Y >= 0 && local.X < 864 && local.Y < 472;
        if (button != null || !inside) { if (kind == 2) CancelMouse(); return; }
        if (!browser.IsBrowserInitialized) return;
        if (kind == 1) { clickFlashPoint = point; clickFlashUntil = Environment.TickCount64 + 160; clickFlashDirty = true; Changed(false); }
        if (kind == 1 || kind == 5)
        {
            addressFocus = addressSelected = false; focus(this);
            if (context.Visibility == Visibility.Visible) { ClearContext(); return; }
            browser.GetBrowserHost().SendFocusEvent(true);
        }
        if (kind == 1) leftHeld = true;
        if (kind == 2) leftHeld = false;
        if (kind == 5) rightHeld = true;
        if (kind == 6) rightHeld = false;
        var flags = (leftHeld ? CefEventFlags.LeftMouseButton : CefEventFlags.None) | (rightHeld ? CefEventFlags.RightMouseButton : CefEventFlags.None);
        lastMouse = new CefSharp.MouseEvent((int)(local.X * 2), (int)(local.Y * 2), flags);
        var host = browser.GetBrowserHost();
        switch (kind)
        {
            case 0: host.SendMouseMoveEvent(lastMouse, false); break;
            case 1: host.SendMouseMoveEvent(lastMouse, false); host.SendMouseClickEvent(lastMouse, MouseButtonType.Left, false, 1); break;
            case 2: host.SendMouseClickEvent(lastMouse, MouseButtonType.Left, true, 1); break;
            case 3: host.SendMouseMoveEvent(lastMouse, false); host.SendMouseWheelEvent(lastMouse, 0, (int)(value * 2)); break;
            case 5: host.SendMouseMoveEvent(lastMouse, false); host.SendMouseClickEvent(lastMouse, MouseButtonType.Right, false, 1); break;
            case 6: host.SendMouseClickEvent(lastMouse, MouseButtonType.Right, true, 1); break;
        }
    }
    private void ClearContext()
    {
        foreach (var item in context.Children.OfType<Button>()) actions.Remove(item);
        context.Children.Clear(); context.Visibility = Visibility.Collapsed; Changed();
    }
    private void ShowContext(string link, string selection, int x, int y)
    {
        if (Closed) return; ClearContext();
        void Add(string text, Action action) => context.Children.Add(Button(text, 244, () => { action(); ClearContext(); }));
        Add(Loc.Get("BrowserBack"), () => browser.Back()); Add(Loc.Get("BrowserReload"), () => browser.Reload());
        if (!string.IsNullOrEmpty(link))
        { Add(Loc.Get("BrowserOpenLink"), () => newWindow(link)); Add(Loc.Get("BrowserCopyLink"), () => Clipboard.SetText(link)); }
        if (!string.IsNullOrEmpty(selection)) Add(Loc.Get("BrowserCopy"), () => Clipboard.SetText(selection));
        Add(Loc.Get("BrowserPaste"), () => { if (Clipboard.ContainsText()) Edit(new KeyboardEdit(Clipboard.GetText())); });
        Add(Loc.Get("SnapshotClose"), () => { });
        context.Visibility = Visibility.Visible;
        Canvas.SetLeft(context, Math.Clamp(x / 2.0, 0, 614)); Canvas.SetTop(context, Math.Clamp(y / 2.0, 0, Math.Max(0, 472-context.Children.Count*48)));
        Changed();
    }
    private void Paint(bool isPopup, IntPtr buffer, int width, int height)
    {
        if (Closed) return;
        lock (pixelsLock)
        {
            ref byte[]? destination = ref (isPopup ? ref popup : ref pixels);
            if (destination?.Length != width * height * 4) destination = new byte[width * height * 4];
            Marshal.Copy(buffer, destination!, 0, destination!.Length);
            if (isPopup) { popupWidth = width; popupHeight = height; }
            else { pixelsWidth = width; pixelsHeight = height; }
        }
        Changed(false);
        if (!isPopup) Interlocked.Increment(ref paintCount);
    }
    public byte[] Render()
    {
        if (appearanceRevision != Appearance.Revision) { chromeDirty = true; appearanceRevision = Appearance.Revision; }
        renderedGeneration = Volatile.Read(ref generation);
        if (!chromeDirty && permissionOverlay.Visibility == Visibility.Visible) { DrawActivity(); return output; }
        // Video frames can go straight into the shared panel bitmap. WPF only
        // redraws the surrounding controls when their appearance changes.
        if (!chromeDirty && context.Visibility != Visibility.Visible && permissionOverlay.Visibility != Visibility.Visible &&
            bookmarkOverlay.Visibility != Visibility.Visible)
        {
            CopyPage(); DrawActivity(); return output;
        }
        chromeDirty = false;
        lock (pixelsLock) if (pixels != null)
        {
            var combined = (byte[])pixels.Clone();
            if (popup != null)
                for (int y = 0; y < popupHeight; y++)
                {
                    int dy = popupRect.Y + y, sx = Math.Max(0, -popupRect.X), dx = Math.Max(0, popupRect.X);
                    int count = Math.Min(popupWidth-sx, pixelsWidth-dx);
                    if (dy >= 0 && dy < pixelsHeight && count > 0) Buffer.BlockCopy(popup, (y*popupWidth+sx)*4, combined, (dy*pixelsWidth+dx)*4, count*4);
                }
            if (viewBitmap == null || viewBitmap.PixelWidth != pixelsWidth || viewBitmap.PixelHeight != pixelsHeight)
                picture.Source = viewBitmap = new WriteableBitmap(pixelsWidth, pixelsHeight, 192, 192, PixelFormats.Bgra32, null);
            viewBitmap.WritePixels(new Int32Rect(0, 0, pixelsWidth, pixelsHeight), combined, pixelsWidth*4, 0);
        }
        surface.Measure(new System.Windows.Size(ControllerMenu.Width, ControllerMenu.Height)); surface.Arrange(new System.Windows.Rect(0, 0, ControllerMenu.Width, ControllerMenu.Height)); surface.UpdateLayout();
        var origin = viewport.TranslatePoint(new System.Windows.Point(), surface);
        viewX = (int)Math.Round(origin.X * 2);
        viewY = (int)Math.Round(origin.Y * 2);
        bitmap.Clear(); bitmap.Render(surface); bitmap.CopyPixels(output, ControllerMenu.PixelWidth*4, 0); DrawActivity(); return output;
    }
    private void CopyPage()
    {
        lock (pixelsLock)
        {
            if (pixels == null || pixelsWidth != ViewWidth || pixelsHeight != ViewHeight) return;
            for (int y = 0; y < ViewHeight; y++)
                Buffer.BlockCopy(pixels, y*ViewWidth*4, output, ((y+viewY)*ControllerMenu.PixelWidth+viewX)*4, ViewWidth*4);
            if (popup == null) return;
            for (int y = 0; y < popupHeight; y++)
            {
                int dy = popupRect.Y+y, sx = Math.Max(0,-popupRect.X), dx = Math.Max(0,popupRect.X);
                int count = Math.Min(popupWidth-sx,ViewWidth-dx);
                if (dy >= 0 && dy < ViewHeight && count > 0)
                    Buffer.BlockCopy(popup,(y*popupWidth+sx)*4,output,((dy+viewY)*ControllerMenu.PixelWidth+viewX+dx)*4,count*4);
            }
        }
    }
    private void DrawActivity()
    {
        // Reserve the last four logical pixels below navigation for a KITT-like
        // scanner. It never covers web content or changes its pointer coordinates.
        double phase = loadClock.Elapsed.TotalSeconds % 1.6 / .8;
        double center = (phase <= 1 ? phase : 2-phase) * (ViewWidth-180) + 90;
        for (int y = viewY-8; y < viewY; y++) for (int x = 0; x < ViewWidth; x++)
        {
            double glow = loading ? Math.Exp(-Math.Pow((x-center)/65,2)) : 0;
            int p = (y*ControllerMenu.PixelWidth+x+viewX)*4;
            output[p] = (byte)(8+20*glow); output[p+1] = (byte)(8+35*glow);
            output[p+2] = (byte)(loading ? 35+220*glow : 18); output[p+3] = 255;
        }
        if (Environment.TickCount64 < clickFlashUntil)
        {
            int cx=(int)(clickFlashPoint.X*2),cy=(int)(clickFlashPoint.Y*2);
            for(int y=cy-18;y<=cy+18;y++) for(int x=cx-18;x<=cx+18;x++)
            {
                float distance=MathF.Sqrt((x-cx)*(x-cx)+(y-cy)*(y-cy));
                if(distance<12 || distance>17 || x<viewX || x>=viewX+ViewWidth || y<viewY || y>=viewY+ViewHeight) continue;
                int at=(y*ControllerMenu.PixelWidth+x)*4;
                output[at]=220;output[at+1]=245;output[at+2]=105;output[at+3]=255;
            }
        }
        else clickFlashDirty=false;
    }
    public void Published() => publishedGeneration = renderedGeneration;
    public void Dispose() { if (browser.IsDisposed) return; CancelMouse(); Closed = true; StopGameHelp(); CancelPermissions(); browser.Dispose(); spatialAudio.Dispose(); }
    private sealed class RenderHandler(ChromiumWebBrowser browser, FloatingBrowser owner) : DefaultRenderHandler(browser)
    {
        public override void OnPaint(PaintElementType type, CefRect dirtyRect, IntPtr buffer, int width, int height) => owner.Paint(type == PaintElementType.Popup, buffer, width, height);
        public override void OnPopupSize(CefRect rect) { lock (owner.pixelsLock) owner.popupRect = rect; }
        public override void OnPopupShow(bool show) { if (!show) { lock (owner.pixelsLock) owner.popup = null; owner.Changed(false); } }
    }
    private sealed class MenuHandler(FloatingBrowser owner) : ContextMenuHandler
    {
        protected override void OnBeforeContextMenu(IWebBrowser chromiumWebBrowser, IBrowser browser, IFrame frame, IContextMenuParams parameters, IMenuModel model)
        {
            string link = parameters.LinkUrl, selection = parameters.SelectionText; int x = parameters.XCoord, y = parameters.YCoord;
            model.Clear(); owner.surface.Dispatcher.BeginInvoke(() => owner.ShowContext(link, selection, x, y));
        }
    }
    private sealed class Popups(FloatingBrowser owner) : LifeSpanHandler
    {
        protected override bool OnBeforePopup(IWebBrowser chromiumWebBrowser, IBrowser browser, IFrame frame, string targetUrl, string targetFrameName,
            WindowOpenDisposition targetDisposition, bool userGesture, IPopupFeatures popupFeatures, IWindowInfo windowInfo, IBrowserSettings browserSettings,
            ref bool noJavascriptAccess, out IWebBrowser? newBrowser)
        {
            newBrowser = null;
            if (userGesture) owner.surface.Dispatcher.BeginInvoke(() => { if (!owner.Closed) owner.newWindow(targetUrl); });
            return true;
        }
    }
    internal static async Task SmokeTest(string output)
    {
        BrowserPermissionStore.SmokeTest(output);
        string html = "<html><body style='background:#152236;color:white;font:24px sans-serif'><h1>VRX browser test</h1><input id='field' style='font:24px sans-serif;display:block'><button id='clicker' onclick='window.clicks=(window.clicks||0)+1' style='font:24px sans-serif;margin-top:12px'>Click test</button><p>Independent floating browser</p></body></html>";
        string url = "data:text/html;base64," + Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(html));
        int keyboardOpens = 0; var openedWindows = new List<string>();
        var audioReady = new TaskCompletionSource<NAudio.Wave.ISampleProvider>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var one = new FloatingBrowser(_ => { }, () => keyboardOpens++, link => openedWindows.Add(link), url,
            captureAudio: samples => audioReady.TrySetResult(samples));
        using var two = new FloatingBrowser(_ => { }, () => { }, _ => { }, url);
        var deadline = DateTime.UtcNow.AddSeconds(20);
        while ((!one.browser.IsBrowserInitialized || !two.browser.IsBrowserInitialized || one.pixels == null || two.pixels == null) && DateTime.UtcNow < deadline) await Task.Delay(50);
        if (one.pixels == null || two.pixels == null) throw new Exception("Offscreen browser did not render");
        if (!await Cef.UIThreadTaskFactory.StartNew(() => !one.browser.GetBrowserHost().IsAudioMuted && one.browser.GetBrowserHost().WindowlessFrameRate == 60))
            throw new Exception("Floating browser audio/frame-rate configuration failed");
        if (!await Cef.UIThreadTaskFactory.StartNew(() => !two.browser.GetBrowserHost().IsAudioMuted &&
            (two.browser.AudioHandler == null) == FloatingBrowserRuntime.NativeAudio))
            throw new Exception("Floating browser did not select the requested audio output.");
        JavascriptResponse response;
        do { response = await one.browser.EvaluateScriptAsync("!!document.getElementById('field')"); if (response.Result is true) break; await Task.Delay(50); } while (DateTime.UtcNow < deadline);
        one.Render();
        void ClickControl(Button button)
        {
            var p = button.TranslatePoint(new System.Windows.Point(button.ActualWidth/2, button.ActualHeight/2), one.surface);
            one.Pointer(1, (float)(p.X/ControllerMenu.Width), (float)(p.Y/ControllerMenu.Height));
            one.Pointer(2, (float)(p.X/ControllerMenu.Width), (float)(p.Y/ControllerMenu.Height));
        }
        ClickControl(one.actions.Keys.Single(b => b.Content?.ToString() == Loc.Get("KeyboardOpen")));
        ClickControl(one.actions.Keys.Single(b => b.Content?.ToString() == Loc.Get("BrowserNew")));
        if (keyboardOpens != 1 || openedWindows.Count != 1) throw new Exception("Offscreen browser toolbar buttons did not receive clicks");
        string loadedAddress = one.browser.Address;
        ClickControl(one.actions.Keys.Single(b => b.ToolTip?.ToString() == Loc.Get("BrowserClearAddress")));
        if (one.Address != "" || !one.addressFocus || one.addressCaret != 0 || one.browser.Address != loadedAddress)
            throw new Exception("Clear address must empty and focus the panel URL without navigating the page");
        one.Edit(new KeyboardEdit("example.com"));
        if (one.Address != "example.com") throw new Exception("Cleared panel URL must accept keyboard or dictated text");
        one.address = loadedAddress; one.addressText.Text = loadedAddress; one.addressFocus = false; one.Changed();
        var initialWidth=await one.browser.EvaluateScriptAsync("window.innerWidth");
        ClickControl(one.actions.Keys.Single(b=>b.Content?.ToString()=="+"));
        deadline=DateTime.UtcNow.AddSeconds(5);
        do { response=await one.browser.EvaluateScriptAsync("window.innerWidth"); if(Convert.ToDouble(response.Result)<Convert.ToDouble(initialWidth.Result)) break; await Task.Delay(25); } while(DateTime.UtcNow<deadline);
        if(Convert.ToDouble(response.Result)>=Convert.ToDouble(initialWidth.Result) || one.zoomReset.Content?.ToString()!="110%") throw new Exception("Browser zoom button did not enlarge the page");
        one.Render(); ClickControl(one.zoomReset);
        deadline=DateTime.UtcNow.AddSeconds(5);
        do { response=await one.browser.EvaluateScriptAsync("window.innerWidth"); if(Convert.ToDouble(response.Result)==Convert.ToDouble(initialWidth.Result)) break; await Task.Delay(25); } while(DateTime.UtcNow<deadline);
        if(Convert.ToDouble(response.Result)!=Convert.ToDouble(initialWidth.Result)) throw new Exception("Browser zoom reset did not restore the page");
        async Task ClickElement(string id)
        {
            var hit = await one.browser.EvaluateScriptAsync($"JSON.stringify((()=>{{let r=document.getElementById('{id}').getBoundingClientRect();return {{x:(r.x+r.width/2)*devicePixelRatio,y:(r.y+r.height/2)*devicePixelRatio}};}})())");
            using var location = System.Text.Json.JsonDocument.Parse(hit.Result!.ToString()!);
            var p = one.viewport.TranslatePoint(new System.Windows.Point(location.RootElement.GetProperty("x").GetDouble()/2, location.RootElement.GetProperty("y").GetDouble()/2), one.surface);
            one.Pointer(0, (float)(p.X/ControllerMenu.Width), (float)(p.Y/ControllerMenu.Height));
            one.Pointer(1, (float)(p.X/ControllerMenu.Width), (float)(p.Y/ControllerMenu.Height));
            one.Pointer(2, (float)(p.X/ControllerMenu.Width), (float)(p.Y/ControllerMenu.Height));
            await Task.Delay(50);
        }
        await ClickElement("clicker");
        response = await one.browser.EvaluateScriptAsync("window.clicks||0");
        if (Convert.ToInt32(response.Result) != 1) throw new Exception("Browser page button did not receive trigger clicks");
        await one.browser.EvaluateScriptAsync("""
            document.getElementById('clicker').onclick = () => {
                window.testAudio = new AudioContext();
                const wave = testAudio.createOscillator();
                wave.frequency.value = 997;
                wave.connect(testAudio.destination);
                wave.start();
            }
            """);
        one.SetAudioDirection(new(1, 0, 0));
        await ClickElement("clicker");
        var heard = await audioReady.Task.WaitAsync(TimeSpan.FromSeconds(8));
        await CheckAudioAsync(right: true);
        one.SetAudioDirection(new(-1, 0, 0));
        await CheckAudioAsync(right: false);
        await one.browser.EvaluateScriptAsync("void testAudio.close()");
        File.WriteAllText(Path.Combine(output, "browser-audio-pass.txt"),
            "PASS: a real Chromium Web Audio stream reached the floating panel spatial mixer and switched stereo sides as the source moved.");

        async Task CheckAudioAsync(bool right)
        {
            var buffer = new float[4800];
            long end = Environment.TickCount64 + 3500;
            while (Environment.TickCount64 < end)
            {
                await Task.Delay(50);
                int count = heard.Read(buffer, 0, buffer.Length);
                double near = 0, far = 0;
                for (int i = 0; i + 1 < count; i += 2)
                {
                    float a = buffer[i + (right ? 1 : 0)], b = buffer[i + (right ? 0 : 1)];
                    near += a * a;
                    far += b * b;
                }
                if (near > 1 && near > 6 * far) return;
            }
            throw new InvalidOperationException("Floating browser audio did not move to the expected side.");
        }
        await ClickElement("field");
        one.Edit(new KeyboardEdit("hello"));
        do { response = await one.browser.EvaluateScriptAsync("document.getElementById('field').value"); if (response.Result?.ToString() == "hello") break; await Task.Delay(50); } while (DateTime.UtcNow < deadline);
        if (response.Result?.ToString() != "hello") throw new Exception("Browser keyboard input failed");
        one.Edit(new KeyboardEdit("p", Backspaces: 2));
        do { response = await one.browser.EvaluateScriptAsync("document.getElementById('field').value"); if (response.Result?.ToString() == "help") break; await Task.Delay(25); } while (DateTime.UtcNow < deadline);
        if (response.Result?.ToString() != "help") throw new Exception("Browser predictive-word replacement deleted the wrong text");
        one.Edit(new KeyboardEdit("", 65, Modifiers: KeyboardModifiers.Control));
        one.Edit(new KeyboardEdit("shortcut"));
        deadline = DateTime.UtcNow.AddSeconds(5);
        do { response = await one.browser.EvaluateScriptAsync("document.getElementById('field').value"); if (response.Result?.ToString() == "shortcut") break; await Task.Delay(25); } while (DateTime.UtcNow < deadline);
        if (response.Result?.ToString() != "shortcut") throw new Exception("Browser Ctrl+A shortcut failed");
        response = await two.browser.EvaluateScriptAsync("document.getElementById('field').value");
        if (response.Result?.ToString() != "") throw new Exception("Separate browsers shared document state");
        one.loading = true; one.loadClock.Restart();
        byte[] first = (byte[])one.Render().Clone(); await Task.Delay(200); byte[] second = one.Render();
        if (first.AsSpan(328*ControllerMenu.PixelWidth*4,8*ControllerMenu.PixelWidth*4).SequenceEqual(second.AsSpan(328*ControllerMenu.PixelWidth*4,8*ControllerMenu.PixelWidth*4)))
            throw new Exception("Browser loading scanner did not animate");
        var image = new PngBitmapEncoder(); image.Frames.Add(BitmapFrame.Create(BitmapSource.Create(ControllerMenu.PixelWidth,ControllerMenu.PixelHeight,192,192,PixelFormats.Pbgra32,null,second,ControllerMenu.PixelWidth*4)));
        using (var file = File.Create(Path.Combine(output, "floating-browser.png"))) image.Save(file);
        one.loading = false; one.Changed(); one.Render();
        await one.browser.EvaluateScriptAsync("(()=>{let c=document.createElement('canvas');c.id='animation';c.width=200;c.height=100;document.body.append(c);let n=0;window.animateTest=true;function draw(){c.getContext('2d').fillStyle='rgb('+((n++*17)%255)+',30,80)';c.getContext('2d').fillRect(0,0,200,100);if(window.animateTest)requestAnimationFrame(draw);}draw();})()");
        await Task.Delay(100);
        int beforePaint = Volatile.Read(ref one.paintCount);
        var sampleClock = System.Diagnostics.Stopwatch.StartNew();
        while (sampleClock.ElapsedMilliseconds < 1000) { one.Render(); one.Published(); await Task.Delay(16); }
        int paints = Volatile.Read(ref one.paintCount)-beforePaint;
        await one.browser.EvaluateScriptAsync("window.animateTest=false;document.getElementById('animation').remove()");
        if (paints < 5) throw new Exception("Browser animation did not keep producing video frames");
        File.WriteAllText(Path.Combine(output,"browser-performance.txt"),$"Offscreen canvas paints: {paints/sampleClock.Elapsed.TotalSeconds:F1} fps; configured limit 60 fps; host audio unmuted. This is a desktop smoke measurement, not headset playback.\n");
        one.Pointer(5, .5f, .4f); one.Pointer(6, .5f, .4f);
        deadline = DateTime.UtcNow.AddSeconds(5);
        while (one.context.Visibility != Visibility.Visible && DateTime.UtcNow < deadline) await Task.Delay(25);
        if (one.context.Visibility != Visibility.Visible) throw new Exception("Browser right-click did not open its floating context menu");
        one.ClearContext();
        await one.browser.EvaluateScriptAsync("document.body.style.height='3000px';window.scrollTo(0,0)");
        deadline = DateTime.UtcNow.AddSeconds(5);
        do
        {
            one.Pointer(3, .5f, .8f, -240); await Task.Delay(50);
            response = await one.browser.EvaluateScriptAsync("window.scrollY");
            if (Convert.ToDouble(response.Result) > 0) break;
        } while (DateTime.UtcNow < deadline);
        if (Convert.ToDouble(response.Result) <= 0) throw new Exception("Browser thumbstick scrolling failed");
        one.browser.Load("chrome://credits"); deadline = DateTime.UtcNow.AddSeconds(10);
        do
        {
            response = await one.browser.EvaluateScriptAsync("location.href.startsWith('chrome://credits') && document.title === 'Credits' && document.body.innerText.length > 200");
            if (response.Result is true) break; await Task.Delay(50);
        } while (DateTime.UtcNow < deadline);
        if (response.Result is not true)
        {
            var details = await one.browser.EvaluateScriptAsync("JSON.stringify({href:location.href,title:document.title,text:document.body.innerText.slice(0,150)})");
            throw new Exception("Chromium component notices are not accessible: " + details.Result);
        }
        await PermissionsSmokeTest(output);
        await GameHelpSmokeTest(output);
        BrowserBookmarks.SmokeTest(output);
        await WebcamSmokeTest();
    }

    private static async Task WebcamSmokeTest()
    {
        using var camera = new FloatingBrowser(_ => { }, () => { }, _ => { }, WebcamPage.Url, webcam: true);
        async Task<bool> WaitUntil(string script)
        {
            long end = Environment.TickCount64 + 12000;
            while (Environment.TickCount64 < end)
            {
                if (camera.browser.CanExecuteJavascriptInMainFrame &&
                    (await camera.browser.EvaluateScriptAsync(script)).Result is true) return true;
                await Task.Delay(50);
            }
            return false;
        }
        if (!await WaitUntil("!!document.getElementById('start') && window.isSecureContext"))
            throw new Exception("The local webcam page did not open as a secure origin.");
        await camera.browser.EvaluateScriptAsync("document.getElementById('start').click()");
        if (!await WaitUntil("document.getElementById('video').videoWidth > 0 && !!document.getElementById('devices').value"))
        {
            var detail = await camera.browser.EvaluateScriptAsync("document.getElementById('status').textContent");
            throw new Exception("The webcam did not show the synthetic stream and selectable device: " + detail.Result);
        }
        var painted = (byte[])camera.Render().Clone();
        camera.CopyPage();
        int firstPixelBelow = ((camera.viewY + ViewHeight) * ControllerMenu.PixelWidth + camera.viewX) * 4;
        if (!painted.AsSpan(firstPixelBelow).SequenceEqual(camera.output.AsSpan(firstPixelBelow)))
            throw new Exception("The webcam fast path painted outside the camera viewport.");
        await camera.browser.EvaluateScriptAsync("document.getElementById('stop').click()");
        if (!await WaitUntil("document.getElementById('video').srcObject === null && document.getElementById('status').textContent === 'Camera off'"))
            throw new Exception("Stopping the webcam did not release its stream.");
    }
}
