using System.Diagnostics;
using System.Text.Json;
using Automaton.Core.Interfaces;
using Automaton.Core.Models;
using Automaton.Core.Services;
using Microsoft.Data.Sqlite;

namespace PersonaBot;

public sealed class SavedChat
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Title { get; set; } = "New chat";
    public bool Pinned { get; set; }
    public bool Renamed { get; set; }
    public DateTimeOffset Updated { get; set; } = DateTimeOffset.UtcNow;
}

/// <summary>
/// PersonaBot's chats, in Automaton's database and Automaton's shape: the app is one Automaton
/// project ("personabot"), each chat is an Automaton thread, and each chat's AI history is an
/// Automaton conversation saved through <see cref="ConversationPersistenceService"/>
/// (ConversationState and ConversationMessages). What only PersonaBot has (the chat display with
/// screenshots, the draft and attachment, the queue, pins, pending guidance) is in PersonaBot's own
/// tables beside them, written in the same transaction. Conversations share one desktop; a chat
/// never holds a VM checkpoint (see <see cref="SessionStore"/>).
/// </summary>
public sealed class ChatStore
{
    public const string DefaultProjectId = "personabot";
    public const string Agent = "personabot";
    private static readonly JsonSerializerOptions Json = new();
    private readonly ConfigDatabase _db;
    private readonly string _projectId;
    private readonly string? _projectName;

    /// <param name="projectId">The Automaton project that holds these chats: a persona's project (PersonaStore); tests use their own.</param>
    /// <param name="legacy">A pre-multichat session to turn into the first chat when the project has none.</param>
    /// <param name="projectName">The project's display name when it has to be created (the persona's name).</param>
    public ChatStore(string? projectId = null, SessionState? legacy = null, string? projectName = null)
    {
        _projectId = string.IsNullOrWhiteSpace(projectId) ? DefaultProjectId : projectId.Trim();
        _projectName = string.IsNullOrWhiteSpace(projectName) ? null : projectName.Trim();
        Trace.WriteLine($"ChatStore ctor enter: project={_projectId}");
        _db = PersonaBotDatabase.EnsureReady();
        // Creating the project and importing the old JSON chats commit together: a failed import
        // leaves no project row, so it runs again next time instead of being skipped.
        var fresh = false;
        var imported = false;
        _db.ExecuteInTransaction((_, _) =>
        {
            fresh = EnsureProject();
            if (fresh && _projectId == DefaultProjectId)
            {
                imported = JsonChatImport.Import(this);
            }
        });
        Trace.WriteLine($"ChatStore ctor: project {_projectId} fresh={fresh}, imported={imported}");
        if (imported)
        {
            JsonChatImport.BackUpImportedFiles();
        }
        if (Chats.Count == 0)
        {
            Trace.WriteLine($"ChatStore ctor: no chats yet, creating one{(fresh && legacy != null ? " from the legacy session" : "")}");
            Create(fresh ? legacy : null);
        }
        if (!Chats.Any(chat => chat.Id == ActiveId))
        {
            Trace.WriteLine("ChatStore ctor: the active chat is gone, activating the first");
            Activate(Chats[0].Id);
        }
        Trace.WriteLine($"ChatStore ctor exit: {Chats.Count} chats, active={ActiveId}");
    }

    public string ProjectId => _projectId;

    /// <summary>Pinned first, then most recently updated.</summary>
    public IReadOnlyList<SavedChat> Chats => _db.Query(@"
            SELECT t.Id, t.Name, t.LastModifiedAt, c.Pinned, c.Renamed
            FROM Threads t JOIN PersonaBotChats c ON c.ThreadId = t.Id
            WHERE t.ProjectId = @Project AND COALESCE(t.IsArchived, 0) = 0
            ORDER BY c.Pinned DESC, t.LastModifiedAt DESC",
            P("@Project", _projectId))
        .Select(row => new SavedChat
        {
            Id = (string)row["Id"]!, Title = (string)row["Name"]!, Pinned = Convert.ToInt64(row["Pinned"]) != 0,
            Renamed = Convert.ToInt64(row["Renamed"]) != 0, Updated = DateTimeOffset.Parse((string)row["LastModifiedAt"]!)
        })
        .ToList();

    public string ActiveId => _db.ExecuteScalar<string>("SELECT ActiveThreadId FROM PersonaBotSession WHERE ProjectId = @Project", P("@Project", _projectId))
        ?? throw new InvalidOperationException("No chat is active.");

    /// <summary>Loads a chat's content (no VM checkpoint).</summary>
    public SessionState Load(string id)
    {
        Find(id);
        Trace.WriteLine($"ChatStore.Load enter: {id}");
        var state = new SessionState();
        var conversation = new ConversationPersistenceService(StateId(id), _projectId).LoadStateAsync().GetAwaiter().GetResult();
        state.History = conversation?.Messages?.ToList() ?? [];
        var chat = _db.Query("SELECT * FROM PersonaBotChats WHERE ThreadId = @Id", P("@Id", id)).Single();
        state.Draft = (string)chat["Draft"]!;
        state.Attachment = chat["Attachment"] as byte[];
        state.AiPaused = Convert.ToInt64(chat["AiPaused"]) != 0;
        state.Interrupted = Convert.ToInt64(chat["Interrupted"]) != 0;
        state.PendingInstructions = chat["PendingInstructions"] is string pending && pending.Length > 0
            ? JsonSerializer.Deserialize<List<Message>>(pending, Json) ?? [] : [];
        state.Transcript = _db.Query("SELECT Role, Text, Image, Time FROM PersonaBotTranscript WHERE ThreadId = @Id ORDER BY Position", P("@Id", id))
            .Select(row => new TranscriptEntry((string)row["Role"]!, (string)row["Text"]!, row["Image"] as byte[], DateTimeOffset.Parse((string)row["Time"]!)))
            .ToList();
        state.Queue = _db.Query("SELECT Id, Text, Image, Time, Files FROM PersonaBotQueue WHERE ThreadId = @Id ORDER BY Position", P("@Id", id))
            .Select(row => new QueuedMessage((string)row["Id"]!, (string)row["Text"]!, row["Image"] as byte[], DateTimeOffset.Parse((string)row["Time"]!),
                row["Files"] is string files && files.Length > 0 ? JsonSerializer.Deserialize<List<ChatAttachment>>(files, Json) : null))
            .ToList();
        Trace.WriteLine($"ChatStore.Load exit: {state.History.Count} AI messages, {state.Transcript.Count} transcript entries, {state.Queue.Count} queued");
        return state;
    }

    public SavedChat Create(SessionState? legacy = null)
    {
        Trace.WriteLine("ChatStore.Create enter");
        var chat = new SavedChat();
        var now = DateTime.UtcNow;
        _db.ExecuteInTransaction((_, _) =>
        {
            _db.SaveThread(chat.Id, _projectId, chat.Title, StateId(chat.Id), false, now, now);
            _db.Execute("INSERT INTO PersonaBotChats (ThreadId, ProjectId, UpdatedAt) VALUES (@Id, @Project, @Now)",
                P("@Id", chat.Id), P("@Project", _projectId), P("@Now", now.ToString("O")));
            SetActive(chat.Id);
        });
        Save(chat.Id, legacy ?? new());
        Trace.WriteLine($"ChatStore.Create exit: {chat.Id}");
        return chat;
    }

    /// <summary>Saves a chat's content. Any VM checkpoint on <paramref name="state"/> is ignored here.</summary>
    public void Save(string id, SessionState state)
    {
        ArgumentNullException.ThrowIfNull(state);
        Trace.WriteLine($"ChatStore.Save enter: {id}");
        var chat = Find(id);
        _db.ExecuteInTransaction((_, _) => SaveContent(chat, state));
        Trace.WriteLine($"ChatStore.Save exit: {id}");
    }

    /// <summary>The body of <see cref="Save"/>; must run inside a database transaction.</summary>
    internal void SaveContent(SavedChat chat, SessionState state)
    {
        ArgumentNullException.ThrowIfNull(chat);
        ArgumentNullException.ThrowIfNull(state);
        Trace.WriteLine($"ChatStore.SaveContent enter: {chat.Id}, {state.History.Count} AI messages, {state.Transcript.Count} transcript entries");
        var now = DateTime.UtcNow;
        var first = state.Transcript.FirstOrDefault(e => e.Role == "You")?.Text;
        if (!chat.Renamed && !string.IsNullOrWhiteSpace(first))
        {
            chat.Title = first.Replace('\r', ' ').Replace('\n', ' ')[..Math.Min(first.Length, 70)];
        }
        // Automaton keys each stored message by its Id (ConversationMessages.MessageId, unique per chat).
        // Automaton's own app always sets one; PersonaBot's provider replies and tool results may not, so
        // give those a stable Id now: it sticks to the message object, and later saves reuse it.
        foreach (var message in state.History.Where(m => string.IsNullOrWhiteSpace(m.Id)))
        {
            message.Id = Guid.NewGuid().ToString();
        }
        // The AI history, as an Automaton conversation. Saving joins this transaction.
        var conversation = new ConversationState
        {
            SessionId = chat.Id, Agent = Agent, DisplayName = chat.Title, Messages = state.History.ToList()
        };
        if (!new ConversationPersistenceService(StateId(chat.Id), _projectId).SaveStateAsync(conversation).GetAwaiter().GetResult())
        {
            Trace.TraceError($"ChatStore.SaveContent: the conversation of {chat.Id} ({state.History.Count} messages) could not be saved");
            throw new IOException("The chat's conversation could not be saved to the database.");
        }
        _db.Execute("UPDATE Threads SET Name = @Name, LastModifiedAt = @Now WHERE Id = @Id",
            P("@Name", chat.Title), P("@Now", now.ToString("O")), P("@Id", chat.Id));
        _db.Execute(@"UPDATE PersonaBotChats SET Draft = @Draft, Attachment = @Attachment, AiPaused = @Paused, Interrupted = @Interrupted,
                PendingInstructions = @Pending, UpdatedAt = @Now WHERE ThreadId = @Id",
            P("@Draft", state.Draft ?? ""), P("@Attachment", state.Attachment), P("@Paused", state.AiPaused ? 1 : 0),
            P("@Interrupted", state.Interrupted ? 1 : 0), P("@Pending", JsonSerializer.Serialize(state.PendingInstructions, Json)),
            P("@Now", now.ToString("O")), P("@Id", chat.Id));
        SaveTranscript(chat.Id, state.Transcript);
        _db.Execute("DELETE FROM PersonaBotQueue WHERE ThreadId = @Id", P("@Id", chat.Id));
        var position = 0;
        foreach (var item in state.Queue)
        {
            _db.Execute("INSERT INTO PersonaBotQueue (ThreadId, Position, Id, Text, Image, Time, Files) VALUES (@Thread, @Position, @Id, @Text, @Image, @Time, @Files)",
                P("@Thread", chat.Id), P("@Position", position++), P("@Id", item.Id), P("@Text", item.Text), P("@Image", item.Image), P("@Time", item.Time.ToString("O")),
                P("@Files", item.Files is { Count: > 0 } ? JsonSerializer.Serialize(item.Files, Json) : null));
        }
        chat.Updated = now;
        Trace.WriteLine($"ChatStore.SaveContent exit: {chat.Id}, {position} queued, renamed={chat.Renamed}");
    }

    /// <summary>
    /// The transcript only grows, so only new entries are written (screenshots are stored once).
    /// If the stored entries no longer match the start of the new transcript, it is rewritten.
    /// </summary>
    private void SaveTranscript(string id, IReadOnlyList<TranscriptEntry> transcript)
    {
        var stored = Convert.ToInt32(_db.ExecuteScalar<long>("SELECT COUNT(*) FROM PersonaBotTranscript WHERE ThreadId = @Id", P("@Id", id)));
        var keep = stored;
        if (stored > transcript.Count)
        {
            keep = 0;
        }
        else if (stored > 0)
        {
            var last = _db.Query("SELECT Role, Text, Time FROM PersonaBotTranscript WHERE ThreadId = @Id AND Position = @Position",
                P("@Id", id), P("@Position", stored - 1)).SingleOrDefault();
            var expected = transcript[stored - 1];
            if (last == null || (string)last["Role"]! != expected.Role || (string)last["Text"]! != expected.Text
                || (string)last["Time"]! != expected.Time.ToString("O"))
            {
                keep = 0;
            }
        }
        if (keep == 0 && stored > 0)
        {
            Trace.WriteLine($"ChatStore.SaveTranscript: {id}'s {stored} stored entries no longer match the {transcript.Count} new ones: rewriting");
            _db.Execute("DELETE FROM PersonaBotTranscript WHERE ThreadId = @Id", P("@Id", id));
        }
        for (var position = keep; position < transcript.Count; position++)
        {
            var entry = transcript[position];
            _db.Execute("INSERT INTO PersonaBotTranscript (ThreadId, Position, Role, Text, Image, Time) VALUES (@Id, @Position, @Role, @Text, @Image, @Time)",
                P("@Id", id), P("@Position", position), P("@Role", entry.Role), P("@Text", entry.Text), P("@Image", entry.Image), P("@Time", entry.Time.ToString("O")));
        }
        Trace.WriteLine($"ChatStore.SaveTranscript: kept {keep}, wrote {transcript.Count - keep}");
    }

    public void Activate(string id)
    {
        Trace.WriteLine($"ChatStore.Activate enter: {id} in {_projectId}");
        Find(id);
        _db.ExecuteInTransaction((_, _) => SetActive(id));
        Trace.WriteLine($"ChatStore.Activate exit: {id}");
    }

    public void Rename(string id, string title)
    {
        if (string.IsNullOrWhiteSpace(title))
        {
            Trace.TraceWarning($"ChatStore.Rename: refused an empty name for {id}");
            throw new ArgumentException("Enter a chat name.");
        }
        Trace.WriteLine($"ChatStore.Rename enter: {id}, {title.Trim().Length} chars");
        Find(id);
        _db.ExecuteInTransaction((_, _) =>
        {
            _db.RenameThread(id, title.Trim());
            _db.Execute("UPDATE PersonaBotChats SET Renamed = 1 WHERE ThreadId = @Id", P("@Id", id));
        });
        Trace.WriteLine($"ChatStore.Rename exit: {id}");
    }

    public void Pin(string id, bool pinned)
    {
        Trace.WriteLine($"ChatStore.Pin: {id} pinned={pinned}");
        Find(id);
        _db.Execute("UPDATE PersonaBotChats SET Pinned = @Pinned WHERE ThreadId = @Id", P("@Pinned", pinned ? 1 : 0), P("@Id", id));
    }

    /// <summary>
    /// Empties a chat, keeping the chat itself: its messages, the model's history (and its compaction archives),
    /// queued messages and paused/interrupted state go. A name the person gave it and its pin stay; an automatic
    /// name goes back to "New chat". Whatever is typed in its message box, and its attachment, are kept.
    /// </summary>
    public void Clear(string id)
    {
        var chat = Find(id);
        Trace.WriteLine($"ChatStore.Clear enter: {id}");
        var row = _db.Query("SELECT Draft, Attachment FROM PersonaBotChats WHERE ThreadId = @Id", P("@Id", id)).Single();
        var kept = new SessionState { Draft = (string)row["Draft"]!, Attachment = row["Attachment"] as byte[] };
        if (!chat.Renamed)
        {
            chat.Title = new SavedChat().Title;
        }
        _db.ExecuteInTransaction((_, _) =>
        {
            new ConversationPersistenceService(StateId(id), _projectId).DeleteStateAndArchives();
            SaveContent(chat, kept);
        });
        Trace.WriteLine($"ChatStore.Clear exit: {id} is now \"{chat.Title}\"");
    }

    public void Delete(string id)
    {
        Find(id);
        Trace.WriteLine($"ChatStore.Delete enter: {id}");
        _db.ExecuteInTransaction((_, _) => DeleteContent(id));
        var remaining = Chats;
        if (remaining.Count == 0)
        {
            Trace.WriteLine("ChatStore.Delete: that was the last chat, creating a new one");
            Create();
        }
        else if (CurrentActiveOrNull() == id)
        {
            Trace.WriteLine($"ChatStore.Delete: {id} was active, activating {remaining[0].Id}");
            Activate(remaining[0].Id);
        }
        Trace.WriteLine($"ChatStore.Delete exit: {remaining.Count} chats remain");
    }

    /// <summary>Removes a chat and its conversation; must run inside a database transaction.</summary>
    internal void DeleteContent(string id)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(id);
        Trace.WriteLine($"ChatStore.DeleteContent: {id}");
        new ConversationPersistenceService(StateId(id), _projectId).DeleteStateAndArchives();
        _db.Execute("DELETE FROM PersonaBotTranscript WHERE ThreadId = @Id", P("@Id", id));
        _db.Execute("DELETE FROM PersonaBotQueue WHERE ThreadId = @Id", P("@Id", id));
        _db.Execute("DELETE FROM PersonaBotChats WHERE ThreadId = @Id", P("@Id", id));
        _db.DeleteThread(id);
    }

    /// <summary>The Automaton conversation id of a chat: "{project}/{chat}".</summary>
    public string StateId(string id) => _projectId + "/" + id;

    internal SavedChat Find(string id)
    {
        if (string.IsNullOrWhiteSpace(id) || !Guid.TryParseExact(id, "N", out _))
        {
            Trace.TraceWarning($"ChatStore.Find: invalid chat id '{id}' in {_projectId}");
            throw new ArgumentException("Invalid chat identifier.");
        }
        if (Chats.FirstOrDefault(c => c.Id == id) is { } found) return found;
        Trace.TraceWarning($"ChatStore.Find: chat {id} does not exist in {_projectId}");
        throw new ArgumentException("Chat does not exist.");
    }

    internal string? CurrentActiveOrNull() =>
        _db.ExecuteScalar<string>("SELECT ActiveThreadId FROM PersonaBotSession WHERE ProjectId = @Project", P("@Project", _projectId));

    /// <summary>Points the session at a chat; must run inside a database transaction.</summary>
    internal void SetActive(string id)
    {
        Trace.WriteLine($"ChatStore.SetActive: {id} in {_projectId}");
        _db.Execute(@"INSERT INTO PersonaBotSession (ProjectId, ActiveThreadId, UpdatedAt) VALUES (@Project, @Id, @Now)
                ON CONFLICT(ProjectId) DO UPDATE SET ActiveThreadId = excluded.ActiveThreadId, UpdatedAt = excluded.UpdatedAt",
            P("@Project", _projectId), P("@Id", id), P("@Now", DateTime.UtcNow.ToString("O")));
    }

    /// <summary>Creates the project row if missing. True when it was just created (a fresh database for this project).</summary>
    private bool EnsureProject()
    {
        var exists = _db.ExecuteScalar<long>("SELECT COUNT(*) FROM Projects WHERE Id = @Id", P("@Id", _projectId)) > 0;
        if (exists) return false;
        var now = DateTime.UtcNow.ToString("O");
        _db.Execute("INSERT INTO Projects (Id, Name, Path, CreatedAt, UpdatedAt) VALUES (@Id, @Name, @Path, @Now, @Now)",
            P("@Id", _projectId), P("@Name", _projectName ?? (_projectId == DefaultProjectId ? "Website Tester" : _projectId)), P("@Path", Settings.Root), P("@Now", now));
        Trace.WriteLine($"ChatStore.EnsureProject: created project {_projectId}");
        return true;
    }

    internal ConfigDatabase Database => _db;

    internal static SqliteParameter P(string name, object? value) => new(name, value ?? DBNull.Value);
}
