using Automaton.AI.Interfaces;
using Automaton.AI.Providers;
using Automaton.Core.Models;
using Microsoft.Extensions.Logging;

namespace Automaton.AI.Services;

/// <summary>
/// Default implementation of provider factory.
/// </summary>
public class ProviderFactory : IProviderFactory
{
    private readonly ILoggerFactory _loggerFactory;
    private readonly HttpClient? _httpClient;

    public ProviderFactory(ILoggerFactory loggerFactory)
    {
        _loggerFactory = loggerFactory;
    }

    /// <summary>Opt-in transport supplied and owned by the calling host.</summary>
    public ProviderFactory(ILoggerFactory loggerFactory, HttpClient httpClient) : this(loggerFactory)
    {
        _httpClient = httpClient ?? throw new ArgumentNullException(nameof(httpClient));
    }

    public IAIProvider CreateProvider(string providerId, ProviderConfig config)
    {
        // Check API key requirements based on provider type
        var providerType = config.Type.ToLowerInvariant();

        if (providerType == "openai" && string.IsNullOrEmpty(config.ApiKey))
        {
            throw new ArgumentException($"API key is required for provider: {providerId}");
        }

        // OpenAI-compatible endpoints require a base URL.
        if (ProviderTypes.IsOpenAIHttp(providerType) && string.IsNullOrEmpty(config.BaseUrl))
        {
            throw new ArgumentException($"Base URL is required for provider: {providerId}");
        }

        return providerType switch
        {
            "mock" => CreateMockProvider(providerId, config),
            "llamacpp" => CreateLlamaCppProvider(providerId, config),
            "openai-compatible" => CreateLlamaCppProvider(providerId, config),
            "openai_compatible" => CreateLlamaCppProvider(providerId, config),
            ProviderTypes.OpenAIResponses => CreateResponsesProvider(config),
            "openai_responses" => CreateResponsesProvider(config),
            _ => throw new NotSupportedException($"Provider type not supported: {config.Type}")
        };
    }

    private IAIProvider CreateMockProvider(string providerId, ProviderConfig config)
    {
        var logger = _loggerFactory.CreateLogger<MockProvider>();
        return new MockProvider(logger);
    }

    private IAIProvider CreateLlamaCppProvider(string providerId, ProviderConfig config)
    {
        var logger = _loggerFactory.CreateLogger<LlamaCppHttpProvider>();
        return _httpClient == null
            ? new LlamaCppHttpProvider(config.BaseUrl!, config.ApiKey, config.RequestParameters, logger)
            : new LlamaCppHttpProvider(_httpClient, config.BaseUrl!, config.ApiKey, config.RequestParameters, logger);
    }

    private IAIProvider CreateResponsesProvider(ProviderConfig config)
    {
        var logger = _loggerFactory.CreateLogger<LlamaCppHttpProvider>();
        return _httpClient == null
            ? new OpenAIResponsesProvider(config.BaseUrl!, config.ApiKey, config.RequestParameters, logger)
            : new OpenAIResponsesProvider(_httpClient, config.BaseUrl!, config.ApiKey, config.RequestParameters, logger);
    }
}
