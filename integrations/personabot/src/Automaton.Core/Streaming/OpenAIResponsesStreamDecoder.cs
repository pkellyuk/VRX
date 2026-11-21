using System.Text.Json;

namespace Automaton.Core.Streaming;

/// <summary>
/// Pure decoder for the <c>/responses</c> stream: one SSE <c>data:</c> payload -&gt; the same typed
/// events the <c>/chat/completions</c> decoder produces, so the IO loop, the apply step and the
/// remote replay all stay shared. Each payload names itself in a top-level <c>type</c>
/// (<c>response.output_text.delta</c> and so on); a chat chunk has no such field, which is how
/// <see cref="OpenAIStreamDecoder"/> tells the two apart line by line. Stateless: same bytes in,
/// same events out.
///
/// Tool calls are keyed by <c>output_index</c>, the item's place in the reply, which plays the part
/// the chat stream's tool-call index does.
/// </summary>
public static class OpenAIResponsesStreamDecoder
{
    /// <summary>
    /// Decodes a payload whose top-level <c>type</c> is a <c>/responses</c> event. False for
    /// anything else, which is left to the chat decoder.
    /// </summary>
    public static bool TryDecode(string payload, out IReadOnlyList<LlmStreamEvent> events)
    {
        events = Array.Empty<LlmStreamEvent>();
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(payload);
        }
        catch (JsonException)
        {
            return false;
        }

        using (document)
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                root.TryGetProperty("choices", out _) ||
                !root.TryGetProperty("type", out var typeElement) ||
                typeElement.ValueKind != JsonValueKind.String)
            {
                return false;
            }

            var type = typeElement.GetString() ?? string.Empty;
            if (!type.StartsWith("response.", StringComparison.Ordinal) && type != "error")
            {
                return false;
            }

            events = Decode(type, root);
            return true;
        }
    }

    private static IReadOnlyList<LlmStreamEvent> Decode(string type, JsonElement root)
    {
        switch (type)
        {
            case "response.output_text.delta":
            case "response.refusal.delta":
                return Text(root, "delta") is { Length: > 0 } text
                    ? new LlmStreamEvent[] { new ContentDeltaEvent(text) }
                    : Array.Empty<LlmStreamEvent>();

            case "response.reasoning_summary_text.delta":
            case "response.reasoning_text.delta":
                return Text(root, "delta") is { Length: > 0 } reasoning
                    ? new LlmStreamEvent[] { new ReasoningDeltaEvent(reasoning) }
                    : Array.Empty<LlmStreamEvent>();

            case "response.reasoning_summary_part.added":
                // Summary parts are separate paragraphs; the deltas carry no break between them.
                return Number(root, "summary_index") > 0
                    ? new LlmStreamEvent[] { new ReasoningDeltaEvent("\n\n") }
                    : Array.Empty<LlmStreamEvent>();

            case "response.output_item.added":
                return root.TryGetProperty("item", out var added) && Text(added, "type") == "function_call"
                    ? new LlmStreamEvent[] { FunctionCall(root, added) }
                    : Array.Empty<LlmStreamEvent>();

            case "response.function_call_arguments.delta":
                return Text(root, "delta") is { Length: > 0 } fragment
                    ? new LlmStreamEvent[] { new ToolCallDeltaEvent(Number(root, "output_index"), null, null, null, fragment) }
                    : Array.Empty<LlmStreamEvent>();

            case "response.function_call_arguments.done":
                return Text(root, "arguments") is { } arguments
                    ? new LlmStreamEvent[] { new ToolCallArgumentsDoneEvent(Number(root, "output_index"), arguments) }
                    : Array.Empty<LlmStreamEvent>();

            case "response.output_item.done":
                return ItemDone(root);

            case "response.completed":
            case "response.incomplete":
                return Finished(root);

            case "response.failed":
                return new LlmStreamEvent[]
                {
                    new StreamFailedEvent(FailureMessage(root.TryGetProperty("response", out var failed) ? failed : root)),
                    new StreamDoneEvent()
                };

            case "error":
                return new LlmStreamEvent[] { new StreamFailedEvent(FailureMessage(root)), new StreamDoneEvent() };

            default:
                return Array.Empty<LlmStreamEvent>();
        }
    }

    private static IReadOnlyList<LlmStreamEvent> ItemDone(JsonElement root)
    {
        if (!root.TryGetProperty("item", out var item))
        {
            return Array.Empty<LlmStreamEvent>();
        }

        switch (Text(item, "type"))
        {
            case "function_call":
                var events = new List<LlmStreamEvent> { FunctionCall(root, item) };
                if (Text(item, "arguments") is { } arguments)
                {
                    events.Add(new ToolCallArgumentsDoneEvent(Number(root, "output_index"), arguments));
                }

                return events;

            case "reasoning":
                return new LlmStreamEvent[] { new ReasoningItemEvent(Text(item, "id"), Text(item, "encrypted_content")) };

            default:
                return Array.Empty<LlmStreamEvent>();
        }
    }

    private static IReadOnlyList<LlmStreamEvent> Finished(JsonElement root)
    {
        var events = new List<LlmStreamEvent>();
        if (!root.TryGetProperty("response", out var response))
        {
            events.Add(new FinishEvent("stop"));
            events.Add(new StreamDoneEvent());
            return events;
        }

        if (response.TryGetProperty("usage", out var usage) && usage.ValueKind == JsonValueKind.Object)
        {
            var input = Number(usage, "input_tokens");
            var output = Number(usage, "output_tokens");
            var total = Number(usage, "total_tokens");
            events.Add(new UsageEvent(input, output, total > 0 ? total : input + output));
        }

        events.Add(new FinishEvent(FinishReason(response)));
        events.Add(new StreamDoneEvent());
        return events;
    }

    /// <summary>
    /// The chat-completions stop reason a finished reply amounts to: <c>length</c> when the output
    /// limit cut it off, so truncation is handled the same way on both formats.
    /// </summary>
    public static string FinishReason(JsonElement response)
    {
        if (Text(response, "status") == "incomplete")
        {
            var reason = response.TryGetProperty("incomplete_details", out var details) && details.ValueKind == JsonValueKind.Object
                ? Text(details, "reason")
                : null;
            return reason == "content_filter" ? "content_filter" : "length";
        }

        if (response.TryGetProperty("output", out var output) && output.ValueKind == JsonValueKind.Array &&
            output.EnumerateArray().Any(item => Text(item, "type") == "function_call"))
        {
            return "tool_calls";
        }

        return "stop";
    }

    /// <summary>The message a failed reply or an <c>error</c> event carries.</summary>
    public static string FailureMessage(JsonElement element)
    {
        if (element.TryGetProperty("error", out var error) && error.ValueKind == JsonValueKind.Object)
        {
            element = error;
        }

        var message = Text(element, "message");
        var code = Text(element, "code");
        return string.IsNullOrWhiteSpace(message)
            ? $"The model endpoint reported a failed reply{(string.IsNullOrWhiteSpace(code) ? "" : $" ({code})")}."
            : string.IsNullOrWhiteSpace(code) ? message : $"{message} ({code})";
    }

    private static ToolCallDeltaEvent FunctionCall(JsonElement root, JsonElement item) =>
        new(Number(root, "output_index"), Text(item, "call_id") ?? Text(item, "id"), "function", Text(item, "name"), null);

    private static string? Text(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;

    private static int Number(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt32(out var number)
            ? number
            : 0;
}
