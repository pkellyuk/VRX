using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Automaton.Core.Interfaces;
using Automaton.Core.Models;
using Automaton.Core.Services.Search;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services;

/// <summary>
/// Full-text search over conversation history, using SQLite FTS5 in the derived
/// <see cref="SearchDatabase"/> (<c>search.db</c>).
/// </summary>
/// <remarks>
/// <para>
/// Reads run in any process. Writes run in the one process that owns the
/// <see cref="SearchIndexer"/>; everywhere else the write methods hand the work to the indexer
/// when one is present in this process and otherwise log and do nothing, because several
/// processes writing whole transcripts into one file is the contention this split removes.
/// See SEARCH.md.
/// </para>
/// <para>
/// Index rows are keyed two ways. <c>StateId</c> is the source row's key
/// (<c>ConversationState.Id</c>, or the dream mirror's id), which is what indexing and
/// reconciliation work in. <c>ConversationId</c> is the SessionId, which is what every search
/// result has always carried and what the rest of the application opens conversations by.
/// </para>
/// </remarks>
public sealed class ConversationSearchService : IDisposable
{
    /// <summary>Source name for ordinary chats in <c>IndexProgress</c>.</summary>
    public const string ChatSource = "chat";

    /// <summary>Source name for dream mirrors in <c>IndexProgress</c>.</summary>
    public const string DreamSource = "dream";

    private static readonly object InstanceLock = new();
    private static ConversationSearchService? _instance;
    private static readonly JsonSerializerOptions ReadOptions = new() { PropertyNameCaseInsensitive = true };
    private static readonly JsonSerializerOptions WriteOptions = new() { WriteIndented = false, PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    private readonly SearchDatabase _db;
    private readonly ConfigDatabase _source;
    private readonly object _writeLock = new();
    private bool _disposed;

    /// <summary>
    /// The shared instance over the shared databases. Rebuilt if the shared search database was
    /// replaced underneath it - which happens when something searched before the master declared
    /// itself writable, and an instance built on the read-only handle would refuse every write.
    /// </summary>
    public static ConversationSearchService Instance
    {
        get
        {
            lock (InstanceLock)
            {
                var db = SearchDatabase.Instance;
                if (_instance == null || !ReferenceEquals(_instance._db, db))
                {
                    if (_instance != null)
                    {
                        AutomatonDebug.Log("ConversationSearchService.Instance: search database changed; rebuilding the shared service");
                    }

                    _instance = new ConversationSearchService(db, ConfigDatabase.Instance);
                }

                return _instance;
            }
        }
    }

    public ConversationSearchService(SearchDatabase database, ConfigDatabase source)
    {
        _db = database ?? throw new ArgumentNullException(nameof(database));
        _source = source ?? throw new ArgumentNullException(nameof(source));
        AutomatonDebug.Log($"ConversationSearchService: constructed - available={_db.IsAvailable}, writable={_db.Writable}");
    }

    private static readonly object SemanticLock = new();
    private static SemanticBinding _semantic = new(null, null, null);
    private INearestNeighbours? _defaultScan;

    /// <summary>
    /// Points this process's searches at an embedding server and a vector index.
    /// </summary>
    /// <remarks>
    /// Called in every process that searches, not only the one that indexes: a conversation host
    /// runs the <c>remember</c> tool, and semantic recall is worth most exactly there. Hosts read
    /// the vectors and never write them, which the read-only search database already enforces.
    /// Passing a null client turns semantic search off again. The index defaults to a managed
    /// scan over whichever search database this service already holds, which is deliberately not
    /// built here: a host opens search.db on its first search, and constructing one at startup
    /// would move that decision earlier for no gain.
    /// </remarks>
    public static void ConfigureSemanticSearch(IEmbeddingClient? client, INearestNeighbours? neighbours = null)
    {
        lock (SemanticLock) Volatile.Write(ref _semantic, new(client, neighbours, null));
        AutomatonDebug.Log($"ConversationSearchService.ConfigureSemanticSearch: embeddings={client != null}, index={neighbours != null}");
    }

    /// <summary>
    /// Temporarily installs a host's semantic configuration. Disposing the lease
    /// restores the previous live binding unless another caller replaced it.
    /// Clients remain owned by their callers; this lease never disposes them.
    /// </summary>
    public static IDisposable UseSemanticSearch(IEmbeddingClient? client, INearestNeighbours? neighbours = null)
    {
        lock (SemanticLock)
        {
            var binding = new SemanticBinding(client, neighbours, _semantic);
            Volatile.Write(ref _semantic, binding); return binding;
        }
    }

    private sealed class SemanticBinding(IEmbeddingClient? client, INearestNeighbours? neighbours, SemanticBinding? previous) : IDisposable
    {
        internal readonly IEmbeddingClient? Client = client;
        internal readonly INearestNeighbours? Neighbours = neighbours;
        private readonly SemanticBinding? _previous = previous;
        private bool _removed;
        public void Dispose()
        {
            lock (SemanticLock)
            {
                _removed = true;
                var current = _semantic;
                while (current._removed && current._previous != null) current = current._previous;
                Volatile.Write(ref _semantic, current);
            }
        }
    }

    /// <summary>Whether a search here can also match by meaning.</summary>
    public bool SemanticAvailable => Volatile.Read(ref _semantic).Client != null && _db.IsAvailable;

    /// <summary>Raised when a conversation is indexed: (conversation id, indexed-at).</summary>
    public event Action<string, string>? OnConversationIndexed;

    /// <summary>False when this process cannot see the search database (the master has not built it yet).</summary>
    public bool IsAvailable => _db.IsAvailable;

    /// <summary>
    /// Whether a rebuild asked for here would actually happen: true in the process holding the
    /// indexer lock, false everywhere else. A caller that reports what it did needs to know.
    /// </summary>
    public bool CanIndex => SearchIndexer.Current != null || (_db.Writable && _db.IsAvailable);

    /// <summary>
    /// Makes sure the shared instance exists. Indexing is no longer hooked here: the
    /// <see cref="SearchIndexer"/> in the master hooks save events; this process may only read.
    /// Kept so existing callers need not change.
    /// </summary>
    public static void Initialize()
    {
        try
        {
            _ = Instance;
            AutomatonDebug.Log("ConversationSearchService.Initialize: instance ready");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationSearchService.Initialize: failed - {ex.Message}");
        }
    }

    // ── Indexing ──────────────────────────────────────────────────────────

    /// <summary>
    /// Indexes one source row from what is stored, if the stored content differs from what the
    /// index was built from. Returns true when the index changed. The fingerprint is a hash of
    /// the stored State JSON, so it catches a change however the source's revision number was
    /// assigned. Removes the index rows when the source row is gone.
    /// </summary>
    public bool IndexStoredConversation(string source, string id)
    {
        if (string.IsNullOrWhiteSpace(source))
        {
            throw new ArgumentException("source is required", nameof(source));
        }

        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("id is required", nameof(id));
        }

        AutomatonDebug.Log($"ConversationSearchService.IndexStoredConversation: Entry - {source}:{id}");
        if (!EnsureWritable(nameof(IndexStoredConversation)))
        {
            return false;
        }

        var loaded = LoadForIndexing(source, id);
        if (loaded == null)
        {
            RemoveStateFromIndex(source, id);
            AutomatonDebug.Log($"ConversationSearchService.IndexStoredConversation: Exit - {source}:{id} gone from source; removed");
            return true;
        }

        var progress = _db.Query(
            "SELECT FtsRevision FROM IndexProgress WHERE Source = @Source AND Id = @Id",
            new SqliteParameter("@Source", source), new SqliteParameter("@Id", id)).FirstOrDefault();

        if (progress != null && string.Equals(progress["FtsRevision"] as string, loaded.Fingerprint, StringComparison.Ordinal))
        {
            // Same content; only the cheap columns moved. Record them so the next poll skips this row.
            _db.Execute(
                "UPDATE IndexProgress SET SourceUpdatedAt = @Updated, SourceRevision = @Revision, SourceLength = @Len WHERE Source = @Source AND Id = @Id",
                new SqliteParameter("@Updated", (object?)loaded.UpdatedAt ?? DBNull.Value),
                new SqliteParameter("@Revision", loaded.Revision),
                new SqliteParameter("@Len", (long)loaded.JsonLength),
                new SqliteParameter("@Source", source),
                new SqliteParameter("@Id", id));
            AutomatonDebug.Log($"ConversationSearchService.IndexStoredConversation: Exit - {source}:{id} unchanged");
            return false;
        }

        var state = loaded.State;
        WriteIndex(source, id, state, loaded.Origins, loaded.Fingerprint, loaded.UpdatedAt, loaded.Revision, loaded.JsonLength);
        AutomatonDebug.Log($"ConversationSearchService.IndexStoredConversation: Exit - {source}:{id} indexed {state.Messages?.Count ?? 0} message(s), {loaded.Origins.Count(o => o == SearchOrigins.Compressed)} compressed");
        return true;
    }

    /// <summary>
    /// Indexes a conversation from an in-memory state. The fingerprint recorded is of this
    /// object's serialisation, which may differ from the stored JSON, so the next reconciliation
    /// re-checks the row; correctness does not depend on the two matching.
    /// </summary>
    public void IndexConversation(ConversationState state, string? stateId = null)
    {
        if (state == null)
        {
            return;
        }

        var id = stateId ?? state.SessionId;
        if (string.IsNullOrWhiteSpace(id))
        {
            AutomatonDebug.LogWarning("ConversationSearchService.IndexConversation: state has no id; skipped");
            return;
        }

        if (!_db.Writable && SearchIndexer.Current == null)
        {
            AutomatonDebug.Log($"ConversationSearchService.IndexConversation: {id} - this process does not index");
            return;
        }

        if (!_db.Writable)
        {
            SearchIndexer.Current!.QueueConversation(id);
            return;
        }

        // The dialog renders the stored full transcript, and a hit's position and id must be
        // that transcript's, not the in-memory context's - a compaction summary has a
        // different id and position in each. When the row is stored, index from the store.
        if (HasStoredState(id))
        {
            AutomatonDebug.Log($"ConversationSearchService.IndexConversation: {id} is stored; indexing from the stored transcript");
            IndexStoredConversation(ChatSource, id);
            return;
        }

        var json = JsonSerializer.Serialize(state, WriteOptions);
        var (messages, origins) = SplitForIndexing(state.Messages);
        var split = CloneWithMessages(state, messages);
        WriteIndex(ChatSource, id, split, origins, Fingerprint(json), null, null, null);
    }

    private bool HasStoredState(string stateId)
    {
        try
        {
            return _source.Query("SELECT 1 FROM ConversationState WHERE Id = @Id LIMIT 1", new SqliteParameter("@Id", stateId)).Count > 0;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ConversationSearchService.HasStoredState: {stateId}: {ex.Message}");
            return false;
        }
    }

    /// <summary>Indexes a dream mirror from an in-memory state. Its id is the source SessionId.</summary>
    public void IndexDreamConversation(ConversationState state)
    {
        if (state == null || string.IsNullOrWhiteSpace(state.SessionId))
        {
            return;
        }

        if (!_db.Writable)
        {
            SearchIndexer.Current?.QueueDream(state.SessionId);
            return;
        }

        var json = JsonSerializer.Serialize(state, WriteOptions);
        var (messages, origins) = SplitForIndexing(state.Messages);
        var split = CloneWithMessages(state, messages);
        WriteIndex(DreamSource, state.SessionId, split, origins, Fingerprint(json), null, null, null);
    }

    /// <summary>Indexes a conversation by its persistence state id.</summary>
    public void IndexConversationById(string conversationId, string? projectId = null)
    {
        if (string.IsNullOrWhiteSpace(conversationId))
        {
            return;
        }

        if (!_db.Writable)
        {
            SearchIndexer.Current?.QueueConversation(conversationId);
            return;
        }

        try
        {
            IndexStoredConversation(ChatSource, conversationId);
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationSearchService.IndexConversationById: {conversationId} failed: {ex.Message}");
        }
    }

    private void WriteIndex(string source, string stateId, ConversationState state, IReadOnlyList<string> origins, string fingerprint, string? updatedAt, long? revision, long? length)
    {
        var table = source == DreamSource ? "DreamConversationSearch" : "ConversationSearch";
        var now = DateTime.UtcNow.ToString("O");
        var conversationId = string.IsNullOrWhiteSpace(state.SessionId) ? stateId : state.SessionId;
        var messages = state.Messages ?? new List<Message>();
        origins ??= Array.Empty<string>();

        lock (_writeLock)
        {
            _db.ExecuteInTransaction(tx =>
            {
                tx.Execute($"DELETE FROM {table} WHERE StateId = @StateId", new SqliteParameter("@StateId", stateId));

                // The loop index is the message's position in the transcript the dialog opens,
                // so it must advance over skipped messages too - the same rule the chunker keeps.
                for (var messageIndex = 0; messageIndex < messages.Count; messageIndex++)
                {
                    var message = messages[messageIndex];
                    if (message == null)
                    {
                        continue;
                    }

                    var origin = messageIndex < origins.Count ? SearchOrigins.Normalize(origins[messageIndex]) : SearchOrigins.Raw;
                    if (message.Role == "system" && origin == SearchOrigins.Raw && (message.Content?.Length ?? 0) > 50000)
                    {
                        continue;
                    }

                    var content = message.Content ?? string.Empty;
                    if (IsPlaceholderContent(content))
                    {
                        continue;
                    }

                    if (content.Length > 100000)
                    {
                        content = content[..100000] + "... [truncated]";
                    }

                    tx.Execute($@"
                        INSERT INTO {table} (StateId, ConversationId, SessionId, ThreadId, ProjectId, Role, Content, Timestamp, MessageId, MessageIndex, Origin)
                        VALUES (@StateId, @ConversationId, @SessionId, @ThreadId, @ProjectId, @Role, @Content, @Timestamp, @MessageId, @MessageIndex, @Origin)",
                        new SqliteParameter("@StateId", stateId),
                        new SqliteParameter("@ConversationId", conversationId),
                        new SqliteParameter("@SessionId", conversationId),
                        new SqliteParameter("@ThreadId", conversationId),
                        new SqliteParameter("@ProjectId", (object?)state.ProjectId ?? DBNull.Value),
                        new SqliteParameter("@Role", message.Role ?? string.Empty),
                        new SqliteParameter("@Content", content),
                        new SqliteParameter("@Timestamp", now),
                        new SqliteParameter("@MessageId", string.IsNullOrWhiteSpace(message.Id) ? DBNull.Value : message.Id),
                        new SqliteParameter("@MessageIndex", messageIndex),
                        new SqliteParameter("@Origin", origin));
                }

                tx.Execute(@"
                    INSERT INTO IndexProgress (Source, Id, FtsRevision, SourceUpdatedAt, SourceRevision, SourceLength, IndexedAt)
                    VALUES (@Source, @Id, @Fts, @Updated, @Revision, @Len, @Now)
                    ON CONFLICT(Source, Id) DO UPDATE SET
                        FtsRevision = excluded.FtsRevision,
                        SourceUpdatedAt = excluded.SourceUpdatedAt,
                        SourceRevision = excluded.SourceRevision,
                        SourceLength = excluded.SourceLength,
                        IndexedAt = excluded.IndexedAt",
                    new SqliteParameter("@Source", source),
                    new SqliteParameter("@Id", stateId),
                    new SqliteParameter("@Fts", fingerprint),
                    new SqliteParameter("@Updated", (object?)updatedAt ?? DBNull.Value),
                    new SqliteParameter("@Revision", (object?)revision ?? DBNull.Value),
                    new SqliteParameter("@Len", (object?)length ?? DBNull.Value),
                    new SqliteParameter("@Now", now));
            });
        }

        OnConversationIndexed?.Invoke(conversationId, now);
    }

    /// <summary>
    /// One stored conversation, loaded the way the index sees it.
    /// </summary>
    /// <param name="Fingerprint">
    /// A hash of exactly what is in <paramref name="State"/>, which is what
    /// <c>IndexProgress.FtsRevision</c> records. Both workers compare against this, so neither
    /// can believe it is current when the other indexed different content.
    /// </param>
    /// <param name="Origins">
    /// One entry per message in <c>State.Messages</c>, <see cref="SearchOrigins.Raw"/> or
    /// <see cref="SearchOrigins.Compressed"/>. The list is the full transcript - every raw
    /// message ever saved, then every compaction summary - so a message compaction dropped from
    /// the model's context is still here, and the summary that replaced it is here too.
    /// </param>
    public sealed record IndexableConversation(
        string Source,
        string Id,
        string Fingerprint,
        ConversationState State,
        string? UpdatedAt,
        long Revision,
        int JsonLength,
        IReadOnlyList<string> Origins);

    /// <summary>
    /// Reads one source row and assembles the state both the FTS worker and the vector worker
    /// index. Null when the row is gone.
    /// </summary>
    /// <remarks>
    /// Shared deliberately rather than duplicated. The two workers have to agree on which
    /// messages a conversation contains and in what order: the vector worker records a message's
    /// position so the search dialog can scroll to it, and a second loader that assembled the
    /// list differently would put chunks against the wrong messages without anything failing.
    /// </remarks>
    public IndexableConversation? LoadForIndexing(string source, string id)
    {
        if (string.IsNullOrWhiteSpace(source))
        {
            throw new ArgumentException("source is required", nameof(source));
        }

        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("id is required", nameof(id));
        }

        var dream = source == DreamSource;
        var table = dream ? "DreamConversationState" : "ConversationState";
        var row = _source.Query(
            $"SELECT Id, ProjectId, State, Revision, UpdatedAt FROM {table} WHERE Id = @Id",
            new SqliteParameter("@Id", id)).FirstOrDefault();

        if (row == null)
        {
            AutomatonDebug.Log($"ConversationSearchService.LoadForIndexing: {source}:{id} is not in the source");
            return null;
        }

        var json = row["State"] as string ?? string.Empty;

        // A conversation saved through UpdateStateAsync has fresh rows in ConversationMessages and
        // a stale State column, so the messages are read from the stored tables when they have
        // them and the fingerprint covers what is actually indexed - not the JSON alone. The
        // stored transcript is the full one: raw messages first, compaction summaries after,
        // each tagged with its origin.
        var (normalized, origins) = dream ? (new List<Message>(), new List<string>()) : LoadFullTranscriptForIndexing(id);
        var fingerprint = Fingerprint(json, normalized, origins);

        ConversationState? state = null;
        try
        {
            state = string.IsNullOrWhiteSpace(json) ? null : JsonSerializer.Deserialize<ConversationState>(json, ReadOptions);
        }
        catch (JsonException ex)
        {
            AutomatonDebug.LogWarning($"ConversationSearchService.LoadForIndexing: {source}:{id} has unreadable state: {ex.Message}");
        }

        state ??= new ConversationState();
        if (normalized.Count > 0)
        {
            state.Messages = normalized;
        }

        if (dream && (state.Messages == null || state.Messages.Count == 0))
        {
            state.Messages = LoadDreamMessages(id);
        }

        if (normalized.Count == 0)
        {
            // Nothing stored in the message tables (a dream mirror, or a row that only ever had
            // JSON): the in-memory messages may still carry a merged summary, so split it the
            // same way the stored path would have.
            (state.Messages, origins) = SplitForIndexing(state.Messages);
        }

        state.SessionId = string.IsNullOrWhiteSpace(state.SessionId) ? id : state.SessionId;
        state.ProjectId ??= row["ProjectId"] as string;

        return new IndexableConversation(
            source,
            id,
            fingerprint,
            state,
            row["UpdatedAt"] as string,
            row["Revision"] == null ? 0L : Convert.ToInt64(row["Revision"]),
            json.Length,
            origins);
    }

    /// <summary>
    /// The stored full transcript as the index sees it: one message and one origin per row, in
    /// the order <see cref="ConfigDatabase.LoadFullTranscript"/> defines - which is also the
    /// order the transcript viewer shows, so chunk positions and hit positions line up.
    /// </summary>
    private (List<Message> Messages, List<string> Origins) LoadFullTranscriptForIndexing(string stateId)
    {
        var messages = new List<Message>();
        var origins = new List<string>();
        try
        {
            foreach (var r in _source.LoadFullTranscript(stateId))
            {
                messages.Add(new Message
                {
                    Id = r.TryGetValue("MessageId", out var mid) ? mid as string ?? string.Empty : string.Empty,
                    Role = r.TryGetValue("Role", out var role) ? role as string ?? string.Empty : string.Empty,
                    Content = StripReasoning(r.TryGetValue("Content", out var content) ? content as string : null),
                });
                origins.Add(SearchOrigins.Normalize(r.TryGetValue("Origin", out var origin) ? origin as string : null));
            }
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ConversationSearchService.LoadFullTranscriptForIndexing: {stateId}: {ex.Message}");
            messages.Clear();
            origins.Clear();
        }

        AutomatonDebug.Log($"ConversationSearchService.LoadFullTranscriptForIndexing: {stateId} - {messages.Count} row(s), {origins.Count(o => o == SearchOrigins.Compressed)} compressed");
        return (messages, origins);
    }

    /// <summary>
    /// Splits an in-memory message list the way the stored full transcript is laid out: the
    /// summary compaction merged into a system message is taken out of it and appended as its
    /// own compressed entry, so what the index holds under "raw" never contains compressor
    /// output and what it holds under "compressed" is exactly the summary.
    /// </summary>
    public static (List<Message> Messages, List<string> Origins) SplitForIndexing(IReadOnlyList<Message>? source)
    {
        var messages = new List<Message>();
        var origins = new List<string>();
        var summaries = new List<Message>();

        foreach (var message in source ?? Array.Empty<Message>())
        {
            if (message == null)
            {
                continue;
            }

            var summary = string.Equals(message.Role, "system", StringComparison.OrdinalIgnoreCase)
                ? CompressionSupport.ExtractSummary(message.Content)
                : null;

            if (summary == null)
            {
                messages.Add(new Message { Id = message.Id, Role = message.Role, Content = StripReasoning(message.Content), Parts = message.Parts });
                origins.Add(SearchOrigins.Raw);
                continue;
            }

            // A system message that is nothing but the summary - compaction with no system
            // prompt to merge into - leaves nothing raw behind, and an empty row would be a
            // blank entry in the viewer.
            var stripped = CompressionSupport.StripSummary(message.Content);
            if (!string.IsNullOrWhiteSpace(stripped))
            {
                messages.Add(new Message
                {
                    Id = message.Id,
                    Role = message.Role,
                    Content = stripped,
                    Parts = message.Parts
                });
                origins.Add(SearchOrigins.Raw);
            }

            summaries.Add(new Message
            {
                Id = string.IsNullOrWhiteSpace(message.Id) ? null : $"summary/{message.Id}",
                Role = "system",
                Content = summary
            });
        }

        foreach (var summary in summaries)
        {
            messages.Add(summary);
            origins.Add(SearchOrigins.Compressed);
        }

        return (messages, origins);
    }

    private static ConversationState CloneWithMessages(ConversationState state, List<Message> messages)
    {
        return new ConversationState
        {
            SessionId = state.SessionId,
            MessageId = state.MessageId,
            Agent = state.Agent,
            Directory = state.Directory,
            Worktree = state.Worktree,
            ProjectId = state.ProjectId,
            Messages = messages,
            DisplayName = state.DisplayName,
            Revision = state.Revision,
            LastSavedAt = state.LastSavedAt
        };
    }

    /// <summary>The fingerprint over the stored JSON and the normalized messages together.</summary>
    public static string Fingerprint(string json, IReadOnlyList<Message> messages)
    {
        return Fingerprint(json, messages, Array.Empty<string>());
    }

    /// <summary>
    /// The fingerprint over the stored JSON, the transcript rows and their origins. The origin
    /// is hashed because the same text moving from raw to compressed is a change the index has
    /// to notice.
    /// </summary>
    public static string Fingerprint(string json, IReadOnlyList<Message> messages, IReadOnlyList<string> origins)
    {
        using var sha = SHA256.Create();
        void Feed(string text)
        {
            var bytes = Encoding.UTF8.GetBytes(text ?? string.Empty);
            sha.TransformBlock(bytes, 0, bytes.Length, null, 0);
            sha.TransformBlock(new byte[] { 0 }, 0, 1, null, 0);
        }

        Feed(json);
        var list = messages ?? Array.Empty<Message>();
        for (var i = 0; i < list.Count; i++)
        {
            var message = list[i];
            Feed(message.Role ?? string.Empty);
            Feed(message.Content ?? string.Empty);
            Feed(origins != null && i < origins.Count ? origins[i] : SearchOrigins.Raw);
        }

        sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
        return Convert.ToHexString(sha.Hash ?? Array.Empty<byte>());
    }

    private List<Message> LoadDreamMessages(string id)
    {
        try
        {
            return _source.Query(
                    "SELECT Role, Content FROM DreamConversationMessages WHERE StateId = @StateId ORDER BY MessageIndex ASC",
                    new SqliteParameter("@StateId", id))
                .Select(r => new Message { Role = r["Role"] as string ?? string.Empty, Content = r["Content"] as string ?? string.Empty })
                .ToList();
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ConversationSearchService.LoadDreamMessages: {id}: {ex.Message}");
            return new List<Message>();
        }
    }

    /// <summary>Removes one source row's index rows and its progress row.</summary>
    public void RemoveStateFromIndex(string source, string id)
    {
        if (string.IsNullOrWhiteSpace(id) || !EnsureWritable(nameof(RemoveStateFromIndex)))
        {
            return;
        }

        var table = source == DreamSource ? "DreamConversationSearch" : "ConversationSearch";
        lock (_writeLock)
        {
            _db.ExecuteInTransaction(tx =>
            {
                tx.Execute($"DELETE FROM {table} WHERE StateId = @Id", new SqliteParameter("@Id", id));
                tx.Execute("DELETE FROM IndexProgress WHERE Source = @Source AND Id = @Id",
                    new SqliteParameter("@Source", source), new SqliteParameter("@Id", id));
            });
        }

        AutomatonDebug.Log($"ConversationSearchService.RemoveStateFromIndex: removed {source}:{id}");
    }

    /// <summary>Removes a conversation from the chat index by the SessionId results carry.</summary>
    public void RemoveFromIndex(string conversationId)
    {
        if (string.IsNullOrWhiteSpace(conversationId) || !EnsureWritable(nameof(RemoveFromIndex)))
        {
            return;
        }

        lock (_writeLock)
        {
            _db.ExecuteInTransaction(tx =>
            {
                var stateIds = tx.Query("SELECT DISTINCT StateId FROM ConversationSearch WHERE ConversationId = @C", new SqliteParameter("@C", conversationId))
                    .Select(r => r["StateId"] as string)
                    .Where(s => !string.IsNullOrWhiteSpace(s))
                    .ToList();

                tx.Execute("DELETE FROM ConversationSearch WHERE ConversationId = @C", new SqliteParameter("@C", conversationId));
                foreach (var stateId in stateIds)
                {
                    tx.Execute("DELETE FROM IndexProgress WHERE Source = @Source AND Id = @Id",
                        new SqliteParameter("@Source", ChatSource), new SqliteParameter("@Id", stateId!));
                }
            });
        }
    }

    /// <summary>
    /// Rebuilds the index row by row from the source. In the indexing process this marks every
    /// row stale and reconciles, so search keeps answering throughout; elsewhere it asks the
    /// indexer in this process, or does nothing when this process does not index.
    /// </summary>
    public void RebuildIndex()
    {
        AutomatonDebug.Log("ConversationSearchService.RebuildIndex: Entry");
        var indexer = SearchIndexer.Current;
        if (indexer != null)
        {
            indexer.RequestRebuild();
            indexer.ReconcileAsync(CancellationToken.None).GetAwaiter().GetResult();
            AutomatonDebug.Log("ConversationSearchService.RebuildIndex: Exit - handed to the indexer");
            return;
        }

        if (!EnsureWritable(nameof(RebuildIndex)))
        {
            return;
        }

        try
        {
            _db.Execute("UPDATE IndexProgress SET FtsRevision = NULL");
            foreach (var (source, table) in new[] { (ChatSource, "ConversationState"), (DreamSource, "DreamConversationState") })
            {
                foreach (var row in _source.Query($"SELECT Id FROM {table}"))
                {
                    var id = row["Id"] as string;
                    if (!string.IsNullOrWhiteSpace(id))
                    {
                        IndexStoredConversation(source, id);
                    }
                }
            }

            AutomatonDebug.Log($"ConversationSearchService.RebuildIndex: Exit - {GetIndexedCount()} conversations indexed");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationSearchService.RebuildIndex: failed: {ex.Message}");
        }
    }

    public void RebuildIndexInBackground()
    {
        var indexer = SearchIndexer.Current;
        if (indexer != null)
        {
            indexer.RequestRebuild();
            return;
        }

        if (!_db.Writable)
        {
            AutomatonDebug.Log("ConversationSearchService.RebuildIndexInBackground: this process does not index");
            return;
        }

        _ = Task.Run(RebuildIndex);
    }

    private bool EnsureWritable(string what)
    {
        if (_db.Writable && _db.IsAvailable)
        {
            return true;
        }

        AutomatonDebug.Log($"ConversationSearchService.{what}: this process may only read the index; the master maintains it");
        return false;
    }

    /// <summary>SHA-256 of the stored JSON, hex. What <c>IndexProgress</c> compares.</summary>
    public static string Fingerprint(string json)
    {
        var bytes = SHA256.HashData(Encoding.UTF8.GetBytes(json ?? string.Empty));
        return Convert.ToHexString(bytes);
    }

    // ── Search ────────────────────────────────────────────────────────────

    /// <summary>Marker wrapped around each matched term inside a hit snippet.</summary>
    public static readonly string HighlightStart = ((char)1).ToString();

    /// <summary>Closing counterpart to <see cref="HighlightStart"/>.</summary>
    public static readonly string HighlightEnd = ((char)2).ToString();

    /// <summary>
    /// Searches conversation history for matching text, one result per conversation. By default
    /// searches normal chats only; pass <paramref name="source"/> to target dream mirrors or both.
    /// </summary>
    public List<ConversationSearchResult> Search(string query, int maxResults = 10, string? projectId = null, SearchSource source = SearchSource.Chats)
    {
        if (string.IsNullOrWhiteSpace(query) || !_db.IsAvailable)
        {
            return [];
        }

        AutomatonDebug.Log($"ConversationSearchService.Search: query='{query}', max={maxResults}, projectId='{projectId ?? "null"}', source={source}");
        var results = new List<ConversationSearchResult>();

        try
        {
            if (source is SearchSource.Chats or SearchSource.Both)
            {
                AddSearchResults(query, maxResults, projectId, results, dream: false);
            }

            if (source is SearchSource.Dreams or SearchSource.Both)
            {
                var remaining = source == SearchSource.Both ? Math.Max(0, maxResults - results.Count) : maxResults;
                if (remaining > 0)
                {
                    AddSearchResults(query, remaining, projectId, results, dream: true);
                }
            }

            AttachImportance(results);
            AutomatonDebug.Log($"ConversationSearchService.Search: '{query}' returned {results.Count} conversations");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationSearchService.Search: failed: {ex.Message}");
        }

        return results;
    }

    /// <summary>
    /// Every matching message as its own hit, for the search dialog that groups hits under a
    /// conversation. Matched terms are wrapped in <see cref="HighlightStart"/>/<see cref="HighlightEnd"/>.
    /// </summary>
    public List<ConversationMessageHit> SearchMessages(string query, int maxHits = 500, string? projectId = null)
    {
        return SearchMessages(query, maxHits, projectId, null);
    }

    /// <summary>
    /// Message-level keyword search restricted to the given origins. Null means every origin;
    /// an empty list means none (nothing is returned); otherwise only rows whose <c>Origin</c>
    /// is listed - so a reader can ask for raw messages alone, summaries alone, or both. The
    /// filter is applied in the query, not to the result, so the hit limit is spent on rows the
    /// caller wants.
    /// </summary>
    public List<ConversationMessageHit> SearchMessages(string query, int maxHits, string? projectId, IReadOnlyCollection<string>? origins)
    {
        return SearchMessages(query, maxHits, projectId, origins, default);
    }

    /// <summary>Keyword search over the given origins, limited to conversations active in <paramref name="dates"/>.</summary>
    public List<ConversationMessageHit> SearchMessages(string query, int maxHits, string? projectId, IReadOnlyCollection<string>? origins, SearchDateRange dates,
        string? excludeConversationId = null)
    {
        var originFilter = NormalizeOrigins(origins);
        AutomatonDebug.Log($"ConversationSearchService.SearchMessages: Entry - query='{query}', maxHits={maxHits}, projectId='{projectId ?? "null"}', origins={(originFilter == null ? "all" : string.Join("+", originFilter))}, dates={dates.FromKey ?? "-"}..{dates.ToExclusiveKey ?? "-"}");
        if (string.IsNullOrWhiteSpace(query) || maxHits <= 0 || !_db.IsAvailable)
        {
            return [];
        }

        if (originFilter != null && originFilter.Count == 0)
        {
            AutomatonDebug.Log("ConversationSearchService.SearchMessages: Exit - every origin excluded, nothing can match");
            return [];
        }

        var hits = new List<ConversationMessageHit>();
        try
        {
            var ftsQuery = FormatFtsQuery(query, out var needsOrFallback);
            hits = QueryMessageHits(ftsQuery, maxHits, projectId, originFilter, dates, excludeConversationId: excludeConversationId);

            if (hits.Count == 0 && needsOrFallback)
            {
                var orQuery = BuildOrFallbackQuery(query.Split(' ', StringSplitOptions.RemoveEmptyEntries));
                hits = QueryMessageHits(orQuery, maxHits, projectId, originFilter, dates, excludeConversationId: excludeConversationId);
            }

            if (hits.Count == 0)
            {
                hits = QueryLiteralMessageHits(query, maxHits, projectId, originFilter, dates, excludeConversationId: excludeConversationId);
            }
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationSearchService.SearchMessages: failed for '{query}': {ex.Message}");
        }

        AttachImportance(hits);
        AutomatonDebug.Log($"ConversationSearchService.SearchMessages: Exit - {hits.Count} hit(s)");
        return hits;
    }

    /// <summary>
    /// Fills in <see cref="ConversationMessageHit.Importance"/> from the reader's tags, one
    /// query for the whole list. A hit without a stored row (an older index row with no
    /// message id) stays untagged.
    /// </summary>
    private void AttachImportance(List<ConversationMessageHit> hits)
    {
        if (hits == null || hits.Count == 0)
        {
            return;
        }

        try
        {
            var tags = _source.LoadMemoryTags(hits.Select(h => h.StateId));
            if (tags.Count == 0)
            {
                return;
            }

            var tagged = 0;
            foreach (var hit in hits)
            {
                if (string.IsNullOrWhiteSpace(hit.StateId) || string.IsNullOrWhiteSpace(hit.MessageId))
                {
                    continue;
                }

                if (tags.TryGetValue(ConfigDatabase.MemoryTagKey(hit.StateId, hit.MessageId), out var score))
                {
                    hit.Importance = score;
                    tagged++;
                }
            }

            AutomatonDebug.Log($"ConversationSearchService.AttachImportance: {tagged} of {hits.Count} hit(s) carry a tag");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ConversationSearchService.AttachImportance: {ex.Message}; hits stay untagged");
        }
    }

    /// <summary>The conversation-level twin of <see cref="AttachImportance(List{ConversationMessageHit})"/>.</summary>
    private void AttachImportance(List<ConversationSearchResult> results)
    {
        if (results == null || results.Count == 0)
        {
            return;
        }

        try
        {
            var tags = _source.LoadMemoryTags(results.Select(r => r.StateId));
            if (tags.Count == 0)
            {
                return;
            }

            foreach (var result in results)
            {
                if (string.IsNullOrWhiteSpace(result.StateId) || string.IsNullOrWhiteSpace(result.MessageId))
                {
                    continue;
                }

                if (tags.TryGetValue(ConfigDatabase.MemoryTagKey(result.StateId, result.MessageId), out var score))
                {
                    result.Importance = score;
                }
            }
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ConversationSearchService.AttachImportance: {ex.Message}; results stay untagged");
        }
    }


    /// <summary>
    /// Null when every origin is wanted (a null or empty request, or one naming both);
    /// otherwise the recognised origins asked for, which may be empty when nothing recognised
    /// was named.
    /// </summary>
    private static IReadOnlyList<string>? NormalizeOrigins(IReadOnlyCollection<string>? origins)
    {
        if (origins == null)
        {
            return null;
        }

        var wanted = origins
            .Where(o => !string.IsNullOrWhiteSpace(o))
            .Select(SearchOrigins.Normalize)
            .Distinct(StringComparer.Ordinal)
            .ToList();

        if (wanted.Contains(SearchOrigins.Raw) && wanted.Contains(SearchOrigins.Compressed))
        {
            return null;
        }

        return wanted;
    }

    /// <summary>
    /// The SQL for a date range, or empty when there is none. Reads the source through the
    /// attached alias, so nothing is copied into the index and the dates cannot go stale. Stored
    /// values are ISO-8601 strings in two suffix forms ('Z' and '+00:00'); comparing against a
    /// suffix-free prefix orders correctly with both.
    /// </summary>
    private static string DateRangePredicate(SearchDateRange range, string tableName, List<SqliteParameter> parameters)
    {
        if (range.IsEmpty)
        {
            return string.Empty;
        }

        var clauses = new List<string>();
        if (range.FromKey != null)
        {
            clauses.Add("COALESCE(cs.UpdatedAt, cs.CreatedAt) >= @DateFrom");
            parameters.Add(new SqliteParameter("@DateFrom", range.FromKey));
        }

        if (range.ToExclusiveKey != null)
        {
            clauses.Add("COALESCE(cs.CreatedAt, cs.UpdatedAt) < @DateTo");
            parameters.Add(new SqliteParameter("@DateTo", range.ToExclusiveKey));
        }

        return $@" AND EXISTS (
            SELECT 1 FROM {SearchDatabase.SourceAlias}.ConversationState cs
            WHERE cs.Id = {tableName}.StateId AND {string.Join(" AND ", clauses)})";
    }

    /// <summary>Which of these state ids fall inside the range. Empty range: all of them.</summary>
    private HashSet<string> StateIdsInRange(IEnumerable<string> stateIds, SearchDateRange range)
    {
        var ids = stateIds.Where(id => !string.IsNullOrWhiteSpace(id)).Distinct(StringComparer.Ordinal).ToList();
        if (range.IsEmpty || ids.Count == 0)
        {
            return new HashSet<string>(ids, StringComparer.Ordinal);
        }

        var inside = new HashSet<string>(StringComparer.Ordinal);
        try
        {
            var parameters = new List<SqliteParameter>();
            var names = new List<string>();
            for (var i = 0; i < ids.Count; i++)
            {
                names.Add($"@Id{i}");
                parameters.Add(new SqliteParameter($"@Id{i}", ids[i]));
            }

            var clauses = new List<string>();
            if (range.FromKey != null)
            {
                clauses.Add("COALESCE(cs.UpdatedAt, cs.CreatedAt) >= @DateFrom");
                parameters.Add(new SqliteParameter("@DateFrom", range.FromKey));
            }

            if (range.ToExclusiveKey != null)
            {
                clauses.Add("COALESCE(cs.CreatedAt, cs.UpdatedAt) < @DateTo");
                parameters.Add(new SqliteParameter("@DateTo", range.ToExclusiveKey));
            }

            var rows = _db.Query(
                $"SELECT cs.Id FROM {SearchDatabase.SourceAlias}.ConversationState cs WHERE cs.Id IN ({string.Join(", ", names)}) AND {string.Join(" AND ", clauses)}",
                parameters.ToArray());
            foreach (var row in rows)
            {
                if (row["Id"] is string id)
                {
                    inside.Add(id);
                }
            }
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ConversationSearchService.StateIdsInRange: {ex.Message}; keeping every hit");
            return new HashSet<string>(ids, StringComparer.Ordinal);
        }

        AutomatonDebug.Log($"ConversationSearchService.StateIdsInRange: {inside.Count} of {ids.Count} conversation(s) inside {range.FromKey ?? "-"}..{range.ToExclusiveKey ?? "-"}");
        return inside;
    }

    /// <summary>The SQL for an origin filter, or empty when every origin is wanted.</summary>
    private static string OriginPredicate(IReadOnlyList<string>? origins, List<SqliteParameter> parameters)
    {
        if (origins == null)
        {
            return string.Empty;
        }

        var names = new List<string>();
        for (var i = 0; i < origins.Count; i++)
        {
            var name = $"@Origin{i}";
            names.Add(name);
            parameters.Add(new SqliteParameter(name, origins[i]));
        }

        // Rows indexed before Origin existed were rebuilt, so a null origin is not expected;
        // treat one as raw anyway rather than losing the row from both views.
        var predicate = $" AND COALESCE(Origin, '{SearchOrigins.Raw}') IN ({string.Join(", ", names)})";
        return predicate;
    }

    // ── Semantic search ───────────────────────────────────────────────────

    /// <summary>How long a query embedding may take before the search gives up and stays lexical.</summary>
    private static readonly TimeSpan QueryEmbeddingTimeout = TimeSpan.FromSeconds(10);

    /// <summary>
    /// Keyword search, plus conversations that mean what was asked, merged into one order.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The lexical half is exactly <see cref="Search(string, int, string?, SearchSource)"/> and is
    /// never degraded by this: when no model is configured, no server is answering, or no vector
    /// generation is live, the semantic list is empty and the fusion reproduces the lexical order
    /// unchanged.
    /// </para>
    /// <para>
    /// The query is embedded with a short timeout - shorter than the indexer's - so a server busy
    /// with a backfill batch costs a lexical answer rather than a stalled tool call.
    /// </para>
    /// </remarks>
    public async Task<List<ConversationSearchResult>> SearchAsync(
        string query,
        int maxResults = 10,
        string? projectId = null,
        SearchSource source = SearchSource.Chats,
        bool semantic = true,
        CancellationToken cancellationToken = default)
    {
        var lexical = Search(query, maxResults, projectId, source);
        if (!semantic || string.IsNullOrWhiteSpace(query))
        {
            return lexical;
        }

        var neighbours = await NearestAsync(query, projectId, source, maxResults * 4, cancellationToken).ConfigureAwait(false);
        if (neighbours.Count == 0)
        {
            return lexical;
        }

        // One result per conversation, keeping its nearest chunk - the same shape the lexical
        // half returns, so the two lists are comparable at all.
        var best = new Dictionary<string, ChunkNeighbour>(StringComparer.Ordinal);
        foreach (var neighbour in neighbours)
        {
            if (!best.TryGetValue(neighbour.ConversationId, out var existing) || neighbour.Distance < existing.Distance)
            {
                best[neighbour.ConversationId] = neighbour;
            }
        }

        var semanticResults = best.Values
            .OrderBy(n => n.Distance)
            .Select(n => new ConversationSearchResult
            {
                ConversationId = n.ConversationId,
                SessionId = n.ConversationId,
                ProjectId = n.ProjectId,
                Role = n.Role,
                Content = n.Text,
                Snippet = Excerpt(n.Text),
                Source = n.Source,
                MatchKind = MatchKinds.Semantic,
                Distance = n.Distance,
                Origin = n.Origin,
                StateId = n.StateId,
                MessageId = n.MessageId,
            })
            .ToList();

        var fused = HybridRanker.Fuse(lexical, semanticResults, r => r.ConversationId, maxResults);
        var merged = new List<ConversationSearchResult>(fused.Count);
        foreach (var hit in fused)
        {
            hit.Item.MatchKind = hit.MatchKind;
            if (hit.SemanticRank.HasValue && best.TryGetValue(hit.Item.ConversationId, out var neighbour))
            {
                hit.Item.Distance = neighbour.Distance;
            }

            merged.Add(hit.Item);
        }

        AttachImportance(merged.Where(r => r.Importance == null).ToList());
        AutomatonDebug.Log($"ConversationSearchService.SearchAsync: '{query}' - {lexical.Count} lexical, {semanticResults.Count} semantic, {merged.Count} merged");
        return merged;
    }

    /// <summary>
    /// Message-level search, with chunks that mean what was asked merged into the same order.
    /// </summary>
    /// <remarks>
    /// The three inputs are three sources, each of which stands alone. <paramref name="origins"/>
    /// says which rows keyword search reads - raw messages, summaries, both, or (an empty list)
    /// neither. <paramref name="semantic"/> adds hits found by meaning over every origin,
    /// regardless of that filter, so a search by meaning alone is a valid request: an empty
    /// origin list with <paramref name="semantic"/> on returns only what meaning found.
    /// </remarks>
    public async Task<List<ConversationMessageHit>> SearchMessagesAsync(
        string query,
        int maxHits = 500,
        string? projectId = null,
        bool semantic = true,
        CancellationToken cancellationToken = default,
        IReadOnlyCollection<string>? origins = null,
        SearchDateRange dates = default)
    {
        var originFilter = NormalizeOrigins(origins);
        var keywordsOff = originFilter != null && originFilter.Count == 0;
        var lexical = keywordsOff ? new List<ConversationMessageHit>() : SearchMessages(query, maxHits, projectId, originFilter, dates);
        if (!semantic || string.IsNullOrWhiteSpace(query) || maxHits <= 0)
        {
            return lexical;
        }

        var neighbours = await NearestAsync(query, projectId, SearchSource.Chats, Math.Min(maxHits, 50), cancellationToken).ConfigureAwait(false);
        if (!dates.IsEmpty && neighbours.Count > 0)
        {
            var inside = StateIdsInRange(neighbours.Select(n => n.StateId), dates);
            neighbours = neighbours.Where(n => inside.Contains(n.StateId)).ToList();
        }

        if (neighbours.Count == 0)
        {
            return lexical;
        }

        var semanticHits = neighbours
            .Select(n => new ConversationMessageHit
            {
                ConversationId = n.ConversationId,
                ProjectId = n.ProjectId,
                Role = n.Role,
                MarkedSnippet = Excerpt(n.Text),
                MatchKind = MatchKinds.Semantic,
                Distance = n.Distance,
                StateId = n.StateId,
                MessageIndex = n.MessageIndex,
                MessageId = n.MessageId,
                Origin = n.Origin,
            })
            .ToList();

        // Keyed by conversation and message, so a keyword hit and a semantic hit on the same
        // message are one row rather than two saying the same thing.
        var fused = HybridRanker.Fuse(
            lexical,
            semanticHits,
            h => h.MessageIndex >= 0 ? $"{h.ConversationId}#{h.MessageIndex}" : $"{h.ConversationId}#{h.MarkedSnippet}",
            maxHits);

        var merged = fused.Select(h =>
        {
            h.Item.MatchKind = h.MatchKind;
            return h.Item;
        }).ToList();

        // The semantic half arrived untagged; one query covers the merged list.
        AttachImportance(merged.Where(h => h.Importance == null).ToList());

        AutomatonDebug.Log($"ConversationSearchService.SearchMessagesAsync: '{query}' - {lexical.Count} lexical, {semanticHits.Count} semantic, {merged.Count} merged");
        return merged;
    }

    /// <summary>
    /// Embeds the query and asks the index for its nearest chunks. Empty - never an exception -
    /// whenever semantic search cannot answer, so a caller's lexical results always stand.
    /// </summary>
    private async Task<IReadOnlyList<ChunkNeighbour>> NearestAsync(
        string query,
        string? projectId,
        SearchSource source,
        int k,
        CancellationToken cancellationToken)
    {
        var binding = Volatile.Read(ref _semantic);
        var client = binding.Client;
        if (client == null || !_db.IsAvailable)
        {
            return Array.Empty<ChunkNeighbour>();
        }

        var index = binding.Neighbours ?? (_defaultScan ??= new ManagedVectorScan(_db));

        try
        {
            var generation = new VectorIndex(_db).ActiveGeneration();
            if (generation == null)
            {
                AutomatonDebug.Log("ConversationSearchService.NearestAsync: no vector generation is active yet; lexical only");
                return Array.Empty<ChunkNeighbour>();
            }

            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(QueryEmbeddingTimeout);

            if (!await client.IsAvailableAsync(timeout.Token).ConfigureAwait(false))
            {
                AutomatonDebug.Log("ConversationSearchService.NearestAsync: the embedding server is not answering; lexical only");
                return Array.Empty<ChunkNeighbour>();
            }

            var embedded = await client.EmbedAsync(new[] { EmbeddingContract.QueryText(query) }, timeout.Token).ConfigureAwait(false);
            if (embedded.Count == 0)
            {
                return Array.Empty<ChunkNeighbour>();
            }

            var nearest = index.Nearest(embedded[0], generation.Value, projectId, source, k);
            var kept = nearest.Where(n => n.Distance <= MaxSemanticDistance).ToList();
            if (kept.Count < nearest.Count)
            {
                AutomatonDebug.Log($"ConversationSearchService.NearestAsync: '{query}' - dropped {nearest.Count - kept.Count} of {nearest.Count} chunk(s) beyond distance {MaxSemanticDistance}");
            }

            return kept;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ConversationSearchService.NearestAsync: '{query}' fell back to lexical: {ex.Message}");
            return Array.Empty<ChunkNeighbour>();
        }
    }

    /// <summary>
    /// The visible piece of a semantic hit. No highlight markers: nothing in it is guaranteed to
    /// match a word of the query, which is the point.
    /// </summary>
    private static string Excerpt(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return string.Empty;
        }

        var trimmed = text.Trim();
        return trimmed.Length <= 240 ? trimmed : trimmed[..240] + "...";
    }

    /// <summary>Bounded keyword results. Archive filtering happens before paging; errors are surfaced to the caller.</summary>
    public List<ConversationMessageHit> SearchMessagePage(string query, int limit, int offset = 0,
        string? projectId = null, string archive = "active", IReadOnlyCollection<string>? origins = null)
    {
        if (string.IsNullOrWhiteSpace(query) || query.Length > 1000) throw new ArgumentException("Supply a search query of 1–1000 characters.");
        if (limit is < 1 or > 101 || offset is < 0 or > 1_000_000) throw new ArgumentOutOfRangeException(nameof(limit));
        if (archive is not ("active" or "archived" or "all")) throw new ArgumentException("Unknown archive filter.");
        if (!_db.IsAvailable) throw new InvalidOperationException("The search index is unavailable.");
        var filter = NormalizeOrigins(origins);
        var formatted = FormatFtsQuery(query, out var fallback);
        var literal = false;
        // Select the matching strategy at offset zero. A later empty page must not
        // suddenly switch to broader OR/literal matches and repeat earlier results.
        if (QueryMessageHits(formatted, 1, null, filter, archive: archive, sourceProjectId: projectId).Count == 0)
        {
            if (fallback)
            {
                var alternative = BuildOrFallbackQuery(query.Split(' ', StringSplitOptions.RemoveEmptyEntries));
                if (QueryMessageHits(alternative, 1, null, filter, archive: archive, sourceProjectId: projectId).Count != 0)
                    formatted = alternative;
                else literal = true;
            }
            else literal = true;
        }
        var hits = literal ? QueryLiteralMessageHits(query, limit, null, filter, offset: offset, archive: archive, sourceProjectId: projectId)
            : QueryMessageHits(formatted, limit, null, filter, offset: offset, archive: archive, sourceProjectId: projectId);
        AttachPageImportance(hits);
        return hits;
    }

    private void AttachPageImportance(List<ConversationMessageHit> hits)
    {
        var wanted = hits.Where(hit => !string.IsNullOrWhiteSpace(hit.MessageId))
            .Select(hit => (hit.StateId, MessageId: hit.MessageId!)).Distinct().ToList();
        if (wanted.Count == 0) return;
        try
        {
            // Load only the page's tags, rather than every tag in its chats.
            var parameters = new List<SqliteParameter>();
            var values = new List<string>();
            for (var i = 0; i < wanted.Count; i++)
            {
                values.Add($"(@s{i},@m{i})");
                parameters.Add(new($"@s{i}", wanted[i].StateId));
                parameters.Add(new($"@m{i}", wanted[i].MessageId));
            }
            var tags = _source.Query($"WITH page(StateId,MessageId) AS (VALUES {string.Join(",", values)}) SELECT t.StateId,t.MessageId,t.Score FROM MemoryTags t INNER JOIN page p ON t.StateId=p.StateId AND t.MessageId=p.MessageId", parameters.ToArray())
                .ToDictionary(row => ((string)row["StateId"]!, (string)row["MessageId"]!), row => Convert.ToInt32(row["Score"]));
            foreach (var hit in hits)
                if (hit.MessageId != null && tags.TryGetValue((hit.StateId, hit.MessageId), out var score)) hit.Importance = score;
        }
        catch (Exception ex) { AutomatonDebug.LogWarning($"ConversationSearchService.AttachPageImportance: {ex.Message}; optional tags unavailable"); }
    }

    private static string ArchivePredicate(string? archive, string? projectId, List<SqliteParameter> parameters)
    {
        if (archive == null) return string.Empty; // Preserve existing search contracts.
        const string sameState = "replace(trim(t.StateFilePath),char(92),'/')=replace(trim(cs.Id),char(92),'/') COLLATE NOCASE";
        var archived = $"(COALESCE(cs.IsArchived, 0) <> 0 OR EXISTS (SELECT 1 FROM {SearchDatabase.SourceAlias}.Threads t WHERE {sameState} AND t.IsArchived<>0))";
        var project = projectId == null ? "" : $" AND (cs.ProjectId=@SourceProject OR EXISTS (SELECT 1 FROM {SearchDatabase.SourceAlias}.Threads t WHERE {sameState} AND t.ProjectId=@SourceProject))";
        if (projectId != null) parameters.Add(new SqliteParameter("@SourceProject", projectId));
        return $" AND EXISTS (SELECT 1 FROM {SearchDatabase.SourceAlias}.ConversationState cs WHERE cs.Id=ConversationSearch.StateId" + project +
            (archive == "all" ? "" : archive == "archived" ? $" AND {archived}" : $" AND NOT {archived}") + ")";
    }

    private static string ExcludeConversation(string? id, List<SqliteParameter> parameters)
    {
        if (id == null) return "";
        parameters.Add(new SqliteParameter("@ExcludedConversation", id));
        return " AND ConversationId <> @ExcludedConversation";
    }

    private List<ConversationMessageHit> QueryMessageHits(string ftsQuery, int maxHits, string? projectId, IReadOnlyList<string>? origins = null, SearchDateRange dates = default, int offset = 0, string? archive = null, string? sourceProjectId = null, string? excludeConversationId = null)
    {
        var snippet = $"snippet(ConversationSearch, 6, '{HighlightStart}', '{HighlightEnd}', '...', 24)";
        if (archive != null) snippet = $"substr({snippet},1,1600)"; // A single token can itself be huge.
        var sql = $@"
            SELECT ConversationId, ProjectId, Role, Timestamp, StateId, MessageId, MessageIndex, Origin,
                   bm25(ConversationSearch) AS Rank,
                   {snippet} AS Snippet
            FROM ConversationSearch
            WHERE ConversationSearch MATCH @Query";

        var parameters = new List<SqliteParameter> { new("@Query", ftsQuery), new("@Limit", maxHits) };
        if (!string.IsNullOrEmpty(projectId))
        {
            sql += " AND ProjectId = @ProjectId";
            parameters.Add(new SqliteParameter("@ProjectId", projectId));
        }

        sql += ExcludeConversation(excludeConversationId, parameters);
        sql += OriginPredicate(origins, parameters);
        sql += DateRangePredicate(dates, "ConversationSearch", parameters);
        sql += ArchivePredicate(archive, sourceProjectId, parameters);
        sql += RankOrdering + (archive == null ? "" : ", StateId, MessageIndex, MessageId, rowid") + " LIMIT @Limit OFFSET @Offset";
        parameters.Add(new SqliteParameter("@Offset", offset));
        return _db.Query(sql, parameters.ToArray()).Select(BuildMessageHit).Where(h => h != null).Select(h => h!).ToList();
    }

    private List<ConversationMessageHit> QueryLiteralMessageHits(string query, int maxHits, string? projectId, IReadOnlyList<string>? origins = null, SearchDateRange dates = default, int offset = 0, string? archive = null, string? sourceProjectId = null, string? excludeConversationId = null)
    {
        // Only a window around the match is read back; whole bodies would pull megabytes to discard.
        var sql = @"
            SELECT ConversationId, ProjectId, Role, Timestamp, StateId, MessageId, MessageIndex, Origin, 0 AS Rank,
                   instr(lower(Content), lower(@Query)) AS MatchPos,
                   length(Content) AS TotalLength,
                   substr(Content, MAX(1, instr(lower(Content), lower(@Query)) - 60), length(@Query) + 160) AS Snippet
            FROM ConversationSearch
            WHERE instr(lower(Content), lower(@Query)) > 0";

        var parameters = new List<SqliteParameter> { new("@Query", query), new("@Limit", maxHits) };
        if (!string.IsNullOrEmpty(projectId))
        {
            sql += " AND ProjectId = @ProjectId";
            parameters.Add(new SqliteParameter("@ProjectId", projectId));
        }

        sql += ExcludeConversation(excludeConversationId, parameters);
        sql += OriginPredicate(origins, parameters);
        sql += DateRangePredicate(dates, "ConversationSearch", parameters);
        sql += ArchivePredicate(archive, sourceProjectId, parameters);
        sql += " ORDER BY Timestamp DESC" + (archive == null ? "" : ", StateId, MessageIndex, MessageId, rowid") + " LIMIT @Limit OFFSET @Offset";
        parameters.Add(new SqliteParameter("@Offset", offset));

        var hits = new List<ConversationMessageHit>();
        foreach (var row in _db.Query(sql, parameters.ToArray()))
        {
            var hit = BuildMessageHit(row);
            if (hit == null)
            {
                continue;
            }

            var matchPos = ConvertToDouble(row["MatchPos"]);
            var totalLength = ConvertToDouble(row["TotalLength"]);
            hit.MarkedSnippet = BuildMarkedLiteralSnippet(hit.MarkedSnippet, query, matchPos > 61, matchPos + query.Length + 100 < totalLength);
            hits.Add(hit);
        }

        return hits;
    }

    private static ConversationMessageHit? BuildMessageHit(Dictionary<string, object?> row)
    {
        var conversationId = row["ConversationId"] as string ?? string.Empty;
        if (string.IsNullOrWhiteSpace(conversationId))
        {
            return null;
        }

        // A keyword hit now knows its row too, so the dialog can go straight to the message
        // instead of inferring it from the snippet; the probe remains the fallback when an
        // older index row has no position.
        return new ConversationMessageHit
        {
            ConversationId = conversationId,
            ProjectId = row["ProjectId"] as string,
            Role = row["Role"] as string ?? string.Empty,
            Timestamp = row["Timestamp"] as string ?? string.Empty,
            Rank = ConvertToDouble(row["Rank"]),
            MarkedSnippet = row["Snippet"] as string ?? string.Empty,
            StateId = row.TryGetValue("StateId", out var stateId) ? stateId as string ?? string.Empty : string.Empty,
            MessageId = row.TryGetValue("MessageId", out var messageId) ? messageId as string : null,
            MessageIndex = row.TryGetValue("MessageIndex", out var messageIndex) && messageIndex != null ? Convert.ToInt32(messageIndex) : -1,
            Origin = SearchOrigins.Normalize(row.TryGetValue("Origin", out var origin) ? origin as string : null)
        };
    }

    /// <summary>
    /// Wraps the matched term inside an already-windowed snippet and re-adds the ellipses the
    /// window lost. No whitespace collapsing: callers strip the markers to recover an exact
    /// substring of the message.
    /// </summary>
    private static string BuildMarkedLiteralSnippet(string window, string query, bool truncatedAtStart, bool truncatedAtEnd)
    {
        if (string.IsNullOrEmpty(window) || string.IsNullOrEmpty(query))
        {
            return window ?? string.Empty;
        }

        var index = window.IndexOf(query, StringComparison.OrdinalIgnoreCase);
        if (index < 0)
        {
            return window;
        }

        var marked = window[..index] + HighlightStart + window[index..(index + query.Length)] + HighlightEnd + window[(index + query.Length)..];
        if (truncatedAtStart)
        {
            marked = "..." + marked;
        }

        if (truncatedAtEnd)
        {
            marked += "...";
        }

        return marked;
    }

    private void AddSearchResults(string query, int maxResults, string? projectId, List<ConversationSearchResult> accumulator, bool dream)
    {
        var tableName = dream ? "DreamConversationSearch" : "ConversationSearch";
        var sourceTag = dream ? "dream" : "chat";

        void Add(IEnumerable<ConversationSearchResult> hits)
        {
            foreach (var hit in hits)
            {
                hit.Source = sourceTag;
                if (!accumulator.Any(r => r.ConversationId == hit.ConversationId && r.Source == hit.Source))
                {
                    accumulator.Add(hit);
                }
            }
        }

        var (ftsHits, needsOrFallback) = SearchWithFts(query, maxResults, projectId, tableName, excludeDreamChats: !dream);
        Add(ftsHits);

        // AND found nothing: retry with OR so terms can match across different messages.
        if (ftsHits.Count == 0 && needsOrFallback)
        {
            var (orHits, _) = SearchWithFts(BuildOrFallbackQuery(query.Split(' ', StringSplitOptions.RemoveEmptyEntries)), maxResults, projectId, tableName, excludeDreamChats: !dream);
            Add(orHits);
            if (orHits.Count > 0)
            {
                return;
            }
        }

        var have = accumulator.Count(r => r.Source == sourceTag);
        if (have < maxResults && (have == 0 || ShouldUseLiteralFallback(query)))
        {
            Add(SearchWithLiteralSubstring(query, maxResults - have, projectId, ftsHits, tableName, excludeDreamChats: !dream));
        }
    }

    /// <summary>
    /// The query to try when every word together found nothing. With three or more words it
    /// asks for any two of the three most specific together, so "penguin beak fracture
    /// treatment" cannot come back with everything that ever said "treatment"; with two words
    /// either alone will do, because one of them may simply be misspelt.
    /// </summary>
    private static string BuildOrFallbackQuery(string[] tokens)
    {
        var fallbackTokens = MeaningfulTerms(string.Join(" ", tokens ?? Array.Empty<string>()))
            .OrderByDescending(t => t.Any(char.IsDigit))
            .ThenByDescending(t => t.Length)
            .ThenBy(t => t, StringComparer.OrdinalIgnoreCase)
            .Take(3)
            .Select(FtsTerm)
            .ToList();

        if (fallbackTokens.Count < 3)
        {
            return string.Join(" OR ", fallbackTokens);
        }

        var pairs = new List<string>();
        for (var i = 0; i < fallbackTokens.Count; i++)
        {
            for (var j = i + 1; j < fallbackTokens.Count; j++)
            {
                pairs.Add($"({fallbackTokens[i]} AND {fallbackTokens[j]})");
            }
        }

        return string.Join(" OR ", pairs);
    }

    private static readonly Regex ReasoningBlock = new(@"<think>.*?</think>", RegexOptions.Singleline | RegexOptions.IgnoreCase | RegexOptions.Compiled);

    /// <summary>
    /// The content with the model's reasoning taken out. Half of the nearest chunks to a
    /// nonsense query were fragments like "Let me fix the query" from inside think blocks:
    /// process, not memory, and close to everything. The reply outside the block stays. A
    /// message that was nothing but reasoning becomes empty and is skipped.
    /// </summary>
    public static string StripReasoning(string? content)
    {
        if (string.IsNullOrEmpty(content) || content.IndexOf("<think>", StringComparison.OrdinalIgnoreCase) < 0)
        {
            return content ?? string.Empty;
        }

        var stripped = ReasoningBlock.Replace(content, string.Empty);

        // An unterminated block (a reply cut off mid-thought) runs to the end.
        var open = stripped.IndexOf("<think>", StringComparison.OrdinalIgnoreCase);
        if (open >= 0)
        {
            stripped = stripped[..open];
        }

        return stripped.Trim();
    }

    /// <summary>
    /// Content that is a placeholder for something removed rather than text anyone wrote -
    /// an image the desktop stripped from a computer-use turn. Indexing it makes the
    /// placeholder itself a search result.
    /// </summary>
    public static bool IsPlaceholderContent(string? content)
    {
        if (string.IsNullOrWhiteSpace(content))
        {
            return true;
        }

        var trimmed = content.Trim();
        return trimmed.StartsWith("[image from ", StringComparison.OrdinalIgnoreCase) && trimmed.EndsWith(" removed]", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// The predicate that keeps dream-driver chats (full of [DREAM] injections) out of chat
    /// results. It reads the source through the attached alias, so the index never has to
    /// copy the flag and go stale.
    /// </summary>
    private static string ExcludeDreamChatsPredicate(string tableName) =>
        $@" AND NOT EXISTS (
            SELECT 1 FROM {SearchDatabase.SourceAlias}.ConversationState cs
            WHERE cs.SessionId = {tableName}.ConversationId AND cs.ExcludeFromDream = 1)";

    private (List<ConversationSearchResult> Results, bool NeedsOrFallback) SearchWithFts(string query, int maxResults, string? projectId, string tableName, bool excludeDreamChats)
    {
        var results = new List<ConversationSearchResult>();
        var needsOrFallback = false;
        try
        {
            var ftsQuery = FormatFtsQuery(query, out needsOrFallback);
            var sql = $@"
                SELECT ConversationId, SessionId, ThreadId, ProjectId, Role, Timestamp, Origin, StateId, MessageId,
                       bm25({tableName}) AS Rank,
                       snippet({tableName}, 6, '[', ']', '...', 30) AS Snippet
                FROM {tableName}
                WHERE {tableName} MATCH @Query";

            var parameters = new List<SqliteParameter> { new("@Query", ftsQuery), new("@Limit", maxResults) };
            if (!string.IsNullOrEmpty(projectId))
            {
                sql += " AND ProjectId = @ProjectId";
                parameters.Add(new SqliteParameter("@ProjectId", projectId));
            }

            if (excludeDreamChats)
            {
                sql += ExcludeDreamChatsPredicate(tableName);
            }

            sql += RankOrdering + " LIMIT @Limit";

            foreach (var row in _db.Query(sql, parameters.ToArray()))
            {
                AddResultIfNew(results, row, row["Snippet"] as string ?? string.Empty, ConvertToDouble(row["Rank"]));
            }
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ConversationSearchService.SearchWithFts: {tableName} failed for '{query}': {ex.Message}");
        }

        return (results, needsOrFallback);
    }

    private List<ConversationSearchResult> SearchWithLiteralSubstring(string query, int maxResults, string? projectId, IEnumerable<ConversationSearchResult> existingResults, string tableName, bool excludeDreamChats)
    {
        var results = new List<ConversationSearchResult>();
        var excluded = existingResults.Select(r => r.ConversationId).Where(id => !string.IsNullOrWhiteSpace(id)).ToHashSet(StringComparer.Ordinal);

        var sql = $@"
            SELECT ConversationId, SessionId, ThreadId, ProjectId, Role, Timestamp, Origin, StateId, MessageId,
                CASE
                    WHEN instr(lower(Content), lower(@Query)) > 51
                        THEN '...' || substr(Content, instr(lower(Content), lower(@Query)) - 50, length(@Query) + 100)
                    ELSE substr(Content, 1, length(@Query) + 100)
                END ||
                CASE
                    WHEN instr(lower(Content), lower(@Query)) + length(@Query) + 50 < length(Content) THEN '...'
                    ELSE ''
                END AS Snippet
            FROM {tableName}
            WHERE instr(lower(Content), lower(@Query)) > 0";

        var parameters = new List<SqliteParameter> { new("@Query", query), new("@Limit", Math.Max(maxResults * 5, maxResults)) };
        if (!string.IsNullOrEmpty(projectId))
        {
            sql += " AND ProjectId = @ProjectId";
            parameters.Add(new SqliteParameter("@ProjectId", projectId));
        }

        if (excludeDreamChats)
        {
            sql += ExcludeDreamChatsPredicate(tableName);
        }

        sql += " ORDER BY Timestamp DESC LIMIT @Limit";

        foreach (var row in _db.Query(sql, parameters.ToArray()))
        {
            var conversationId = row["ConversationId"] as string ?? string.Empty;
            if (excluded.Contains(conversationId))
            {
                continue;
            }

            AddResultIfNew(results, row, row["Snippet"] as string ?? string.Empty, 0);
            excluded.Add(conversationId);
            if (results.Count >= maxResults)
            {
                break;
            }
        }

        return results;
    }

    /// <summary>
    /// How keyword hits are ordered. bm25 is negative, better the more negative, and a tool
    /// row's score is halved towards zero so that a fetched page or a command's output ranks
    /// behind what a person or the assistant actually said when both match. The first live test
    /// (SEARCH.md §9) returned four Wikipedia maintenance tags and an RSS URL for "military
    /// service"; they still match, they no longer lead. Tool rows are not excluded: a file path
    /// or an error string is exactly what keyword search is for.
    /// </summary>
    private const string RankOrdering = " ORDER BY (CASE WHEN Role = 'tool' THEN Rank * 0.5 ELSE Rank END)";

    /// <summary>
    /// The cosine distance beyond which a chunk is not offered as a semantic hit at all. The
    /// nearest-k scan always returns k rows, however far; without a floor a query on a common
    /// word fills the list with whatever is least far, and rank fusion then places those above
    /// real keyword hits.
    /// </summary>
    /// <remarks>
    /// Measured on 8 September 2026 against Qwen3-Embedding-0.6B over 8,199 chunks (SEARCH.md
    /// §9): true positives for five queries sat between 0.21 and 0.40; the nearest chunk to a
    /// nonsense query was 0.51; an idiom's near-misses were 0.36 to 0.43. 0.45 removes every
    /// nonsense hit and keeps every true one seen. The first guess, 0.65, never fired once.
    /// Settable so tests using a bag-of-words stand-in, whose distances are not the model's, can
    /// lift it.
    /// </remarks>
    public static double MaxSemanticDistance { get; set; } = 0.45;

    /// <summary>
    /// Words that carry no meaning on their own. They are dropped from a query before it is
    /// shaped - "asleep at the wheel" is a search for asleep and wheel - because a term like
    /// "the" matches nearly every row and, once in the OR fallback, returns the whole corpus.
    /// A query that is nothing but stopwords keeps them, so it still searches for something.
    /// </summary>
    private static readonly HashSet<string> StopWords = new(StringComparer.OrdinalIgnoreCase)
    {
        "a", "an", "the", "and", "or", "of", "to", "in", "on", "at", "for", "by", "with", "from",
        "is", "are", "was", "were", "be", "been", "it", "its", "this", "that", "these", "those",
        "as", "if", "then", "than", "so", "do", "does", "did", "has", "have", "had", "not", "no",
        "i", "me", "my", "we", "our", "you", "your", "he", "she", "they", "them", "their", "his", "her",
        "what", "which", "who", "how", "when", "where", "why", "about", "into", "up", "out", "any", "some",
    };

    /// <summary>True for a word too common to search by.</summary>
    public static bool IsStopWord(string? word)
    {
        return !string.IsNullOrWhiteSpace(word) && StopWords.Contains(word.Trim());
    }

    /// <summary>The query words that count, with stopwords removed - unless nothing else is left.</summary>
    public static List<string> MeaningfulTerms(string query)
    {
        var tokens = (query ?? string.Empty)
            .Split(' ', StringSplitOptions.RemoveEmptyEntries)
            .Select(t => t.Trim().Replace("\"", ""))
            .Where(t => t.Length > 0)
            .ToList();

        var kept = tokens.Where(t => !StopWords.Contains(t)).ToList();
        return kept.Count > 0 ? kept : tokens;
    }

    /// <summary>
    /// One query word as FTS5 sees it. Words of four letters or more match as a prefix, which
    /// is what stands in for stemming now the tokenizer does none: "service" finds "services"
    /// and "serviced", and "statue" no longer finds "status" - the porter stemmer reduced both
    /// to "statu", which is how a search for a golden statue returned Shopify status fields.
    /// Short words match exactly, because "the*" would match everything and "cat*" would find
    /// catalogue.
    /// </summary>
    private static string FtsTerm(string token)
    {
        var clean = token.Replace("\"", "");
        return clean.Length >= 4 ? $"\"{clean}\" *" : $"\"{clean}\"";
    }

    /// <summary>Formats a user query into valid FTS5 syntax.</summary>
    private static string FormatFtsQuery(string query, out bool needsOrFallback)
    {
        needsOrFallback = false;
        query = query.Trim();

        if (query.Contains(" AND ") || query.Contains(" OR ") || query.Contains(" NOT ") || query.StartsWith('"') || query.Contains('*'))
        {
            return query;
        }

        var tokens = MeaningfulTerms(query);
        if (tokens.Count == 0)
        {
            return query;
        }

        var cleanTokens = tokens.Select(FtsTerm).ToList();
        if (tokens.Count == 1)
        {
            return cleanTokens[0];
        }

        needsOrFallback = true;
        return string.Join(" AND ", cleanTokens);
    }

    private static bool ShouldUseLiteralFallback(string query)
    {
        return !string.IsNullOrWhiteSpace(query)
            && (Regex.IsMatch(query, @"[.:/\\@_-]") || Regex.IsMatch(query, @"\d+\.\d+\.\d+\.\d+"));
    }

    private static void AddResultIfNew(List<ConversationSearchResult> results, Dictionary<string, object?> row, string snippet, double rank)
    {
        var conversationId = row["ConversationId"] as string ?? string.Empty;
        if (string.IsNullOrWhiteSpace(conversationId) || results.Any(r => r.ConversationId == conversationId))
        {
            return;
        }

        results.Add(new ConversationSearchResult
        {
            ConversationId = conversationId,
            SessionId = row["SessionId"] as string ?? string.Empty,
            ThreadId = row["ThreadId"] as string ?? string.Empty,
            ProjectId = row["ProjectId"] as string,
            Role = row["Role"] as string ?? string.Empty,
            Content = row.TryGetValue("Content", out var content) ? content as string ?? string.Empty : string.Empty,
            Timestamp = row["Timestamp"] as string ?? string.Empty,
            Rank = rank,
            Snippet = snippet,
            Origin = SearchOrigins.Normalize(row.TryGetValue("Origin", out var origin) ? origin as string : null),
            StateId = row.TryGetValue("StateId", out var stateId) ? stateId as string ?? string.Empty : string.Empty,
            MessageId = row.TryGetValue("MessageId", out var messageId) ? messageId as string : null
        });
    }

    private static double ConvertToDouble(object? value)
    {
        return value switch
        {
            double d => d,
            float f => f,
            decimal m => (double)m,
            long l => l,
            int i => i,
            string s when double.TryParse(s, out var parsed) => parsed,
            _ => 0
        };
    }

    /// <summary>The number of indexed conversations.</summary>
    public int GetIndexedCount()
    {
        return _db.IsAvailable ? (int)_db.ExecuteScalar<long>("SELECT COUNT(DISTINCT ConversationId) FROM ConversationSearch") : 0;
    }

    /// <summary>The number of indexed messages.</summary>
    public int GetTotalMessagesIndexed()
    {
        return _db.IsAvailable ? (int)_db.ExecuteScalar<long>("SELECT COUNT(*) FROM ConversationSearch") : 0;
    }

    public void Dispose()
    {
        _disposed = true;
    }
}

/// <summary>
/// A window of conversation activity. A conversation matches when the span from its creation
/// to its last update overlaps the window, both ends inclusive to the day.
/// </summary>
/// <remarks>
/// Conversation-level on purpose. The message tables are rewritten on every save, so a message
/// row's timestamp is the last save, not when it was sent; the index's own timestamp is when
/// the row was indexed. The conversation's first and last save are the only dates that mean
/// what a reader expects.
/// </remarks>
public readonly record struct SearchDateRange(DateTimeOffset? From, DateTimeOffset? To)
{
    public bool IsEmpty => From == null && To == null;

    /// <summary>Start of the first day, as the ISO prefix the stored ISO-8601 strings compare against.</summary>
    public string? FromKey => From?.UtcDateTime.Date.ToString("yyyy-MM-ddTHH:mm:ss");

    /// <summary>Start of the day after the last day - an exclusive upper bound.</summary>
    public string? ToExclusiveKey => To?.UtcDateTime.Date.AddDays(1).ToString("yyyy-MM-ddTHH:mm:ss");
}

/// <summary>Result from a conversation search.</summary>
public class ConversationSearchResult
{
    public string ConversationId { get; set; } = string.Empty;
    public string SessionId { get; set; } = string.Empty;
    public string ThreadId { get; set; } = string.Empty;
    public string? ProjectId { get; set; }
    public string Role { get; set; } = string.Empty;
    public string Content { get; set; } = string.Empty;
    public string Timestamp { get; set; } = string.Empty;
    public double Rank { get; set; }
    public string Snippet { get; set; } = string.Empty;

    /// <summary>"chat" or "dream" — which pool this result came from.</summary>
    public string Source { get; set; } = "chat";

    /// <summary>
    /// How this result was found: <see cref="MatchKinds.Lexical"/>, <see cref="MatchKinds.Semantic"/>
    /// or <see cref="MatchKinds.Both"/>. A reader needs it to weigh a result whose text does not
    /// contain a single word of the query.
    /// </summary>
    public string MatchKind { get; set; } = MatchKinds.Lexical;

    /// <summary>
    /// Cosine distance from the query, 0 to 2, lower being nearer. Null unless this was found by
    /// meaning. Kept separate from <see cref="Rank"/> rather than folded into it: bm25 is
    /// unbounded and negative and a distance is neither, so one field holding both would be a
    /// number nothing could interpret.
    /// </summary>
    public double? Distance { get; set; }

    /// <summary>
    /// <see cref="SearchOrigins.Raw"/> when the matched text is a message as it was sent,
    /// <see cref="SearchOrigins.Compressed"/> when it is a summary compaction wrote. Both are
    /// indexed, so the same fact can come back twice; the reader decides which to trust, and
    /// is told which is which rather than being handed a blend.
    /// </summary>
    public string Origin { get; set; } = SearchOrigins.Raw;

    /// <summary>The stored row this came from, and the matched message's id, when the index has them.</summary>
    public string StateId { get; set; } = string.Empty;

    public string? MessageId { get; set; }

    /// <summary>
    /// The reader's own tag on this memory, 0 (irrelevant) to 10 (important), or null when
    /// untagged. Set from the search dialog; see <see cref="ConfigDatabase.SetMemoryTag"/>.
    /// </summary>
    public int? Importance { get; set; }
}

/// <summary>
/// A single matching message from <see cref="ConversationSearchService.SearchMessages"/>. Not
/// collapsed per conversation, so one conversation can produce many hits.
/// </summary>
public class ConversationMessageHit
{
    /// <summary>The indexed conversation key, which is the conversation's SessionId.</summary>
    public string ConversationId { get; set; } = string.Empty;

    public string? ProjectId { get; set; }

    public string Role { get; set; } = string.Empty;

    /// <summary>When the message was indexed, not when it was sent.</summary>
    public string Timestamp { get; set; } = string.Empty;

    /// <summary>BM25 rank; lower sorts first. Zero for literal-substring fallback hits.</summary>
    public double Rank { get; set; }

    /// <summary>Snippet with matched terms wrapped in the highlight markers.</summary>
    public string MarkedSnippet { get; set; } = string.Empty;

    /// <summary>How this hit was found; see <see cref="MatchKinds"/>.</summary>
    public string MatchKind { get; set; } = MatchKinds.Lexical;

    /// <summary>Cosine distance, when this was found by meaning. Null otherwise.</summary>
    public double? Distance { get; set; }

    /// <summary>
    /// The stored row this came from. A SessionId can resolve to more than one stored row, so
    /// knowing which removes a guess when the transcript is opened.
    /// </summary>
    public string StateId { get; set; } = string.Empty;

    /// <summary>
    /// The matched message's position in the transcript, or -1 when unknown.
    /// </summary>
    /// <remarks>
    /// A keyword hit is located in the transcript by finding its snippet as a verbatim substring,
    /// which works because FTS5 returns a contiguous run of the original text. A hit found by
    /// meaning has no such anchor - the query's words need not appear in it at all - so it
    /// carries its position instead, which is exact rather than inferred.
    /// </remarks>
    public int MessageIndex { get; set; } = -1;

    /// <summary>The matched message's id, when the index has one.</summary>
    public string? MessageId { get; set; }

    /// <summary>Whether the matched text is a message as sent or a compaction summary; see <see cref="SearchOrigins"/>.</summary>
    public string Origin { get; set; } = SearchOrigins.Raw;

    /// <summary>The reader's tag on this memory, 0 (irrelevant) to 10 (important); null when untagged.</summary>
    public int? Importance { get; set; }
}

/// <summary>Which conversation pool the remember/Search call should query.</summary>
public enum SearchSource
{
    Chats = 0,
    Dreams = 1,
    Both = 2
}
