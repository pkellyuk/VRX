using System.Text.Json;
using System.Text.Json.Serialization;

namespace Automaton.Core.Streaming;

/// <summary>
/// OpenAI token-usage wire DTO. Promoted to a public top-level type (rather
/// than kept nested in <see cref="OpenAIStreamDecoder"/>) because the
/// non-streaming <c>OpenAIResponse.Usage</c> in <c>LlamaCppHttpProvider</c>
/// also needs it. Shape is identical to the streaming-side usage payload;
/// sharing a single type guarantees the two paths stay in lockstep.
/// </summary>
public sealed class OpenAIUsage
{
    [JsonPropertyName("prompt_tokens")]
    public int PromptTokens { get; set; }

    [JsonPropertyName("completion_tokens")]
    public int CompletionTokens { get; set; }

    [JsonPropertyName("total_tokens")]
    public int TotalTokens { get; set; }
}

/// <summary>
/// Pure decoder: raw SSE line -&gt; ordered semantic events. No IO, no side
/// effects, no mutable state.
///
/// The event ordering within a chunk (finish, reasoning, content, tool calls,
/// usage) is fixed and is a load-bearing contract &mdash; the apply step
/// (<see cref="LlmStreamApplier"/>) depends on it. Same bytes in =&gt; same
/// events out; this is the property the remote "replay the recorded byte
/// stream" design depends on.
/// </summary>
public static class OpenAIStreamDecoder
{
    /// <summary>
    /// Map one raw SSE line to an ordered list of typed events.
    /// </summary>
    public static IReadOnlyList<LlmStreamEvent> DecodeLine(string? rawLine)
    {
        if (string.IsNullOrWhiteSpace(rawLine) || !rawLine.StartsWith("data:", StringComparison.Ordinal))
        {
            return Array.Empty<LlmStreamEvent>();
        }

        var payload = rawLine["data:".Length..].Trim();
        if (payload == "[DONE]")
        {
            return new LlmStreamEvent[] { new StreamDoneEvent() };
        }

        // A /responses payload names its event in a top-level "type"; a chat chunk never has one.
        if ((payload.Contains("\"response.", StringComparison.Ordinal) || payload.Contains("\"error\"", StringComparison.Ordinal)) &&
            OpenAIResponsesStreamDecoder.TryDecode(payload, out var responsesEvents))
        {
            return responsesEvents;
        }

        var chunk = JsonSerializer.Deserialize<OpenAIStreamChunk>(payload);
        if (chunk == null)
        {
            return Array.Empty<LlmStreamEvent>();
        }

        return DecodeChunk(chunk);
    }

    private static IReadOnlyList<LlmStreamEvent> DecodeChunk(OpenAIStreamChunk chunk)
    {
        var choice = chunk.Choices?.FirstOrDefault();
        if (choice == null)
        {
            // include_usage streams finish with a usage-only chunk whose choices
            // array is empty. Keep its accounting without inventing content.
            return chunk.Usage == null
                ? Array.Empty<LlmStreamEvent>()
                : new LlmStreamEvent[] { new UsageEvent(
                    chunk.Usage.PromptTokens,
                    chunk.Usage.CompletionTokens,
                    chunk.Usage.TotalTokens) };
        }

        var events = new List<LlmStreamEvent>();

        if (!string.IsNullOrEmpty(choice.FinishReason))
        {
            events.Add(new FinishEvent(choice.FinishReason));
        }

        var reasoningDelta = choice.Delta?.ReasoningContent ?? choice.Delta?.Reasoning;
        if (!string.IsNullOrEmpty(reasoningDelta))
        {
            events.Add(new ReasoningDeltaEvent(reasoningDelta));
        }

        // Emit even whitespace-only content; the apply step decides whether to
        // swallow it (see the stray-whitespace handling in LlmStreamApplier).
        var contentDelta = choice.Delta?.Content;
        if (!string.IsNullOrEmpty(contentDelta))
        {
            events.Add(new ContentDeltaEvent(contentDelta));
        }

        if (choice.Delta?.ToolCalls != null)
        {
            foreach (var toolCallDelta in choice.Delta.ToolCalls)
            {
                events.Add(new ToolCallDeltaEvent(
                    toolCallDelta.Index ?? 0,
                    toolCallDelta.Id,
                    toolCallDelta.Type,
                    toolCallDelta.Function?.Name,
                    toolCallDelta.Function?.Arguments));
            }
        }

        if (chunk.Usage != null)
        {
            events.Add(new UsageEvent(
                chunk.Usage.PromptTokens,
                chunk.Usage.CompletionTokens,
                chunk.Usage.TotalTokens));
        }

        return events;
    }

    // Wire-format DTOs. Internal to the decoder; nothing else in the system
    // depends on the OpenAI SSE shape. Kept private nested to keep the public
    // surface area of the shared type to events + decoder entry point only.
    // OpenAIUsage is promoted to a top-level public type above so non-streaming
    // DTOs can share it.
    private sealed class OpenAIStreamChunk
    {
        [JsonPropertyName("choices")]
        public List<OpenAIStreamChoice>? Choices { get; set; }

        [JsonPropertyName("usage")]
        public OpenAIUsage? Usage { get; set; }
    }

    private sealed class OpenAIStreamChoice
    {
        [JsonPropertyName("delta")]
        public OpenAIStreamDelta? Delta { get; set; }

        [JsonPropertyName("finish_reason")]
        public string? FinishReason { get; set; }
    }

    private sealed class OpenAIStreamDelta
    {
        [JsonPropertyName("content")]
        public string? Content { get; set; }

        [JsonPropertyName("reasoning")]
        public string? Reasoning { get; set; }

        [JsonPropertyName("reasoning_content")]
        public string? ReasoningContent { get; set; }

        [JsonPropertyName("tool_calls")]
        public List<OpenAIToolCallDelta>? ToolCalls { get; set; }
    }

    private sealed class OpenAIToolCallDelta
    {
        [JsonPropertyName("index")]
        public int? Index { get; set; }

        [JsonPropertyName("id")]
        public string? Id { get; set; }

        [JsonPropertyName("type")]
        public string? Type { get; set; }

        [JsonPropertyName("function")]
        public OpenAIFunctionCallDelta? Function { get; set; }
    }

    private sealed class OpenAIFunctionCallDelta
    {
        [JsonPropertyName("name")]
        public string? Name { get; set; }

        [JsonPropertyName("arguments")]
        public string? Arguments { get; set; }
    }
}
