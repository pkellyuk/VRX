using System.Text.Json;
using System.Text.Json.Serialization;

namespace Automaton.Core.Interfaces;

/// <summary>
/// Represents a tool that can be executed by an agent.
/// </summary>
public interface ITool
{
    /// <summary>
    /// Unique identifier for the tool.
    /// </summary>
    string Id { get; }

    /// <summary>
    /// Human-readable description of what the tool does.
    /// </summary>
    string Description { get; }

    /// <summary>
    /// JSON schema describing the tool's input parameters.
    /// </summary>
    JsonSchema Parameters { get; }

    /// <summary>
    /// Executes the tool with the given arguments.
    /// </summary>
    /// <param name="args">Tool arguments as JSON string.</param>
    /// <param name="context">Execution context containing session information.</param>
    /// <returns>Tool execution result.</returns>
    Task<ToolResult> ExecuteAsync(string args, ToolContext context);
}

/// <summary>
/// JSON schema definition for tool parameters.
/// </summary>
public class JsonSchema
{
    /// <summary>
    /// Human-readable description of the tool this schema belongs to. Populated so
    /// providers can forward the tool description to the model alongside the schema.
    /// </summary>
    [JsonPropertyName("description")]
    public string? Description { get; set; }

    /// <summary>
    /// Optional verbatim JSON schema. When set, providers should send this to the model
    /// instead of the simplified typed representation below. Used for externally supplied
    /// schemas (e.g. MCP tools) that use JSON-schema features the typed model can't express.
    /// </summary>
    [JsonPropertyName("rawSchema")]
    public JsonElement? RawSchema { get; set; }

    [JsonPropertyName("type")]
    public string Type { get; set; } = "object";

    [JsonPropertyName("properties")]
    public Dictionary<string, JsonSchemaProperty> Properties { get; set; } = new();

    [JsonPropertyName("required")]
    public List<string> Required { get; set; } = new();

    [JsonPropertyName("additionalProperties")]
    public bool AdditionalProperties { get; set; } = false;
}

/// <summary>
/// JSON schema property definition.
/// </summary>
public class JsonSchemaProperty
{
    [JsonPropertyName("type")]
    public string Type { get; set; } = "string";

    [JsonPropertyName("description")]
    public string Description { get; set; } = string.Empty;

    [JsonPropertyName("enum")]
    public List<string>? Enum { get; set; }

    [JsonPropertyName("items")]
    public JsonSchema? Items { get; set; }

    [JsonPropertyName("minimum")]
    public double? Minimum { get; set; }

    [JsonPropertyName("maximum")]
    public double? Maximum { get; set; }
}

/// <summary>
/// Result of tool execution.
/// </summary>
public class ToolResult
{
    [JsonPropertyName("title")]
    public string Title { get; set; } = string.Empty;

    [JsonPropertyName("output")]
    public string Output { get; set; } = string.Empty;

    [JsonPropertyName("metadata")]
    public Dictionary<string, object> Metadata { get; set; } = new();

    [JsonPropertyName("attachments")]
    public List<ToolAttachment> Attachments { get; set; } = new();
}

/// <summary>
/// File attachment from tool execution.
/// </summary>
public class ToolAttachment
{
    [JsonPropertyName("filename")]
    public string Filename { get; set; } = string.Empty;

    [JsonPropertyName("content")]
    public string Content { get; set; } = string.Empty;

    [JsonPropertyName("contentType")]
    public string ContentType { get; set; } = "text/plain";
}

/// <summary>
/// Context provided to tools during execution.
/// </summary>
public class ToolContext
{
    /// <summary>Copies the context's scope/callback references with an independent operation token.</summary>
    public ToolContext WithCancellation(CancellationToken token)
    { var copy = (ToolContext)MemberwiseClone(); copy.CancellationToken = token; return copy; }

    [JsonPropertyName("sessionId")]
    public string SessionId { get; set; } = string.Empty;

    [JsonPropertyName("messageId")]
    public string MessageId { get; set; } = string.Empty;

    [JsonPropertyName("agent")]
    public string Agent { get; set; } = string.Empty;

    [JsonPropertyName("callId")]
    public string? CallId { get; set; }

    [JsonPropertyName("directory")]
    public string Directory { get; set; } = string.Empty;

    [JsonPropertyName("worktree")]
    public string Worktree { get; set; } = string.Empty;

    /// <summary>
    /// Currently selected project ID (from UI treeview selection).
    /// Null if workspace chat is active.
    /// </summary>
    [JsonPropertyName("projectId")]
    public string? ProjectId { get; set; }

    /// <summary>
    /// Currently selected project name (from UI treeview selection).
    /// Null if workspace chat is active.
    /// </summary>
    [JsonPropertyName("projectName")]
    public string? ProjectName { get; set; }

    /// <summary>
    /// Currently selected project folder path (from UI treeview selection).
    /// Null if workspace chat is active.
    /// </summary>
    [JsonPropertyName("projectPath")]
    public string? ProjectPath { get; set; }

    /// <summary>
    /// The calendar watch (CalendarTask id) this turn is executing for, if any.
    /// Set by the calendar scheduler when running a watch's headless check so that
    /// inbox alerts raised during the turn are attributed back to the watch.
    /// Null for ordinary interactive turns.
    /// </summary>
    [JsonPropertyName("calendarTaskId")]
    public string? CalendarTaskId { get; set; }

    /// <summary>
    /// The chat's reply length, <c>terse</c> or <c>verbose</c>; null is normal. Turned into one
    /// instruction on each request by <c>VerbosityContextProvider</c>.
    /// </summary>
    [JsonPropertyName("responseVerbosity")]
    public string? ResponseVerbosity { get; set; }

    /// <summary>
    /// The id the chat is saved under - what a tool needs to send something back into this chat
    /// later (a background worker's result). Null where a turn is not run for a saved chat.
    /// </summary>
    [JsonPropertyName("conversationStateId")]
    public string? ConversationStateId { get; set; }

    /// <summary>The chat's model is on a provider marked as a cloud model (ProviderConfig.IsCloud).</summary>
    [JsonPropertyName("isCloudModel")]
    public bool IsCloudModel { get; set; }

    [JsonPropertyName("abort")]
    [JsonIgnore]
    public CancellationToken CancellationToken { get; set; }

    [JsonPropertyName("messages")]
    public List<Message> Messages { get; set; } = new();

    [JsonIgnore]
    public Action<string, Dictionary<string, object>>? MetadataCallback { get; set; }

    [JsonIgnore]
    public Func<PermissionRequest, Task>? AskPermissionCallback { get; set; }

    /// <summary>Optional host prompt with a nested operation's cancellation lifetime.</summary>
    [JsonIgnore]
    public Func<PermissionRequest, CancellationToken, Task>? AskPermissionWithCancellationCallback { get; set; }

    /// <summary>
    /// The ids of the tools this conversation is actually being offered, set by the turn
    /// processor from the schemas it sends. A chat's tool list is fixed when the chat is
    /// created, so a tool can use this to avoid pointing the model at a tool it does not have.
    /// Null when nobody has said.
    /// </summary>
    [System.Text.Json.Serialization.JsonIgnore]
    public IReadOnlyCollection<string>? AvailableToolIds { get; set; }

    [JsonIgnore]
    public Func<QuestionRequest, Task<QuestionResponse>>? AskQuestionCallback { get; set; }

    /// <summary>Optional host question with a nested operation's cancellation lifetime.</summary>
    [JsonIgnore]
    public Func<QuestionRequest, CancellationToken, Task<QuestionResponse>>? AskQuestionWithCancellationCallback { get; set; }

    [JsonIgnore]
    public Action<UIMessage>? UIMessageCallback { get; set; }

    /// <summary>
    /// A one-line progress report from a long-running tool ("Making the song 45%"). Set by the
    /// turn processor only while a tool runs, and carried to the chat's status line as a
    /// <c>tool_progress</c> stream event - in a headless host as well as the desktop, which
    /// <see cref="UIMessageCallback"/> is not. Never added to the conversation.
    /// </summary>
    [JsonIgnore]
    public Action<string>? ProgressCallback { get; set; }
}

/// <summary>
/// Message in a conversation.
/// </summary>
public class Message
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("role")]
    public string Role { get; set; } = string.Empty; // "user", "assistant", "system", "tool"

    [JsonPropertyName("content")]
    public string Content { get; set; } = string.Empty;

    [JsonPropertyName("parts")]
    public List<MessagePart> Parts { get; set; } = new();

    /// <summary>
    /// Files the user attached to this message. Images become inline image content
    /// parts at provider time; text and staged attachments are folded into
    /// <see cref="Content"/> before the message is ever sent.
    /// </summary>
    [JsonPropertyName("attachments")]
    public List<Models.ChatAttachment> Attachments { get; set; } = new();
}

/// <summary>
/// Part of a message (text, tool call, tool result, reasoning, etc.)
/// </summary>
public class MessagePart
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("type")]
    public string Type { get; set; } = string.Empty; // "text", "tool", "reasoning", "step-start", "step-finish", "patch"

    [JsonPropertyName("text")]
    public string? Text { get; set; }

    [JsonPropertyName("tool")]
    public string? Tool { get; set; }

    [JsonPropertyName("callId")]
    public string? CallId { get; set; }

    [JsonPropertyName("state")]
    public ToolCallState? State { get; set; }

    [JsonPropertyName("metadata")]
    public Dictionary<string, object> Metadata { get; set; } = new();
}

/// <summary>
/// State of a tool call.
/// </summary>
public class ToolCallState
{
    [JsonPropertyName("status")]
    public string Status { get; set; } = "pending"; // "pending", "running", "completed", "error"

    [JsonPropertyName("input")]
    public Dictionary<string, object> Input { get; set; } = new();

    [JsonPropertyName("output")]
    public string? Output { get; set; }

    [JsonPropertyName("error")]
    public string? Error { get; set; }

    [JsonPropertyName("title")]
    public string? Title { get; set; }

    [JsonPropertyName("time")]
    public ToolCallTime? Time { get; set; }
}

/// <summary>
/// Timing information for tool execution.
/// </summary>
public class ToolCallTime
{
    [JsonPropertyName("start")]
    public long Start { get; set; }

    [JsonPropertyName("end")]
    public long? End { get; set; }
}

/// <summary>
/// Permission request for accessing resources or executing actions.
/// </summary>
public class PermissionRequest
{
    [JsonPropertyName("permission")]
    public string Permission { get; set; } = string.Empty; // "bash", "edit", "read", "external_directory", etc.

    [JsonPropertyName("patterns")]
    public List<string> Patterns { get; set; } = new();

    [JsonPropertyName("always")]
    public List<string> Always { get; set; } = new();

    [JsonPropertyName("metadata")]
    public Dictionary<string, object> Metadata { get; set; } = new();
}

public class QuestionRequest
{
    /// <summary>
    /// How long a blocking question waits for an answer before giving up.
    ///
    /// Without a bound the turn parks forever on an unanswered dialog, which is
    /// indistinguishable from a hang for anyone watching from another device.
    /// The inbox escalation is what reaches a person who is away from the screen,
    /// so the wait itself is kept short: an autonomous run that stalls for ten
    /// minutes on a question nobody is there to read has stopped being autonomous.
    /// </summary>
    public static readonly TimeSpan AnswerTimeout = TimeSpan.FromMinutes(5);

    /// <summary>
    /// How long to wait before posting an inbox alert about a question nobody
    /// has answered. Short enough to still be useful, long enough not to fire
    /// for a question answered at the screen a few seconds later.
    /// </summary>
    public static readonly TimeSpan EscalationDelay = TimeSpan.FromMinutes(2);

    [JsonPropertyName("question")]
    public string Question { get; set; } = string.Empty;

    [JsonPropertyName("options")]
    public List<QuestionChoice> Options { get; set; } = new();

    [JsonPropertyName("multiSelect")]
    public bool MultiSelect { get; set; }
}

public class QuestionChoice
{
    [JsonPropertyName("label")]
    public string Label { get; set; } = string.Empty;

    [JsonPropertyName("description")]
    public string Description { get; set; } = string.Empty;

    [JsonPropertyName("markdown")]
    public string? Markdown { get; set; }
}

public class QuestionResponse
{
    [JsonPropertyName("answer")]
    public string Answer { get; set; } = string.Empty;

    [JsonPropertyName("selectedOptions")]
    public List<string> SelectedOptions { get; set; } = new();

    /// <summary>
    /// True when nobody answered in time. Distinct from an empty answer, which
    /// is a person choosing to say nothing - the model should be told which of
    /// the two happened rather than being handed a blank either way.
    /// </summary>
    [JsonPropertyName("timedOut")]
    public bool TimedOut { get; set; }

    /// <summary>An answer nobody gave, for the waiters that ran out of time.</summary>
    public static QuestionResponse Unanswered() => new() { TimedOut = true };
}

/// <summary>
/// UI message for displaying in the chat interface.
/// </summary>
public class UIMessage
{
    [JsonPropertyName("type")]
    public UIMessageType Type { get; set; } = UIMessageType.Info;

    [JsonPropertyName("content")]
    public string Content { get; set; } = string.Empty;

    [JsonPropertyName("role")]
    public string? Role { get; set; }

    [JsonPropertyName("toolName")]
    public string? ToolName { get; set; }

    [JsonPropertyName("isError")]
    public bool IsError { get; set; }
}

/// <summary>
/// Type of UI message.
/// </summary>
public enum UIMessageType
{
    Info,
    UserMessage,
    AssistantMessage,
    SystemMessage,
    ToolMessage,
    ToolResult,
    StatusUpdate,
    Error
}
