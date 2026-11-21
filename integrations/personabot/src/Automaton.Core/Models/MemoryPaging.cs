using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

/// <summary>Optional read-only paging extension; absence preserves the existing remember protocol.</summary>
public sealed record MemorySearchPageRequest
{
    [JsonPropertyName("version")] public int Version { get; init; } = 1;
    [JsonPropertyName("probe")] public bool Probe { get; init; }
    [JsonPropertyName("offset")] public int Offset { get; init; }
    [JsonPropertyName("limit")] public int Limit { get; init; } = 20;
    [JsonPropertyName("archive")] public string Archive { get; init; } = "active";
    [JsonPropertyName("origin")] public string Origin { get; init; } = "all";
    [JsonPropertyName("project")] public string? Project { get; init; }
}

public sealed record MemorySearchChatLabel(
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("archived")] bool Archived);

public sealed class MemorySearchPageResponse
{
    [JsonPropertyName("request")] public MemorySearchPageRequest? Request { get; set; }
    [JsonPropertyName("query")] public string Query { get; set; } = "";
    [JsonPropertyName("supportsSearch")] public bool SupportsSearch { get; set; }
    [JsonPropertyName("supportsHistory")] public bool SupportsHistory { get; set; }
    [JsonPropertyName("freshness")] public string Freshness { get; set; } = "";
    [JsonPropertyName("hasMore")] public bool HasMore { get; set; }
    [JsonPropertyName("hits")] public List<RememberSearchHitBody> Hits { get; set; } = [];
    [JsonPropertyName("chats")] public Dictionary<string, MemorySearchChatLabel> Chats { get; set; } = new(StringComparer.Ordinal);
}

public sealed record MemoryHistoryPageRequest
{
    [JsonPropertyName("version")] public int Version { get; init; } = 1;
    [JsonPropertyName("stateId")] public string StateId { get; init; } = "";
    [JsonPropertyName("chapters")] public bool Chapters { get; init; }
    [JsonPropertyName("messageId")] public string? MessageId { get; init; }
    [JsonPropertyName("offset")] public int Offset { get; init; }
    [JsonPropertyName("limit")] public int Limit { get; init; } = 20;
    [JsonPropertyName("contentOffset")] public int ContentOffset { get; init; }
    [JsonPropertyName("maxChars")] public int MaxChars { get; init; } = 4000;
}

public sealed record MemoryHistoryMessage(
    [property: JsonPropertyName("messageId")] string MessageId,
    [property: JsonPropertyName("position")] long Position,
    [property: JsonPropertyName("role")] string Role,
    [property: JsonPropertyName("content")] string Content,
    [property: JsonPropertyName("contentOffset")] long ContentOffset,
    [property: JsonPropertyName("contentLength")] long ContentLength,
    [property: JsonPropertyName("hasMoreContent")] bool HasMoreContent,
    [property: JsonPropertyName("hasParts")] bool HasParts,
    [property: JsonPropertyName("hasAttachments")] bool HasAttachments);

public sealed record MemoryHistoryPage(
    [property: JsonPropertyName("stateId")] string StateId,
    [property: JsonPropertyName("revision")] long Revision,
    [property: JsonPropertyName("store")] string Store,
    [property: JsonPropertyName("offset")] int Offset,
    [property: JsonPropertyName("limit")] int Limit,
    [property: JsonPropertyName("hasMore")] bool HasMore,
    [property: JsonPropertyName("messages")] IReadOnlyList<MemoryHistoryMessage> Messages);

public sealed class MemoryHistoryPageResponse
{
    [JsonPropertyName("request")] public MemoryHistoryPageRequest? Request { get; set; }
    [JsonPropertyName("data")] public MemoryHistoryPage? Data { get; set; }
}
