namespace Automaton.Core.Streaming;

/// <summary>
/// One semantic event decoded from the LLM wire stream. Deliberately free of
/// any wire/JSON detail so the apply step (and the remote replay client)
/// depends only on meaning, not on the OpenAI SSE shape.
/// </summary>
public abstract record LlmStreamEvent;

/// <summary>The model declared why it stopped.</summary>
public sealed record FinishEvent(string Reason) : LlmStreamEvent;

/// <summary>Out-of-band chain-of-thought delta.</summary>
public sealed record ReasoningDeltaEvent(string Text) : LlmStreamEvent;

/// <summary>Visible assistant text delta.</summary>
public sealed record ContentDeltaEvent(string Text) : LlmStreamEvent;

/// <summary>Partial tool-call assembly, identified by <see cref="Index"/>.</summary>
public sealed record ToolCallDeltaEvent(
    int Index,
    string? Id,
    string? Type,
    string? Name,
    string? ArgumentsFragment) : LlmStreamEvent;

/// <summary>
/// A tool call's whole arguments, sent once the call is complete. <c>/responses</c> sends these
/// after the fragments, and some servers send only these, so they replace what the fragments built.
/// </summary>
public sealed record ToolCallArgumentsDoneEvent(int Index, string Arguments) : LlmStreamEvent;

/// <summary>
/// A finished reasoning item from <c>/responses</c>. Its encrypted content is the model's own
/// thinking, which it needs back on the next request of the same turn; nobody can read it.
/// </summary>
public sealed record ReasoningItemEvent(string? Id, string? EncryptedContent) : LlmStreamEvent;

/// <summary>The endpoint reported, inside a successful HTTP stream, that the reply failed.</summary>
public sealed record StreamFailedEvent(string Message) : LlmStreamEvent;

/// <summary>Token usage accounting emitted by some providers.</summary>
public sealed record UsageEvent(int PromptTokens, int CompletionTokens, int TotalTokens) : LlmStreamEvent;

/// <summary>
/// Terminal marker for the <c>data: [DONE]</c> sentinel. Signals the IO loop
/// to stop; carries no state.
/// </summary>
public sealed record StreamDoneEvent : LlmStreamEvent;
