using PersonaBot;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal sealed partial class VrexSession
{
    private static readonly HttpClient VoiceDownloads = new() { Timeout = TimeSpan.FromHours(1) };
    private readonly SemaphoreSlim voiceModelsGate = new(1, 1);
    private readonly object turnGate = new();
    private readonly object voiceGate = new();
    private VoiceClient? voice;
    private float voicePan, voiceRear;
    private CancellationTokenSource? liveVoice;
    private Task? voiceLoop, voiceInstall;
    private Task<string>? interrupted;
    private bool turnUsesLiveVoice;
    private string[] awaitingChoice = [];
    private sealed record ScreenRequest(int Id, TaskCompletionSource<PcFrame?> Completion);
    private ScreenRequest? pendingScreen;
    private int screenRequestId;
    private long nextMeter;
    private bool Live => liveVoice is { IsCancellationRequested: false };
    private int Avatar => Math.Clamp(settings.VrexAvatar, 0, VrexAppearance.Avatars.Length - 1);
    private string VoiceForAvatar => settings.VrexAvatarVoices?.GetValueOrDefault(Avatar) ??
        (Avatar == 0 ? settings.Voice.Name : null) ?? VrexAppearance.DefaultVoice(Avatar);

    private void ReportVoiceOptions() =>
        emit(new("voice-options", VoiceForAvatar,
            Choices: VoiceCatalog.All.Select(option => $"{option.Id}\t{option.Label}").ToArray(),
            VoiceInstalled: VoiceModels.IsInstalled(VoiceModels.Speech)));

    private void SelectVoice(string? id)
    {
        var choice = VoiceCatalog.All.FirstOrDefault(item => item.Id == id);
        if (choice == null) return;
        settings.VrexAvatarVoices ??= [];
        settings.VrexAvatarVoices[Avatar] = choice.Id;
        settings.Save();
        ReportVoiceOptions();
    }

    private void VoiceStatus(string text, bool active) => emit(new("voice-state", text, VoiceLive: active));

    private void ReportMic(float level)
    {
        long now = Environment.TickCount64;
        if (!Live || now < nextMeter) return;
        nextMeter = now + 160;
        emit(new("voice-meter", X: (int)Math.Round(Math.Clamp(level * 2.5f, 0f, 1f) * 100)));
    }

    private sealed class MicProgress(Action<float> report) : IProgress<float>
    {
        public void Report(float value) => report(value);
    }

    private VoiceClient GetVoice()
    {
        lock (voiceGate)
        {
            if (voice != null) return voice;
            voice = VoiceClient.TryCreate() ?? throw new IOException("The local PersonaBot voice helper is missing.");
            voice.SetDirection(voicePan, voiceRear);
            voice.PartialWords += words =>
            {
                if (Live && chatOpen) emit(new("voice-partial", words));
            };
            voice.TalkedOver += words =>
            {
                if (!Live) return;
                words.Level += ReportMic;
                Interlocked.Exchange(ref interrupted, words.Words);
                VoiceStatus("Listening to your interruption…", true);
            };
            return voice;
        }
    }

    private void SetVoiceDirection(float pan, float rear)
    {
        lock (voiceGate)
        {
            voicePan = pan;
            voiceRear = rear;
            voice?.SetDirection(pan, rear);
        }
    }

    private void BeginVoiceInstall()
    {
        if (voiceInstall is { IsCompleted:false } || Live) return;
        voiceInstall = Task.Run(async () =>
        {
            try
            {
                await EnsureVoiceModelsAsync(live:false, shutdown.Token);
                VoiceStatus("Local voices ready.", Live);
            }
            catch (OperationCanceledException) when (shutdown.IsCancellationRequested) { }
            catch (Exception ex) { VoiceStatus("Couldn’t install local voices. " + ex.Message, Live); }
        });
    }

    private async Task EnsureVoiceModelsAsync(bool live, CancellationToken token)
    {
        await voiceModelsGate.WaitAsync(token);
        try
        {
            VoiceModel[] wanted = live ? [VoiceModels.Speech, VoiceModels.Hearing, VoiceModels.Detector] : [VoiceModels.Speech];
            bool installed = false;
            foreach (var model in wanted)
            {
                if (VoiceModels.IsInstalled(model)) continue;
                await VoiceModels.InstallAsync(model, VoiceDownloads,
                    new VoiceProgress(update => VoiceStatus(update.Status, Live)), token);
                installed = true;
            }
            if (installed && voice != null) await voice.ReloadAsync();
            ReportVoiceOptions();
        }
        finally { voiceModelsGate.Release(); }
    }

    private sealed class VoiceProgress(Action<(double Fraction, string Status)> report) : IProgress<(double Fraction, string Status)>
    {
        public void Report((double Fraction, string Status) value) => report(value);
    }

    private Task BeginTurn(string words, bool speak, PcFrame? image, bool fromMicrophone, bool automatedGoal = false, bool resumedTask = false)
    {
        lock (turnGate)
        {
            if (!chatOpen || changingChat) throw new InvalidOperationException("Wait for Vrex chat to open before starting a conversation.");
            if (fromMicrophone && !Live) throw new OperationCanceledException();
            if (!automatedGoal && goalWork is { IsCompleted: false } && goalRun is { IsCancellationRequested: false })
                throw new InvalidOperationException("Turn off Achieve goal before starting a separate chat turn.");
            if (string.IsNullOrWhiteSpace(settings.Model)) throw new InvalidOperationException("Choose a vision-capable model in Vrex settings first.");
            if (guestStop is { IsCompleted:false }) throw new InvalidOperationException("Ubuntu is still powering off.");
            if (conversation is { IsCompleted:false }) throw new InvalidOperationException("Vrex is still working. Stop the task first.");
            if (chat == null || !ReferenceEquals(chatComputer, computer)) ResetChat();
            observationCancellation?.Cancel();
            Volatile.Write(ref awaitingChoice, []);
            turn?.Dispose();
            turn = CancellationTokenSource.CreateLinkedTokenSource(shutdown.Token);
            var token = turn.Token;
            CancellationToken? liveToken = liveVoice?.Token;
            bool live = liveToken is { IsCancellationRequested: false };
            bool voiceRequested = !fromMicrophone && speak;
            if (!fromMicrophone && live) voice?.CancelListening();
            string userRequest = words.Split("\nThe suggestion I selected was:", 2, StringSplitOptions.None)[0];
            pcAnnotationRequested = VrexPcPrompt.RequestsAnnotation(userRequest);
            pointRequested = VrexPcPrompt.RequestsPointing(userRequest);
            zoomRequested = VrexPcPrompt.RequestsZoomInspection(userRequest);
            markTarget = image?.Surface is "settings" or "browser" ? "panel" :
                VrexPcPrompt.TargetsUbuntu(userRequest) ? "ubuntu" :
                image != null || userRequest.Contains("PC screen", StringComparison.OrdinalIgnoreCase) ? "pc" : null;
            long now = Environment.TickCount64;
            retryingAnnotation = false;
            if (pcAnnotationRequested)
            {
                annotationToRetry = markTarget != "panel" ? (markTarget, zoomRequested, now) : null;
            }
            else if (annotationToRetry is { } recent && now - recent.Started <= 120_000 &&
                (markTarget == null || markTarget == recent.Target))
            {
                if (VrexPcPrompt.IsAnnotationRetry(userRequest))
                {
                    retryingAnnotation = pcAnnotationRequested = true;
                    zoomRequested |= recent.Zoom;
                    markTarget = recent.Target;
                }
                else
                    annotationToRetry = VrexPcPrompt.IsZoomFeedback(userRequest) ? (recent.Target, true, recent.Started) : null;
            }
            else annotationToRetry = null;
            performingGoalTurn = automatedGoal;
            turnWorkPending = true;
            turnSpeaks = speak;
            turnUsesLiveVoice = fromMicrophone;
            goalTurnFailed = false;
            pcAdviceOnly = !automatedGoal && (pcAnnotationRequested || pointRequested || zoomRequested || VrexPcPrompt.IsGameGuidance(words));
            return conversation = Task.Run(() => RunTurnAsync(words, voiceRequested,
                live ? liveToken : null, token, image, automatedGoal, resumedTask));
        }
    }

    private void AcceptChoice(string? value, bool speak)
    {
        if (value == null || !Volatile.Read(ref awaitingChoice).Contains(value, StringComparer.Ordinal)) return;
        Volatile.Write(ref awaitingChoice, []);
        turn?.Cancel();
        Task? finishing = conversation;
        string selectedChat = ChatId;
        _ = Task.Run(async () =>
        {
            if (finishing != null) await finishing;
            if (shutdown.IsCancellationRequested) return;
            try
            {
                Task reply;
                lock (turnGate)
                {
                    if (changingChat || ChatId != selectedChat) return;
                    reply = BeginTurn(value, speak, null, fromMicrophone:false);
                }
                await reply;
            }
            catch (InvalidOperationException ex) { emit(new("error", ex.Message)); }
        });
    }

    private async Task StartForegroundVoiceAsync()
    {
        if (Live) return;
        if (goalWork is { IsCompleted: false } running)
        {
            HoldForegroundWork(ChatId);
            StopGoalRun();
            await running;
        }
        if (voiceLoop is { IsCompleted: false } ending) await ending;
        if (chatOpen) BeginLiveVoice();
    }

    private async Task StopForegroundVoiceAsync()
    {
        StopLiveVoice(cancelVoiceTurn: true);
        if (voiceLoop != null) await voiceLoop;
        if (!heldChatWork.ContainsKey(ChatId)) return;
        if (turnUsesLiveVoice && conversation is { IsCompleted: false } ending) await ending;
        if (conversation is { IsCompleted: false }) return;
        ResumeForegroundWork();
    }

    private void BeginLiveVoice()
    {
        if (voiceLoop is { IsCompleted:false }) return;
        if (string.IsNullOrWhiteSpace(settings.Model))
        {
            VoiceStatus("Choose a model in Vrex chat Settings before starting live voice.", false);
            return;
        }
        liveVoice?.Dispose();
        observationCancellation?.Cancel();
        liveVoice = CancellationTokenSource.CreateLinkedTokenSource(shutdown.Token);
        var token = liveVoice.Token;
        VoiceStatus("Starting live voice…", true);
        voiceLoop = Task.Run(() => LiveVoiceAsync(token));
    }

    private void StopLiveVoice(bool cancelVoiceTurn = false)
    {
        liveVoice?.Cancel();
        if (cancelVoiceTurn)
        {
            lock (turnGate)
                if (turnUsesLiveVoice) turn?.Cancel();
        }
        voice?.CancelListening();
        voice?.StopSpeaking();
    }

    private async Task LiveVoiceAsync(CancellationToken token)
    {
        string end = "Live voice off.";
        try
        {
            await EnsureVoiceModelsAsync(live:true, token);
            var local = GetVoice();
            var meter = new MicProgress(ReportMic);
            while (!token.IsCancellationRequested)
            {
                Task? pending;
                lock (turnGate) pending = conversation;
                if (pending is { IsCompleted:false }) await pending.WaitAsync(token);
                Task<string>? speech = Interlocked.Exchange(ref interrupted, null);
                VoiceStatus(speech == null ? "Listening… speak to Vrex." : "Listening to your interruption…", true);
                string words;
                try
                {
                    words = speech != null ? await speech.WaitAsync(token) : await local.ListenAsync(meter, token, continuous: true, partial: true);
                }
                catch (OperationCanceledException) when (!token.IsCancellationRequested) { continue; }
                finally { emit(new("voice-partial")); }
                if (string.IsNullOrWhiteSpace(words) || SpokenWords.IsJustStop(words)) continue;
                if (token.IsCancellationRequested) break;
                if (!local.CanBargeIn) { end = "This microphone is unavailable for barge-in."; break; }
                emit(new("voice-heard", words));
                VoiceStatus("Preparing a reply…", true);
                PcFrame? screen = null;
                bool guide = VrexPcPrompt.IsSpokenGuide(words);
                if (guide || VrexPcPrompt.RequestsPcScreen(words))
                {
                    screen = await RequestVoiceScreenAsync(guide, token);
                    if (guide) words = VrexPcPrompt.Guide(words, hasScreen: screen != null);
                    else if (screen == null) words += VrexPcPrompt.MissingContext;
                }
                Task reply;
                try { reply = BeginTurn(words, true, screen, fromMicrophone:true); }
                catch (InvalidOperationException) when (conversation is { IsCompleted:false }) { continue; }
                await reply.WaitAsync(token);
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception ex) { end = "Live voice stopped. " + ex.Message; }
        finally
        {
            _ = Interlocked.Exchange(ref interrupted, null);
            if (liveVoice is { } current && current.Token == token) current.Cancel();
            VoiceStatus(end, false);
        }
    }

    private async Task<PcFrame?> RequestVoiceScreenAsync(bool guide, CancellationToken token)
    {
        var request = new ScreenRequest(++screenRequestId, new(TaskCreationOptions.RunContinuationsAsynchronously));
        Volatile.Write(ref pendingScreen, request);
        try
        {
            emit(new("voice-needs-screen", X: request.Id, Y: guide ? 1 : 0));
            return await request.Completion.Task.WaitAsync(TimeSpan.FromSeconds(3), token);
        }
        catch (TimeoutException) { return null; }
        finally { Volatile.Write(ref pendingScreen, null); }
    }

    private void AcceptVoiceScreen(VrexRequest request)
    {
        var pending = Volatile.Read(ref pendingScreen);
        if (Live && pending?.Id == request.Key)
            pending.Completion.TrySetResult(DecodePcImage(request.PcImage, request.X, request.Y, request.ScreenSurface));
    }

    private async Task SpeakRecapAsync(string text, CancellationToken token, bool announceState = true)
    {
        bool live = Live;
        if (!VoiceModels.IsInstalled(VoiceModels.Speech))
        {
            if (!token.IsCancellationRequested)
                VoiceStatus("Install local voices in Room settings to hear Vrex’s replies.", live);
            return;
        }
        try
        {
            var local = GetVoice();
            using var cancel = live && liveVoice != null
                ? CancellationTokenSource.CreateLinkedTokenSource(token, liveVoice.Token) : null;
            if (live) VoiceStatus("Speaking… you can interrupt.", true);
            if (announceState) emit(new("state", "speaking"));
            else emit(new("speech-active", X: 1));
            try
            {
                await local.SpeakAsync(text, VoiceCatalog.Find(VoiceForAvatar),
                    settings.Voice.Speed, cancel?.Token ?? token, listen:live, partial:live);
            }
            finally { if (!announceState) emit(new("speech-active")); }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested || (live && !Live)) { }
        catch (Exception ex)
        {
            if (!token.IsCancellationRequested)
                VoiceStatus("Local playback failed. " + ex.Message, live && Live);
        }
    }
}
