using System.Diagnostics;
using System.Text.Json;

namespace PersonaBot;

/// <summary>
/// A tool call's arguments as the model sent them. Models sometimes send JSON that doesn't parse (for
/// example <c>{"at":"0.52",0.7}</c> instead of <c>{"at":"0.52,0.7"}</c>). The chat must survive that: the
/// call is kept in the history with no arguments, and the bot is told what was wrong so it can try again.
/// </summary>
public static class ToolArguments
{
    /// <summary>The longest part of the bad arguments repeated back to the bot.</summary>
    public const int MaxEcho = 400;

    /// <summary>The arguments as an object for the chat history, or an empty one when they don't parse.</summary>
    public static Dictionary<string, object> ForHistory(string tool, string? arguments)
    {
        ArgumentNullException.ThrowIfNull(tool);
        if (string.IsNullOrWhiteSpace(arguments))
        {
            Trace.WriteLine($"ToolArguments.ForHistory: {tool} sent no arguments");
            return new();
        }
        try
        {
            return JsonSerializer.Deserialize<Dictionary<string, object>>(arguments) ?? new();
        }
        catch (JsonException ex)
        {
            Trace.WriteLine($"ToolArguments.ForHistory: {tool} sent arguments that aren't a JSON object ({ex.Message}): {Echo(arguments)}");
            return new();
        }
    }

    /// <summary>Parses the arguments, or throws an <see cref="ArgumentException"/> the bot can act on.</summary>
    public static JsonDocument Parse(string tool, string arguments)
    {
        ArgumentNullException.ThrowIfNull(tool);
        ArgumentNullException.ThrowIfNull(arguments);
        try
        {
            return JsonDocument.Parse(string.IsNullOrWhiteSpace(arguments) ? "{}" : arguments);
        }
        catch (JsonException ex)
        {
            Trace.WriteLine($"ToolArguments.Parse: {tool} arguments rejected ({ex.Message}): {Echo(arguments)}");
            throw new ArgumentException(Invalid(tool, arguments, ex.Message), ex);
        }
    }

    /// <summary>What the bot is told when its arguments weren't valid JSON.</summary>
    public static string Invalid(string tool, string arguments, string reason)
    {
        ArgumentNullException.ThrowIfNull(tool);
        ArgumentNullException.ThrowIfNull(arguments);
        ArgumentNullException.ThrowIfNull(reason);
        return $"{tool} didn't run: its arguments weren't valid JSON ({reason}). You sent: {Echo(arguments)} " +
            "Send the call again as one JSON object. Every position is a single quoted string, e.g. \"at\":\"0.42,0.71\".";
    }

    private static string Echo(string arguments) => arguments.Length > MaxEcho ? arguments[..MaxEcho] + "…" : arguments;
}
