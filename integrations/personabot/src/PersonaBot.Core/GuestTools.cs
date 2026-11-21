using System.Text;
using System.Text.Json;
using Automaton.Core.Interfaces;
using Automaton.Core.Services.ComputerUse;
using Automaton.Core.Services.TaskLists;
using Automaton.Tools.Tools;

namespace PersonaBot;

public sealed class GuestTools : IDisposable
{
    private readonly Dictionary<string, ITool> _tools;
    private readonly IGuestShell _shell;
    private readonly ITextRecognizer? _recognizer;
    private readonly bool _groundMarks;
    private readonly Dictionary<IBotScreen, ScreenTools> _botTools = [];

    /// <summary>The computer-use tools bound to one screen: the main display's, or one bot's own.</summary>
    private sealed record ScreenTools(GuestComputer Computer, ComputerUseSession Session, IReadOnlyList<ITool> Tools);

    private ScreenTools BuildScreenTools(GuestComputer computer, Action<IReadOnlyList<ScreenMark>, bool> marks)
    {
        System.Diagnostics.Trace.WriteLine($"GuestTools.BuildScreenTools enter: recognizer {(_recognizer == null ? "none" : "on")}");
        var session = new ComputerUseSession((IComputerScreen)computer, new GridOverlayRenderer(), computer, _recognizer);
        var mark = new MarkTool(computer.GetVirtualDesktopBounds, marks, capture: _groundMarks ? computer.CaptureVirtualDesktop : null);
        mark.ReferenceShown += view => MarkReferenceShown?.Invoke(view);
        ITool[] tools = [new BeginGuest(session, computer.DisplayName), new ComputerUseEndTool(session), new ComputerUseScreensTool(session),
            new ComputerUseGridTool(session), new ComputerUseZoomOutTool(session), new ComputerUseScreenshotTool(session),
            new ComputerUseClickTool(session), new ComputerUseClickAtTool(session), new ComputerUseMoveTool(session),
            new ComputerUseDragTool(session), new ComputerUseScrollTool(session), new ComputerUseTypeTool(session),
            new ComputerUseKeyTool(session), new ComputerUseFindTextTool(session), new ComputerUsePeekTool(session),
            mark,
            // xidentify: a local model tags the screen. Always offered; it says so when this computer has none.
            new ScreenTagTool(() => Tagger, token => computer.CaptureAsync(token), marks)];
        System.Diagnostics.Trace.WriteLine($"GuestTools.BuildScreenTools exit: {tools.Length} tools");
        return new ScreenTools(computer, session, tools);
    }
    public IReadOnlyCollection<string> Ids => _tools.Keys;
    /// <param name="taskLists">
    /// Where each chat's task list is kept (Automaton's <see cref="ChatTaskListStore"/>); with one, bots get the
    /// task_list tool and the chat shows the list. Smoke checks pass none.
    /// </param>
    public GuestTools(GuestComputer computer, IGuestShell shell, ITextRecognizer? recognizer = null,
        ChatTaskListStore? taskLists = null, bool groundMarks = false)
    {
        ArgumentNullException.ThrowIfNull(computer);
        ArgumentNullException.ThrowIfNull(shell);
        System.Diagnostics.Trace.WriteLine($"GuestTools enter: shell {shell.GetType().Name}, recognizer {(recognizer == null ? "none" : recognizer.GetType().Name)}, task lists {(taskLists == null ? "off" : "on")}");
        _shell = shell;
        _recognizer = recognizer;
        _groundMarks = groundMarks;
        TaskLists = taskLists;
        var main = BuildScreenTools(computer, (marks, replace) => MarksChanged?.Invoke(marks, replace));
        Session = main.Session;
        ITool[] tools = [.. main.Tools, new ShellTool(shell), new ReadTool(shell), new WriteTool(shell),
            new SnapshotTool(() => Recorder), new RecordTool(() => Recorder)];
        _tools = tools.ToDictionary(t => t.Id, StringComparer.Ordinal);
        if (taskLists != null) _tools[TaskListToolInfo.ToolId] = new TaskListTool(() => taskLists);
        // Take control / give back: a bot may take the desktop back itself, saying why (TakeControlTool).
        var take = new TakeControlTool(() => PersonHasControl, (reason, chat) =>
        {
            if (!PersonHasControl) return true;
            if (ControlTakenBack is not { } handler)
            {
                System.Diagnostics.Trace.TraceWarning($"GuestTools.TakeControl: refused for chat {chat ?? "?"}: nobody handles ControlTakenBack");
                return false;
            }
            System.Diagnostics.Trace.WriteLine($"GuestTools.TakeControl: bot takes the desktop back for chat {chat ?? "?"} (reason {reason?.Length ?? 0} chars)");
            PersonHasControl = false;
            handler(reason, chat);
            return true;
        });
        _tools[take.Id] = take;
        // Its own screen or the main display: a bot moves between them, and the person's view goes with it.
        _tools[SwitchScreenTool.ToolId] = new SwitchScreenTool();
        var find = new FindInRecordingsTool(() => RecordingSearch);
        _tools[find.Id] = find;
        // Guide Me: pointing, offering, and Automaton's question tool (answered in the guide's bubble).
        _tools[GuidePointTool.ToolId] = new GuidePointTool(() => Guide, computer.GetVirtualDesktopBounds);
        _tools[GuideOfferTool.ToolId] = new GuideOfferTool(() => Guide);
        var question = new QuestionTool(new Automaton.Core.Services.InboxService());
        _tools[question.Id] = question;
        // The personas' calendar (the Calendar window): the bots book, move and cancel their time.
        _tools[CalendarTool.ToolId] = new CalendarTool(() => CalendarSource?.Invoke(), () => PersonaSource?.Invoke(),
            chat => PersonaForChat?.Invoke(chat), () => CalendarChanged?.Invoke());
        System.Diagnostics.Trace.WriteLine($"GuestTools: {_tools.Count} tools, task lists {(taskLists == null ? "off" : "on")}");
    }
    /// <summary>The main display's computer-use session.</summary>
    public ComputerUseSession Session { get; }
    public event Action<GridView>? MarkReferenceShown;
    public void ResetMarkReference() => ((MarkTool)_tools["cu_mark"]).ResetReference();

    /// <summary>
    /// Gives a bot its own screen (XMEETING.md §3), starting it if need be; null leaves every bot on the main
    /// display. With it set, a bot that has a Linux user of its own (<see cref="UsesOwnScreen"/>) gets
    /// computer-use tools bound to its screen, and its shell, terminal and browser open there too.
    /// </summary>
    public Func<GuestAccount, CancellationToken, Task<IBotScreen>>? OwnScreens { get; set; }

    /// <summary>Whether this account's tools work on a screen of its own rather than the main display.</summary>
    public bool UsesOwnScreen(GuestAccount account)
    {
        ArgumentNullException.ThrowIfNull(account);
        // The desktop's own user (the Website Tester) lives on the main display: its Firefox is already open there.
        // A bot the person asked onto the main display works there too.
        return OwnScreens != null && !account.IsDesktopUser && account.Style == GuestAccountStyle.Debian && !IsOnMainDisplay(account);
    }

    private readonly HashSet<string> _onMainDisplay = new(StringComparer.Ordinal);

    /// <summary>Whether the person has asked this bot to work on the machine's main display instead of its own screen.</summary>
    public bool IsOnMainDisplay(GuestAccount account)
    {
        ArgumentNullException.ThrowIfNull(account);
        lock (_onMainDisplay) return _onMainDisplay.Contains(account.User);
    }

    /// <summary>
    /// Moves a bot onto the machine's main display (the person's desktop), or back to its own screen. Its next tool
    /// call uses the new screen: computer-use tools, shell, terminal and browser. Returns whether anything changed.
    /// </summary>
    public bool SetOnMainDisplay(GuestAccount account, bool onMain)
    {
        ArgumentNullException.ThrowIfNull(account);
        bool changed;
        lock (_onMainDisplay) changed = onMain ? _onMainDisplay.Add(account.User) : _onMainDisplay.Remove(account.User);
        System.Diagnostics.Trace.WriteLine($"GuestTools.SetOnMainDisplay: {account.User} {(onMain ? "on the main display" : "on its own screen")}{(changed ? "" : " (already)")}");
        return changed;
    }

    /// <summary>A tool call is starting (its id, and the chat it's for): the bots act out the kind of work.</summary>
    public event Action<string, string?>? ToolStarted;

    /// <summary>
    /// Does cu_switch_screen for a bot: moves it to its own screen (true) or the main display (false), and the
    /// person's view with it; returns what the bot is told. Null when the app can't switch screens.
    /// </summary>
    public Func<GuestAccount, bool, string?, CancellationToken, Task<string>>? ScreenSwitcher { get; set; }

    /// <summary>cu_switch_screen with the calling bot's account: explains when it can't, else lets the app switch.</summary>
    private async Task<ToolResult> SwitchScreenAsync(GuestAccount? account, string arguments, string? chatId, CancellationToken token)
    {
        var (own, reason) = SwitchScreenTool.Read(arguments);
        System.Diagnostics.Trace.WriteLine($"GuestTools.SwitchScreenAsync enter: {account?.User ?? "root"} to {(own ? "its own screen" : "the main display")} (chat {chatId ?? "?"}), reason {reason?.Length ?? 0} chars");
        string? cannot = account == null ? "You have no screen of your own here: you work on the main display."
            : account.IsDesktopUser ? "You are the desktop's own user: the main display is your screen, so there's no other screen to switch to."
            : account.Style != GuestAccountStyle.Debian || OwnScreens == null ? "Bots don't have screens of their own on this computer (or it's turned off in Settings): you work on the main display."
            : ScreenSwitcher == null ? "Screens can't be switched just now."
            : null;
        if (cannot != null)
        {
            System.Diagnostics.Trace.WriteLine($"GuestTools.SwitchScreenAsync exit: not switched: {cannot}");
            return new ToolResult { Title = "Can't switch screens", Output = cannot };
        }
        var told = await ScreenSwitcher!(account!, own, reason, token);
        System.Diagnostics.Trace.WriteLine($"GuestTools.SwitchScreenAsync exit: {told.Length} chars told");
        return new ToolResult { Title = own ? "On your own screen" : "On the main display", Output = told };
    }

    /// <summary>What a bot is told, mid-task, when the person moves it onto the main display or back.</summary>
    public static string MainDisplayMessage(bool onMain) => onMain
        ? "The person has asked you to work on the machine's main display (:0), their own desktop, instead of your own screen. " +
          "From your next tool call, your computer-use tools see and control the main display, and your terminal and `personabot-browser` open there. " +
          "Call cu_begin, then take a fresh screenshot. Work alongside the person's windows: don't close or change theirs unless the task needs it."
        : "The person has moved you back to your own screen. From your next tool call, your computer-use tools, terminal and browser are on your own screen again; " +
          "call cu_begin, then take a fresh screenshot. Leave the main display as it is.";

    /// <summary>Raised when a bot starts working on its own screen: at its first tool call there, and at each cu_begin.</summary>
    public event Action<IBotScreen, GuestAccount>? BotScreenInUse;

    /// <summary>A bot's pointer moved on its own screen.</summary>
    public event Action<IBotScreen, ScreenPoint>? BotPointerMoved;

    /// <summary>The personas' calendar, for the calendar tool; null (or returning null) until the app has one.</summary>
    public Func<PersonaCalendar?>? CalendarSource { get; set; }

    /// <summary>The personas, for the calendar tool (booking by name).</summary>
    public Func<PersonaStore?>? PersonaSource { get; set; }

    /// <summary>The persona a chat belongs to (chat id → persona id): calendar bookings default to the calling bot.</summary>
    public Func<string?, string?>? PersonaForChat { get; set; }

    /// <summary>A bot made, changed or removed a booking.</summary>
    public event Action? CalendarChanged;

    /// <summary>A bot is about to use the mouse or keyboard (the tool's id), so the app can act it out.</summary>
    public event Action<string>? InputUsed;

    /// <summary>A bot clicked on its own screen.</summary>
    public event Action<IBotScreen, ScreenPoint>? BotPointerClicked;

    /// <summary>A bot drew on its own screen with cu_mark or cu_tags: the marks, and whether they replace those shown.</summary>
    public event Action<IBotScreen, IReadOnlyList<ScreenMark>, bool>? BotMarksChanged;

    private (ScreenTools Tools, bool Created) ToolsFor(IBotScreen screen)
    {
        lock (_botTools)
        {
            if (_botTools.TryGetValue(screen, out var existing)) return (existing, false);
            System.Diagnostics.Trace.WriteLine($"GuestTools.ToolsFor enter: new tools for display :{screen.DisplayNumber} ({_botTools.Count} bot screens so far)");
            var computer = new GuestComputer(screen);
            computer.PointerMoved += point => BotPointerMoved?.Invoke(screen, point);
            computer.PointerClicked += point => BotPointerClicked?.Invoke(screen, point);
            var made = BuildScreenTools(computer, (marks, replace) => BotMarksChanged?.Invoke(screen, marks, replace));
            _botTools[screen] = made;
            System.Diagnostics.Trace.WriteLine($"GuestTools.ToolsFor: computer-use tools for display :{screen.DisplayNumber}");
            return (made, true);
        }
    }

    /// <summary>Ends every computer-use session: the main display's and each bot's.</summary>
    public void Dispose()
    {
        System.Diagnostics.Trace.WriteLine("GuestTools.Dispose enter");
        Session.Dispose();
        lock (_botTools)
        {
            System.Diagnostics.Trace.WriteLine($"GuestTools.Dispose: ending {_botTools.Count} bot screen sessions");
            foreach (var bot in _botTools.Values) bot.Session.Dispose();
            _botTools.Clear();
        }
        System.Diagnostics.Trace.WriteLine("GuestTools.Dispose exit");
    }

    /// <summary>The tools that press keys or move the guest's pointer, which take turns with the person's input (RunControl).</summary>
    public static readonly IReadOnlySet<string> InputToolIds = new HashSet<string>(StringComparer.Ordinal)
    {
        "cu_click", "cu_click_at", "cu_move", "cu_drag", "cu_scroll", "cu_type", "cu_key", "cu_focus_window"
    };

    /// <summary>Whether a tool uses the guest's mouse or keyboard.</summary>
    public static bool UsesInput(string tool)
    {
        ArgumentNullException.ThrowIfNull(tool);
        return InputToolIds.Contains(tool);
    }

    /// <summary>Each chat's task list, keyed by chat id; null when bots don't keep one.</summary>
    public ChatTaskListStore? TaskLists { get; }

    /// <summary>Where cu_snapshot and cu_record save to: the app's timeline. Null where there is none (smoke checks).</summary>
    public IScreenRecorder? Recorder { get; set; }

    /// <summary>The timeline find_in_recordings searches; null where there is none (smoke checks).</summary>
    public IRecordingSearch? RecordingSearch { get; set; }

    /// <summary>The host's local vision model for cu_tags; null where there is none.</summary>
    public IScreenTagger? Tagger { get; set; }

    private volatile bool _personHasControl;

    /// <summary>
    /// The person has taken control of this machine's desktop (the Control switch): the bots' mouse and keyboard
    /// tools are refused, with <see cref="PersonHasControlMessage"/>, until it is given back. Looking (screenshots,
    /// grids, text) and the shell still work.
    /// </summary>
    public bool PersonHasControl { get => _personHasControl; set => _personHasControl = value; }

    /// <summary>What a bot is told when it tries the mouse or keyboard while the person has control.</summary>
    public const string PersonHasControlMessage =
        "The person has taken control of the desktop: your mouse and keyboard are switched off until they give it back. " +
        "Don't try again yourself. If the person asked you to carry on, or the task needs the desktop and they have left it, call cu_take_control with a short reason; " +
        "otherwise end your turn now with a short note of what you were about to do, and you will be told when control is given back.";

    /// <summary>What a working bot is told when the person starts sharing the desktop with it (Control: Shared).</summary>
    public const string DesktopSharedMessage =
        "The person is now sharing the desktop with you: they may click and type on it while you work. Carry on, " +
        "but look at the screen before each action, as they may have changed it, and leave alone anything they are in the middle of.";

    /// <summary>What a working bot is told when the person stops sharing and leaves the desktop to it (Control: Off).</summary>
    public const string DesktopYoursMessage = "The person has stopped using the desktop: it is yours alone again.";

    /// <summary>Raised when a bot takes the desktop back with cu_take_control: its reason and the chat it came from.</summary>
    public event Action<string, string?>? ControlTakenBack;

    /// <summary>Raised (with the chat's id) when a bot's input was refused because the person has control.</summary>
    public event Action<string?>? InputRefused;

    /// <summary>Raised when a bot draws on the person's view with cu_mark: the marks, and whether they replace those shown.</summary>
    public event Action<IReadOnlyList<ScreenMark>, bool>? MarksChanged;
    public Func<string, bool>? AllowTool { private get; init; }
    public Dictionary<string, JsonSchema> Schemas => SchemasFor(guide: false);

    /// <summary>
    /// The tools a run gets: in Guide Me the guide's (pointing, offering, questions) and never cu_take_control; otherwise
    /// everything but the guide's.
    /// </summary>
    public Dictionary<string, JsonSchema> SchemasFor(bool guide) => _tools
        .Where(p => AllowTool?.Invoke(p.Key) != false)
        .Where(p => guide ? !GuideRules.Withheld.Contains(p.Key) : !GuideRules.GuideOnly.Contains(p.Key))
        .ToDictionary(p => p.Key, p => { p.Value.Parameters.Description = p.Value.Description; return p.Value.Parameters; });

    /// <summary>Whatever shows Guide Me (the 3D bot); null when no guide is on screen.</summary>
    public IGuide? Guide { get; set; }

    /// <summary>
    /// True while a Guide Me session runs. Who may use the desktop is still the person's Control switch: the guide asks
    /// them to change it when it's wrong for a step, and never changes it itself.
    /// </summary>
    public bool GuideActive { get; set; }

    /// <summary>
    /// In a Guide Me session, whether the person is looking at the main display (their desktop) rather than the bot's
    /// own screen. While true, a bot with a screen of its own looks and points at the main display instead, so it sees
    /// what the person sees.
    /// </summary>
    public bool GuideOnMainDisplay { get; set; } = true;

    /// <summary>
    /// Runs one tool. With an <paramref name="account"/> (a persona's), bash, read and write run as that
    /// Linux user in its home folder; without one they run as root (the platform smoke checks).
    /// <paramref name="chatId"/> is the chat the call comes from, which task_list keeps its list under.
    /// </summary>
    public async Task<ToolResult> ExecuteAsync(string id, string arguments, CancellationToken token, GuestAccount? account = null, string? chatId = null)
    {
        token.ThrowIfCancellationRequested();
        if (AllowTool?.Invoke(id) == false)
            return new ToolResult { Title = "Permission required", Output = "The user must enable this tool in Vrex Settings." };
        ArgumentNullException.ThrowIfNull(arguments);
        System.Diagnostics.Trace.WriteLine($"GuestTools.ExecuteAsync enter: {id} for {account?.User ?? "root"} (chat {chatId ?? "?"}, {arguments.Length} chars of arguments)");
        var watch = System.Diagnostics.Stopwatch.StartNew();
        // Bad JSON from the model becomes a tool error the bot can fix, not the end of the chat.
        using var doc = ToolArguments.Parse(id, arguments);
        ValidateArguments(doc.RootElement);
        if (!_tools.TryGetValue(id, out var tool))
        {
            System.Diagnostics.Trace.TraceWarning($"GuestTools.ExecuteAsync: unknown tool {id} (chat {chatId ?? "?"})");
            throw new ArgumentException("This tool is unavailable in PersonaBot: " + id);
        }
        if (PersonHasControl && UsesInput(id))
        {
            // Take control / give back (as Automaton's VPS handover): the bot may look, not touch.
            System.Diagnostics.Trace.WriteLine($"GuestTools.ExecuteAsync: {id} refused: the person has control (chat {chatId ?? "?"})");
            InputRefused?.Invoke(chatId);
            return new ToolResult { Title = "The person has control", Output = PersonHasControlMessage };
        }
        if (UsesInput(id)) InputUsed?.Invoke(id);
        ToolStarted?.Invoke(id, chatId);
        if (id == SwitchScreenTool.ToolId) return await SwitchScreenAsync(account, arguments, chatId, token);
        // A guide sees what the person sees: on the main display, a bot's own screen is set aside.
        if (account != null && UsesOwnScreen(account) && !(GuideActive && GuideOnMainDisplay))
        {
            // This bot works on its own screen: its computer-use tools are that screen's, and its shell, terminal
            // and browser open there.
            var screen = await OwnScreens!(account, token);
            var (own, created) = ToolsFor(screen);
            System.Diagnostics.Trace.WriteLine($"GuestTools.ExecuteAsync: {id} for {account.User} on its own display :{screen.DisplayNumber}{(created ? " (new tools)" : "")}");
            account = account.OnDisplay(screen.DisplayNumber);
            if (own.Tools.FirstOrDefault(t => t.Id == id) is { } screenTool) tool = screenTool;
            if (created || id == "cu_begin") BotScreenInUse?.Invoke(screen, account);
        }
        if (account != null && tool is ShellTool or ReadTool or WriteTool)
        {
            System.Diagnostics.Trace.WriteLine($"GuestTools.ExecuteAsync: {id} as {account.User}");
            var shell = new AccountShell(_shell, account);
            // As a persona, everything shows in its Bot terminal window on the desktop.
            tool = tool switch { ShellTool => new ShellTool(shell, account), ReadTool => new ReadTool(shell, account), _ => new WriteTool(shell, account) };
        }
        // The timeline says which bot captured each screenshot or recording.
        if (account != null && tool is SnapshotTool snapshot) tool = snapshot.For(account.DisplayName);
        if (account != null && tool is RecordTool record) tool = record.For(account.DisplayName);
        var home = account?.Home ?? "/home/tc";
        // Questions go to the guide's bubble when one is on screen; without one the question tool says it can't ask.
        Func<QuestionRequest, Task<QuestionResponse>>? ask = Guide is { } guide ? request => guide.AskAsync(request, token) : null;
        var result = await Task.Run(() => tool.ExecuteAsync(arguments, new ToolContext { SessionId = chatId ?? "", Directory = home, Worktree = home, CancellationToken = token, AskQuestionCallback = ask }), token);
        System.Diagnostics.Trace.WriteLine($"GuestTools.ExecuteAsync exit: {id} for {account?.User ?? "root"} in {watch.ElapsedMilliseconds} ms: \"{result.Title}\", {result.Output?.Length ?? 0} chars");
        return result;
    }
    public static void ValidateArguments(JsonElement args)
    {
        if (args.ValueKind != JsonValueKind.Object)
        {
            System.Diagnostics.Trace.TraceWarning($"GuestTools.ValidateArguments: refused: arguments are {args.ValueKind}, not an object");
            throw new ArgumentException("Tool arguments must be an object.");
        }
        foreach (var property in args.EnumerateObject())
            if (property.Name.Equals("deviceId", StringComparison.OrdinalIgnoreCase) || property.Name.Equals("machine", StringComparison.OrdinalIgnoreCase))
            {
                System.Diagnostics.Trace.TraceWarning($"GuestTools.ValidateArguments: refused: device routing argument {property.Name}");
                throw new ArgumentException("Guest tools target only their own guest; device and machine routing is unavailable.");
            }
    }
    private abstract class SimpleTool : ITool
    {
        public abstract string Id { get; }
        public abstract string Description { get; }
        public abstract JsonSchema Parameters { get; }
        public abstract Task<ToolResult> ExecuteAsync(string args, ToolContext context);
        protected static JsonSchema Schema(params string[] fields) => new() { Properties = fields.ToDictionary(f => f, f => new JsonSchemaProperty { Type = "string", Description = f + " inside the guest." }), Required = fields.ToList() };
        protected static string String(JsonElement args, string key) => args.GetProperty(key).GetString() ?? throw new ArgumentException(key + " is required.");
    }
    private sealed class BeginGuest(ComputerUseSession session, string displayName) : SimpleTool
    {
        public override string Id => "cu_begin";
        public override string Description => $"Take control of the {displayName}. All guest screen and input tools act only inside this guest.";
        public override JsonSchema Parameters { get; } = Schema("reason");
        public override Task<ToolResult> ExecuteAsync(string args, ToolContext context)
        {
            using var doc = JsonDocument.Parse(args);
            System.Diagnostics.Trace.WriteLine("GuestTools.BeginGuest.ExecuteAsync: cu_begin");
            session.Begin(String(doc.RootElement, "reason"));
            return Task.FromResult(new ToolResult { Title = "Guest control ready", Output = $"Use cu_grid to inspect the {displayName} screen." });
        }
    }
    private sealed class ShellTool(IGuestShell shell, GuestAccount? visibleTo = null) : SimpleTool
    {
        public override string Id => "bash";
        public override string Description => "Run a shell script inside the guest using its shell and work folder. " +
            "Standard input is closed, so use non-interactive options. For browsing, documents and anything visual, use the guest screen. " +
            "No Windows commands or paths are available. Commands are bounded to 150 seconds.";
        public override JsonSchema Parameters { get; } = Schema("command");
        public override async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
        {
            using var doc = JsonDocument.Parse(args);
            var command = String(doc.RootElement, "command");
            var script = visibleTo == null ? command : BotTerminal.VisibleCommand(command, visibleTo);
            System.Diagnostics.Trace.WriteLine($"GuestTools.ShellTool.ExecuteAsync enter: {command.Length} chars as {visibleTo?.User ?? "root"}");
            var output = await shell.ExecuteAsync(script, context.CancellationToken);
            System.Diagnostics.Trace.WriteLine($"GuestTools.ShellTool.ExecuteAsync exit: {output.Length} chars of output");
            return new ToolResult { Title = "Guest shell", Output = output };
        }
    }
    private sealed class ReadTool(IGuestShell shell, GuestAccount? visibleTo = null) : SimpleTool
    {
        public override string Id => "read";
        public override string Description => "Read up to 600 lines of a file inside the guest. Use the shell tool with sed to read other line ranges.";
        public override JsonSchema Parameters { get; } = Schema("path");
        public override async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
        {
            using var doc = JsonDocument.Parse(args);
            var path = String(doc.RootElement, "path");
            var script = "head -n 600 -- " + GuestShellText.Quote(path);
            if (visibleTo != null) script = BotTerminal.WithNote("[read] " + path, script, visibleTo);
            System.Diagnostics.Trace.WriteLine($"GuestTools.ReadTool.ExecuteAsync enter: {path} as {visibleTo?.User ?? "root"}");
            var output = await shell.ExecuteAsync(script, context.CancellationToken);
            System.Diagnostics.Trace.WriteLine($"GuestTools.ReadTool.ExecuteAsync exit: {path}, {output.Length} chars");
            return new ToolResult { Title = "Guest file", Output = output };
        }
    }
    private sealed class WriteTool(IGuestShell shell, GuestAccount? visibleTo = null) : SimpleTool
    {
        public override string Id => "write";
        public override string Description => "Write a UTF-8 text file inside the guest, replacing its existing contents. Relative paths are in your guest work folder.";
        public override JsonSchema Parameters { get; } = Schema("path", "content");
        public override async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
        {
            using var doc = JsonDocument.Parse(args);
            var octal = GuestShellText.Octal(Encoding.UTF8.GetBytes(String(doc.RootElement, "content")));
            var path = String(doc.RootElement, "path");
            var script = "printf '%b' '" + octal + "' > " + GuestShellText.Quote(path) + "; r=$?; sync; exit $r";
            if (visibleTo != null) script = BotTerminal.WithNote("[wrote] " + path, script, visibleTo);
            System.Diagnostics.Trace.WriteLine($"GuestTools.WriteTool.ExecuteAsync enter: {path} as {visibleTo?.User ?? "root"}, {octal.Length} chars encoded");
            var output = await shell.ExecuteAsync(script, context.CancellationToken);
            System.Diagnostics.Trace.WriteLine($"GuestTools.WriteTool.ExecuteAsync exit: {path}, {output.Length} chars of output");
            return new ToolResult { Title = "Guest file saved", Output = output };
        }
    }
}
