using System.Diagnostics;
using Automaton.Core.Services;

namespace PersonaBot;

/// <summary>
/// PersonaBot keeps its data in Automaton's SQLite database (<see cref="ConfigDatabase"/>, config.db)
/// so Automaton's infrastructure (conversations, threads, search, and later calendar, goals and the
/// master/slave link) works over PersonaBot's data as it is.
///
/// The database lives in PersonaBot's own data folder (<see cref="Settings.Root"/>), never in the
/// user's real Automaton home (%USERPROFILE%\.automaton): <see cref="ConfigureAutomatonHome"/> points
/// <see cref="AutomatonPaths"/> there before anything opens the database, and <see cref="EnsureReady"/>
/// refuses to continue if the database ended up anywhere else.
///
/// PersonaBot's own tables sit beside Automaton's (prefixed PersonaBot) instead of changing Automaton's
/// schema: the chat display with its screenshots, the draft, the queue, pins, the personas, and the session's VM checkpoint.
/// </summary>
public static class PersonaBotDatabase
{
    public const string DatabaseFileName = "config.db";
    private static readonly object Gate = new();
    private static bool _ready;

    /// <summary>
    /// Points Automaton's data folder at PersonaBot's. Hosts call this at start-up, after the data
    /// folder is final (after any host override) and
    /// before anything else can touch <see cref="AutomatonPaths"/>. Safe to call again with the same folder.
    /// </summary>
    public static void ConfigureAutomatonHome(string? dataDirectory = null)
    {
        var root = Path.GetFullPath(dataDirectory ?? Settings.Root);
        Trace.WriteLine($"PersonaBotDatabase.ConfigureAutomatonHome enter: {root}");
        Directory.CreateDirectory(root);
        AutomatonPaths.ConfigureHomeDirectory(root);
        Trace.WriteLine("PersonaBotDatabase.ConfigureAutomatonHome exit");
    }

    /// <summary>
    /// Opens the database (configuring the home folder if no host did), checks it is PersonaBot's own,
    /// creates PersonaBot's tables and the PersonaBot project row, and imports the old JSON chat files once.
    /// </summary>
    public static ConfigDatabase EnsureReady(string? dataDirectory = null)
    {
        lock (Gate)
        {
            if (_ready)
            {
                return ConfigDatabase.Instance;
            }
            Trace.WriteLine("PersonaBotDatabase.EnsureReady enter");
            ConfigureAutomatonHome(dataDirectory);
            var db = ConfigDatabase.Instance;
            var expected = Path.GetFullPath(Path.Combine(AutomatonPaths.HomeDirectory, DatabaseFileName));
            var userAutomaton = Path.GetFullPath(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), AutomatonPaths.AutomatonDirectoryName));
            if (!string.Equals(Path.GetFullPath(db.DatabasePath), expected, StringComparison.OrdinalIgnoreCase)
                || Path.GetFullPath(db.DatabasePath).StartsWith(userAutomaton + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            {
                Trace.TraceError($"PersonaBotDatabase.EnsureReady: refused {db.DatabasePath}: expected {expected}");
                throw new InvalidOperationException($"PersonaBot's database resolved to '{db.DatabasePath}', not its own data folder. Refusing to use another Automaton installation's data.");
            }
            CreateTables(db);
            _ready = true;
            Trace.WriteLine($"PersonaBotDatabase.EnsureReady exit: {db.DatabasePath}");
            return db;
        }
    }

    private static void CreateTables(ConfigDatabase db)
    {
        ArgumentNullException.ThrowIfNull(db);
        Trace.WriteLine("PersonaBotDatabase.CreateTables enter");
        db.ExecuteInTransaction((_, _) =>
        {
            db.Execute(@"
                CREATE TABLE IF NOT EXISTS PersonaBotChats (
                    ThreadId TEXT PRIMARY KEY,
                    ProjectId TEXT NOT NULL,
                    Pinned INTEGER NOT NULL DEFAULT 0,
                    Renamed INTEGER NOT NULL DEFAULT 0,
                    Draft TEXT NOT NULL DEFAULT '',
                    Attachment BLOB,
                    AiPaused INTEGER NOT NULL DEFAULT 0,
                    Interrupted INTEGER NOT NULL DEFAULT 0,
                    PendingInstructions TEXT,
                    UpdatedAt TEXT NOT NULL
                )");
            db.Execute("CREATE INDEX IF NOT EXISTS idx_personabotchats_project ON PersonaBotChats(ProjectId)");
            db.Execute(@"
                CREATE TABLE IF NOT EXISTS PersonaBotTranscript (
                    ThreadId TEXT NOT NULL,
                    Position INTEGER NOT NULL,
                    Role TEXT NOT NULL,
                    Text TEXT NOT NULL,
                    Image BLOB,
                    Time TEXT NOT NULL,
                    PRIMARY KEY (ThreadId, Position)
                )");
            db.Execute(@"
                CREATE TABLE IF NOT EXISTS PersonaBotQueue (
                    ThreadId TEXT NOT NULL,
                    Position INTEGER NOT NULL,
                    Id TEXT NOT NULL,
                    Text TEXT NOT NULL,
                    Image BLOB,
                    Time TEXT NOT NULL,
                    Files TEXT,
                    PRIMARY KEY (ThreadId, Position)
                )");
            // Databases created before queued messages could carry files.
            if (!db.Query("PRAGMA table_info(PersonaBotQueue)").Any(column => (string?)column["name"] == "Files"))
            {
                Trace.WriteLine("PersonaBotDatabase.CreateTables: adding PersonaBotQueue.Files");
                db.Execute("ALTER TABLE PersonaBotQueue ADD COLUMN Files TEXT");
            }
            db.Execute(@"
                CREATE TABLE IF NOT EXISTS PersonaBotSnapshots (
                    Tag TEXT PRIMARY KEY,
                    Name TEXT NOT NULL,
                    Note TEXT NOT NULL DEFAULT '',
                    CreatedAt TEXT NOT NULL
                )");
            // Personas (PERSONAS.md §3): each names its Automaton project and its Linux user in the guest.
            db.Execute(@"
                CREATE TABLE IF NOT EXISTS PersonaBotPersonas (
                    Id TEXT PRIMARY KEY,
                    ProjectId TEXT NOT NULL UNIQUE,
                    Name TEXT NOT NULL,
                    LinuxUser TEXT UNIQUE,
                    Uid INTEGER NOT NULL,
                    Avatar TEXT NOT NULL,
                    Colour TEXT NOT NULL,
                    Tagline TEXT NOT NULL DEFAULT '',
                    Instructions TEXT NOT NULL DEFAULT '',
                    BuiltIn INTEGER NOT NULL DEFAULT 0,
                    Position INTEGER NOT NULL DEFAULT 0,
                    CreatedAt TEXT NOT NULL
                )");
            // What Automaton's CalendarTasks has no column for: whose booking it is, how long, and whether it needs the desktop.
            db.Execute(@"
                CREATE TABLE IF NOT EXISTS PersonaBotBookings (
                    TaskId TEXT PRIMARY KEY,
                    PersonaId TEXT NOT NULL,
                    DurationMinutes INTEGER NOT NULL,
                    NeedsDesktop INTEGER NOT NULL DEFAULT 1
                )");
            // Machines (DISTROS.md §3): each distribution and architecture installed, with its folder and screen size.
            db.Execute(@"
                CREATE TABLE IF NOT EXISTS PersonaBotMachines (
                    Id TEXT PRIMARY KEY,
                    Name TEXT NOT NULL,
                    Distribution TEXT NOT NULL,
                    Architecture TEXT NOT NULL,
                    Folder TEXT NOT NULL,
                    ScreenWidth INTEGER NOT NULL DEFAULT 1280,
                    ScreenHeight INTEGER NOT NULL DEFAULT 800,
                    CreatedAt TEXT NOT NULL
                )");
            // Databases from before each machine recorded its desktop user (the person's own account on newer machines).
            if (!db.Query("PRAGMA table_info(PersonaBotMachines)").Any(column => (string?)column["name"] == "DesktopUser"))
            {
                Trace.WriteLine("PersonaBotDatabase.CreateTables: adding PersonaBotMachines.DesktopUser");
                db.Execute("ALTER TABLE PersonaBotMachines ADD COLUMN DesktopUser TEXT");
            }
            // Whether a persona has full sudo (otherwise only apt), set in its editor.
            if (!db.Query("PRAGMA table_info(PersonaBotPersonas)").Any(column => (string?)column["name"] == "FullSudo"))
            {
                Trace.WriteLine("PersonaBotDatabase.CreateTables: adding PersonaBotPersonas.FullSudo");
                db.Execute("ALTER TABLE PersonaBotPersonas ADD COLUMN FullSudo INTEGER NOT NULL DEFAULT 0");
            }
            // A persona's own voice (null: the default voice in Settings), set in its editor.
            if (!db.Query("PRAGMA table_info(PersonaBotPersonas)").Any(column => (string?)column["name"] == "Voice"))
            {
                Trace.WriteLine("PersonaBotDatabase.CreateTables: adding PersonaBotPersonas.Voice");
                db.Execute("ALTER TABLE PersonaBotPersonas ADD COLUMN Voice TEXT");
            }
            // Screenshots and recordings on the person's timeline (RECORDINGS.md): who made them and why.
            db.Execute(@"
                CREATE TABLE IF NOT EXISTS PersonaBotRecordings (
                    Id TEXT PRIMARY KEY,
                    Kind TEXT NOT NULL,
                    PersonaId TEXT,
                    ChatId TEXT,
                    MadeBy TEXT NOT NULL,
                    Reason TEXT NOT NULL,
                    Summary TEXT,
                    StartedAt TEXT NOT NULL,
                    EndedAt TEXT,
                    Folder TEXT NOT NULL,
                    FrameCount INTEGER NOT NULL DEFAULT 0
                )");
            db.Execute("CREATE INDEX IF NOT EXISTS IX_PersonaBotRecordings_Persona ON PersonaBotRecordings (PersonaId, StartedAt)");
            db.Execute(@"
                CREATE TABLE IF NOT EXISTS PersonaBotPreferences (
                    Key TEXT PRIMARY KEY,
                    Value TEXT
                )");
            db.Execute(@"
                CREATE TABLE IF NOT EXISTS PersonaBotSession (
                    ProjectId TEXT PRIMARY KEY,
                    ActiveThreadId TEXT,
                    Saved INTEGER NOT NULL DEFAULT 0,
                    SnapshotTag TEXT,
                    CpuCount INTEGER NOT NULL DEFAULT 0,
                    MemoryMb INTEGER NOT NULL DEFAULT 0,
                    FrozenFrame BLOB,
                    UpdatedAt TEXT NOT NULL
                )");
        });
        Trace.WriteLine("PersonaBotDatabase.CreateTables exit");
    }
}
