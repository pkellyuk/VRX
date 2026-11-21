using System.Diagnostics;
using System.Net.Http;
using System.Net.Http.Json;
using System.Text.Json.Serialization;

namespace Automaton.Core.Services.Search;

/// <summary>Turns text into vectors.</summary>
public interface IEmbeddingClient
{
    /// <summary>The contract the vectors satisfy; its fingerprint names the generation they belong in.</summary>
    EmbeddingContract Contract { get; }

    /// <summary>Whether the server is up. Cheap, and cached briefly; asked before each batch.</summary>
    Task<bool> IsAvailableAsync(CancellationToken cancellationToken);

    /// <summary>Embeds each text, in order. L2-normalised. Throws when the server cannot answer.</summary>
    Task<IReadOnlyList<float[]>> EmbedAsync(IReadOnlyList<string> texts, CancellationToken cancellationToken);
}

/// <summary>
/// The server returned vectors of a width the contract cannot use.
/// </summary>
/// <remarks>
/// Its own type because it is the one embedding failure that must not be retried. Every other
/// failure is transient - the server is starting, the GPU is busy, the process died - and the
/// worker's answer is to wait and try again. A wrong width is a configuration mistake that will
/// still be wrong in thirty seconds, and retrying it forever pins the GPU on a typo.
/// </remarks>
public sealed class EmbeddingDimensionException : Exception
{
    public EmbeddingDimensionException(int returned, int configured)
        : base($"The embedding server returned {returned} dimensions; {configured} were configured.")
    {
        Returned = returned;
        Configured = configured;
    }

    public int Returned { get; }

    public int Configured { get; }
}

/// <summary>
/// The llama.cpp embedding server on loopback, started from Model Studio.
/// </summary>
/// <remarks>
/// <para>
/// Speaks the OpenAI-shaped <c>/v1/embeddings</c> route llama-server exposes in embedding mode.
/// Every call has a bounded timeout: the indexer's vector worker calls this outside any
/// transaction, so a stall here costs a retry, never a held write lock. Nothing leaves the
/// machine; the address is fixed to loopback.
/// </para>
/// <para>
/// The health result is cached for a few seconds. Asking before every batch is right - a server
/// that died should not cost a 60-second timeout per batch - but a fresh HTTP round trip per
/// batch measurably dominates a backfill of small batches.
/// </para>
/// <para>
/// The caller's cancellation token sets the real deadline. A query embeds with a short one so a
/// server busy with a backfill batch degrades to lexical search rather than stalling a tool call;
/// the worker embeds with the full timeout because it has nothing better to do than wait.
/// </para>
/// </remarks>
public sealed class LlamaEmbeddingClient : IEmbeddingClient, IDisposable
{
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(60);
    private static readonly TimeSpan HealthTimeout = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan HealthCacheFor = TimeSpan.FromSeconds(15);

    private readonly HttpClient _http;
    private readonly Uri _base;
    private readonly object _healthLock = new();

    private DateTime _healthCheckedAt = DateTime.MinValue;
    private bool _healthy;

    public LlamaEmbeddingClient(int port, EmbeddingContract contract, HttpMessageHandler? handler = null)
    {
        if (port <= 0 || port > 65535)
        {
            throw new ArgumentOutOfRangeException(nameof(port));
        }

        Contract = contract ?? throw new ArgumentNullException(nameof(contract));
        _base = new Uri($"http://127.0.0.1:{port}/");
        _http = handler == null ? new HttpClient() : new HttpClient(handler);
        _http.Timeout = RequestTimeout;
        AutomatonDebug.Log($"LlamaEmbeddingClient: constructed for {_base} model={contract.ModelName}");
    }

    public EmbeddingContract Contract { get; }

    public async Task<bool> IsAvailableAsync(CancellationToken cancellationToken)
    {
        lock (_healthLock)
        {
            if (_healthy && DateTime.UtcNow - _healthCheckedAt < HealthCacheFor)
            {
                return true;
            }
        }

        var healthy = false;
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(HealthTimeout);
            using var response = await _http.GetAsync(new Uri(_base, "health"), timeout.Token).ConfigureAwait(false);
            healthy = response.IsSuccessStatusCode;
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or OperationCanceledException)
        {
            AutomatonDebug.Log($"LlamaEmbeddingClient.IsAvailableAsync: not available - {ex.GetType().Name}");
        }

        lock (_healthLock)
        {
            _healthy = healthy;
            _healthCheckedAt = DateTime.UtcNow;
        }

        return healthy;
    }

    public async Task<IReadOnlyList<float[]>> EmbedAsync(IReadOnlyList<string> texts, CancellationToken cancellationToken)
    {
        if (texts == null)
        {
            throw new ArgumentNullException(nameof(texts));
        }

        if (texts.Count == 0)
        {
            return Array.Empty<float[]>();
        }

        AutomatonDebug.Log($"LlamaEmbeddingClient.EmbedAsync: Entry - {texts.Count} text(s)");
        var stopwatch = Stopwatch.StartNew();

        using var response = await _http.PostAsJsonAsync(
            new Uri(_base, "v1/embeddings"),
            new EmbeddingRequest { Model = Contract.ModelName, Input = texts },
            cancellationToken).ConfigureAwait(false);

        if (!response.IsSuccessStatusCode)
        {
            var detail = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            throw new InvalidOperationException($"The embedding server answered {(int)response.StatusCode}: {Shorten(detail)}");
        }

        var payload = await response.Content.ReadFromJsonAsync<EmbeddingResponse>(cancellationToken: cancellationToken).ConfigureAwait(false)
            ?? throw new InvalidOperationException("The embedding server returned no body.");

        if (payload.Data == null || payload.Data.Count != texts.Count)
        {
            throw new InvalidOperationException($"The embedding server returned {payload.Data?.Count ?? 0} vectors for {texts.Count} texts.");
        }

        var vectors = payload.Data
            .OrderBy(d => d.Index)
            .Select(d => Normalise(Fit(d.Embedding ?? Array.Empty<float>())))
            .ToList();

        // The one number that settles how long a backfill takes; see SEARCH.md.
        AutomatonDebug.Log($"LlamaEmbeddingClient.EmbedAsync: Exit - embedded {vectors.Count} text(s) of {vectors[0].Length} dims in {stopwatch.ElapsedMilliseconds}ms");
        return vectors;
    }

    /// <summary>
    /// Truncates to the contract's dimension when the model returns more (a Matryoshka model
    /// serves its full width), and refuses when it returns fewer.
    /// </summary>
    private float[] Fit(float[] vector)
    {
        if (vector.Length == Contract.Dimensions)
        {
            return vector;
        }

        if (vector.Length < Contract.Dimensions)
        {
            throw new EmbeddingDimensionException(vector.Length, Contract.Dimensions);
        }

        var cut = new float[Contract.Dimensions];
        Array.Copy(vector, cut, Contract.Dimensions);
        return cut;
    }

    /// <summary>L2-normalises in place so cosine distance is a plain dot product.</summary>
    public static float[] Normalise(float[] vector)
    {
        if (vector == null)
        {
            throw new ArgumentNullException(nameof(vector));
        }

        double sum = 0;
        foreach (var v in vector)
        {
            sum += (double)v * v;
        }

        var norm = Math.Sqrt(sum);
        if (norm <= 0)
        {
            return vector;
        }

        for (var i = 0; i < vector.Length; i++)
        {
            vector[i] = (float)(vector[i] / norm);
        }

        return vector;
    }

    private static string Shorten(string text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return string.Empty;
        }

        return text.Length <= 200 ? text : text[..200] + "…";
    }

    public void Dispose()
    {
        _http.Dispose();
    }

    private sealed class EmbeddingRequest
    {
        [JsonPropertyName("model")]
        public string Model { get; set; } = string.Empty;

        [JsonPropertyName("input")]
        public IReadOnlyList<string> Input { get; set; } = Array.Empty<string>();
    }

    private sealed class EmbeddingResponse
    {
        [JsonPropertyName("data")]
        public List<EmbeddingDatum>? Data { get; set; }
    }

    private sealed class EmbeddingDatum
    {
        [JsonPropertyName("index")]
        public int Index { get; set; }

        [JsonPropertyName("embedding")]
        public float[]? Embedding { get; set; }
    }
}
