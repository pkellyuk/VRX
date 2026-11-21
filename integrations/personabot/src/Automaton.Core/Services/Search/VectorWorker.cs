using System.Threading.Channels;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services.Search;

/// <summary>Tuning for the vector worker; defaults are the deployment's, tests shorten them.</summary>
public sealed record VectorWorkerOptions
{
    /// <summary>How many conversations are taken from the queue between health checks.</summary>
    public int BatchSize { get; init; } = 32;

    /// <summary>
    /// How many chunks are sent to the embedding server in one request. Small on purpose: each
    /// request has its own 60-second timeout, and on a CPU-only server (about 45 tokens a second)
    /// that covers two chunks of 2,000 characters of code with room to spare and four with none.
    /// Nothing is lost by it, because <c>--parallel 1</c> embeds inputs one at a time anyway.
    /// </summary>
    public int ChunksPerRequest { get; init; } = 2;

    /// <summary>How long to wait after the server refuses or fails before trying again.</summary>
    public TimeSpan RetryDelay { get; init; } = TimeSpan.FromSeconds(30);

    /// <summary>The longest a conversation that keeps failing waits between attempts.</summary>
    public TimeSpan MaxRetryDelay { get; init; } = TimeSpan.FromHours(1);
}

/// <summary>
/// The second worker in the indexer: turns indexed conversations into vectors.
/// </summary>
/// <remarks>
/// <para>
/// Its own thread and its own queue, because it waits on something the FTS worker never does.
/// An embedding call takes hundreds of milliseconds when the server is warm and forever when it
/// is not running; sharing a thread would let that delay the transcript index every search
/// depends on. The two write disjoint tables in the same file, both in transactions measured in
/// milliseconds, so SQLite's own serialisation is all the coordination they need.
/// </para>
/// <para>
/// <b>The embedding call never happens inside a transaction.</b> That is enforced by shape rather
/// than by discipline: <see cref="SearchDatabase.ExecuteInTransaction"/> takes a synchronous
/// delegate, so there is nothing to await inside one. Work is snapshotted, embedded outside, and
/// committed only if the source has not moved underneath it.
/// </para>
/// <para>
/// What needs embedding is decided entirely inside search.db. <c>IndexProgress.FtsRevision</c> is
/// already a fingerprint of exactly the content the chunks are cut from, so comparing it to
/// <c>VectorProgress.VecRevision</c> answers the question without reading config.db at all - and
/// it means vectors are always built from content the FTS worker has already finished with,
/// never from a conversation that is still streaming.
/// </para>
/// </remarks>
public sealed class VectorWorker : IDisposable
{
    private readonly SearchDatabase _db;
    private readonly ConversationSearchService _search;
    private readonly VectorIndex _index;
    private readonly IEmbeddingClient _embeddings;
    private readonly VectorWorkerOptions _options;
    private readonly Channel<WorkItem> _queue = Channel.CreateUnbounded<WorkItem>(new UnboundedChannelOptions { SingleReader = true });
    private readonly HashSet<string> _pending = new(StringComparer.Ordinal);
    private readonly object _sync = new();
    private readonly CancellationTokenSource _lifetime = new();

    // Touched only by the worker loop, so it needs no lock.
    private readonly Dictionary<WorkItem, int> _failures = new();

    private Task? _worker;
    private bool _disposed;
    private bool _stopped;

    private readonly record struct WorkItem(string Source, string Id);

    public VectorWorker(
        SearchDatabase db,
        ConversationSearchService search,
        VectorIndex index,
        IEmbeddingClient embeddings,
        VectorWorkerOptions? options = null)
    {
        _db = db ?? throw new ArgumentNullException(nameof(db));
        _search = search ?? throw new ArgumentNullException(nameof(search));
        _index = index ?? throw new ArgumentNullException(nameof(index));
        _embeddings = embeddings ?? throw new ArgumentNullException(nameof(embeddings));
        _options = options ?? new VectorWorkerOptions();
    }

    /// <summary>The generation being written. Zero before <see cref="Start"/>.</summary>
    public int Generation { get; private set; }

    /// <summary>False once a fatal error has stopped this worker for the life of the process.</summary>
    public bool IsRunning => _worker != null && !_disposed && !_stopped;

    /// <summary>How many conversations are queued and not yet embedded.</summary>
    public int Pending
    {
        get
        {
            lock (_sync)
            {
                return _pending.Count;
            }
        }
    }

    /// <summary>Raised after each conversation is embedded, removed or skipped.</summary>
    public event Action<string, string>? Embedded;

    /// <summary>
    /// Picks the generation for the configured contract - creating it when the fingerprint is
    /// new - and starts the worker thread.
    /// </summary>
    public void Start()
    {
        AutomatonDebug.Log("VectorWorker.Start: Entry");
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(VectorWorker));
        }

        if (_worker != null)
        {
            AutomatonDebug.Log("VectorWorker.Start: Exit - already running");
            return;
        }

        var fingerprint = _embeddings.Contract.Fingerprint;
        Generation = _index.GenerationFor(fingerprint) ?? _index.CreateGeneration(_embeddings.Contract);
        _worker = Task.Run(() => RunAsync(_lifetime.Token));
        AutomatonDebug.Log($"VectorWorker.Start: Exit - generation {Generation}, fingerprint {fingerprint[..12]}");
    }

    /// <summary>Queues one conversation. Duplicates are dropped.</summary>
    public void Queue(string source, string id)
    {
        if (string.IsNullOrWhiteSpace(source) || string.IsNullOrWhiteSpace(id))
        {
            return;
        }

        if (!IsRunning)
        {
            return;
        }

        var key = source + ":" + id;
        lock (_sync)
        {
            if (!_pending.Add(key))
            {
                return;
            }
        }

        if (_queue.Writer.TryWrite(new WorkItem(source, id)))
        {
            return;
        }

        lock (_sync)
        {
            _pending.Remove(key);
        }

        AutomatonDebug.LogWarning($"VectorWorker.Queue: could not queue {key}");
    }

    /// <summary>
    /// Queues every conversation whose vectors are missing or built from older content, and
    /// removes vectors whose FTS row is gone. Reads only search.db.
    /// </summary>
    public void Reconcile()
    {
        if (!IsRunning)
        {
            return;
        }

        AutomatonDebug.Log($"VectorWorker.Reconcile: Entry - generation {Generation}");
        var queued = 0;
        var removed = 0;

        try
        {
            var stale = _db.Query(@"
                SELECT p.Source AS Source, p.Id AS Id
                FROM IndexProgress p
                LEFT JOIN VectorProgress v
                       ON v.Source = p.Source AND v.Id = p.Id AND v.Generation = @G
                WHERE p.FtsRevision IS NOT NULL
                  AND (v.VecRevision IS NULL OR v.VecRevision <> p.FtsRevision)",
                new SqliteParameter("@G", Generation));

            foreach (var row in stale)
            {
                Queue(row["Source"] as string ?? string.Empty, row["Id"] as string ?? string.Empty);
                queued++;
            }

            // An orphan is a conversation whose progress row is gone - the source row was
            // deleted and the FTS worker removed it. A row that is merely stale (FtsRevision
            // NULL, because a rebuild marked it) is a conversation about to be re-indexed, and
            // its vectors stay: once FTS catches up with the same fingerprint they are current
            // again with no re-embedding. Treating stale as gone here deleted every vector in
            // the index the first time an FTS rebuild ran while the embedding server was down.
            var orphans = _db.Query(@"
                SELECT v.Source AS Source, v.Id AS Id
                FROM VectorProgress v
                WHERE v.Generation = @G
                  AND NOT EXISTS (
                      SELECT 1 FROM IndexProgress p
                      WHERE p.Source = v.Source AND p.Id = v.Id)",
                new SqliteParameter("@G", Generation));

            foreach (var row in orphans)
            {
                var source = row["Source"] as string ?? string.Empty;
                var id = row["Id"] as string ?? string.Empty;
                _index.RemoveState(Generation, source, id);
                removed++;
                Embedded?.Invoke(source, id);
            }
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"VectorWorker.Reconcile: failed: {ex.Message}");
        }

        AutomatonDebug.Log($"VectorWorker.Reconcile: Exit - queued {queued}, removed {removed}");
        ActivateIfComplete();
    }

    private async Task RunAsync(CancellationToken token)
    {
        AutomatonDebug.Log("VectorWorker.RunAsync: Entry");
        try
        {
            while (await _queue.Reader.WaitToReadAsync(token).ConfigureAwait(false))
            {
                var batch = new List<WorkItem>(_options.BatchSize);
                while (batch.Count < _options.BatchSize && _queue.Reader.TryRead(out var item))
                {
                    batch.Add(item);
                }

                if (batch.Count == 0)
                {
                    continue;
                }

                if (!await _embeddings.IsAvailableAsync(token).ConfigureAwait(false))
                {
                    AutomatonDebug.Log($"VectorWorker.RunAsync: the embedding server is not answering; {batch.Count} conversation(s) wait");
                    Requeue(batch);
                    await Task.Delay(_options.RetryDelay, token).ConfigureAwait(false);
                    continue;
                }

                foreach (var item in batch)
                {
                    if (token.IsCancellationRequested)
                    {
                        break;
                    }

                    try
                    {
                        await EmbedOneAsync(item, token).ConfigureAwait(false);
                        _failures.Remove(item);
                        Done(item);
                    }
                    catch (OperationCanceledException) when (token.IsCancellationRequested)
                    {
                        break;
                    }
                    catch (EmbeddingDimensionException ex)
                    {
                        // Not transient, and never will be. Retrying this every thirty seconds
                        // would hold the GPU for a configuration mistake.
                        AutomatonDebug.LogError($"VectorWorker.RunAsync: {ex.Message} Set embedding_dimension to match the model, then restart. Semantic search is off until then.");
                        _stopped = true;
                        return;
                    }
                    catch (Exception ex)
                    {
                        var retryIn = NextRetryDelay(item);
                        AutomatonDebug.LogWarning($"VectorWorker.RunAsync: {item.Source}:{item.Id} failed, retrying in {retryIn}: {ex.Message}");
                        RequeueAfter(item, retryIn, token);
                        await Task.Delay(_options.RetryDelay, token).ConfigureAwait(false);
                    }
                }

                ActivateIfComplete();
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"VectorWorker.RunAsync: stopped: {ex.Message}");
        }

        AutomatonDebug.Log("VectorWorker.RunAsync: Exit");
    }

    private async Task EmbedOneAsync(WorkItem item, CancellationToken token)
    {
        var loaded = _search.LoadForIndexing(item.Source, item.Id);
        if (loaded == null)
        {
            _index.RemoveState(Generation, item.Source, item.Id);
            Embedded?.Invoke(item.Source, item.Id);
            return;
        }

        // The FTS worker queues every conversation it touches, changed or not, and an FTS
        // rebuild touches all of them. The doc said this check existed; it did not, and a
        // tokenizer change was about to re-embed 8,000 chunks whose text had not moved.
        if (string.Equals(_index.RecordedRevision(Generation, item.Source, item.Id), loaded.Fingerprint, StringComparison.Ordinal))
        {
            AutomatonDebug.Log($"VectorWorker.EmbedOneAsync: {item.Source}:{item.Id} is already embedded from this content; skipping");
            Embedded?.Invoke(item.Source, item.Id);
            return;
        }

        var chunks = TranscriptChunker.Chunk(item.Source, item.Id, loaded.State, loaded.Origins);
        var fingerprint = _embeddings.Contract.Fingerprint;

        if (chunks.Count == 0)
        {
            _index.RecordEmpty(Generation, fingerprint, item.Source, item.Id, loaded.Fingerprint);
            Embedded?.Invoke(item.Source, item.Id);
            return;
        }

        var texts = chunks.Select(c => EmbeddingContract.DocumentText(c.Role, c.Text)).ToList();
        var vectors = await EmbedInRequestsAsync(texts, token).ConfigureAwait(false);

        // The source can have moved while we were embedding. Committing vectors built from the
        // previous content would leave the index quietly describing a conversation that no
        // longer says that; the reconciliation will offer this row again.
        var fresh = _search.LoadForIndexing(item.Source, item.Id);
        if (fresh == null)
        {
            _index.RemoveState(Generation, item.Source, item.Id);
            Embedded?.Invoke(item.Source, item.Id);
            return;
        }

        if (!string.Equals(fresh.Fingerprint, loaded.Fingerprint, StringComparison.Ordinal))
        {
            AutomatonDebug.Log($"VectorWorker.EmbedOneAsync: {item.Source}:{item.Id} changed while embedding; discarding");
            return;
        }

        _index.ReplaceChunks(Generation, fingerprint, item.Source, item.Id, chunks, vectors, loaded.Fingerprint);
        Embedded?.Invoke(item.Source, item.Id);
    }

    /// <summary>
    /// Embeds a conversation a few chunks at a time. Sending it whole gave the request timeout a
    /// size limit: on a CPU server every conversation past about 2,700 tokens timed out, was
    /// retried at the same size, and burned the CPU indefinitely without storing a vector.
    /// </summary>
    private async Task<IReadOnlyList<float[]>> EmbedInRequestsAsync(IReadOnlyList<string> texts, CancellationToken token)
    {
        var vectors = new List<float[]>(texts.Count);
        foreach (var request in texts.Chunk(_options.ChunksPerRequest))
        {
            vectors.AddRange(await _embeddings.EmbedAsync(request, token).ConfigureAwait(false));
        }

        return vectors;
    }

    /// <summary>
    /// Publishes the generation once nothing is outstanding.
    /// </summary>
    /// <remarks>
    /// <para>
    /// An empty generation is never published, and that guard is doing more work than it looks.
    /// "Nothing is outstanding" is true both when a backfill has finished and when it has not
    /// started - the queue is equally empty a moment after the worker begins, before the FTS
    /// worker has written a single progress row. Activating there would publish a generation
    /// holding nothing, and every semantic search would return nothing while the status line
    /// read healthy.
    /// </para>
    /// <para>
    /// Refusing outright rather than comparing against how much is indexed is the honest rule: an
    /// empty index and an index that has not been built yet are indistinguishable from here, and
    /// treating both as not-built costs nothing. Semantic search is off either way, and the next
    /// reconciliation activates the generation as soon as it holds something.
    /// </para>
    /// </remarks>
    private void ActivateIfComplete()
    {
        if (!IsRunning || Generation <= 0)
        {
            return;
        }

        try
        {
            if (Pending > 0)
            {
                return;
            }

            var active = _index.ActiveGeneration();
            if (active == Generation)
            {
                return;
            }

            var chunks = _index.ChunkCount(Generation);
            if (chunks == 0)
            {
                AutomatonDebug.Log($"VectorWorker.ActivateIfComplete: generation {Generation} holds no chunks yet; not activating");
                return;
            }

            _index.Activate(Generation);
            AutomatonDebug.Log($"VectorWorker.ActivateIfComplete: generation {Generation} is now active with {chunks} chunk(s)");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"VectorWorker.ActivateIfComplete: {ex.Message}");
        }
    }

    private void Requeue(IReadOnlyList<WorkItem> batch)
    {
        // The pending entries stay set, so nothing is queued twice and a drained queue is not
        // mistaken for completed work while these are waiting.
        foreach (var item in batch)
        {
            _queue.Writer.TryWrite(item);
        }
    }

    /// <summary>Records a failure and doubles the wait for each one in a row, up to the cap.</summary>
    private TimeSpan NextRetryDelay(WorkItem item)
    {
        var failures = _failures.GetValueOrDefault(item) + 1;
        _failures[item] = failures;

        var ticks = _options.RetryDelay.Ticks * (1L << Math.Min(failures - 1, 16));
        return TimeSpan.FromTicks(Math.Min(ticks, _options.MaxRetryDelay.Ticks));
    }

    private void RequeueAfter(WorkItem item, TimeSpan delay, CancellationToken token)
    {
        // The pending entry stays set for the whole wait. Clearing it would let the reconcile
        // timer queue the item straight back, and would let a drained queue activate a generation
        // that is still missing this conversation.
        _ = Task.Delay(delay, token).ContinueWith(
            _ => _queue.Writer.TryWrite(item),
            CancellationToken.None,
            TaskContinuationOptions.OnlyOnRanToCompletion,
            TaskScheduler.Default);
    }

    private void Done(WorkItem item)
    {
        lock (_sync)
        {
            _pending.Remove(item.Source + ":" + item.Id);
        }
    }

    /// <summary>Cooperatively stops embedding work before disposing its cancellation source.</summary>
    public async Task StopAsync()
    {
        if (_disposed) return;
        _lifetime.Cancel();
        _queue.Writer.TryComplete();
        try { if (_worker != null) await _worker.ConfigureAwait(false); }
        finally { Dispose(); }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        AutomatonDebug.Log("VectorWorker.Dispose: Entry");
        _lifetime.Cancel();
        _queue.Writer.TryComplete();
        _lifetime.Dispose();
        AutomatonDebug.Log("VectorWorker.Dispose: Exit");
    }
}
