using System.Diagnostics;
using System.Text.Json;
using Automaton.Core.Interfaces;

namespace PersonaBot;

/// <summary>
/// cu_take_control: a bot switches Control back off itself, taking the desktop back from the person, with a reason
/// the person sees at once (the switch flips, the status line and the bot's speech say why). For when the person has
/// asked it to carry on, or the job needs the desktop and the person isn't using it; never to grab it while they are.
/// </summary>
public sealed class TakeControlTool(Func<bool> personHasControl, Func<string, string?, bool> takeBack) : ITool
{
    public const string ToolId = "cu_take_control";

    public string Id => ToolId;

    public string Description =>
        "Take the desktop back when the person has control (the Control switch is On and your mouse and keyboard are refused). " +
        "Use it when the person has asked you to carry on, or the task needs the desktop and they have left it; " +
        "not while they are clearly using it themselves. The person sees at once that you took it and your reason. " +
        "Afterwards look at the screen first: they may have changed it.";

    public JsonSchema Parameters { get; } = new()
    {
        Properties = new Dictionary<string, JsonSchemaProperty>
        {
            ["reason"] = new() { Type = "string", Description = "Why you need the desktop now, in a few words the person will see." }
        },
        Required = ["reason"]
    };

    public Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        ArgumentNullException.ThrowIfNull(args);
        ArgumentNullException.ThrowIfNull(context);
        using var doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(args) ? "{}" : args);
        var reason = doc.RootElement.TryGetProperty("reason", out var r) && r.ValueKind == JsonValueKind.String ? r.GetString()?.Trim() : null;
        if (string.IsNullOrWhiteSpace(reason))
        {
            Trace.TraceWarning($"TakeControlTool.ExecuteAsync: refused for chat {context.SessionId}: no reason");
            throw new ArgumentException("Give a short reason the person will see.");
        }
        Trace.WriteLine($"TakeControlTool.ExecuteAsync: chat {context.SessionId}: {reason}");
        if (!personHasControl())
        {
            Trace.WriteLine($"TakeControlTool.ExecuteAsync exit: chat {context.SessionId}: the person doesn't have control");
            return Task.FromResult(new ToolResult { Title = "You have the desktop", Output = "Nobody else has control: you can use the mouse and keyboard already." });
        }
        var taken = takeBack(reason, string.IsNullOrEmpty(context.SessionId) ? null : context.SessionId);
        Trace.WriteLine($"TakeControlTool.ExecuteAsync exit: chat {context.SessionId}: taken={taken}");
        return Task.FromResult(taken
            ? new ToolResult { Title = "Control taken back", Output = "You have the desktop again and the person has been told why. Look at the screen first: they may have changed it." }
            : new ToolResult { Title = "Control not taken", Output = "The desktop couldn't be taken back just now (nothing is listening for it here). Ask the person to switch Control off." });
    }
}
