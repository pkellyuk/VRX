using System.Text.Json;
using Automaton.Core.Interfaces;

namespace PersonaBot;

public sealed class SessionState
{
    public int Version { get; set; } = 1;
    public string? SnapshotTag { get; set; }
    public int CpuCount { get; set; }
    public int MemoryMb { get; set; }
    public List<Message> History { get; set; } = [];
    public List<Message> PendingInstructions { get; set; } = [];
    public List<TranscriptEntry> Transcript { get; set; } = [];
    public string Draft { get; set; } = "";
    public byte[]? Attachment { get; set; }
    public byte[]? FrozenFrame { get; set; }
    public bool Interrupted { get; set; }
    public bool AiPaused { get; set; }
    /// <summary>Messages still waiting in the chat's queue. Restored held: they never start on their own after reopening.</summary>
    public List<QueuedMessage> Queue { get; set; } = [];
}

public sealed record TranscriptEntry(string Role, string Text, byte[]? Image, DateTimeOffset Time);

/// <summary>
/// The app's session: the open chat plus the VM checkpoint that goes with it. Without a path it lives in
/// PersonaBot's database (the PersonaBotSession row and the open chat), saved in one transaction so the
/// checkpoint and the chat always match. With a path it reads or writes a JSON file (the pre-database
/// format, used by the one-time import and by tests).
/// </summary>
public static class SessionStore
{
    /// <summary>Where the pre-database session file lived; imported once, then renamed session.json.imported.</summary>
    public static string LegacyPath => Path.Combine(Settings.Root, "session.json");

    public static SessionState? Load(string? path = null)
    {
        System.Diagnostics.Trace.WriteLine($"SessionStore.Load enter: {path ?? "database"}");
        if (path == null)
        {
            return LoadFor(ActiveStore());
        }
        if (!File.Exists(path))
        {
            System.Diagnostics.Trace.WriteLine($"SessionStore.Load exit: no file at {path}");
            return null;
        }
        var state = JsonSerializer.Deserialize<SessionState>(File.ReadAllText(path)) ?? throw new IOException("The saved session is empty.");
        System.Diagnostics.Trace.WriteLine($"SessionStore.Load exit: {path}: {state.History.Count} AI messages, {state.Transcript.Count} transcript entries, checkpoint {state.SnapshotTag ?? "none"}");
        return Validate(state);
    }

    public static void Save(SessionState state, string? path = null)
    {
        ArgumentNullException.ThrowIfNull(state);
        System.Diagnostics.Trace.WriteLine($"SessionStore.Save enter: {path ?? "database"}, checkpoint {state.SnapshotTag ?? "none"}");
        if (path == null)
        {
            SaveFor(ActiveStore(), state);
            return;
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var temporary = path + ".tmp";
        using (var stream = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            JsonSerializer.Serialize(stream, state);
            stream.Flush(flushToDisk: true);
        }
        File.Move(temporary, path, overwrite: true);
        System.Diagnostics.Trace.WriteLine($"SessionStore.Save exit: wrote {path}");
    }

    private static SessionState Validate(SessionState state)
    {
        if (state.Version != 1)
        {
            System.Diagnostics.Trace.TraceWarning($"SessionStore.Validate: refused version {state.Version}");
            throw new IOException("This saved session needs a compatible PersonaBot version.");
        }
        if (state.SnapshotTag != null && (!GuestShellText.IsCheckpointTag(state.SnapshotTag) || state.CpuCount is < 1 or > 256 || state.MemoryMb is < 1024 or > 131072))
        {
            System.Diagnostics.Trace.TraceWarning($"SessionStore.Validate: refused checkpoint {state.SnapshotTag}: {state.CpuCount} CPUs, {state.MemoryMb} MB");
            throw new IOException("The saved VM checkpoint configuration is invalid.");
        }
        return state;
    }

    /// <summary>The open chat and its VM checkpoint, or null if the app has never saved a session.</summary>
    public static SessionState? LoadFor(ChatStore store)
    {
        System.Diagnostics.Trace.WriteLine("SessionStore.LoadFromDatabase enter");
        ArgumentNullException.ThrowIfNull(store);
        var row = store.Database.Query("SELECT * FROM PersonaBotSession WHERE ProjectId = @Project AND Saved = 1",
            ChatStore.P("@Project", store.ProjectId)).SingleOrDefault();
        if (row == null)
        {
            System.Diagnostics.Trace.WriteLine("SessionStore.LoadFromDatabase exit: no saved session");
            return null;
        }
        var state = store.Load(store.ActiveId);
        state.SnapshotTag = row["SnapshotTag"] as string;
        state.CpuCount = Convert.ToInt32(row["CpuCount"]);
        state.MemoryMb = Convert.ToInt32(row["MemoryMb"]);
        state.FrozenFrame = row["FrozenFrame"] as byte[];
        System.Diagnostics.Trace.WriteLine($"SessionStore.LoadFromDatabase exit: chat={store.ActiveId}, checkpoint={state.SnapshotTag ?? "none"}");
        return Validate(state);
    }

    /// <summary>Saves the open chat's content and the session row (checkpoint) in one transaction.</summary>
    public static void SaveFor(ChatStore store, SessionState state)
    {
        System.Diagnostics.Trace.WriteLine($"SessionStore.SaveToDatabase enter: checkpoint={state.SnapshotTag ?? "none"}");
        ArgumentNullException.ThrowIfNull(store);
        ArgumentNullException.ThrowIfNull(state);
        var active = store.Find(store.ActiveId);
        store.Database.ExecuteInTransaction((_, _) =>
        {
            store.SaveContent(active, state);
            WriteSessionRow(store, state);
        });
        System.Diagnostics.Trace.WriteLine("SessionStore.SaveToDatabase exit");
    }

    /// <summary>
    /// The open persona's chats: the session belongs to whichever persona was showing, so the open chat is
    /// saved to, and reopened from, that persona's project.
    /// </summary>
    private static ChatStore ActiveStore()
    {
        var persona = new PersonaStore().Active;
        System.Diagnostics.Trace.WriteLine($"SessionStore.ActiveStore: persona {persona.Id}, project {persona.ProjectId}");
        return new ChatStore(persona.ProjectId, projectName: persona.Name);
    }

    /// <summary>Writes the checkpoint fields of the session row; must run inside a database transaction.</summary>
    internal static void WriteSessionRow(ChatStore store, SessionState state)
    {
        ArgumentNullException.ThrowIfNull(store);
        ArgumentNullException.ThrowIfNull(state);
        System.Diagnostics.Trace.WriteLine($"SessionStore.WriteSessionRow: project {store.ProjectId}, checkpoint {state.SnapshotTag ?? "none"}, {state.CpuCount} CPUs, {state.MemoryMb} MB, frozen frame {state.FrozenFrame?.Length ?? 0} bytes");
        store.Database.Execute(@"UPDATE PersonaBotSession SET Saved = 1, SnapshotTag = @Tag, CpuCount = @Cpu, MemoryMb = @Memory,
                FrozenFrame = @Frame, UpdatedAt = @Now WHERE ProjectId = @Project",
            ChatStore.P("@Tag", state.SnapshotTag), ChatStore.P("@Cpu", state.CpuCount), ChatStore.P("@Memory", state.MemoryMb),
            ChatStore.P("@Frame", state.FrozenFrame), ChatStore.P("@Now", DateTime.UtcNow.ToString("O")), ChatStore.P("@Project", store.ProjectId));
    }
}
