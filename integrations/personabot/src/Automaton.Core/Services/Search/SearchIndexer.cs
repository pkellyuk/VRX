using System.IO;
using System.Threading.Channels;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services.Search;

/// <summary>Tuning for the indexer; defaults are the deployment's, tests shorten them.</summary>
public sealed record SearchIndexerOptions
{
    /// <summary>How often source and index are compared row by row.</summary>
    public TimeSpan ReconcileInterval { get; init; } = TimeSpan.FromSeconds(60);

    /// <summary>
    /// How long a conversation must have been unchanged before it is indexed. A save during a
    /// streaming reply is followed by another within a second; indexing each would be wasted work.
    /// </summary>
    public TimeSpan QuietPeriod { get; init; } = TimeSpan.FromMilliseconds(1500);

    /// <summary>Whether to hook this process's own save events. Off in tests that drive the queue directly.</summary>
    public bool HookSystemEvents { get; init; } = true;

    /// <summary>Tuning for the vector worker, when one runs.</summary>
    public VectorWorkerOptions? VectorOptions { get; init; }
}

/// <summary>
/// The one process-wide writer of <see cref="SearchDatabase"/>.
/// </summary>
/// <remarks>
/// <para>
/// Runs in the master and nowhere else. It is told about work three ways: a turn finished in a
/// conversation the master runs (the workspace queues the state id), this process saved a
/// conversation of its own (the save event), and a periodic reconciliation that compares every
/// source row's fingerprint to the index and queues whatever differs - the catch-all for saves
/// made by other processes, for anything missed while the master was down, and for deletions.
/// </para>
/// <para>
/// Ownership is an exclusive lock on <c>search.db.lock</c> beside the database, held for the life
/// of the process and released by the operating system on a crash. It fails closed: if the lock
/// cannot be taken for any reason, this process does not index and says so. A second master
/// still answers searches from the file.
/// </para>
/// <para>
/// The FTS worker is its own thread with its own queue: a few milliseconds of CPU and a short
/// write per conversation, never waiting on anything outside the file. See SEARCH.md.
/// </para>
/// </remarks>
public sealed class SearchIndexer : IDisposable
{
    private static readonly object CurrentLock = new();

    private readonly ConversationSearchService _search;
    private readonly SearchDatabase _db;
    private readonly ConfigDatabase _source;
    private readonly SearchIndexerOptions _options;
    private readonly Channel<IndexWork> _ftsQueue = Channel.CreateUnbounded<IndexWork>(new UnboundedChannelOptions { SingleReader = true });
    private readonly HashSet<string> _pending = new(StringComparer.Ordinal);
    private readonly object _sync = new();
    private readonly CancellationTokenSource _lifetime = new();

    private FileStream? _lock;
    private VectorWorker? _vectors;
    private Task? _ftsWorker;
    private Timer? _reconcileTimer;
    private int _reconciling;
    private int _reconcileAgain;
    private bool _disposed;
    private volatile bool _stopping;
    private Task? _stopTask;
    private Task _vectorReconcile = Task.CompletedTask;

    private readonly record struct IndexWork(string Source, string Id);

    /// <summary>The indexer running in this process, or null when this process does not index.</summary>
    public static SearchIndexer? Current { get; private set; }

    /// <summary>(source, id) after each row is indexed or removed.</summary>
    public event Action<string, string>? Indexed;

    /// <summary>The vector worker, when an embedding client was given; null otherwise.</summary>
    public VectorWorker? Vectors => _vectors;
    public SearchIndexer(ConversationSearchService search, SearchDatabase db, ConfigDatabase source, SearchIndexerOptions? options = null)
    {
        _search = search ?? throw new ArgumentNullException(nameof(search));
        _db = db ?? throw new ArgumentNullException(nameof(db));
        _source = source ?? throw new ArgumentNullException(nameof(source));
        _options = options ?? new SearchIndexerOptions();
    }

    public bool IsRunning => _lock != null && !_disposed && !_stopping;
    public string DatabasePath => _db.DatabasePath;

    /// <summary>How many rows are queued and not yet indexed.</summary>
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

    /// <summary>
    /// Makes this process the indexer, using the shared database instances. Returns null - and
    /// logs why - when the lock could not be taken or the database is not writable.
    /// </summary>
    public static SearchIndexer? StartInThisProcess(SearchIndexerOptions? options = null, IEmbeddingClient? embeddings = null)
    {
        AutomatonDebug.Log($"SearchIndexer.StartInThisProcess: Entry - embeddings={embeddings != null}");
        lock (CurrentLock)
        {
            if (Current != null)
            {
                AutomatonDebug.Log("SearchIndexer.StartInThisProcess: Exit - already running here");
                return Current;
            }

            SearchIndexer? indexer = null;
            try
            {
                indexer = new SearchIndexer(ConversationSearchService.Instance, SearchDatabase.Instance, ConfigDatabase.Instance, options);
                if (!indexer.TryStart(embeddings))
                {
                    indexer.Dispose();
                    AutomatonDebug.LogWarning("SearchIndexer.StartInThisProcess: Exit - this process will not index");
                    return null;
                }
            }
            catch (Exception ex)
            {
                indexer?.Dispose();
                AutomatonDebug.LogError($"SearchIndexer.StartInThisProcess: Exit - failed: {ex.Message}");
                return null;
            }

            Current = indexer;
            AutomatonDebug.Log("SearchIndexer.StartInThisProcess: Exit - indexing in this process");
            return indexer;
        }
    }

    /// <summary>
    /// Takes the lock and starts the workers and the reconciliation timer. False, with a
    /// reason logged, when it cannot. The vector worker starts only when an embedding client is
    /// given; without one the FTS worker runs alone and search stays lexical.
    /// </summary>
    public bool TryStart(IEmbeddingClient? embeddings = null)
    {
        AutomatonDebug.Log($"SearchIndexer.TryStart: Entry - lock={_db.LockPath}");
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(SearchIndexer));
        }

        if (IsRunning)
        {
            AutomatonDebug.Log("SearchIndexer.TryStart: Exit - already running");
            return true;
        }

        if (!_db.Writable || !_db.IsAvailable)
        {
            AutomatonDebug.LogWarning("SearchIndexer.TryStart: Exit - the search database is not writable in this process");
            return false;
        }

        try
        {
            _lock = new FileStream(_db.LockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1, FileOptions.DeleteOnClose);
        }
        catch (IOException ex)
        {
            AutomatonDebug.LogWarning($"SearchIndexer.TryStart: Exit - another process holds the indexer lock ({ex.Message})");
            return false;
        }
        catch (Exception ex)
        {
            // Fail closed. Not knowing whether we own the lock is the same as not owning it.
            AutomatonDebug.LogError($"SearchIndexer.TryStart: Exit - could not take the indexer lock: {ex.Message}");
            return false;
        }

        _ftsWorker = Task.Run(() => FtsWorkerAsync(_lifetime.Token));
        StartVectors(embeddings);

        if (_options.HookSystemEvents)
        {
            SystemEvents.OnConversationSavedDetailed += OnConversationSaved;
            SystemEvents.OnDreamConversationSaved += OnDreamSaved;
        }

        // First reconciliation straight away: it is the incremental replacement for the old
        // startup rebuild, and it catches everything saved while no indexer was running.
        _reconcileTimer = new Timer(_ => ReconcileQuietly(), null, TimeSpan.Zero, _options.ReconcileInterval);

        AutomatonDebug.Log("SearchIndexer.TryStart: Exit - lock taken, worker and timer started");
        return true;
    }
    /// <summary>
    /// Starts the vector worker, or explains in the log why semantic search is off. A failure
    /// here never stops the FTS worker: keyword search working is the floor this feature sits
    /// on, and it must not depend on a model being configured or a server being up.
    /// </summary>
    private void StartVectors(IEmbeddingClient? embeddings)
    {
        if (embeddings == null)
        {
            AutomatonDebug.Log("SearchIndexer.StartVectors: no embedding client configured; semantic search is off and search is lexical");
            return;
        }

        try
        {
            var index = new VectorIndex(_db);
            _vectors = new VectorWorker(_db, _search, index, embeddings, _options.VectorOptions);
            _vectors.Start();
            AutomatonDebug.Log($"SearchIndexer.StartVectors: vector worker running on generation {_vectors.Generation}");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"SearchIndexer.StartVectors: could not start the vector worker: {ex.Message}");
            _vectors?.Dispose();
            _vectors = null;
        }
    }

    private void OnConversationSaved(ConversationSavedEventInfo info)
    {
        if (info == null || string.IsNullOrWhiteSpace(info.StateId))
        {
            return;
        }

        QueueConversation(info.StateId);
    }

    private void OnDreamSaved(Models.ConversationState state)
    {
        if (state == null || string.IsNullOrWhiteSpace(state.SessionId))
        {
            return;
        }

        QueueDream(state.SessionId);
    }

    /// <summary>Queues a conversation by its persistence state id (the key of <c>ConversationState</c>).</summary>
    public void QueueConversation(string stateId) => Queue(ConversationSearchService.ChatSource, stateId);

    /// <summary>Queues a dream mirror by its id (the source SessionId).</summary>
    public void QueueDream(string id) => Queue(ConversationSearchService.DreamSource, id);

    private void Queue(string source, string id)
    {
        if (_stopping || _disposed) return;
        if (string.IsNullOrWhiteSpace(id))
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

        if (!_ftsQueue.Writer.TryWrite(new IndexWork(source, id)))
        {
            lock (_sync)
            {
                _pending.Remove(key);
            }

            AutomatonDebug.LogWarning($"SearchIndexer.Queue: could not queue {key}");
            return;
        }

        AutomatonDebug.Log($"SearchIndexer.Queue: queued {key}");
    }

    /// <summary>
    /// Marks every row stale and reconciles, so the whole index is rebuilt row by row while
    /// search keeps answering. The only rebuild there is.
    /// </summary>
    public void RequestRebuild()
    {
        AutomatonDebug.Log("SearchIndexer.RequestRebuild: Entry");
        try
        {
            _db.Execute("UPDATE IndexProgress SET FtsRevision = NULL, SourceUpdatedAt = NULL, SourceRevision = NULL, SourceLength = NULL");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"SearchIndexer.RequestRebuild: could not mark rows stale: {ex.Message}");
        }

        _ = Task.Run(() => ReconcileAsync(_lifetime.Token));
        AutomatonDebug.Log("SearchIndexer.RequestRebuild: Exit");
    }

    private void ReconcileQuietly()
    {
        _ = ReconcileAsync(_lifetime.Token);
    }

    /// <summary>
    /// Compares source to index per row and queues whatever differs; removes index rows whose
    /// source is gone. Cheap columns (UpdatedAt, Revision, byte length) decide whether the
    /// stored JSON is re-hashed, so a poll does not read every transcript every minute; the
    /// fingerprint decides whether it is re-indexed. Public so tests can drive it.
    /// </summary>
    public async Task ReconcileAsync(CancellationToken token)
    {
        if (_stopping || _disposed || token.IsCancellationRequested) return;
        if (Interlocked.Exchange(ref _reconciling, 1) == 1)
        {
            // One is running. It will go round again when it finishes, so a request that
            // arrived mid-pass (a rebuild marking rows stale) is not lost.
            Interlocked.Exchange(ref _reconcileAgain, 1);
            AutomatonDebug.Log("SearchIndexer.ReconcileAsync: Exit - already reconciling; another pass requested");
            return;
        }

        try
        {
            if (_stopping || token.IsCancellationRequested) return;
            do
            {
                Interlocked.Exchange(ref _reconcileAgain, 0);
                AutomatonDebug.Log("SearchIndexer.ReconcileAsync: Entry");
                var queued = 0;
                var removed = 0;
                queued += ReconcileSource(ConversationSearchService.ChatSource, "ConversationState", ref removed);
                queued += ReconcileSource(ConversationSearchService.DreamSource, "DreamConversationState", ref removed);

                // Off this thread: a first pass queues every conversation in the database, and
                // the reconcile timer should not be held while that happens.
                var vectors = _vectors;
                if (vectors != null)
                {
                    lock (_sync)
                    {
                        if (!_stopping && _vectorReconcile.IsCompleted)
                            _vectorReconcile = Task.Run(() => vectors.Reconcile());
                    }
                }

                AutomatonDebug.Log($"SearchIndexer.ReconcileAsync: Exit - queued {queued}, removed {removed}");
            }
            while (!token.IsCancellationRequested && Interlocked.Exchange(ref _reconcileAgain, 0) == 1);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested)
        {
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"SearchIndexer.ReconcileAsync: failed: {ex.Message}");
        }
        finally
        {
            Interlocked.Exchange(ref _reconciling, 0);
        }

        await Task.CompletedTask.ConfigureAwait(false);
    }

    private int ReconcileSource(string source, string table, ref int removed)
    {
        var sourceRows = _source
            .Query($"SELECT Id, Revision, UpdatedAt, length(State) AS Len FROM {table}")
            .ToDictionary(r => (string)r["Id"]!, r => r, StringComparer.Ordinal);

        var progress = _db
            .Query("SELECT Id, FtsRevision, SourceUpdatedAt, SourceRevision, SourceLength FROM IndexProgress WHERE Source = @Source",
                new SqliteParameter("@Source", source))
            .ToDictionary(r => (string)r["Id"]!, r => r, StringComparer.Ordinal);

        var queued = 0;
        foreach (var (id, row) in sourceRows)
        {
            if (!progress.TryGetValue(id, out var indexed) || indexed["FtsRevision"] == null)
            {
                Queue(source, id);
                queued++;
                continue;
            }

            var sameUpdated = string.Equals(indexed["SourceUpdatedAt"] as string, row["UpdatedAt"] as string, StringComparison.Ordinal);
            var sameRevision = Convert.ToInt64(indexed["SourceRevision"] ?? -1L) == Convert.ToInt64(row["Revision"] ?? -2L);
            var sameLength = Convert.ToInt64(indexed["SourceLength"] ?? -1L) == Convert.ToInt64(row["Len"] ?? -2L);
            if (sameUpdated && sameRevision && sameLength)
            {
                continue;
            }

            Queue(source, id);
            queued++;
        }

        foreach (var id in progress.Keys.Where(id => !sourceRows.ContainsKey(id)))
        {
            try
            {
                _search.RemoveStateFromIndex(source, id);
                removed++;
                Indexed?.Invoke(source, id);
            }
            catch (Exception ex)
            {
                AutomatonDebug.LogWarning($"SearchIndexer.ReconcileSource: could not remove {source}:{id}: {ex.Message}");
            }
        }

        return queued;
    }

    private async Task FtsWorkerAsync(CancellationToken token)
    {
        AutomatonDebug.Log("SearchIndexer.FtsWorkerAsync: Entry");
        try
        {
            await foreach (var work in _ftsQueue.Reader.ReadAllAsync(token).ConfigureAwait(false))
            {
                var key = work.Source + ":" + work.Id;
                try
                {
                    await WaitUntilQuietAsync(work, token).ConfigureAwait(false);
                    lock (_sync)
                    {
                        _pending.Remove(key);
                    }

                    var changed = _search.IndexStoredConversation(work.Source, work.Id);
                    AutomatonDebug.Log($"SearchIndexer.FtsWorkerAsync: {key} {(changed ? "indexed" : "unchanged")}");
                    Indexed?.Invoke(work.Source, work.Id);

                    // Unconditionally, not only when the FTS row changed: a conversation can be
                    // current in FTS and absent from a newly created vector generation. The
                    // vector worker re-checks before it embeds, so a redundant queue costs a
                    // lookup.
                    _vectors?.Queue(work.Source, work.Id);
                }
                catch (OperationCanceledException) when (token.IsCancellationRequested)
                {
                    break;
                }
                catch (Exception ex)
                {
                    lock (_sync)
                    {
                        _pending.Remove(key);
                    }

                    AutomatonDebug.LogError($"SearchIndexer.FtsWorkerAsync: {key} failed: {ex.Message}");
                }
            }
        }
        catch (OperationCanceledException)
        {
        }

        AutomatonDebug.Log("SearchIndexer.FtsWorkerAsync: Exit");
    }

    /// <summary>
    /// Waits for the source row to stop changing before it is read. Streaming saves arrive
    /// several times a second; indexing after the last one is the same work as indexing after
    /// each and produces the same index.
    /// </summary>
    private async Task WaitUntilQuietAsync(IndexWork work, CancellationToken token)
    {
        if (_options.QuietPeriod <= TimeSpan.Zero)
        {
            return;
        }

        var table = work.Source == ConversationSearchService.DreamSource ? "DreamConversationState" : "ConversationState";
        for (var attempt = 0; attempt < 4; attempt++)
        {
            var updated = _source.ExecuteScalar<string>($"SELECT UpdatedAt FROM {table} WHERE Id = @Id", new SqliteParameter("@Id", work.Id));
            if (!DateTime.TryParse(updated, null, System.Globalization.DateTimeStyles.RoundtripKind, out var at))
            {
                return;
            }

            var age = DateTime.UtcNow - at.ToUniversalTime();
            if (age >= _options.QuietPeriod)
            {
                return;
            }

            await Task.Delay(_options.QuietPeriod, token).ConfigureAwait(false);
        }
    }

    /// <summary>Stops owned workers before releasing the writer lease. Cancelling the wait does not release it early.</summary>
    public Task StopAsync(CancellationToken token = default)
    {
        lock (_sync)
        {
            _stopTask ??= StopCoreAsync();
            return _stopTask.WaitAsync(token);
        }
    }

    private async Task StopCoreAsync()
    {
        if (_disposed) return;
        _stopping = true;
        _lifetime.Cancel();
        _ftsQueue.Writer.TryComplete();
        var timer = Interlocked.Exchange(ref _reconcileTimer, null);
        if (timer != null) await timer.DisposeAsync().ConfigureAwait(false);
        while (Volatile.Read(ref _reconciling) != 0) await Task.Delay(10).ConfigureAwait(false);
        Task vectors;
        lock (_sync) vectors = _vectorReconcile;
        try
        {
            await Task.WhenAll(_ftsWorker ?? Task.CompletedTask, vectors,
                _vectors?.StopAsync() ?? Task.CompletedTask).ConfigureAwait(false);
        }
        finally { Dispose(); }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        AutomatonDebug.Log("SearchIndexer.Dispose: Entry");

        if (_options.HookSystemEvents && _lock != null)
        {
            SystemEvents.OnConversationSavedDetailed -= OnConversationSaved;
            SystemEvents.OnDreamConversationSaved -= OnDreamSaved;
        }

        _vectors?.Dispose();
        _reconcileTimer?.Dispose();
        _lifetime.Cancel();
        _ftsQueue.Writer.TryComplete();
        _lock?.Dispose();
        _lock = null;

        lock (CurrentLock)
        {
            if (ReferenceEquals(Current, this))
            {
                Current = null;
            }
        }

        AutomatonDebug.Log("SearchIndexer.Dispose: Exit");
    }
}
