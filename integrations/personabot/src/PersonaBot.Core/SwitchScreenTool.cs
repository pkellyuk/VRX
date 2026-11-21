using System.Diagnostics;
using System.Text.Json;
using Automaton.Core.Interfaces;

namespace PersonaBot;

/// <summary>
/// cu_switch_screen: a bot with a screen of its own moves between it and the machine's main display (the person's
/// desktop), and the person's view goes with it, so they watch where it works. "own": it works on its own screen
/// and the person is shown that screen. "main": it works on the main display, alongside the person's windows, and
/// the person is shown the main display. The same as the toolbar's screen buttons, for when the person asks
/// ("show me your desktop", "do it on my screen"). GuestTools runs it (it needs the calling bot's account); the app
/// does the switching (<see cref="GuestTools.ScreenSwitcher"/>).
/// </summary>
public sealed class SwitchScreenTool : ITool
{
    public const string ToolId = "cu_switch_screen";

    public string Id => ToolId;

    public string Description =>
        "Move between your own screen and the machine's main display (the person's own desktop); the person's view moves with you, so they watch where you work. " +
        "screen=\"own\": you work on your own screen and the person is shown it (\"show me your desktop\", or a job best kept off theirs). " +
        "screen=\"main\": you work on the main display, alongside their windows (\"do it on my screen\", or something already open there). " +
        "Afterwards call cu_begin and take a fresh screenshot. Only bots with a screen of their own can switch.";

    public JsonSchema Parameters { get; } = new()
    {
        Properties = new Dictionary<string, JsonSchemaProperty>
        {
            ["screen"] = new() { Type = "string", Description = "\"own\" for your own screen, \"main\" for the main display (the person's desktop).", Enum = ["own", "main"] },
            ["reason"] = new() { Type = "string", Description = "Why, in a few words the person will see." }
        },
        Required = ["screen"]
    };

    /// <summary>Reads the arguments: whether it's the bot's own screen, and the reason (if any).</summary>
    public static (bool Own, string? Reason) Read(string args)
    {
        ArgumentNullException.ThrowIfNull(args);
        using var doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(args) ? "{}" : args);
        var screen = doc.RootElement.TryGetProperty("screen", out var s) && s.ValueKind == JsonValueKind.String ? s.GetString()?.Trim().ToLowerInvariant() : null;
        var reason = doc.RootElement.TryGetProperty("reason", out var r) && r.ValueKind == JsonValueKind.String ? r.GetString()?.Trim() : null;
        return screen switch
        {
            "own" or "mine" or "bot" => (true, string.IsNullOrWhiteSpace(reason) ? null : reason),
            "main" or "person" or "theirs" or "desktop" => (false, string.IsNullOrWhiteSpace(reason) ? null : reason),
            _ => throw new ArgumentException("screen must be \"own\" (your own screen) or \"main\" (the main display, the person's desktop).")
        };
    }

    /// <summary>Called only without a bot's account (GuestTools runs it with one): there's no screen of its own to switch to.</summary>
    public Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        ArgumentNullException.ThrowIfNull(args);
        ArgumentNullException.ThrowIfNull(context);
        Trace.WriteLine($"SwitchScreenTool.ExecuteAsync: chat {context.SessionId}: no bot account, nothing to switch");
        return Task.FromResult(new ToolResult { Title = "Can't switch", Output = "You have no screen of your own here: you work on the main display." });
    }
}
