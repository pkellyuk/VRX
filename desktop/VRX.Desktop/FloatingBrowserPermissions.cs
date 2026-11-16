using CefSharp;
using CefSharp.Enums;
using CefSharp.Handler;
using System.Collections.Concurrent;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace VRX.Desktop;

internal sealed partial class FloatingBrowser
{
    private enum PermissionAnswer { Allow, Deny, Cancel }
    private sealed class PendingPermission(string page, string origin, string[] scopes, int epoch,
        Action<PermissionAnswer> finish, Action dispose)
    {
        internal readonly string Page = page, Origin = origin;
        internal readonly string[] Scopes = scopes;
        internal readonly int Epoch = epoch;
        private int done;
        internal bool Active => Volatile.Read(ref done) == 0;
        internal bool Resolve(PermissionAnswer answer, Action? commit = null)
        {
            if (Interlocked.CompareExchange(ref done, 1, 0) != 0) return false;
            try { commit?.Invoke(); }
            finally
            {
                _ = Cef.UIThreadTaskFactory.StartNew(() =>
                {
                    try { finish(answer); }
                    catch (Exception e) when (e is ObjectDisposedException or InvalidOperationException) { }
                    finally { dispose(); }
                });
            }
            return true;
        }
        internal void Abandon()
        {
            if (Interlocked.CompareExchange(ref done, 1, 0) == 0) dispose();
        }
    }
    private readonly Border permissionOverlay = new() { Width = 864, Height = 472, Visibility = Visibility.Collapsed,
        Padding = new Thickness(24), Background = new SolidColorBrush(Color.FromArgb(250, 16, 24, 40)) };
    private readonly TextBlock permissionTitle = new() { FontSize = 24, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 0, 0, 12) };
    private readonly TextBlock permissionText = new() { FontSize = 18, TextWrapping = TextWrapping.Wrap };
    private readonly ScrollViewer permissionScroll = new() { VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    private readonly StackPanel permissionButtons = new() { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 12, 0, 0) };
    private readonly HashSet<Button> permissionControls = [];
    private readonly List<PendingPermission> permissionQueue = [];
    private readonly ConcurrentDictionary<ulong, PendingPermission> permissionPrompts = new();
    private int permissionEpoch;

    private void InitializePermissions()
    {
        var content = new DockPanel();
        DockPanel.SetDock(permissionTitle, Dock.Top); content.Children.Add(permissionTitle);
        DockPanel.SetDock(permissionButtons, Dock.Bottom); content.Children.Add(permissionButtons);
        permissionScroll.Content = permissionText; content.Children.Add(permissionScroll);
        permissionOverlay.Child = content; viewport.Children.Add(permissionOverlay);
        Canvas.SetLeft(permissionOverlay, 0); Canvas.SetTop(permissionOverlay, 0);
    }
    private bool IsPermissionButton(Button button) => permissionControls.Contains(button);
    private void PermissionButtons(params (string Label, Action Action)[] controls)
    {
        foreach (var button in permissionControls) actions.Remove(button);
        permissionControls.Clear(); permissionButtons.Children.Clear();
        foreach (var (label, action) in controls)
        {
            var button = Button(label, double.NaN, action); button.MinWidth = 140;
            permissionControls.Add(button); permissionButtons.Children.Add(button);
        }
    }
    private static string PermissionLabel(string scope) => scope switch
    {
        "microphone" => Loc.Get("BrowserPermissionMicrophone"),
        "camera" => Loc.Get("BrowserPermissionCamera"),
        "desktop-audio" => Loc.Get("BrowserPermissionDesktopAudio"),
        "desktop-video" => Loc.Get("BrowserPermissionDesktopVideo"),
        "permission:Geolocation" => Loc.Get("BrowserPermissionLocation"),
        "permission:Notifications" => Loc.Get("BrowserPermissionNotifications"),
        "permission:Clipboard" => Loc.Get("BrowserPermissionClipboard"),
        _ => Loc.Format("BrowserPermissionOther", Regex.Replace(scope.Replace("permission:", ""), "(?<=[a-z])([A-Z])", " $1"))
    };
    private static string[] MediaScopes(MediaAccessPermissionType permissions)
    {
        const MediaAccessPermissionType known = MediaAccessPermissionType.AudioCapture | MediaAccessPermissionType.VideoCapture |
            MediaAccessPermissionType.DesktopAudioCapture | MediaAccessPermissionType.DesktopVideoCapture;
        if ((permissions & ~known) != 0) return [];
        var scopes = new List<string>();
        if (permissions.HasFlag(MediaAccessPermissionType.AudioCapture)) scopes.Add("microphone");
        if (permissions.HasFlag(MediaAccessPermissionType.VideoCapture)) scopes.Add("camera");
        if (permissions.HasFlag(MediaAccessPermissionType.DesktopAudioCapture)) scopes.Add("desktop-audio");
        if (permissions.HasFlag(MediaAccessPermissionType.DesktopVideoCapture)) scopes.Add("desktop-video");
        return scopes.ToArray();
    }
    private static string[] PromptScopes(PermissionRequestType requested)
    {
        uint seen = 0; var scopes = new List<string>();
        foreach (var permission in Enum.GetValues<PermissionRequestType>())
        {
            uint bit = (uint)permission;
            if (bit == 0 || (bit & (bit - 1)) != 0 || (bit & (uint)requested) == 0) continue;
            seen |= bit;
            scopes.Add(permission == PermissionRequestType.MicStream ? "microphone" :
                permission == PermissionRequestType.CameraStream ? "camera" : "permission:" + permission);
        }
        return seen == (uint)requested ? scopes.Distinct().ToArray() : [];
    }
    private void EnqueuePermission(PendingPermission request)
    {
        if (!request.Active) return;
        if (Closed || request.Epoch != Volatile.Read(ref permissionEpoch) || request.Scopes.Length == 0 ||
            BrowserPermissionStore.Origin(request.Page) == null || BrowserPermissionStore.Origin(request.Origin) == null)
        { request.Resolve(PermissionAnswer.Cancel); return; }
        var saved = request.Scopes.Select(s => FloatingBrowserRuntime.Permissions.Choice(request.Page, request.Origin, s)).ToArray();
        if (saved.All(s => s.HasValue))
        { request.Resolve(saved.All(s => s == true) ? PermissionAnswer.Allow : PermissionAnswer.Deny); return; }
        // No capability is granted merely because another capability is remembered.
        if (permissionQueue.Count >= 8) { request.Resolve(PermissionAnswer.Cancel); return; }
        permissionQueue.Add(request); ShowNextPermission();
    }
    private void ShowNextPermission()
    {
        permissionQueue.RemoveAll(p => !p.Active);
        while (permissionQueue.Count != 0)
        {
            var queued = permissionQueue[0];
            var saved = queued.Scopes.Select(s => FloatingBrowserRuntime.Permissions.Choice(queued.Page, queued.Origin, s)).ToArray();
            if (!saved.All(s => s.HasValue)) break;
            queued.Resolve(saved.All(s => s == true) ? PermissionAnswer.Allow : PermissionAnswer.Deny);
            permissionQueue.RemoveAt(0);
        }
        if (permissionQueue.Count == 0) { permissionOverlay.Visibility = Visibility.Collapsed; Changed(); return; }
        var request = permissionQueue[0];
        CancelMouse(); ClearContext();
        permissionTitle.Text = Loc.Get("BrowserPermissionTitle");
        string origin = request.Page == request.Origin ? Loc.Format("BrowserPermissionOrigin", request.Origin) :
            Loc.Format("BrowserPermissionEmbedded", request.Origin, request.Page);
        permissionText.Text = origin + "\n\n" + string.Join("\n", request.Scopes.Select(s => "• " + PermissionLabel(s))) +
            "\n\n" + Loc.Get("BrowserPermissionRemember");
        PermissionButtons((Loc.Get("BrowserPermissionAllow"), () => AnswerPermission(request, true)),
            (Loc.Get("BrowserPermissionDeny"), () => AnswerPermission(request, false)));
        permissionScroll.ScrollToTop(); permissionOverlay.Visibility = Visibility.Visible; Changed();
    }
    private void AnswerPermission(PendingPermission request, bool allow)
    {
        bool current = !Closed && request.Epoch == Volatile.Read(ref permissionEpoch);
        request.Resolve(current ? allow ? PermissionAnswer.Allow : PermissionAnswer.Deny : PermissionAnswer.Cancel, current ? () =>
        {
            try { FloatingBrowserRuntime.Permissions.Remember(request.Page, request.Origin, request.Scopes, allow); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            { status.Text = Loc.Get("BrowserPermissionSaveFailed"); }
        } : null);
        permissionQueue.Remove(request); ShowNextPermission();
    }
    private void PermissionsNavigating()
    {
        Interlocked.Increment(ref permissionEpoch);
        surface.Dispatcher.BeginInvoke(() =>
        {
            int epoch = Volatile.Read(ref permissionEpoch);
            foreach (var request in permissionQueue.Where(p => p.Epoch != epoch).ToArray())
            { request.Resolve(PermissionAnswer.Cancel); permissionQueue.Remove(request); }
            ShowNextPermission();
        });
    }
    private void CancelPermissions()
    {
        Interlocked.Increment(ref permissionEpoch);
        foreach (var request in permissionQueue) request.Resolve(PermissionAnswer.Cancel);
        permissionQueue.Clear(); permissionOverlay.Visibility = Visibility.Collapsed;
    }
    private void ShowSitePermissions()
    {
        if (permissionQueue.Count != 0) { ShowNextPermission(); return; }
        string? page = BrowserPermissionStore.Origin(browser.Address);
        permissionTitle.Text = Loc.Get("BrowserSiteAccess");
        var saved = page == null ? [] : FloatingBrowserRuntime.Permissions.ForPage(page);
        permissionText.Text = (page ?? Loc.Get("BrowserPermissionNoSite")) + "\n\n" + (saved.Length == 0 ? Loc.Get("BrowserPermissionNoSaved") :
            string.Join("\n", saved.Select(d => Loc.Format("BrowserPermissionSaved", PermissionLabel(d.Permission),
                Loc.Get(d.Allow ? "BrowserPermissionAllow" : "BrowserPermissionDeny"), d.Origin))));
        PermissionButtons((Loc.Get("BrowserPermissionReset"), () => { if (page != null) _ = ResetSitePermissions(page); }),
            (Loc.Get("SnapshotClose"), () => { permissionOverlay.Visibility = Visibility.Collapsed; Changed(); }));
        CancelMouse(); ClearContext(); permissionScroll.ScrollToTop(); permissionOverlay.Visibility = Visibility.Visible; Changed();
    }
    private static IEnumerable<ContentSettingTypes> ContentSettings(string scope)
    {
        if (scope == "microphone") return [ContentSettingTypes.MediaStreamMic];
        if (scope == "camera") return [ContentSettingTypes.MediaStreamCamera];
        if (scope == "desktop-audio") return [ContentSettingTypes.DisplayMediaSystemAudio];
        if (scope == "desktop-video") return [ContentSettingTypes.DisplayCapture];
        string name = scope.Replace("permission:", "");
        if (!Enum.TryParse<PermissionRequestType>(name, out var permission) || !Enum.IsDefined(permission)) return [];
        if (permission == PermissionRequestType.MultipleDownloads) return [ContentSettingTypes.AutomaticDownloads];
        if (permission == PermissionRequestType.Clipboard) return [ContentSettingTypes.ClipboardReadWrite, ContentSettingTypes.ClipboardSanitizedWrite];
        if (permission == PermissionRequestType.VrSession) return [ContentSettingTypes.Vr];
        if (permission == PermissionRequestType.ArSession) return [ContentSettingTypes.Ar];
        if (permission == PermissionRequestType.RegisterProtocolHandler) return [ContentSettingTypes.ProtocolHandlers];
        if (permission == PermissionRequestType.FileSystemAccess) return [ContentSettingTypes.FileSystemReadGuard, ContentSettingTypes.FileSystemWriteGuard];
        return Enum.TryParse<ContentSettingTypes>(name, out var setting) && Enum.IsDefined(setting) ? [setting] : [];
    }
    private async Task ResetSitePermissions(string page)
    {
        try
        {
            var removed = FloatingBrowserRuntime.Permissions.Forget(page);
            await Cef.UIThreadTaskFactory.StartNew(() =>
            {
                if (browser.IsDisposed || !browser.IsBrowserInitialized) return;
                using var context = browser.GetBrowserHost().RequestContext;
                foreach (var decision in removed)
                    foreach (var setting in ContentSettings(decision.Permission))
                        context.SetContentSetting(decision.Origin, decision.Page, setting, ContentSettingValues.Default);
            });
            if (!Closed) { CancelPermissions(); browser.Reload(); Changed(); }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException)
        { status.Text = Loc.Get("BrowserPermissionResetFailed"); Changed(); }
    }
    private sealed class SitePermissionHandler(FloatingBrowser owner) : PermissionHandler
    {
        private static string Page(IBrowser browser)
        { using var frame = browser.MainFrame; return BrowserPermissionStore.Origin(frame?.Url ?? "") ?? ""; }
        protected override bool OnRequestMediaAccessPermission(IWebBrowser control, IBrowser browser, IFrame frame,
            string requestingOrigin, MediaAccessPermissionType requestedPermissions, IMediaAccessCallback callback)
        {
            var request = new PendingPermission(Page(browser), BrowserPermissionStore.Origin(requestingOrigin) ?? "",
                MediaScopes(requestedPermissions), Volatile.Read(ref owner.permissionEpoch), answer =>
                { if (answer == PermissionAnswer.Allow) callback.Continue(requestedPermissions); else callback.Cancel(); }, callback.Dispose);
            owner.surface.Dispatcher.BeginInvoke(() => owner.EnqueuePermission(request)); return true;
        }
        protected override bool OnShowPermissionPrompt(IWebBrowser control, IBrowser browser, ulong promptId,
            string requestingOrigin, PermissionRequestType requestedPermissions, IPermissionPromptCallback callback)
        {
            var request = new PendingPermission(Page(browser), BrowserPermissionStore.Origin(requestingOrigin) ?? "",
                PromptScopes(requestedPermissions), Volatile.Read(ref owner.permissionEpoch), answer =>
                    callback.Continue(answer == PermissionAnswer.Allow ? PermissionRequestResult.Accept :
                        answer == PermissionAnswer.Deny ? PermissionRequestResult.Deny : PermissionRequestResult.Dismiss),
                () => { owner.permissionPrompts.TryRemove(promptId, out _); callback.Dispose(); });
            owner.permissionPrompts[promptId] = request;
            owner.surface.Dispatcher.BeginInvoke(() => owner.EnqueuePermission(request)); return true;
        }
        protected override void OnDismissPermissionPrompt(IWebBrowser control, IBrowser browser, ulong promptId, PermissionRequestResult result)
        {
            if (!owner.permissionPrompts.TryRemove(promptId, out var request)) return;
            bool active = request.Active; request.Abandon();
            if (active) owner.surface.Dispatcher.BeginInvoke(() => { owner.permissionQueue.Remove(request); owner.ShowNextPermission(); });
        }
    }

    private sealed class PermissionTestServer : IDisposable
    {
        private readonly System.Net.Sockets.TcpListener listener = new(System.Net.IPAddress.Loopback, 0);
        private readonly CancellationTokenSource stop = new();
        private readonly Task worker;
        internal string Url { get; }
        internal PermissionTestServer(string? fixture = null)
        {
            listener.Start(); Url = "http://127.0.0.1:" + ((System.Net.IPEndPoint)listener.LocalEndpoint).Port + "/";
            worker = Task.Run(async () =>
            {
                string html = fixture ?? "<html><body><button onclick='ask()'>Request microphone</button><button id='notify' onclick='notify()'>Request notifications</button><script>window.permissionResult='idle';window.notificationResult='idle';async function ask(){window.permissionResult='requesting';try{let stream=await navigator.mediaDevices.getUserMedia({audio:true});stream.getTracks().forEach(t=>t.stop());window.permissionResult='allowed';}catch(e){window.permissionResult=e.name;}}async function notify(){window.notificationResult=await Notification.requestPermission();}</script></body></html>";
                byte[] page = System.Text.Encoding.UTF8.GetBytes(html);
                byte[] header = System.Text.Encoding.ASCII.GetBytes("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + page.Length + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
                try
                {
                    while (!stop.IsCancellationRequested)
                    {
                        using var client = await listener.AcceptTcpClientAsync(stop.Token);
                        using var stream = client.GetStream();
                        byte[] request = new byte[8192]; int used = 0;
                        while (used < request.Length)
                        {
                            int count = await stream.ReadAsync(request.AsMemory(used), stop.Token);
                            if (count == 0) break;
                            used += count;
                            if (System.Text.Encoding.ASCII.GetString(request, 0, used).Contains("\r\n\r\n", StringComparison.Ordinal)) break;
                        }
                        await stream.WriteAsync(header, stop.Token); await stream.WriteAsync(page, stop.Token);
                    }
                }
                catch (Exception e) when (e is OperationCanceledException or System.Net.Sockets.SocketException or IOException or ObjectDisposedException) { }
            });
        }
        public void Dispose() { stop.Cancel(); listener.Stop(); }
    }
    private static async Task PermissionsSmokeTest(string output)
    {
        using var server = new PermissionTestServer();
        using var panel = new FloatingBrowser(_ => { }, () => { }, _ => { }, server.Url);
        async Task WaitFor(Func<Task<bool>> condition, string failure)
        {
            var deadline = DateTime.UtcNow.AddSeconds(12);
            while (DateTime.UtcNow < deadline) { if (await condition()) return; await Task.Delay(30); }
            throw new Exception(failure);
        }
        async Task<bool> ScriptIs(string script, string expected)
        {
            if (!panel.browser.IsBrowserInitialized || !panel.browser.CanExecuteJavascriptInMainFrame) return false;
            try
            {
                var result = await panel.browser.EvaluateScriptAsync(script);
                return result.Success && result.Result?.ToString() == expected;
            }
            catch (Exception) when (panel.browser.IsLoading || !panel.browser.CanExecuteJavascriptInMainFrame) { return false; }
        }
        void Click(string label)
        {
            panel.Render(); var button = panel.permissionControls.Single(b => b.Content?.ToString() == label);
            var point = button.TranslatePoint(new Point(button.ActualWidth / 2, button.ActualHeight / 2), panel.surface);
            panel.Pointer(1, (float)(point.X / ControllerMenu.Width), (float)(point.Y / ControllerMenu.Height));
            panel.Pointer(2, (float)(point.X / ControllerMenu.Width), (float)(point.Y / ControllerMenu.Height));
        }
        await WaitFor(() => ScriptIs("window.permissionResult", "idle"), "Permission test website did not load");
        await panel.browser.EvaluateScriptAsync("ask();true");
        await WaitFor(() => Task.FromResult(panel.permissionQueue.Count == 1), "Chromium media access request did not reach the floating permission prompt");
        panel.Render();
        if (!panel.permissionText.Text.Contains(BrowserPermissionStore.Origin(server.Url)!) || !panel.permissionText.Text.Contains(Loc.Get("BrowserPermissionMicrophone")))
            throw new Exception("The projected permission prompt must identify the real requesting origin and capability");
        var shot = new System.Windows.Media.Imaging.PngBitmapEncoder(); shot.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(panel.bitmap));
        using (var file = File.Create(Path.Combine(output, "browser-permission-prompt.png"))) shot.Save(file);
        Click(Loc.Get("BrowserPermissionAllow"));
        await WaitFor(() => ScriptIs("window.permissionResult", "allowed"), "Allow did not complete Chromium's media request");
        string origin = BrowserPermissionStore.Origin(server.Url)!;
        if (FloatingBrowserRuntime.Permissions.Choice(origin, origin, "microphone") != true) throw new Exception("Allow was not remembered");
        await panel.browser.EvaluateScriptAsync("ask();true");
        await WaitFor(() => ScriptIs("window.permissionResult", "allowed"), "Remembered microphone Allow did not apply");
        if (panel.permissionQueue.Count != 0) throw new Exception("Remembered Allow should not prompt again");
        panel.ShowSitePermissions(); Click(Loc.Get("BrowserPermissionReset"));
        await WaitFor(() => ScriptIs("window.permissionResult", "idle"), "Reset did not reload the permission test site");
        await panel.browser.EvaluateScriptAsync("ask();true");
        await WaitFor(() => Task.FromResult(panel.permissionQueue.Count == 1), "Reset must restore the media permission prompt");
        Click(Loc.Get("BrowserPermissionDeny"));
        await WaitFor(() => ScriptIs("window.permissionResult", "NotAllowedError"), "Deny did not reject Chromium's media request");
        if (FloatingBrowserRuntime.Permissions.Choice(origin, origin, "microphone") != false) throw new Exception("Deny was not remembered");
        await panel.browser.EvaluateScriptAsync("ask();true");
        await WaitFor(() => ScriptIs("window.permissionResult", "NotAllowedError"), "Remembered microphone Deny did not apply");
        if (panel.permissionQueue.Count != 0) throw new Exception("Remembered Deny should not prompt again");
        // A real user gesture exercises the separate, general-permission callback.
        var hit = await panel.browser.EvaluateScriptAsync("JSON.stringify((()=>{let r=document.getElementById('notify').getBoundingClientRect();return {x:(r.x+r.width/2)/innerWidth,y:(r.y+r.height/2)/innerHeight};})())");
        using (var location = System.Text.Json.JsonDocument.Parse(hit.Result!.ToString()!))
        {
            panel.Render();
            var point = panel.viewport.TranslatePoint(new Point(location.RootElement.GetProperty("x").GetDouble() * 864,
                location.RootElement.GetProperty("y").GetDouble() * 472), panel.surface);
            panel.Pointer(1, (float)(point.X / ControllerMenu.Width), (float)(point.Y / ControllerMenu.Height));
            panel.Pointer(2, (float)(point.X / ControllerMenu.Width), (float)(point.Y / ControllerMenu.Height));
        }
        await WaitFor(() => Task.FromResult(panel.permissionQueue.Count == 1), "Notification request did not reach the general permission prompt");
        if (!panel.permissionQueue[0].Scopes.Contains("permission:Notifications")) throw new Exception("General permissions must identify their capability");
        Click(Loc.Get("BrowserPermissionDeny"));
        await WaitFor(() => ScriptIs("window.notificationResult", "denied"), "General permission Deny did not reach Chromium");
        if (FloatingBrowserRuntime.Permissions.Choice(origin, origin, "permission:Notifications") != false)
            throw new Exception("General permission decisions must be remembered independently of microphone access");
        // Cancellation is independent of the permission type and never saves a grant.
        PermissionAnswer? answer = null;
        var pending = new PendingPermission(origin, "https://embedded.example", ["camera"], Volatile.Read(ref panel.permissionEpoch), a => answer = a, () => { });
        panel.EnqueuePermission(pending); panel.PermissionsNavigating();
        await WaitFor(() => Task.FromResult(answer.HasValue), "Navigation did not cancel a pending permission callback");
        if (answer != PermissionAnswer.Cancel || FloatingBrowserRuntime.Permissions.Choice(origin, "https://embedded.example", "camera") != null)
            throw new Exception("Navigation must not remember or grant an unanswered permission");
        if (pending.Resolve(PermissionAnswer.Allow)) throw new Exception("A dismissed request must not resolve twice");
        File.WriteAllText(Path.Combine(output, "browser-permissions-pass.txt"), "PASS: real Chromium media callback with synthetic devices and general notification callback; in-panel origin/capability prompt; Allow, Deny, remembered decisions, reset/reload and navigation cancellation. No real microphone or camera opened.");
    }
}
