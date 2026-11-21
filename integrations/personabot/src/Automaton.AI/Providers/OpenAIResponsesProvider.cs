using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Automaton.AI.Interfaces;
using Automaton.AI.Services;
using Automaton.Core.Interfaces;
using Automaton.Core.Services;
using Automaton.Core.Streaming;
using Microsoft.Extensions.Logging;

namespace Automaton.AI.Providers;

/// <summary>
/// Speaks the OpenAI <c>/responses</c> format, for models that need it: GPT 6.1 Terra on Azure
/// refuses thinking and tools together on <c>/chat/completions</c> (6 Nov 2026). Provider type
/// <c>openai-responses</c>.
///
/// Only the request and the whole-reply parse are its own. Sending, retries, refused fields,
/// streaming, loop detection and truncation are <see cref="LlamaCppHttpProvider"/>'s: the stream
/// decoder reads both formats line by line, which also keeps a remote device's replay of the raw
/// stream working.
///
/// Nothing is kept on the server (<c>store: false</c>), so the conversation is Automaton's own and
/// a chat can move between providers. What the model needs back is its reasoning items: they are
/// stored on the assistant message (<see cref="ProviderReasoningItems"/>) and sent again, ahead of
/// the calls they led to, for the rest of the turn.
/// </summary>
public sealed class OpenAIResponsesProvider : LlamaCppHttpProvider
{
    private const string EncryptedReasoning = "reasoning.encrypted_content";

    /// <summary>
    /// Configured fields that only <c>/chat/completions</c> servers read, or that this request
    /// already sends under its own name.
    /// </summary>
    private static readonly HashSet<string> NotSentHere = new(StringComparer.OrdinalIgnoreCase)
    {
        "model",
        "input",
        "instructions",
        "messages",
        "tools",
        "tool_choice",
        "stream",
        "temperature",
        "max_tokens",
        "max_completion_tokens",
        "max_output_tokens",
        "reasoning",
        "reasoning_effort",
        "reasoning_budget_tokens",
        "chat_template_kwargs",
        "store",
        "include"
    };

    public OpenAIResponsesProvider(
        string baseUrl,
        string? apiKey,
        IReadOnlyDictionary<string, JsonElement>? requestParameters = null,
        ILogger<LlamaCppHttpProvider>? logger = null)
        : base(baseUrl, apiKey, requestParameters, logger, null)
    {
    }

    /// <summary>Uses a caller-owned HTTP client, as the matching <see cref="LlamaCppHttpProvider"/> constructor does.</summary>
    public OpenAIResponsesProvider(
        HttpClient httpClient,
        string baseUrl,
        string? apiKey,
        IReadOnlyDictionary<string, JsonElement>? requestParameters = null,
        ILogger<LlamaCppHttpProvider>? logger = null)
        : base(baseUrl, apiKey, requestParameters, logger, httpClient ?? throw new ArgumentNullException(nameof(httpClient)))
    {
    }

    public override string Id => "openai-responses";

    private protected override string RequestPath => "responses";

    private protected override IEnumerable<string> OptionalRequestFields() =>
        _requestParameters.Keys
            .Where(key => !NotSentHere.Contains(key))
            .Concat(new[] { "store", "include", "parallel_tool_calls" });

    private protected override string BuildRequestJson(
        List<Message> messages,
        Dictionary<string, JsonSchema> tools,
        string model,
        double? temperature,
        int? maxTokens,
        bool useStreaming)
    {
        var (instructions, input) = ConvertInput(messages);
        var body = new JsonObject
        {
            ["model"] = model,
            ["input"] = input
        };

        if (!string.IsNullOrWhiteSpace(instructions))
        {
            body["instructions"] = instructions;
        }

        if (tools.Count > 0)
        {
            body["tools"] = ConvertTools(tools);
            body["tool_choice"] = "auto";
        }

        if (temperature.HasValue && !RejectedRequestArguments.IsRejected(_baseUrl, model, "temperature"))
        {
            body["temperature"] = temperature.Value;
        }

        if (maxTokens.HasValue)
        {
            body["max_output_tokens"] = maxTokens.Value;
        }

        body["stream"] = useStreaming;
        if (!RejectedRequestArguments.IsRejected(_baseUrl, model, "store"))
        {
            body["store"] = false;
        }

        var reasoning = BuildReasoning(model);
        if (reasoning != null)
        {
            body["reasoning"] = reasoning;

            // Without server-side storage this is the only way to get the model's thinking back.
            if (!IsEffortNone(reasoning) && !RejectedRequestArguments.IsRejected(_baseUrl, model, "include"))
            {
                body["include"] = new JsonArray(EncryptedReasoning);
            }
        }

        foreach (var parameter in _requestParameters)
        {
            if (string.IsNullOrWhiteSpace(parameter.Key) ||
                NotSentHere.Contains(parameter.Key) ||
                RejectedRequestArguments.IsRejected(_baseUrl, model, parameter.Key))
            {
                continue;
            }

            body[parameter.Key] = JsonNode.Parse(parameter.Value.GetRawText());
        }

        var json = body.ToJsonString();
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[OpenAIResponsesProvider.BuildRequestJson] {input.Count} input item(s), {tools.Count} tool(s), reasoning={reasoning?.ToJsonString() ?? "(none)"}, length={json.Length}");
        return json;
    }

    /// <summary>
    /// The <c>reasoning</c> object from the thinking level (<c>reasoning_effort</c>) or a
    /// configured <c>reasoning</c>. None when neither is set: a model that does not reason refuses it.
    /// </summary>
    private JsonObject? BuildReasoning(string model)
    {
        JsonObject? reasoning = null;
        if (_requestParameters.TryGetValue("reasoning", out var configured) && configured.ValueKind == JsonValueKind.Object)
        {
            reasoning = JsonNode.Parse(configured.GetRawText())?.AsObject();
        }

        if (_requestParameters.TryGetValue("reasoning_effort", out var effort) && effort.ValueKind == JsonValueKind.String)
        {
            reasoning ??= new JsonObject();
            reasoning["effort"] = effort.GetString();
        }

        if (reasoning == null)
        {
            return null;
        }

        if (IsEffortNone(reasoning) || RejectedRequestArguments.IsRejected(_baseUrl, model, RejectedRequestArguments.ReasoningSummary))
        {
            reasoning.Remove("summary");
        }
        else if (!reasoning.ContainsKey("summary"))
        {
            // GPT never shows its thinking itself; the summary is what the Thinking panel can show.
            reasoning["summary"] = "auto";
        }

        return reasoning;
    }

    private static bool IsEffortNone(JsonObject reasoning) =>
        reasoning["effort"] is JsonValue value &&
        value.TryGetValue<string>(out var effort) &&
        string.Equals(effort, "none", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// The conversation as <c>/responses</c> input items, with every system message gathered into
    /// <c>instructions</c>. A tool call goes only with its result and a result only with its call:
    /// either one alone is refused.
    /// </summary>
    internal static (string? Instructions, JsonArray Input) ConvertInput(IReadOnlyList<Message> messages)
    {
        var instructions = new List<string>();
        var input = new JsonArray();
        var answered = CollectToolResultCallIds(messages.ToList());
        var sentCalls = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var lastUserIndex = -1;
        for (var index = 0; index < messages.Count; index++)
        {
            if (string.Equals(messages[index].Role, "user", StringComparison.OrdinalIgnoreCase))
            {
                lastUserIndex = index;
            }
        }

        for (var index = 0; index < messages.Count; index++)
        {
            var message = messages[index];
            switch (message.Role)
            {
                case "system":
                    if (!string.IsNullOrWhiteSpace(message.Content) && !instructions.Contains(message.Content.Trim()))
                    {
                        instructions.Add(message.Content.Trim());
                    }

                    break;

                case "assistant":
                    AddAssistant(message, index > lastUserIndex, answered, sentCalls, input);
                    break;

                case "tool":
                    AddToolResult(message, sentCalls, input);
                    break;

                default:
                    AddUser(message, input);
                    break;
            }
        }

        return (instructions.Count == 0 ? null : string.Join("\n\n", instructions), input);
    }

    private static void AddUser(Message message, JsonArray input)
    {
        var images = message.Attachments
            .Where(attachment => attachment.IsImage && !string.IsNullOrEmpty(attachment.Base64Data))
            .ToList();

        if (images.Count == 0)
        {
            if (!string.IsNullOrEmpty(message.Content))
            {
                input.Add(new JsonObject { ["role"] = "user", ["content"] = message.Content });
            }

            return;
        }

        var content = new JsonArray();
        if (!string.IsNullOrEmpty(message.Content))
        {
            content.Add(new JsonObject { ["type"] = "input_text", ["text"] = message.Content });
        }

        foreach (var image in images)
        {
            content.Add(new JsonObject { ["type"] = "input_image", ["image_url"] = image.ToDataUri() });
        }

        input.Add(new JsonObject { ["role"] = "user", ["content"] = content });
    }

    private static void AddAssistant(
        Message message,
        bool inCurrentTurn,
        HashSet<string> answered,
        HashSet<string> sentCalls,
        JsonArray input)
    {
        var text = StripInlineThinking(message.Content);
        var calls = message.Parts
            .Where(part => part.Type == "tool" &&
                           !string.IsNullOrWhiteSpace(part.Tool) &&
                           !string.IsNullOrWhiteSpace(part.CallId) &&
                           answered.Contains(part.CallId!))
            .ToList();

        if (string.IsNullOrWhiteSpace(text) && calls.Count == 0)
        {
            return;
        }

        // Reasoning goes back only for the turn in progress, where the model is mid-way through a
        // chain of tool calls; earlier turns' thinking is not used again. A reasoning item must be
        // followed by what it led to, which the check above makes sure of.
        if (inCurrentTurn)
        {
            foreach (var item in ProviderReasoningItems.Read(message).Where(item => !string.IsNullOrEmpty(item.EncryptedContent)))
            {
                var reasoning = new JsonObject { ["type"] = "reasoning" };
                if (!string.IsNullOrEmpty(item.Id))
                {
                    reasoning["id"] = item.Id;
                }

                reasoning["summary"] = new JsonArray();
                reasoning["encrypted_content"] = item.EncryptedContent;
                input.Add(reasoning);
            }
        }

        if (!string.IsNullOrWhiteSpace(text))
        {
            input.Add(new JsonObject { ["role"] = "assistant", ["content"] = text });
        }

        foreach (var call in calls)
        {
            input.Add(new JsonObject
            {
                ["type"] = "function_call",
                ["call_id"] = call.CallId,
                ["name"] = call.Tool,
                ["arguments"] = JsonSerializer.Serialize(call.State?.Input ?? new Dictionary<string, object>())
            });
            sentCalls.Add(call.CallId!);
        }
    }

    private static void AddToolResult(Message message, HashSet<string> sentCalls, JsonArray input)
    {
        var callId = message.Parts
            .Where(part => part.Type == "tool" && !string.IsNullOrWhiteSpace(part.CallId))
            .Select(part => part.CallId)
            .FirstOrDefault();

        if (callId == null || !sentCalls.Contains(callId))
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[OpenAIResponsesProvider.AddToolResult] Leaving out a tool result whose call was not sent, callId={callId ?? "(none)"}");
            return;
        }

        input.Add(new JsonObject
        {
            ["type"] = "function_call_output",
            ["call_id"] = callId,
            ["output"] = message.Content ?? string.Empty
        });
    }

    private JsonArray ConvertTools(Dictionary<string, JsonSchema> tools)
    {
        var converted = new JsonArray();
        foreach (var (name, schema) in tools)
        {
            var description = !string.IsNullOrEmpty(schema.Description)
                ? schema.Description
                : schema.Properties.TryGetValue("description", out var descriptionProperty) ? descriptionProperty.Description : string.Empty;

            // Flat, not nested under "function" as the chat format has it.
            converted.Add(new JsonObject
            {
                ["type"] = "function",
                ["name"] = name,
                ["description"] = description ?? string.Empty,
                ["parameters"] = JsonSerializer.SerializeToNode(ConvertSchema(schema))
            });
        }

        return converted;
    }

    /// <summary>A whole reply: what summaries and other non-streamed calls get back.</summary>
    private protected override AIResponse ParseResponse(string responseJson)
    {
        return ParseReply(responseJson);
    }

    internal static AIResponse ParseReply(string responseJson)
    {
        using var document = JsonDocument.Parse(responseJson);
        var root = document.RootElement;
        if (root.TryGetProperty("response", out var wrapped) && wrapped.ValueKind == JsonValueKind.Object)
        {
            root = wrapped;
        }

        if (Text(root, "status") == "failed" ||
            (root.TryGetProperty("error", out var error) && error.ValueKind == JsonValueKind.Object))
        {
            throw new InvalidOperationException($"The model endpoint failed the reply: {OpenAIResponsesStreamDecoder.FailureMessage(root)}");
        }

        var response = new AIResponse { FinishReason = OpenAIResponsesStreamDecoder.FinishReason(root) };
        var text = new StringBuilder();
        var reasoning = new List<string>();

        if (root.TryGetProperty("output", out var output) && output.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in output.EnumerateArray())
            {
                switch (Text(item, "type"))
                {
                    case "message":
                        foreach (var part in Array(item, "content"))
                        {
                            if (Text(part, "type") is "output_text" or "refusal")
                            {
                                text.Append(Text(part, "text") ?? Text(part, "refusal"));
                            }
                        }

                        break;

                    case "reasoning":
                        reasoning.AddRange(Array(item, "summary").Select(part => Text(part, "text")).Where(part => !string.IsNullOrWhiteSpace(part))!);
                        reasoning.AddRange(Array(item, "content").Select(part => Text(part, "text")).Where(part => !string.IsNullOrWhiteSpace(part))!);
                        response.ReasoningItems.Add(new ProviderReasoningItem
                        {
                            Id = Text(item, "id"),
                            EncryptedContent = Text(item, "encrypted_content")
                        });
                        break;

                    case "function_call":
                        response.ToolCalls.Add(new ToolCallRequest
                        {
                            Id = Text(item, "call_id") ?? Text(item, "id") ?? Guid.NewGuid().ToString("N"),
                            Tool = Text(item, "name") ?? string.Empty,
                            Arguments = string.IsNullOrWhiteSpace(Text(item, "arguments")) ? "{}" : Text(item, "arguments")!
                        });
                        break;
                }
            }
        }

        var (visibleText, inlineReasoning) = SplitInlineThinking(text.ToString());
        response.Text = visibleText;
        response.Reasoning = reasoning.Count > 0 ? string.Join("\n\n", reasoning) : inlineReasoning;
        response.Truncated = response.FinishReason == "length";

        if (root.TryGetProperty("usage", out var usage) && usage.ValueKind == JsonValueKind.Object)
        {
            var inputTokens = Number(usage, "input_tokens");
            var outputTokens = Number(usage, "output_tokens");
            var total = Number(usage, "total_tokens");
            response.Usage = new AIUsage
            {
                InputTokens = inputTokens,
                OutputTokens = outputTokens,
                TotalTokens = total > 0 ? total : inputTokens + outputTokens
            };
        }

        if (response.Truncated)
        {
            AutomatonDebug.LogWarning($"[OpenAIResponsesProvider.ParseReply] Response TRUNCATED at max_output_tokens, textChars={response.Text?.Length ?? 0}, reasoningChars={response.Reasoning?.Length ?? 0}.");
        }

        return response;
    }

    private static IEnumerable<JsonElement> Array(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Array
            ? value.EnumerateArray()
            : Enumerable.Empty<JsonElement>();

    private static string? Text(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;

    private static int Number(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number)
            ? number
            : 0;
}
