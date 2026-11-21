using System.Diagnostics;
using System.Text.Json;

namespace PersonaBot;

/// <summary>
/// One-time import of the JSON chat files PersonaBot used before it moved to Automaton's database:
/// Chats\index.json and Chats\&lt;id&gt;.json (one per chat), plus session.json (the open chat and the VM
/// checkpoint). Runs inside the database transaction that creates the PersonaBot project, so a failed
/// import leaves no half-imported chats. Afterwards the files are kept, renamed as a backup
/// (Chats.imported, session.json.imported).
/// </summary>
internal static class JsonChatImport
{
    private sealed class LegacyIndex
    {
        public string? ActiveId { get; set; }
        public List<SavedChat> Chats { get; set; } = [];
    }

    /// <summary>Imports the JSON chats into the store's database. Must run inside the caller's transaction. True if anything was imported.</summary>
    public static bool Import(ChatStore store)
    {
        ArgumentNullException.ThrowIfNull(store);
        var root = Settings.Root;
        var chatsDirectory = Path.Combine(root, "Chats");
        var indexPath = Path.Combine(chatsDirectory, "index.json");
        var sessionPath = Path.Combine(root, "session.json");
        Trace.WriteLine($"JsonChatImport.Import enter: index={File.Exists(indexPath)}, session={File.Exists(sessionPath)}");
        if (!File.Exists(indexPath) && !File.Exists(sessionPath))
        {
            Trace.WriteLine("JsonChatImport.Import exit: nothing to import");
            return false;
        }
        var index = File.Exists(indexPath) ? JsonSerializer.Deserialize<LegacyIndex>(File.ReadAllText(indexPath)) ?? new() : new();
        var session = SessionStore.Load(sessionPath);
        var db = store.Database;
        var imported = 0;
        db.ExecuteInTransaction((_, _) =>
        {
            foreach (var chat in index.Chats.Where(c => Guid.TryParseExact(c.Id, "N", out _)))
            {
                var file = Path.Combine(chatsDirectory, chat.Id + ".json");
                var content = SessionStore.Load(file) ?? new SessionState();
                AddChat(store, chat, content);
                imported++;
            }
            if (imported == 0 && session != null)
            {
                // A pre-multichat install: its one conversation lived in session.json alone.
                var only = new SavedChat();
                AddChat(store, only, session);
                index.ActiveId = only.Id;
                imported++;
            }
            // The chat that was open: the index's choice, else its first chat (or the single legacy chat).
            var active = index.Chats.Any(c => c.Id == index.ActiveId) ? index.ActiveId : index.Chats.FirstOrDefault()?.Id ?? index.ActiveId;
            if (active != null)
            {
                store.SetActive(active);
                if (session != null)
                {
                    // session.json was written with the open chat when the app last closed: that copy
                    // is the newest, and its VM checkpoint belongs with it.
                    store.SaveContent(store.Find(active), session);
                    SessionStore.WriteSessionRow(store, session);
                }
            }
        });
        Trace.WriteLine($"JsonChatImport.Import exit: {imported} chats imported, checkpoint={session?.SnapshotTag ?? "none"}");
        return imported > 0;
    }

    /// <summary>After the import has committed: keeps the JSON files as a backup under new names.</summary>
    public static void BackUpImportedFiles()
    {
        var chatsDirectory = Path.Combine(Settings.Root, "Chats");
        var sessionPath = Path.Combine(Settings.Root, "session.json");
        Trace.WriteLine($"JsonChatImport.BackUpImportedFiles enter: {Settings.Root}");
        Backup(chatsDirectory, chatsDirectory + ".imported");
        Backup(sessionPath, sessionPath + ".imported");
        Trace.WriteLine("JsonChatImport.BackUpImportedFiles exit");
    }

    private static void AddChat(ChatStore store, SavedChat chat, SessionState content)
    {
        var db = store.Database;
        var updated = chat.Updated.UtcDateTime;
        Trace.WriteLine($"JsonChatImport.AddChat: {chat.Id} ({content.History.Count} AI messages, {content.Transcript.Count} transcript entries), updated {updated:O}");
        db.SaveThread(chat.Id, store.ProjectId, chat.Title, store.StateId(chat.Id), false, updated, updated);
        db.Execute("INSERT INTO PersonaBotChats (ThreadId, ProjectId, Pinned, Renamed, UpdatedAt) VALUES (@Id, @Project, @Pinned, @Renamed, @Now)",
            ChatStore.P("@Id", chat.Id), ChatStore.P("@Project", store.ProjectId), ChatStore.P("@Pinned", chat.Pinned ? 1 : 0),
            ChatStore.P("@Renamed", chat.Renamed ? 1 : 0), ChatStore.P("@Now", updated.ToString("O")));
        store.SaveContent(store.Find(chat.Id), content);
        // Keep the chat list's order: saving stamps "now", the import restores the original time.
        db.Execute("UPDATE Threads SET LastModifiedAt = @Updated WHERE Id = @Id", ChatStore.P("@Updated", updated.ToString("O")), ChatStore.P("@Id", chat.Id));
    }

    private static void Backup(string path, string backup)
    {
        try
        {
            if (Directory.Exists(path) && !Directory.Exists(backup))
            {
                Directory.Move(path, backup);
            }
            else if (File.Exists(path) && !File.Exists(backup))
            {
                File.Move(path, backup);
            }
            Trace.WriteLine($"JsonChatImport.Backup: {path} -> {backup}");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // The data is already in the database; a file that couldn't be renamed is only a stale copy.
            Trace.WriteLine($"JsonChatImport.Backup: could not rename {path}: {ex.Message}");
        }
    }
}
