using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Threading.Tasks;

namespace Automaton.AI.Services;

// Single source of truth for "is this provider failure worth retrying?".
// Used by the HTTP send loop (LlamaCppHttpProvider) and the agent loop
// (ConversationTurnProcessor) so both layers agree on what counts as transient.
public static class TransientErrorPolicy
{
    public static readonly TimeSpan BaseDelay = TimeSpan.FromSeconds(2);
    public static readonly TimeSpan MaxDelay = TimeSpan.FromSeconds(30);

    // 5xx server errors and a few transient 4xx are worth retrying; a restarting
    // provider commonly returns 502/503/504. 501/505 are permanent capability errors.
    public static bool IsTransientHttpStatus(HttpStatusCode statusCode)
    {
        var code = (int)statusCode;

        if (code == 408 || code == 425 || code == 429 || code == 529)
        {
            return true;
        }

        if (code >= 500 && code != 501 && code != 505)
        {
            return true;
        }

        return false;
    }

    // True for connection drops, resets, timeouts, and 5xx-style failures that
    // surface as exceptions. An HttpRequestException that carries a definite
    // non-transient HTTP status (e.g. 401/403/400) is NOT transient — retrying it
    // would just hang, so it must surface for the caller to act on.
    public static bool IsTransientException(Exception ex)
    {
        if (ex == null)
        {
            return false;
        }

        for (var current = ex; current != null; current = current.InnerException)
        {
            if (current is HttpRequestException httpEx)
            {
                if (httpEx.StatusCode.HasValue && !IsTransientHttpStatus(httpEx.StatusCode.Value))
                {
                    return false;
                }

                return true;
            }

            if (current is IOException ||
                current is System.Net.Sockets.SocketException ||
                current is TimeoutException ||
                current is TaskCanceledException)
            {
                return true;
            }
        }

        return false;
    }

    /// <summary>The longest wait between a turn's retries of a dropped provider connection.</summary>
    public static readonly TimeSpan TurnRetryMaxDelay = TimeSpan.FromSeconds(5);

    /// <summary>
    /// The wait before a turn retries a dropped provider connection: 2s, 4s, then every 5s. A turn
    /// retries until the user presses Stop, so the wait stays short enough that a provider coming
    /// back is noticed within seconds.
    /// </summary>
    public static TimeSpan ComputeTurnRetryDelay(int attempt)
    {
        var backoff = ComputeBackoffDelay(attempt);
        return backoff < TurnRetryMaxDelay ? backoff : TurnRetryMaxDelay;
    }

    // Exponential backoff (2s, 4s, 8s, ...) capped at MaxDelay so a long outage
    // keeps retrying patiently without hammering the provider.
    public static TimeSpan ComputeBackoffDelay(int attempt)
    {
        if (attempt < 1)
        {
            attempt = 1;
        }

        var exponent = Math.Min(attempt - 1, 10);
        var seconds = Math.Min(BaseDelay.TotalSeconds * Math.Pow(2, exponent), MaxDelay.TotalSeconds);
        return TimeSpan.FromSeconds(seconds);
    }
}
