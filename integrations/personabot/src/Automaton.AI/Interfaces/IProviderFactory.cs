using Automaton.Core.Models;

namespace Automaton.AI.Interfaces;

/// <summary>
/// Factory for creating AI providers from configuration.
/// </summary>
public interface IProviderFactory
{
    /// <summary>
    /// Creates an AI provider based on configuration.
    /// </summary>
    /// <param name="providerId">Provider identifier.</param>
    /// <param name="config">Provider configuration.</param>
    /// <returns>Configured AI provider.</returns>
    IAIProvider CreateProvider(string providerId, ProviderConfig config);
}