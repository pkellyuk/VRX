using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace Automaton.AI.Services;

/// <summary>
/// The request fields a model's endpoint has refused, and changing a request to suit.
///
/// Thinking levels add fields that llama-server reads and hosted endpoints may not:
/// <c>chat_template_kwargs</c> and <c>reasoning_budget_tokens</c>. Some hosted endpoints ignore
/// what they do not know; Azure answers 400 "Unrecognized request argument supplied" and the turn
/// fails (30 Oct 2026). The address cannot say which kind an endpoint is - kodos.online is
/// llama-server behind a public name - so the endpoint's own answer decides. A field it refuses
/// is dropped, and remembered until Automaton restarts, because a chat builds a new provider each
/// turn and each of those would otherwise pay for the same refusal.
///
/// One Azure address serves every deployment on the resource, and they differ: DeepSeek takes
/// <c>max_tokens</c> where GPT 6.1 answers 400 and asks for <c>max_completion_tokens</c>
/// (30 Oct 2026). So a refusal is remembered for the endpoint and model together.
/// </summary>
public static class RejectedRequestArguments
{
    private static readonly ConcurrentDictionary<string, ConcurrentDictionary<string, byte>> RejectedByEndpoint =
        new(StringComparer.OrdinalIgnoreCase);

    private const string TokenLimit = "max_tokens";
    private const string CompletionTokenLimit = "max_completion_tokens";
    private const string Temperature = "temperature";

    /// <summary>
    /// The readable summary of a <c>/responses</c> model's thinking. OpenAI refuses it to an
    /// organisation it has not verified, and it is only ever for display, so it can go.
    /// </summary>
    public const string ReasoningSummary = "reasoning.summary";

    private static readonly string[] RefusalPhrases =
    {
        "unrecognized_request_argument",
        "unrecognized request argument",
        "unknown parameter",
        "unsupported parameter"
    };

    /// <summary>Whether this model's endpoint has already refused the field.</summary>
    public static bool IsRejected(string endpoint, string model, string argument) =>
        RejectedByEndpoint.TryGetValue(Key(endpoint, model), out var rejected) && rejected.ContainsKey(argument);

    /// <summary>Records a refusal, so later requests for the model change the field.</summary>
    public static void Remember(string endpoint, string model, string argument) =>
        RejectedByEndpoint.GetOrAdd(Key(endpoint, model), _ => new ConcurrentDictionary<string, byte>(StringComparer.Ordinal))[argument] = 0;

    /// <summary>The name this model's endpoint takes the output limit under.</summary>
    public static string TokenLimitField(string endpoint, string model) =>
        IsRejected(endpoint, model, TokenLimit) ? CompletionTokenLimit : TokenLimit;

    /// <summary>
    /// The optional fields a 400's body says the endpoint will not take. Only fields in
    /// <paramref name="optional"/> are ever named, so nothing the request needs can be dropped.
    /// </summary>
    public static IReadOnlyList<string> Named(string? errorContent, IEnumerable<string> optional)
    {
        if (!IsRefusal(errorContent))
        {
            return Array.Empty<string>();
        }

        return optional
            .Where(name => Mentions(errorContent!, name))
            .ToList();
    }

    /// <summary>
    /// Changes a request a 400 has refused, when the refusal is one the request can answer: an
    /// optional field goes, temperature goes (a reasoning model takes only its default), and the
    /// output limit moves to the name the endpoint asks for. Each change is remembered. False
    /// when nothing in the request can answer the refusal, which also ends a repeat of it.
    /// </summary>
    public static bool TryRevise(
        string endpoint,
        string model,
        string json,
        string? errorContent,
        IEnumerable<string> optional,
        out string revised,
        out IReadOnlyList<string> refused)
    {
        var body = JsonNode.Parse(json)?.AsObject()
            ?? throw new ArgumentException("The request body is not a JSON object.", nameof(json));

        var dropped = Named(errorContent, optional.Where(body.ContainsKey)).ToList();
        if (body.ContainsKey(Temperature) && RefusesTemperature(errorContent))
        {
            dropped.Add(Temperature);
        }

        if (body["reasoning"] is JsonObject reasoning && reasoning.ContainsKey("summary") && RefusesReasoningSummary(errorContent))
        {
            reasoning.Remove("summary");
            Remember(endpoint, model, ReasoningSummary);
            dropped.Add(ReasoningSummary);
        }

        var renamed = body.ContainsKey(TokenLimit) && AsksForCompletionTokenLimit(errorContent);
        foreach (var argument in dropped.Where(argument => argument != ReasoningSummary))
        {
            body.Remove(argument);
            Remember(endpoint, model, argument);
        }

        if (renamed)
        {
            var limit = body[TokenLimit]?.DeepClone();
            body.Remove(TokenLimit);
            body[CompletionTokenLimit] = limit;
            Remember(endpoint, model, TokenLimit);
            dropped.Add(TokenLimit);
        }

        revised = body.ToJsonString();
        refused = dropped;
        return dropped.Count > 0;
    }

    private static string Key(string endpoint, string model) => $"{endpoint}\n{model}";

    private static bool IsRefusal(string? errorContent) =>
        !string.IsNullOrEmpty(errorContent) &&
        RefusalPhrases.Any(phrase => errorContent.Contains(phrase, StringComparison.OrdinalIgnoreCase));

    private static bool RefusesTemperature(string? errorContent) =>
        !string.IsNullOrEmpty(errorContent) &&
        Mentions(errorContent, Temperature) &&
        (IsRefusal(errorContent) || errorContent.Contains("unsupported value", StringComparison.OrdinalIgnoreCase));

    private static bool RefusesReasoningSummary(string? errorContent) =>
        !string.IsNullOrEmpty(errorContent) &&
        Mentions(errorContent, ReasoningSummary) &&
        (IsRefusal(errorContent) ||
         errorContent.Contains("unsupported value", StringComparison.OrdinalIgnoreCase) ||
         errorContent.Contains("unsupported_value", StringComparison.OrdinalIgnoreCase) ||
         errorContent.Contains("verified", StringComparison.OrdinalIgnoreCase));

    private static bool AsksForCompletionTokenLimit(string? errorContent) =>
        IsRefusal(errorContent) &&
        Mentions(errorContent!, TokenLimit) &&
        Mentions(errorContent!, CompletionTokenLimit);

    private static bool Mentions(string errorContent, string name) =>
        Regex.IsMatch(errorContent, $@"\b{Regex.Escape(name)}\b", RegexOptions.CultureInvariant);
}
