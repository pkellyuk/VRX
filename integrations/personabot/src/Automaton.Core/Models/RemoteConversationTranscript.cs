using System.Collections.Generic;
using System.Linq;
using Automaton.Core.Interfaces;

using Automaton.Core.Services;

namespace Automaton.Core.Models;

public sealed class RemoteConversationTranscript
{
    public string DisplayName { get; set; } = string.Empty;
    public long Revision { get; set; }
    [System.Text.Json.Serialization.JsonIgnore(Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public RemoteAttachmentCapabilities? AttachmentCapabilities { get; set; }
    [System.Text.Json.Serialization.JsonIgnore(Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public int? PromptProtocolVersion { get; set; }
    public string StateId { get; set; } = string.Empty;
    public string SessionId { get; set; } = string.Empty;
    public string Directory { get; set; } = string.Empty;
    public string Worktree { get; set; } = string.Empty;
    public string LastSavedAt { get; set; } = string.Empty;
    public bool UseRolesEnabled { get; set; }
    public bool ShowThinkingEnabled { get; set; } = true;
    public bool ShowToolCallsEnabled { get; set; } = true;
    public bool CollapseThinkingEnabled { get; set; } = true;
    public bool CollapseToolCallsEnabled { get; set; } = true;
    public string? SelectedProviderKey { get; set; }
    public string? SelectedModelId { get; set; }
    public string? ReasoningEffort { get; set; }

    /// <summary>The chat's reply length, <c>terse</c> or <c>verbose</c>; null is normal.</summary>
    public string? Verbosity { get; set; }

    /// <summary>The model's working task list for this chat; empty when it has none.</summary>
    public List<Automaton.Core.Services.TaskLists.TaskListItem> TaskList { get; set; } = [];

    /// <summary>
    /// The provider that actually serves this conversation on the host, which is
    /// not always <see cref="SelectedProviderKey"/>: that field is what the client
    /// asked for, and the host substitutes when the request names a provider it
    /// does not have. Reporting only the request is how a chat could sit there
    /// labelled with a model that had never answered a single turn.
    /// </summary>
    public string? ResolvedProviderKey { get; set; }

    /// <summary>The model id actually sent to that provider.</summary>
    public string? ResolvedModelId { get; set; }

    /// <summary>
    /// The endpoint the turn is sent to. A provider's name says nothing about
    /// where it points, so the URL is the only thing that distinguishes a local
    /// server from a public one.
    /// </summary>
    public string? ResolvedBaseUrl { get; set; }

    /// <summary>
    /// Set when the host could not honour the requested provider and chose another.
    /// Surfaced in the UI rather than only logged.
    /// </summary>
    public string? ModelSubstitutionReason { get; set; }
    public bool IsProcessing { get; set; }
    public string? StatusText { get; set; }
    public string? ContextUsage { get; set; }
    public List<RemoteConversationModelOption> AvailableModels { get; set; } = [];
    public List<RemoteConversationTranscriptMessage> Messages { get; set; } = [];

    /// <summary>
    /// True when the server had more messages than <c>MaxMessages</c> and truncated the response.
    /// The client can re-request without the limit to get the full transcript.
    /// </summary>
    [System.Text.Json.Serialization.JsonPropertyName("hasMore")]
    public bool HasMore { get; set; }

    /// <summary>
    /// Total number of messages stored on the server for this conversation.
    /// When <c>HasMore</c> is true, this is larger than <c>Messages.Count</c>.
    /// </summary>
    [System.Text.Json.Serialization.JsonPropertyName("totalMessageCount")]
    [System.Text.Json.Serialization.JsonIgnore(Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public int? TotalMessageCount { get; set; }

    public static string BuildContextUsage(IEnumerable<Message> messages, ConversationCompressionSettings? settings)
    {
        var tokenCount = ConversationContextCompressor.EstimateTokens(messages);
        var maxContextTokens = settings?.ContextLimitTokens;
        return maxContextTokens.HasValue && maxContextTokens > 0
            ? $"Context: {tokenCount} tokens{Environment.NewLine}Max: {maxContextTokens}  {(int)Math.Round((double)tokenCount / maxContextTokens.Value * 100)}%"
            : $"Context: {tokenCount} tokens";
    }
}

public sealed class RemoteConversationModelOption
{
    public string? ProviderKey { get; set; }
    public string? ModelId { get; set; }
    public string Label { get; set; } = string.Empty;
    public bool IsDefault { get; set; }

    /// <summary>Optional capability metadata. Absent on older peers; clients must not guess their ladder.</summary>
    [System.Text.Json.Serialization.JsonIgnore(Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public List<string>? ReasoningEffortLevels { get; set; }
}

public sealed class RemoteConversationTranscriptMessage
{
    /// <summary>
    /// Copies attachments for a transcript, leaving the payload behind. Images
    /// are a few hundred kilobytes each and a transcript is fetched repeatedly;
    /// the receiver resolves the bytes from the attachment store by id.
    /// </summary>
    public static List<ChatAttachment> Describe(IEnumerable<ChatAttachment>? attachments)
    {
        if (attachments is null)
        {
            return [];
        }

        return attachments
            .Select(a => new ChatAttachment
            {
                Id = a.Id,
                FileName = a.FileName,
                MediaType = a.MediaType,
                Kind = a.Kind,
                ByteSize = a.ByteSize,
                Error = a.Error,
                // Base64Data, TextContent and StagedPath are deliberately omitted.
            })
            .ToList();
    }

    public string Id { get; set; } = string.Empty;
    public string Role { get; set; } = string.Empty;
    public string Content { get; set; } = string.Empty;
    public List<MessagePart> Parts { get; set; } = [];

    /// <summary>
    /// Files sent with this message, as metadata only - the bytes stay in the
    /// attachment store on the device that holds them. Without this a
    /// conversation rebuilt from a transcript loses its images, which is every
    /// conversation: a local chat is answered through this same protocol.
    /// </summary>
    public List<ChatAttachment> Attachments { get; set; } = [];
}
