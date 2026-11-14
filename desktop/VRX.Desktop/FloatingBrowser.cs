using CefSharp;
using CefSharp.Handler;
using CefSharp.OffScreen;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using CefRect = CefSharp.Structs.Rect;

namespace VRX.Desktop;

internal static class FloatingBrowserRuntime
{
    private static bool started;
    internal static void Ensure()
    {
        if (started) return;
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VRX", "browser");
        Cef.EnableWaitForBrowsersToClose();
        var settings = new CefSettings { WindowlessRenderingEnabled = true, MultiThreadedMessageLoop = true,
            RootCachePath = root, CachePath = Path.Combine(root, "profile"), LogSeverity = LogSeverity.Error,
            LogFile = Path.Combine(root, "browser.log") };
        Directory.CreateDirectory(root);
        settings.CefCommandLineArgs.Add("disable-background-networking");
        if (!Cef.Initialize(settings)) throw new InvalidOperationException("Chromium could not start");
        started = true;
    }
    internal static void Shutdown()
    {
        if (!started) return;
        Cef.WaitForBrowsersToClose(); Cef.Shutdown(); started = false;
    }
}

internal sealed class FloatingBrowser : IFloatingPanel, IFloatingTextTarget
{
    private const int ViewWidth = 1728, ViewHeight = 944;
    private readonly ChromiumWebBrowser browser;
    private readonly UserControl surface;
    private readonly Image picture = new() { Width = 864, Height = 472, Stretch = Stretch.Fill };
    private readonly Canvas viewport = new() { Width = 864, Height = 472, ClipToBounds = true };
    private readonly StackPanel context = new() { Width = 250, Background = new SolidColorBrush(Color.FromRgb(30, 45, 65)), Visibility = Visibility.Collapsed };
    private readonly TextBlock addressText = new() { FontSize = 19, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly TextBlock status = new() { Foreground = Brushes.LightGray, FontSize = 15, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly Dictionary<Button, Action> actions = new();
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
    private bool leftHeld, rightHeld, addressFocus, addressSelected;
    private string address = "";
    private int addressCaret;
    private CefSharp.MouseEvent lastMouse;
    private int generation = 1, renderedGeneration, publishedGeneration;
    public bool Closed { get; private set; }
    public bool Dirty => Volatile.Read(ref generation) != publishedGeneration;
    internal string Address => address;
    internal bool CanType => addressFocus || browser.IsBrowserInitialized;
    private void Changed() => Interlocked.Increment(ref generation);
    internal FloatingBrowser(Action<FloatingBrowser> focus, Action openKeyboard, Action<string> newWindow, string initialUrl = "https://www.google.com")
    {
        FloatingBrowserRuntime.Ensure(); this.focus = focus; this.openKeyboard = openKeyboard; this.newWindow = newWindow;
        var root = new Grid { Margin = new Thickness(48) };
        foreach (double height in new[] { 56.0, 64.0, 472.0, 24.0, 56.0 }) root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(height) });
        void Add(UIElement item, int row) { Grid.SetRow(item, row); root.Children.Add(item); }
        Add(new Border { Height = 56, CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Color.FromRgb(37, 50, 72)),
            Child = new TextBlock { Text = Loc.Get("BrowserTitle"), Foreground = Brushes.White, FontSize = 25, Margin = new Thickness(12, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center } }, 0);
        var nav = new DockPanel { Margin = new Thickness(0, 8, 0, 8) };
        nav.Children.Add(Button("←", 52, () => { if (browser is { IsBrowserInitialized: true }) browser.Back(); }));
        nav.Children.Add(Button("→", 52, () => { if (browser is { IsBrowserInitialized: true }) browser.Forward(); }));
        nav.Children.Add(Button("↻", 52, () => { if (browser is { IsBrowserInitialized: true }) browser.Reload(); }));
        var go = Button(Loc.Get("BrowserGo"), 68, Navigate); DockPanel.SetDock(go, Dock.Right); nav.Children.Add(go);
        var field = Button("", double.NaN, () => { addressFocus = addressSelected = true; addressCaret = address.Length; focus(this); openKeyboard(); Changed(); });
        field.HorizontalContentAlignment = HorizontalAlignment.Left; field.Content = addressText; nav.Children.Add(field); Add(nav, 1);
        viewport.Children.Add(picture); viewport.Children.Add(context); Add(viewport, 2); Add(status, 3);
        var bottom = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 0) };
        bottom.Children.Add(Button(Loc.Get("KeyboardOpen"), 160, () => { focus(this); openKeyboard(); }));
        bottom.Children.Add(Button(Loc.Get("BrowserNew"), 190, () => newWindow("https://www.google.com")));
        bottom.Children.Add(Button(Loc.Get("BrowserLicenses"), 130, () => browser!.Load("chrome://credits")));
        bottom.Children.Add(Button(Loc.Get("SnapshotClose"), 130, () => Closed = true)); Add(bottom, 4);
        surface = new UserControl { Width = ControllerMenu.Width, Height = ControllerMenu.Height, FontFamily = new FontFamily("Segoe UI"), Content = FloatingPanelChrome.Wrap(root) };
        browser = new ChromiumWebBrowser(initialUrl, automaticallyCreateBrowser: false, useLegacyRenderHandler: false)
        { Size = new System.Drawing.Size(ViewWidth, ViewHeight) };
        browser.RenderHandler = new RenderHandler(browser, this);
        browser.MenuHandler = new MenuHandler(this); browser.LifeSpanHandler = new Popups(this);
        browser.BrowserInitialized += (_, _) => browser.SetZoomLevel(Math.Log(2) / Math.Log(1.2));
        browser.AddressChanged += (_, e) => surface.Dispatcher.BeginInvoke(() =>
        { if (!Closed && !addressFocus) { address = e.Address; addressText.Text = address; Changed(); } });
        browser.LoadingStateChanged += (_, e) => surface.Dispatcher.BeginInvoke(() =>
        { if (!Closed) { status.Text = e.IsLoading ? Loc.Get("BrowserLoading") : address; Changed(); } });
        address = initialUrl; addressText.Text = initialUrl; browser.CreateBrowser(browserSettings: new BrowserSettings { WindowlessFrameRate = 20 });
    }
    private Button Button(string text, double width, Action action)
    {
        var button = new Button { Content = text, Width = width, Height = 48, FontSize = 20, Margin = new Thickness(0, 0, 6, 0), Padding = new Thickness(8, 0, 8, 0) };
        actions[button] = () => { action(); Changed(); }; return button;
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
        if (Closed) return;
        if (addressFocus)
        {
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
        void Key(int code)
        {
            host.SendKeyEvent(new CefSharp.KeyEvent { WindowsKeyCode = code, NativeKeyCode = code, Type = KeyEventType.RawKeyDown });
            if (code is 8 or 9 or 13) host.SendKeyEvent(new CefSharp.KeyEvent { WindowsKeyCode = code, Type = KeyEventType.Char });
            host.SendKeyEvent(new CefSharp.KeyEvent { WindowsKeyCode = code, NativeKeyCode = code, Type = KeyEventType.KeyUp });
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
        if (kind == 4) { held = null; CancelMouse(); return; }
        var point = new System.Windows.Point(x * ControllerMenu.Width, y * ControllerMenu.Height);
        var button = actions.Keys.LastOrDefault(b => IsDrawn(b) && b.IsEnabled && new System.Windows.Rect(b.TranslatePoint(new System.Windows.Point(), surface), b.RenderSize).Contains(point));
        if (kind == 1) { held = button; focus(this); }
        if (kind == 2 && held != null)
        { var down = held; held = null; if (down == button) actions[down](); return; }
        var local = surface.TranslatePoint(point, viewport);
        bool inside = local.X >= 0 && local.Y >= 0 && local.X < 864 && local.Y < 472;
        if (button != null || !inside) { if (kind == 2) CancelMouse(); return; }
        if (!browser.IsBrowserInitialized) return;
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
        var copy = new byte[width * height * 4]; Marshal.Copy(buffer, copy, 0, copy.Length);
        lock (pixelsLock)
        {
            if (isPopup) { popup = copy; popupWidth = width; popupHeight = height; }
            else { pixels = copy; pixelsWidth = width; pixelsHeight = height; }
        }
        Changed();
    }
    public byte[] Render()
    {
        renderedGeneration = Volatile.Read(ref generation);
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
        bitmap.Clear(); bitmap.Render(surface); var output = new byte[ControllerMenu.PixelBytes]; bitmap.CopyPixels(output, ControllerMenu.PixelWidth*4, 0); return output;
    }
    public void Published() => publishedGeneration = renderedGeneration;
    public void Dispose() { if (browser.IsDisposed) return; CancelMouse(); Closed = true; browser.Dispose(); }
    private sealed class RenderHandler(ChromiumWebBrowser browser, FloatingBrowser owner) : DefaultRenderHandler(browser)
    {
        public override void OnPaint(PaintElementType type, CefRect dirtyRect, IntPtr buffer, int width, int height) => owner.Paint(type == PaintElementType.Popup, buffer, width, height);
        public override void OnPopupSize(CefRect rect) { lock (owner.pixelsLock) owner.popupRect = rect; }
        public override void OnPopupShow(bool show) { if (!show) { lock (owner.pixelsLock) owner.popup = null; owner.Changed(); } }
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
        string html = "<html><body style='background:#152236;color:white;font:24px sans-serif'><h1>VRX browser test</h1><input id='field' style='font:24px sans-serif;display:block'><button id='clicker' onclick='window.clicks=(window.clicks||0)+1' style='font:24px sans-serif;margin-top:12px'>Click test</button><p>Independent floating browser</p></body></html>";
        string url = "data:text/html;base64," + Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(html));
        int keyboardOpens = 0; var openedWindows = new List<string>();
        using var one = new FloatingBrowser(_ => { }, () => keyboardOpens++, link => openedWindows.Add(link), url);
        using var two = new FloatingBrowser(_ => { }, () => { }, _ => { }, url);
        var deadline = DateTime.UtcNow.AddSeconds(20);
        while ((!one.browser.IsBrowserInitialized || !two.browser.IsBrowserInitialized || one.pixels == null || two.pixels == null) && DateTime.UtcNow < deadline) await Task.Delay(50);
        if (one.pixels == null || two.pixels == null) throw new Exception("Offscreen browser did not render");
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
        await ClickElement("field");
        one.Edit(new KeyboardEdit("hello"));
        do { response = await one.browser.EvaluateScriptAsync("document.getElementById('field').value"); if (response.Result?.ToString() == "hello") break; await Task.Delay(50); } while (DateTime.UtcNow < deadline);
        if (response.Result?.ToString() != "hello") throw new Exception("Browser keyboard input failed");
        one.Edit(new KeyboardEdit("p", Backspaces: 2));
        do { response = await one.browser.EvaluateScriptAsync("document.getElementById('field').value"); if (response.Result?.ToString() == "help") break; await Task.Delay(25); } while (DateTime.UtcNow < deadline);
        if (response.Result?.ToString() != "help") throw new Exception("Browser predictive-word replacement deleted the wrong text");
        response = await two.browser.EvaluateScriptAsync("document.getElementById('field').value");
        if (response.Result?.ToString() != "") throw new Exception("Separate browsers shared document state");
        one.Render(); var image = new PngBitmapEncoder(); image.Frames.Add(BitmapFrame.Create(one.bitmap));
        using (var file = File.Create(Path.Combine(output, "floating-browser.png"))) image.Save(file);
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
    }
}
