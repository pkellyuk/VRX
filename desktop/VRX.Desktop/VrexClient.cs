using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Text.Json;
using System.Threading.Channels;
using System.Windows;
using VRX.VrexBridge;

namespace VRX.Desktop;

internal enum VrexVmPhase { Off, Preparing, Booting, Running, Stopping, Moving, Failed, DisplayUnavailable }

internal interface IVrexClient
{
    int Width { get; }
    int Height { get; }
    string Status { get; }
    string State { get; }
    VrexVmPhase VmPhase { get; }
    string VmDetail { get; }
    bool Ready { get; }
    int Revision { get; }
    bool FrameChanged {get;}
    event Action<VrexEvent>? Changed;
    void EnsureWorker();
    bool Send(VrexRequest request);
    void Stop();
    byte[]? ReadFrame();
}

internal sealed class VrexClient : IVrexClient
{
    internal static VrexClient Current { get; } = new();
    private Process? process;
    private Task? stopping;
    private Channel<VrexRequest>? requests;
    private MemoryMappedFile? map;
    private MemoryMappedViewAccessor? memory;
    private Mutex? frameLock;
    private string channel = "";
    private int generation;
    private byte[] frame = [];
    public int Width { get; private set; } = 1280;
    public int Height { get; private set; } = 800;
    public string Status { get; private set; } = "Vrex Ubuntu is stopped.";
    public VrexVmPhase VmPhase { get; private set; }
    public string VmDetail { get; private set; } = "Start Ubuntu when you’re ready.";
    private string state="off";
    private bool observerSpeaking;
    private AudioDirection? audioDirection;
    private readonly UbuntuSpatialAudio guestAudio = new();
    internal void ConfigureGuestAudio(bool spatial, BrowserAudioBackend backend, int volume) => guestAudio.Configure(spatial, backend, volume);
    internal AudioDirection? GuestAudioDirection { set => guestAudio.SetDirection(value); }
    private float lastVoicePan = float.NaN;
    private float lastVoiceRear;
    internal AudioDirection? AudioDirection
    {
        set
        {
            audioDirection = value;
            if (!LocalVoiceInstalled || !Running) return;
            float distance = value == null ? 0 : MathF.Sqrt(value.X * value.X + value.Y * value.Y + value.Z * value.Z);
            float pan = distance > .01f ? value!.X / distance : 0f;
            float rear = distance > .01f ? MathF.Max(0, value!.Z / distance) : 0f;
            if (MathF.Abs(pan - lastVoicePan) < .025f && MathF.Abs(rear - lastVoiceRear) < .025f) return;
            if (requests?.Writer.TryWrite(new("voice-pan", Pan: pan, Rear: rear)) == true)
            {
                lastVoicePan = pan;
                lastVoiceRear = rear;
            }
        }
    }
    internal bool LiveVoiceActive { get; private set; }
    internal int Avatar { get; private set; }
    internal uint ShellRgb { get; private set; }
    internal string VoiceStatus { get; private set; } = "";
    internal VrexEvent VoiceOptions { get; private set; } = new("voice-options");
    internal bool LocalVoiceInstalled => VoiceOptions.VoiceInstalled;
    internal void ToggleLiveVoice()
    {
        if (LiveVoiceActive) { StopLiveVoice(); return; }
        if (!CanConverse)
        {
            VoiceStatus = "Choose a model in Vrex chat Settings to use live voice.";
            Notify(new("voice-state"));
            return;
        }
        if (!Send(new("voice-start"))) return;
        LiveVoiceActive = true;
        VoiceStatus = "Starting live voice…";
        Notify(new("voice-state"));
    }

    internal void StopLiveVoice()
    {
        if (!LiveVoiceActive || !Running) return;
        if (!Send(new("voice-stop"))) return;
        LiveVoiceActive = false;
        VoiceStatus = "Stopping live voice…";
        Notify(new("voice-state"));
    }
    internal void StopChat()
    {
        chatStopRevision++;
        pcPermissionRevision++;
        nextPcLook = 0;
        if (!Running || !Send(new("chat-stop"))) return;
        VoiceStatus = "Stopping Vrex…";
        Notify(new("voice-state"));
    }
    private bool speakReplies;
    private bool allowPcScreen, allowPanelScreen, helpful;
    private Task<VrexScreenCapture.Frame>? pcCapture;
    private long nextPcLook;
    private int pcPermissionRevision;
    private int chatStopRevision;
    private bool chatOpen;
    private readonly string chatPreferencesPath = Path.Combine(VrxDataDirectory.Root, "vrex-chat-settings.json");
    internal bool AllowPcScreen => allowPcScreen;
    internal bool AllowPanelScreen => allowPanelScreen;
    internal bool AllowUbuntuMarks { get; private set; }
    internal bool AllowBotClipboard { get; private set; }
    internal bool AllowBotFileCopy { get; private set; }
    private VrexMainScreenMarks? mainScreenMarks;
    internal IReadOnlyList<string> PcTools { get; private set; } = [];
    internal bool Helpful => helpful;
    internal void TogglePcScreen()
    {
        allowPcScreen = !allowPcScreen;
        SaveChatPreferences();
        pcPermissionRevision++;
        nextPcLook = 0;
        if (Running) Send(new("pc-sharing", Key: allowPcScreen ? 1 : 0));
        if (!allowPcScreen && Running) Send(new("stop-observing"));
        if (!allowPcScreen) ClearMainScreenMarks();
        Notify(new("pc-mode"));
    }
    internal void ToggleHelpful()
    {
        helpful = !helpful;
        SaveChatPreferences();
        nextPcLook = 0;
        if (!helpful && Running) Send(new("stop-observing"));
        Notify(new("pc-mode"));
    }
    internal void TogglePanelScreen()
    {
        allowPanelScreen = !allowPanelScreen;
        SaveChatPreferences();
        nextPcLook = 0;
        if (!allowPanelScreen && Running) Send(new("stop-observing"));
        Notify(new("pc-mode"));
    }

    internal void PollPcScreen(Func<(VrexScreenCapture.Frame Frame, string Surface)?> capturePanel)
    {
        if (!chatOpen || (!allowPcScreen && !allowPanelScreen) || !helpful || !CanConverse || LiveVoiceActive || State is not ("idle" or "off" or "starting"))
        { nextPcLook = 0; return; }
        long now = Environment.TickCount64;
        if (nextPcLook == 0) { nextPcLook = now + Random.Shared.Next(5, 31) * 1000; return; }
        if (now < nextPcLook || pcCapture != null) return;
        nextPcLook = now + Random.Shared.Next(5, 31) * 1000;
        if (allowPanelScreen && capturePanel() is { } panel)
        {
            Send(new("observe-pc", panel.Surface, panel.Frame.PointerX, panel.Frame.PointerY,
                PcImage: Convert.ToBase64String(panel.Frame.Png)));
            return;
        }
        if (allowPcScreen) _ = CapturePcAsync(null);
    }

    internal async Task<bool> CapturePcAsync(string? prompt)
    {
        if (!allowPcScreen || !CanConverse || (prompt == null && (!helpful || pcCapture != null))) return false;
        var capture = pcCapture ??= Task.Run(VrexScreenCapture.Capture);
        int permission = pcPermissionRevision;
        try
        {
            var image = await capture;
            if (!allowPcScreen || permission != pcPermissionRevision || !CanConverse ||
                (prompt == null && (!helpful || LiveVoiceActive || State is not ("idle" or "off" or "starting")))) return false;
            return Send(new(prompt == null ? "observe-pc" : "chat", prompt, image.PointerX, image.PointerY,
                PcImage: Convert.ToBase64String(image.Png)));
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            if (prompt != null) { Status = "PC screen capture failed: " + ex.Message; Notify(new("status", Status)); }
            return false;
        }
        finally { if (ReferenceEquals(pcCapture, capture)) pcCapture = null; }
    }

    internal async Task<bool> SendChatAsync(string prompt)
    {
        bool sent;
        if (!VrexPcPrompt.RequestsPcScreen(prompt)) sent = Send(new("chat", prompt));
        else
        {
            int revision = chatStopRevision;
            sent = AllowPcScreen && await CapturePcAsync(prompt);
            if (!sent && revision == chatStopRevision) sent = Send(new("chat", prompt + VrexPcPrompt.MissingContext));
        }
        if (sent) RecordTypedMessage(prompt);
        return sent;
    }

    internal async Task<string?> GuideAsync(string draft, Func<(VrexScreenCapture.Frame Frame, string Surface)?>? capturePanel)
    {
        string prompt = VrexPcPrompt.Guide(draft);
        int revision = chatStopRevision;
        bool sent = AllowPcScreen && await CapturePcAsync(prompt);
        if (revision != chatStopRevision || !CanConverse) return "Guide Me was stopped.";
        if (!sent && AllowPanelScreen && capturePanel?.Invoke() is { } panel)
            sent = Send(new("chat", prompt, panel.Frame.PointerX, panel.Frame.PointerY,
                PcImage: Convert.ToBase64String(panel.Frame.Png), ScreenSurface: panel.Surface));
        if (sent) { RecordTypedMessage(draft.Length == 0 ? "Guide Me" : draft); return null; }
        if (AllowPcScreen) return "Couldn’t send the main screen. Try Guide Me again.";
        return AllowPanelScreen ? "Point at the browser or settings panel, then press Guide Me."
            : "Turn on PC screen to get guidance for your game.";
    }

    internal void ChooseReply(string choice)
    {
        if (Send(new("choose", choice, SpeakSummary: SpeakReplies))) RecordTypedMessage(choice);
    }

    private readonly List<VrexChatLine> conversationLog = [];
    internal IEnumerable<VrexChatLine> ChatHistory => conversationLog;
    private readonly List<VrexChatSummary> incomingChats = [];
    private List<VrexChatLine>? incomingTranscript;
    private string? incomingChatId;
    internal IReadOnlyList<VrexChatSummary> Chats { get; private set; } = [];
    internal string? ActiveChatId { get; private set; }
    internal bool ChatChanging { get; private set; }

    internal void ChangeChat(string action, string? id = null)
    {
        if (!chatOpen || ChatChanging || !Send(new(action, id))) return;
        ChatChanging = true;
        chatStopRevision++;
        pcPermissionRevision++;
        nextPcLook = 0;
        Notify(new("chat-list-pending"));
    }
    internal IReadOnlyList<VrexTaskStep> TaskSteps { get; private set; } = [];

    private void RecordTypedMessage(string text)
    {
        if (VoiceStatus == "Vrex stopped.") { VoiceStatus = ""; Notify(new("voice-state")); }
        RecordChatLine("You", text);
        Notify(new("chat-written", text));
    }

    private void RecordChatLine(string speaker, string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return;
        conversationLog.Add(new(speaker == "You", text));
    }

    private Func<(VrexScreenCapture.Frame Frame, string Surface)?>? captureGuidePanel;

    private async Task AnswerVoiceScreenAsync(int id, bool guide)
    {
        string? screenshot = null;
        string? surface = null;
        int x = -1, y = -1;
        int permission = pcPermissionRevision;
        var worker = process;
        if (AllowPcScreen && LiveVoiceActive && CanConverse)
        {
            try
            {
                var frame = await Task.Run(VrexScreenCapture.Capture);
                if (AllowPcScreen && permission == pcPermissionRevision && LiveVoiceActive)
                {
                    screenshot = Convert.ToBase64String(frame.Png);
                    x = frame.PointerX;
                    y = frame.PointerY;
                }
            }
            catch (Exception ex) when (ex is IOException or InvalidOperationException or System.ComponentModel.Win32Exception)
            { Debug.WriteLine("[Vrex screen capture] " + ex.Message); }
        }
        if (!Running || !ReferenceEquals(worker, process) || !LiveVoiceActive || !CanConverse) return;
        if (permission != pcPermissionRevision) screenshot = null;
        else if (screenshot == null && guide && AllowPanelScreen && captureGuidePanel?.Invoke() is { } panel)
        {
            screenshot = Convert.ToBase64String(panel.Frame.Png);
            x = panel.Frame.PointerX;
            y = panel.Frame.PointerY;
            surface = panel.Surface;
        }
        Send(new("voice-context", X: x, Y: y, Key: id, PcImage: screenshot, ScreenSurface: surface));
    }
    internal bool SpeakReplies
    {
        get=>speakReplies;
        set
        {
            speakReplies=value;
            SaveChatPreferences();
            if(value) return;
            if (Running && !LiveVoiceActive) requests?.Writer.TryWrite(new("voice-silence"));
            Revision++;
        }
    }
    public string State
    {
        get => observerSpeaking && state is "idle" or "off" ? "speaking" : state;
        private set => state = value;
    }
    public bool Ready { get; private set; }
    public int Revision { get; private set; }
    internal bool VmBusy => VmPhase is VrexVmPhase.Preparing or VrexVmPhase.Booting or VrexVmPhase.Stopping or VrexVmPhase.Moving;
    public bool FrameChanged => Ready && VmPhase != VrexVmPhase.DisplayUnavailable && (memory==null || memory.ReadInt32(16)!=generation);
    internal VrexEvent Configuration { get; private set; } = new("settings", Model:"",Endpoint:"https://api.openai.com/v1",Provider:"openai-compatible");
    internal bool ConfigurationLoaded { get; private set; }
    internal bool CanConverse => chatOpen && Running && !ChatChanging && ConfigurationLoaded && !string.IsNullOrWhiteSpace(Configuration.Model);
    public event Action<VrexEvent>? Changed;
    internal bool Running => process is { HasExited:false };
    private VrexClient()
    {
        var saved = VrexChatPreferences.Load(chatPreferencesPath);
        helpful = saved.Helpful;
        speakReplies = saved.SpeakReplies;
        allowPcScreen = saved.AllowPcScreen;
        allowPanelScreen = saved.AllowPanelScreen;
        PcTools = saved.PcTools ?? [];
        AllowUbuntuMarks = saved.AllowUbuntuMarks;
        AllowBotClipboard = saved.AllowBotClipboard;
        AllowBotFileCopy = saved.AllowBotFileCopy;
    }

    private void SaveChatPreferences() => new VrexChatPreferences(helpful, speakReplies, allowPcScreen, allowPanelScreen, PcTools.ToArray(),
        AllowUbuntuMarks, AllowBotClipboard, AllowBotFileCopy)
        .Save(chatPreferencesPath);

    internal void SetChatOpen(bool open, Func<(VrexScreenCapture.Frame Frame, string Surface)?>? capturePanel = null)
    {
        if (open == chatOpen) return;
        if (open) EnsureWorker();
        chatOpen = open;
        captureGuidePanel = open ? capturePanel : null;
        nextPcLook = 0;
        if (!open)
        {
            ChatChanging = false;
            chatStopRevision++;
            pcPermissionRevision++;
            LiveVoiceActive = false;
            observerSpeaking = false;
            ClearAnnotations();
        }
        if (Running) Send(ChatActivation());
    }

    private VrexRequest ChatActivation() => new("chat-active", X: allowPcScreen ? 1 : 0, Key: chatOpen ? 1 : 0,
        PcTools: chatOpen ? PcTools.ToArray() : null, AllowUbuntuMarks: chatOpen && AllowUbuntuMarks,
        AllowBotClipboard: chatOpen && AllowBotClipboard, AllowBotFileCopy: chatOpen && AllowBotFileCopy);

    internal void SetBotTransferPermissions(bool clipboard, bool files)
    {
        if (!chatOpen || !Send(new("bot-transfer-permissions", AllowBotClipboard: clipboard, AllowBotFileCopy: files))) return;
        AllowBotClipboard = clipboard;
        AllowBotFileCopy = files;
        SaveChatPreferences();
        Notify(new("pc-mode"));
    }

    internal void SetUbuntuMarks(bool allowed)
    {
        if (!chatOpen || !Send(new("ubuntu-marks-permission", Key: allowed ? 1 : 0))) return;
        AllowUbuntuMarks = allowed;
        SaveChatPreferences();
        if (!allowed) Notify(new("screen-marks", "ubuntu", X: 1, Marks: []));
        Notify(new("pc-permissions"));
    }

    private void ClearMainScreenMarks()
    {
        mainScreenMarks?.Close();
        mainScreenMarks = null;
    }

    internal void ClearAnnotations()
    {
        ClearMainScreenMarks();
        Notify(new("screen-marks", "ubuntu", X: 1, Marks: []));
    }

    internal void SetPcTool(string id, bool allowed)
    {
        if (!chatOpen || !Send(new("pc-permission", id, Key: allowed ? 1 : 0))) return;
        UpdatePcTools(allowed ? [.. PcTools, id] : PcTools.Where(tool => tool != id).ToArray());
    }

    internal void SetAllPcTools(bool allowed)
    {
        if (!chatOpen || !Send(new(allowed ? "pc-enable-all" : "pc-control-stop"))) return;
        UpdatePcTools(allowed ? VrexPcPermissions.Tools.Select(tool => tool.Id).ToArray() : []);
    }

    private void UpdatePcTools(string[] selected)
    {
        PcTools = VrexChatPreferences.KnownTools(selected);
        if (!PcTools.Contains("pc_mark")) ClearMainScreenMarks();
        SaveChatPreferences();
        Notify(new("pc-permissions"));
    }

    public void EnsureWorker()
    {
        if (Running) return;
        CloseFrames(); generation=0; Ready=false; ConfigurationLoaded=false; State="off";
        observerSpeaking = false;
        lastVoicePan = float.NaN;
        string executable = Path.Combine(AppContext.BaseDirectory,"vrex","VRX.VrexHost.exe");
        if (!File.Exists(executable)) throw new FileNotFoundException("Build the Vrex worker before opening Ubuntu.",executable);
        channel = VrexProtocol.ChannelPrefix + Guid.NewGuid().ToString("N");
        var start = new ProcessStartInfo(executable) { UseShellExecute=false,CreateNoWindow=true,
            RedirectStandardInput=true,RedirectStandardOutput=true,RedirectStandardError=true };
        start.ArgumentList.Add("--channel="+channel);
        var worker = Process.Start(start) ?? throw new IOException("Vrex could not start.");
        process=worker;
        stopping=null;
        requests=Channel.CreateBounded<VrexRequest>(new BoundedChannelOptions(256) { SingleReader=true,FullMode=BoundedChannelFullMode.Wait });
        if (chatOpen) requests.Writer.TryWrite(ChatActivation());
        var outgoing=requests;
        _=Task.Run(async () =>
        {
            try { await foreach(var command in outgoing.Reader.ReadAllAsync()) { await worker.StandardInput.WriteLineAsync(JsonSerializer.Serialize(command)); await worker.StandardInput.FlushAsync(); } }
            catch(Exception ex) when(ex is IOException or InvalidOperationException) { Report(new("error","Vrex connection closed."),worker); }
        });
        _=Task.Run(async () =>
        {
            try
            {
                while(await worker.StandardOutput.ReadLineAsync() is { } line)
                    if (DecodeWorkerEvent(line) is { } message) Report(message,worker);
                await worker.WaitForExitAsync(); Report(new("stopped","Vrex Ubuntu stopped."),worker);
            }
            catch(Exception ex) when(ex is IOException or JsonException or InvalidOperationException) { Report(new("error","Vrex: "+ex.Message),worker); }
        });
        _=Task.Run(async () => { try { while(await worker.StandardError.ReadLineAsync() is { } line) Debug.WriteLine("[Vrex] "+line); } catch(IOException) { } });
    }
    internal static VrexEvent? DecodeWorkerEvent(string line)
    {
        if (line.Length > VrexProtocol.MaxGridEventChars ||
            (line.Length >= VrexProtocol.MaxEventChars && !line.StartsWith("{\"Type\":\"grid-view\",", StringComparison.Ordinal)))
            return null;
        var message = JsonSerializer.Deserialize<VrexEvent>(line);
        return message?.Grid is { Image.Length: > VrexProtocol.MaxPcImageBytes } ? null : message;
    }

    private void Report(VrexEvent message,Process? source=null)
    {
        if (message.Type == "guest-audio")
        {
            if (source != null && ReferenceEquals(process, source) && message.Text is { Length: > 0 and <= 25600 })
                guestAudio.Push(message.Text);
            return;
        }
        Application.Current?.Dispatcher.BeginInvoke(() =>
        {
            if(source!=null && !ReferenceEquals(process,source))return;
            if (stopping != null && VmPhase == VrexVmPhase.Stopping && message.Type is not ("stopped" or "settings" or "model-saved" or "model-settings-error")) return;
            if (VmPhase == VrexVmPhase.Stopping && stopping == null &&
                (message.Type == "ready" || (message.VmPhase != null && message.Type != "ubuntu-stopped"))) return;
            if (message.VmPhase != null && Enum.TryParse<VrexVmPhase>(message.VmPhase, true, out var phase))
            {
                VmPhase = phase;
                VmDetail = message.Text ?? "";
            }
            switch(message.Type)
            {
                case "chat-list-start":
                    if (!chatOpen) return;
                    incomingChats.Clear();
                    incomingChatId = message.Text;
                    incomingTranscript = message.X == 1 ? [] : null;
                    return;
                case "chat-list-item":
                    if (chatOpen && message.Chat is { } chat) incomingChats.Add(chat);
                    return;
                case "chat-list-line":
                    if (chatOpen && message.ChatLine is { } line && incomingTranscript != null)
                    {
                        if (message.X == 1 && incomingTranscript.Count > 0)
                        {
                            var previous = incomingTranscript[^1];
                            incomingTranscript[^1] = previous with { Text = previous.Text + line.Text };
                        }
                        else incomingTranscript.Add(line);
                    }
                    return;
                case "chat-list-ready":
                    if (!chatOpen) return;
                    Chats = incomingChats.ToArray();
                    ActiveChatId = incomingChatId;
                    if (incomingTranscript != null)
                    {
                        conversationLog.Clear();
                        conversationLog.AddRange(incomingTranscript);
                        incomingTranscript = null;
                        ChatChanging = false;
                    }
                    break;
                case "chat-list-error":
                    ChatChanging = false;
                    break;
                case "chat-changed":
                    ClearAnnotations();
                    break;
                case "grid-view":
                case "grid-marker":
                    if (!chatOpen || (message.Text == "pc" && !AllowPcScreen)) return;
                    break;
                case "screen-marks":
                    if (!chatOpen || message.Marks is not { } marks) return;
                    if (message.Text == "pc")
                    {
                        if (AllowPcScreen && PcTools.Contains("pc_mark"))
                        {
                            if (message.X == 1 && marks.Length == 0) ClearMainScreenMarks();
                            else
                            {
                                try
                                {
                                    mainScreenMarks ??= new VrexMainScreenMarks();
                                    mainScreenMarks.Update(marks, message.X == 1);
                                }
                                catch (Exception ex) when (ex is IOException or InvalidOperationException or FormatException or ArgumentException or System.ComponentModel.Win32Exception)
                                {
                                    ClearMainScreenMarks();
                                    Status = "Couldn’t show the PC annotation: " + ex.Message;
                                    State = "error";
                                    Notify(new("error", Status));
                                }
                            }
                        }
                        return;
                    }
                    if (message.Text != "ubuntu" || !Ready || !AllowUbuntuMarks) return;
                    break;
                case "settings": Configuration=message; ConfigurationLoaded=true; break;
                case "pc-permissions": return;
                case "pc-permissions-revoked":
                    if (!chatOpen) return;
                    UpdatePcTools([]);
                    return;
                case "speech-active": observerSpeaking = message.X != 0; break;
                case "state":
                    State=message.Text??"idle";
                    if (state == "speaking") { lastVoicePan = float.NaN; AudioDirection = audioDirection; }
                    break;
                case "voice-options": VoiceOptions = message; break;
                case "appearance": Avatar = message.Avatar; ShellRgb = message.ShellRgb; break;
                case "voice-needs-screen": _ = AnswerVoiceScreenAsync(message.X, message.Y == 1); return;
                case "voice-state":
                    LiveVoiceActive = message.VoiceLive;
                    VoiceStatus = message.Text ?? "";
                    break;
                case "voice-partial":
                    if (!chatOpen || (!LiveVoiceActive && message.Text != null)) return;
                    break;
                case "ready":
                    Ready=true; Status=message.Text??"Ubuntu ready";
                    VmPhase=VrexVmPhase.Running; VmDetail=Status; break;
                case "error":
                    Status=message.Text??"Vrex error";
                    if (message.VmPhase == null) State="error";
                    break;
                case "stopped":
                    ChatChanging = false;
                    ClearAnnotations();
                    Ready=false; CloseFrames();
                    guestAudio.Stop();
                    LiveVoiceActive=false; VoiceStatus=""; observerSpeaking=false;
                    if (VmPhase == VrexVmPhase.Stopping)
                    { State="off"; VmPhase=VrexVmPhase.Off; VmDetail="Ubuntu is powered off."; }
                    else if (VmPhase != VrexVmPhase.Failed)
                    { State="error"; VmPhase=VrexVmPhase.Failed; VmDetail="The Ubuntu helper exited. Try starting Ubuntu again."; }
                    Status=VmDetail; break;
                case "ubuntu-stopped":
                    Ready = false; CloseFrames(); guestAudio.Stop();
                    VmPhase = VrexVmPhase.Off; VmDetail = Status = message.Text ?? "Ubuntu is powered off.";
                    break;
                case "status": case "activity": Status=message.Text??""; break;
            }
            if (message.Type == "suggestion" && (!helpful ||
                (message.ObservedSurface is "browser" or "settings" ? !allowPanelScreen : !allowPcScreen))) return;
            if (message.Type is "reply" or "suggestion") RecordChatLine("Vrex", message.Text);
            else if (message.Type == "voice-heard") RecordChatLine("You", message.Text);
            if (message.Type == "task-list") TaskSteps = message.Tasks ?? [];
            Notify(message);
        });
    }
    private void Notify(VrexEvent message) { Revision++; Changed?.Invoke(message); }
    public bool Send(VrexRequest request)
    {
        if(request.Type=="input-cancel" && !Running)return true;
        if (request.PcImage != null && !(request is { Type: "observe-pc", Text: "browser" or "settings" } or
            { Type: "chat" or "voice-context", ScreenSurface: "browser" or "settings" }
            ? allowPanelScreen : allowPcScreen)) return false;
        bool start = request.Type is "start" or "configure-ubuntu-start";
        bool storage = request.Type is "set-vm-path" or "move-vm";
        if ((start || storage) && Running && (VmBusy || Ready))
            return false;
        EnsureWorker();
        if (request.Type is "chat" or "observe-pc") request = request with { SpeakSummary = SpeakReplies };
        if(requests!.Writer.TryWrite(request))
        {
            if (start)
            {
                VmPhase=VrexVmPhase.Preparing;
                VmDetail="Requesting Ubuntu startup…";
                Status=VmDetail;
                Notify(new("vm-status"));
            }
            if (storage)
            {
                VmPhase=VrexVmPhase.Moving;
                VmDetail="Checking the destination folder…";
                Status=VmDetail;
                Notify(new("vm-status"));
            }
            return true;
        }
        // Reserve eventual delivery for releases even when the input queue fills.
        // A lost release would leave the agent waiting for human control forever.
        if(request.Type is "up" or "input-cancel" or "chat-active" or "chat-stop" or "voice-stop" or "ubuntu-stop" or "pc-control-stop" or "pc-sharing" or "pc-permission" or "pc-enable-all" or "bot-transfer-permissions"
            || request is { Type: "clipboard-sync" or "goal-run", Key: 0 })
        {
            var outgoing=requests;
            _ = DeliverAsync(outgoing.Writer.WriteAsync(request));
            return true;
        }
        Status="Vrex is busy processing input. Release the trigger and try again."; Revision++;
        return false;

        static async Task DeliverAsync(ValueTask write)
        {
            try { await write; } catch (ChannelClosedException) { }
        }
    }
    public void Stop() => _ = StopAsync();
    internal void StopUbuntu()
    {
        if (VmPhase == VrexVmPhase.Moving) { Stop(); return; }
        if (!Running || !Send(new("ubuntu-stop"))) return;
        VmPhase = VrexVmPhase.Stopping;
        guestAudio.SetDirection(null);
        VmDetail = Status = "Powering off Ubuntu…";
        Notify(new("vm-status"));
    }
    internal Task StopAsync()
    {
        ClearAnnotations();
        if(stopping != null) return stopping;
        if(!Running) return Task.CompletedTask;
        VmPhase=VrexVmPhase.Stopping;
        guestAudio.SetDirection(null);
        VmDetail="Powering off Ubuntu and closing its display.";
        Status=VmDetail;
        Notify(new("vm-status"));
        requests?.Writer.TryWrite(new("stop")); requests?.Writer.TryComplete();
        var old=process!;
        return stopping=Task.Run(async () =>
        {
            try
            {
                using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(50));
                try { await old.WaitForExitAsync(timeout.Token); }
                catch(OperationCanceledException)
                {
                    if(!old.HasExited) old.Kill(entireProcessTree:true);
                    await old.WaitForExitAsync();
                }
            }
            catch(InvalidOperationException) { }
        });
    }
    public byte[]? ReadFrame()
    {
        if(!Ready || VmPhase == VrexVmPhase.DisplayUnavailable) return null;
        try
        {
            if(memory==null)
            {
                map=MemoryMappedFile.OpenExisting(channel,MemoryMappedFileRights.Read);
                memory=map.CreateViewAccessor(0,VrexProtocol.Capacity,MemoryMappedFileAccess.Read);
                frameLock=Mutex.OpenExisting(channel+".Lock");
            }
            bool locked; try {locked=frameLock!.WaitOne(0);} catch(AbandonedMutexException) {locked=true;}
            if(!locked) return null;
            try
            {
                if(memory.ReadInt32(0)!=VrexProtocol.Magic || memory.ReadInt32(4)!=VrexProtocol.Version) throw new IOException("Vrex display protocol mismatch.");
                int next=memory.ReadInt32(16); if(next==generation) return null;
                int width=memory.ReadInt32(8),height=memory.ReadInt32(12);
                if(width<1 || width>VrexProtocol.MaxWidth || height<1 || height>VrexProtocol.MaxHeight) throw new IOException("Invalid Ubuntu display size.");
                Width=width; Height=height;
                if(frame.Length!=width*height*4) frame=new byte[width*height*4];
                var handle = memory.SafeMemoryMappedViewHandle;
                bool retained = false;
                try
                {
                    handle.DangerousAddRef(ref retained);
                    var pixels = IntPtr.Add(handle.DangerousGetHandle(), checked((int)(memory.PointerOffset + VrexProtocol.HeaderBytes)));
                    System.Runtime.InteropServices.Marshal.Copy(pixels, frame, 0, frame.Length);
                }
                finally { if (retained) handle.DangerousRelease(); }
                generation=next;
                return frame;
            }
            finally {frameLock!.ReleaseMutex();}
        }
        catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or WaitHandleCannotBeOpenedException) { Status=ex.Message; Revision++; return null; }
    }
    private void CloseFrames() {memory?.Dispose();map?.Dispose();frameLock?.Dispose();memory=null;map=null;frameLock=null;}
}
