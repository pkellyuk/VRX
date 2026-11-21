using System.Text.Json;
using Automaton.Core.Interfaces;
using Automaton.Core.Streaming;

namespace Automaton.AI.Interfaces;

/// <summary>
/// Interface for AI providers that can generate responses and tool calls.
/// </summary>
public interface IAIProvider
{
    /// <summary>
    /// Provider identifier (e.g., "openai", "anthropic", "ollama").
    /// </summary>
    string Id { get; }

    /// <summary>
    /// Generates a response with optional tool calls for the given messages.
    /// </summary>
    /// <param name="messages">Conversation history.</param>
    /// <param name="tools">Available tools with their schemas.</param>
    /// <param name="model">Model identifier.</param>
    /// <param name="temperature">Temperature for generation.</param>
    /// <param name="maxTokens">Maximum tokens to generate.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    /// <returns>AI response containing text and optional tool calls.</returns>
    Task<AIResponse> GenerateResponseAsync(
        List<Message> messages,
        Dictionary<string, JsonSchema> tools,
        string model,
        double? temperature = null,
        int? maxTokens = null,
        CancellationToken cancellationToken = default,
        Action<string>? textDeltaCallback = null,
        RawStreamRecorder? rawStreamRecorder = null);
}
