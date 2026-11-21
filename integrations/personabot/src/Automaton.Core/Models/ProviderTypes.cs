namespace Automaton.Core.Models;

/// <summary>
/// The provider types Automaton talks to over HTTP, named by the API format they speak.
/// <c>openai-compatible</c> sends <c>/chat/completions</c>, which llama-server, DeepSeek and most
/// hosted endpoints take. <c>openai-responses</c> sends <c>/responses</c>, which newer OpenAI models
/// need for thinking and tools together: GPT 6.1 Terra on Azure refuses that pair on
/// <c>/chat/completions</c> (6 Nov 2026).
/// </summary>
public static class ProviderTypes
{
    public const string OpenAICompatible = "openai-compatible";
    public const string OpenAIResponses = "openai-responses";

    /// <summary>Speaks <c>/chat/completions</c>.</summary>
    public static bool IsChatCompletions(string? type) =>
        type?.Trim().ToLowerInvariant() is "llamacpp" or OpenAICompatible or "openai_compatible";

    /// <summary>Speaks <c>/responses</c>.</summary>
    public static bool IsResponses(string? type) =>
        type?.Trim().ToLowerInvariant() is OpenAIResponses or "openai_responses";

    /// <summary>
    /// An OpenAI-style HTTP endpoint in either format: it needs a base URL, takes images, and
    /// lists its models at <c>/models</c>.
    /// </summary>
    public static bool IsOpenAIHttp(string? type) => IsChatCompletions(type) || IsResponses(type);
}
