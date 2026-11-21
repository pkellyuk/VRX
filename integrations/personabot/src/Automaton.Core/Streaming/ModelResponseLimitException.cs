namespace Automaton.Core.Streaming;

/// <summary>An explicit host resource limit; retrying the same response is not useful.</summary>
public sealed class ModelResponseLimitException(string message) : InvalidOperationException(message);

/// <summary>Local transport metadata. This option is never sent to the provider.</summary>
public static class ModelResponsePolicy
{
    public static readonly HttpRequestOptionsKey<bool> StreamingResponse = new("automaton.streaming-response");
}
