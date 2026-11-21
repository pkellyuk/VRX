using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using Automaton.Core.Interfaces;
using Automaton.Core.Services.ComputerUse;

namespace PersonaBot;

/// <summary>
/// Guide Me: instead of doing a job, the bot guides the person through it on their own desktop. It points at the
/// thing to use and says what to do (<see cref="GuidePointTool"/>), asks questions (Automaton's question tool), keeps
/// a task list, draws on the screen (cu_mark), and may offer to do a step itself, only with a yes (<see
/// cref="GuideOfferTool"/>). Whatever shows the guide (the 3D bot over the window) implements this.
/// </summary>
public interface IGuide
{
    /// <summary>
    /// Points at a spot on the desktop (guest screen pixels) and says <see cref="GuidePointRequest.Say"/>; returns when
    /// the person clicks the desktop (if asked to wait for that), presses Done, Do it for me or Complete the task, picks one of the
    /// request's options, types something, or the wait runs out.
    /// </summary>
    Task<GuideReply> PointAsync(GuidePointRequest request, CancellationToken token);

    /// <summary>Asks the person a question in the guide's bubble: options to pick and/or free text.</summary>
    Task<QuestionResponse> AskAsync(QuestionRequest request, CancellationToken token);

    /// <summary>Offers to do <paramref name="step"/> for the person and returns their choice.</summary>
    Task<GuideOfferOutcome> OfferAsync(string step, CancellationToken token);
}

public enum GuideOfferOutcome { Declined, DoStep, CompleteTask }

/// <param name="X">Guest screen pixels.</param>
/// <param name="WaitForClick">Return when the person clicks the desktop (else only on a button or typing).</param>
/// <param name="Options">Short replies shown as buttons ("Blue", "The cheaper one"): one click answers.</param>
public sealed record GuidePointRequest(double X, double Y, string Say, bool WaitForClick, IReadOnlyList<string>? Options = null);

/// <summary>How a pointing step ended. DoIt: the person asked the bot to do the step for them.</summary>
public enum GuideOutcome { Clicked, Done, Stuck, Said, TimedOut, DoIt, CompleteTask }

/// <param name="Text">What the person typed, if they did.</param>
/// <param name="ClickX">Where they clicked (guest pixels), if they did.</param>
public sealed record GuideReply(GuideOutcome Outcome, string? Text = null, double? ClickX = null, double? ClickY = null)
{
    /// <summary>The reply as the bot reads it.</summary>
    public string Describe(GuidePointRequest asked)
    {
        ArgumentNullException.ThrowIfNull(asked);
        var where = ClickX is { } x && ClickY is { } y
            ? $" at {x:0},{y:0} ({Math.Sqrt(Math.Pow(x - asked.X, 2) + Math.Pow(y - asked.Y, 2)):0} px from where you pointed)"
            : "";
        return Outcome switch
        {
            GuideOutcome.Clicked => $"The person clicked the desktop{where}. Take a screenshot to see what changed before the next step.",
            GuideOutcome.Done => "The person pressed Done: they say they've done it. Take a screenshot to check before the next step.",
            GuideOutcome.Stuck => "The person is stuck. Look at the screen and explain this step differently, point more precisely, or offer to do it for them (guide_offer).",
            GuideOutcome.DoIt => "The person pressed Do it for me, and their Control switch lets you use the desktop: do this step now while they " +
                                 "watch (they see your pointer and your bot doing it), then take a screenshot and carry on guiding with guide_point. " +
                                 "Still never type passwords or card details, or press Buy, Pay or Submit: point at those for them instead.",
            GuideOutcome.CompleteTask => GuideRules.CompleteTaskInstruction,
            GuideOutcome.Said => $"The person replied: \"{Text}\". Respond to it.",
            _ => "The person didn't respond in time. Ask whether they are still there before going on."
        };
    }
}

/// <summary>guide_point: the bot flies to a spot on the person's desktop, points at it and says what to do there.</summary>
public sealed class GuidePointTool(Func<IGuide?> guide, Func<ScreenRect> screen) : ITool
{
    public const string ToolId = "guide_point";

    public string Id => ToolId;

    public string Description =>
        "Guide Me: point at one thing on the person's desktop and tell them what to do with it, then wait for them. " +
        "Use it for each step instead of clicking yourself. at is the thing's centre in screen pixels (from cu_find_text, cu_tags or a screenshot). " +
        "say is one short instruction (\"Click Buy, top right\"). options are short replies shown as buttons, for anything you'd like them to choose " +
        "or tell you (\"Blue\", \"Black\"): give them whenever a step has a choice. It returns when they click the desktop, press Done, pick an option, " +
        "type to you, or delegate one step or the rest of the task; look at the screen before the next step.";

    public JsonSchema Parameters { get; } = new()
    {
        Properties = new Dictionary<string, JsonSchemaProperty>
        {
            ["at"] = new() { Type = "string", Description = "\"x,y\" in screen pixels: the centre of the thing to use." },
            ["say"] = new() { Type = "string", Description = "One short instruction, as you'd say it beside them." },
            ["wait_for_click"] = new() { Type = "boolean", Description = "Return as soon as they click the desktop (default true). False for steps that are typing or reading." },
            ["options"] = new() { Type = "string", Description = "Up to 5 short replies the person can click instead of typing, separated by |: \"The 256 GB one | The 512 GB one | Not sure\"." }
        },
        Required = ["at", "say"]
    };

    public async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        ArgumentNullException.ThrowIfNull(args);
        ArgumentNullException.ThrowIfNull(context);
        using var doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(args) ? "{}" : args);
        var root = doc.RootElement;
        var say = root.TryGetProperty("say", out var s) && s.ValueKind == JsonValueKind.String ? s.GetString()?.Trim() : null;
        if (string.IsNullOrWhiteSpace(say)) throw new ArgumentException("say what the person should do.");
        var at = root.TryGetProperty("at", out var a) && a.ValueKind == JsonValueKind.String ? a.GetString() : null;
        var bounds = screen();
        var point = ScreenMarkParser.Point(at ?? "", bounds);
        var wait = !root.TryGetProperty("wait_for_click", out var w) || w.ValueKind != JsonValueKind.False;
        if (guide() is not { } shown)
        {
            Trace.TraceWarning($"GuidePointTool.ExecuteAsync: chat {context.SessionId}: no guide on screen");
            return new ToolResult { Title = "No guide", Output = "Guide Me isn't on screen. Say what to do in your reply instead." };
        }
        var options = Options(root);
        var request = new GuidePointRequest(point.X, point.Y, say.Length > 200 ? say[..200] : say, wait, options);
        Trace.WriteLine($"GuidePointTool.ExecuteAsync enter: chat {context.SessionId}: at {point.X:0},{point.Y:0}, wait for click={wait}, {say.Length} chars");
        var reply = await shown.PointAsync(request, context.CancellationToken);
        Trace.WriteLine($"GuidePointTool.ExecuteAsync exit: {reply.Outcome}");
        return new ToolResult { Title = $"Guided: {reply.Outcome}", Output = reply.Describe(request) };
    }

    /// <summary>The clickable replies: "a | b | c" (or a JSON array, if a model sends one), up to 5 of up to 60 characters.</summary>
    internal static List<string> Options(JsonElement root)
    {
        if (!root.TryGetProperty("options", out var value)) return [];
        IEnumerable<string> raw = value.ValueKind switch
        {
            JsonValueKind.String => (value.GetString() ?? "").Split('|'),
            JsonValueKind.Array => value.EnumerateArray().Where(e => e.ValueKind == JsonValueKind.String).Select(e => e.GetString() ?? ""),
            _ => []
        };
        return raw.Select(t => t.Trim()).Where(t => t.Length > 0).Select(t => t.Length > 60 ? t[..60].TrimEnd() : t).Distinct().Take(5).ToList();
    }
}

/// <summary>guide_offer: the bot offers to do a step, and the person can delegate the step or the whole task.</summary>
public sealed class GuideOfferTool(Func<IGuide?> guide) : ITool
{
    public const string ToolId = "guide_offer";

    public string Id => ToolId;

    public string Description =>
        "Guide Me: offer to do one step for the person (when they're stuck, or it's fiddly). Only if they say yes do it (with a yes, their Control switch " +
        "lets you use the mouse and keyboard), then carry on guiding with guide_point. If they delegate the rest of the task, follow that instruction. " +
        "If they say no, guide them through it.";

    public JsonSchema Parameters { get; } = new()
    {
        Properties = new Dictionary<string, JsonSchemaProperty>
        {
            ["step"] = new() { Type = "string", Description = "The step you'd do, in a few words (\"fill in the delivery address\")." }
        },
        Required = ["step"]
    };

    public async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        ArgumentNullException.ThrowIfNull(args);
        ArgumentNullException.ThrowIfNull(context);
        using var doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(args) ? "{}" : args);
        var step = doc.RootElement.TryGetProperty("step", out var s) && s.ValueKind == JsonValueKind.String ? s.GetString()?.Trim() : null;
        if (string.IsNullOrWhiteSpace(step)) throw new ArgumentException("Say which step you'd do.");
        if (guide() is not { } shown) return new ToolResult { Title = "No guide", Output = "Guide Me isn't on screen; ask in your reply instead." };
        Trace.WriteLine($"GuideOfferTool.ExecuteAsync enter: chat {context.SessionId}: offering \"{step}\"");
        var choice = await shown.OfferAsync(step, context.CancellationToken);
        Trace.WriteLine($"GuideOfferTool.ExecuteAsync exit: {choice}");
        return choice switch
        {
            GuideOfferOutcome.DoStep => new ToolResult { Title = "Yes: do it", Output = $"The person said yes: do \"{step}\" now with the mouse and keyboard. Then carry on guiding with guide_point." },
            GuideOfferOutcome.CompleteTask => new ToolResult { Title = "Complete the task", Output = GuideRules.CompleteTaskInstruction },
            _ => new ToolResult { Title = "No thanks", Output = "The person said no: guide them through it themselves with guide_point." }
        };
    }
}

/// <summary>
/// The person's drawing on the desktop, put into words for the guide with exact screen positions: its own screenshots
/// don't show the person's ink, so it needs to be told where they circled, underlined or pointed.
/// </summary>
public static class GuideDrawing
{
    /// <summary>A description of <paramref name="strokes"/> (guest screen pixels), or null when there are none.</summary>
    public static string? Describe(IReadOnlyList<IReadOnlyList<(double X, double Y)>> strokes)
    {
        ArgumentNullException.ThrowIfNull(strokes);
        var described = strokes.Where(s => s.Count > 0).Take(8).Select(Stroke).ToList();
        if (described.Count == 0) return null;
        var lines = described.Select((text, i) => $"{i + 1}) {text}");
        return "The person drew on the screen to show you what they mean (screen pixels): " + string.Join("; ", lines) +
               ". Use what they marked as the thing they're asking about.";
    }

    /// <summary>One stroke: a dot, a circle round an area, a line (from where to where), or a scribble over an area.</summary>
    public static string Stroke(IReadOnlyList<(double X, double Y)> points)
    {
        ArgumentNullException.ThrowIfNull(points);
        if (points.Count == 0) throw new ArgumentException("A stroke has at least one point.", nameof(points));
        string At((double X, double Y) p) => GuideRules.At(p.X, p.Y);
        var first = points[0];
        var last = points[^1];
        double minX = points.Min(p => p.X), maxX = points.Max(p => p.X), minY = points.Min(p => p.Y), maxY = points.Max(p => p.Y);
        var size = Math.Max(maxX - minX, maxY - minY);
        if (points.Count < 3 || size < 6) return $"a dot at {At(first)}";
        double length = 0;
        for (var i = 1; i < points.Count; i++) length += Distance(points[i - 1], points[i]);
        var gap = Distance(first, last);
        var area = $"{GuideRules.At(minX, minY)} to {GuideRules.At(maxX, maxY)} (centre {GuideRules.At((minX + maxX) / 2, (minY + maxY) / 2)})";
        // Back near where it started, having gone round: a circle (or box) round something.
        if (gap < size * 0.3 && length > size * 2) return $"circled the area {area}";
        // About as long as the distance it covers: a straight line, ending where it points.
        if (length < gap * 1.25) return $"a line from {At(first)} to {At(last)}";
        return $"a scribble over {area}";
    }

    private static double Distance((double X, double Y) a, (double X, double Y) b) => Math.Sqrt(Math.Pow(a.X - b.X, 2) + Math.Pow(a.Y - b.Y, 2));
}

/// <summary>The instructions a bot gets in Guide Me, on top of its usual rules.</summary>
public static class GuideRules
{
    public const string CompleteTaskInstruction =
        "The person pressed Complete the task. Take over the current task from this point and carry out the remaining steps yourself, " +
        "using the desktop tools while their Control setting permits it. Do not offer to do each step or wait for routine confirmation. " +
        "Pause for a necessary decision, a step the person must do, or if they take back control. " +
        "Never type passwords or card details or press Buy, Pay or Submit. Verify the result and report what you completed. " +
        "This delegation applies only to the current task.";

    /// <summary>Tools a guide doesn't get: it may not take the desktop back on its own.</summary>
    public static readonly IReadOnlySet<string> Withheld = new HashSet<string>(StringComparer.Ordinal) { TakeControlTool.ToolId };

    /// <summary>Tools only a guide gets.</summary>
    public static readonly IReadOnlySet<string> GuideOnly = new HashSet<string>(StringComparer.Ordinal) { GuidePointTool.ToolId, GuideOfferTool.ToolId, "question" };

    public const string Prompt = """
        GUIDE ME. The person has asked you to guide them, not to do the job for them. They have the mouse and keyboard;
        yours are switched off (unless they accept an offer). Your job is to get them to their goal on their own desktop, step by step:
        1. Look at the screen (screenshot, cu_find_text, cu_tags) to see where they are.
        2. If you need to know something (which model, which size, their budget), ask with the question tool, always with 2 to 5 short
           clickable options (they can still type). Prefer clicks to typing everywhere: never ask an open question when you can offer choices.
        3. Set a short task list (task_list) of the steps to their goal, and keep it up to date as they go.
        4. For each step, call guide_point at the exact thing to use, with one short instruction. When the step involves a choice or you'd
           like to hear back, give options (buttons) too. Use cu_mark to highlight more if it helps.
           Then look at the screen again to see what they did before the next step.
        5. They can press Do it for me on any step: then do that step yourself, and go back to guiding. If a step is fiddly, you may also
           offer with guide_offer; only with their yes do it. Their Control switch says who may use the desktop: never ask them to change
           it in words; the guide shows them where it is when it's wrong for a step. If an action is refused because they have control,
           point at the step with guide_point instead.
        6. If a guide tool reports that the person pressed Complete the task, follow that delegation for this task only.
           Do the remaining steps yourself while Control permits. Use questions or guide pointing only when the person must decide or act.
           Once finished, verify the result and report it as your final reply. Start a new request in ordinary step-by-step guide mode.
        7. When they've reached the goal while being guided, or you've found what they asked, say so in one or two short sentences, then ask what next with
           the question tool and clickable options (for example "Show me something else", "That's all, thanks"). If they pick "That's
           all", your short goodbye is your final reply.
        They can draw on the screen with a pen to show you something: you'll be told where (and may get a screenshot with their drawing).
        Treat what they circle, underline or point at as what they mean, and guide them from there.
        You'll be told which screen they're looking at (their own desktop, or your screen); your screenshots, cu_find_text, cu_tags and
        guide_point all work on that one. If it's unclear which screen, window or thing they mean, ask with the question tool rather than guessing.
        Keep everything you say short and friendly, like someone standing beside them. Never type passwords, card numbers or other secrets
        for them, and never press Buy, Pay or Submit yourself: those are always theirs.
        """;

    /// <summary>The chat's title for a guide session: "Guide: " and the start of the question.</summary>
    public static string ChatTitle(string question)
    {
        ArgumentNullException.ThrowIfNull(question);
        var text = string.Join(' ', question.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
        if (text.Length > 48) text = text[..47].TrimEnd() + "…";
        return "Guide: " + (text.Length == 0 ? "help" : text);
    }

    /// <summary>What the guide is told about the screen the person is looking at.</summary>
    public static string ScreenNote(bool mainDisplay, string botName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(botName);
        return mainDisplay
            ? "The person is looking at their own desktop (the main display). Your screenshots, cu_find_text, cu_tags and guide_point now all use that screen, not yours. " +
              "To show them your own screen instead (they ask to see your desktop), call cu_switch_screen with screen=\"own\"."
            : $"The person is looking at your own screen ({botName}'s). Your screenshots, cu_find_text, cu_tags and guide_point use it. " +
              "To go back to their desktop, call cu_switch_screen with screen=\"main\".";
    }

    /// <summary>
    /// What a guide's bot is told when the person switches to another bot mid-task: Guide Me has ended, but the task
    /// carries on, in the background, without the bubble.
    /// </summary>
    public const string EndedButCarryOn =
        "The person switched to another bot, so Guide Me has ended: there's no bubble now, so guide_point, guide_offer and questions in the bubble no longer reach them. " +
        "Carry on with the task in the background: your screenshots and clicks are on your own screen again (if you have one). " +
        "When you finish, or need them, say so in your reply: they see it in the bot timeline and you'll be brought to their attention.";

    /// <summary>"x,y" for a point, as the guide tools take it.</summary>
    public static string At(double x, double y) => string.Create(CultureInfo.InvariantCulture, $"{x:0},{y:0}");
}
