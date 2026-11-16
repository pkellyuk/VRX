using CefSharp;
using System.IO;
using CefSharp.Handler;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;

namespace VRX.Desktop;

internal sealed partial class FloatingBrowser
{
    private GameHelpImage? helpImage;
    private CancellationTokenSource? helpCancel;
    private Button helpRetry = null!;
    private bool helpAttached, helpFilled, helpSent, helpAttempted;
    private int helpAttachmentEpoch;
    private string helpStage = "";
    private void AddGameHelpControls(DockPanel nav)
    {
        helpRetry = Button(Loc.Get("GameHelpRetry"), 110, RetryGameHelp);
        helpRetry.Visibility = Visibility.Collapsed; DockPanel.SetDock(helpRetry, Dock.Right); nav.Children.Add(helpRetry);
    }
    internal void StartGameHelp(GameHelpImage image)
    {
        StopGameHelp(); helpImage = image; helpAttached = helpFilled = helpSent = helpAttempted = false;
        helpRetry.Visibility = Visibility.Visible; RetryGameHelp();
    }
    private void StopGameHelp()
    {
        helpCancel?.Cancel(); helpCancel?.Dispose(); helpCancel = null;
        helpImage?.Dispose(); helpImage = null;
    }
    private void RetryGameHelp()
    {
        if (helpImage == null || helpSent || Closed) return;
        if (!helpAttached && helpStage == "GameHelpRetryNeeded") helpAttempted = false;
        helpCancel?.Cancel(); helpCancel?.Dispose(); helpCancel = new();
        _ = RunGameHelp(helpImage, helpCancel.Token);
    }
    private void HelpStatus(GameHelpProvider provider, string stage)
    {
        helpStage = stage;
        string label = stage switch
        {
            "GameHelpOpening" => Loc.Get("GameHelpOpening"),
            "GameHelpUploading" => Loc.Get("GameHelpUploading"),
            "GameHelpPreparingQuestion" => Loc.Get("GameHelpPreparingQuestion"),
            "GameHelpSearching" => Loc.Get("GameHelpSearching"),
            "GameHelpNeedsSetup" => Loc.Get("GameHelpNeedsSetup"),
            "GameHelpNoConsent" => Loc.Get("GameHelpNoConsent"),
            "GameHelpExistingDraft" => Loc.Get("GameHelpExistingDraft"),
            _ => Loc.Get("GameHelpRetryNeeded")
        };
        string text = Loc.Format("GameHelpProgress", provider.Name, label);
        if (status.Text != text) { status.Text = text; Changed(); }
    }
    // Both the C# address check and a guard inside the target document are used.
    // A redirect between checks must never receive the screenshot bytes.
    private static string HelpGuard(GameHelpProvider provider) =>
        "if(window.location.protocol!==" + JsonSerializer.Serialize(provider.LoopbackFixture ? "http:" : "https:") +
        (provider.LoopbackFixture ? "||window.location.host!==" + JsonSerializer.Serialize(new Uri(provider.Url).Authority) : "||window.location.port!==''&&window.location.port!=='443'") +
        "||!(" + string.Join("||", provider.Hosts.Select(h => "window.location.hostname===" + JsonSerializer.Serialize(h))) + "))return 'wrong-origin';";
    private static string HelpComposerScript(GameHelpProvider provider) => provider.Id switch
    {
        "google-lens" => "[...document.querySelectorAll('textarea,input')].find(e=>visible(e)&&(/ask anything|add to your search/i.test((e.getAttribute('aria-label')||'')+' '+(e.placeholder||''))||e.name==='q'))",
        "google-ai" => "[...document.querySelectorAll('textarea')].find(e=>visible(e)&&/ask anything/i.test((e.getAttribute('aria-label')||'')+' '+(e.placeholder||'')))",
        "gemini" => "[...document.querySelectorAll('[contenteditable=true]')].find(e=>visible(e)&&/prompt.*gemini/i.test(e.getAttribute('aria-label')||''))",
        _ => "[...document.querySelectorAll('textarea,[contenteditable=true]')].find(e=>visible(e)&&/message|ask|copilot|prompt/i.test((e.getAttribute('aria-label')||'')+' '+(e.getAttribute('placeholder')||'')))"
    };
    private static string HelpPrelude(GameHelpProvider provider) => "(()=>{" + HelpGuard(provider) +
        "const visible=e=>{let r=e.getBoundingClientRect();return r.width>0&&r.height>0;};";
    private static string HelpReadyScript(GameHelpProvider provider)
    {
        string composer = HelpComposerScript(provider);
        return HelpPrelude(provider) + "const body=document.body.innerText;" +
            "if(/before you continue to google|sign in to try tools|unusual traffic|verify you are human/i.test(body))return 'setup';" +
            "let composer=" + composer + ";" +
            (provider.Id == "google-lens" ? "let lens=/Search any image with Google Lens/i.test(body);" : "let lens=false;") +
            "if(composer||lens){let inputs=[...document.querySelectorAll('input[type=file]')].filter(e=>!e.disabled);" +
            "let file=inputs.find(e=>/image|\\.jpg|\\.png/i.test(e.accept))||inputs.find(e=>!e.accept);if(file){file.setAttribute('data-vrx-help-upload','1');return 'file';}}" +
            "let buttons=[...document.querySelectorAll('button,[role=button]')].filter(visible);" +
            "let add=buttons.find(e=>/^(Search by image|Add files, tools and select a model|Upload & tools|Add photos and files|Attach files|Add content)$/i.test(e.getAttribute('aria-label')||''));" +
            "if(add){add.click();return 'opening';}" +
            (provider.Id == "copilot" ? "let chat=document.querySelector('a[aria-label=\"Go to Chat\"]');if(chat){chat.click();return 'opening';}" : "") +
            "return 'setup';})()";
    }
    private static string HelpAttachScript(GameHelpProvider provider, byte[] bytes)
    {
        return HelpPrelude(provider) + "const input=document.querySelector('input[type=file][data-vrx-help-upload=\"1\"]');if(!input||input.disabled)return 'missing';" +
            "const raw=atob(" + JsonSerializer.Serialize(Convert.ToBase64String(bytes)) + ");const data=new Uint8Array(raw.length);for(let n=0;n<raw.length;n++)data[n]=raw.charCodeAt(n);" +
            "const transfer=new DataTransfer();transfer.items.add(new File([data],'vrx-game-help.jpg',{type:'image/jpeg'}));input.files=transfer.files;" +
            "window.__vrxGameHelpAttached=true;input.dispatchEvent(new Event('input',{bubbles:true}));input.dispatchEvent(new Event('change',{bubbles:true}));return 'attached';})()";
    }
    private static string HelpFillScript(GameHelpProvider provider, string prompt) => HelpPrelude(provider) +
        "const input=" + HelpComposerScript(provider) + ";if(!input)return 'missing';const prompt=" + JsonSerializer.Serialize(prompt) + ";" +
        "let old=input.isContentEditable?input.innerText:input.value;" + (provider.Id == "google-lens" ? "" : "if(old.trim()&&old.trim()!==prompt.trim())return 'draft';") + "input.focus();" +
        "if(input.isContentEditable){document.execCommand('selectAll',false,null);document.execCommand('insertText',false,prompt);}else{" +
        "let proto=input.tagName==='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;Object.getOwnPropertyDescriptor(proto,'value').set.call(input,prompt);}" +
        "input.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'insertText',data:prompt}));input.dispatchEvent(new Event('change',{bubbles:true}));return 'filled';})()";
    private static string HelpSendScript(GameHelpProvider provider, string prompt) => HelpPrelude(provider) +
        "const input=" + HelpComposerScript(provider) + ";if(!input)return 'missing';" +
        "if((input.isContentEditable?input.innerText:input.value).trim()!==" + JsonSerializer.Serialize(prompt) + ".trim())return 'missing';" +
        "let send=[...document.querySelectorAll('button,[role=button],input[type=submit]')].find(e=>visible(e)&&!e.disabled&&e.getAttribute('aria-disabled')!=='true'&&/^(Send|Submit|Submit prompt|Send message|Search|Google Search)$/i.test(e.getAttribute('aria-label')||e.value||e.innerText||''));" +
        (provider.Id == "google-lens" ? "if(!send&&input.name==='q'){input.focus();return document.activeElement===input?'enter':'waiting';}" : "") +
        "if(!send)return 'waiting';send.click();return 'sent';})()";
    private void CompleteGameHelp(GameHelpImage image)
    {
        helpSent = true; helpRetry.Visibility = Visibility.Collapsed; HelpStatus(image.Provider, "GameHelpSearching"); image.Dispose();
    }
    private async Task<JavascriptResponse?> HelpEvaluate(string script, CancellationToken token)
    {
        if (!browser.CanExecuteJavascriptInMainFrame) return null;
        try { var result = await browser.EvaluateScriptAsync(script); token.ThrowIfCancellationRequested(); return result; }
        catch (Exception) when (!token.IsCancellationRequested && (browser.IsLoading || !browser.CanExecuteJavascriptInMainFrame)) { return null; }
    }
    private async Task RunGameHelp(GameHelpImage image, CancellationToken token)
    {
        var provider = image.Provider; string prompt = Loc.Get("GameHelpPrompt");
        HelpStatus(provider, "GameHelpOpening");
        var deadline = DateTime.UtcNow.AddSeconds(60);
        try
        {
            while (DateTime.UtcNow < deadline)
            {
                token.ThrowIfCancellationRequested();
                if (Closed || !ReferenceEquals(helpImage, image)) return;
                if (addressFocus) { StopGameHelp(); helpRetry.Visibility = Visibility.Collapsed; return; }
                if (!GameHelpPreferences.Current.Allowed(provider)) { HelpStatus(provider, "GameHelpNoConsent"); StopGameHelp(); return; }
                if (!browser.IsBrowserInitialized || !browser.CanExecuteJavascriptInMainFrame) { await Task.Delay(250, token); continue; }
                if (!provider.CanUpload(browser.Address)) { HelpStatus(provider, "GameHelpNeedsSetup"); await Task.Delay(700, token); continue; }
                if (helpAttached && Volatile.Read(ref permissionEpoch) != helpAttachmentEpoch)
                {
                    bool visualResult = (provider.Id is "google-lens" or "google-ai") &&
                        !string.IsNullOrEmpty(System.Web.HttpUtility.ParseQueryString(new Uri(browser.Address).Query)["vsrid"]);
                    if (!visualResult) helpAttached = helpFilled = helpAttempted = false;
                    helpAttachmentEpoch = Volatile.Read(ref permissionEpoch);
                }
                if (helpAttached && (provider.Id is "google-lens" or "google-ai") &&
                    System.Web.HttpUtility.ParseQueryString(new Uri(browser.Address).Query)["q"] == prompt)
                { CompleteGameHelp(image); return; }
                if (!helpAttached)
                {
                    if (helpAttempted)
                    {
                        var marker = await HelpEvaluate(HelpPrelude(provider) + "return window.__vrxGameHelpAttached===true;})()", token);
                        bool visualResult = (provider.Id is "google-lens" or "google-ai") && !string.IsNullOrEmpty(System.Web.HttpUtility.ParseQueryString(new Uri(browser.Address).Query)["vsrid"]);
                        if (marker?.Result is true || visualResult) { helpAttached = true; helpAttachmentEpoch = Volatile.Read(ref permissionEpoch); }
                        else HelpStatus(provider, "GameHelpOpening");
                        await Task.Delay(500, token); continue;
                    }
                    var ready = await HelpEvaluate(HelpReadyScript(provider), token);
                    if (ready?.Result?.ToString() != "file")
                    { HelpStatus(provider, ready == null || ready.Result?.ToString() == "opening" ? "GameHelpOpening" : "GameHelpNeedsSetup"); await Task.Delay(700, token); continue; }
                    if (!GameHelpPreferences.Current.Allowed(provider)) continue;
                    HelpStatus(provider, "GameHelpUploading");
                    helpAttempted = true;
                    var uploaded = await HelpEvaluate(HelpAttachScript(provider, image.Bytes), token);
                    if (uploaded?.Result?.ToString() != "attached") { await Task.Delay(500, token); continue; }
                    helpAttached = true; helpAttachmentEpoch = Volatile.Read(ref permissionEpoch); await Task.Delay(1800, token); continue;
                }
                if (!helpFilled)
                {
                    HelpStatus(provider, "GameHelpPreparingQuestion");
                    var filled = await HelpEvaluate(HelpFillScript(provider, prompt), token);
                    if (filled?.Result?.ToString() == "draft") { HelpStatus(provider, "GameHelpExistingDraft"); return; }
                    helpFilled = filled?.Result?.ToString() == "filled"; await Task.Delay(600, token); continue;
                }
                var sent = await HelpEvaluate(HelpSendScript(provider, prompt), token);
                if (sent?.Result?.ToString() == "enter")
                {
                    var host = browser.GetBrowserHost(); host.SendFocusEvent(true);
                    host.SendKeyEvent(new KeyEvent { WindowsKeyCode = 13, NativeKeyCode = 13, Type = KeyEventType.RawKeyDown });
                    host.SendKeyEvent(new KeyEvent { WindowsKeyCode = 13, NativeKeyCode = 13, Type = KeyEventType.Char });
                    host.SendKeyEvent(new KeyEvent { WindowsKeyCode = 13, NativeKeyCode = 13, Type = KeyEventType.KeyUp });
                    await Task.Delay(900, token); continue;
                }
                if (sent?.Result?.ToString() == "sent")
                {
                    CompleteGameHelp(image); return;
                }
                if (sent?.Result?.ToString() == "missing") helpFilled = false;
                await Task.Delay(500, token);
            }
            HelpStatus(provider, "GameHelpRetryNeeded");
        }
        catch (OperationCanceledException) { }
        catch (Exception) when (!Closed && !token.IsCancellationRequested)
        { HelpStatus(provider, "GameHelpRetryNeeded"); }
    }
    private sealed class GameHelpFileDialogs(FloatingBrowser owner) : DialogHandler
    {
        protected override bool OnFileDialog(IWebBrowser control, IBrowser browser, CefFileDialogMode mode, string title, string defaultFilePath,
            IReadOnlyCollection<string> filters, IReadOnlyCollection<string> extensions, IReadOnlyCollection<string> descriptions, IFileDialogCallback callback)
        {
            if (owner.helpImage == null || owner.helpSent) return false;
            // Automated handoff supplies a File object to the approved page itself.
            // An attachment control must never leave an OS picker stranded in VR.
            using (callback) callback.Cancel(); return true;
        }
    }
    private sealed class ProbeFileDialogs : DialogHandler
    {
        protected override bool OnFileDialog(IWebBrowser control, IBrowser browser, CefFileDialogMode mode, string title, string defaultFilePath,
            IReadOnlyCollection<string> filters, IReadOnlyCollection<string> extensions, IReadOnlyCollection<string> descriptions, IFileDialogCallback callback)
        { using (callback) callback.Cancel(); return true; }
    }
    internal static async Task ProbeGameHelp(string output)
    {
        if (!Environment.GetCommandLineArgs().Contains("--smoke-test")) throw new InvalidOperationException("Provider probes require an isolated smoke profile");
        var reports = new List<string>();
        foreach (var url in new[] { "https://www.google.com/imghp?hl=en", "https://www.google.com/ai?hl=en", "https://copilot.microsoft.com/", "https://gemini.google.com/app" })
        {
            using var panel = new FloatingBrowser(_ => { }, () => { }, _ => { }, url);
            panel.browser.DialogHandler = new ProbeFileDialogs();
            var deadline = DateTime.UtcNow.AddSeconds(25);
            while ((!panel.browser.IsBrowserInitialized || !panel.browser.CanExecuteJavascriptInMainFrame) && DateTime.UtcNow < deadline) await Task.Delay(100);
            await Task.Delay(2500);
            if (!panel.browser.CanExecuteJavascriptInMainFrame) { reports.Add(url + "\nNo document context"); continue; }
            var response = await panel.browser.EvaluateScriptAsync("JSON.stringify({url:location.href,title:document.title,text:document.body.innerText.slice(0,2500),inputs:[...document.querySelectorAll('input,textarea,[contenteditable=true]')].map(e=>({tag:e.tagName,type:e.type,id:e.id,accept:e.accept,label:e.getAttribute('aria-label'),placeholder:e.getAttribute('placeholder')})),buttons:[...document.querySelectorAll('button,[role=button]')].map(e=>({tag:e.tagName,text:e.innerText.slice(0,90),label:e.getAttribute('aria-label')})).slice(0,60)})");
            reports.Add(url + "\n" + response.Result);
            await panel.browser.EvaluateScriptAsync("(()=>{let b=[...document.querySelectorAll('button')].find(e=>/^(Reject all|Reject)$/.test(e.innerText.trim()));if(b)b.click();})()");
            await Task.Delay(1800);
            await panel.browser.EvaluateScriptAsync("(()=>{let b=[...document.querySelectorAll('button,[role=button]')].find(e=>/^(Search by image|Add files, tools and select a model|Upload & tools)$/.test(e.getAttribute('aria-label')||''));if(b)b.click();})()");
            await Task.Delay(1200);
            response = await panel.browser.EvaluateScriptAsync("JSON.stringify({url:location.href,text:document.body.innerText.slice(-3500),inputs:[...document.querySelectorAll('input,textarea,[contenteditable=true]')].map(e=>({tag:e.tagName,type:e.type,id:e.id,accept:e.accept,label:e.getAttribute('aria-label')})),buttons:[...document.querySelectorAll('button,[role=button],a')].map(e=>({text:e.innerText.slice(0,90),label:e.getAttribute('aria-label'),href:e.getAttribute('href')})).filter(e=>e.label||e.href||/upload|file|image/i.test(e.text)).slice(-45)})");
            reports.Add("AFTER CONTROLS\n" + response.Result);
            var provider = GameHelpPreferences.Providers.Single(p => p.Url == url);
            if (provider.Id is "google-lens" or "google-ai")
            {
                GameHelpPreferences.Current.Permit(provider, true);
                panel.StartGameHelp(CreateHelpTestImage(provider));
                deadline = DateTime.UtcNow.AddSeconds(40);
                while (!panel.helpSent && DateTime.UtcNow < deadline) await Task.Delay(250);
                await Task.Delay(3000);
                if (panel.browser.CanExecuteJavascriptInMainFrame)
                {
                    response = await panel.browser.EvaluateScriptAsync("JSON.stringify({url:location.href,text:document.body.innerText.slice(0,1600),inputs:[...document.querySelectorAll('textarea,input:not([type=hidden]),[contenteditable=true]')].map(e=>({tag:e.tagName,type:e.type,label:e.getAttribute('aria-label'),value:(e.value||e.innerText||'').slice(0,300)})),buttons:[...document.querySelectorAll('button,[role=button]')].map(e=>({text:e.innerText.slice(0,60),label:e.getAttribute('aria-label')})).slice(0,35)})");
                    reports.Add("HANDOFF " + provider.Name + " sent=" + panel.helpSent + " attached=" + panel.helpAttached + " stage=" + panel.helpStage + "\n" + response.Result);
                    byte[] pixels = panel.Render();
                    var shot = new System.Windows.Media.Imaging.PngBitmapEncoder();
                    shot.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(System.Windows.Media.Imaging.BitmapSource.Create(pixelWidth: ControllerMenu.PixelWidth, pixelHeight: ControllerMenu.PixelHeight, dpiX: 192, dpiY: 192,
                        pixelFormat: System.Windows.Media.PixelFormats.Pbgra32, palette: null, pixels: pixels, stride: ControllerMenu.PixelWidth * 4)));
                    using var file = File.Create(Path.Combine(output, "game-help-" + provider.Id + ".png")); shot.Save(file);
                }
            }
            File.WriteAllText(Path.Combine(output, "game-help-provider-probe.txt"), string.Join("\n\n", reports));
        }
    }
    private static GameHelpImage CreateHelpTestImage(GameHelpProvider provider)
    {
        var visual = new System.Windows.Media.DrawingVisual();
        using (var dc = visual.RenderOpen())
        {
            dc.DrawRectangle(System.Windows.Media.Brushes.MidnightBlue, null, new Rect(0, 0, 640, 420));
            var type = new System.Windows.Media.Typeface("Segoe UI");
            void Text(string value, double size, double y) => dc.DrawText(new System.Windows.Media.FormattedText(value,
                System.Globalization.CultureInfo.GetCultureInfo("en-US"), FlowDirection.LeftToRight, type, size, System.Windows.Media.Brushes.White, 1), new Point(20, y));
            Text("SOKOBAN TEST PUZZLE", 28, 10); Text("VRX synthetic test image", 16, 385);
            for (int y = 0; y < 5; y++) for (int x = 0; x < 9; x++)
                dc.DrawRectangle(x == 0 || x == 8 || y == 0 || y == 4 ? System.Windows.Media.Brushes.SlateGray : System.Windows.Media.Brushes.LightGray,
                    new System.Windows.Media.Pen(System.Windows.Media.Brushes.DarkSlateGray, 2), new Rect(50 + x * 56, 70 + y * 56, 54, 54));
            dc.DrawEllipse(System.Windows.Media.Brushes.ForestGreen, null, new Point(50 + 2.5 * 56, 70 + 2.5 * 56), 18, 18);
            dc.DrawRectangle(System.Windows.Media.Brushes.Goldenrod, null, new Rect(50 + 3 * 56 + 7, 70 + 2 * 56 + 7, 40, 40));
            dc.DrawEllipse(System.Windows.Media.Brushes.Crimson, null, new Point(50 + 6.5 * 56, 70 + 2.5 * 56), 16, 16);
        }
        var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap(640, 420, 96, 96, System.Windows.Media.PixelFormats.Pbgra32); bitmap.Render(visual);
        return GameHelpImage.Capture(bitmap, provider);
    }
    private static async Task GameHelpSmokeTest(string output)
    {
        GameHelpPreferences.SmokeTest(output);
        const string html = "<html><body><input type=file accept='image/jpeg'><textarea aria-label='Ask anything'></textarea><button aria-label='Send'>Send</button><script>window.uploads=0;window.sent=false;document.querySelector('input').onchange=async function(){window.uploads++;window.fileName=this.files[0].name;window.fileBytes=(await this.files[0].arrayBuffer()).byteLength;};document.querySelector('button').onclick=function(){window.receivedQuestion=document.querySelector('textarea').value;window.sent=!!window.fileBytes&&!!window.receivedQuestion;document.querySelector('textarea').value='';};</script></body></html>";
        using var server = new PermissionTestServer(html);
        var provider = new GameHelpProvider("google-ai", "Local test", server.Url, ["127.0.0.1"], true);
        using var panel = new FloatingBrowser(_ => { }, () => { }, _ => { }, server.Url);
        async Task WaitFor(Func<Task<bool>> condition, string failure)
        {
            var deadline = DateTime.UtcNow.AddSeconds(12);
            while (DateTime.UtcNow < deadline) { if (await condition()) return; await Task.Delay(30); }
            throw new Exception(failure);
        }
        async Task<bool> ScriptTrue(string script)
        {
            if (!panel.browser.IsBrowserInitialized || !panel.browser.CanExecuteJavascriptInMainFrame) return false;
            var result = await panel.browser.EvaluateScriptAsync(script); return result.Success && result.Result is true;
        }
        await WaitFor(() => ScriptTrue("window.uploads===0"), "Game-help fixture did not load");
        var preferences = GameHelpPreferences.Current; preferences.Permit(GameHelpPreferences.Providers.Single(p => p.Id == "google-ai"), false);
        var refused = CreateHelpTestImage(provider); panel.StartGameHelp(refused);
        await WaitFor(() => Task.FromResult(panel.helpStage == "GameHelpNoConsent"), "Game help must check screenshot-sharing consent");
        if (!await ScriptTrue("window.uploads===0") || refused.Bytes.Any(b => b != 0)) throw new Exception("Without consent, no image may be transferred and the image buffer must be cleared");
        preferences.Permit(GameHelpPreferences.Providers.Single(p => p.Id == "google-ai"), true);
        var image = CreateHelpTestImage(provider); panel.StartGameHelp(image);
        await WaitFor(() => Task.FromResult(panel.helpSent), "Game-help fixture did not complete automatic attachment and submission");
        if (!await ScriptTrue("window.sent===true&&window.uploads===1&&window.fileName==='vrx-game-help.jpg'&&window.fileBytes>1000") || image.Bytes.Any(b => b != 0))
            throw new Exception("Game help must attach one image without a picker, submit and clear its source buffer");
        var question = await panel.browser.EvaluateScriptAsync("window.receivedQuestion");
        if (question.Result?.ToString() != Loc.Get("GameHelpPrompt")) throw new Exception("Game help must send the configured question with its image");
        var rejected = await panel.browser.EvaluateScriptAsync(HelpAttachScript(GameHelpPreferences.Providers[0], [1, 2, 3]));
        if (rejected.Result?.ToString() != "wrong-origin" || !await ScriptTrue("window.uploads===1")) throw new Exception("The document-side guard must reject attachment on an unapproved origin");
        preferences.Permit(GameHelpPreferences.Providers.Single(p => p.Id == "google-ai"), false);
        File.WriteAllText(Path.Combine(output, "game-help-pass.txt"), "PASS: per-provider consent defaults/persistence/revocation; approved-origin guard; synthetic JPEG transfer through a real browser file input; automatic question submission; exactly one upload; source buffer cleanup. Local fixture only; live-provider probes are separate.");
    }
}
