using System.Text.Json;
using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

public class ProviderConfig
{
    [JsonPropertyName("type")]
    public string Type { get; set; } = "openai";

    [JsonPropertyName("apiKey")]
    public string? ApiKey { get; set; }

    [JsonPropertyName("baseUrl")]
    public string? BaseUrl { get; set; }

    [JsonPropertyName("defaultModel")]
    public string? DefaultModel { get; set; }

    [JsonPropertyName("maxContextTokens")]
    public int? MaxContextTokens { get; set; }

    /// <summary>
    /// Cap on the tokens the model may generate in one reply, per provider.
    ///
    /// Separate from <see cref="MaxContextTokens"/>, which is how much it can read. It
    /// belongs to the model rather than the app: the ceilings differ wildly between them
    /// and a single global value is either too small for the roomy ones or a hard error on
    /// the strict ones. Null falls back to the global <c>Llm.MaxTokens</c>.
    /// </summary>
    [JsonPropertyName("maxOutputTokens")]
    public int? MaxOutputTokens { get; set; }

    /// <summary>
    /// The thinking levels this model understands, in the order the chat's Effort button
    /// steps through them. Empty uses the default ladder.
    ///
    /// Per model because the vocabulary is not standard: DeepSeek's hosted API defines
    /// none/low/high/max, while llama.cpp accepts whatever the model's chat template
    /// branches on. Sending a level the far side does not define is not an error — it is
    /// silently ignored, which is worse, because the button appears to work.
    /// </summary>
    [JsonPropertyName("reasoningEffortLevels")]
    public List<string>? ReasoningEffortLevels { get; set; }

    [JsonPropertyName("enabled")]
    public bool Enabled { get; set; } = true;

    /// <summary>
    /// Whether a finished reply is followed by one small extra request that predicts the
    /// user's next message for the chat box (SUGGESTREPLY.md). Off by default and per provider:
    /// it is one more request per reply, cheap on a local server and a real cost on a paid API.
    /// </summary>
    [JsonPropertyName("suggestReplies")]
    public bool SuggestReplies { get; set; }

    /// <summary>
    /// The provider is a hosted (cloud) model. Set by hand in Settings, because nothing about a
    /// provider says where it runs - the local llama-server is reached through kodos.online. A chat
    /// on a cloud provider may start background workers (the background_worker tool); one on a
    /// local model never does, since its single server would be shared by every worker.
    /// </summary>
    [JsonPropertyName("isCloud")]
    public bool IsCloud { get; set; }

    [JsonPropertyName("requestParameters")]
    public Dictionary<string, JsonElement> RequestParameters { get; set; } = new();
}
