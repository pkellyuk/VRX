using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

/// <summary>
/// Represents the complete state of a conversation for persistence.
/// </summary>
public class ConversationState
{
    /// <summary>
    /// Unique session identifier.
    /// </summary>
    [JsonPropertyName("sessionId")]
    public string SessionId { get; set; } = string.Empty;

    /// <summary>
    /// Current message ID for tracking.
    /// </summary>
    [JsonPropertyName("messageId")]
    public string MessageId { get; set; } = string.Empty;

    /// <summary>
    /// Agent identifier (cli, desktop, etc.).
    /// </summary>
    [JsonPropertyName("agent")]
    public string Agent { get; set; } = string.Empty;

    /// <summary>
    /// Working directory for the session.
    /// </summary>
    [JsonPropertyName("directory")]
    public string Directory { get; set; } = string.Empty;

    /// <summary>
    /// Worktree path for the session.
    /// </summary>
    [JsonPropertyName("worktree")]
    public string Worktree { get; set; } = string.Empty;

    /// <summary>
    /// Associated project ID (if any).
    /// </summary>
    [JsonPropertyName("projectId")]
    public string? ProjectId { get; set; }

    /// <summary>
    /// Conversation messages history.
    /// </summary>
    [JsonPropertyName("messages")]
    public List<Core.Interfaces.Message> Messages { get; set; } = new();

    /// <summary>
    /// Current compression settings.
    /// </summary>
    [JsonPropertyName("compressionSettings")]
    public Core.Services.ConversationCompressionSettings? CompressionSettings { get; set; }

    /// <summary>
    /// Whether reviewer roles should run automatically for this chat.
    /// </summary>
    [JsonPropertyName("useRolesEnabled")]
    public bool UseRolesEnabled { get; set; }

    /// <summary>
    /// Whether thinking blocks should be shown in the UI.
    /// </summary>
    [JsonPropertyName("showThinkingEnabled")]
    public bool ShowThinkingEnabled { get; set; } = true;

    /// <summary>
    /// Whether tool call entries should be shown in the UI.
    /// </summary>
    [JsonPropertyName("showToolCallsEnabled")]
    public bool ShowToolCallsEnabled { get; set; } = true;

    /// <summary>
    /// Whether thinking blocks start collapsed in the UI.
    /// </summary>
    [JsonPropertyName("collapseThinkingEnabled")]
    public bool CollapseThinkingEnabled { get; set; } = true;

    /// <summary>
    /// Whether tool call blocks start collapsed in the UI.
    /// </summary>
    [JsonPropertyName("collapseToolCallsEnabled")]
    public bool CollapseToolCallsEnabled { get; set; } = true;

    /// <summary>
    /// Optional provider override for this chat. Null means use the current default provider.
    /// </summary>
    [JsonPropertyName("selectedProviderKey")]
    public string? SelectedProviderKey { get; set; }

    /// <summary>
    /// Optional model override for this chat. Null means use the current default model.
    /// </summary>
    [JsonPropertyName("selectedModelId")]
    public string? SelectedModelId { get; set; }

    /// <summary>
    /// Optional per-chat thinking level, one of <c>none</c>, <c>low</c>, <c>medium</c> or
    /// <c>high</c>. Null means "whatever the provider was configured with", so a chat that
    /// has never touched the control keeps behaving exactly as it did before this existed.
    /// </summary>
    [JsonPropertyName("reasoningEffort")]
    public string? ReasoningEffort { get; set; }

    /// <summary>
    /// Optional per-chat reply length, <c>terse</c> or <c>verbose</c>; null is normal. See
    /// <see cref="ResponseVerbosity"/>.
    /// </summary>
    [JsonPropertyName("verbosity")]
    public string? Verbosity { get; set; }

    /// <summary>
    /// Whether tool permissions are auto-approved (true) or require user confirmation (false).
    /// </summary>
    [JsonPropertyName("fullPermissionMode")]
    public bool FullPermissionMode { get; set; }

    /// <summary>
    /// Timestamp when the state was last saved.
    /// </summary>
    [JsonPropertyName("lastSavedAt")]
    public DateTimeOffset LastSavedAt { get; set; }

    /// <summary>
    /// Monotonic revision for ordering conversation updates across devices.
    /// </summary>
    [JsonPropertyName("revision")]
    public long Revision { get; set; }

    /// <summary>
    /// Display name for the conversation (used in remote sync).
    /// </summary>
    [JsonPropertyName("displayName")]
    public string? DisplayName { get; set; }

    /// <summary>
    /// Whether the LLM is currently processing/generating a response.
    /// </summary>
    [JsonPropertyName("isProcessing")]
    public bool IsProcessing { get; set; }

    /// <summary>
    /// Status text describing the current state of the conversation.
    /// </summary>
    [JsonPropertyName("statusText")]
    public string? StatusText { get; set; }

    /// <summary>
    /// Version of the state format for migration support.
    /// </summary>
    [JsonPropertyName("version")]
    public int Version { get; set; } = 1;

    /// <summary>
    /// Create a new ConversationState instance.
    /// </summary>
    public ConversationState()
    {
        SessionId = Guid.NewGuid().ToString();
        MessageId = Guid.NewGuid().ToString();
        LastSavedAt = DateTimeOffset.UtcNow;
        Version = 1;
    }
}
