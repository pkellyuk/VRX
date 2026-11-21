using System.Text;
using Automaton.AI.Interfaces;
using Automaton.Core.Services;

namespace Automaton.Core.Streaming;

/// <summary>
/// Folds one decoded <see cref="LlmStreamEvent"/> into the streaming
/// accumulators and drives the live UI side effects. This is the only place
/// in the streaming path that mutates state or calls back into the UI.
///
/// Per <see cref="OpenAIStreamDecoder"/>, the decoder's intra-chunk event
/// order is fixed (finish -&gt; reasoning -&gt; content -&gt; tool calls -&gt;
/// usage) and this method depends on it.
/// </summary>
public static class LlmStreamApplier
{
    public static void Apply(
        LlmStreamEvent streamEvent,
        StringBuilder contentBuilder,
        StringBuilder reasoningBuilder,
        Dictionary<int, StreamingToolCallBuilder> toolCalls,
        AIResponse aiResponse,
        Action<string>? textDeltaCallback,
        LiveThinkBlockState liveThinkState)
    {
        switch (streamEvent)
        {
            case FinishEvent finish:
                aiResponse.FinishReason = finish.Reason;
                break;

            case ReasoningDeltaEvent reasoning:
                // Out-of-band reasoning is delivered separately from text content.
                // It is buffered (not emitted per-delta) so the live UI can later
                // flush a single `<think>...</think>` block that matches what
                // AppendAssistantMessageContent reconstructs from Parts.reasoning on
                // reload — per-delta emit would diverge on whitespace and break the
                // prefix check.
                reasoningBuilder.Append(reasoning.Text);
                // Mark that this stream carries reasoning out-of-band. Some models
                // (e.g. Minimax M3) ALSO mirror the same reasoning as inline
                // `<think>` tags in the content channel; once we know the out-of-
                // band channel is in use, the inline copy is a duplicate and the
                // content channel is stripped of it (see ContentDeltaEvent below).
                liveThinkState.SawOutOfBandReasoning = true;
                if (textDeltaCallback != null)
                {
                    if (!liveThinkState.BlockEmitted)
                    {
                        liveThinkState.PendingReasoning.Append(reasoning.Text);
                    }
                    else
                    {
                        // Reasoning arrived AFTER content already started streaming, so
                        // the live `<think>` block is already closed and can't take it.
                        // It still lands in aiResponse.Reasoning, so reload reconstructs
                        // a block containing both early and late reasoning that diverges
                        // from the live UI. Logged so we know if this actually fires; the
                        // real fix is splitting persisted reasoning into multiple
                        // Parts.reasoning entries (one per contiguous batch).
                        AutomatonDebug.LogWarning($"[REASONORDER] Late reasoning delta after content started: deltaLen={reasoning.Text.Length}, totalReasoningLen={reasoningBuilder.Length}, contentLen={contentBuilder.Length}. Live UI will not show this; reload will.");
                    }
                }
                break;

            case ContentDeltaEvent content:
                var contentText = content.Text;

                // Dual-channel dedupe: when this stream also delivers reasoning
                // out-of-band, any inline `<think>...</think>` in the content
                // channel is a duplicate of it (the Minimax M3 case). Strip it
                // here so (a) the inline copy never reaches the live UI or
                // contentBuilder, and (b) its non-whitespace opener can't trigger
                // an early flush of the still-incomplete out-of-band buffer (which
                // is what produced the truncated "ghost" + full duplicate block).
                // Single-channel models never set the flag, so they are untouched.
                if (liveThinkState.SawOutOfBandReasoning)
                {
                    contentText = liveThinkState.InlineThinkStripper.Strip(contentText);
                    if (contentText.Length == 0)
                    {
                        // Entire delta was inline-think (or a partial tag carried to
                        // the next delta). Nothing visible to show, and crucially no
                        // flush — the out-of-band buffer keeps accumulating until the
                        // first real visible content (or end of stream).
                        break;
                    }
                }

                // Some models emit a stray whitespace delta (e.g. a lone "\n") between
                // the reasoning stream and the tool-call/visible-text stream. Flushing
                // the reasoning block on that whitespace would close the live `<think>`
                // block *before reasoning finished streaming*, producing a truncated
                // block; the full reasoning then reappears as a second block on reload
                // (the duplicated/truncated `<think>` bug). So while reasoning is still
                // streaming, swallow whitespace-only deltas instead of flushing.
                var isStrayWhitespaceBeforeReasoningFlush =
                    liveThinkState.HasUnflushedReasoning && string.IsNullOrWhiteSpace(contentText);
                if (!isStrayWhitespaceBeforeReasoningFlush)
                {
                    // Flush any buffered reasoning as a single trimmed `<think>...</think>`
                    // block before the visible content begins. Matches reload exactly.
                    FlushPendingReasoningBlock(liveThinkState, textDeltaCallback);
                    contentBuilder.Append(contentText);
                    textDeltaCallback?.Invoke(contentText);
                }
                break;

            case ToolCallDeltaEvent toolCall:
                if (!toolCalls.TryGetValue(toolCall.Index, out var builder))
                {
                    builder = new StreamingToolCallBuilder();
                    toolCalls[toolCall.Index] = builder;
                }
                builder.Apply(toolCall.Id, toolCall.Type, toolCall.Name, toolCall.ArgumentsFragment);
                break;

            case ToolCallArgumentsDoneEvent argumentsDone:
                if (!toolCalls.TryGetValue(argumentsDone.Index, out var doneBuilder))
                {
                    doneBuilder = new StreamingToolCallBuilder();
                    toolCalls[argumentsDone.Index] = doneBuilder;
                }
                doneBuilder.ReplaceArguments(argumentsDone.Arguments);
                break;

            case ReasoningItemEvent reasoningItem:
                aiResponse.ReasoningItems.Add(new ProviderReasoningItem
                {
                    Id = reasoningItem.Id,
                    EncryptedContent = reasoningItem.EncryptedContent
                });
                break;

            case StreamFailedEvent failed:
                aiResponse.StreamError = failed.Message;
                aiResponse.FinishReason = "error";
                break;

            case UsageEvent usage:
                aiResponse.Usage = new AIUsage
                {
                    InputTokens = usage.PromptTokens,
                    OutputTokens = usage.CompletionTokens,
                    TotalTokens = usage.TotalTokens
                };
                break;
        }
    }

    /// <summary>
    /// Flushes the buffered reasoning as a single trimmed
    /// <c><think>...</think></c> block matching what reload reconstructs from
    /// <c>Parts.reasoning</c>. Called from the IO loop after the stream ends
    /// for thinking-only responses, and from <see cref="Apply"/> on the first
    /// genuine content delta.
    /// </summary>
    public static void FlushPendingReasoningBlock(LiveThinkBlockState state, Action<string>? textDeltaCallback)
    {
        if (state == null || textDeltaCallback == null)
        {
            return;
        }

        if (!state.HasUnflushedReasoning)
        {
            return;
        }

        var trimmed = state.PendingReasoning.ToString().Trim();
        state.PendingReasoning.Clear();

        if (trimmed.Length == 0)
        {
            return;
        }

        textDeltaCallback($"<think>{Environment.NewLine}{trimmed}{Environment.NewLine}</think>{Environment.NewLine}{Environment.NewLine}");
        state.BlockEmitted = true;
    }

    /// <summary>
    /// Emits any content the inline-think stripper held back at end of stream
    /// (a false partial tag that turned out to be literal text). Called from the
    /// IO loop after the stream ends, alongside
    /// <see cref="FlushPendingReasoningBlock"/>. No-op unless dual-channel
    /// stripping was active and a tail was carried.
    /// </summary>
    public static void FlushInlineThinkCarry(
        LiveThinkBlockState state,
        StringBuilder contentBuilder,
        Action<string>? textDeltaCallback)
    {
        if (state == null || !state.SawOutOfBandReasoning)
        {
            return;
        }

        var leftover = state.InlineThinkStripper.Flush();
        if (leftover.Length == 0)
        {
            return;
        }

        FlushPendingReasoningBlock(state, textDeltaCallback);
        contentBuilder.Append(leftover);
        textDeltaCallback?.Invoke(leftover);
    }
}

/// <summary>
/// Buffers per-stream reasoning deltas so they can be emitted to the live UI
/// as a single trimmed block matching what <c>AppendThinkBlock</c> produces
/// on transcript reload. Per-delta emit doesn't work because the reload path
/// trims the whole concatenated reasoning string once, while live deltas
/// retain leading/trailing/internal whitespace as the model sent them. The
/// byte-level mismatch is enough to fail <c>AreCompatibleRemoteMessages</c>.
/// </summary>
public sealed class LiveThinkBlockState
{
    public StringBuilder PendingReasoning { get; } = new();
    public bool BlockEmitted { get; set; }

    /// <summary>
    /// Set once this stream delivers reasoning out-of-band (a
    /// <c>reasoning_content</c>/<c>reasoning</c> delta). Gates the dual-channel
    /// inline-`&lt;think&gt;` stripping in <see cref="LlmStreamApplier.Apply"/>.
    /// </summary>
    public bool SawOutOfBandReasoning { get; set; }

    /// <summary>
    /// Strips the duplicate inline `&lt;think&gt;` copy from the content channel
    /// for models that emit reasoning on both channels. Only consulted once
    /// <see cref="SawOutOfBandReasoning"/> is true.
    /// </summary>
    public InlineThinkStripper InlineThinkStripper { get; } = new();

    public bool HasUnflushedReasoning => PendingReasoning.Length > 0;
}

/// <summary>
/// Assembles a streamed tool call from the per-delta <c>id</c> / <c>type</c> /
/// <c>name</c> / arguments fragments. Decoupled from the OpenAI wire DTO so
/// the apply step depends on primitives, not the SSE shape.
/// </summary>
public sealed class StreamingToolCallBuilder
{
    private readonly StringBuilder _arguments = new();

    public string? Id { get; private set; }
    public string? Type { get; private set; }
    public string? Name { get; private set; }

    public void Apply(string? id, string? type, string? name, string? argumentsFragment)
    {
        if (!string.IsNullOrWhiteSpace(id))
        {
            Id = id;
        }

        if (!string.IsNullOrWhiteSpace(type))
        {
            Type = type;
        }

        if (!string.IsNullOrWhiteSpace(name))
        {
            Name = name;
        }

        if (!string.IsNullOrEmpty(argumentsFragment))
        {
            _arguments.Append(argumentsFragment);
        }
    }

    /// <summary>Takes the complete arguments in place of the fragments gathered so far.</summary>
    public void ReplaceArguments(string? arguments)
    {
        if (arguments == null)
        {
            return;
        }

        _arguments.Clear();
        _arguments.Append(arguments);
    }

    public ToolCallRequest? ToToolCallRequest()
    {
        if (string.IsNullOrWhiteSpace(Name))
        {
            return null;
        }

        var arguments = _arguments.Length == 0 ? "{}" : _arguments.ToString();
        return new ToolCallRequest
        {
            Id = string.IsNullOrWhiteSpace(Id) ? Guid.NewGuid().ToString("N") : Id,
            Tool = Name,
            Arguments = arguments
        };
    }
}
