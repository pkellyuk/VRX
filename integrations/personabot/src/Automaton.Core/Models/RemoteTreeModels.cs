using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

public sealed class RemoteTreeSnapshot
{
    [JsonPropertyName("projects")]
    public List<RemoteTreeProjectItem> Projects { get; set; } = [];

    [JsonPropertyName("threads")]
    public List<RemoteTreeThreadItem> Threads { get; set; } = [];
}

public sealed class RemoteTreeProjectItem
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("name")]
    public string Name { get; set; } = string.Empty;

    [JsonPropertyName("path")]
    public string Path { get; set; } = string.Empty;

    [JsonPropertyName("threadCount")]
    public int ThreadCount { get; set; }
}

public sealed class RemoteTreeThreadItem
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("name")]
    public string Name { get; set; } = string.Empty;

    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("projectName")]
    public string ProjectName { get; set; } = string.Empty;

    [JsonPropertyName("projectPath")]
    public string ProjectPath { get; set; } = string.Empty;

    [JsonPropertyName("automationEnabled")]
    public bool AutomationEnabled { get; set; }

    [JsonPropertyName("lastModifiedAt")]
    public string LastModifiedAt { get; set; } = string.Empty;
}

public sealed class RemoteTreeSnapshotEnvelope
{
    public string SourceDeviceId { get; init; } = string.Empty;
    public RemoteTreeSnapshot Snapshot { get; init; } = new();
    public long Sequence { get; init; }
}

public sealed class RemoteConversationUpdatedEnvelope
{
    public string SourceDeviceId { get; init; } = string.Empty;
    public long Revision { get; init; }
    public string DisplayName { get; init; } = string.Empty;
    public string StateId { get; init; } = string.Empty;
    public string LastSavedAt { get; init; } = string.Empty;
    public List<Core.Interfaces.Message> Messages { get; init; } = new();
    public bool? IsProcessing { get; init; }
    public string? StatusText { get; init; }
    public string? ContextUsage { get; init; }
    public string? StreamingMessageId { get; init; }
    public string? StreamingRole { get; init; }
    public string? StreamingContent { get; init; }
    public string? StreamingDelta { get; init; }
    public bool? IsStreamingFinal { get; init; }
    public string? UserMessage { get; init; }
    public RemoteConversationStreamEvent? StreamEvent { get; init; }
}

// Request/Response envelope types for GET_TREE and GET_CONVERSATION
public sealed class TreeResponseEnvelope
{
    public string SourceDeviceId { get; init; } = string.Empty;
    public long Revision { get; init; }
    public RemoteTreeSnapshot Snapshot { get; init; } = new();
    public bool IsEmpty { get; init; }
    public string SessionId { get; init; } = string.Empty;
}

public sealed class ConversationResponseEnvelope
{
    public string SourceDeviceId { get; init; } = string.Empty;
    public long Revision { get; init; }
    public string StateId { get; init; } = string.Empty;
    public string DisplayName { get; init; } = string.Empty;
    public string LastSavedAt { get; init; } = string.Empty;
    public List<Core.Interfaces.Message> Messages { get; init; } = new();
    public bool? IsProcessing { get; init; }
    public string? StatusText { get; init; }
    public string? ContextUsage { get; init; }
    public bool IsEmpty { get; init; }
}
