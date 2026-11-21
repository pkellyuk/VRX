using PersonaBot;
using SkiaSharp;
using Automaton.AI.Interfaces;
using Automaton.Core.Services.ComputerUse;
using Automaton.Core.Interfaces;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal static partial class VrexSelfTest
{
    private sealed class Guest : IGuestScreen, IGuestShell, IGuestInput
    {
        public ScreenRect ScreenBounds => new(0,0,1280,800);
        public readonly List<string> Actions = [];
        public IGuestInput CreateInput() => this;
        public Task<byte[]> CaptureAsync(CancellationToken token)
        {
            using var image = new SKBitmap(1280,800); image.Erase(SKColors.SteelBlue);
            using var png = image.Encode(SKEncodedImageFormat.Png,90);
            return Task.FromResult(png.ToArray());
        }
        public Task<string> ExecuteAsync(string command,CancellationToken token = default)
        {
            Actions.Add(command);
            return Task.FromResult(command.Contains(SmokeFixtureProvider.ShellMarker) ? SmokeFixtureProvider.ShellMarker + "\nExit code: 0" : "\nExit code: 0");
        }
        public Task SynchronizeAsync(CancellationToken token) => Task.CompletedTask;
        public void MoveTo(ScreenPoint point) => Actions.Add($"move:{point.X},{point.Y}");
        public void Button(MouseButton button,bool down) => Actions.Add($"button:{button},{down}");
        public void Wheel(bool positive,bool horizontal) => Actions.Add($"wheel:{positive}");
        public void PressKeys(string keys) => Actions.Add("keys:" + keys);
        public void TypeText(string text,Func<bool>? stop) => Actions.Add("text:" + text);
    }
    internal static async Task<int> RunAsync()
    {
        try
        {
            await VrexInputQueueTests.RunAsync();
            await VerifyStorageMoveAsync();
            await VerifyChatMemoryAsync();
            await VerifyGoalsAsync();
            await VerifyCalendarAsync();
            await VerifyAppearanceAsync();
            await VerifyPcPermissionsAsync();
            await VerifyMarkReferenceAsync();
            VerifyGridPreview();
            await VerifyChatActivationAsync();
            await VerifyIndependentConfigurationAsync();
            await VerifyForegroundChatAsync();
            Check(new[] { "Help me", "Guide me.", "What should I do next?", "Show me", "Look at the screen.",
                "Hey Vrex, could you look at the screen please?" }.All(VrexPcPrompt.IsSpokenGuide) &&
                new[] { "Help me write a note", "Show me the graph", "Don't look at the screen" }.All(text => !VrexPcPrompt.IsSpokenGuide(text)) &&
                VrexPcPrompt.IsGameGuidance(VrexPcPrompt.Guide("Look at the screen.")),
                "Spoken Guide Me commands trigger the advice prompt without matching ordinary conversation or pointing requests");
            Check(new[] { "What is this game?", "Help me with this game", "What do you see?", "Look at my main screen",
                "I found it. Can you take a look at my screen?", "Are you sure you can't see my screen? Can you check again?" }
                .All(VrexPcPrompt.RequestsPcScreen) && !VrexPcPrompt.RequestsPcScreen("Open Ubuntu Settings."),
                "PC screen intent must include the spoken game question and exclude ordinary Ubuntu requests.");
            Check(VrexPcPrompt.RequestsPcScreen("Go and research my game that I'm playing right now.") &&
                VrexPcPrompt.RequestsAnnotation("Annotate the screen where I should move.") &&
                !VrexPcPrompt.RequestsPcScreen("Annotate the Ubuntu screen."),
                "Game research and Windows marks request the shared PC image, while Ubuntu marks use the guest.");
            Check(new[] { "How do I beat this boss?", "Look up this quest for me.", "How can I finish this puzzle?",
                "Help me with the current mission.", "How should I get past this checkpoint?" }
                .All(prompt => VrexPcPrompt.IsGameGuidance(prompt) && VrexPcPrompt.RequestsPcScreen(prompt)) &&
                !VrexPcPrompt.RequestsPcScreen("Look up the Ubuntu audio settings."),
                "Guidance applies across game genres and ordinary Ubuntu requests stay in the guest.");
            var recap = VrexSpokenSummary.Split("I opened Ubuntu Settings.\n\nDetails: `adb shell`\n[VREX_SPOKEN_RECAP] I opened Settings for you.");
            Check(recap == ("I opened Ubuntu Settings.\n\nDetails: `adb shell`", "I opened Settings for you."),
                "A spoken recap must keep its formatting marker and technical text out of speech without changing the written reply.");
            Check(VrexSpokenSummary.Excerpt("**Open Settings first.**\n```sh\nsecret command\n```\nThen choose Display. [Instructions](https://example.test/guide). Fourth sentence.")
                == "Open Settings first. Then choose Display. Instructions.", "The spoken fallback must preserve useful prose, dropping code and markup.");
            var question = VrexSpokenSummary.SplitChoices("Which layout do you prefer?\n[VREX_CHOICES] Wide | Compact\n[VREX_SPOKEN_RECAP] Which layout?");
            Check(question.Choices.SequenceEqual(new[] { "Wide", "Compact" }) && question.Written.Contains("SPOKEN_RECAP") && !question.Written.Contains("WIDE |"),
                "Question choices and speech recap must be processed independently.");
            Check(VrexProtocol.IsChannel(VrexProtocol.ChannelPrefix + Guid.NewGuid().ToString("N")),"Private channel validation");
            Check(!VrexProtocol.IsChannel("Local\\arbitrary"),"Unknown channel must fail");
            byte[] bgra=VrexSession.OpaqueBgra(new byte[]{10,20,30,0,40,50,60,0},2,1);
            Check(bgra.SequenceEqual(new byte[]{10,20,30,255,40,50,60,255}),"RFB unused bytes must become opaque alpha without changing colours");
            var control=new RunControl();
            var releaseAction=new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            var longAction=control.ActAsync(()=>releaseAction.Task,CancellationToken.None);
            using(var pausing=new VrexPause())
            {
                int paused=0;Exception? pauseError=null;
                pausing.Request(control,()=>paused++,ex=>pauseError=ex,CancellationToken.None);
                Check(control.IsPaused && !pausing.Completion.IsCompleted,"Pause must return to the command reader while an action drains");
                var waiting=pausing.Completion;pausing.Resume();
                await waiting.WaitAsync(TimeSpan.FromSeconds(2));
                Check(!control.IsPaused && paused==0 && pauseError==null,"Resume must cancel pending pause without a late paused notification");
                releaseAction.SetResult(true);await longAction;
                pausing.Request(control,()=>paused++,ex=>pauseError=ex,CancellationToken.None);
                await pausing.Completion.WaitAsync(TimeSpan.FromSeconds(2));
                Check(control.IsPaused && paused==1 && pauseError==null,"A completed pause must report the actual paused state");
            }
            Check(!control.IsPaused,"Disposing the pause controller must release the agent");
            var guest = new Guest(); var computer = new GuestComputer(guest, "Ubuntu desktop");
            using var tools = new GuestTools(computer,guest);
            var begin = await tools.ExecuteAsync("cu_begin","{\"reason\":\"Vrex guest-only smoke check\"}",CancellationToken.None);
            Check(begin.Output?.Contains("Ubuntu desktop") == true &&
                tools.Schemas["cu_begin"].Description!.Contains("Ubuntu desktop") &&
                computer.GetMonitors().Single().DeviceName == "Ubuntu desktop",
                "The guest tools and display label must identify the Ubuntu desktop.");
            Check(!tools.Ids.Contains("powershell") && !tools.Ids.Contains("remote"),"Host tools must not be registered");
            var profile = GuestProfile.ForCloudMachine("Ubuntu desktop", "x86-64", "VRX", false);
            var bot = VrexSession.Vrex;
            Check(bot.Id == "vrex" && profile.AccountStyle == GuestAccountStyle.Debian,"One Vrex Ubuntu persona");
            var events = new List<(string Role,string Text)>();
            var fullDisplay = await guest.CaptureAsync(CancellationToken.None);
            const string focus = "The pointer was at (1240, 710). Consider the full display.";
            using (var standalone = new TestingChat(new Settings { Model = "offline-fixture" }, null,
                new SmokeFixtureProvider(fullDisplay, focus, withoutTools: true), persona: bot,
                systemPrompt: "Vrex can discuss the supplied display while Ubuntu is powered off."))
            {
                bool answered = false;
                await standalone.RunAsync("What is on the screen?", (role, text) =>
                    answered |= role == "PersonaBot" && text.Contains("while the guest was off"),
                    CancellationToken.None, transientScreen: fullDisplay, transientScreenFocus: focus);
                Check(answered, "Vrex must answer questions with Ubuntu powered off.");
            }
            using var chat = new TestingChat(new Settings {Model="offline-fixture",SuggestReplies=false}, tools,
                new SmokeFixtureProvider(fullDisplay, focus, extraTool: "goal"),profile,bot,[bot])
            {
                AdditionalToolSchemas = () => new() { ["goal"] = new JsonSchema() },
                ExecuteAdditionalTool = (name, _, _) => Task.FromResult(new ToolResult
                    { Output = name == "goal" ? "VREX_EXTRA_TOOL_OK" : "Wrong tool" })
            };
            await chat.RunAsync("Inspect your Ubuntu screen and verify the guest tools.",(role,text)=>events.Add((role,text)),
                CancellationToken.None, transientScreen: fullDisplay, transientScreenFocus: focus);
            Check(events.Any(e=>e.Text.Contains("Offline provider fixture passed")),"Actual PersonaBot model/tool/image loop failed");
            computer.Click(new ScreenPoint(42,60)); computer.TypeText("Vrex test");
            Check(guest.Actions.Contains("move:42,60") && guest.Actions.Contains("text:Vrex test"),"Input must reach only the supplied guest");
            var state = new SessionState(); chat.Capture(state);
            string path = Path.Combine(Settings.Root,"conversation-test.json"); SessionStore.Save(state,path);
            Check(SessionStore.Load(path)!.History.Count == state.History.Count,"Conversation persistence failed");
            var oldNote = TestingChat.CreateInstruction("The operator restarted the Linux desktop from disk. Saved chat and files remain, but open applications and RAM state were not restored. Call cu_begin and inspect a fresh cu_grid before acting. Continue the user's existing scope when asked.");
            state.History.Add(oldNote);
            state.PendingInstructions.Add(oldNote);
            chat.Restore(state, reopenNote: profile.ReopenNote);
            var restoredChat = new SessionState();
            chat.Capture(restoredChat);
            Check(restoredChat.History.Count == state.History.Count - 1 &&
                restoredChat.PendingInstructions.Any(note => note.Content == profile.ReopenNote) &&
                restoredChat.PendingInstructions.All(note => note.Content != oldNote.Content),
                "Restoring Ubuntu chat must replace the old Linux restart cue without losing the conversation.");
            var machine = new Machine {Id=VrexProtocol.MachineId,Name="Vrex Ubuntu",Distribution=VrexProtocol.Distribution,
                Architecture=VrexProtocol.Architecture,Folder=Settings.Root};
            var args = CloudRuntime.BuildArguments(machine, 2, 4096, "", 6001, 6002, QemuAcceleration.Whpx,
                sharedGpu: true, framebufferPort: 6003);
            Check(args.Contains("q35") && args.Contains("whpx") &&
                args.Any(a => a.StartsWith(QemuGraphics.Device)) && !args.Contains("-fsdev") && !args.Contains("-virtfs"),
                "Ubuntu starts with WHPX and shared GPU without host folders.");
            foreach (var display in VrexResolution.Options)
            {
                machine.ScreenWidth = display.Width;
                machine.ScreenHeight = display.Height;
                var displayArgs = CloudRuntime.BuildArguments(machine, 2, 4096, "", 6001, 6002,
                    QemuAcceleration.Whpx, sharedGpu: true, framebufferPort: 6003);
                Check(displayArgs.Any(a => a.Contains($"xres={display.Width},yres={display.Height}")), "Ubuntu resolution must match the renderer.");
            }
            bool rejected = false; try { QemuAcceleration.Arguments("tcg","aarch64"); } catch(NotSupportedException) { rejected=true; }
            Check(rejected,"The Vrex platform must reject ARM guests");
            using var lease = MachineLease.Acquire(Path.Combine(Settings.Root,"lease"));
            bool secondRejected=false; try { using var second=MachineLease.Acquire(Path.Combine(Settings.Root,"lease")); } catch(IOException) { secondRejected=true; }
            Check(secondRejected,"A second owner of the Ubuntu VM must be refused");
            Console.WriteLine("PASS: real PersonaBot tool loop, opaque screen feedback, cancellable/coalesced guest input, responsive pause/resume, one Vrex identity, persistent conversation, exclusive VM lease and x86-64 runtime policy.");
            return 0;
        }
        catch(Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static async Task VerifyPcPermissionsAsync()
    {
        using var desktop = new VrexDesktopTools();
        Check(desktop.Enabled.Length == 0 && desktop.Schemas().Count == 0, "Windows tools start disabled");
        desktop.EnableAll();
        Check(desktop.Enabled.Length == VrexPcPermissions.Tools.Length && desktop.Schemas().Keys.All(VrexPcPermissions.IsWorkspaceTool),
            "Enable all selects every tool, while only workspace tools work without screen sharing");
        await desktop.ExecuteAsync("pc_write_file", "{\"name\":\"notes.txt\",\"text\":\"Vrex test\"}", default);
        var notes = await desktop.ExecuteAsync("pc_read_file", "{\"name\":\"notes.txt\"}", default);
        Check(notes.Output == "Vrex test", "Vrex can read and write a scoped workspace file");
        var files = await desktop.ExecuteAsync("pc_list_files", "{}", default);
        Check(files.Output == "notes.txt", "Vrex can list its workspace files");
        bool traversalRejected = false, shellRejected = false;
        try { await desktop.ExecuteAsync("pc_read_file", "{\"name\":\"../settings.json\"}", default); }
        catch (ArgumentException) { traversalRejected = true; }
        try { await desktop.ExecuteAsync("pc_open_app", "{\"app\":\"powershell\"}", default); }
        catch (ArgumentException) { shellRejected = true; }
        Check(traversalRejected && shellRejected, "The workspace is isolated and app launch uses its allowlist");
        desktop.SetSharing(true);
        Check(desktop.Schemas().Count == VrexPcPermissions.Tools.Length + 1 &&
            desktop.Schemas().ContainsKey(VrexDesktopTools.InspectTool), "Every Windows permission maps to a model tool, alongside read-only inspection");
        desktop.SetPermission("pc_begin", false);
        Check(desktop.Schemas().ContainsKey("pc_mark") && !desktop.Schemas().ContainsKey("pc_click"),
            "Windows annotation does not grant input control");
        desktop.SetPermission("pc_begin", true);
        if (new Automaton.Core.Services.ComputerUse.DesktopCaptureService().GetMonitors().Any(monitor => monitor.IsPrimary))
        {
            bool drawn = false;
            desktop.MarksChanged += (marks, replace, _) =>
                drawn |= replace && marks.Count == 1 && marks[0].Kind == ScreenMarkKind.Arrow;
            const string mark = """{"marks":[{"kind":"arrow","at":"0.1,0.2","to":"0.5,0.5"}]}""";
            await desktop.ExecuteAsync("pc_mark", """{"inspect":true}""", default);
            await desktop.ExecuteAsync("pc_mark", mark, default);
            Check(drawn, "Windows annotation emits a drawing");
            desktop.SetPermission("pc_mark", false);
            var rejected = await desktop.ExecuteAsync("pc_mark", mark, default);
            Check(rejected.Title == "PC permission required", "revoked Windows annotation is blocked at execution");
            desktop.SetPermission("pc_begin", false);
            drawn = false;
            var readOnly = await desktop.ExecuteAsync(VrexDesktopTools.InspectTool, mark, default);
            Check(readOnly.Attachments.Count == 1 && !drawn, "Sharing permits read-only inspection even if marks and input are off");
            var zoom = await desktop.ExecuteAsync(VrexDesktopTools.InspectTool, """{"zoom_from":"B2","zoom_to":"C3","inspect":false}""", default);
            Check(zoom.Attachments.Count == 1 && desktop.HasZoomedMarkReference && !drawn, "The read-only tool zooms the current reference without drawing");
            desktop.SetSharing(false);
            var privateScreen = await desktop.ExecuteAsync(VrexDesktopTools.InspectTool, "{}", default);
            Check(privateScreen.Attachments.Count == 0 && !desktop.Schemas().ContainsKey(VrexDesktopTools.InspectTool),
                "Turning off sharing revokes the inspection tool and its output");
            desktop.SetSharing(true);
            desktop.SetPermission("pc_begin", true);
            var begin = await desktop.ExecuteAsync("pc_begin", "{\"reason\":\"Verify primary display scope\"}", default);
            Check(begin.Metadata.TryGetValue("monitors", out object? displays) && displays is 1,
                "Windows tool session sees exactly the primary display");
            await desktop.ExecuteAsync("pc_end", "{}", default);
        }
        desktop.SetPermission("pc_click", false);
        Check(VrexPcPrompt.IsAnnotationRetry("And you try one more time.") &&
            VrexPcPrompt.IsZoomFeedback("It's not. You didn't use the ZoomGrid.") &&
            !VrexPcPrompt.IsAnnotationRetry("Try again on Ubuntu") &&
            !VrexPcPrompt.IsAnnotationRetry("I don't want you to try again."),
            "Only clear retries and zoom corrections carry annotation context");
        var blocked = await desktop.ExecuteAsync("pc_click", "{}", default);
        Check(!desktop.Schemas().ContainsKey("pc_click") && blocked.Title == "PC permission required",
            "Revoked Windows tool calls are rejected at execution");
        desktop.RevokeAll();
        Check(desktop.Schemas().Keys.SequenceEqual([VrexDesktopTools.InspectTool]) && desktop.Enabled.Length == 0,
            "Stop PC control revokes every individual tool permission while the screen remains shared");
        Console.WriteLine("PASS: Windows tools are off by default and independently gated at schema and execution time.");
    }

    private static async Task VerifyMarkReferenceAsync()
    {
        var bounds = new ScreenRect(-500, 40, 1200, 900);
        IReadOnlyList<ScreenMark>? placed = null;
        using var image = new SKBitmap(bounds.Width, bounds.Height);
        image.Erase(SKColors.SteelBlue);
        var mark = new MarkTool(() => bounds, (marks, _) => placed = marks, capture: () => image.Copy());
        var context = new ToolContext();
        const string box = """{"marks":[{"kind":"box","at":"A1","to":"1,1"}]}""";
        Check((await mark.ExecuteAsync(box, context)).Title == "Inspect before marking" && placed == null,
            "Annotation requires an image of the current display");
        var firstView = await mark.ExecuteAsync("""{"inspect":true,"zoom_from":"B2","zoom_to":"C3"}""", context);
        Check(firstView.Attachments.Count == 1 && firstView.Output.Contains("fresh full-screen grid"),
            "Zoom using stale labels recovers with the full labelled screen");
        var full = new GridOverlayRenderer().Render(image, bounds);
        var cropped = await mark.ExecuteAsync("""{"inspect":true,"zoom_from":"B2","zoom_to":"C3"}""", context);
        Check(cropped.Output.Contains("only a magnified crop") &&
            MarkTool.IsInspection("""{"inspect":true}""") && !MarkTool.IsInspection(box),
            "Zoom describes the crop and distinguishes inspection from placing marks");
        await mark.ExecuteAsync(box, context);
        var first = full.Resolve("B2")!.Point;
        var last = full.Resolve("C3")!.Point;
        Check(placed is [var shape] && shape.Points[0] == (first.X - bounds.X, first.Y - bounds.Y) &&
            shape.Points[1] == (last.X - bounds.X, last.Y - bounds.Y),
            "Zoomed grid labels and image fractions map back to the correct screen pixels");
        mark.ResetReference();
        Check((await mark.ExecuteAsync(box, context)).Title == "Inspect before marking",
            "A prior turn cannot reuse an old grid");
        Check(VrexPcPrompt.RequestsPointing("Can you point to the headline?") &&
              VrexPcPrompt.RequestsPointing("Guide me with this request: Show me the units graph") &&
              !VrexPcPrompt.RequestsAnnotation("Can you point to the headline?"),
            "A pointing request is distinct from a persistent annotation");
        Check(VrexPcPrompt.RequestsPcScreen("Can you try again and zoom into his face?") &&
              !VrexPcPrompt.RequestsPcScreen("Zoom in on the Ubuntu screen."),
            "A zoom follow-up requests the PC image while an explicit Ubuntu zoom stays in the guest");
    }

    private static void VerifyGridPreview()
    {
        var messages = new List<VrexEvent>();
        var preview = new VrexGridPreview(messages.Add);
        byte[] encoded = [4, 5, 6];
        preview.Show("pc", new GridView { Region = new(-100, 50, 201, 101), ImageBytes = encoded }, "grid");
        preview.Indicate("ubuntu", 0, 50, clicked: true);
        preview.Indicate("pc", 0, 75, clicked: true);
        Check(messages.Count == 0, "The grid feed only sends images while its viewer is open");
        preview.SetOpen(true);
        Check(messages is [{ Type: "grid-view", Text: "pc", Grid: not null },
            { Type: "grid-marker", X: 1, Point: { X: .5, Y: .25 } }] && ReferenceEquals(messages[0].Grid!.Image, encoded),
            "The latest grid and click use the exact model image and its zoomed region");
        preview.Indicate("pc", 300, 75, clicked: false);
        Check(messages[^1] is { Type: "grid-marker", Point: null }, "A target outside the crop must not be drawn on a different location");
        preview.Clear("pc");
        Check(messages[^1] is { Type: "grid-view", Grid: null }, "Revoking access clears the preview");
    }

    private static async Task VerifyChatActivationAsync()
    {
        var events = new List<VrexEvent>();
        string channel = VrexProtocol.ChannelPrefix + Guid.NewGuid().ToString("N");
        await using var session = new VrexSession(channel, Settings.Root, events.Add);
        await session.HandleAsync(new("pc-enable-all"));
        await session.HandleAsync(new("voice-start"));
        Check(events.Count == 0, "Closed chat must reject permission changes and live voice");
        await session.HandleAsync(new("chat-active", X: 1, Key: 1, PcTools: ["pc_screenshot"]));
        await session.HandleAsync(new("pc-permission", "pc_read_file", Key: 1));
        var tools = events.Last(item => item.Type == "pc-permissions").Choices!;
        Check(tools.Length == 2 && tools.Contains("pc_screenshot") && tools.Contains("pc_read_file"),
            "Opening chat applies the saved tool permissions");
        Check(!session.ExtraToolSchemas().ContainsKey("clipboard_text") && !session.ExtraToolSchemas().ContainsKey("copy_selected_files"),
            "Bot clipboard and file transfer are opt-in");
        await session.HandleAsync(new("bot-transfer-permissions", AllowBotClipboard: true, AllowBotFileCopy: true));
        Check(session.ExtraToolSchemas().ContainsKey("clipboard_text") && !session.ExtraToolSchemas().ContainsKey("copy_selected_files"),
            "An enabled clipboard is available while file transfer still requires a live Ubuntu guest");
        await session.HandleAsync(new("bot-transfer-permissions", AllowBotFileCopy: true));
        var denied = await session.ExecuteExtraToolAsync("clipboard_text", """{"machine":"windows","action":"read"}""", default);
        Check(!session.ExtraToolSchemas().ContainsKey("clipboard_text") && denied.Output.StartsWith("The user has not enabled"),
            "Revoking permission blocks a stale clipboard call");
        await session.HandleAsync(new("chat-active"));
        events.Clear();
        await session.HandleAsync(new("pc-enable-all"));
        await session.HandleAsync(new("chat", "This should be ignored while closed."));
        Check(events.Count == 0, "Closing chat disables new turns and tool changes");
        Console.WriteLine("PASS: Vrex chat activation gates voice, turns and tool permissions.");
    }

    private static async Task VerifyGoalsAsync()
    {
        var events = new List<VrexEvent>();
        var channel = VrexProtocol.ChannelPrefix + Guid.NewGuid().ToString("N");
        await using var session = new VrexSession(channel, Settings.Root, events.Add);
        Check(!session.ExtraToolSchemas().ContainsKey("goal"), "Closed chat does not offer goals");
        await session.HandleAsync(new("chat-active", Key: 1));
        var chatId = events.Last(item => item.Type == "chat-list-start").Text!;
        var schemas = session.ExtraToolSchemas();
        Check(schemas.ContainsKey("goal") && schemas.ContainsKey("task_list"), "Goal and checklist tools are both visible");
        await session.ExecuteExtraToolAsync("goal", """{"action":"create","title":"Preview round trip","description":"Verify a test checklist","steps":["Inspect the page"]}""", default);
        await session.HandleAsync(new("goals"));
        var created = events.Last(item => item.Type == "goal-item").Goal!;
        Check(created is { Title: "Preview round trip", Status: "Active", Total: 1, Done: 0 }, "The goals panel sees the bot’s saved goal");
        var finish = System.Text.Json.JsonSerializer.Serialize(new { action = "finish", id = created.Id });
        var refusal = await session.ExecuteExtraToolAsync("goal", finish, default);
        Check(!refusal.Output.StartsWith('{'), "An unfinished goal cannot be reported as complete");
        var service = new Automaton.Core.Services.Goals.GoalTrackingService { ChecksExecutable = false };
        string project = VrexChatStore.Project;
        var human = Automaton.Core.Services.Goals.GoalCaller.ForHuman(project, chatId);
        service.Bind(human, chatId, created.Id);
        Check(service.LoopStart(chatId, project, "", true).Proceed && service.LoopBeforeIteration(chatId).Proceed,
            "An active saved goal can start Automaton’s continuation loop");
        var done = System.Text.Json.JsonSerializer.Serialize(new { action = "task", id = created.Id, step = "T1", status = "Done", note = "Observed the expected checklist." });
        await session.ExecuteExtraToolAsync("goal", done, default);
        Check(service.LoopAfterIteration(chatId, null).Proceed, "Recorded progress permits the next turn");
        await session.ExecuteExtraToolAsync("goal", finish, default);
        Check(!service.LoopAfterIteration(chatId, null).Proceed, "The loop stops at the review gate");
        await session.HandleAsync(new("goal-detail", created.Id));
        Check(events.Last(item => item.Type == "goal-detail").Goal is { Status: "Awaiting review", Done: 1, ActiveStep: null },
            "Completed steps and review state are visible");
        await session.HandleAsync(new("goal-accept", created.Id));
        await session.HandleAsync(new("goal-detail", created.Id));
        Check(events.Last(item => item.Type == "goal-detail").Goal?.Status == "Completed",
            "Human review completes the shared goal");
        await session.ExecuteExtraToolAsync("goal", """{"action":"create","title":"Retry fixture","description":"Retry after a guest restart","steps":["Read the current screen","Keep verified work"]}""", default);
        await session.HandleAsync(new("goals"));
        var retry = events.Last(item => item.Type == "goal-item" && item.Goal?.Title == "Retry fixture").Goal!;
        var blockStep = System.Text.Json.JsonSerializer.Serialize(new { action = "task", id = retry.Id, step = "T1", status = "Blocked", note = "Ubuntu needs to restart." });
        var finishStep = System.Text.Json.JsonSerializer.Serialize(new { action = "task", id = retry.Id, step = "T2", status = "Done", note = "Saved the result." });
        await session.ExecuteExtraToolAsync("goal", blockStep, default);
        await session.ExecuteExtraToolAsync("goal", finishStep, default);
        await session.ExecuteExtraToolAsync("goal", System.Text.Json.JsonSerializer.Serialize(new { action = "block", id = retry.Id, note = "Waiting for Ubuntu." }), default);
        await session.HandleAsync(new("goal-detail", retry.Id));
        Check(events.Last().Goal?.Steps?.First().Note == "Ubuntu needs to restart.", "The panel shows why a step was blocked");
        await session.ExecuteExtraToolAsync("goal", System.Text.Json.JsonSerializer.Serialize(new { action = "resume", id = retry.Id }), default);
        await session.HandleAsync(new("goal-detail", retry.Id));
        Check(events.Last().Goal is { Status: "Active", Done: 1, Steps: [{ Status: "Todo" }, { Status: "Done" }] },
            "Retry requeues the blocked step and preserves completed work");
        await session.HandleAsync(new("goal-pause", retry.Id));
        await session.HandleAsync(new("goal-detail", retry.Id));
        Check(events.Last().Goal is { Status: "Backlog", Done: 1, Steps: [{ Status: "Todo" }, { Status: "Done" }] } &&
            service.ListGoals(human, Automaton.Core.Models.GoalStatus.Active).All(item => item.Goal.Id != retry.Id),
            "Pausing a goal preserves steps and excludes it from continuous goal work");
        await session.HandleAsync(new("goal-delete", retry.Id));
        Check(service.GetGoal(human, retry.Id).Tasks.Count == 2, "An unconfirmed request cannot delete a goal");
        await session.HandleAsync(new("goal-delete", retry.Id, Key: 1));
        Check(events.Last(item => item.Type == "goal-deleted").Text == retry.Id &&
            service.ListGoals(human).All(item => item.Goal.Id != retry.Id), "Confirmed delete removes the goal");
        Console.WriteLine("PASS: durable goals alongside task lists, progress continuation and human completion review.");
    }

    private static async Task VerifyCalendarAsync()
    {
        var events = new List<VrexEvent>();
        var channel = VrexProtocol.ChannelPrefix + Guid.NewGuid().ToString("N");
        await using var session = new VrexSession(channel, Settings.Root, events.Add);
        Check(!session.ExtraToolSchemas().ContainsKey("calendar"), "Closed chat does not expose calendar actions");
        await session.HandleAsync(new("chat-active", Key: 1));
        Check(session.ExtraToolSchemas().ContainsKey("calendar"), "Vrex can use its local calendar from chat");
        var day = DateTime.Today.AddDays(2);
        var start = day.ToString("yyyy-MM-dd", System.Globalization.CultureInfo.InvariantCulture);
        var arguments = System.Text.Json.JsonSerializer.Serialize(new { action = "create", title = "Fixture calendar booking", start = start + " 09:30", minutes = 30 });
        var guest = new Guest();
        using var guestTools = new GuestTools(new GuestComputer(guest, "Ubuntu desktop"), guest)
        {
            AllowTool = session.AllowsGuestTool
        };
        using var chat = new TestingChat(new Settings { Model = "offline-fixture" }, guestTools,
            new CalendarChatProvider(arguments), persona: VrexSession.Vrex, household: [VrexSession.Vrex])
        {
            AdditionalToolSchemas = session.ExtraToolSchemas,
            ExecuteAdditionalTool = session.ExecuteExtraToolAsync
        };
        bool answered = false;
        await chat.RunAsync("Book the fixture time in my calendar.", (role, text) =>
            answered |= role == "PersonaBot" && text == "Booked the fixture time.", CancellationToken.None);
        Check(answered, "Ubuntu chat must answer with a single, worker-owned calendar tool");
        await session.HandleAsync(new("calendar", start));
        var booking = events.Last(item => item.Type == "calendar-item").Booking!;
        Check(booking.Title == "Fixture calendar booking" && booking.Start == day.AddHours(9.5) && booking.Minutes == 30,
            "A local calendar booking appears in the requested week");
        var clash = await session.ExecuteExtraToolAsync("calendar", arguments, default);
        Check(clash.Output.Contains("has the desktop", StringComparison.Ordinal), "Calendar rejects overlapping bookings");
        var remove = System.Text.Json.JsonSerializer.Serialize(new { action = "delete", id = booking.Id });
        await session.ExecuteExtraToolAsync("calendar", remove, default);
        events.Clear();
        await session.HandleAsync(new("calendar", start));
        Check(!events.Any(item => item.Type == "calendar-item"), "Deleting a booking clears it from the calendar");
        Console.WriteLine("PASS: local Vrex calendar booking, weekly listing, clash handling and removal.");
    }

    private sealed class CalendarChatProvider(string arguments) : IAIProvider
    {
        private bool bookingRequested;
        public string Id => "calendar-chat-fixture";

        public Task<AIResponse> GenerateResponseAsync(List<Message> messages, Dictionary<string, JsonSchema> tools, string model,
            double? temperature = null, int? maxTokens = null, CancellationToken cancellationToken = default,
            Action<string>? textDeltaCallback = null, Automaton.Core.Streaming.RawStreamRecorder? rawStreamRecorder = null)
        {
            Check(tools.ContainsKey("cu_grid") && tools.ContainsKey(CalendarTool.ToolId),
                "The provider receives the guest tools and the Vrex calendar together");
            if (bookingRequested)
            {
                Check(messages.Any(message => message.Role == "tool" && message.Content.Contains("Fixture calendar booking")),
                    "The calendar call returns the booking created by Vrex");
                return Task.FromResult(new AIResponse { Text = "Booked the fixture time." });
            }
            bookingRequested = true;
            return Task.FromResult(new AIResponse
            {
                ToolCalls = [new() { Id = "fixture-calendar", Tool = CalendarTool.ToolId, Arguments = arguments }]
            });
        }
    }

    private static async Task VerifyForegroundChatAsync()
    {
        var events = new System.Collections.Concurrent.ConcurrentQueue<VrexEvent>();
        var fixture = new ForegroundProvider();
        var channel = VrexProtocol.ChannelPrefix + Guid.NewGuid().ToString("N");
        await using var session = new VrexSession(channel, Settings.Root, events.Enqueue, suppliedProvider: fixture);
        await session.HandleAsync(new("configure-model", Provider: "openai-compatible", Model: "foreground-test",
            Endpoint: "http://127.0.0.1:8080/v1", ApiKey: "fixture"));
        await session.HandleAsync(new("chat-active", Key: 1));
        await session.HandleAsync(new("chat-new"));
        string first = events.Last(item => item.Type == "chat-list-start").Text!;
        await session.HandleAsync(new("chat", "Work on the first chat"));
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(12));
        Check((await fixture.Prompts.Reader.ReadAsync(timeout.Token)).Contains("Work on the first chat"), "The first chat starts its own work");
        await session.HandleAsync(new("chat-new"));
        Check(fixture.Cancelled.Task.IsCompletedSuccessfully &&
            events.Any(item => item.Chat is { OnHold: true } entry && entry.Id == first),
            "Switching chats cancels the running turn and marks the first chat on hold");
        await session.HandleAsync(new("chat", "Answer in the foreground chat"));
        Check((await fixture.Prompts.Reader.ReadAsync(timeout.Token)).Contains("Answer in the foreground chat"),
            "The new chat can start an independent turn");
        await session.HandleAsync(new("chat-switch", first));
        Check((await fixture.Prompts.Reader.ReadAsync(timeout.Token)).Contains("Resume the interrupted task"),
            "Returning to the held chat resumes its saved task");
        await session.HandleAsync(new("chat-stop"));
        Console.WriteLine("PASS: one foreground chat, cancelled old work, visible hold and resume on return.");
    }

    private sealed class ForegroundProvider : Automaton.AI.Interfaces.IAIProvider
    {
        private int requests;
        internal System.Threading.Channels.Channel<string> Prompts { get; } = System.Threading.Channels.Channel.CreateUnbounded<string>();
        internal TaskCompletionSource Cancelled { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public string Id => "foreground-fixture";

        public async Task<AIResponse> GenerateResponseAsync(List<Message> messages, Dictionary<string, JsonSchema> tools, string model,
            double? temperature = null, int? maxTokens = null, CancellationToken cancellationToken = default,
            Action<string>? textDeltaCallback = null, Automaton.Core.Streaming.RawStreamRecorder? rawStreamRecorder = null)
        {
            Prompts.Writer.TryWrite(messages.Last(item => item.Role == "user").Content);
            if (Interlocked.Increment(ref requests) == 1)
            {
                try { await Task.Delay(Timeout.Infinite, cancellationToken); }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                { Cancelled.TrySetResult(); throw; }
            }
            return new AIResponse { Text = "Foreground fixture done." };
        }
    }

    private static async Task VerifyIndependentConfigurationAsync()
    {
        var events = new List<VrexEvent>();
        var channel = VrexProtocol.ChannelPrefix + Guid.NewGuid().ToString("N");
        await using var session = new VrexSession(channel, Settings.Root, events.Add);
        await session.HandleAsync(new("configure-ubuntu", CpuCount: 6, MemoryMb: 8192, PlayGuestAudio: false));
        await session.HandleAsync(new("configure-model", Provider: "openai-compatible", Model: "fixture",
            Endpoint: "https://model.example.test/v1", ApiKey: "fixture-key"));
        Check(events.Last().Type == "model-saved", "Model saving must acknowledge success.");
        var afterModel = Settings.Load();
        Check(afterModel.Model == "fixture" && afterModel.ApiKey == "fixture-key" &&
            afterModel.CpuCount == 6 && afterModel.MemoryMb == 8192 && !afterModel.VrexGuestAudio,
            "Saving the model must preserve Ubuntu settings.");
        await session.HandleAsync(new("configure-model", Provider: "openai-compatible", Model: "fixture",
            Endpoint: "https://different.example.test/v1"));
        Check(events.Last().Type == "model-settings-error", "A stored key must not move to another provider endpoint.");
        await session.HandleAsync(new("configure-ubuntu", CpuCount: 4, MemoryMb: 4096));
        var afterUbuntu = Settings.Load();
        Check(afterUbuntu.CpuCount == 4 && afterUbuntu.BaseUrl == "https://model.example.test/v1" &&
            afterUbuntu.Model == "fixture" && afterUbuntu.ApiKey == "fixture-key",
            "Saving Ubuntu settings must preserve the model and key.");
        Console.WriteLine("PASS: independent model and Ubuntu configuration, saved key protection.");
    }

    private static async Task VerifyAppearanceAsync()
    {
        var events = new List<VrexEvent>();
        var channel = VrexProtocol.ChannelPrefix + Guid.NewGuid().ToString("N");
        await using (var session = new VrexSession(channel, Settings.Root, events.Add))
        {
            session.ReportSettings();
            ExpectVoice("bm_fable");
            Check(events.Last(item => item.Type == "settings").Thinking == "low", "Vrex defaults to low thinking");
            Check(events.Last(item => item.Type == "settings").Verbosity == "terse", "Vrex starts with terse replies");
            await session.HandleAsync(new("verbosity", "normal"));
            await session.HandleAsync(new("thinking", "high"));
            Check(Settings.Load().CreateProviderConfig().RequestParameters["reasoning_effort"].GetString() == "high",
                "Vrex forwards the selected reasoning effort");
            await session.HandleAsync(new("thinking", "default"));
            Check(!Settings.Load().CreateProviderConfig().RequestParameters.ContainsKey("reasoning_effort"),
                "Default delegates thinking effort to the provider");
            await session.HandleAsync(new("thinking", "none"));
            Check(Settings.Load().CreateProviderConfig().RequestParameters["reasoning_effort"].GetString() == "none",
                "Vrex sends an explicit off level even for custom endpoints");
            var deepSeek = new Settings { ProviderType = "openai-compatible", BaseUrl = "https://api.deepseek.com/v1", VrexThinking = "none" };
            Check(deepSeek.CreateProviderConfig().RequestParameters["thinking"].GetProperty("type").GetString() == "disabled",
                "Vrex sends the native thinking-off switch to DeepSeek");
            await session.HandleAsync(new("chat-active", Key: 1));
            string activeChat = events.Last(item => item.Type == "chat-list-start").Text!;
            Automaton.Core.Services.TaskLists.ChatTaskListStore.Shared.Set(activeChat, [new() { Text = "Inspect the Ubuntu display", Status = "in_progress" }]);
            Check(events.Last(item => item.Type == "task-list").Tasks is [{ Status: "in_progress" }],
                "Vrex forwards PersonaBot task-list changes");
            await session.HandleAsync(new("avatar-select", Key: 1));
            ExpectVoice("bf_emma");
            await session.HandleAsync(new("voice-select", "bm_fable"));
            ExpectVoice("bm_fable");
            await session.HandleAsync(new("avatar-select", Key: 2));
            ExpectVoice("bm_george");
            await session.HandleAsync(new("avatar-select", Key: 1));
            ExpectVoice("bm_fable");
            await session.HandleAsync(new("shell-select", Key: 0x297FE2));
        }
        events.Clear();
        await using (var restored = new VrexSession(channel, Settings.Root, events.Add))
        {
            restored.ReportSettings();
            var appearance = events.Last(item => item.Type == "appearance");
            Check(events.Last(item => item.Type == "settings").Thinking == "none", "Vrex restores the saved thinking level");
            Check(events.Last(item => item.Type == "settings").Verbosity == "normal", "Vrex restores the selected reply length");
            if (appearance.Avatar != 1 || appearance.ShellRgb != 0x297FE2)
                throw new InvalidOperationException("Vrex did not restore its companion model and shell colour.");
            ExpectVoice("bm_fable");
        }
        Console.WriteLine("PASS: per-companion default and custom voices, avatar selection and shell colour persistence.");

        void ExpectVoice(string id)
        {
            if (events.Last(item => item.Type == "voice-options").Text != id)
                throw new InvalidOperationException($"Expected companion voice {id}.");
        }
    }

    private static async Task VerifyChatMemoryAsync()
    {
        const string oldText = "Searchable obsidian wayfinder";
        const string secondText = "Unrelated copper sundial";
        string legacy = Path.Combine(Settings.Root, "conversation.json");
        SessionStore.Save(new SessionState
        {
            History = [new Message { Role = "user", Content = oldText }]
        }, legacy);
        var store = new VrexChatStore(Settings.Root);
        string original = store.Active;
        Check(store.Load().Transcript is [{ Text: oldText }], "The old JSON chat opens with its existing transcript");
        store.Create();
        string second = store.Active;
        Check(second != original && store.Load().History.Count == 0, "A new chat has independent history");
        Check(store.Recall("obsidian", second).Contains(oldText, StringComparison.Ordinal) &&
            !store.Recall("obsidian", original).Contains(oldText, StringComparison.Ordinal), "FTS finds the migrated message only from another chat");
        store.Save(second, new SessionState
        {
            History = [new Message { Role = "user", Content = secondText }],
            Transcript = [new TranscriptEntry("You", secondText, null, DateTimeOffset.UtcNow)]
        });
        var events = new List<VrexEvent>();
        await using var session = new VrexSession(VrexProtocol.ChannelPrefix + Guid.NewGuid().ToString("N"), Settings.Root, events.Add);
        await session.HandleAsync(new("chat-active", Key: 1));
        Check(events.Last(message => message.Type == "chat-list-start").Text == second &&
            events.Any(message => message.ChatLine?.Text == secondText), "The last selected chat and transcript reopen");
        await session.HandleAsync(new("chat-switch", original));
        events.Clear();
        await session.HandleAsync(new("chat-clear", original));
        Check(store.Active == original && store.Load().History.Count == 0 &&
            !events.Any(message => message.Type == "chat-list-line") && !store.Recall("obsidian", second).Contains("obsidian"),
            "Clear keeps the chat and removes its history and memory");
        await session.HandleAsync(new("chat-delete", second));
        Check(store.Entries.Single().Id == original && !store.Recall("sundial", original).Contains("sundial"),
            "Delete removes the chat and its memory");
        await session.HandleAsync(new("chat-delete", original));
        Check(store.Entries.Single().Id != original && store.Load().Transcript.Count == 0,
            "Deleting the last chat opens a fresh chat and does not reimport the old transcript");
        Console.WriteLine("PASS: migrated chat, independent SQLite histories, scoped FTS recall, switch, clear and delete.");
    }

    private static async Task VerifyStorageMoveAsync()
    {
        string current = Path.Combine(Settings.Root, "fixture-vm");
        string destination = Path.Combine(Settings.Root, "fixture-destination");
        string active = "";
        await VrexVmStorage.ChangeAsync(current, destination, false, () => active = destination, _ => { }, CancellationToken.None);
        if (active != destination || !Directory.Exists(destination))
            throw new IOException("Choosing a VM folder failed.");

        byte[] guestData = [3, 1, 4, 1, 5, 9];
        string sourceDisk = Path.Combine(destination, CloudInstaller.DiskName);
        string targetDisk = Path.Combine(current, CloudInstaller.DiskName);
        await File.WriteAllBytesAsync(sourceDisk, guestData);
        const string failure = "Simulated settings write failure.";
        try
        {
            await VrexVmStorage.ChangeAsync(destination, current, true, () => throw new IOException(failure), _ => { }, CancellationToken.None);
            throw new InvalidOperationException("VM folder move did not propagate a settings failure.");
        }
        catch (IOException ex) when (ex.Message == failure) { }
        if (File.Exists(targetDisk) || !File.Exists(sourceDisk))
            throw new IOException("A failed VM move did not leave the original disk intact.");

        await VrexVmStorage.ChangeAsync(destination, current, true, () => active = current, _ => { }, CancellationToken.None);
        if (active != current || File.Exists(sourceDisk) || !(await File.ReadAllBytesAsync(targetDisk)).SequenceEqual(guestData))
            throw new IOException("Moving the VM failed to preserve its disk data.");
        Console.WriteLine("PASS: VM folder selection, disk move, and rollback after a settings failure.");
    }

    internal static async Task<int> VoiceAsync()
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(20) };
        using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(25));
        foreach (var model in new[] { VoiceModels.Speech, VoiceModels.Hearing, VoiceModels.Detector })
        {
            if (VoiceModels.IsInstalled(model)) continue;
            Console.WriteLine($"Preparing isolated test model: {model.Title}.");
            await VoiceModels.InstallAsync(model, http, null, timeout.Token);
        }
        var launch = new System.Diagnostics.ProcessStartInfo(Path.Combine(AppContext.BaseDirectory, "voice", "personabot-voice.exe"))
        {
            UseShellExecute = false, CreateNoWindow = true
        };
        foreach (string argument in new[] { "--bargetest", VoiceModels.FolderOf(VoiceModels.Speech),
            VoiceModels.FolderOf(VoiceModels.Hearing), VoiceModels.DetectorFile }) launch.ArgumentList.Add(argument);
        using var helper = System.Diagnostics.Process.Start(launch) ?? throw new IOException("Couldn't start the voice self-test.");
        await helper.WaitForExitAsync(timeout.Token);
        return helper.ExitCode;
    }
}
