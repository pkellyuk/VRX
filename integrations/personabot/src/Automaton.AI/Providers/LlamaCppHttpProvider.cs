using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using Automaton.AI.Interfaces;
using Automaton.AI.Services;
using Automaton.Core.Interfaces;
using Automaton.Core.Services;
using Automaton.Core.Streaming;
using Microsoft.Extensions.Logging;

namespace Automaton.AI.Providers;

using Debug = AutomatonDebug;

/// <summary>
/// llama.cpp provider using direct HTTP calls to OpenAI-compatible API.
/// <see cref="OpenAIResponsesProvider"/> derives from it to speak <c>/responses</c> instead: it
/// replaces the request, the endpoint path and the whole-reply parse, and shares the rest -
/// sending, retries, refused fields, streaming, loop detection and truncation.
/// </summary>
public class LlamaCppHttpProvider : IAIProvider, IExecutionBudgetAwareProvider
{
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromMinutes(20);
    private static readonly TimeSpan StreamingIdleTimeout = TimeSpan.FromMinutes(2);
    private static readonly TimeSpan StreamingCompletionGracePeriod = TimeSpan.FromSeconds(10);
    private readonly HttpClient _httpClient;
    private readonly bool _borrowedClient;
    private readonly ILogger<LlamaCppHttpProvider>? _logger;
    private protected readonly string _baseUrl;
    private readonly string? _apiKey;
    private protected readonly IReadOnlyDictionary<string, JsonElement> _requestParameters;
    private readonly SemaphoreSlim _requestGate = new(1, 1);

    public virtual string Id => "llamacpp";

    /// <summary>The endpoint path under the base URL that requests go to.</summary>
    private protected virtual string RequestPath => "chat/completions";

    public LlamaCppHttpProvider(
        string baseUrl,
        string? apiKey,
        IReadOnlyDictionary<string, JsonElement>? requestParameters = null,
        ILogger<LlamaCppHttpProvider>? logger = null)
        : this(baseUrl, apiKey, requestParameters, logger, null)
    {
    }

    /// <summary>
    /// Uses a caller-owned HTTP client without changing its defaults or disposing
    /// it. Credentials are attached to each request, so the client can be pooled.
    /// The caller must not configure shared default authorization headers/cookies.
    /// </summary>
    public LlamaCppHttpProvider(
        HttpClient httpClient,
        string baseUrl,
        string? apiKey,
        IReadOnlyDictionary<string, JsonElement>? requestParameters = null,
        ILogger<LlamaCppHttpProvider>? logger = null)
        : this(baseUrl, apiKey, requestParameters, logger, httpClient ?? throw new ArgumentNullException(nameof(httpClient)))
    {
    }

    private protected LlamaCppHttpProvider(string baseUrl, string? apiKey, IReadOnlyDictionary<string, JsonElement>? requestParameters,
        ILogger<LlamaCppHttpProvider>? logger, HttpClient? httpClient)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.Constructor] Creating provider for base URL: {baseUrl}");

        _baseUrl = NormalizeBaseUrl(baseUrl);
        _apiKey = apiKey;
        _requestParameters = requestParameters ?? new Dictionary<string, JsonElement>();
        _borrowedClient = httpClient != null;
        _httpClient = httpClient ?? new HttpClient
        {
            Timeout = RequestTimeout
        };
        _logger = logger;

        if (!string.IsNullOrEmpty(apiKey))
        {
            if (!_borrowedClient) _httpClient.DefaultRequestHeaders.Add("Authorization", $"Bearer {apiKey}");
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.Constructor] API key configured");
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.Constructor] Provider created successfully");
    }

    public async Task<AIResponse> GenerateResponseAsync(
        List<Message> messages,
        Dictionary<string, JsonSchema> tools,
        string model,
        double? temperature = null,
        int? maxTokens = null,
        CancellationToken cancellationToken = default,
        Action<string>? textDeltaCallback = null,
        RawStreamRecorder? rawStreamRecorder = null)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.GenerateResponseAsync] Entering for model: {model}");
        using var accounting = ExecutionBudgetScope.Current?.BeginModel(messages, tools, maxTokens, cancellationToken);
        if (accounting != null) { cancellationToken = accounting.Token; maxTokens = accounting.MaximumOutputTokens; }
        var originalDelta = textDeltaCallback;
        if (accounting != null && originalDelta != null) textDeltaCallback = text => { accounting.AddDelta(text); originalDelta(text); };
        var gateEntered = false;

        try
        {
            await _requestGate.WaitAsync(cancellationToken);
            gateEntered = true;
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.GenerateResponseAsync] Generating response with llama.cpp at {_baseUrl}, model: {model}");

            var useStreaming = textDeltaCallback != null;
            var json = BuildRequestJson(messages, tools, model, temperature, maxTokens, useStreaming);

            using var response = await SendRequestWithRetryAsync(json, model, useStreaming, cancellationToken);

            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.GenerateResponseAsync] HTTP success: {response.StatusCode}");

            var result = useStreaming
                ? await ReadStreamingResponseAsync(response, textDeltaCallback!, cancellationToken, rawStreamRecorder)
                : await ReadNonStreamingResponseAsync(response, textDeltaCallback, cancellationToken);
            accounting?.Complete(result);
            return result;
        }
        catch (HttpRequestException ex)
        {
            throw HandleHttpException(ex);
        }
        catch (Exception ex)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR, $"[LlamaCppHttpProvider.GenerateResponseAsync] Error generating response with llama.cpp provider: {ex.Message}");
            AutomatonDebug.LogException("LlamaCppHttpProvider", "GenerateResponseAsync", ex);
            throw;
        }
        finally
        {
            if (gateEntered)
            {
                _requestGate.Release();
            }
        }
    }

    /// <summary>The request body for this provider's API format.</summary>
    private protected virtual string BuildRequestJson(
        List<Message> messages,
        Dictionary<string, JsonSchema> tools,
        string model,
        double? temperature,
        int? maxTokens,
        bool useStreaming)
    {
        // Validate and sanitize: ensure only one system prompt at position 0
        return SanitizeRequestJson(BuildChatRequestPayload(messages, tools, model, temperature, maxTokens, useStreaming));
    }

    /// <summary>
    /// The request fields a 400 may name as refused and that can be taken off the request.
    /// Only the configured extras: what the request needs is never dropped.
    /// </summary>
    private protected virtual IEnumerable<string> OptionalRequestFields() =>
        _requestParameters.Keys.Where(key => !ReservedRequestParameterKeys.Contains(key));

    private string BuildChatRequestPayload(
        List<Message> messages,
        Dictionary<string, JsonSchema> tools,
        string model,
        double? temperature,
        int? maxTokens,
        bool useStreaming)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] BuildChatRequestPayload: Entry - {messages?.Count ?? 0} messages, {tools?.Count ?? 0} tools, model={model}");

        var openAiMessages = ConvertMessages(messages, model);
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] BuildChatRequestPayload: Converted {openAiMessages.Count} messages");
        for (var i = 0; i < openAiMessages.Count; i++)
        {
            var msg = openAiMessages[i];
            var toolCallStr = msg.ToolCalls != null && msg.ToolCalls.Count > 0
                ? $", toolCalls={msg.ToolCalls.Count} [{string.Join(", ", msg.ToolCalls.Select(t => $"{t.Function?.Name}({t.Id})"))}]"
                : "";
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] BuildChatRequestPayload: OpenAI msg[{i}] role={msg.Role}, contentLen={msg.ContentAsText?.Length ?? 0}{toolCallStr}");
        }

        var openAiTools = tools.Select(kvp => new
        {
            type = "function",
            function = new
            {
                name = kvp.Key,
                description = !string.IsNullOrEmpty(kvp.Value.Description)
                    ? kvp.Value.Description
                    : (kvp.Value.Properties.TryGetValue("description", out var descProp)
                        ? descProp.Description : string.Empty),
                parameters = ConvertSchema(kvp.Value)
            }
        }).ToList();
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] BuildChatRequestPayload: Converted {openAiTools.Count} tools");

        var requestBody = new Dictionary<string, object?>
        {
            ["model"] = model,
            ["messages"] = openAiMessages,
            ["tools"] = openAiTools.Count > 0 ? openAiTools : null,
            ["tool_choice"] = openAiTools.Count > 0 ? "auto" : null,
            ["temperature"] = temperature,
            [RejectedRequestArguments.TokenLimitField(_baseUrl, model)] = maxTokens,
            ["stream"] = useStreaming
        };
        ApplyRequestParameters(requestBody, model);

        // Off the body altogether: a dictionary's nulls are written out, not ignored.
        if (RejectedRequestArguments.IsRejected(_baseUrl, model, "temperature"))
        {
            requestBody.Remove("temperature");
        }

        var json = JsonSerializer.Serialize(requestBody, new JsonSerializerOptions
        {
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
        });

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] BuildChatRequestPayload: JSON payload length={json.Length}");
        Debug.Log($"[PROVIDERHTTPCHECK] Request payload: {json}");
        return json;
    }

    private async Task<HttpResponseMessage> SendRequestWithRetryAsync(
        string json,
        string model,
        bool useStreaming,
        CancellationToken cancellationToken)
    {
        if (string.IsNullOrEmpty(json))
        {
            throw new ArgumentException("Request JSON must not be empty.", nameof(json));
        }

        var completionsUrl = $"{_baseUrl}/{RequestPath}";
        var attempt = 0;
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.SendRequestWithRetryAsync] Entry - url={completionsUrl}, streaming={useStreaming}");

        while (true)
        {
            attempt++;
            cancellationToken.ThrowIfCancellationRequested();
            ExecutionBudgetScope.Current?.BeforeModelRequest();

            HttpResponseMessage response;
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Post, completionsUrl)
                {
                    Content = new StringContent(json, Encoding.UTF8, "application/json")
                };
                request.Options.Set(ModelResponsePolicy.StreamingResponse, useStreaming);
                if (_borrowedClient && !string.IsNullOrEmpty(_apiKey))
                    request.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", _apiKey);

                AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.SendRequestWithRetryAsync] Attempt {attempt} - sending request to {completionsUrl}, streaming: {useStreaming}, content: {json}");
                response = await _httpClient.SendAsync(
                    request,
                    useStreaming ? HttpCompletionOption.ResponseHeadersRead : HttpCompletionOption.ResponseContentRead,
                    cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                // Caller (user/host) cancelled — never retry a deliberate cancellation.
                throw;
            }
            catch (Exception ex) when (TransientErrorPolicy.IsTransientException(ex))
            {
                // Connection refused/reset, DNS hiccup, or HttpClient timeout — typically a
                // provider restart or a blip. Keep going rather than bailing out.
                var delay = TransientErrorPolicy.ComputeBackoffDelay(attempt);
                AutomatonDebug.LogWarning($"[LlamaCppHttpProvider.SendRequestWithRetryAsync] Transient network error on attempt {attempt} ({ex.GetType().Name}: {ex.Message}), retrying in {delay.TotalSeconds:0.#}s...");
                await Task.Delay(delay, cancellationToken);
                continue;
            }

            if (response.IsSuccessStatusCode)
            {
                AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.SendRequestWithRetryAsync] Exit - success on attempt {attempt}, status={response.StatusCode}");
                return response;
            }

            var statusCode = response.StatusCode;
            var retryAfter = response.Headers.RetryAfter?.Delta;
            string errorContent;
            using (response) errorContent = await response.Content.ReadAsStringAsync(cancellationToken);
            AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR, $"[LlamaCppHttpProvider.SendRequestWithRetryAsync] HTTP error {statusCode} on attempt {attempt}: {errorContent}");

            if (TransientErrorPolicy.IsTransientHttpStatus(statusCode))
            {
                var delay = retryAfter ?? TransientErrorPolicy.ComputeBackoffDelay(attempt);
                AutomatonDebug.LogWarning($"[LlamaCppHttpProvider.SendRequestWithRetryAsync] Transient HTTP status {(int)statusCode} on attempt {attempt}, retrying in {delay.TotalSeconds:0.#}s...");
                await Task.Delay(delay, cancellationToken);
                continue;
            }

            // A strict endpoint refuses a field it does not take, and names it. The field is
            // ours to change, so the request goes again without it, or under the name asked for.
            if (statusCode == System.Net.HttpStatusCode.BadRequest &&
                RejectedRequestArguments.TryRevise(
                    _baseUrl,
                    model,
                    json,
                    errorContent,
                    OptionalRequestFields(),
                    out var revised,
                    out var refused))
            {
                AutomatonDebug.LogWarning($"[LlamaCppHttpProvider.SendRequestWithRetryAsync] {_baseUrl} refused {string.Join(", ", refused)} for {model}; changing the request, and keeping the change for this model from now on");
                json = revised;
                continue;
            }

            // Non-transient (e.g. 400 malformed, 401/403 auth, 404). Retrying would just
            // hang forever, so surface it for the caller to act on. The status code is
            // attached so upstream retry logic can tell this apart from a connection drop.
            AutomatonDebug.LogError($"[LlamaCppHttpProvider.SendRequestWithRetryAsync] Non-transient HTTP status {(int)statusCode}, surfacing error after {attempt} attempt(s)");

            // 413 nearly always means a reverse proxy in front of the model rejected the
            // body, not the model itself - and its stock response is an HTML error page,
            // which is useless in a chat bubble. Say what actually needs changing.
            if (statusCode == System.Net.HttpStatusCode.RequestEntityTooLarge)
            {
                throw new HttpRequestException(
                    $"The endpoint at {completionsUrl} rejected the request as too large " +
                    $"({FormatBytes(json.Length)} sent). This is usually a reverse proxy limit rather than the model: " +
                    "nginx defaults 'client_max_body_size' to 1m. Either raise it on the server " +
                    "(e.g. 'client_max_body_size 25m;') or send fewer/smaller attachments.",
                    null,
                    statusCode);
            }

            throw new HttpRequestException($"HTTP {statusCode} from {completionsUrl}: {errorContent}", null, statusCode);
        }
    }

    private static string FormatBytes(long bytes)
    {
        if (bytes >= 1024L * 1024) return $"{bytes / (1024.0 * 1024):F1} MB";
        if (bytes >= 1024) return $"{bytes / 1024.0:F0} KB";
        return $"{bytes} B";
    }

    private HttpRequestException HandleHttpException(HttpRequestException ex)
    {
        // A definite HTTP status response (e.g. 401/400) is not a connection-level
        // problem — pass it through unchanged so its StatusCode survives for the
        // retry policy and it isn't misreported as a timeout/host error.
        if (ex.StatusCode.HasValue)
        {
            return ex;
        }

        var message = ex.Message;

        if (message.Contains("invalid status line"))
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR, $"[LlamaCppHttpProvider] Llama.cpp server disconnected or returned invalid response. Is the server running? Error: {message}");
            return new HttpRequestException($"Llama.cpp server error: Connection lost or server returned invalid response. Please ensure the server is running and try again.", ex);
        }

        if (message.Contains("No such host"))
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR, $"[LlamaCppHttpProvider] Cannot connect to llama.cpp server. Host not found: {message}");
            return new HttpRequestException($"Cannot connect to llama.cpp server. Please ensure the server is running at {_baseUrl}.", ex);
        }

        if (message.Contains("timeout") || message.Contains("Timeout"))
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR, $"[LlamaCppHttpProvider] Request to llama.cpp server timed out: {message}");
            return new HttpRequestException("Llama.cpp server request timed out. The model may be processing a long response.", ex);
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR, $"[LlamaCppHttpProvider] HTTP error: {message}");
        return ex;
    }

    private async Task<AIResponse> ReadNonStreamingResponseAsync(
        HttpResponseMessage response,
        Action<string>? textDeltaCallback,
        CancellationToken cancellationToken)
    {
        var responseJson = await response.Content.ReadAsStringAsync(cancellationToken);
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.ReadNonStreamingResponseAsync] Response received, length: {responseJson.Length}");

        var aiResponse = ParseResponse(responseJson);

        if (textDeltaCallback != null)
        {
            // Surface out-of-band reasoning to the live UI as a `<think>...</think>`
            // block so the live ChatMessage.Content matches what AppendAssistantMessageContent
            // reconstructs from Parts.reasoning on a later transcript reload.
            // Without this, live Content lacks the think block while reload-Content has
            // it, and AreCompatibleRemoteMessages fails the prefix check, triggering
            // a full ReplaceAll + scroll-to-top + reformat.
            if (!string.IsNullOrEmpty(aiResponse.Reasoning))
            {
                textDeltaCallback($"<think>{Environment.NewLine}{aiResponse.Reasoning.Trim()}{Environment.NewLine}</think>{Environment.NewLine}{Environment.NewLine}");
            }

            if (!string.IsNullOrEmpty(aiResponse.Text))
            {
                textDeltaCallback(aiResponse.Text);
            }
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.ReadNonStreamingResponseAsync] llama.cpp response generated with {aiResponse.ToolCalls.Count} tool calls");
        return aiResponse;
    }

    /// <summary>A whole (non-streamed) reply in this provider's API format.</summary>
    private protected virtual AIResponse ParseResponse(string responseJson)
    {
        var responseObj = JsonSerializer.Deserialize<OpenAIResponse>(responseJson);

        if (responseObj == null)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR, $"[LlamaCppHttpProvider.ReadNonStreamingResponseAsync] Failed to deserialize response");
            throw new InvalidOperationException("Failed to deserialize response");
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.ReadNonStreamingResponseAsync] Deserialized response successfully");
        return CreateAiResponse(responseObj);
    }

    private static string NormalizeBaseUrl(string baseUrl)
    {
        // A base URL pasted with its endpoint path still on it, in either format.
        var normalized = baseUrl.Trim().TrimEnd('/');
        foreach (var endpointPath in new[] { "/chat/completions", "/responses" })
        {
            if (normalized.EndsWith(endpointPath, StringComparison.OrdinalIgnoreCase))
            {
                normalized = normalized[..^endpointPath.Length].TrimEnd('/');
            }
        }

        return normalized;
    }

    private static bool IsDeepSeekEndpoint(string baseUrl)
    {
        return Uri.TryCreate(baseUrl, UriKind.Absolute, out var uri) &&
               uri.Host.EndsWith("deepseek.com", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// True only for the DeepSeek models that actually round-trip a reasoning channel
    /// and reject assistant tool calls that arrive without it.
    /// </summary>
    /// <remarks>
    /// This deliberately keys on the model rather than the endpoint. Gating on the host
    /// alone caught every model served from deepseek.com, including non-reasoning ones
    /// such as deepseek-chat and deepseek-v4-flash. Those never emit reasoning content,
    /// so the "reasoning is missing" guard below stripped every tool call they made,
    /// the matching tool results were dropped as orphans, and the model was handed a
    /// history in which it had never called anything - so it called the same tool again,
    /// every round trip, until it hit the round-trip limit.
    /// </remarks>
    private static bool RequiresDeepSeekReasoning(string baseUrl, string? model)
    {
        if (!IsDeepSeekEndpoint(baseUrl))
        {
            return false;
        }

        if (string.IsNullOrWhiteSpace(model))
        {
            // Unknown model on a DeepSeek endpoint: assume it does not need the reasoning
            // channel. Sending a tool call that is not wanted produces a clear API error;
            // silently deleting one produces an unbreakable loop.
            AutomatonDebug.LogWarning("[PROVIDERHTTPCHECK] RequiresDeepSeekReasoning: no model name supplied; assuming reasoning is not required");
            return false;
        }

        var requires = model.Contains("reasoner", StringComparison.OrdinalIgnoreCase) ||
                       model.Contains("-r1", StringComparison.OrdinalIgnoreCase) ||
                       model.StartsWith("r1", StringComparison.OrdinalIgnoreCase);

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] RequiresDeepSeekReasoning: model={model}, requires={requires}");
        return requires;
    }

    private static readonly HashSet<string> ReservedRequestParameterKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "model",
        "messages",
        "tools",
        "tool_choice",
        "stream",
        "temperature",
        "max_tokens"
    };

    private void ApplyRequestParameters(Dictionary<string, object?> requestBody, string model)
    {
        foreach (var parameter in _requestParameters)
        {
            if (string.IsNullOrWhiteSpace(parameter.Key) ||
                ReservedRequestParameterKeys.Contains(parameter.Key) ||
                RejectedRequestArguments.IsRejected(_baseUrl, model, parameter.Key))
            {
                continue;
            }

            requestBody[parameter.Key] = parameter.Value;
        }
    }

    private string SanitizeRequestJson(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;

        if (!root.TryGetProperty("messages", out var messagesProp) || messagesProp.ValueKind != JsonValueKind.Array)
        {
            return json;
        }

        var messageList = messagesProp.EnumerateArray().ToList();
        int? firstSystemIndex = null;
        var systemMessageContent = new List<KeyValuePair<string, JsonElement>>();

        for (int i = 0; i < messageList.Count; i++)
        {
            var msg = messageList[i];
            if (msg.TryGetProperty("role", out var roleProp) && roleProp.ValueKind == JsonValueKind.String)
            {
                var role = roleProp.GetString();
                if (role == "system")
                {
                    if (!firstSystemIndex.HasValue)
                    {
                        firstSystemIndex = i;
                    }
                    // Collect content from all system messages
                    if (msg.TryGetProperty("content", out var contentProp))
                    {
                        systemMessageContent.Add(new KeyValuePair<string, JsonElement>($"system_{i}", contentProp));
                    }
                }
            }
        }

        // No system messages at all - nothing to do
        if (!firstSystemIndex.HasValue)
        {
            return json;
        }

        // Check if there are any OTHER system messages after the first one
        bool hasMultipleSystemMessages = systemMessageContent.Count > 1;
        if (!hasMultipleSystemMessages)
        {
            for (int i = 0; i < messageList.Count; i++)
            {
                if (i != firstSystemIndex.Value)
                {
                    var msg = messageList[i];
                    if (msg.TryGetProperty("role", out var r) &&
                        r.ValueKind == JsonValueKind.String &&
                        r.GetString() == "system")
                    {
                        hasMultipleSystemMessages = true;
                        break;
                    }
                }
            }
        }

        // If first system is at index 0 and there are no duplicates, nothing to do
        if (firstSystemIndex == 0 && !hasMultipleSystemMessages)
        {
            return json;
        }

        // Filter out all system messages
        var nonSystemMessages = messageList
            .Where((msg, idx) => idx != firstSystemIndex)
            .Where(msg => !(msg.TryGetProperty("role", out var r) && r.ValueKind == JsonValueKind.String && r.GetString() == "system"))
            .ToList();

        // Build combined system prompt, deduplicating identical content
        var uniqueContents = systemMessageContent
            .Select(kvp => kvp.Value.ValueKind == JsonValueKind.String ? kvp.Value.GetString() ?? "" : kvp.Value.GetRawText())
            .Where(s => !string.IsNullOrEmpty(s))
            .Distinct(StringComparer.Ordinal)
            .ToList();

        var combinedContent = uniqueContents.Count > 1
            ? string.Join("\n\n---\n\n", uniqueContents)
            : uniqueContents.FirstOrDefault() ?? "";

        var newMessages = new JsonArray();

        // Add combined system message at position 0
        newMessages.Add(new JsonObject { ["role"] = "system", ["content"] = combinedContent });

        // Add all non-system messages
        foreach (var msg in nonSystemMessages)
        {
            var messageNode = JsonNode.Parse(msg.GetRawText());
            if (messageNode != null)
            {
                newMessages.Add(messageNode);
            }
        }

        var requestNode = JsonNode.Parse(json)?.AsObject();
        if (requestNode == null)
        {
            return json;
        }

        requestNode["messages"] = newMessages;

        var newJson = requestNode.ToJsonString(new JsonSerializerOptions
        {
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
        });

        var dedupNote = uniqueContents.Count != systemMessageContent.Count
            ? $" (deduplicated {systemMessageContent.Count} -> {uniqueContents.Count})"
            : "";
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.SanitizeRequestJson] Moved system prompt to index 0{dedupNote}. Original length: {json.Length}, New length: {newJson.Length}");
        return newJson;
    }

    private async Task<AIResponse> ReadStreamingResponseAsync(
        HttpResponseMessage response,
        Action<string> textDeltaCallback,
        CancellationToken cancellationToken,
        RawStreamRecorder? rawStreamRecorder = null)
    {
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        using var reader = new StreamReader(stream);

        var aiResponse = new AIResponse();
        var contentBuilder = new StringBuilder();
        var reasoningBuilder = new StringBuilder();
        var toolCalls = new Dictionary<int, StreamingToolCallBuilder>();
        var liveThinkState = new LiveThinkBlockState();
        var loopMonitor = new StreamingLoopMonitor();

        while (!cancellationToken.IsCancellationRequested)
        {
            var lineTimeout = string.IsNullOrEmpty(aiResponse.FinishReason)
                ? StreamingIdleTimeout
                : StreamingCompletionGracePeriod;

            string? line;
            try
            {
                line = await ReadLineWithTimeoutAsync(reader, lineTimeout, cancellationToken);
            }
            catch (TimeoutException) when (HasStreamingPayload(contentBuilder, reasoningBuilder, toolCalls, aiResponse))
            {
                AutomatonDebug.Log(
                    AutomatonDebug.DEBUG_INFO,
                    $"[LlamaCppHttpProvider.ReadStreamingResponseAsync] Stream went idle for {lineTimeout.TotalSeconds:F0}s after partial payload; finalizing without explicit terminator.");
                break;
            }

            if (line == null)
            {
                break;
            }

            // Diagnostic capture: when AUTOMATON_DUMP_SSE points at a file path,
            // append the verbatim raw SSE line. Opt-in and fire-and-forget so it
            // can never perturb the live stream. Use this to confirm a model's
            // exact wire layout (e.g. whether Minimax M3 emits reasoning on both
            // `reasoning_content` AND inline `<think>` content). Disabled unless
            // the env var is set.
            DumpRawSseLine(line);

            // M2 (XCHAT): record the raw line for the future replay path. Fire-and-
            // forget for legacy recorder failures. An explicit host resource limit
            // must stop the call rather than return a silently truncated replay.
            // Other recorder failures must never perturb the live decode/apply
            // stream the user is already seeing. Nulls are excluded by the break
            // above; the line is the verbatim byte string off the socket, including
            // blank SSE separators and the `data: [DONE]` terminator.
            if (rawStreamRecorder != null)
            {
                try
                {
                    rawStreamRecorder.RecordLine(line);
                }
                catch (ModelResponseLimitException) { throw; }
                catch (Exception recorderEx)
                {
                    AutomatonDebug.LogWarning($"[XCHAT-RECORDER] RawStreamRecorder.RecordLine failed; continuing without recording further lines this stream. ex={recorderEx.GetType().Name}: {recorderEx.Message}");
                    rawStreamRecorder = null;
                }
            }

            // This loop is pure IO: it only pulls raw lines off the socket. All wire
            // decoding (SSE framing, [DONE], JSON -> typed deltas) lives in the pure
            // OpenAIStreamDecoder in Automaton.Core.Streaming, and all side effects
            // (accumulators, the live `<think>` flush, textDeltaCallback) live in
            // LlmStreamApplier. Keeping those three concerns separate is what lets
            // a future remote client replay a recorded byte buffer through the
            // *same* decoder + apply path and get byte-identical state — no
            // reconciliation needed. See XCHAT.md §3, §7 #4.
            var streamComplete = false;
            var loopAborted = false;
            foreach (var streamEvent in OpenAIStreamDecoder.DecodeLine(line))
            {
                if (streamEvent is StreamDoneEvent)
                {
                    streamComplete = true;
                    break;
                }

                LlmStreamApplier.Apply(
                    streamEvent,
                    contentBuilder,
                    reasoningBuilder,
                    toolCalls,
                    aiResponse,
                    textDeltaCallback,
                    liveThinkState);

                // Loop watch runs in the IO loop, never inside Apply — see the
                // StreamingLoopMonitor class docs for why the apply step has to
                // stay a pure fold. Observed after Apply so SawOutOfBandReasoning
                // reflects this event.
                if (ObserveForLoop(streamEvent, loopMonitor, liveThinkState))
                {
                    loopAborted = true;
                    break;
                }
            }

            await OutboundFlowScope.WaitAsync(cancellationToken).ConfigureAwait(false);

            if (loopAborted)
            {
                // Discard any partially-streamed tool calls. Their JSON arguments
                // are truncated and their call ids would never be answered, which
                // breaks call/result pairing on the next request.
                var discarded = toolCalls.Count;
                toolCalls.Clear();

                aiResponse.LoopAborted = true;
                aiResponse.LoopAbortDiagnosis = loopMonitor.Diagnosis;
                aiResponse.FinishReason = "loop_detected";

                AutomatonDebug.LogWarning($"[LlamaCppHttpProvider.ReadStreamingResponseAsync] Aborting stream: model is looping. {loopMonitor.Diagnosis}. Discarded {discarded} partial tool call(s); keeping {contentBuilder.Length} content chars and {reasoningBuilder.Length} reasoning chars.");
                break;
            }

            if (streamComplete)
            {
                break;
            }
        }

        // If dual-channel inline-`<think>` stripping was active, emit any tail
        // the stripper held back (a false partial tag that turned out to be
        // literal content). Must run before the reasoning flush so ordering of
        // any flushed think block vs. trailing content matches the live stream.
        LlmStreamApplier.FlushInlineThinkCarry(liveThinkState, contentBuilder, textDeltaCallback);

        // Stream ended without visible content following the reasoning (e.g. a
        // thinking-only response, or content arrived only as tool calls). Flush
        // the buffered reasoning so the live UI still shows the think block.
        LlmStreamApplier.FlushPendingReasoningBlock(liveThinkState, textDeltaCallback);

        FinalizeStreamingResponse(aiResponse, contentBuilder, reasoningBuilder, toolCalls);

        // The HTTP status was 200, but the endpoint said in the stream that the reply failed.
        // A partial reply is not an answer: say what the endpoint said.
        if (!string.IsNullOrWhiteSpace(aiResponse.StreamError))
        {
            AutomatonDebug.LogError($"[LlamaCppHttpProvider.ReadStreamingResponseAsync] The endpoint failed the reply mid-stream: {aiResponse.StreamError}");
            throw new InvalidOperationException($"The model endpoint at {_baseUrl} failed the reply: {aiResponse.StreamError}");
        }

        if (aiResponse.Text == null)
        {
            throw new InvalidOperationException("AI response text is null after streaming.");
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.ReadStreamingResponseAsync] Stream completed with {aiResponse.Text.Length} text chars and {aiResponse.ToolCalls.Count} tool calls, finishReason={aiResponse.FinishReason}, truncated={aiResponse.Truncated}, loopAborted={aiResponse.LoopAborted}");
        return aiResponse;
    }

    // ========================================================================
    // Wire decode (pure) vs. apply (side effects)
    //
    // The streaming pipeline is split into three concerns so the remote "replay
    // a recorded byte buffer through the same decoder" design is reachable:
    //   1. IO            — ReadStreamingResponseAsync's loop reads raw lines.
    //   2. Decode (pure) — OpenAIStreamDecoder (Automaton.Core.Streaming) turns
    //                      a raw SSE line into typed LlmStreamEvents. No socket,
    //                      no callbacks, no mutable accumulators. Same bytes in
    //                      => same events out.
    //   3. Apply         — LlmStreamApplier (Automaton.Core.Streaming) folds
    //                      events into the accumulators and drives the live
    //                      `<think>` flush + textDeltaCallback.
    //
    // A future remote client can record the raw response bytes, then feed them
    // back through OpenAIStreamDecoder + LlmStreamApplier and arrive at byte-
    // identical state as the owner — which is what removes the need for the
    // transcript reconciliation layer entirely. See XCHAT.md §3, §7 #4.
    // ========================================================================

    /// <summary>
    /// Feeds one decoded event to the live loop monitor and reports whether it
    /// tripped detection.
    ///
    /// Channel routing matters here. Out-of-band reasoning is always analyzed.
    /// Content is analyzed only while the stream has *not* delivered reasoning
    /// out-of-band: models that do both (e.g. Minimax M3) mirror identical
    /// reasoning on the content channel as inline <c>&lt;think&gt;</c>, and
    /// counting the same text on both channels would manufacture a false loop.
    /// For inline-only models the content channel is the only place reasoning
    /// appears, and the monitor picks out just the think spans — visible answer
    /// text is never abortable.
    /// </summary>
    private static bool ObserveForLoop(
        LlmStreamEvent streamEvent,
        StreamingLoopMonitor loopMonitor,
        LiveThinkBlockState liveThinkState)
    {
        if (streamEvent == null || loopMonitor == null || liveThinkState == null)
        {
            return false;
        }

        if (streamEvent is ReasoningDeltaEvent reasoning)
        {
            return loopMonitor.ObserveReasoning(reasoning.Text);
        }

        if (streamEvent is ContentDeltaEvent content && !liveThinkState.SawOutOfBandReasoning)
        {
            return loopMonitor.ObserveContent(content.Text);
        }

        return false;
    }

    private static void FinalizeStreamingResponse(
        AIResponse aiResponse,
        StringBuilder contentBuilder,
        StringBuilder reasoningBuilder,
        Dictionary<int, StreamingToolCallBuilder> toolCalls)
    {
        if (aiResponse == null)
        {
            throw new ArgumentNullException(nameof(aiResponse));
        }

        if (contentBuilder == null)
        {
            throw new ArgumentNullException(nameof(contentBuilder));
        }

        if (reasoningBuilder == null)
        {
            throw new ArgumentNullException(nameof(reasoningBuilder));
        }

        if (toolCalls == null)
        {
            throw new ArgumentNullException(nameof(toolCalls));
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.FinalizeStreamingResponse] Entry - finishReason={aiResponse.FinishReason ?? "(none)"}, contentChars={contentBuilder.Length}, reasoningChars={reasoningBuilder.Length}, partialToolCalls={toolCalls.Count}");

        var (visibleText, inlineReasoning) = SplitInlineThinking(contentBuilder.ToString());
        aiResponse.Text = visibleText;
        aiResponse.Reasoning = reasoningBuilder.Length == 0 ? inlineReasoning : reasoningBuilder.ToString();

        // `length` means the model hit its max_tokens budget mid-generation — it
        // did not choose to stop. Flag it so the agent loop can tell this apart
        // from a finished turn instead of treating an empty response as "done".
        aiResponse.Truncated = string.Equals(aiResponse.FinishReason, "length", StringComparison.OrdinalIgnoreCase);

        var droppedToolCalls = 0;
        foreach (var toolCall in toolCalls.OrderBy(kvp => kvp.Key)
                                          .Select(kvp => kvp.Value.ToToolCallRequest())
                                          .Where(call => call != null))
        {
            // A call cut off mid-arguments carries unparseable JSON and a call id
            // that can never be answered, which breaks call/result pairing on the
            // next request. Only discard the ones that genuinely failed to close —
            // a truncated turn can still contain earlier, complete calls.
            if (aiResponse.Truncated && !HasCompleteArguments(toolCall!))
            {
                droppedToolCalls++;
                aiResponse.TruncatedToolCalls.Add(toolCall!.Tool);
                AutomatonDebug.LogWarning($"[LlamaCppHttpProvider.FinalizeStreamingResponse] Discarding tool call '{toolCall!.Tool}' (id={toolCall.Id}): arguments truncated by the token limit and do not parse as JSON.");
                continue;
            }

            aiResponse.ToolCalls.Add(toolCall!);
        }

        if (aiResponse.Truncated)
        {
            AutomatonDebug.LogWarning($"[LlamaCppHttpProvider.FinalizeStreamingResponse] Response TRUNCATED at the max_tokens limit (finish_reason=length). Kept {aiResponse.Text?.Length ?? 0} visible text chars and {aiResponse.Reasoning?.Length ?? 0} reasoning chars; kept {aiResponse.ToolCalls.Count} tool call(s), discarded {droppedToolCalls} incomplete. Raise llm.maxTokens if this recurs.");
        }

        if (string.IsNullOrEmpty(aiResponse.FinishReason))
        {
            aiResponse.FinishReason = "stop";
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[LlamaCppHttpProvider.FinalizeStreamingResponse] Exiting - finishReason={aiResponse.FinishReason}, truncated={aiResponse.Truncated}, toolCalls={aiResponse.ToolCalls.Count}");
    }

    /// <summary>
    /// Reports whether a streamed tool call's accumulated arguments form complete,
    /// parseable JSON. Used to tell a call that finished before the token limit
    /// from one that was sliced through the middle of its argument object.
    /// </summary>
    private static bool HasCompleteArguments(ToolCallRequest toolCall)
    {
        if (toolCall == null)
        {
            return false;
        }

        if (string.IsNullOrWhiteSpace(toolCall.Arguments))
        {
            // ToToolCallRequest normalizes "no arguments streamed" to "{}", so an
            // empty string here means nothing usable arrived at all.
            return false;
        }

        try
        {
            using var _ = JsonDocument.Parse(toolCall.Arguments);
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    // Opt-in raw SSE capture. No-op unless AUTOMATON_DUMP_SSE is set to a file
    // path. Best-effort: any failure is swallowed so diagnostics never break a
    // live response. Each line is written verbatim (one per line) so the dump
    // can be replayed or eyeballed to confirm a model's reasoning/content wire
    // layout.
    private static void DumpRawSseLine(string line)
    {
        var path = Environment.GetEnvironmentVariable("AUTOMATON_DUMP_SSE");
        if (string.IsNullOrWhiteSpace(path))
        {
            return;
        }

        try
        {
            File.AppendAllText(path, line + Environment.NewLine);
        }
        catch
        {
            // Diagnostics must never perturb the live stream.
        }
    }

    private static async Task<string?> ReadLineWithTimeoutAsync(
        StreamReader reader,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        try
        {
            return await reader.ReadLineAsync(cancellationToken).AsTask().WaitAsync(timeout, cancellationToken);
        }
        catch (TimeoutException ex)
        {
            throw new TimeoutException(
                $"Timed out waiting {timeout.TotalSeconds:F0}s for the next streaming response chunk.",
                ex);
        }
    }

    private static bool HasStreamingPayload(
        StringBuilder contentBuilder,
        StringBuilder reasoningBuilder,
        IReadOnlyDictionary<int, StreamingToolCallBuilder> toolCalls,
        AIResponse aiResponse)
    {
        return contentBuilder.Length > 0 ||
               reasoningBuilder.Length > 0 ||
               toolCalls.Count > 0 ||
               !string.IsNullOrWhiteSpace(aiResponse.FinishReason);
    }

    private static AIResponse CreateAiResponse(OpenAIResponse responseObj)
    {
        var aiResponse = new AIResponse
        {
            FinishReason = responseObj.Choices?.FirstOrDefault()?.FinishReason ?? "unknown"
        };

        var firstChoice = responseObj.Choices?.FirstOrDefault();
        if (firstChoice?.Message?.ContentAsText != null)
        {
            var (visibleText, inlineReasoning) = SplitInlineThinking(firstChoice.Message.ContentAsText);
            aiResponse.Text = visibleText;
            aiResponse.Reasoning = firstChoice.Message.ReasoningContent
                                   ?? firstChoice.Message.Reasoning
                                   ?? inlineReasoning;
        }

        aiResponse.Reasoning ??= firstChoice?.Message?.ReasoningContent ?? firstChoice?.Message?.Reasoning;

        // Same max_tokens truncation check as the streaming path. Non-streaming
        // responses arrive whole, so tool call arguments are never half-written
        // here — only the flag is needed, not the argument salvage.
        aiResponse.Truncated = string.Equals(aiResponse.FinishReason, "length", StringComparison.OrdinalIgnoreCase);
        if (aiResponse.Truncated)
        {
            AutomatonDebug.LogWarning($"[LlamaCppHttpProvider.CreateAiResponse] Response TRUNCATED at the max_tokens limit (finish_reason=length), textChars={aiResponse.Text?.Length ?? 0}, reasoningChars={aiResponse.Reasoning?.Length ?? 0}. Raise llm.maxTokens if this recurs.");
        }

        ExtractToolCalls(responseObj, aiResponse);
        ExtractUsage(responseObj, aiResponse);

        return aiResponse;
    }

    private static void ExtractToolCalls(OpenAIResponse responseObj, AIResponse aiResponse)
    {
        var toolCalls = responseObj.Choices?.FirstOrDefault()?.Message?.ToolCalls;
        if (toolCalls == null)
        {
            return;
        }

        foreach (var toolCall in toolCalls)
        {
            if (toolCall.Function?.Name == null)
            {
                continue;
            }

            aiResponse.ToolCalls.Add(new ToolCallRequest
            {
                Id = toolCall.Id,
                Tool = toolCall.Function.Name,
                Arguments = ExtractToolCallArguments(toolCall.Function)
            });
        }
    }

    private static string ExtractToolCallArguments(OpenAIFunctionCall function)
    {
        if (function.Arguments == null)
        {
            return "{}";
        }

        if (function.Arguments.Value.ValueKind == JsonValueKind.String)
        {
            return function.Arguments.Value.GetString() ?? "{}";
        }

        return function.Arguments.Value.GetRawText();
    }

    private static void ExtractUsage(OpenAIResponse responseObj, AIResponse aiResponse)
    {
        if (responseObj.Usage == null)
        {
            return;
        }

        aiResponse.Usage = new AIUsage
        {
            InputTokens = responseObj.Usage.PromptTokens,
            OutputTokens = responseObj.Usage.CompletionTokens,
            TotalTokens = responseObj.Usage.TotalTokens
        };
    }

    private List<OpenAIMessage> ConvertMessages(List<Message> messages, string? model = null)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertMessages: Entry - {messages?.Count ?? 0} messages, model={model ?? "(none)"}");
        var result = new List<OpenAIMessage>();
        var requiresDeepSeekReasoning = RequiresDeepSeekReasoning(_baseUrl, model);
        var lastUserMessageIndex = requiresDeepSeekReasoning
            ? messages.FindLastIndex(message => string.Equals(message.Role, "user", StringComparison.OrdinalIgnoreCase))
            : -1;

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertMessages: requiresDeepSeekReasoning={requiresDeepSeekReasoning}, lastUserMessageIndex={lastUserMessageIndex}");

        var (validToolCallIds, skippedDeepSeekToolCallIds) = CollectValidToolCallIds(messages, requiresDeepSeekReasoning, lastUserMessageIndex);
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertMessages: validToolCallIds count={validToolCallIds.Count}, skippedDeepSeekToolCallIds count={skippedDeepSeekToolCallIds.Count}");

        // Tool calls that actually have a matching tool result. An assistant
        // tool call without one (e.g. the user interrupted while it was still
        // pending) is an orphan that providers like MiniMax reject with
        // "tool call result does not follow tool call (2013)".
        var toolResultCallIds = CollectToolResultCallIds(messages);
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertMessages: toolResultCallIds count={toolResultCallIds.Count}");

        for (var messageIndex = 0; messageIndex < messages.Count; messageIndex++)
        {
            ConvertMessageToOpenAi(
                messages[messageIndex], messageIndex,
                requiresDeepSeekReasoning, lastUserMessageIndex,
                validToolCallIds, skippedDeepSeekToolCallIds, toolResultCallIds,
                result);
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertMessages: Exit - {result.Count} OpenAI messages produced");
        return result;
    }

    private (HashSet<string> Valid, HashSet<string> Skipped) CollectValidToolCallIds(
        List<Message> messages,
        bool requiresDeepSeekReasoning,
        int lastUserMessageIndex)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] CollectValidToolCallIds: Entry - requiresDeepSeekReasoning={requiresDeepSeekReasoning}");
        var validToolCallIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var skippedDeepSeekToolCallIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        for (var messageIndex = 0; messageIndex < messages.Count; messageIndex++)
        {
            var msg = messages[messageIndex];
            if (msg.Role != "assistant")
            {
                continue;
            }

            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] CollectValidToolCallIds: messageIndex={messageIndex}, found assistant message, parts count={msg.Parts?.Count ?? 0}");
            var reasoningContent = ExtractReasoningContent(msg);
            var isHistoricalDeepSeekMessage = requiresDeepSeekReasoning &&
                                              lastUserMessageIndex >= 0 &&
                                              messageIndex < lastUserMessageIndex;
            var skipDeepSeekToolCalls = requiresDeepSeekReasoning &&
                                        !isHistoricalDeepSeekMessage &&
                                        string.IsNullOrWhiteSpace(reasoningContent);
            foreach (var part in msg.Parts)
            {
                if (part.Type != "tool" || string.IsNullOrWhiteSpace(part.CallId))
                {
                    AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] CollectValidToolCallIds: Skipping part - type={part.Type}, callId=[{part.CallId ?? "null/empty"}]");
                    continue;
                }

                if (skipDeepSeekToolCalls)
                {
                    // Warning, not debug: dropping a tool call removes the model's only
                    // evidence that it already ran that tool, and it will call it again.
                    AutomatonDebug.LogWarning($"[PROVIDERHTTPCHECK] CollectValidToolCallIds: DROPPING DeepSeek tool call because assistant reasoning is missing - tool={part.Tool}, callId={part.CallId}. The model will not see this call or its result.");
                    skippedDeepSeekToolCallIds.Add(part.CallId!);
                    continue;
                }

                AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] CollectValidToolCallIds: Adding valid tool call - tool={part.Tool}, callId={part.CallId}");
                validToolCallIds.Add(part.CallId!);
            }
        }

        if (skippedDeepSeekToolCallIds.Count > 0)
        {
            AutomatonDebug.LogError($"[PROVIDERHTTPCHECK] CollectValidToolCallIds: {skippedDeepSeekToolCallIds.Count} tool call(s) were dropped from this request while {validToolCallIds.Count} were kept. A model that cannot see its own tool calls will repeat them - if this count climbs each round trip, that is the cause.");
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] CollectValidToolCallIds: Exit - validCount={validToolCallIds.Count}, skippedCount={skippedDeepSeekToolCallIds.Count}");
        return (validToolCallIds, skippedDeepSeekToolCallIds);
    }

    private protected static HashSet<string> CollectToolResultCallIds(List<Message> messages)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[PROVIDERHTTPCHECK] CollectToolResultCallIds: Entry");
        var toolResultCallIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        if (messages == null)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[PROVIDERHTTPCHECK] CollectToolResultCallIds: Exit - messages null");
            return toolResultCallIds;
        }

        foreach (var msg in messages)
        {
            if (!string.Equals(msg.Role, "tool", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            foreach (var part in msg.Parts)
            {
                if (part.Type != "tool" || string.IsNullOrWhiteSpace(part.CallId))
                {
                    continue;
                }

                toolResultCallIds.Add(part.CallId!);
            }
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] CollectToolResultCallIds: Exit - count={toolResultCallIds.Count}");
        return toolResultCallIds;
    }

    private void ConvertMessageToOpenAi(
        Message msg,
        int messageIndex,
        bool requiresDeepSeekReasoning,
        int lastUserMessageIndex,
        HashSet<string> validToolCallIds,
        HashSet<string> skippedDeepSeekToolCallIds,
        HashSet<string> toolResultCallIds,
        List<OpenAIMessage> result)
    {
        var role = msg.Role switch
        {
            "user" => "user",
            "assistant" => "assistant",
            "system" => "system",
            "tool" => "tool",
            _ => "user"
        };

        var reasoningContent = ExtractReasoningContent(msg);
        var includeDeepSeekReasoning = requiresDeepSeekReasoning && messageIndex > lastUserMessageIndex;
        var isHistoricalDeepSeekMessage = requiresDeepSeekReasoning &&
                                          lastUserMessageIndex >= 0 &&
                                          messageIndex < lastUserMessageIndex;

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertMessageToOpenAi: messageIndex={messageIndex}, role={role}, hasReasoning={!string.IsNullOrWhiteSpace(reasoningContent)}, isHistorical={isHistoricalDeepSeekMessage}");

        if (role == "assistant")
        {
            var toolParts = msg.Parts
                .Where(p => p.Type == "tool" && !string.IsNullOrWhiteSpace(p.Tool) && !string.IsNullOrWhiteSpace(p.CallId))
                .ToList();

            // Drop orphaned tool calls (no matching tool result), e.g. a call left
            // pending when the user interrupted. Sending them triggers MiniMax 2013.
            var orphanedToolParts = toolParts.Where(p => !toolResultCallIds.Contains(p.CallId!)).ToList();
            if (orphanedToolParts.Count > 0)
            {
                AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertMessageToOpenAi: Dropping {orphanedToolParts.Count} orphaned tool call(s) with no matching result: [{string.Join(", ", orphanedToolParts.Select(p => p.CallId))}]");
                toolParts = toolParts.Where(p => toolResultCallIds.Contains(p.CallId!)).ToList();
            }

            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertMessageToOpenAi: assistant message has {toolParts.Count} tool parts");
            if (toolParts.Count > 0)
            {
                ConvertAssistantMessageWithToolCalls(
                    msg, toolParts, reasoningContent,
                    isHistoricalDeepSeekMessage, requiresDeepSeekReasoning,
                    result);
                return;
            }
        }

        if (role == "tool")
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertMessageToOpenAi: tool message, validIds={validToolCallIds.Count}");
            ConvertToolMessage(msg, isHistoricalDeepSeekMessage,
                validToolCallIds, skippedDeepSeekToolCallIds, result);
            return;
        }

        var simpleContent = role == "assistant" ? StripInlineThinking(msg.Content) : msg.Content;
        var simpleReasoning = role == "assistant" && includeDeepSeekReasoning ? reasoningContent : null;

        // After dropping orphaned tool calls an assistant message can become empty.
        // Emitting an empty assistant message is invalid for some providers, so skip it.
        if (role == "assistant" && string.IsNullOrWhiteSpace(simpleContent) && string.IsNullOrWhiteSpace(simpleReasoning))
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[PROVIDERHTTPCHECK] ConvertMessageToOpenAi: Skipping empty assistant message (no content, no tool calls)");
            return;
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertMessageToOpenAi: Adding simple message - role={role}, contentLength={simpleContent?.Length ?? 0}");
        result.Add(new OpenAIMessage
        {
            Role = role,
            Content = BuildContent(msg, role, simpleContent),
            ReasoningContent = simpleReasoning
        });
    }

    /// <summary>
    /// Returns the plain string unless the message carries images, in which case it
    /// returns the multi-part array form the OpenAI schema requires for them.
    ///
    /// Only user messages are considered: an image on any other role is not something
    /// the chat schema accepts, and sending one is a 400 rather than a degraded answer.
    /// </summary>
    private static object? BuildContent(Message msg, string role, string? text)
    {
        if (role != "user")
        {
            return text;
        }

        var images = msg.Attachments
            .Where(attachment => attachment.IsImage && !string.IsNullOrEmpty(attachment.Base64Data))
            .ToList();
        if (images.Count == 0)
        {
            return text;
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] BuildContent: user message carries {images.Count} image attachment(s)");

        var parts = new List<object>();
        if (!string.IsNullOrEmpty(text))
        {
            parts.Add(new { type = "text", text });
        }

        foreach (var image in images)
        {
            parts.Add(new
            {
                type = "image_url",
                image_url = new { url = image.ToDataUri() }
            });
        }

        return parts;
    }

    private static void ConvertAssistantMessageWithToolCalls(
        Message msg,
        List<MessagePart> toolParts,
        string? reasoningContent,
        bool isHistoricalDeepSeekMessage,
        bool requiresDeepSeekReasoning,
        List<OpenAIMessage> result)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertAssistantMessageWithToolCalls: Entry - toolParts={toolParts.Count}, isHistorical={isHistoricalDeepSeekMessage}, requiresReasoning={requiresDeepSeekReasoning}");
        if (requiresDeepSeekReasoning && !isHistoricalDeepSeekMessage && string.IsNullOrWhiteSpace(reasoningContent))
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertAssistantMessageWithToolCalls: Skipping - requires reasoning but none provided");
            return;
        }

        var toolCalls = toolParts.Select(BuildToolCallFromPart).ToList();
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertAssistantMessageWithToolCalls: Adding assistant message with {toolCalls.Count} tool calls");

        result.Add(new OpenAIMessage
        {
            Role = "assistant",
            Content = StripInlineThinking(msg.Content),
            ReasoningContent = isHistoricalDeepSeekMessage ? null : reasoningContent,
            ToolCalls = toolCalls
        });
    }

    private static OpenAIToolCall BuildToolCallFromPart(MessagePart part)
    {
        return new OpenAIToolCall
        {
            Id = part.CallId!,
            Type = "function",
            Function = new OpenAIFunctionCall
            {
                Name = part.Tool,
                Arguments = JsonSerializer.SerializeToElement(
                    JsonSerializer.Serialize(part.State?.Input ?? new Dictionary<string, object>()))
            }
        };
    }

    private static void ConvertToolMessage(
        Message msg,
        bool isHistoricalDeepSeekMessage,
        HashSet<string> validToolCallIds,
        HashSet<string> skippedDeepSeekToolCallIds,
        List<OpenAIMessage> result)
    {
        var toolCallId = msg.Parts
            .FirstOrDefault(p => p.Type == "tool" && p.CallId != null)
            ?.CallId;

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertToolMessage: toolCallId=[{toolCallId ?? "null"}], valid={validToolCallIds.Contains(toolCallId ?? "")}, skipped={skippedDeepSeekToolCallIds.Contains(toolCallId ?? "")}");

        if (toolCallId == null ||
            skippedDeepSeekToolCallIds.Contains(toolCallId) ||
            !validToolCallIds.Contains(toolCallId))
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertToolMessage: Skipping tool message");
            return;
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] ConvertToolMessage: Adding tool message with callId={toolCallId}");
        result.Add(new OpenAIMessage
        {
            Role = "tool",
            Content = msg.Content,
            ToolCallId = toolCallId
        });
    }

    private static string? ExtractReasoningContent(Message message)
    {
        var reasoningParts = message.Parts
            .Where(part => part.Type == "reasoning" && !string.IsNullOrWhiteSpace(part.Text))
            .Select(part => part.Text!.Trim())
            .ToList();

        var (_, inlineReasoning) = SplitInlineThinking(message.Content);
        if (!string.IsNullOrWhiteSpace(inlineReasoning))
        {
            reasoningParts.Add(inlineReasoning);
        }

        if (reasoningParts.Count == 0)
        {
            return null;
        }

        return string.Join("\n", reasoningParts);
    }

    private protected static string StripInlineThinking(string? content)
    {
        return SplitInlineThinking(content).VisibleText;
    }

    private protected static (string VisibleText, string? Reasoning) SplitInlineThinking(string? content)
    {
        if (string.IsNullOrEmpty(content))
        {
            return (string.Empty, null);
        }

        var visible = content;
        var reasoning = new List<string>();
        var searchStart = 0;

        while (searchStart < visible.Length)
        {
            var openIndex = visible.IndexOf("<think>", searchStart, StringComparison.OrdinalIgnoreCase);
            if (openIndex < 0)
            {
                break;
            }

            var closeIndex = visible.IndexOf("</think>", openIndex + "<think>".Length, StringComparison.OrdinalIgnoreCase);
            if (closeIndex < 0)
            {
                break;
            }

            var reasoningStart = openIndex + "<think>".Length;
            var reasoningText = visible[reasoningStart..closeIndex].Trim();
            if (!string.IsNullOrWhiteSpace(reasoningText))
            {
                reasoning.Add(reasoningText);
            }

            var afterClose = closeIndex + "</think>".Length;
            visible = visible.Remove(openIndex, afterClose - openIndex);
            searchStart = openIndex;
        }

        return (visible.Trim(), reasoning.Count == 0 ? null : string.Join("\n", reasoning));
    }

    private protected object ConvertSchema(JsonSchema schema)
    {
        // Externally supplied schemas (e.g. MCP tools) are passed through verbatim so no
        // JSON-schema features are lost in the simplified typed model below.
        if (schema.RawSchema.HasValue && schema.RawSchema.Value.ValueKind == JsonValueKind.Object)
        {
            return schema.RawSchema.Value;
        }

        var result = new Dictionary<string, object>
        {
            ["type"] = schema.Type,
            ["properties"] = ConvertProperties(schema.Properties),
            ["required"] = schema.Required,
            ["additionalProperties"] = schema.AdditionalProperties
        };

        // Remove empty collections
        if (schema.Required.Count == 0)
            result.Remove("required");
        if (!schema.AdditionalProperties)
            result.Remove("additionalProperties");

        return result;
    }

    private Dictionary<string, object> ConvertProperties(Dictionary<string, JsonSchemaProperty> properties)
    {
        return properties.ToDictionary(
            kvp => kvp.Key,
            kvp => (object)ConvertProperty(kvp.Value));
    }

    private Dictionary<string, object> ConvertProperty(JsonSchemaProperty property)
    {
        var result = new Dictionary<string, object>
        {
            ["type"] = property.Type,
            ["description"] = property.Description
        };

        // Handle enum
        if (property.Enum != null && property.Enum.Any())
        {
            result["enum"] = property.Enum;
        }

        // Handle array items
        if (property.Items != null)
        {
            result["items"] = ConvertSchema(property.Items);
        }

        // Handle nested object properties
        if (property.Type == "object" && property.Items == null)
        {
            // This is a nested object, we need to include its properties
            // However, JsonSchemaProperty doesn't have Properties, only Items
            // So we can't support nested objects directly in current schema model
            // For now, leave as object type without properties
        }

        // Remove description if empty
        if (string.IsNullOrEmpty(property.Description))
            result.Remove("description");

        return result;
    }

    // OpenAI API response classes
    private class OpenAIResponse
    {
        [JsonPropertyName("id")]
        public string Id { get; set; } = string.Empty;

        [JsonPropertyName("choices")]
        public List<OpenAIChoice>? Choices { get; set; }

        [JsonPropertyName("usage")]
        public OpenAIUsage? Usage { get; set; }
    }

    private class OpenAIChoice
    {
        [JsonPropertyName("index")]
        public int Index { get; set; }

        [JsonPropertyName("message")]
        public OpenAIMessage? Message { get; set; }

        [JsonPropertyName("finish_reason")]
        public string? FinishReason { get; set; }
    }

    private class OpenAIMessage
    {
        [JsonPropertyName("role")]
        public string Role { get; set; } = string.Empty;

        /// <summary>
        /// Either a plain string or a list of content parts.
        ///
        /// The OpenAI chat schema allows both, and image attachments force the second
        /// form: an image can only be expressed as an <c>image_url</c> part. Responses
        /// always come back as a string, so read this through <see cref="ContentAsText"/>
        /// rather than casting.
        /// </summary>
        [JsonPropertyName("content")]
        public object? Content { get; set; }

        [JsonPropertyName("reasoning")]
        public string? Reasoning { get; set; }

        [JsonPropertyName("reasoning_content")]
        public string? ReasoningContent { get; set; }

        [JsonPropertyName("tool_calls")]
        public List<OpenAIToolCall>? ToolCalls { get; set; }

        [JsonPropertyName("tool_call_id")]
        public string? ToolCallId { get; set; }

        /// <summary>
        /// Flattens <see cref="Content"/> to text, whichever shape it is in.
        ///
        /// Deserialized responses arrive as a <see cref="JsonElement"/>, not a string,
        /// so a plain cast silently yields null and the model's reply vanishes.
        /// </summary>
        /// <remarks>
        /// <see cref="JsonIgnoreAttribute"/> is load-bearing, not tidiness. This type is
        /// serialized directly into the request body, and a public computed property is
        /// serialized along with everything else — so without this the payload carried a
        /// second copy of every message's content under a field the server does not
        /// know. On an image message that meant the whole base64 blob twice, since
        /// stringifying the content-part list expands the data URI in full.
        /// </remarks>
        [JsonIgnore]
        public string? ContentAsText => ContentToText(Content);

        private static string? ContentToText(object? content)
        {
            switch (content)
            {
                case null:
                    return null;
                case string text:
                    return text;
                case JsonElement { ValueKind: JsonValueKind.String } element:
                    return element.GetString();
                case JsonElement { ValueKind: JsonValueKind.Array } element:
                    // A multi-part content array: keep the text parts, drop images.
                    var parts = element.EnumerateArray()
                        .Where(part => part.TryGetProperty("type", out var type) && type.GetString() == "text")
                        .Select(part => part.TryGetProperty("text", out var text) ? text.GetString() : null)
                        .Where(text => !string.IsNullOrEmpty(text));
                    return string.Join("\n", parts);
                case IEnumerable<object> list:
                    return string.Join("\n", list.Select(item => item?.ToString()).Where(s => !string.IsNullOrEmpty(s)));
                default:
                    return content.ToString();
            }
        }
    }

    private class OpenAIToolCall
    {
        [JsonPropertyName("id")]
        public string Id { get; set; } = string.Empty;

        [JsonPropertyName("type")]
        public string Type { get; set; } = string.Empty;

        [JsonPropertyName("function")]
        public OpenAIFunctionCall? Function { get; set; }
    }

    private class OpenAIFunctionCall
    {
        [JsonPropertyName("name")]
        public string? Name { get; set; }

        [JsonPropertyName("arguments")]
        public JsonElement? Arguments { get; set; }
    }
}
