namespace Automaton.Core.Services;

/// <summary>
/// The words for what an agent is doing right now - "Running read…", "Finished bash", "Compacting
/// context…" - from a stream event's kind and the few fields that name things. Kept apart from
/// <see cref="StreamEventDescriber"/> and free of dependencies so the Director app can link it
/// (XADVPS.md §2): a chat watched from the phone reads exactly as it does on the desk.
/// </summary>
public static class StreamActivityText
{
    /// <summary>Fallback text when the event says nothing useful.</summary>
    public const string Working = "Working…";

    public static string Describe(string? kind, string? toolName, string? roleName, string? message)
    {
        var tool = string.IsNullOrWhiteSpace(toolName) ? "tool" : toolName;

        return kind switch
        {
            "tool_call_started" => $"Running {tool}…",
            "tool_call_completed" => $"Finished {tool}",
            "tool_call_failed" => $"{tool} failed",
            "role_response" => string.IsNullOrWhiteSpace(roleName)
                ? "Role feedback received"
                : $"{roleName} responded",
            "assistant_response_rejected" => "Response rejected",
            "streaming_loop_aborted" => "Stopped a looping response",
            "provider_retrying" => "Connection lost - retrying…",
            "tool_progress" => string.IsNullOrWhiteSpace(message)
                ? $"Running {tool}…"
                : message.Trim(),
            "turn_incomplete" => "Turn ended early",
            "compaction_started" => "Compacting context…",
            "compaction_completed" => "Context compacted",
            _ => Working
        };
    }
}
