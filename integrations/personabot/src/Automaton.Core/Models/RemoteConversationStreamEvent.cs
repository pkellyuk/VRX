using System.Text.Json.Serialization;
using Automaton.Core.Interfaces;

namespace Automaton.Core.Models;

public sealed class RemoteConversationStreamEvent
{
    [JsonPropertyName("kind")]
    public string Kind { get; set; } = string.Empty;

    [JsonPropertyName("toolName")]
    public string? ToolName { get; set; }

    [JsonPropertyName("toolArguments")]
    public Dictionary<string, object>? ToolArguments { get; set; }

    [JsonPropertyName("toolResult")]
    public ToolResult? ToolResult { get; set; }

    [JsonPropertyName("errorMessage")]
    public string? ErrorMessage { get; set; }

    [JsonPropertyName("roleName")]
    public string? RoleName { get; set; }

    [JsonPropertyName("roleText")]
    public string? RoleText { get; set; }

    [JsonPropertyName("estimatedTokens")]
    public int? EstimatedTokens { get; set; }

    [JsonPropertyName("triggerTokens")]
    public int? TriggerTokens { get; set; }

    /// Post-compaction token count. Set on `compaction_completed` events so
    /// remote viewers can show the dialog's "Tokens after" row. Null on legacy
    /// senders and on non-completion events.
    [JsonPropertyName("finalTokens")]
    public int? FinalTokens { get; set; }

    /// Configured context-window ceiling (ConversationCompressionSettings.ContextLimitTokens).
    /// Lets remote viewers show the dialog's "Max tokens" row. Null on legacy
    /// senders.
    [JsonPropertyName("limitTokens")]
    public int? LimitTokens { get; set; }

    /// How a compaction ended, on `compaction_completed`: one of the
    /// <see cref="CompactionOutcomes"/>. With <see cref="ErrorMessage"/> for the reason when
    /// it is not <see cref="CompactionOutcomes.Summarised"/>. Null on legacy senders.
    [JsonPropertyName("compactionOutcome")]
    public string? CompactionOutcome { get; set; }
}

/// <summary>How a compaction ended.</summary>
public static class CompactionOutcomes
{
    /// <summary>The older messages were replaced by a summary the model wrote.</summary>
    public const string Summarised = "summarised";

    /// <summary>
    /// The older messages were replaced, but by the stub fallback: the model's summary
    /// failed or came back empty. The context is smaller and nothing describes what left it.
    /// </summary>
    public const string Stub = "stub";

    /// <summary>Nothing was changed.</summary>
    public const string NotApplied = "not_applied";

    /// <summary>Compaction itself threw.</summary>
    public const string Failed = "failed";
}
