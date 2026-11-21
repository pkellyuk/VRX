using System.Text.Json;
using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

public enum GoalStatus
{
    Backlog = 0,
    Active = 1,
    Completed = 2,
    Blocked = 3,
    Cancelled = 4,

    /// <summary>
    /// The model has finished and the goal waits for the human's one review. Appended so
    /// the integers already stored keep their meaning. Not terminal: a review can send it back.
    /// </summary>
    AwaitingReview = 5
}

public sealed class SessionOpenBody
{
    [JsonPropertyName("mode")]
    public string Mode { get; set; } = "interactive";

    [JsonPropertyName("title")]
    public string? Title { get; set; }
}

public sealed class SessionOpenedBody
{
    [JsonPropertyName("accepted")]
    public bool Accepted { get; set; } = true;

    [JsonPropertyName("message")]
    public string Message { get; set; } = string.Empty;
}

public sealed class SessionInputBody
{
    public const string KindText = "text";
    public const string KindConversationGet = "conversation_get";
    public const string KindConversationSend = "conversation_send";
    public const string KindConversationSendStreaming = "conversation_send_streaming";
    public const string KindConversationConfigure = "conversation_configure";
    public const string KindConversationClear = "conversation_clear";

    /// <summary>
    /// Asks the device that owns a conversation for the bytes behind one of its
    /// attachments. A transcript carries only the metadata - it is re-fetched on
    /// every refresh, and shipping a screenshot each time would be absurd - so
    /// the payload is requested once, by id, when something actually needs it.
    /// </summary>
    public const string KindAttachmentGet = "attachment_get";

    /// <summary>
    /// Asks the device that ran a conversation's last turn to predict the user's next message
    /// (SUGGESTREPLY.md). Read-only: it takes no turn lease and saves nothing. The answer is a
    /// <see cref="ReplySuggestionBody"/> as JSON.
    /// </summary>
    public const string KindConversationSuggestReply = "conversation_suggest_reply";

    [JsonPropertyName("kind")]
    public string Kind { get; set; } = KindText;

    [JsonPropertyName("text")]
    public string? Text { get; set; }

    [JsonPropertyName("stateId")]
    public string? StateId { get; set; }

    /// <summary>The attachment wanted by <see cref="KindAttachmentGet"/>.</summary>
    [JsonPropertyName("attachmentId")]
    public string? AttachmentId { get; set; }

    [JsonPropertyName("displayName")]
    public string? DisplayName { get; set; }

    [JsonPropertyName("fallbackWorkingDirectory")]
    public string? FallbackWorkingDirectory { get; set; }

    [JsonPropertyName("userMessage")]
    public string? UserMessage { get; set; }

    [JsonPropertyName("systemPrompt")]
    public string? SystemPrompt { get; set; }

    /// <summary>
    /// Files attached to <see cref="UserMessage"/>. Payloads travel inline; staged ones
    /// are written to disk by the receiving device, which is the only machine whose
    /// paths the agent's file tools can actually open.
    /// </summary>
    [JsonPropertyName("attachments")]
    public List<ChatAttachment>? Attachments { get; set; }

    /// <summary>Opt-in bounded attachment send. Null preserves the existing send behavior.</summary>
    [JsonPropertyName("attachmentProtocolVersion")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public int? AttachmentProtocolVersion { get; set; }

    /// <summary>Negotiated prompt close/cancellation support. Null preserves legacy prompt behavior.</summary>
    [JsonPropertyName("promptProtocolVersion")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public int? PromptProtocolVersion { get; set; }

    [JsonPropertyName("useRolesEnabled")]
    public bool? UseRolesEnabled { get; set; }

    [JsonPropertyName("showThinkingEnabled")]
    public bool? ShowThinkingEnabled { get; set; }

    [JsonPropertyName("showToolCallsEnabled")]
    public bool? ShowToolCallsEnabled { get; set; }

    [JsonPropertyName("collapseThinkingEnabled")]
    public bool? CollapseThinkingEnabled { get; set; }

    [JsonPropertyName("collapseToolCallsEnabled")]
    public bool? CollapseToolCallsEnabled { get; set; }

    [JsonPropertyName("selectedProviderKey")]
    public string? SelectedProviderKey { get; set; }

    [JsonPropertyName("selectedModelId")]
    public string? SelectedModelId { get; set; }

    /// <summary>
    /// Per-chat thinking level. An empty string clears the override back to whatever the
    /// answering device's provider is configured with; null leaves it untouched.
    /// </summary>
    [JsonPropertyName("reasoningEffort")]
    public string? ReasoningEffort { get; set; }

    /// <summary>
    /// Per-chat reply length (<c>terse</c>, <c>normal</c> or <c>verbose</c>). An empty string or
    /// <c>normal</c> clears it; null leaves it untouched.
    /// </summary>
    [JsonPropertyName("verbosity")]
    public string? Verbosity { get; set; }

    /// <summary>
    /// When set for <c>KindConversationGet</c>, the server returns only the latest <c>MaxMessages</c> messages
    /// and signals via <see cref="RemoteConversationTranscript.HasMore"/> whether older messages exist.
    /// </summary>
    [JsonPropertyName("maxMessages")]
    public int? MaxMessages { get; set; }

    /// <summary>
    /// For <c>KindConversationSendStreaming</c>: run these roles against the conversation as it
    /// stands, instead of a turn ("Run roles now" in Chat Controls). No user message is added;
    /// the roles run whatever their invocation setting, and their feedback is acted on as after
    /// any turn. An empty list means every enabled role. Older hosts ignore it and reject the
    /// empty message, which the sender reports.
    /// </summary>
    public List<string>? RunRoles { get; set; }
}

/// <summary>The answer to <see cref="SessionInputBody.KindConversationSuggestReply"/>.</summary>
public sealed class ReplySuggestionBody
{
    /// <summary>The predicted next message, or null when there is none.</summary>
    [JsonPropertyName("suggestion")]
    public string? Suggestion { get; set; }

    /// <summary>Why there is no suggestion, for the log. Null when there is one.</summary>
    [JsonPropertyName("skippedReason")]
    public string? SkippedReason { get; set; }

    public static ReplySuggestionBody Skipped(string reason) => new() { SkippedReason = reason };
}

public sealed class SessionEventBody
{
    [JsonPropertyName("kind")]
    public string Kind { get; set; } = "output";

    [JsonPropertyName("stream")]
    public string? Stream { get; set; }

    [JsonPropertyName("chunk")]
    public string? Chunk { get; set; }

    [JsonPropertyName("seq")]
    public int Seq { get; set; }

    /// Per-stream monotonic counter for text deltas only. Independent of the
    /// global per-device Seq so that interleaved non-text envelopes (permission
    /// requests, status, structured stream events) do not introduce phantom
    /// gaps in the receiver's text reorder buffer. Defaults to -1 ("absent")
    /// for non-text envelopes and for legacy senders that don't emit it.
    [JsonPropertyName("textSeq")]
    public int TextSeq { get; set; } = -1;
}

public sealed class SessionCancelBody
{
    [JsonPropertyName("requestId")]
    public string RequestId { get; set; } = string.Empty;
}

public sealed class TreeSnapshotBody
{
    [JsonPropertyName("sequence")]
    public long Sequence { get; set; }

    [JsonPropertyName("snapshot")]
    public RemoteTreeSnapshot Snapshot { get; set; } = new();
}

public sealed class ConversationUpdatedBody
{
    [JsonPropertyName("revision")]
    public long Revision { get; set; }

    [JsonPropertyName("stateId")]
    public string StateId { get; set; } = string.Empty;

    [JsonPropertyName("displayName")]
    public string DisplayName { get; set; } = string.Empty;

    [JsonPropertyName("lastSavedAt")]
    public string LastSavedAt { get; set; } = string.Empty;

    [JsonPropertyName("messages")]
    public List<Core.Interfaces.Message> Messages { get; set; } = new();

    [JsonPropertyName("isProcessing")]
    public bool? IsProcessing { get; set; }

    [JsonPropertyName("statusText")]
    public string? StatusText { get; set; }

    [JsonPropertyName("contextUsage")]
    public string? ContextUsage { get; set; }

    [JsonPropertyName("streamingMessageId")]
    public string? StreamingMessageId { get; set; }

    [JsonPropertyName("streamingRole")]
    public string? StreamingRole { get; set; }

    [JsonPropertyName("streamingContent")]
    public string? StreamingContent { get; set; }

    [JsonPropertyName("streamingDelta")]
    public string? StreamingDelta { get; set; }

    [JsonPropertyName("isStreamingFinal")]
    public bool? IsStreamingFinal { get; set; }

    [JsonPropertyName("userMessage")]
    public string? UserMessage { get; set; }

    [JsonPropertyName("streamEvent")]
    public RemoteConversationStreamEvent? StreamEvent { get; set; }
}

// Tree operation body classes

public sealed class ProjectAddBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("path")]
    public string Path { get; set; } = string.Empty;

    [JsonPropertyName("name")]
    public string Name { get; set; } = string.Empty;
}

public sealed class ProjectRemoveBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;
}

public sealed class ProjectRenameBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("newName")]
    public string NewName { get; set; } = string.Empty;
}

public sealed class ProjectCloneBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("newName")]
    public string NewName { get; set; } = string.Empty;
}

public sealed class ThreadAddBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("name")]
    public string Name { get; set; } = string.Empty;
}

public sealed class ThreadRemoveBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("threadId")]
    public string ThreadId { get; set; } = string.Empty;
}

public sealed class ThreadRenameBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("threadId")]
    public string ThreadId { get; set; } = string.Empty;

    [JsonPropertyName("newName")]
    public string NewName { get; set; } = string.Empty;
}

public sealed class ThreadCloneBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("threadId")]
    public string ThreadId { get; set; } = string.Empty;

    [JsonPropertyName("newName")]
    public string NewName { get; set; } = string.Empty;
}

public sealed class ThreadSetAutomationBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("threadId")]
    public string ThreadId { get; set; } = string.Empty;

    [JsonPropertyName("enabled")]
    public bool Enabled { get; set; }
}

public sealed class ConversationCompactBody
{
    [JsonPropertyName("stateId")]
    public string StateId { get; set; } = string.Empty;
}

public sealed class ConversationStopBody
{
    [JsonPropertyName("stateId")]
    public string StateId { get; set; } = string.Empty;
}

public sealed class ConversationSetPermissionModeBody
{
    [JsonPropertyName("stateId")]
    public string StateId { get; set; } = string.Empty;

    [JsonPropertyName("fullPermissionMode")]
    public bool FullPermissionMode { get; set; }
}

public sealed class RemotePromptClosedBody
{
    [JsonPropertyName("version")]
    public int Version { get; set; } = 1;
    [JsonPropertyName("requestId")]
    public string RequestId { get; set; } = string.Empty;
    [JsonPropertyName("promptKind")]
    public string PromptKind { get; set; } = string.Empty;
}

public sealed class RemotePermissionRequestBody
{
    [JsonPropertyName("requestId")]
    public string RequestId { get; set; } = string.Empty;

    [JsonPropertyName("request")]
    public Automaton.Core.Interfaces.PermissionRequest Request { get; set; } = new();
}

/// <summary>
/// A question a remote turn needs answered, forwarded to the device that is
/// driving the conversation.
///
/// Questions used to have no wire form at all: the host answered them on its
/// own screen, so a chat driven from anywhere else waited on a dialog nobody
/// was looking at. This is the permission round-trip applied to questions.
/// </summary>
public sealed class RemoteQuestionRequestBody
{
    [JsonPropertyName("requestId")]
    public string RequestId { get; set; } = string.Empty;

    [JsonPropertyName("request")]
    public Automaton.Core.Interfaces.QuestionRequest Request { get; set; } = new();
}

public sealed class RemoteQuestionResponseBody
{
    [JsonPropertyName("requestId")]
    public string RequestId { get; set; } = string.Empty;

    [JsonPropertyName("response")]
    public Automaton.Core.Interfaces.QuestionResponse Response { get; set; } = new();
}

public sealed class RemotePermissionResponseBody
{
    [JsonPropertyName("requestId")]
    public string RequestId { get; set; } = string.Empty;

    [JsonPropertyName("granted")]
    public bool Granted { get; set; }
}

public sealed class OperationResultBody
{
    [JsonPropertyName("success")]
    public bool Success { get; set; }

    [JsonPropertyName("message")]
    public string Message { get; set; } = string.Empty;

    [JsonPropertyName("data")]
    public string? Data { get; set; }
}

public sealed class GoalAddBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("goalId")]
    public string GoalId { get; set; } = string.Empty;

    [JsonPropertyName("description")]
    public string Description { get; set; } = string.Empty;

    [JsonPropertyName("workingFolder")]
    public string? WorkingFolder { get; set; }

    [JsonPropertyName("successCriteria")]
    public List<string> SuccessCriteria { get; set; } = new();

    [JsonPropertyName("milestones")]
    public List<GoalMilestoneBody> Milestones { get; set; } = new();

    [JsonPropertyName("plan")]
    public List<string> Plan { get; set; } = new();

    [JsonPropertyName("downloadedDocs")]
    public List<GoalDownloadedDocBody> DownloadedDocs { get; set; } = new();
}

public sealed class GoalDownloadedDocBody
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("title")]
    public string Title { get; set; } = string.Empty;

    [JsonPropertyName("sourceUrl")]
    public string SourceUrl { get; set; } = string.Empty;

    [JsonPropertyName("relativePath")]
    public string RelativePath { get; set; } = string.Empty;

    [JsonPropertyName("contentType")]
    public string? ContentType { get; set; }

    [JsonPropertyName("summary")]
    public string? Summary { get; set; }

    [JsonPropertyName("fetchedAt")]
    public string FetchedAt { get; set; } = string.Empty;
}

public sealed class GoalMilestoneBody
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("description")]
    public string Description { get; set; } = string.Empty;

    [JsonPropertyName("criteria")]
    public List<string> Criteria { get; set; } = new();

    [JsonPropertyName("dependsOn")]
    public List<string> DependsOn { get; set; } = new();
}

public sealed class GoalUpdateBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("goalId")]
    public string GoalId { get; set; } = string.Empty;

    [JsonPropertyName("description")]
    public string? Description { get; set; }

    [JsonPropertyName("workingFolder")]
    public string? WorkingFolder { get; set; }

    [JsonPropertyName("successCriteria")]
    public List<string>? SuccessCriteria { get; set; }

    [JsonPropertyName("milestones")]
    public List<GoalMilestoneBody>? Milestones { get; set; }

    [JsonPropertyName("plan")]
    public List<string>? Plan { get; set; }

    [JsonPropertyName("downloadedDocs")]
    public List<GoalDownloadedDocBody>? DownloadedDocs { get; set; }

    [JsonPropertyName("status")]
    public string? Status { get; set; }
}

public sealed class GoalDeleteBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("goalId")]
    public string GoalId { get; set; } = string.Empty;
}

public sealed class GoalMoveBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("goalId")]
    public string GoalId { get; set; } = string.Empty;

    [JsonPropertyName("newStatus")]
    public string NewStatus { get; set; } = string.Empty;
}

public sealed class GoalSnapshotBody
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("goals")]
    public List<ProjectGoalBody> Goals { get; set; } = new();
}

public sealed class ProjectGoalBody
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("description")]
    public string Description { get; set; } = string.Empty;

    [JsonPropertyName("workingFolder")]
    public string? WorkingFolder { get; set; }

    [JsonPropertyName("status")]
    public string Status { get; set; } = string.Empty;

    [JsonPropertyName("successCriteria")]
    public List<string> SuccessCriteria { get; set; } = new();

    [JsonPropertyName("milestones")]
    public List<GoalMilestoneBody> Milestones { get; set; } = new();

    [JsonPropertyName("plan")]
    public List<string> Plan { get; set; } = new();

    [JsonPropertyName("downloadedDocs")]
    public List<GoalDownloadedDocBody> DownloadedDocs { get; set; } = new();

    [JsonPropertyName("createdAt")]
    public string CreatedAt { get; set; } = string.Empty;

    [JsonPropertyName("updatedAt")]
    public string UpdatedAt { get; set; } = string.Empty;
}

// Screen viewing body classes

public sealed class ScreenViewRequestBody
{
    [JsonPropertyName("width")]
    public int Width { get; set; }

    [JsonPropertyName("height")]
    public int Height { get; set; }

    [JsonPropertyName("compressQuality")]
    public int CompressQuality { get; set; } = 75;
}

public sealed class ScreenViewAcceptBody
{
    [JsonPropertyName("width")]
    public int Width { get; set; }

    [JsonPropertyName("height")]
    public int Height { get; set; }

    [JsonPropertyName("pixelFormat")]
    public string PixelFormat { get; set; } = "RGB32";
}

public sealed class ScreenViewRejectBody
{
    [JsonPropertyName("reason")]
    public string Reason { get; set; } = string.Empty;
}

public sealed class ScreenViewFrameBody
{
    [JsonPropertyName("seq")]
    public int Sequence { get; set; }

    [JsonPropertyName("timestampUtc")]
    public long TimestampUtc { get; set; }

    [JsonPropertyName("jpegLength")]
    public int JpegLength { get; set; }
}

public sealed class ScreenViewInputBody
{
    [JsonPropertyName("kind")]
    public string Kind { get; set; } = string.Empty;

    [JsonPropertyName("x")]
    public int X { get; set; }

    [JsonPropertyName("y")]
    public int Y { get; set; }

    [JsonPropertyName("buttons")]
    public int Buttons { get; set; }

    [JsonPropertyName("keysym")]
    public uint Keysym { get; set; }

    [JsonPropertyName("pressed")]
    public bool Pressed { get; set; }
}

public sealed class ProviderShareBody
{
    [JsonPropertyName("providerKey")]
    public string ProviderKey { get; set; } = string.Empty;

    [JsonPropertyName("providerConfig")]
    public ProviderConfig ProviderConfig { get; set; } = new();
}

public sealed class ProviderShareResultBody
{
    [JsonPropertyName("providerKey")]
    public string ProviderKey { get; set; } = string.Empty;

    [JsonPropertyName("success")]
    public bool Success { get; set; }

    [JsonPropertyName("message")]
    public string Message { get; set; } = string.Empty;

    [JsonPropertyName("deviceId")]
    public string DeviceId { get; set; } = string.Empty;

    [JsonPropertyName("deviceName")]
    public string DeviceName { get; set; } = string.Empty;
}

public sealed class InboxAlertBody
{
    [JsonPropertyName("item")]
    public InboxItem Item { get; set; } = new();
}

// Event argument classes for provider sharing
// Request/Response body classes
public sealed class GetTreeBody
{
    [JsonPropertyName("ifRevisionGreaterThan")]
    public long IfRevisionGreaterThan { get; set; }
}

public sealed class TreeResponseBody
{
    [JsonPropertyName("revision")]
    public long Revision { get; set; }

    [JsonPropertyName("snapshot")]
    public RemoteTreeSnapshot Snapshot { get; set; } = new();

    [JsonPropertyName("isEmpty")]
    public bool IsEmpty { get; set; }  // True when revision hasn't changed

    // Per-process identifier for the sending device's tree state. Changes whenever
    // the sender restarts so the receiver can detect a stale revision counter and
    // re-fetch a full snapshot.
    [JsonPropertyName("sessionId")]
    public string SessionId { get; set; } = string.Empty;
}

public sealed class GetConversationBody
{
    [JsonPropertyName("stateId")]
    public string StateId { get; set; } = string.Empty;

    [JsonPropertyName("ifRevisionGreaterThan")]
    public long IfRevisionGreaterThan { get; set; }
}

public sealed class ConversationResponseBody
{
    [JsonPropertyName("revision")]
    public long Revision { get; set; }

    [JsonPropertyName("stateId")]
    public string StateId { get; set; } = string.Empty;

    [JsonPropertyName("displayName")]
    public string DisplayName { get; set; } = string.Empty;

    [JsonPropertyName("lastSavedAt")]
    public string LastSavedAt { get; set; } = string.Empty;

    [JsonPropertyName("messages")]
    public List<Core.Interfaces.Message> Messages { get; set; } = new();

    [JsonPropertyName("isProcessing")]
    public bool? IsProcessing { get; set; }

    [JsonPropertyName("statusText")]
    public string? StatusText { get; set; }

    [JsonPropertyName("contextUsage")]
    public string? ContextUsage { get; set; }

    [JsonPropertyName("isEmpty")]
    public bool IsEmpty { get; set; }  // True when revision hasn't changed
}

public sealed class RememberSearchBody
{
    [JsonPropertyName("page")] public MemorySearchPageRequest? Page { get; set; }
    [JsonPropertyName("query")]
    public string Query { get; set; } = string.Empty;

    [JsonPropertyName("maxResults")]
    public int MaxResults { get; set; } = 10;

    [JsonPropertyName("source")]
    public string Source { get; set; } = "chats";

    /// <summary>Optional project path filter. Null/empty = no project filter (search whole index on the remote device).</summary>
    [JsonPropertyName("projectFilter")]
    public string? ProjectFilter { get; set; }

    /// <summary>True if the originator called <c>remember_global</c> (no project filter, ever).</summary>
    [JsonPropertyName("global")]
    public bool Global { get; set; }

    // ── The search dialog's search (Ctrl+Shift+F, "All devices") ──────────────
    // Optional, so an older peer ignores them and answers with its plain keyword search; the
    // dialog shows those hits without titles or a way to open them.

    /// <summary>
    /// Run the dialog's search - keywords over the chosen origins, optionally by meaning, within
    /// the dates - and reply with each hit's full detail and the conversations they belong to.
    /// </summary>
    [JsonPropertyName("detailed")]
    public bool Detailed { get; set; }

    /// <summary>With <see cref="Detailed"/>: merge in hits found by meaning, where the peer has an embedding model.</summary>
    [JsonPropertyName("semantic")]
    public bool Semantic { get; set; }

    /// <summary>With <see cref="Detailed"/>: the keyword origins to read ("raw", "compressed"); empty means none.</summary>
    [JsonPropertyName("origins")]
    public List<string>? Origins { get; set; }

    // The dates go as UTC DateTimes, never DateTimeOffsets: a DateTimeOffset is written as
    // "...+00:00", the envelope's re-write escapes that '+' as +, and the receiver's
    // checksum over the body text no longer matches, so the whole message is rejected.

    /// <summary>With <see cref="Detailed"/>: first day of conversation activity to include, in UTC.</summary>
    [JsonPropertyName("from")]
    public DateTime? From { get; set; }

    /// <summary>With <see cref="Detailed"/>: last day of conversation activity to include, in UTC.</summary>
    [JsonPropertyName("to")]
    public DateTime? To { get; set; }
}

public sealed class RememberSearchHitBody
{
    [JsonPropertyName("conversationId")]
    public string ConversationId { get; set; } = string.Empty;

    [JsonPropertyName("projectId")]
    public string? ProjectId { get; set; }

    [JsonPropertyName("role")]
    public string Role { get; set; } = string.Empty;

    [JsonPropertyName("snippet")]
    public string Snippet { get; set; } = string.Empty;

    [JsonPropertyName("rank")]
    public double Rank { get; set; }

    [JsonPropertyName("source")]
    public string Source { get; set; } = "chat";

    [JsonPropertyName("timestamp")]
    public string Timestamp { get; set; } = string.Empty;

    /// <summary>"raw" for a message as sent, "compressed" for a compaction summary. Absent from older peers, which reads as raw.</summary>
    [JsonPropertyName("origin")]
    public string Origin { get; set; } = "raw";

    // Set only in answer to a detailed search; see RememberSearchBody.Detailed.

    /// <summary>The stored ConversationState row the hit came from.</summary>
    [JsonPropertyName("stateId")]
    public string? StateId { get; set; }

    [JsonPropertyName("messageId")]
    public string? MessageId { get; set; }

    [JsonPropertyName("messageIndex")]
    public int MessageIndex { get; set; } = -1;

    /// <summary>The snippet with the search's highlight markers in place.</summary>
    [JsonPropertyName("markedSnippet")]
    public string? MarkedSnippet { get; set; }

    [JsonPropertyName("matchKind")]
    public string? MatchKind { get; set; }

    [JsonPropertyName("distance")]
    public double? Distance { get; set; }

    /// <summary>The owning device's tag on this memory, 0..10, or null when untagged.</summary>
    [JsonPropertyName("importance")]
    public int? Importance { get; set; }
}

/// <summary>
/// One conversation a detailed search's hits belong to: how to label it, and where it sits in
/// the owning device's tree so the searcher can open it.
/// </summary>
public sealed class RememberSearchConversationBody
{
    [JsonPropertyName("sessionId")]
    public string SessionId { get; set; } = string.Empty;

    [JsonPropertyName("stateId")]
    public string StateId { get; set; } = string.Empty;

    [JsonPropertyName("title")]
    public string Title { get; set; } = string.Empty;

    /// <summary>ConversationState.ProjectId - usually a working directory, not a Projects row.</summary>
    [JsonPropertyName("projectId")]
    public string? ProjectId { get; set; }

    [JsonPropertyName("projectName")]
    public string? ProjectName { get; set; }

    [JsonPropertyName("updatedAt")]
    public string? UpdatedAt { get; set; }

    [JsonPropertyName("messageCount")]
    public int MessageCount { get; set; }

    /// <summary>True when the stored row is an archive left behind by clearing the chat.</summary>
    [JsonPropertyName("isArchived")]
    public bool IsArchived { get; set; }

    /// <summary>The chat in the owner's tree, when the conversation is one; null for the Director, dreams and cleared archives.</summary>
    [JsonPropertyName("chat")]
    public ChatLocation? Chat { get; set; }
}

/// <summary>A chat in its device's tree: what a search result needs to open it.</summary>
public sealed class ChatLocation
{
    [JsonPropertyName("threadId")]
    public string ThreadId { get; set; } = string.Empty;

    /// <summary>The Projects row the chat sits under.</summary>
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = string.Empty;

    [JsonPropertyName("name")]
    public string Name { get; set; } = string.Empty;

    [JsonPropertyName("projectName")]
    public string? ProjectName { get; set; }

    /// <summary>True when the chat is archived, and so out of every tree until restored.</summary>
    [JsonPropertyName("isArchived")]
    public bool IsArchived { get; set; }
}

public sealed class RememberSearchResponseBody
{
    [JsonPropertyName("page")] public MemorySearchPageResponse? Page { get; set; }
    [JsonPropertyName("hits")]
    public List<RememberSearchHitBody> Hits { get; set; } = new();

    [JsonPropertyName("totalMessagesIndexed")]
    public int TotalMessagesIndexed { get; set; }

    [JsonPropertyName("indexedConversations")]
    public int IndexedConversations { get; set; }

    /// <summary>In answer to a detailed search: the conversations the hits belong to. Null from an older peer.</summary>
    [JsonPropertyName("conversations")]
    public List<RememberSearchConversationBody>? Conversations { get; set; }

    [JsonPropertyName("error")]
    public string? Error { get; set; }
}

public sealed class RememberGetBody
{
    [JsonPropertyName("page")] public MemoryHistoryPageRequest? Page { get; set; }
    [JsonPropertyName("conversationId")]
    public string ConversationId { get; set; } = string.Empty;

    [JsonPropertyName("maxMessages")]
    public int MaxMessages { get; set; } = 100;

    /// <summary>
    /// Reply with the whole transcript as the search dialog reads it (every message, ids,
    /// summaries) in <see cref="RememberGetResponseBody.Transcript"/>. An older peer ignores
    /// this and sends its plain message list.
    /// </summary>
    [JsonPropertyName("transcript")]
    public bool Transcript { get; set; }

    /// <summary>With <see cref="Transcript"/>: text from the hit, to pick the right stored row when a session has several.</summary>
    [JsonPropertyName("probe")]
    public string? Probe { get; set; }
}

public sealed class RememberGetMessageBody
{
    [JsonPropertyName("role")]
    public string Role { get; set; } = string.Empty;

    [JsonPropertyName("content")]
    public string Content { get; set; } = string.Empty;
}

public sealed class RememberGetResponseBody
{
    [JsonPropertyName("page")] public MemoryHistoryPageResponse? Page { get; set; }
    [JsonPropertyName("found")]
    public bool Found { get; set; }

    [JsonPropertyName("conversationId")]
    public string ConversationId { get; set; } = string.Empty;

    [JsonPropertyName("sessionId")]
    public string SessionId { get; set; } = string.Empty;

    [JsonPropertyName("projectId")]
    public string? ProjectId { get; set; }

    [JsonPropertyName("totalMessages")]
    public int TotalMessages { get; set; }

    [JsonPropertyName("lastSavedAt")]
    public string? LastSavedAt { get; set; }

    [JsonPropertyName("messages")]
    public List<RememberGetMessageBody> Messages { get; set; } = new();

    /// <summary>In answer to <see cref="RememberGetBody.Transcript"/>: the stored conversation as the search dialog shows it.</summary>
    [JsonPropertyName("transcript")]
    public Services.ConversationTranscript? Transcript { get; set; }

    [JsonPropertyName("error")]
    public string? Error { get; set; }
}

public sealed class ToolInvokeBody
{
    [JsonPropertyName("toolId")]
    public string ToolId { get; set; } = string.Empty;

    /// <summary>The tool's parameters as a raw JSON object string, exactly as the local tool would receive them (minus the deviceId routing field).</summary>
    [JsonPropertyName("argsJson")]
    public string ArgsJson { get; set; } = "{}";
}

public sealed class ToolInvokeResponseBody
{
    /// <summary>The remote <see cref="Automaton.Core.Interfaces.ToolResult"/> serialized as JSON. Null when <see cref="Error"/> is set.</summary>
    [JsonPropertyName("resultJson")]
    public string? ResultJson { get; set; }

    [JsonPropertyName("error")]
    public string? Error { get; set; }
}

public sealed class ProviderShareEventArgs : EventArgs
{
    public string ProviderKey { get; init; } = string.Empty;
    public string ProviderName { get; init; } = string.Empty;
    public int RecipientCount { get; init; }
}

public sealed class ProviderShareReceivedEventArgs : EventArgs
{
    public string SourceDeviceId { get; init; } = string.Empty;
    public string SourceDeviceName { get; init; } = string.Empty;
    public string ProviderKey { get; init; } = string.Empty;
    public string ProviderName { get; init; } = string.Empty;
    public bool WasImported { get; init; }
    public string Message { get; init; } = string.Empty;
}

/// <summary>
/// Raised when a remote device requests tool permission during a remote conversation.
/// The handler should show a permission dialog and call
/// RemoteSessionService.SendRemotePermissionResponse with the result.
/// </summary>
/// <summary>
/// Raised when a remote device needs a question answered during a remote
/// conversation. The handler should show the question and call
/// RemoteSessionService.SendRemoteQuestionResponse with the answer.
/// </summary>
public sealed class RemoteQuestionRequestEventArgs : EventArgs
{
    public string SourceDeviceId { get; init; } = string.Empty;
    public SessionEventBody Body { get; init; } = new();
}

public sealed class RemotePermissionRequestEventArgs : EventArgs
{
    public string SourceDeviceId { get; init; } = string.Empty;
    public SessionEventBody Body { get; init; } = new();
}
