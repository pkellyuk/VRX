using System.IO;
using System.Text.Json;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services;

/// <summary>
/// Centralized SQLite database for all Automaton configuration and state.
/// Defaults to %USERPROFILE%\.automaton\config.db. Applications may select a
/// different filename before the singleton is first accessed.
/// </summary>
public sealed class ConfigDatabase : IDisposable
{
    private static readonly object _lock = new();
    private static ConfigDatabase? _instance;
    private static string _databaseFileName = "config.db";
    private static bool _durableCommitsRequired;
    private readonly string _connectionString;
    private readonly string _synchronousMode;
    private bool _disposed;

    /// <summary>
    /// Gets the singleton instance of ConfigDatabase.
    /// </summary>
    public static ConfigDatabase Instance
    {
        get
        {
            if (_instance == null)
            {
                lock (_lock)
                {
                    _instance ??= new ConfigDatabase();
                }
            }
            return _instance;
        }
    }

    /// <summary>
    /// Selects the database filename for this process. This must be called before
    /// <see cref="Instance"/> is first accessed. Only a filename is accepted so all
    /// Automaton databases remain inside <see cref="AutomatonPaths.HomeDirectory"/>.
    /// Applications which do not call this method continue to use <c>config.db</c>.
    /// </summary>
    public static void ConfigureDatabaseFileName(string fileName)
    {
        if (string.IsNullOrWhiteSpace(fileName))
        {
            throw new ArgumentException("A database filename is required.", nameof(fileName));
        }

        if (fileName.Contains('/') || fileName.Contains('\\') ||
            !string.Equals(fileName, Path.GetFileName(fileName), StringComparison.Ordinal) ||
            !string.Equals(Path.GetExtension(fileName), ".db", StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException("The database must be a .db filename without a directory path.", nameof(fileName));
        }

        lock (_lock)
        {
            if (_instance != null)
            {
                var requestedPath = Path.Combine(AutomatonPaths.HomeDirectory, fileName);
                if (string.Equals(_instance.DatabasePath, requestedPath, StringComparison.OrdinalIgnoreCase))
                {
                    return;
                }

                throw new InvalidOperationException("The configuration database has already been initialized.");
            }

            _databaseFileName = fileName;
        }
    }

    /// <summary>
    /// Gets the database file path.
    /// </summary>
    public string DatabasePath { get; }

    /// <summary>
    /// Requests FULL synchronization for this process's singleton. Call before
    /// first access to Instance. Other hosts retain NORMAL unless they opt in.
    /// An initialized database cannot silently change its connection policy.
    /// </summary>
    public static void RequireDurableCommits()
    {
        lock (_lock)
        {
            if (_instance != null && _instance._synchronousMode != "FULL")
                throw new InvalidOperationException("Durable commits must be selected before the configuration database is initialized.");
            _durableCommitsRequired = true;
        }
    }

    private ConfigDatabase() : this(Path.Combine(AutomatonPaths.HomeDirectory, _databaseFileName), _durableCommitsRequired) { }

    public ConfigDatabase(string databasePath) : this(databasePath, false) { }

    /// <summary>Creates an independent database with an explicit write-durability policy.</summary>
    public ConfigDatabase(string databasePath, bool durableCommits)
    {
        _synchronousMode = durableCommits ? "FULL" : "NORMAL";
        AutomatonDebug.Log("ConfigDatabase: Constructor entry");
        // Ensure directory exists
        DatabasePath = Path.GetFullPath(databasePath);
        Directory.CreateDirectory(Path.GetDirectoryName(DatabasePath)!);
        _connectionString = new SqliteConnectionStringBuilder
        {
            DataSource = DatabasePath,
            Mode = SqliteOpenMode.ReadWriteCreate,
            Cache = SqliteCacheMode.Shared,
            Pooling = true,
            DefaultTimeout = 30
        }.ToString();

        using var connection = CreateOpenConnection();

        // Enable WAL mode for better concurrency.
        using var walCommand = connection.CreateCommand();
        walCommand.CommandText = $"PRAGMA journal_mode=WAL; PRAGMA synchronous={_synchronousMode};";
        walCommand.ExecuteNonQuery();

        // Create tables
        CreateTables(connection);
        EnsureConversationSchema(connection);
        EnsureCalendarWatchColumns(connection);
        EnsureDirectorGoalColumns(connection);
        EnsureInputHistorySchema(connection);
        EnsureConversationAttachmentColumn(connection);
        EnsureFullTranscriptSchema(connection);
        EnsureGoalTrackingSchema(connection);
        DropLegacySearchTables(connection);
        AutomatonDebug.Log($"ConfigDatabase: Constructor exit - DatabasePath={DatabasePath}");
    }

    private SqliteConnection CreateOpenConnection()
    {
        var connection = new SqliteConnection(_connectionString);
        connection.Open();

        using var command = connection.CreateCommand();
        command.CommandText = $"PRAGMA foreign_keys=ON; PRAGMA synchronous={_synchronousMode}; PRAGMA busy_timeout=30000;";
        command.ExecuteNonQuery();

        return connection;
    }

    private static SqliteCommand CreateCommand(SqliteConnection connection, SqliteTransaction? transaction, string sql, params SqliteParameter[] parameters)
    {
        var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = sql;

        foreach (var param in parameters)
        {
            command.Parameters.Add(param);
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
            for (int i = 0; i < reader.FieldCount; i++)
            {
                row[reader.GetName(i)] = reader.IsDBNull(i) ? null : reader.GetValue(i);
            }

            results.Add(row);
        }

        return results;
    }

    private static int ExecuteNonQuery(SqliteConnection connection, SqliteTransaction? transaction, string sql, params SqliteParameter[] parameters)
    {
        using var command = CreateCommand(connection, transaction, sql, parameters);
        return command.ExecuteNonQuery();
    }

    private static T? ExecuteScalarInternal<T>(SqliteConnection connection, SqliteTransaction? transaction, string sql, params SqliteParameter[] parameters)
    {
        using var command = CreateCommand(connection, transaction, sql, parameters);
        var scalarResult = command.ExecuteScalar();
        if (scalarResult == null || scalarResult == DBNull.Value)
        {
            return default;
        }

        return (T)Convert.ChangeType(scalarResult, typeof(T));
    }

    /// <summary>
    /// The transaction every statement on this database joins while one is open on this
    /// async flow. Per instance, so a test's temporary database cannot see another's.
    /// </summary>
    private readonly AsyncLocal<AmbientTransaction?> _ambient = new();

    private sealed record AmbientTransaction(SqliteConnection Connection, SqliteTransaction Transaction);

    /// <summary>
    /// Runs <paramref name="action"/> with every statement it makes — directly, or through
    /// any method it calls — inside one transaction, so the work either lands whole or not
    /// at all.
    ///
    /// The ambient is what makes that true of the whole call tree rather than only of
    /// statements written here. Saving a conversation writes the JSON blob and then the
    /// normalized projection through a web of helpers, each of which used to open its own
    /// connection and commit alone; when one of them threw, the blob had already moved on
    /// and the two disagreed permanently. That is how a live 168-message chat came to be
    /// stored as one row and load as empty.
    ///
    /// The scope must be synchronous. A transaction holds SQLite's write lock, so awaiting
    /// inside one would let another flow queue behind it.
    /// </summary>
    public void ExecuteInTransaction(Action<SqliteConnection, SqliteTransaction> action)
    {
        // Already inside one: join it. Committing a nested scope would end the outer one
        // early and leave the rest of its work outside any transaction.
        if (_ambient.Value is { } joined)
        {
            action(joined.Connection, joined.Transaction);
            return;
        }

        using var connection = CreateOpenConnection();
        using var transaction = connection.BeginTransaction();

        _ambient.Value = new AmbientTransaction(connection, transaction);
        try
        {
            action(connection, transaction);
            transaction.Commit();
        }
        finally
        {
            _ambient.Value = null;
        }
    }

    /// <summary>
    /// Runs <paramref name="sql"/> against the given open connection inside the given
    /// transaction. Used by services that need to chain multiple statements atomically
    /// on the same connection — e.g. the FK-sensitive parent/child rename in
    /// ConversationPersistenceService.ArchiveAndClearState.
    /// </summary>
    internal static int ExecuteOnConnection(SqliteConnection connection, SqliteTransaction transaction, string sql, params SqliteParameter[] parameters)
    {
        return ExecuteNonQuery(connection, transaction, sql, parameters);
    }

    internal static List<Dictionary<string, object?>> QueryOnConnection(SqliteConnection connection, SqliteTransaction transaction, string sql, params SqliteParameter[] parameters)
    {
        return ExecuteQuery(connection, transaction, sql, parameters);
    }

    private static void CreateTables(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText = @"
            CREATE TABLE IF NOT EXISTS Config (
                Key TEXT PRIMARY KEY,
                Value TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS Settings (
                Key TEXT PRIMARY KEY,
                Value TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS Goals (
                Id TEXT PRIMARY KEY,
                ProjectId TEXT NOT NULL,
                Description TEXT NOT NULL,
                WorkingFolder TEXT,
                DownloadedDocs TEXT,
                SuccessCriteria TEXT,
                Milestones TEXT,
                Plan TEXT,
                Status INTEGER NOT NULL,
                CreatedAt TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_goals_projectid ON Goals(ProjectId);
            CREATE INDEX IF NOT EXISTS idx_goals_status ON Goals(Status);

            CREATE TABLE IF NOT EXISTS Projects (
                Id TEXT PRIMARY KEY,
                Name TEXT NOT NULL,
                Path TEXT NOT NULL,
                CreatedAt TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS ConversationState (
                Id TEXT PRIMARY KEY,
                ProjectId TEXT,
                ThreadId TEXT,
                State TEXT NOT NULL,
                SessionId TEXT,
                Agent TEXT,
                Directory TEXT,
                Worktree TEXT,
                UseRolesEnabled INTEGER DEFAULT 0,
                ShowThinkingEnabled INTEGER DEFAULT 1,
                ShowToolCallsEnabled INTEGER DEFAULT 1,
                CollapseThinkingEnabled INTEGER DEFAULT 1,
                CollapseToolCallsEnabled INTEGER DEFAULT 1,
                SelectedProviderKey TEXT,
                SelectedModelId TEXT,
                ReasoningEffort TEXT,
                Verbosity TEXT,
                CompressionSettings TEXT,
                Revision INTEGER DEFAULT 0,
                CreatedAt TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL
            );

            -- Global search correlates every FTS hit back to this table to honour
            -- ExcludeFromDream. Without this index that subquery scans the whole
            -- table once per matched row, reading every State blob each time:
            -- measured at ~14.5s for a single search against a 472-row database
            -- whose blobs average 142KB. With it, the planner does an index
            -- lookup instead - SCAN cs becomes SEARCH cs USING INDEX.
            CREATE INDEX IF NOT EXISTS idx_conversationstate_sessionid ON ConversationState(SessionId);

            CREATE TABLE IF NOT EXISTS ConversationMessages (
                Id TEXT PRIMARY KEY,
                StateId TEXT NOT NULL,
                MessageIndex INTEGER NOT NULL,
                MessageId TEXT NOT NULL,
                Role TEXT NOT NULL,
                Content TEXT,
                Parts TEXT,
                CreatedAt TEXT NOT NULL,
                FOREIGN KEY (StateId) REFERENCES ConversationState(Id) ON DELETE CASCADE
            );

            CREATE INDEX IF NOT EXISTS idx_conversation_messages_stateid ON ConversationMessages(StateId);
            CREATE INDEX IF NOT EXISTS idx_conversation_messages_index ON ConversationMessages(StateId, MessageIndex);
            CREATE UNIQUE INDEX IF NOT EXISTS idx_conversation_messages_unique ON ConversationMessages(StateId, MessageId);

            CREATE TABLE IF NOT EXISTS InputHistory (
                Id TEXT PRIMARY KEY,
                ConversationKey TEXT NOT NULL DEFAULT '',
                Input TEXT NOT NULL,
                Sequence INTEGER NOT NULL DEFAULT 0,
                CreatedAt TEXT NOT NULL
            );

            -- The InputHistory(ConversationKey, Sequence) index is created by
            -- EnsureInputHistorySchema instead: on a database that predates those two
            -- columns the CREATE TABLE above is a no-op, so an index over those
            -- columns would fail with a missing-column error before the migration runs.

            CREATE TABLE IF NOT EXISTS ProjectFiles (
                Id TEXT PRIMARY KEY,
                ProjectId TEXT NOT NULL,
                Name TEXT NOT NULL,
                Content TEXT,
                Description TEXT,
                CreatedAt TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_projectfiles_projectid ON ProjectFiles(ProjectId);

            CREATE TABLE IF NOT EXISTS ProjectTools (
                Id TEXT PRIMARY KEY,
                ProjectId TEXT NOT NULL,
                Name TEXT NOT NULL,
                Description TEXT,
                ToolType TEXT,
                Configuration TEXT,
                IsEnabled INTEGER NOT NULL DEFAULT 1,
                CreatedAt TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_projecttools_projectid ON ProjectTools(ProjectId);

            CREATE TABLE IF NOT EXISTS Threads (
                Id TEXT PRIMARY KEY,
                ProjectId TEXT NOT NULL,
                Name TEXT NOT NULL,
                StateFilePath TEXT NOT NULL,
                IsAutomationEnabled INTEGER NOT NULL DEFAULT 0,
                IsDreamEnabled INTEGER NOT NULL DEFAULT 0,
                CreatedAt TEXT NOT NULL,
                LastModifiedAt TEXT NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_threads_projectid ON Threads(ProjectId);

            CREATE TABLE IF NOT EXISTS CalendarTasks (
                Id          TEXT PRIMARY KEY,
                TaskType    TEXT NOT NULL,
                Title       TEXT NOT NULL,
                Description TEXT,
                DueDate     TEXT NOT NULL,
                DueTime     TEXT,
                Recurrence  TEXT,
                EndAfter    INTEGER,
                EndDate     TEXT,
                Tags        TEXT,
                Status      TEXT NOT NULL DEFAULT 'pending',
                CompletedAt TEXT,
                CreatedAt   TEXT NOT NULL,
                UpdatedAt   TEXT NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_cal_tasks_status  ON CalendarTasks(Status);
            CREATE INDEX IF NOT EXISTS idx_cal_tasks_duedate ON CalendarTasks(DueDate);

            CREATE TABLE IF NOT EXISTS Inbox (
                Id              TEXT PRIMARY KEY,
                SourceSessionId TEXT,
                SourceTaskId    TEXT,
                Title           TEXT,
                Message         TEXT NOT NULL,
                Options         TEXT,
                Status          TEXT NOT NULL DEFAULT 'Unread',
                CreatedAt       TEXT NOT NULL,
                ReadAt          TEXT
            );

            CREATE INDEX IF NOT EXISTS idx_inbox_status  ON Inbox(Status);
            CREATE INDEX IF NOT EXISTS idx_inbox_created ON Inbox(CreatedAt);

            -- Browser WebUI sessions. TokenHash is a SHA-256 of the cookie value,
            -- never the value itself: a stolen config.db must not yield a working
            -- cookie. The row is the whole grant - deleting it revokes the browser.
            CREATE TABLE IF NOT EXISTS WebSessions (
                TokenHash    TEXT PRIMARY KEY,
                Label        TEXT NOT NULL,
                RemoteHost   TEXT,
                CreatedAt    TEXT NOT NULL,
                ExpiresAt    TEXT NOT NULL,
                LastSeenAt   TEXT
            );

            CREATE INDEX IF NOT EXISTS idx_websessions_expires ON WebSessions(ExpiresAt);

            -- Guest invites into one chat (GUESTS.md). Like WebSessions, only SHA-256
            -- hashes of the link token and the guest's cookie are kept. Times are UTC
            -- round-trip strings so they compare as text inside the claim UPDATE.
            CREATE TABLE IF NOT EXISTS GuestInvites (
                Id              TEXT PRIMARY KEY,
                StateId         TEXT NOT NULL,
                ChatName        TEXT NOT NULL,
                GuestName       TEXT NOT NULL,
                TokenHash       TEXT NOT NULL UNIQUE,
                SessionHash     TEXT UNIQUE,
                ShowHistory     INTEGER NOT NULL DEFAULT 0,
                HistoryAnchorId TEXT,
                CreatedAt       TEXT NOT NULL,
                ExpiresAt       TEXT NOT NULL,
                ClaimedAt       TEXT,
                ClaimedFrom     TEXT,
                RevokedAt       TEXT,
                LastSeenAt      TEXT
            );

            CREATE INDEX IF NOT EXISTS idx_guestinvites_state ON GuestInvites(StateId);

            -- Group chat channels (MULTICHAT.md §5). A channel is not a conversation: it has
            -- many authors. Each member's work chat is an ordinary conversation, named by
            -- WorkStateId. Busy is never stored - it is a running assignment.
            CREATE TABLE IF NOT EXISTS GroupChats (
                Id          TEXT PRIMARY KEY,
                Name        TEXT NOT NULL,
                Directory   TEXT NOT NULL,
                RoundCap    INTEGER NOT NULL DEFAULT 3,
                CreatedUtc  TEXT NOT NULL,
                UpdatedUtc  TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS GroupMembers (
                Id              TEXT PRIMARY KEY,
                GroupId         TEXT NOT NULL,
                Name            TEXT NOT NULL,
                Kind            TEXT NOT NULL,
                ProviderKey     TEXT,
                ModelId         TEXT,
                ReasoningEffort TEXT,
                Mode            TEXT NOT NULL DEFAULT 'chat',
                Position        INTEGER NOT NULL DEFAULT 0,
                WorkStateId     TEXT,
                AgentTokenHash  TEXT UNIQUE,
                LastSeenUtc     TEXT
            );

            CREATE INDEX IF NOT EXISTS idx_groupmembers_group ON GroupMembers(GroupId);

            -- Seq is the per-group cursor that the page and MCP waits page on.
            CREATE TABLE IF NOT EXISTS GroupMessages (
                Id           TEXT PRIMARY KEY,
                GroupId      TEXT NOT NULL,
                Seq          INTEGER NOT NULL,
                AuthorId     TEXT NOT NULL,
                Kind         TEXT NOT NULL,
                AssignmentId TEXT,
                Text         TEXT NOT NULL,
                Detail       TEXT,
                CreatedUtc   TEXT NOT NULL
            );

            CREATE UNIQUE INDEX IF NOT EXISTS idx_groupmessages_seq ON GroupMessages(GroupId, Seq);

            CREATE TABLE IF NOT EXISTS GroupAssignments (
                Id          TEXT PRIMARY KEY,
                GroupId     TEXT NOT NULL,
                MemberId    TEXT NOT NULL,
                MessageId   TEXT NOT NULL,
                Text        TEXT NOT NULL,
                Status      TEXT NOT NULL,
                StartedUtc  TEXT NOT NULL,
                EndedUtc    TEXT,
                Outcome     TEXT
            );

            CREATE INDEX IF NOT EXISTS idx_groupassignments_group ON GroupAssignments(GroupId, Status);

            CREATE TABLE IF NOT EXISTS DirectorGoals (
                Id               TEXT PRIMARY KEY,
                Title            TEXT NOT NULL,
                Detail           TEXT,
                WorkingDirectory TEXT NOT NULL,
                ProjectId        TEXT,
                OriginatingThreadId TEXT,
                OriginatingSessionKey TEXT,
                Position         INTEGER NOT NULL DEFAULT 0,
                Status           TEXT NOT NULL DEFAULT 'Queued',
                WorkerStateId    TEXT,
                LastStatus       TEXT,
                Outcome          TEXT,
                BlockedQuestion  TEXT,
                PendingAnswer    TEXT,
                CreatedAt        TEXT NOT NULL,
                StartedAt        TEXT,
                FinishedAt       TEXT
            );

            CREATE INDEX IF NOT EXISTS idx_director_goals_status   ON DirectorGoals(Status);
            CREATE INDEX IF NOT EXISTS idx_director_goals_position ON DirectorGoals(Position);

            CREATE TABLE IF NOT EXISTS CalendarTaskOccurrences (
                TaskId         TEXT NOT NULL,
                OccurrenceDate TEXT NOT NULL,
                Status         TEXT NOT NULL DEFAULT 'pending',
                CompletedAt    TEXT,
                PRIMARY KEY (TaskId, OccurrenceDate),
                FOREIGN KEY (TaskId) REFERENCES CalendarTasks(Id) ON DELETE CASCADE
            );
        ";
        command.ExecuteNonQuery();
    }

    private static void EnsureConversationSchema(SqliteConnection connection)
    {
        var existingColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var command = connection.CreateCommand())
        {
            command.CommandText = "PRAGMA table_info(ConversationState);";
            using var reader = command.ExecuteReader();
            while (reader.Read())
            {
                existingColumns.Add(reader.GetString(1));
            }
        }

        var columnsToAdd = new (string Name, string Definition)[]
        {
            ("SessionId", "TEXT"),
            ("Agent", "TEXT"),
            ("Directory", "TEXT"),
            ("Worktree", "TEXT"),
            ("UseRolesEnabled", "INTEGER DEFAULT 0"),
            ("ShowThinkingEnabled", "INTEGER DEFAULT 1"),
            ("ShowToolCallsEnabled", "INTEGER DEFAULT 1"),
            ("CollapseThinkingEnabled", "INTEGER DEFAULT 1"),
            ("CollapseToolCallsEnabled", "INTEGER DEFAULT 1"),
            ("SelectedProviderKey", "TEXT"),
            ("SelectedModelId", "TEXT"),
            // Per-chat thinking level. Null on every pre-existing row, which reads as
            // "leave the provider's configured effort alone".
            ("ReasoningEffort", "TEXT"),
            // Per-chat reply length (terse / verbose). Null on every pre-existing row, which
            // reads as normal.
            ("Verbosity", "TEXT"),
            ("CompressionSettings", "TEXT"),
            ("Revision", "INTEGER DEFAULT 0"),
            // Archive flags: when the user clears or deletes a chat, the row is renamed
            // to a fresh GUID and marked IsArchived=1 so content is preserved in the DB
            // and remains searchable via remember/FTS. See ArchiveAndClearState.
            ("IsArchived", "INTEGER NOT NULL DEFAULT 0"),
            ("ArchivedAt", "TEXT"),
            ("ArchivedReason", "TEXT"),
            ("ArchivedFromId", "TEXT"),
            // Sticky flag: any chat that has ever had Dream mode toggled on is excluded
            // from `remember(source="chats")` results. Dream chats accumulate dream-prompt
            // injections, dream tool calls, and mirror-write traffic — none of which is
            // meaningful conversation content for future curation. Set once, never cleared.
            ("ExcludeFromDream", "INTEGER NOT NULL DEFAULT 0"),
            // Auto-approve tool permissions for this conversation. Previously only ever
            // reached the State JSON blob, which LoadStateAsync does not read once the
            // normalized row exists - so it silently reverted to ASK on every load.
            ("FullPermissionMode", "INTEGER NOT NULL DEFAULT 0")
        };

        foreach (var (name, definition) in columnsToAdd)
        {
            if (existingColumns.Contains(name))
            {
                continue;
            }

            using var alterCommand = connection.CreateCommand();
            alterCommand.CommandText = $"ALTER TABLE ConversationState ADD COLUMN {name} {definition};";
            alterCommand.ExecuteNonQuery();
        }

        using var messagesCommand = connection.CreateCommand();
        messagesCommand.CommandText = @"
            CREATE TABLE IF NOT EXISTS ConversationMessages (
                Id TEXT PRIMARY KEY,
                StateId TEXT NOT NULL,
                MessageIndex INTEGER NOT NULL,
                MessageId TEXT NOT NULL,
                Role TEXT NOT NULL,
                Content TEXT,
                Parts TEXT,
                CreatedAt TEXT NOT NULL,
                FOREIGN KEY (StateId) REFERENCES ConversationState(Id) ON DELETE CASCADE
            );

            CREATE INDEX IF NOT EXISTS idx_conversation_messages_stateid ON ConversationMessages(StateId);
            CREATE INDEX IF NOT EXISTS idx_conversation_messages_index ON ConversationMessages(StateId, MessageIndex);
            CREATE UNIQUE INDEX IF NOT EXISTS idx_conversation_messages_unique ON ConversationMessages(StateId, MessageId);
        ";
        messagesCommand.ExecuteNonQuery();

        EnsureThreadsDreamColumn(connection);
        EnsureThreadsArchiveColumns(connection);
        EnsureDreamSchema(connection);
    }

    /// <summary>
    /// Adds DirectorGoals columns introduced after the table first shipped.
    /// CREATE TABLE IF NOT EXISTS is a no-op once the table exists, so a column added
    /// to that statement never reaches an install that already ran an earlier build -
    /// every insert then fails on the missing column.
    /// </summary>
    private static void EnsureDirectorGoalColumns(SqliteConnection connection)
    {
        AutomatonDebug.Log("ConfigDatabase.EnsureDirectorGoalColumns: Entry");

        var existingColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var command = connection.CreateCommand())
        {
            command.CommandText = "PRAGMA table_info(DirectorGoals);";
            using var reader = command.ExecuteReader();
            while (reader.Read())
            {
                existingColumns.Add(reader.GetString(1));
            }
        }

        if (existingColumns.Count == 0)
        {
            AutomatonDebug.Log("ConfigDatabase.EnsureDirectorGoalColumns: Exit - table does not exist yet");
            return;
        }

        var columnsToAdd = new (string Name, string Definition)[]
        {
            ("PendingAnswer", "TEXT"),
            ("OriginatingThreadId", "TEXT"),
            ("OriginatingSessionKey", "TEXT"),

            // Link to a project goal and one of its tasks, and the record of one attempt
            // (GOALSUPDATE.md 4.8, 8.1, 8.2).
            ("ProjectGoalId", "TEXT"),
            ("GoalTaskId", "TEXT"),
            ("ChainProjectGoal", "INTEGER NOT NULL DEFAULT 0"),
            ("RunId", "TEXT"),
            ("WorkerSessionId", "TEXT"),
            ("ReportedOutcome", "TEXT"),
            ("ReportedSummary", "TEXT"),
            ("ReportedRunId", "TEXT"),
            ("ReportedAt", "TEXT")
        };

        foreach (var (name, definition) in columnsToAdd)
        {
            if (existingColumns.Contains(name))
            {
                continue;
            }

            using var alterCommand = connection.CreateCommand();
            alterCommand.CommandText = $"ALTER TABLE DirectorGoals ADD COLUMN {name} {definition};";
            alterCommand.ExecuteNonQuery();
            AutomatonDebug.Log($"ConfigDatabase.EnsureDirectorGoalColumns: Added column {name}");
        }

        AutomatonDebug.Log("ConfigDatabase.EnsureDirectorGoalColumns: Exit");
    }

    private static void EnsureCalendarWatchColumns(SqliteConnection connection)
    {
        AutomatonDebug.Log("ConfigDatabase.EnsureCalendarWatchColumns: Entry");

        var existingColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var command = connection.CreateCommand())
        {
            command.CommandText = "PRAGMA table_info(CalendarTasks);";
            using var reader = command.ExecuteReader();
            while (reader.Read())
            {
                existingColumns.Add(reader.GetString(1));
            }
        }

        var columnsToAdd = new (string Name, string Definition)[]
        {
            ("IsWatch", "INTEGER NOT NULL DEFAULT 0"),
            ("RunWithoutAsking", "INTEGER NOT NULL DEFAULT 0"),
            ("SourceSessionId", "TEXT"),
            ("WatchSessionId", "TEXT"),
            ("Objective", "TEXT"),
            ("IntervalMinutes", "INTEGER"),
            ("NotifyCriteria", "TEXT"),
            ("StopCondition", "TEXT"),
            ("LastObservation", "TEXT"),
            ("NextRunAt", "TEXT"),
            ("LastRunAt", "TEXT"),
            ("RunCount", "INTEGER NOT NULL DEFAULT 0"),
            ("LastRunError", "TEXT"),
            ("TaskSessionId", "TEXT"),
            ("RecurrenceInterval", "INTEGER NOT NULL DEFAULT 1"),
            ("ScheduleJson", "TEXT"),
            ("ClaimedRunId", "TEXT"),
            ("ClaimedBy", "TEXT"),
            ("ClaimedAt", "TEXT")
        };

        foreach (var (name, definition) in columnsToAdd)
        {
            if (existingColumns.Contains(name))
            {
                continue;
            }

            using var alterCommand = connection.CreateCommand();
            alterCommand.CommandText = $"ALTER TABLE CalendarTasks ADD COLUMN {name} {definition};";
            alterCommand.ExecuteNonQuery();
            AutomatonDebug.Log($"ConfigDatabase.EnsureCalendarWatchColumns: Added column {name}");
        }

        using var indexCommand = connection.CreateCommand();
        indexCommand.CommandText =
            "CREATE INDEX IF NOT EXISTS idx_cal_tasks_nextrun ON CalendarTasks(IsWatch, NextRunAt);";
        indexCommand.ExecuteNonQuery();

        using var runsCommand = connection.CreateCommand();
        runsCommand.CommandText = @"CREATE TABLE IF NOT EXISTS CalendarRuns (
            Id TEXT PRIMARY KEY, TaskId TEXT NOT NULL, SessionId TEXT,
            ScheduledAt TEXT NOT NULL, StartedAt TEXT NOT NULL, FinishedAt TEXT,
            Outcome TEXT NOT NULL, Summary TEXT NOT NULL, MessageId TEXT,
            FOREIGN KEY(TaskId) REFERENCES CalendarTasks(Id) ON DELETE CASCADE);
            CREATE INDEX IF NOT EXISTS idx_calendar_runs_task ON CalendarRuns(TaskId, StartedAt);";
        runsCommand.ExecuteNonQuery();
        AutomatonDebug.Log("ConfigDatabase.EnsureCalendarWatchColumns: Exit");
    }

    private static void EnsureThreadsDreamColumn(SqliteConnection connection)
    {
        AutomatonDebug.Log("ConfigDatabase.EnsureThreadsDreamColumn: Entry");

        var existingColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var command = connection.CreateCommand())
        {
            command.CommandText = "PRAGMA table_info(Threads);";
            using var reader = command.ExecuteReader();
            while (reader.Read())
            {
                existingColumns.Add(reader.GetString(1));
            }
        }

        if (existingColumns.Contains("IsDreamEnabled"))
        {
            AutomatonDebug.Log("ConfigDatabase.EnsureThreadsDreamColumn: Exit - column already present");
            return;
        }

        using var alterCommand = connection.CreateCommand();
        alterCommand.CommandText = "ALTER TABLE Threads ADD COLUMN IsDreamEnabled INTEGER NOT NULL DEFAULT 0;";
        alterCommand.ExecuteNonQuery();

        AutomatonDebug.Log("ConfigDatabase.EnsureThreadsDreamColumn: Exit - column added");
    }

    /// <summary>
    /// Adds the columns that let a chat be archived: hidden from every tree and list
    /// while its row, state and messages stay exactly as they were, so restoring it
    /// is clearing the flag. Unrelated to the ConversationState archive columns,
    /// which keep the content of a chat that was cleared or deleted.
    /// </summary>
    private static void EnsureThreadsArchiveColumns(SqliteConnection connection)
    {
        AutomatonDebug.Log("ConfigDatabase.EnsureThreadsArchiveColumns: Entry");

        var existingColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var command = connection.CreateCommand())
        {
            command.CommandText = "PRAGMA table_info(Threads);";
            using var reader = command.ExecuteReader();
            while (reader.Read())
            {
                existingColumns.Add(reader.GetString(1));
            }
        }

        var added = 0;
        foreach (var (name, definition) in new[]
        {
            ("IsArchived", "INTEGER NOT NULL DEFAULT 0"),
            ("ArchivedAt", "TEXT")
        })
        {
            if (existingColumns.Contains(name))
            {
                continue;
            }

            using var alterCommand = connection.CreateCommand();
            alterCommand.CommandText = $"ALTER TABLE Threads ADD COLUMN {name} {definition};";
            alterCommand.ExecuteNonQuery();
            added++;
        }

        AutomatonDebug.Log($"ConfigDatabase.EnsureThreadsArchiveColumns: Exit - added {added} column(s)");
    }

    private static void EnsureDreamSchema(SqliteConnection connection)
    {
        AutomatonDebug.Log("ConfigDatabase.EnsureDreamSchema: Entry");

        // DreamConversationState mirrors ConversationState column-for-column,
        // plus IterationCount/LastDreamedAt so the LLM can see how many times
        // it has curated a given source conversation and when it last touched it.
        using var stateCommand = connection.CreateCommand();
        stateCommand.CommandText = @"
            CREATE TABLE IF NOT EXISTS DreamConversationState (
                Id TEXT PRIMARY KEY,
                ProjectId TEXT,
                ThreadId TEXT,
                State TEXT NOT NULL,
                SessionId TEXT,
                Agent TEXT,
                Directory TEXT,
                Worktree TEXT,
                UseRolesEnabled INTEGER DEFAULT 0,
                ShowThinkingEnabled INTEGER DEFAULT 1,
                ShowToolCallsEnabled INTEGER DEFAULT 1,
                CollapseThinkingEnabled INTEGER DEFAULT 1,
                CollapseToolCallsEnabled INTEGER DEFAULT 1,
                SelectedProviderKey TEXT,
                SelectedModelId TEXT,
                CompressionSettings TEXT,
                Revision INTEGER DEFAULT 0,
                IterationCount INTEGER NOT NULL DEFAULT 0,
                LastDreamedAt TEXT,
                CreatedAt TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL
            );
        ";
        stateCommand.ExecuteNonQuery();

        // Backfill IterationCount/LastDreamedAt on databases that pre-date these columns.
        var dreamColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var pragma = connection.CreateCommand())
        {
            pragma.CommandText = "PRAGMA table_info(DreamConversationState);";
            using var reader = pragma.ExecuteReader();
            while (reader.Read())
            {
                dreamColumns.Add(reader.GetString(1));
            }
        }

        var dreamAdds = new (string Name, string Definition)[]
        {
            ("IterationCount", "INTEGER NOT NULL DEFAULT 0"),
            ("LastDreamedAt", "TEXT")
        };

        foreach (var (name, definition) in dreamAdds)
        {
            if (dreamColumns.Contains(name))
            {
                continue;
            }

            using var alterCommand = connection.CreateCommand();
            alterCommand.CommandText = $"ALTER TABLE DreamConversationState ADD COLUMN {name} {definition};";
            alterCommand.ExecuteNonQuery();
        }

        // DreamConversationMessages mirrors ConversationMessages column-for-column.
        using var messagesCommand = connection.CreateCommand();
        messagesCommand.CommandText = @"
            CREATE TABLE IF NOT EXISTS DreamConversationMessages (
                Id TEXT PRIMARY KEY,
                StateId TEXT NOT NULL,
                MessageIndex INTEGER NOT NULL,
                MessageId TEXT NOT NULL,
                Role TEXT NOT NULL,
                Content TEXT,
                Parts TEXT,
                CreatedAt TEXT NOT NULL,
                FOREIGN KEY (StateId) REFERENCES DreamConversationState(Id) ON DELETE CASCADE
            );

            CREATE INDEX IF NOT EXISTS idx_dream_messages_stateid ON DreamConversationMessages(StateId);
            CREATE INDEX IF NOT EXISTS idx_dream_messages_index ON DreamConversationMessages(StateId, MessageIndex);
            CREATE UNIQUE INDEX IF NOT EXISTS idx_dream_messages_unique ON DreamConversationMessages(StateId, MessageId);
        ";
        messagesCommand.ExecuteNonQuery();

        AutomatonDebug.Log("ConfigDatabase.EnsureDreamSchema: Exit");
    }

    /// <summary>
    /// Migrates InputHistory to the per-chat layout: adds ConversationKey (which
    /// chat an entry belongs to) and Sequence (position within that chat's history,
    /// oldest = 0). CreatedAt alone cannot order entries because a save stamps every
    /// row with the same timestamp. Rows written before this migration carry an empty
    /// key and are unreachable from the keyed queries - they are left in place, not
    /// deleted.
    /// </summary>
    /// <summary>
    /// Adds the message attachment column to databases created before it existed.
    /// It holds metadata only - the bytes live under
    /// <see cref="AttachmentStore.Directory"/> - so it stays small.
    /// </summary>
    /// <summary>
    /// Removes the FTS5 tables that lived in this file before the search index moved to
    /// search.db. Nothing reads them any more; leaving them would keep every transcript stored
    /// twice and keep this file paying for index writes it no longer receives. Idempotent.
    /// </summary>
    /// <summary>
    /// Goal tracking (GOALSUPDATE.md §4): progress state below the goal, an append-only
    /// journal, conversation links, bindings and cross-project grants. Extends the Goals
    /// table in place; the legacy list columns stay as the migration source.
    /// </summary>
    private static void EnsureGoalTrackingSchema(SqliteConnection connection)
    {
        AutomatonDebug.Log("ConfigDatabase.EnsureGoalTrackingSchema: Entry");
        if (connection == null)
        {
            AutomatonDebug.LogWarning("ConfigDatabase.EnsureGoalTrackingSchema: Exit - no connection");
            return;
        }

        var existingColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var infoCommand = connection.CreateCommand())
        {
            infoCommand.CommandText = "PRAGMA table_info(Goals);";
            using var reader = infoCommand.ExecuteReader();
            while (reader.Read())
            {
                existingColumns.Add(reader.GetString(1));
            }
        }

        if (existingColumns.Count == 0)
        {
            AutomatonDebug.LogWarning("ConfigDatabase.EnsureGoalTrackingSchema: Exit - Goals table does not exist");
            return;
        }

        var columnsToAdd = new (string Name, string Definition)[]
        {
            ("Title", "TEXT"),
            ("Priority", "INTEGER NOT NULL DEFAULT 0"),
            ("Revision", "INTEGER NOT NULL DEFAULT 0"),
            ("WorkRevision", "INTEGER NOT NULL DEFAULT 0"),
            ("NextTaskNumber", "INTEGER NOT NULL DEFAULT 1"),
            ("NextCriterionNumber", "INTEGER NOT NULL DEFAULT 1"),
            ("MigrationVersion", "INTEGER NOT NULL DEFAULT 0"),
            ("CompletedAt", "TEXT"),
            ("PendingScopeChange", "TEXT"),
            ("ReviewSummary", "TEXT"),
            ("ReviewRequestedAt", "TEXT")
        };

        var added = 0;
        foreach (var (name, definition) in columnsToAdd)
        {
            if (existingColumns.Contains(name))
            {
                continue;
            }

            using var alterCommand = connection.CreateCommand();
            alterCommand.CommandText = $"ALTER TABLE Goals ADD COLUMN {name} {definition};";
            alterCommand.ExecuteNonQuery();
            added++;
        }

        using var command = connection.CreateCommand();
        command.CommandText = @"
            CREATE TABLE IF NOT EXISTS GoalTasks (
                Id TEXT PRIMARY KEY,
                GoalId TEXT NOT NULL REFERENCES Goals(Id) ON DELETE CASCADE,
                Ordinal INTEGER NOT NULL,
                ShortId TEXT NOT NULL,
                Description TEXT NOT NULL,
                Status TEXT NOT NULL,
                Note TEXT,
                BlockedReason TEXT,
                DependsOn TEXT,
                Evidence TEXT,
                EvidenceBy TEXT,
                NeedsReview INTEGER NOT NULL DEFAULT 0,
                ClaimedBy TEXT,
                ClaimKind TEXT,
                ClaimedAt TEXT,
                ClaimTouchedAt TEXT,
                CreatedAt TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL,
                CompletedAt TEXT,
                UNIQUE(GoalId, ShortId)
            );
            CREATE INDEX IF NOT EXISTS idx_goaltasks_goal ON GoalTasks(GoalId, Ordinal);
            CREATE INDEX IF NOT EXISTS idx_goaltasks_claim ON GoalTasks(ClaimedBy);

            CREATE TABLE IF NOT EXISTS GoalCriteria (
                Id TEXT PRIMARY KEY,
                GoalId TEXT NOT NULL REFERENCES Goals(Id) ON DELETE CASCADE,
                Ordinal INTEGER NOT NULL,
                ShortId TEXT NOT NULL,
                Description TEXT NOT NULL,
                CheckCommand TEXT,
                Status TEXT NOT NULL,
                RequiredProof TEXT NOT NULL,
                MetBy TEXT,
                Evidence TEXT,
                CheckedAt TEXT,
                CheckedWorkRevision INTEGER,
                LastCheckRunId TEXT,
                Author TEXT NOT NULL,
                UNIQUE(GoalId, ShortId)
            );
            CREATE INDEX IF NOT EXISTS idx_goalcriteria_goal ON GoalCriteria(GoalId, Ordinal);

            CREATE TABLE IF NOT EXISTS GoalCheckRuns (
                Id TEXT PRIMARY KEY,
                GoalId TEXT NOT NULL REFERENCES Goals(Id) ON DELETE CASCADE,
                CriterionId TEXT NOT NULL,
                WorkRevision INTEGER NOT NULL,
                Command TEXT NOT NULL,
                WorkingFolder TEXT NOT NULL,
                StartedAt TEXT NOT NULL,
                FinishedAt TEXT,
                ExitCode INTEGER,
                Result TEXT NOT NULL,
                EvidenceTail TEXT,
                StartedBy TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_goalcheckruns_goal ON GoalCheckRuns(GoalId, StartedAt);

            CREATE TABLE IF NOT EXISTS GoalJournal (
                Id INTEGER PRIMARY KEY AUTOINCREMENT,
                GoalId TEXT NOT NULL REFERENCES Goals(Id) ON DELETE CASCADE,
                TaskId TEXT,
                Kind TEXT NOT NULL,
                Content TEXT NOT NULL,
                StateId TEXT,
                Author TEXT NOT NULL,
                CreatedAt TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_goaljournal_goal ON GoalJournal(GoalId, Id);

            CREATE TABLE IF NOT EXISTS GoalConversations (
                GoalId TEXT NOT NULL REFERENCES Goals(Id) ON DELETE CASCADE,
                StateId TEXT NOT NULL,
                FirstAt TEXT NOT NULL,
                LastAt TEXT NOT NULL,
                PRIMARY KEY (GoalId, StateId)
            );
            CREATE INDEX IF NOT EXISTS idx_goalconversations_state ON GoalConversations(StateId);

            CREATE TABLE IF NOT EXISTS GoalBindings (
                StateId TEXT PRIMARY KEY,
                GoalId TEXT NOT NULL REFERENCES Goals(Id) ON DELETE CASCADE,
                BoundAt TEXT NOT NULL,
                BoundBy TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS GoalProjectGrants (
                StateId TEXT NOT NULL,
                ProjectId TEXT NOT NULL,
                GrantedAt TEXT NOT NULL,
                PRIMARY KEY (StateId, ProjectId)
            );

            CREATE TABLE IF NOT EXISTS AutomatonLoops (
                SessionId TEXT PRIMARY KEY,
                ProjectId TEXT,
                GoalId TEXT,
                Directory TEXT NOT NULL DEFAULT '',
                IsRunning INTEGER NOT NULL DEFAULT 0,
                StartedAt TEXT,
                LastIterationAt TEXT,
                Iterations INTEGER NOT NULL DEFAULT 0,
                BudgetStartedAt TEXT,
                LastProgressKey TEXT,
                StalledIterations INTEGER NOT NULL DEFAULT 0,
                LastError TEXT
            );";
        command.ExecuteNonQuery();

        AutomatonDebug.Log($"ConfigDatabase.EnsureGoalTrackingSchema: Exit - added {added} Goals column(s)");
    }

    /// <summary>
    /// The full-transcript tables: every message a conversation has ever held, and every
    /// summary compaction has written for it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <c>ConversationMessages</c> is the model's context and is rewritten from it on every
    /// save, so a message the compressor dropped is deleted from it on the save that follows.
    /// <c>ConversationMessagesFull</c> is written from the same save with insert-or-update
    /// semantics and is never pruned, so it is a superset: everything that was ever in the
    /// context, in the order it first arrived, with the compaction summary stripped out of the
    /// system prompt it was merged into. Deleting or archiving a conversation carries these rows
    /// with it exactly as it carries the live ones.
    /// </para>
    /// <para>
    /// <c>ConversationSummaries</c> keeps each distinct summary as it was generated, so the
    /// compressed view is kept beside the raw one rather than in place of it. See
    /// FULLTRANSCRIPT.md.
    /// </para>
    /// </remarks>
    private static void EnsureFullTranscriptSchema(SqliteConnection connection)
    {
        AutomatonDebug.Log("ConfigDatabase.EnsureFullTranscriptSchema: Entry");
        if (connection == null)
        {
            AutomatonDebug.LogWarning("ConfigDatabase.EnsureFullTranscriptSchema: Exit - no connection");
            return;
        }

        using var command = connection.CreateCommand();
        command.CommandText = @"
            CREATE TABLE IF NOT EXISTS ConversationMessagesFull (
                Id TEXT PRIMARY KEY,
                StateId TEXT NOT NULL,
                MessageIndex INTEGER NOT NULL,
                MessageId TEXT NOT NULL,
                Role TEXT NOT NULL,
                Content TEXT,
                Parts TEXT,
                Attachments TEXT,
                FirstSeenAt TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL,
                FOREIGN KEY (StateId) REFERENCES ConversationState(Id) ON DELETE CASCADE
            );

            CREATE INDEX IF NOT EXISTS idx_conversation_messages_full_stateid ON ConversationMessagesFull(StateId);
            CREATE INDEX IF NOT EXISTS idx_conversation_messages_full_index ON ConversationMessagesFull(StateId, MessageIndex);
            CREATE UNIQUE INDEX IF NOT EXISTS idx_conversation_messages_full_unique ON ConversationMessagesFull(StateId, MessageId);

            CREATE TABLE IF NOT EXISTS ConversationSummaries (
                Id TEXT PRIMARY KEY,
                StateId TEXT NOT NULL,
                SummaryIndex INTEGER NOT NULL,
                Content TEXT NOT NULL,
                CreatedAt TEXT NOT NULL,
                FOREIGN KEY (StateId) REFERENCES ConversationState(Id) ON DELETE CASCADE
            );

            CREATE INDEX IF NOT EXISTS idx_conversation_summaries_stateid ON ConversationSummaries(StateId, SummaryIndex);

            -- A reader's own judgement of one memory: 0 (irrelevant) to 10 (important), set
            -- from the search dialog. Keyed by the message the way search hits are, so a raw
            -- message and the summary that condensed it are tagged separately.
            CREATE TABLE IF NOT EXISTS MemoryTags (
                Id TEXT PRIMARY KEY,
                StateId TEXT NOT NULL,
                MessageId TEXT NOT NULL,
                Score INTEGER NOT NULL,
                TaggedAt TEXT NOT NULL,
                FOREIGN KEY (StateId) REFERENCES ConversationState(Id) ON DELETE CASCADE
            );

            CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_tags_unique ON MemoryTags(StateId, MessageId);
        ";
        command.ExecuteNonQuery();
        AutomatonDebug.Log("ConfigDatabase.EnsureFullTranscriptSchema: Exit");
    }

    private static void DropLegacySearchTables(SqliteConnection connection)
    {
        AutomatonDebug.Log("ConfigDatabase.DropLegacySearchTables: Entry");

        if (connection == null)
        {
            AutomatonDebug.LogWarning("ConfigDatabase.DropLegacySearchTables: Exit - no connection");
            return;
        }

        try
        {
            using var command = connection.CreateCommand();
            command.CommandText = "DROP TABLE IF EXISTS ConversationSearch; DROP TABLE IF EXISTS DreamConversationSearch;";
            command.ExecuteNonQuery();
            AutomatonDebug.Log("ConfigDatabase.DropLegacySearchTables: Exit - dropped");
        }
        catch (Exception ex)
        {
            // Another process may hold the write lock right now; the next start tries again.
            AutomatonDebug.LogWarning($"ConfigDatabase.DropLegacySearchTables: Exit - {ex.Message}");
        }
    }

    private static void EnsureConversationAttachmentColumn(SqliteConnection connection)
    {
        foreach (var table in new[] { "ConversationMessages", "DreamConversationMessages" })
        {
            var columns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            using (var probe = connection.CreateCommand())
            {
                probe.CommandText = $"PRAGMA table_info({table});";
                using var reader = probe.ExecuteReader();
                while (reader.Read())
                {
                    columns.Add(reader.GetString(1));
                }
            }

            if (columns.Count == 0 || columns.Contains("Attachments"))
            {
                continue;
            }

            using var alter = connection.CreateCommand();
            alter.CommandText = $"ALTER TABLE {table} ADD COLUMN Attachments TEXT;";
            alter.ExecuteNonQuery();
            AutomatonDebug.Log($"ConfigDatabase: added {table}.Attachments");
        }
    }

    private static void EnsureInputHistorySchema(SqliteConnection connection)
    {
        var existingColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var command = connection.CreateCommand())
        {
            command.CommandText = "PRAGMA table_info(InputHistory);";
            using var reader = command.ExecuteReader();
            while (reader.Read())
            {
                existingColumns.Add(reader.GetString(1));
            }
        }

        var columnsToAdd = new (string Name, string Definition)[]
        {
            ("ConversationKey", "TEXT NOT NULL DEFAULT ''"),
            ("Sequence", "INTEGER NOT NULL DEFAULT 0")
        };

        foreach (var (name, definition) in columnsToAdd)
        {
            if (existingColumns.Contains(name))
            {
                continue;
            }

            using var alterCommand = connection.CreateCommand();
            alterCommand.CommandText = $"ALTER TABLE InputHistory ADD COLUMN {name} {definition};";
            alterCommand.ExecuteNonQuery();
        }

        using var indexCommand = connection.CreateCommand();
        indexCommand.CommandText = "CREATE INDEX IF NOT EXISTS idx_inputhistory_conversationkey ON InputHistory(ConversationKey, Sequence);";
        indexCommand.ExecuteNonQuery();
    }

    /// <summary>
    /// Executes a SQL query and returns the results as a list of dictionaries.
    /// </summary>
    public List<Dictionary<string, object?>> Query(string sql, params SqliteParameter[] parameters)
    {
        AutomatonDebug.Log($"ConfigDatabase.Query: Entry - sql={(sql.Length > 100 ? sql.Substring(0, 100) + "..." : sql)}");

        // Joins the open transaction when there is one, so a read inside a save sees that
        // save's own uncommitted writes rather than blocking on them.
        var results = _ambient.Value is { } ambient
            ? ExecuteQuery(ambient.Connection, ambient.Transaction, sql, parameters)
            : QueryOnNewConnection(sql, parameters);

        var resultsJson = JsonSerializer.Serialize(SanitizeQueryResultsForLog(results));
        AutomatonDebug.Log($"ConfigDatabase.Query: Exit - {results.Count} rows returned: {resultsJson}");
        return results;
    }

    private List<Dictionary<string, object?>> QueryOnNewConnection(string sql, SqliteParameter[] parameters)
    {
        using var connection = CreateOpenConnection();
        return ExecuteQuery(connection, transaction: null, sql, parameters);
    }

    private static object SanitizeQueryResultsForLog(List<Dictionary<string, object?>> results)
    {
        const int maxRows = 10;
        const int maxStringLength = 500;

        var sanitizedRows = results.Take(maxRows)
            .Select(row => row.ToDictionary(
                kvp => kvp.Key,
                kvp => SanitizeQueryValueForLog(kvp.Value, maxStringLength)))
            .ToList();

        if (results.Count <= maxRows)
        {
            return sanitizedRows;
        }

        return new
        {
            rows = sanitizedRows,
            truncated = true,
            omittedRows = results.Count - maxRows
        };
    }

    private static object? SanitizeQueryValueForLog(object? value, int maxStringLength)
    {
        if (value is not string text || text.Length <= maxStringLength)
        {
            return value;
        }

        return text[..maxStringLength] + $"... [truncated {text.Length - maxStringLength} chars]";
    }

    /// <summary>
    /// Executes a SQL command (INSERT, UPDATE, DELETE).
    /// </summary>
    public int Execute(string sql, params SqliteParameter[] parameters)
    {
        var truncatedSql = sql.Length > 100 ? sql.Substring(0, 100) + "..." : sql;
        var paramsStr = parameters.Length > 0
            ? $" params=[{string.Join(", ", parameters.Select(p => $"{p.ParameterName}={p.Value}"))}]"
            : "";
        AutomatonDebug.Log($"ConfigDatabase.Execute: Entry - sql={truncatedSql}{paramsStr}");

        // Enlists in the open transaction when there is one; see ExecuteInTransaction.
        var result = _ambient.Value is { } ambient
            ? ExecuteNonQuery(ambient.Connection, ambient.Transaction, sql, parameters)
            : ExecuteOnNewConnection(sql, parameters);
        AutomatonDebug.Log($"ConfigDatabase.Execute: Exit - {result} rows affected");
        return result;
    }

    private int ExecuteOnNewConnection(string sql, SqliteParameter[] parameters)
    {
        using var connection = CreateOpenConnection();
        return ExecuteNonQuery(connection, transaction: null, sql, parameters);
    }

    /// <summary>
    /// Executes a SQL command and returns the first column of the first row.
    /// </summary>
    public T? ExecuteScalar<T>(string sql, params SqliteParameter[] parameters)
    {
        AutomatonDebug.Log($"ConfigDatabase.ExecuteScalar: Entry - sql={(sql.Length > 100 ? sql.Substring(0, 100) + "..." : sql)}");

        T? result;
        if (_ambient.Value is { } ambient)
        {
            result = ExecuteScalarInternal<T>(ambient.Connection, ambient.Transaction, sql, parameters);
        }
        else
        {
            using var connection = CreateOpenConnection();
            result = ExecuteScalarInternal<T>(connection, transaction: null, sql, parameters);
        }
        AutomatonDebug.Log($"ConfigDatabase.ExecuteScalar: Exit");
        return result;
    }

    public void Dispose()
    {
        AutomatonDebug.Log("ConfigDatabase.Dispose: Entry");
        if (_disposed) return;
        _disposed = true;
        AutomatonDebug.Log("ConfigDatabase.Dispose: Exit");
    }

    #region Projects Table Helpers

    /// <summary>
    /// Gets all projects from the database.
    /// </summary>
    public List<Dictionary<string, object?>> GetAllProjects()
    {
        return Query("SELECT * FROM Projects ORDER BY UpdatedAt DESC");
    }

    /// <summary>
    /// Gets a project by ID.
    /// </summary>
    public Dictionary<string, object?>? GetProject(string id)
    {
        return Query("SELECT * FROM Projects WHERE Id = @Id",
            new SqliteParameter("@Id", id)).FirstOrDefault();
    }

    /// <summary>
    /// Saves or updates a project.
    /// </summary>
    public void SaveProject(string id, string name, string path)
    {
        var now = DateTime.UtcNow.ToString("O");
        Execute(@"
            INSERT INTO Projects (Id, Name, Path, CreatedAt, UpdatedAt)
            VALUES (@Id, @Name, @Path, @CreatedAt, @UpdatedAt)
            ON CONFLICT(Id) DO UPDATE SET
                Name = @Name,
                Path = @Path,
                UpdatedAt = @UpdatedAt",
            new SqliteParameter("@Id", id),
            new SqliteParameter("@Name", name),
            new SqliteParameter("@Path", path),
            new SqliteParameter("@CreatedAt", now),
            new SqliteParameter("@UpdatedAt", now));
    }

    /// <summary>
    /// Deletes a project and all its associated data (cascades to Goals, ProjectFiles, ProjectTools, Threads).
    /// </summary>
    public void DeleteProject(string id)
    {
        ExecuteInTransaction((connection, transaction) =>
        {
            // Delete in order to respect foreign key constraints (if any)
            // or just delete in the correct order manually
            ExecuteNonQuery(connection, transaction, "DELETE FROM Goals WHERE ProjectId = @Id", new SqliteParameter("@Id", id));
            ExecuteNonQuery(connection, transaction, "DELETE FROM ProjectFiles WHERE ProjectId = @Id", new SqliteParameter("@Id", id));
            ExecuteNonQuery(connection, transaction, "DELETE FROM ProjectTools WHERE ProjectId = @Id", new SqliteParameter("@Id", id));
            ExecuteNonQuery(connection, transaction, "DELETE FROM Threads WHERE ProjectId = @Id", new SqliteParameter("@Id", id));
            ExecuteNonQuery(connection, transaction, "DELETE FROM Projects WHERE Id = @Id", new SqliteParameter("@Id", id));
        });
    }

    #endregion

    #region Threads Table Helpers

    /// <summary>
    /// Gets all threads for a project from the database, ordered by LastModifiedAt descending.
    /// </summary>
    public List<Dictionary<string, object?>> GetThreadsForProject(string projectId)
    {
        return Query("SELECT * FROM Threads WHERE ProjectId = @ProjectId ORDER BY LastModifiedAt DESC",
            new SqliteParameter("@ProjectId", projectId));
    }

    /// <summary>
    /// Saves or updates a thread.
    /// </summary>
    public void SaveThread(string id, string projectId, string name, string stateFilePath, bool isAutomationEnabled, DateTime createdAt, DateTime lastModifiedAt, bool isDreamEnabled = false)
    {
        AutomatonDebug.Log($"ConfigDatabase.SaveThread: Entry - id={id}, projectId={projectId}, name={name}, isAutomationEnabled={isAutomationEnabled}, isDreamEnabled={isDreamEnabled}");

        Execute(@"
            INSERT INTO Threads (Id, ProjectId, Name, StateFilePath, IsAutomationEnabled, IsDreamEnabled, CreatedAt, LastModifiedAt)
            VALUES (@Id, @ProjectId, @Name, @StateFilePath, @IsAutomationEnabled, @IsDreamEnabled, @CreatedAt, @LastModifiedAt)
            ON CONFLICT(Id) DO UPDATE SET
                Name = @Name,
                StateFilePath = @StateFilePath,
                IsAutomationEnabled = @IsAutomationEnabled,
                IsDreamEnabled = @IsDreamEnabled,
                LastModifiedAt = @LastModifiedAt",
            new SqliteParameter("@Id", id),
            new SqliteParameter("@ProjectId", projectId),
            new SqliteParameter("@Name", name),
            new SqliteParameter("@StateFilePath", stateFilePath),
            new SqliteParameter("@IsAutomationEnabled", isAutomationEnabled ? 1 : 0),
            new SqliteParameter("@IsDreamEnabled", isDreamEnabled ? 1 : 0),
            new SqliteParameter("@CreatedAt", createdAt.ToString("O")),
            new SqliteParameter("@LastModifiedAt", lastModifiedAt.ToString("O")));

        AutomatonDebug.Log("ConfigDatabase.SaveThread: Exit");
    }

    /// <summary>
    /// Deletes a thread.
    /// </summary>
    public void DeleteThread(string id)
    {
        Execute("DELETE FROM Threads WHERE Id = @Id", new SqliteParameter("@Id", id));
    }

    /// <summary>
    /// Gets one thread row with its project's name and path, or null when there is
    /// no such thread. Archived threads are included.
    /// </summary>
    public Dictionary<string, object?>? GetThread(string id)
    {
        AutomatonDebug.Log($"ConfigDatabase.GetThread: Entry - id={id}");

        if (string.IsNullOrWhiteSpace(id))
        {
            AutomatonDebug.Log("ConfigDatabase.GetThread: Exit - empty id");
            return null;
        }

        var row = Query(@"
            SELECT t.*, p.Name AS ProjectName, p.Path AS ProjectPath
            FROM Threads t
            LEFT JOIN Projects p ON p.Id = t.ProjectId
            WHERE t.Id = @Id
            LIMIT 1",
            new SqliteParameter("@Id", id))
            .FirstOrDefault();

        AutomatonDebug.Log($"ConfigDatabase.GetThread: Exit - found={row != null}");
        return row;
    }

    /// <summary>
    /// Every thread with its project's name and path, newest first. Archived threads
    /// are left out unless <paramref name="includeArchived"/> is set.
    /// </summary>
    public List<Dictionary<string, object?>> GetAllThreads(bool includeArchived)
    {
        AutomatonDebug.Log($"ConfigDatabase.GetAllThreads: Entry - includeArchived={includeArchived}");

        var rows = Query($@"
            SELECT t.*, p.Name AS ProjectName, p.Path AS ProjectPath
            FROM Threads t
            INNER JOIN Projects p ON p.Id = t.ProjectId
            {(includeArchived ? string.Empty : "WHERE t.IsArchived = 0")}
            ORDER BY t.LastModifiedAt DESC");

        AutomatonDebug.Log($"ConfigDatabase.GetAllThreads: Exit - {rows.Count} thread(s)");
        return rows;
    }

    /// <summary>
    /// Renames a thread and touches nothing else on the row. Use this rather than
    /// <see cref="SaveThread"/> when only the name changes: SaveThread rewrites every
    /// flag, so a caller that does not have them all to hand resets the rest.
    /// </summary>
    /// <returns>True when the thread existed and was renamed.</returns>
    public bool RenameThread(string id, string newName)
    {
        AutomatonDebug.Log($"ConfigDatabase.RenameThread: Entry - id={id}, newName={newName}");

        if (string.IsNullOrWhiteSpace(id) || string.IsNullOrWhiteSpace(newName))
        {
            AutomatonDebug.Log("ConfigDatabase.RenameThread: Exit - empty id or name");
            return false;
        }

        var affected = Execute(
            "UPDATE Threads SET Name = @Name, LastModifiedAt = @Now WHERE Id = @Id",
            new SqliteParameter("@Id", id),
            new SqliteParameter("@Name", newName),
            new SqliteParameter("@Now", DateTime.UtcNow.ToString("O")));

        AutomatonDebug.Log($"ConfigDatabase.RenameThread: Exit - affected={affected}");
        return affected > 0;
    }

    /// <summary>
    /// Archives or restores a thread. Deliberately the only writer of IsArchived:
    /// <see cref="SaveThread"/> leaves the column alone, so a UI saving its in-memory
    /// tree can never un-archive a chat it does not know about, nor archive one.
    /// LastModifiedAt is left as it was so a restored chat returns to its old place.
    /// </summary>
    /// <returns>True when the thread existed.</returns>
    public bool SetThreadArchived(string id, bool archived)
    {
        AutomatonDebug.Log($"ConfigDatabase.SetThreadArchived: Entry - id={id}, archived={archived}");

        if (string.IsNullOrWhiteSpace(id))
        {
            AutomatonDebug.Log("ConfigDatabase.SetThreadArchived: Exit - empty id");
            return false;
        }

        var affected = Execute(
            "UPDATE Threads SET IsArchived = @IsArchived, ArchivedAt = @ArchivedAt WHERE Id = @Id",
            new SqliteParameter("@Id", id),
            new SqliteParameter("@IsArchived", archived ? 1 : 0),
            new SqliteParameter("@ArchivedAt", archived ? DateTime.UtcNow.ToString("O") : DBNull.Value));

        AutomatonDebug.Log($"ConfigDatabase.SetThreadArchived: Exit - affected={affected}");
        return affected > 0;
    }

    /// <summary>True when the row carries the archived flag.</summary>
    public static bool IsThreadRowArchived(Dictionary<string, object?> row)
    {
        if (row == null)
        {
            return false;
        }

        return row.TryGetValue("IsArchived", out var value) && value != null && value is not DBNull && Convert.ToInt32(value) == 1;
    }

    #endregion

    #region InputHistory Table Helpers

    /// <summary>
    /// Gets the input history for one conversation, oldest first, keeping only the
    /// most recent <paramref name="limit"/> entries.
    /// </summary>
    public List<string> GetInputHistory(string conversationKey, int limit = 200)
    {
        var rows = Query(@"
            SELECT Input FROM (
                SELECT Input, Sequence FROM InputHistory
                WHERE ConversationKey = @Key
                ORDER BY Sequence DESC
                LIMIT @Limit
            )
            ORDER BY Sequence ASC",
            new SqliteParameter("@Key", conversationKey),
            new SqliteParameter("@Limit", limit));
        return rows.Select(r => r["Input"] as string ?? "").Where(s => !string.IsNullOrEmpty(s)).ToList();
    }

    /// <summary>
    /// Saves the entire input history for one conversation, replacing only that
    /// conversation's rows. Entries are renumbered so Sequence 0 is the oldest.
    /// </summary>
    public void SaveInputHistory(string conversationKey, IEnumerable<string> entries)
    {
        var now = DateTime.UtcNow.ToString("O");
        ExecuteInTransaction((connection, transaction) =>
        {
            ExecuteNonQuery(connection, transaction,
                "DELETE FROM InputHistory WHERE ConversationKey = @Key",
                new SqliteParameter("@Key", conversationKey));

            var sequence = 0;
            foreach (var entry in entries)
            {
                if (string.IsNullOrWhiteSpace(entry))
                {
                    continue;
                }

                ExecuteNonQuery(connection, transaction, @"
                    INSERT INTO InputHistory (Id, ConversationKey, Input, Sequence, CreatedAt)
                    VALUES (@Id, @Key, @Input, @Sequence, @CreatedAt)",
                    new SqliteParameter("@Id", Guid.NewGuid().ToString()),
                    new SqliteParameter("@Key", conversationKey),
                    new SqliteParameter("@Input", entry),
                    new SqliteParameter("@Sequence", sequence++),
                    new SqliteParameter("@CreatedAt", now));
            }
        });
    }

    /// <summary>
    /// Deletes all input history for one conversation (e.g. when its thread is deleted).
    /// </summary>
    public void DeleteInputHistory(string conversationKey)
    {
        Execute("DELETE FROM InputHistory WHERE ConversationKey = @Key",
            new SqliteParameter("@Key", conversationKey));
    }

    #endregion

    #region Goals Table Helpers

    /// <summary>
    /// Gets all active goals with their working folders for documentation policy checks.
    /// </summary>
    public List<(string Id, string Description, string WorkingFolder, int Status)> GetAllActiveGoals()
    {
        var rows = Query(@"
            SELECT Id, Description, WorkingFolder, Status
            FROM Goals
            WHERE WorkingFolder IS NOT NULL AND WorkingFolder != ''");

        return rows.Select(r => (
            Id: r["Id"] as string ?? "",
            Description: r["Description"] as string ?? "",
            WorkingFolder: r["WorkingFolder"] as string ?? "",
            Status: (int)(r["Status"] as long? ?? 0)
        )).ToList();
    }

    #endregion

    #region ProjectFiles Table Helpers

    /// <summary>
    /// Gets all project files for a given project, ordered by most recently updated.
    /// </summary>
    public List<Dictionary<string, object?>> GetProjectFiles(string projectId)
    {
        return Query(
            "SELECT * FROM ProjectFiles WHERE ProjectId = @ProjectId ORDER BY UpdatedAt DESC",
            new SqliteParameter("@ProjectId", projectId));
    }

    /// <summary>
    /// Saves or updates a project file.
    /// </summary>
    public void SaveProjectFile(string id, string projectId, string name, string? content, string? description, DateTime createdAt)
    {
        var now = DateTime.UtcNow.ToString("O");
        Execute(@"
            INSERT INTO ProjectFiles (Id, ProjectId, Name, Content, Description, CreatedAt, UpdatedAt)
            VALUES (@Id, @ProjectId, @Name, @Content, @Description, @CreatedAt, @UpdatedAt)
            ON CONFLICT(Id) DO UPDATE SET
                Name = @Name,
                Content = @Content,
                Description = @Description,
                UpdatedAt = @UpdatedAt",
            new SqliteParameter("@Id", id),
            new SqliteParameter("@ProjectId", projectId),
            new SqliteParameter("@Name", name),
            new SqliteParameter("@Content", content ?? (object)DBNull.Value),
            new SqliteParameter("@Description", description ?? (object)DBNull.Value),
            new SqliteParameter("@CreatedAt", createdAt.ToString("O")),
            new SqliteParameter("@UpdatedAt", now));
    }

    /// <summary>
    /// Deletes a project file.
    /// </summary>
    public void DeleteProjectFile(string id)
    {
        Execute("DELETE FROM ProjectFiles WHERE Id = @Id", new SqliteParameter("@Id", id));
    }

    /// <summary>
    /// Deletes all project files for a given project.
    /// </summary>
    public void DeleteProjectFilesForProject(string projectId)
    {
        Execute("DELETE FROM ProjectFiles WHERE ProjectId = @ProjectId", new SqliteParameter("@ProjectId", projectId));
    }

    #endregion

    #region Syslog Settings Helpers

    private const string SyslogSettingsKey = "syslog_settings";

    /// <summary>
    /// Gets the syslog configuration from settings.
    /// </summary>
    /// <summary>One value from the Settings table, or null when the key has never been set.</summary>
    public string? GetSettingValue(string key)
    {
        if (string.IsNullOrWhiteSpace(key))
        {
            return null;
        }

        var row = Query("SELECT Value FROM Settings WHERE Key = @Key", new SqliteParameter("@Key", key)).FirstOrDefault();
        var value = row?["Value"] as string;
        AutomatonDebug.Log($"ConfigDatabase.GetSettingValue: key={key}, found={value != null}");
        return value;
    }

    /// <summary>Writes one value to the Settings table, replacing any previous value for the key.</summary>
    public void SetSettingValue(string key, string value)
    {
        if (string.IsNullOrWhiteSpace(key))
        {
            AutomatonDebug.LogWarning("ConfigDatabase.SetSettingValue: Exit - blank key");
            return;
        }

        Execute("INSERT OR REPLACE INTO Settings (Key, Value, UpdatedAt) VALUES (@Key, @Value, @Now)",
            new SqliteParameter("@Key", key),
            new SqliteParameter("@Value", value ?? string.Empty),
            new SqliteParameter("@Now", DateTime.UtcNow.ToString("O")));
        AutomatonDebug.Log($"ConfigDatabase.SetSettingValue: key={key}, length={value?.Length ?? 0}");
    }

    public SyslogConfig? GetSyslogSettings()
    {
        try
        {
            var row = Query("SELECT Value FROM Settings WHERE Key = @Key",
                new SqliteParameter("@Key", SyslogSettingsKey)).FirstOrDefault();

            if (row != null && row["Value"] is string json)
            {
                return System.Text.Json.JsonSerializer.Deserialize<SyslogConfig>(json);
            }
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError("Exception in GetSyslogSettings(): " + ex.Message + ex.InnerException + ex.StackTrace);
        }
        return null;
    }

    /// <summary>
    /// Saves the syslog configuration to settings.
    /// </summary>
    public void SaveSyslogSettings(SyslogConfig config)
    {
        var json = System.Text.Json.JsonSerializer.Serialize(config);
        Execute(@"
            INSERT OR REPLACE INTO Settings (Key, Value, UpdatedAt)
            VALUES (@Key, @Value, @UpdatedAt)",
            new SqliteParameter("@Key", SyslogSettingsKey),
            new SqliteParameter("@Value", json),
            new SqliteParameter("@UpdatedAt", DateTime.UtcNow.ToString("O")));
    }

    #endregion

    #region ProjectTools Table Helpers

    /// <summary>
    /// Gets all project tools for a given project, ordered by name.
    /// </summary>
    public List<Dictionary<string, object?>> GetProjectTools(string projectId)
    {
        return Query(
            "SELECT * FROM ProjectTools WHERE ProjectId = @ProjectId ORDER BY Name ASC",
            new SqliteParameter("@ProjectId", projectId));
    }

    /// <summary>
    /// Saves or updates a project tool.
    /// </summary>
    public void SaveProjectTool(string id, string projectId, string name, string? description, string? toolType, string? configuration, bool isEnabled, DateTime createdAt)
    {
        var now = DateTime.UtcNow.ToString("O");
        Execute(@"
            INSERT INTO ProjectTools (Id, ProjectId, Name, Description, ToolType, Configuration, IsEnabled, CreatedAt, UpdatedAt)
            VALUES (@Id, @ProjectId, @Name, @Description, @ToolType, @Configuration, @IsEnabled, @CreatedAt, @UpdatedAt)
            ON CONFLICT(Id) DO UPDATE SET
                Name = @Name,
                Description = @Description,
                ToolType = @ToolType,
                Configuration = @Configuration,
                IsEnabled = @IsEnabled,
                UpdatedAt = @UpdatedAt",
            new SqliteParameter("@Id", id),
            new SqliteParameter("@ProjectId", projectId),
            new SqliteParameter("@Name", name),
            new SqliteParameter("@Description", description ?? (object)DBNull.Value),
            new SqliteParameter("@ToolType", toolType ?? (object)DBNull.Value),
            new SqliteParameter("@Configuration", configuration ?? (object)DBNull.Value),
            new SqliteParameter("@IsEnabled", isEnabled ? 1 : 0),
            new SqliteParameter("@CreatedAt", createdAt.ToString("O")),
            new SqliteParameter("@UpdatedAt", now));
    }

    /// <summary>
    /// Deletes a project tool.
    /// </summary>
    public void DeleteProjectTool(string id)
    {
        Execute("DELETE FROM ProjectTools WHERE Id = @Id", new SqliteParameter("@Id", id));
    }

    /// <summary>
    /// Deletes all project tools for a given project.
    /// </summary>
    public void DeleteProjectToolsForProject(string projectId)
    {
        Execute("DELETE FROM ProjectTools WHERE ProjectId = @ProjectId", new SqliteParameter("@ProjectId", projectId));
    }

    #endregion

    #region ConversationState Helpers (Normalized)

    /// <summary>
    /// Updates conversation metadata columns only (no JSON blob).
    /// </summary>
    public void UpdateConversationStateMetadata(string stateId, string? sessionId, string? agent, string? directory, string? worktree,
        bool useRolesEnabled, bool showThinkingEnabled, bool showToolCallsEnabled,
        bool collapseThinkingEnabled, bool collapseToolCallsEnabled,
        string? selectedProviderKey, string? selectedModelId, string? reasoningEffort,
        string? compressionSettingsJson,
        string? projectId, string? threadId, long revision, bool fullPermissionMode,
        string? verbosity)
    {
        Execute(@"
            UPDATE ConversationState
            SET SessionId = @SessionId,
                Agent = @Agent,
                Directory = @Directory,
                Worktree = @Worktree,
                UseRolesEnabled = @UseRolesEnabled,
                ShowThinkingEnabled = @ShowThinkingEnabled,
                ShowToolCallsEnabled = @ShowToolCallsEnabled,
                CollapseThinkingEnabled = @CollapseThinkingEnabled,
                CollapseToolCallsEnabled = @CollapseToolCallsEnabled,
                SelectedProviderKey = @SelectedProviderKey,
                SelectedModelId = @SelectedModelId,
                ReasoningEffort = @ReasoningEffort,
                Verbosity = @Verbosity,
                CompressionSettings = @CompressionSettings,
                ProjectId = @ProjectId,
                ThreadId = @ThreadId,
                Revision = @Revision,
                FullPermissionMode = @FullPermissionMode,
                UpdatedAt = @UpdatedAt
            WHERE Id = @StateId",
            new SqliteParameter("@StateId", stateId),
            new SqliteParameter("@SessionId", sessionId ?? (object)DBNull.Value),
            new SqliteParameter("@Agent", agent ?? (object)DBNull.Value),
            new SqliteParameter("@Directory", directory ?? (object)DBNull.Value),
            new SqliteParameter("@Worktree", worktree ?? (object)DBNull.Value),
            new SqliteParameter("@UseRolesEnabled", useRolesEnabled ? 1 : 0),
            new SqliteParameter("@ShowThinkingEnabled", showThinkingEnabled ? 1 : 0),
            new SqliteParameter("@ShowToolCallsEnabled", showToolCallsEnabled ? 1 : 0),
            new SqliteParameter("@CollapseThinkingEnabled", collapseThinkingEnabled ? 1 : 0),
            new SqliteParameter("@CollapseToolCallsEnabled", collapseToolCallsEnabled ? 1 : 0),
            new SqliteParameter("@SelectedProviderKey", selectedProviderKey ?? (object)DBNull.Value),
            new SqliteParameter("@SelectedModelId", selectedModelId ?? (object)DBNull.Value),
            new SqliteParameter("@ReasoningEffort", reasoningEffort ?? (object)DBNull.Value),
            new SqliteParameter("@Verbosity", verbosity ?? (object)DBNull.Value),
            new SqliteParameter("@CompressionSettings", compressionSettingsJson ?? (object)DBNull.Value),
            new SqliteParameter("@ProjectId", projectId ?? (object)DBNull.Value),
            new SqliteParameter("@ThreadId", threadId ?? (object)DBNull.Value),
            new SqliteParameter("@Revision", revision),
            new SqliteParameter("@FullPermissionMode", fullPermissionMode ? 1 : 0),
            new SqliteParameter("@UpdatedAt", DateTime.UtcNow.ToString("O")));
    }

    /// <summary>
    /// Inserts or refreshes messages in the ConversationMessages table.
    /// </summary>
    public void InsertConversationMessages(string stateId, List<Dictionary<string, object?>> messages)
    {
        if (messages == null || messages.Count == 0) return;

        AutomatonDebug.Log($"ConfigDatabase.InsertConversationMessages: stateId={stateId}, messages={messages.Count}");

        ExecuteInTransaction((connection, transaction) =>
        {
            var maxIndexResult = ExecuteScalarInternal<long>(connection, transaction,
                "SELECT COALESCE(MAX(MessageIndex), -1) FROM ConversationMessages WHERE StateId = @StateId",
                new SqliteParameter("@StateId", stateId));

            long nextIndex = maxIndexResult + 1;

            foreach (var msg in messages)
            {
                var msgId = msg.ContainsKey("messageId") ? msg["messageId"]?.ToString() : msg.ContainsKey("Id") ? msg["Id"]?.ToString() : Guid.NewGuid().ToString();
                var role = msg.ContainsKey("role") ? msg["role"]?.ToString() : msg.ContainsKey("Role") ? msg["Role"]?.ToString() : "unknown";
                var content = msg.ContainsKey("content") ? msg["content"]?.ToString() : msg.ContainsKey("Content") ? msg["Content"]?.ToString() : null;
                var parts = msg.ContainsKey("parts") ? msg["parts"]?.ToString() : msg.ContainsKey("Parts") ? msg["Parts"]?.ToString() : null;

                ExecuteNonQuery(connection, transaction, @"
                    INSERT INTO ConversationMessages (Id, StateId, MessageIndex, MessageId, Role, Content, Parts, Attachments, CreatedAt)
                    VALUES (@Id, @StateId, @MessageIndex, @MessageId, @Role, @Content, @Parts, @Attachments, @CreatedAt)
                    ON CONFLICT(StateId, MessageId) DO UPDATE SET
                        Role = excluded.Role,
                        Content = excluded.Content,
                        Parts = excluded.Parts,
                        -- Attachments are only ever added, never blanked. Several
                        -- writers save the same conversation and not all of them
                        -- carry attachments; whichever saved last would otherwise
                        -- erase the reference and orphan the stored file.
                        Attachments = COALESCE(excluded.Attachments, ConversationMessages.Attachments),
                        MessageIndex = CASE
                            WHEN ConversationMessages.MessageIndex < 0 THEN excluded.MessageIndex
                            ELSE ConversationMessages.MessageIndex
                        END",
                    new SqliteParameter("@Id", Guid.NewGuid().ToString()),
                    new SqliteParameter("@StateId", stateId),
                    new SqliteParameter("@MessageIndex", nextIndex),
                    new SqliteParameter("@MessageId", msgId ?? (object)DBNull.Value),
                    new SqliteParameter("@Role", role ?? (object)DBNull.Value),
                    new SqliteParameter("@Content", content ?? (object)DBNull.Value),
                    new SqliteParameter("@Parts", parts ?? (object)DBNull.Value),
                    new SqliteParameter("@Attachments",
                        (msg.TryGetValue("attachments", out var attachmentsValue) ? attachmentsValue as string : null)
                            ?? (object)DBNull.Value),
                    new SqliteParameter("@CreatedAt", DateTime.UtcNow.ToString("O")));

                nextIndex++;
            }
        });
    }

    /// <summary>
    /// Replaces the normalized message rows for a conversation with the supplied full-state order.
    /// </summary>
    public void ReplaceConversationMessages(string stateId, List<Dictionary<string, object?>> messages)
    {
        ExecuteInTransaction((connection, transaction) =>
        {
            // What each message already had attached, kept across the delete.
            // Several writers save the same conversation and not all of them carry
            // attachments; without this the first save from an attachment-less copy
            // erases the reference and orphans the stored file. Attachments are
            // added by this table, never removed by it.
            var existingAttachments = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var row in QueryOnConnection(connection, transaction,
                "SELECT MessageId, Attachments FROM ConversationMessages WHERE StateId = @StateId AND Attachments IS NOT NULL",
                new SqliteParameter("@StateId", stateId)))
            {
                if (row.TryGetValue("MessageId", out var idValue) && idValue is string existingId
                    && row.TryGetValue("Attachments", out var attValue) && attValue is string existingAtt)
                {
                    existingAttachments[existingId] = existingAtt;
                }
            }

            ExecuteNonQuery(connection, transaction,
                "DELETE FROM ConversationMessages WHERE StateId = @StateId",
                new SqliteParameter("@StateId", stateId));

            for (var i = 0; i < messages.Count; i++)
            {
                var msg = messages[i];
                var msgId = msg.ContainsKey("messageId") ? msg["messageId"]?.ToString() : msg.ContainsKey("Id") ? msg["Id"]?.ToString() : Guid.NewGuid().ToString();
                var role = msg.ContainsKey("role") ? msg["role"]?.ToString() : msg.ContainsKey("Role") ? msg["Role"]?.ToString() : "unknown";
                var content = msg.ContainsKey("content") ? msg["content"]?.ToString() : msg.ContainsKey("Content") ? msg["Content"]?.ToString() : null;
                var parts = msg.ContainsKey("parts") ? msg["parts"]?.ToString() : msg.ContainsKey("Parts") ? msg["Parts"]?.ToString() : null;

                ExecuteNonQuery(connection, transaction, @"
                    INSERT INTO ConversationMessages (Id, StateId, MessageIndex, MessageId, Role, Content, Parts, Attachments, CreatedAt)
                    VALUES (@Id, @StateId, @MessageIndex, @MessageId, @Role, @Content, @Parts, @Attachments, @CreatedAt)",
                    new SqliteParameter("@Id", Guid.NewGuid().ToString()),
                    new SqliteParameter("@StateId", stateId),
                    new SqliteParameter("@MessageIndex", i),
                    new SqliteParameter("@MessageId", msgId ?? (object)DBNull.Value),
                    new SqliteParameter("@Role", role ?? (object)DBNull.Value),
                    new SqliteParameter("@Content", content ?? (object)DBNull.Value),
                    new SqliteParameter("@Parts", parts ?? (object)DBNull.Value),
                    new SqliteParameter("@Attachments",
                        (msg.TryGetValue("attachments", out var attachmentsValue) ? attachmentsValue as string : null)
                            ?? (msgId is not null && existingAttachments.TryGetValue(msgId, out var carried) ? carried : null)
                            ?? (object)DBNull.Value),
                    new SqliteParameter("@CreatedAt", DateTime.UtcNow.ToString("O")));
            }
        });
    }

    /// <summary>
    /// Removes normalized conversation messages that are not present in the supplied full state.
    /// </summary>
    public void PruneConversationMessagesExcept(string stateId, IEnumerable<string> messageIds)
    {
        var ids = messageIds
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .Distinct(StringComparer.Ordinal)
            .ToList();

        if (ids.Count == 0)
        {
            Execute("DELETE FROM ConversationMessages WHERE StateId = @StateId",
                new SqliteParameter("@StateId", stateId));
            return;
        }

        var parameters = new List<SqliteParameter>
        {
            new("@StateId", stateId)
        };
        var placeholders = new List<string>();
        for (var i = 0; i < ids.Count; i++)
        {
            var name = $"@MessageId{i}";
            placeholders.Add(name);
            parameters.Add(new SqliteParameter(name, ids[i]));
        }

        Execute($@"
            DELETE FROM ConversationMessages
            WHERE StateId = @StateId
              AND MessageId NOT IN ({string.Join(", ", placeholders)})",
            parameters.ToArray());
    }

    /// <summary>
    /// Loads conversation messages from the normalized table.
    /// </summary>
    /// <summary>
    /// Records every message of a save in the full-transcript table. New messages are appended
    /// in arrival order; messages already there have their content refreshed (a streaming reply
    /// is saved several times before it is complete); nothing is ever deleted here. A system
    /// message carrying a compaction summary is stored with the summary stripped, so this table
    /// holds only what was actually said. Returns how many rows were new.
    /// </summary>
    public int UpsertFullConversationMessages(string stateId, List<Dictionary<string, object?>> messages)
    {
        return RecordFullTranscript(stateId, messages, Array.Empty<string>()).NewMessages;
    }

    /// <summary>
    /// Appends a compaction summary to a conversation's summary history, unless it is the same
    /// text as the one most recently recorded - every save after a compaction carries the same
    /// summary until the next compaction, and only a new one is worth a row. Returns true when a
    /// row was written.
    /// </summary>
    public bool RecordConversationSummary(string stateId, string summary)
    {
        if (string.IsNullOrWhiteSpace(summary))
        {
            AutomatonDebug.Log($"ConfigDatabase.RecordConversationSummary: Exit - blank summary for stateId={stateId}");
            return false;
        }

        return RecordFullTranscript(stateId, new List<Dictionary<string, object?>>(), new[] { summary }).NewSummaries > 0;
    }

    /// <summary>
    /// The messages of a save into the full table and the summaries they carry into the
    /// summary table, in <b>one transaction</b>. The two belong together: the full table stores
    /// the system prompt with its summary stripped, so a summary that reached the first write
    /// and not the second would exist nowhere - the live row still carries it, but the full
    /// transcript only ever splits summaries out of live rows when it has no rows of its own.
    /// </summary>
    public (int NewMessages, int NewSummaries) RecordFullTranscript(string stateId, List<Dictionary<string, object?>> messages, IReadOnlyList<string> summaries)
    {
        if (string.IsNullOrWhiteSpace(stateId))
        {
            AutomatonDebug.LogWarning("ConfigDatabase.RecordFullTranscript: Exit - blank stateId");
            return (0, 0);
        }

        messages ??= new List<Dictionary<string, object?>>();
        summaries ??= Array.Empty<string>();
        if (messages.Count == 0 && summaries.Count == 0)
        {
            AutomatonDebug.Log($"ConfigDatabase.RecordFullTranscript: Exit - nothing to record for stateId={stateId}");
            return (0, 0);
        }

        AutomatonDebug.Log($"ConfigDatabase.RecordFullTranscript: Entry - stateId={stateId}, messages={messages.Count}, summaries={summaries.Count}");
        var inserted = 0;
        var refreshed = 0;
        var stripped = 0;
        var skippedBlank = 0;
        var written = 0;

        ExecuteInTransaction((connection, transaction) =>
        {
            var existing = new HashSet<string>(StringComparer.Ordinal);
            foreach (var row in ExecuteQuery(connection, transaction,
                "SELECT MessageId FROM ConversationMessagesFull WHERE StateId = @StateId",
                new SqliteParameter("@StateId", stateId)))
            {
                if (row.TryGetValue("MessageId", out var idValue) && idValue is string existingId)
                {
                    existing.Add(existingId);
                }
            }

            var nextIndex = ExecuteScalarInternal<long>(connection, transaction,
                "SELECT COALESCE(MAX(MessageIndex), -1) FROM ConversationMessagesFull WHERE StateId = @StateId",
                new SqliteParameter("@StateId", stateId)) + 1;

            var now = DateTime.UtcNow.ToString("O");
            foreach (var msg in messages)
            {
                var msgId = msg.ContainsKey("messageId") ? msg["messageId"]?.ToString() : msg.ContainsKey("Id") ? msg["Id"]?.ToString() : null;
                if (string.IsNullOrWhiteSpace(msgId))
                {
                    // Without an id there is nothing to upsert against; the live table would
                    // mint one per save and this table would then accumulate a copy per save.
                    continue;
                }

                var role = msg.ContainsKey("role") ? msg["role"]?.ToString() : msg.ContainsKey("Role") ? msg["Role"]?.ToString() : "unknown";
                var content = msg.ContainsKey("content") ? msg["content"]?.ToString() : msg.ContainsKey("Content") ? msg["Content"]?.ToString() : null;
                var parts = msg.ContainsKey("parts") ? msg["parts"]?.ToString() : msg.ContainsKey("Parts") ? msg["Parts"]?.ToString() : null;
                var attachments = msg.TryGetValue("attachments", out var attachmentsValue) ? attachmentsValue as string : null;

                if (string.Equals(role, "system", StringComparison.OrdinalIgnoreCase) && CompressionSupport.ExtractSummary(content) != null)
                {
                    content = CompressionSupport.StripSummary(content);
                    stripped++;

                    // Compaction with no system prompt to merge into makes a system message
                    // that is nothing but the summary. Nothing raw was said in it, so nothing
                    // goes in this table; the summary itself is recorded below.
                    if (string.IsNullOrWhiteSpace(content) && !existing.Contains(msgId))
                    {
                        skippedBlank++;
                        continue;
                    }
                }

                var isNew = !existing.Contains(msgId);
                ExecuteNonQuery(connection, transaction, @"
                    INSERT INTO ConversationMessagesFull (Id, StateId, MessageIndex, MessageId, Role, Content, Parts, Attachments, FirstSeenAt, UpdatedAt)
                    VALUES (@Id, @StateId, @MessageIndex, @MessageId, @Role, @Content, @Parts, @Attachments, @Now, @Now)
                    ON CONFLICT(StateId, MessageId) DO UPDATE SET
                        Role = excluded.Role,
                        Content = excluded.Content,
                        Parts = excluded.Parts,
                        Attachments = COALESCE(excluded.Attachments, ConversationMessagesFull.Attachments),
                        UpdatedAt = excluded.UpdatedAt",
                    new SqliteParameter("@Id", Guid.NewGuid().ToString()),
                    new SqliteParameter("@StateId", stateId),
                    new SqliteParameter("@MessageIndex", nextIndex),
                    new SqliteParameter("@MessageId", msgId),
                    new SqliteParameter("@Role", role ?? "unknown"),
                    new SqliteParameter("@Content", content ?? (object)DBNull.Value),
                    new SqliteParameter("@Parts", parts ?? (object)DBNull.Value),
                    new SqliteParameter("@Attachments", attachments ?? (object)DBNull.Value),
                    new SqliteParameter("@Now", now));

                if (isNew)
                {
                    existing.Add(msgId);
                    nextIndex++;
                    inserted++;
                }
                else
                {
                    refreshed++;
                }
            }

            var latest = ExecuteQuery(connection, transaction,
                "SELECT SummaryIndex, Content FROM ConversationSummaries WHERE StateId = @StateId ORDER BY SummaryIndex DESC LIMIT 1",
                new SqliteParameter("@StateId", stateId)).FirstOrDefault();
            var latestContent = latest?["Content"] as string;
            var nextSummaryIndex = latest == null ? 0 : Convert.ToInt64(latest["SummaryIndex"]) + 1;

            foreach (var summary in summaries)
            {
                if (string.IsNullOrWhiteSpace(summary) || string.Equals(latestContent, summary, StringComparison.Ordinal))
                {
                    continue;
                }

                ExecuteNonQuery(connection, transaction, @"
                    INSERT INTO ConversationSummaries (Id, StateId, SummaryIndex, Content, CreatedAt)
                    VALUES (@Id, @StateId, @SummaryIndex, @Content, @Now)",
                    new SqliteParameter("@Id", Guid.NewGuid().ToString()),
                    new SqliteParameter("@StateId", stateId),
                    new SqliteParameter("@SummaryIndex", nextSummaryIndex),
                    new SqliteParameter("@Content", summary),
                    new SqliteParameter("@Now", now));
                latestContent = summary;
                nextSummaryIndex++;
                written++;
            }
        });

        AutomatonDebug.Log($"ConfigDatabase.RecordFullTranscript: Exit - stateId={stateId}, inserted={inserted}, refreshed={refreshed}, summariesStripped={stripped}, blankSkipped={skippedBlank}, summariesWritten={written}");
        return (inserted, written);
    }

    /// <summary>The lowest and highest importance a memory can be tagged with.</summary>
    public const int MinMemoryScore = 0;
    public const int MaxMemoryScore = 10;

    /// <summary>The dictionary key <see cref="LoadMemoryTags"/> uses for one memory.</summary>
    public static string MemoryTagKey(string stateId, string messageId) => $"{stateId}|{messageId}";

    /// <summary>Tags one memory with an importance from 0 (irrelevant) to 10 (important), replacing any earlier tag.</summary>
    public bool SetMemoryTag(string stateId, string messageId, int score)
    {
        if (string.IsNullOrWhiteSpace(stateId) || string.IsNullOrWhiteSpace(messageId))
        {
            AutomatonDebug.LogWarning($"ConfigDatabase.SetMemoryTag: Exit - blank key (stateId='{stateId}', messageId='{messageId}')");
            return false;
        }

        if (score < MinMemoryScore || score > MaxMemoryScore)
        {
            AutomatonDebug.LogWarning($"ConfigDatabase.SetMemoryTag: Exit - score {score} is outside {MinMemoryScore}..{MaxMemoryScore}");
            return false;
        }

        Execute(@"
            INSERT INTO MemoryTags (Id, StateId, MessageId, Score, TaggedAt)
            VALUES (@Id, @StateId, @MessageId, @Score, @Now)
            ON CONFLICT(StateId, MessageId) DO UPDATE SET Score = excluded.Score, TaggedAt = excluded.TaggedAt",
            new SqliteParameter("@Id", Guid.NewGuid().ToString()),
            new SqliteParameter("@StateId", stateId),
            new SqliteParameter("@MessageId", messageId),
            new SqliteParameter("@Score", score),
            new SqliteParameter("@Now", DateTime.UtcNow.ToString("O")));
        AutomatonDebug.Log($"ConfigDatabase.SetMemoryTag: stateId={stateId}, messageId={messageId}, score={score}");
        return true;
    }

    /// <summary>Removes a memory's tag, so it is neither important nor irrelevant - just untagged.</summary>
    public bool ClearMemoryTag(string stateId, string messageId)
    {
        if (string.IsNullOrWhiteSpace(stateId) || string.IsNullOrWhiteSpace(messageId))
        {
            return false;
        }

        var removed = Execute("DELETE FROM MemoryTags WHERE StateId = @StateId AND MessageId = @MessageId",
            new SqliteParameter("@StateId", stateId),
            new SqliteParameter("@MessageId", messageId));
        AutomatonDebug.Log($"ConfigDatabase.ClearMemoryTag: stateId={stateId}, messageId={messageId}, removed={removed}");
        return removed > 0;
    }

    /// <summary>Every tag on the given conversations, keyed by <see cref="MemoryTagKey"/>.</summary>
    public Dictionary<string, int> LoadMemoryTags(IEnumerable<string> stateIds)
    {
        var tags = new Dictionary<string, int>(StringComparer.Ordinal);
        var ids = (stateIds ?? Array.Empty<string>())
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .Distinct(StringComparer.Ordinal)
            .ToList();
        if (ids.Count == 0)
        {
            return tags;
        }

        // Chunked so a large result set never exceeds the parameter limit.
        const int chunk = 500;
        for (var offset = 0; offset < ids.Count; offset += chunk)
        {
            var slice = ids.Skip(offset).Take(chunk).ToList();
            var parameters = new List<SqliteParameter>();
            var names = new List<string>();
            for (var i = 0; i < slice.Count; i++)
            {
                names.Add($"@S{i}");
                parameters.Add(new SqliteParameter($"@S{i}", slice[i]));
            }

            foreach (var row in Query($"SELECT StateId, MessageId, Score FROM MemoryTags WHERE StateId IN ({string.Join(", ", names)})", parameters.ToArray()))
            {
                if (row["StateId"] is string stateId && row["MessageId"] is string messageId)
                {
                    tags[MemoryTagKey(stateId, messageId)] = Convert.ToInt32(row["Score"]);
                }
            }
        }

        AutomatonDebug.Log($"ConfigDatabase.LoadMemoryTags: {ids.Count} conversation(s), {tags.Count} tag(s)");
        return tags;
    }

    /// <summary>Every message ever saved for a conversation, in first-seen order. Empty for conversations saved before the full table existed.</summary>
    public List<Dictionary<string, object?>> LoadFullConversationMessages(string stateId)
    {
        if (string.IsNullOrWhiteSpace(stateId))
        {
            return new List<Dictionary<string, object?>>();
        }

        var rows = Query(@"
            SELECT MessageId, Role, Content, Parts, Attachments, FirstSeenAt, UpdatedAt
            FROM ConversationMessagesFull
            WHERE StateId = @StateId
            ORDER BY MessageIndex ASC",
            new SqliteParameter("@StateId", stateId));
        AutomatonDebug.Log($"ConfigDatabase.LoadFullConversationMessages: stateId={stateId}, rows={rows.Count}");
        return rows;
    }

    /// <summary>
    /// The conversation a session belongs to, or null. A tool is handed the session and the
    /// summaries are filed under the conversation, so this is the step between them.
    /// </summary>
    public string? FindStateIdBySession(string sessionId)
    {
        if (string.IsNullOrWhiteSpace(sessionId))
        {
            return null;
        }

        var row = Query(@"
            SELECT Id
            FROM ConversationState
            WHERE SessionId = @SessionId
            ORDER BY IsArchived ASC, UpdatedAt DESC
            LIMIT 1",
            new SqliteParameter("@SessionId", sessionId)).FirstOrDefault();
        return row?["Id"] as string;
    }

    /// <summary>How many compaction summaries each conversation has, for the ones that have any.</summary>
    public Dictionary<string, int> LoadConversationSummaryCounts()
    {
        var counts = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        foreach (var row in Query("SELECT StateId, COUNT(*) AS Chapters FROM ConversationSummaries GROUP BY StateId"))
        {
            if (row["StateId"] is string stateId)
            {
                counts[stateId] = Convert.ToInt32(row["Chapters"] ?? 0);
            }
        }

        return counts;
    }

    /// <summary>Every distinct compaction summary recorded for a conversation, oldest first.</summary>
    public List<Dictionary<string, object?>> LoadConversationSummaries(string stateId)
    {
        if (string.IsNullOrWhiteSpace(stateId))
        {
            return new List<Dictionary<string, object?>>();
        }

        var rows = Query(@"
            SELECT Id, SummaryIndex, Content, CreatedAt
            FROM ConversationSummaries
            WHERE StateId = @StateId
            ORDER BY SummaryIndex ASC",
            new SqliteParameter("@StateId", stateId));
        AutomatonDebug.Log($"ConfigDatabase.LoadConversationSummaries: stateId={stateId}, rows={rows.Count}");
        return rows;
    }

    /// <summary>
    /// The conversation as search and the transcript viewer see it: every raw message, then
    /// every compaction summary, each row carrying an <c>Origin</c> of
    /// <see cref="Search.SearchOrigins.Raw"/> or <see cref="Search.SearchOrigins.Compressed"/>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The indexer and the viewer must agree on this list and its order, because a vector chunk
    /// records the position of the message it was cut from and the viewer scrolls to that
    /// position. That is why there is one loader rather than one each.
    /// </para>
    /// <para>
    /// A conversation saved before the full table existed has only its live rows. Those are
    /// used instead, with the summary split out of the system message it was merged into, so
    /// an old conversation is presented the same way as a new one - the only difference is
    /// that whatever compaction had already dropped is gone.
    /// </para>
    /// </remarks>
    public List<Dictionary<string, object?>> LoadFullTranscript(string stateId)
    {
        if (string.IsNullOrWhiteSpace(stateId))
        {
            AutomatonDebug.LogWarning("ConfigDatabase.LoadFullTranscript: Exit - blank stateId");
            return new List<Dictionary<string, object?>>();
        }

        AutomatonDebug.Log($"ConfigDatabase.LoadFullTranscript: Entry - stateId={stateId}");
        var transcript = new List<Dictionary<string, object?>>();
        var summaries = new List<string>();

        var full = LoadFullConversationMessages(stateId);
        var usedLiveRows = full.Count == 0;
        var source = usedLiveRows ? LoadConversationMessages(stateId) : full;

        foreach (var row in source)
        {
            var role = row.TryGetValue("Role", out var roleValue) ? roleValue as string ?? string.Empty : string.Empty;
            var content = row.TryGetValue("Content", out var contentValue) ? contentValue as string : null;

            if (usedLiveRows && string.Equals(role, "system", StringComparison.OrdinalIgnoreCase))
            {
                var summary = CompressionSupport.ExtractSummary(content);
                if (summary != null)
                {
                    summaries.Add(summary);
                    content = CompressionSupport.StripSummary(content);
                    if (string.IsNullOrWhiteSpace(content))
                    {
                        // The whole message was the summary; there is no raw row to show.
                        continue;
                    }
                }
            }

            transcript.Add(new Dictionary<string, object?>(StringComparer.Ordinal)
            {
                ["MessageId"] = row.TryGetValue("MessageId", out var idValue) ? idValue as string ?? string.Empty : string.Empty,
                ["Role"] = role,
                ["Content"] = content ?? string.Empty,
                ["Parts"] = row.TryGetValue("Parts", out var partsValue) ? partsValue as string : null,
                ["Attachments"] = row.TryGetValue("Attachments", out var attachmentsValue) ? attachmentsValue as string : null,
                ["Origin"] = Search.SearchOrigins.Raw
            });
        }

        var recorded = LoadConversationSummaries(stateId);
        if (recorded.Count > 0)
        {
            // Recorded summaries are the history; the one still merged into a legacy system
            // prompt is at best the latest of them and is already there.
            summaries.Clear();
            foreach (var row in recorded)
            {
                transcript.Add(new Dictionary<string, object?>(StringComparer.Ordinal)
                {
                    ["MessageId"] = row["Id"] as string ?? string.Empty,
                    ["Role"] = "system",
                    ["Content"] = row["Content"] as string ?? string.Empty,
                    ["Parts"] = null,
                    ["Attachments"] = null,
                    ["Origin"] = Search.SearchOrigins.Compressed
                });
            }
        }

        foreach (var summary in summaries)
        {
            transcript.Add(new Dictionary<string, object?>(StringComparer.Ordinal)
            {
                ["MessageId"] = $"legacy-summary/{stateId}/{transcript.Count}",
                ["Role"] = "system",
                ["Content"] = summary,
                ["Parts"] = null,
                ["Attachments"] = null,
                ["Origin"] = Search.SearchOrigins.Compressed
            });
        }

        var compressedCount = transcript.Count(r => Equals(r["Origin"], Search.SearchOrigins.Compressed));
        AutomatonDebug.Log($"ConfigDatabase.LoadFullTranscript: Exit - stateId={stateId}, raw={transcript.Count - compressedCount}, compressed={compressedCount}, fromLiveRows={usedLiveRows}");
        return transcript;
    }

    /// <summary>Rows the normalized projection holds for a conversation.</summary>
    public int CountConversationMessages(string stateId)
    {
        var rows = Query("SELECT COUNT(*) AS Count FROM ConversationMessages WHERE StateId = @StateId",
            new SqliteParameter("@StateId", stateId));

        return rows.Count > 0 && rows[0]["Count"] is { } value ? Convert.ToInt32(value) : 0;
    }

    /// <summary>
    /// Messages in the authoritative JSON blob, or -1 when that cannot be determined.
    /// Counted in SQLite so a 700KB conversation is not deserialized just to be measured.
    /// </summary>
    public int CountStateBlobMessages(string stateId)
    {
        try
        {
            var rows = Query(
                "SELECT COALESCE(json_array_length(json_extract(State, '$.messages')), json_array_length(json_extract(State, '$.Messages')), -1) AS Count " +
                "FROM ConversationState WHERE Id = @Id",
                new SqliteParameter("@Id", stateId));

            return rows.Count > 0 && rows[0]["Count"] is { } value ? Convert.ToInt32(value) : -1;
        }
        catch (Exception ex)
        {
            // No JSON1 extension, or a blob that is not an object. Unknown, not zero: the
            // callers treat -1 as "no opinion" and leave existing behaviour alone.
            AutomatonDebug.LogWarning($"ConfigDatabase.CountStateBlobMessages: could not count messages for {stateId} - {ex.Message}");
            return -1;
        }
    }

    public List<Dictionary<string, object?>> LoadConversationMessages(string stateId)
    {
        var results = Query(@"
            SELECT MessageId, Role, Content, Parts, Attachments
            FROM ConversationMessages
            WHERE StateId = @StateId
            ORDER BY MessageIndex ASC",
            new SqliteParameter("@StateId", stateId));

        // One line per call. The per-row dump this replaced was half of everything the desktop
        // logged while the search index rebuilt, and it was left over from a provider
        // investigation that is long closed.
        var partsChars = 0L;
        var withParts = 0;
        foreach (var row in results)
        {
            var parts = row.TryGetValue("Parts", out var value) ? value as string : null;
            if (string.IsNullOrEmpty(parts))
            {
                continue;
            }

            withParts++;
            partsChars += parts.Length;
        }

        AutomatonDebug.Log($"ConfigDatabase.LoadConversationMessages: stateId={stateId}, rows={results.Count}, rowsWithParts={withParts}, partsChars={partsChars}");
        return results;
    }

    /// <summary>
    /// Loads conversation metadata from the normalized columns.
    /// </summary>
    public Dictionary<string, object?>? LoadConversationStateMetadata(string stateId)
    {
        return Query(@"
            SELECT Id, ProjectId, ThreadId, SessionId, Agent, Directory, Worktree,
                   UseRolesEnabled, ShowThinkingEnabled, ShowToolCallsEnabled,
                   CollapseThinkingEnabled, CollapseToolCallsEnabled,
                   SelectedProviderKey, SelectedModelId, ReasoningEffort, Verbosity, CompressionSettings, Revision,
                   FullPermissionMode, UpdatedAt
            FROM ConversationState
            WHERE Id = @Id",
            new SqliteParameter("@Id", stateId)).FirstOrDefault();
    }

    /// <summary>
    /// Migrates existing JSON-based conversation state to the normalized schema.
    /// </summary>
    public void MigrateConversationState(string stateId)
    {
        var row = Query("SELECT State FROM ConversationState WHERE Id = @Id",
            new SqliteParameter("@Id", stateId)).FirstOrDefault();

        if (row == null || row["State"] is not string json || string.IsNullOrWhiteSpace(json))
        {
            return;
        }

        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(json);
            var root = doc.RootElement;

            var sessionId = root.TryGetProperty("sessionId", out var sessionIdProp) ? sessionIdProp.GetString() : null;
            var agent = root.TryGetProperty("agent", out var agentProp) ? agentProp.GetString() : null;
            var directory = root.TryGetProperty("directory", out var dirProp) ? dirProp.GetString() : null;
            var worktree = root.TryGetProperty("worktree", out var workProp) ? workProp.GetString() : null;
            var useRoles = root.TryGetProperty("useRolesEnabled", out var rolesProp) && rolesProp.GetBoolean();
            var showThinking = root.TryGetProperty("showThinkingEnabled", out var thinkProp) && thinkProp.GetBoolean();
            var showTools = root.TryGetProperty("showToolCallsEnabled", out var toolsProp) && toolsProp.GetBoolean();
            var collapseThinking = root.TryGetProperty("collapseThinkingEnabled", out var collapseThinkProp) && collapseThinkProp.GetBoolean();
            var collapseTools = root.TryGetProperty("collapseToolCallsEnabled", out var collapseToolsProp) && collapseToolsProp.GetBoolean();
            var providerKey = root.TryGetProperty("selectedProviderKey", out var provProp) ? provProp.GetString() : null;
            var modelId = root.TryGetProperty("selectedModelId", out var modelProp) ? modelProp.GetString() : null;
            var reasoningEffort = root.TryGetProperty("reasoningEffort", out var effortProp) ? effortProp.GetString() : null;
            var verbosity = root.TryGetProperty("verbosity", out var verbosityProp) && verbosityProp.ValueKind == System.Text.Json.JsonValueKind.String
                ? verbosityProp.GetString()
                : null;
            var compressionSettings = root.TryGetProperty("compressionSettings", out var compProp) ? compProp.GetRawText() : null;
            var revision = root.TryGetProperty("revision", out var revisionProp) && revisionProp.TryGetInt64(out var parsedRevision)
                ? parsedRevision
                : 0L;
            // The JSON blob is where this value survived while there was no column for
            // it, so migrating a legacy row is the one chance to recover it.
            var fullPermissionMode = root.TryGetProperty("fullPermissionMode", out var permProp)
                && permProp.ValueKind == System.Text.Json.JsonValueKind.True;

            var messages = new List<Dictionary<string, object?>>();
            if (root.TryGetProperty("messages", out var msgsProp) && msgsProp.ValueKind == System.Text.Json.JsonValueKind.Array)
            {
                foreach (var msg in msgsProp.EnumerateArray())
                {
                    var msgDict = new Dictionary<string, object?>();
                    foreach (var prop in msg.EnumerateObject())
                    {
                        msgDict[prop.Name] = prop.Value.ValueKind == System.Text.Json.JsonValueKind.String
                            ? prop.Value.GetString()
                            : prop.Value.GetRawText();
                    }
                    messages.Add(msgDict);
                }
            }

            UpdateConversationStateMetadata(
                stateId, sessionId, agent, directory, worktree,
                useRoles, showThinking, showTools, collapseThinking, collapseTools,
                providerKey, modelId, reasoningEffort, compressionSettings, null, null, revision, fullPermissionMode, verbosity);

            if (messages.Count > 0)
            {
                InsertConversationMessages(stateId, messages);
            }

            AutomatonDebug.Log($"ConfigDatabase.MigrateConversationState: Migrated {messages.Count} messages for stateId={stateId}");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConfigDatabase.MigrateConversationState: Failed to migrate stateId={stateId}: {ex.Message}");
        }
    }

    #endregion

    private static string TruncateForLog(string? value, int maxLength = 200)
    {
        if (string.IsNullOrEmpty(value)) return "(empty/null)";
        return value.Length <= maxLength ? value : value[..maxLength] + "...";
    }
}
