using System.IO;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services.Search;

/// <summary>
/// The derived search database, <c>search.db</c>, beside <c>config.db</c>.
/// </summary>
/// <remarks>
/// <para>
/// Everything in here is rebuilt from <c>config.db</c>: the FTS5 tables over transcripts and
/// the progress rows that say which stored content each index row was built from. Deleting the file loses nothing. It exists as its own file because SQLite serialises
/// writers per file, and indexing - a delete and re-insert of a whole transcript - used to sit in
/// the same file as the inbox, the calendar and the grants, so a host re-indexing a long chat
/// could stall a calendar write for as long as the busy timeout allowed. See SEARCH.md.
/// </para>
/// <para>
/// One process writes this file: the one that holds the indexer lock (<see cref="SearchIndexer"/>).
/// Every other process opens it for reads only, enforced here by <see cref="Writable"/> - a
/// write from a process that was not told it may write is a bug, and throws. The source
/// database is attached read-only as <c>src</c> on every connection so a search can still
/// consult source rows (which chats are excluded from dreaming, for instance) in one statement.
/// </para>
/// <para>
/// This file holds every indexed transcript's text in its FTS tables. It is conversation data
/// and is handled as <c>config.db</c> is.
/// </para>
/// </remarks>
public sealed class SearchDatabase : IDisposable
{
    /// <summary>The alias the source database is attached under on every connection.</summary>
    public const string SourceAlias = "src";

    private static readonly object InstanceLock = new();
    private static SearchDatabase? _instance;
    private static bool _instanceWritable;

    private readonly string _connectionString;
    private readonly string _sourcePath;
    private bool _disposed;

    /// <summary>
    /// The shared instance for this process. Read-only unless <see cref="ConfigureWritable"/> was
    /// called first, which only the process that will own the indexer does.
    /// </summary>
    public static SearchDatabase Instance
    {
        get
        {
            if (_instance != null)
            {
                return _instance;
            }

            lock (InstanceLock)
            {
                _instance ??= new SearchDatabase(
                    PathFor(ConfigDatabase.Instance.DatabasePath),
                    ConfigDatabase.Instance.DatabasePath,
                    _instanceWritable);
                return _instance;
            }
        }
    }

    /// <summary>
    /// Says that this process is the one that writes the index. Must be called before the first
    /// use of <see cref="Instance"/>; afterwards it is too late and an exception says so, because
    /// a process that silently kept a read-only handle would index nothing and report nothing.
    /// </summary>
    public static void ConfigureWritable()
    {
        lock (InstanceLock)
        {
            if (_instance != null && !_instance.Writable)
            {
                // Something searched before the master declared itself. A read-only handle has
                // written nothing, so it is safe to replace it with a writable one now.
                AutomatonDebug.LogWarning("SearchDatabase.ConfigureWritable: replacing a read-only instance opened before the indexer declared itself");
                _instance.Dispose();
                _instance = null;
            }

            _instanceWritable = true;
        }
    }

    /// <summary>Where the search database lives for a given config database: same folder, derived name.</summary>
    public static string PathFor(string configDatabasePath)
    {
        if (string.IsNullOrWhiteSpace(configDatabasePath))
        {
            throw new ArgumentException("A config database path is required.", nameof(configDatabasePath));
        }

        var directory = Path.GetDirectoryName(Path.GetFullPath(configDatabasePath)) ?? string.Empty;
        var stem = Path.GetFileNameWithoutExtension(configDatabasePath);
        var name = string.Equals(stem, "config", StringComparison.OrdinalIgnoreCase) ? "search.db" : stem + ".search.db";
        return Path.Combine(directory, name);
    }

    /// <summary>
    /// Opens (creating, when writable) the search database at <paramref name="databasePath"/>
    /// with <paramref name="sourceDatabasePath"/> attached as <c>src</c>.
    /// </summary>
    public SearchDatabase(string databasePath, string sourceDatabasePath, bool writable)
    {
        if (string.IsNullOrWhiteSpace(databasePath))
        {
            throw new ArgumentException("A search database path is required.", nameof(databasePath));
        }

        if (string.IsNullOrWhiteSpace(sourceDatabasePath))
        {
            throw new ArgumentException("A source database path is required.", nameof(sourceDatabasePath));
        }

        AutomatonDebug.Log($"SearchDatabase: Constructor entry - path={databasePath}, writable={writable}");
        DatabasePath = Path.GetFullPath(databasePath);
        _sourcePath = Path.GetFullPath(sourceDatabasePath);
        Writable = writable;

        _connectionString = new SqliteConnectionStringBuilder
        {
            DataSource = DatabasePath,
            Mode = writable ? SqliteOpenMode.ReadWriteCreate : SqliteOpenMode.ReadWrite,

            // Private, not shared: shared-cache connections in one process take table locks that
            // busy_timeout does not wait on, so a reader on the reconciliation thread could fail
            // the worker's write with "database table is locked". WAL gives the concurrency.
            Cache = SqliteCacheMode.Private,
            Pooling = true,
            DefaultTimeout = 30
        }.ToString();

        if (!writable && !File.Exists(DatabasePath))
        {
            // The master has not built it yet. Searches answer empty rather than this process
            // creating an empty file the master would then find and trust.
            IsAvailable = false;
            AutomatonDebug.LogWarning($"SearchDatabase: {DatabasePath} does not exist and this process may not create it; search is unavailable here");
            return;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(DatabasePath)!);

        using var connection = CreateOpenConnection();
        if (writable)
        {
            using var wal = connection.CreateCommand();
            wal.CommandText = "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;";
            wal.ExecuteNonQuery();
            CreateTables(connection);
        }

        IsAvailable = true;
        AutomatonDebug.Log($"SearchDatabase: Constructor exit - path={DatabasePath}, writable={writable}");
    }

    public string DatabasePath { get; }

    /// <summary>The lock file the indexer takes to become the one writer.</summary>
    public string LockPath => DatabasePath + ".lock";

    /// <summary>Whether this process may write. False in conversation hosts.</summary>
    public bool Writable { get; }

    /// <summary>False when the file does not exist and this process may not create it.</summary>
    public bool IsAvailable { get; }

    private static void CreateTables(SqliteConnection connection)
    {
        AutomatonDebug.Log("SearchDatabase.CreateTables: Entry");
        UpgradeFtsTablesIfNeeded(connection);
        using var command = connection.CreateCommand();

        // No stemmer. Porter folded "statue" and "status" to one token, and a search for a
        // golden statue returned Shopify status fields; queries stand in for stemming with a
        // prefix on longer words instead (ConversationSearchService.FtsTerm).
        // FTS5 keeps its own shadow tables; no explicit indexes on the virtual tables.
        // StateId is the source row's key (ConversationState.Id / DreamConversationState.Id);
        // ConversationId is the SessionId the rest of the app has always keyed hits by.
        // MessageId and MessageIndex locate the row in the transcript the dialog opens, and
        // Origin says whether the text is a message as sent ('raw') or a compaction summary
        // ('compressed'). All three are UNINDEXED: a search for the word "raw" must not match
        // every row in the index.
        command.CommandText = @"
            CREATE VIRTUAL TABLE IF NOT EXISTS ConversationSearch USING fts5(
                StateId UNINDEXED,
                ConversationId,
                SessionId,
                ThreadId,
                ProjectId,
                Role,
                Content,
                Timestamp,
                MessageId UNINDEXED,
                MessageIndex UNINDEXED,
                Origin UNINDEXED,
                tokenize='unicode61 remove_diacritics 2'
            );

            CREATE VIRTUAL TABLE IF NOT EXISTS DreamConversationSearch USING fts5(
                StateId UNINDEXED,
                ConversationId,
                SessionId,
                ThreadId,
                ProjectId,
                Role,
                Content,
                Timestamp,
                MessageId UNINDEXED,
                MessageIndex UNINDEXED,
                Origin UNINDEXED,
                tokenize='unicode61 remove_diacritics 2'
            );

            -- One row per source row. FtsRevision is a content fingerprint of what was
            -- actually indexed, so a change is detected however the source's revision number
            -- was assigned. The three Source* columns are the cheap pre-filter that decides
            -- whether the stored JSON is worth re-hashing at all.
            CREATE TABLE IF NOT EXISTS IndexProgress (
                Source           TEXT NOT NULL,
                Id               TEXT NOT NULL,
                FtsRevision      TEXT,
                SourceUpdatedAt  TEXT,
                SourceRevision   INTEGER,
                SourceLength     INTEGER,
                IndexedAt        TEXT,
                PRIMARY KEY (Source, Id)
            );

            -- Vector generations. A generation is one embedding contract's worth of vectors:
            -- change the model, the dimension, the instruction or any chunking constant and the
            -- fingerprint moves, a new generation is built beside the live one, and the swap is
            -- one transaction. State is 'building' then 'active'; queries only ever read the
            -- active one, so a half-built generation is never visible.
            CREATE TABLE IF NOT EXISTS IndexGenerations (
                Generation   INTEGER PRIMARY KEY,
                Fingerprint  TEXT NOT NULL,
                Dims         INTEGER NOT NULL,
                State        TEXT NOT NULL,
                CreatedAt    TEXT
            );

            -- Vector progress, deliberately its own table rather than columns on IndexProgress:
            -- the generation is part of the key, and a fingerprint change has to invalidate
            -- vectors without touching the FTS rows. VecRevision holds the FtsRevision the
            -- chunks were cut from, which is how the vector worker finds its work without ever
            -- reading config.db.
            CREATE TABLE IF NOT EXISTS VectorProgress (
                Source         TEXT NOT NULL,
                Id             TEXT NOT NULL,
                Generation     INTEGER NOT NULL,
                VecRevision    TEXT,
                VecFingerprint TEXT,
                ChunkCount     INTEGER,
                IndexedAt      TEXT,
                PRIMARY KEY (Source, Id, Generation)
            );";

        command.ExecuteNonQuery();
        AutomatonDebug.Log("SearchDatabase.CreateTables: Exit");
    }

    /// <summary>
    /// A virtual table cannot gain a column, so an index built before <c>Origin</c> existed is
    /// dropped and its progress rows cleared; the first reconciliation then rebuilds it row by
    /// row, exactly as it would after the file was deleted. Vector progress is cleared with it,
    /// because chunk positions are counted over a transcript that now includes summaries.
    /// </summary>
    private static void UpgradeFtsTablesIfNeeded(SqliteConnection connection)
    {
        AutomatonDebug.Log("SearchDatabase.UpgradeFtsTablesIfNeeded: Entry");
        if (connection == null)
        {
            AutomatonDebug.LogWarning("SearchDatabase.UpgradeFtsTablesIfNeeded: Exit - no connection");
            return;
        }

        var hasTable = false;
        var hasOrigin = false;
        using (var info = connection.CreateCommand())
        {
            info.CommandText = "PRAGMA table_info(ConversationSearch);";
            using var reader = info.ExecuteReader();
            while (reader.Read())
            {
                hasTable = true;
                if (string.Equals(reader.GetString(1), "Origin", StringComparison.OrdinalIgnoreCase))
                {
                    hasOrigin = true;
                }
            }
        }

        // The tokenizer is part of the table's own definition; a table built with the stemmer
        // has "porter" in it and has to be rebuilt without. Chunks are cut from content, not
        // tokens, so the vectors are untouched by this one.
        var stemmed = false;
        if (hasTable)
        {
            using var definition = connection.CreateCommand();
            definition.CommandText = "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'ConversationSearch';";
            var sql = definition.ExecuteScalar() as string ?? string.Empty;
            stemmed = sql.Contains("porter", StringComparison.OrdinalIgnoreCase);
        }

        if (!hasTable || (hasOrigin && !stemmed))
        {
            AutomatonDebug.Log($"SearchDatabase.UpgradeFtsTablesIfNeeded: Exit - nothing to do (table={hasTable}, origin={hasOrigin}, stemmed={stemmed})");
            return;
        }

        var clearVectors = !hasOrigin;

        // This runs before the CREATE batch, so a file from before VectorProgress existed
        // has no such table yet; deleting from it would throw out of the constructor after
        // the FTS tables were already gone. Clear only what is there.
        var present = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var tables = connection.CreateCommand())
        {
            tables.CommandText = "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN ('IndexProgress', 'VectorProgress');";
            using var reader = tables.ExecuteReader();
            while (reader.Read())
            {
                present.Add(reader.GetString(0));
            }
        }

        // Progress rows are marked stale, not deleted, exactly as RequestRebuild does: the
        // reconciliation re-indexes a stale row, and the vector worker keeps a stale row's
        // vectors. Deleting the rows made every vector an orphan. Vector progress goes only
        // when the chunk shape itself changed (the Origin upgrade moved every position).
        using var upgrade = connection.CreateCommand();
        upgrade.CommandText = "DROP TABLE IF EXISTS ConversationSearch; DROP TABLE IF EXISTS DreamConversationSearch;"
            + (present.Contains("IndexProgress") ? " UPDATE IndexProgress SET FtsRevision = NULL, SourceUpdatedAt = NULL, SourceRevision = NULL, SourceLength = NULL;" : string.Empty)
            + (clearVectors && present.Contains("VectorProgress") ? " DELETE FROM VectorProgress;" : string.Empty);
        upgrade.ExecuteNonQuery();
        AutomatonDebug.Log($"SearchDatabase.UpgradeFtsTablesIfNeeded: Exit - dropped the FTS tables (origin={hasOrigin}, stemmed={stemmed}) and cleared progress ({string.Join(", ", present.Where(p => clearVectors || p != "VectorProgress"))}); reconciliation will rebuild");
    }

    private SqliteConnection CreateOpenConnection()
    {
        var connection = new SqliteConnection(_connectionString);
        connection.Open();

        using var pragma = connection.CreateCommand();
        pragma.CommandText = "PRAGMA busy_timeout=30000;";
        pragma.ExecuteNonQuery();


        // The source, for predicates that need it. Attached rather than copied: which chats
        // are excluded from dreaming is a fact about the source, and copying it into the index
        // would let the two disagree. Only ever read through this alias.
        if (File.Exists(_sourcePath) && !IsAttached(connection))
        {
            using var attach = connection.CreateCommand();
            attach.CommandText = $"ATTACH DATABASE @path AS {SourceAlias};";
            attach.Parameters.AddWithValue("@path", _sourcePath);
            attach.ExecuteNonQuery();
        }

        return connection;
    }

    /// <summary>
    /// Pooled connections come back with the source still attached; attaching twice is an error.
    /// </summary>
    private static bool IsAttached(SqliteConnection connection)
    {
        using var list = connection.CreateCommand();
        list.CommandText = "PRAGMA database_list;";
        using var reader = list.ExecuteReader();
        while (reader.Read())
        {
            if (string.Equals(reader.GetString(1), SourceAlias, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }

        return false;
    }

    private void ThrowIfUnavailable()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(SearchDatabase));
        }

        if (!IsAvailable)
        {
            throw new InvalidOperationException("The search database is not available in this process.");
        }
    }

    private void ThrowIfReadOnly()
    {
        if (!Writable)
        {
            throw new InvalidOperationException("This process may only read the search database; the indexer lives in the master.");
        }
    }

    public List<Dictionary<string, object?>> Query(string sql, params SqliteParameter[] parameters)
    {
        ThrowIfUnavailable();
        using var connection = CreateOpenConnection();
        return ExecuteQuery(connection, null, sql, parameters);
    }

    public int Execute(string sql, params SqliteParameter[] parameters)
    {
        ThrowIfUnavailable();
        ThrowIfReadOnly();
        using var connection = CreateOpenConnection();
        using var command = CreateCommand(connection, null, sql, parameters);
        return command.ExecuteNonQuery();
    }

    public T? ExecuteScalar<T>(string sql, params SqliteParameter[] parameters)
    {
        ThrowIfUnavailable();
        using var connection = CreateOpenConnection();
        using var command = CreateCommand(connection, null, sql, parameters);
        var result = command.ExecuteScalar();
        if (result == null || result == DBNull.Value)
        {
            return default;
        }

        return (T)Convert.ChangeType(result, typeof(T));
    }

    /// <summary>Runs <paramref name="work"/> inside one transaction; rows and progress commit together or not at all.</summary>
    public void ExecuteInTransaction(Action<SearchTransaction> work)
    {
        if (work == null)
        {
            throw new ArgumentNullException(nameof(work));
        }

        ThrowIfUnavailable();
        ThrowIfReadOnly();
        using var connection = CreateOpenConnection();
        using var transaction = connection.BeginTransaction();
        work(new SearchTransaction(connection, transaction));
        transaction.Commit();
    }

    private static SqliteCommand CreateCommand(SqliteConnection connection, SqliteTransaction? transaction, string sql, params SqliteParameter[] parameters)
    {
        var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = sql;
        foreach (var parameter in parameters)
        {
            command.Parameters.Add(parameter);
        }

        return command;
    }

    private static List<Dictionary<string, object?>> ExecuteQuery(SqliteConnection connection, SqliteTransaction? transaction, string sql, params SqliteParameter[] parameters)
    {
        var results = new List<Dictionary<string, object?>>();
        using var command = CreateCommand(connection, transaction, sql, parameters);
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            var row = new Dictionary<string, object?>();
            for (var i = 0; i < reader.FieldCount; i++)
            {
                row[reader.GetName(i)] = reader.IsDBNull(i) ? null : reader.GetValue(i);
            }

            results.Add(row);
        }

        return results;
    }

    /// <summary>The connection and transaction handed to <see cref="ExecuteInTransaction"/> work.</summary>
    public sealed class SearchTransaction
    {
        private readonly SqliteConnection _connection;
        private readonly SqliteTransaction _transaction;

        internal SearchTransaction(SqliteConnection connection, SqliteTransaction transaction)
        {
            _connection = connection;
            _transaction = transaction;
        }

        public int Execute(string sql, params SqliteParameter[] parameters)
        {
            using var command = CreateCommand(_connection, _transaction, sql, parameters);
            return command.ExecuteNonQuery();
        }

        public List<Dictionary<string, object?>> Query(string sql, params SqliteParameter[] parameters)
        {
            return ExecuteQuery(_connection, _transaction, sql, parameters);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        AutomatonDebug.Log($"SearchDatabase.Dispose: {DatabasePath}");
    }
}
