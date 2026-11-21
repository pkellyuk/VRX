using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

/// <summary>
/// Status of an inbox item.
/// </summary>
public enum InboxItemStatus
{
    Unread,
    Read,
    Archived
}

/// <summary>
/// A single inbox alert — "something happened, check this human". Posted by the AI
/// (typically via a question broadcast) from any conversation, surfaced in one place
/// for the human to monitor. Backed by the Inbox table in config.db.
/// </summary>
public class InboxItem
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = Guid.NewGuid().ToString();

    /// <summary>
    /// The conversation (stateId) this alert originated from, so the human can open it
    /// to dig in. For a watch alert this is the watch's child session.
    /// </summary>
    [JsonPropertyName("sourceSessionId")]
    public string? SourceSessionId { get; set; }

    /// <summary>
    /// The calendar watch (CalendarTask id) that produced this alert, if any.
    /// </summary>
    [JsonPropertyName("sourceTaskId")]
    public string? SourceTaskId { get; set; }

    /// <summary>
    /// Optional short title/heading for the alert.
    /// </summary>
    [JsonPropertyName("title")]
    public string? Title { get; set; }

    /// <summary>
    /// The alert body — what happened and what the human should look at.
    /// </summary>
    [JsonPropertyName("message")]
    public string Message { get; set; } = string.Empty;

    /// <summary>
    /// Optional choices carried from a question broadcast (the option labels).
    /// </summary>
    [JsonPropertyName("options")]
    public List<string> Options { get; set; } = new();

    [JsonPropertyName("status")]
    public InboxItemStatus Status { get; set; } = InboxItemStatus.Unread;

    [JsonPropertyName("createdAt")]
    public DateTime CreatedAt { get; set; } = DateTime.UtcNow;

    [JsonPropertyName("readAt")]
    public DateTime? ReadAt { get; set; }
}
