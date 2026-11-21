using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Threading.Tasks;
using Automaton.Core.Models;

namespace Automaton.Core.Services;

/// <summary>
/// Service for persisting conversation state to the SQLite database.
/// Uses normalized schema: metadata in columns, messages in separate table.
/// </summary>
public class ConversationPersistenceService
{
    private static readonly JsonSerializerOptions MessagePartsJsonOptions = new()
    {
        WriteIndented = false,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    private readonly string _stateId;
    private readonly JsonSerializerOptions _jsonOptions;
    private readonly string? _projectId;

    /// <summary>
    /// Event triggered when state is saved.
    /// </summary>
    public event Action<ConversationState>? OnStateSaved;

    /// <summary>
    /// Event triggered when state is loaded.
    /// </summary>
    public event Action<ConversationState>? OnStateLoaded;

    /// <summary>
    /// Creates a new ConversationPersistenceService instance using the centralized home directory path.
    /// </summary>
    public ConversationPersistenceService()
    {
        AutomatonDebug.Log("ConversationPersistenceService: Default constructor entry");
        _stateId = "desktop_default";
        _projectId = null;
        _jsonOptions = new JsonSerializerOptions
        {
            WriteIndented = false,
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase
        };
        AutomatonDebug.Log($"ConversationPersistenceService: Default constructor exit - stateId={_stateId}");
    }

    /// <summary>
    /// Creates a new ConversationPersistenceService instance for project-specific conversation state.
    /// </summary>
    /// <param name="stateId">Unique identifier for this conversation state. For project threads, use a unique per-thread value like thread.StateFilePath ("projects/{projectId}/{threadId}.json").</param>
    /// <param name="projectId">Optional project ID to associate with the state (e.g., the project folder path).</param>
    public ConversationPersistenceService(string stateId, string? projectId = null)
    {
        AutomatonDebug.Log($"ConversationPersistenceService: Constructor entry - stateId={stateId}, projectId={projectId}");
        _stateId = stateId;
        _projectId = projectId;
        _jsonOptions = new JsonSerializerOptions
        {
            WriteIndented = false,
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase
        };
        AutomatonDebug.Log("ConversationPersistenceService: Constructor exit");
    }

    /// <summary>
    /// Saves the conversation state to the database (legacy full-replace method).
    /// Prefer UpdateStateAsync() for better performance.
    /// </summary>
    /// <param name="state">The conversation state to save.</param>
    /// <returns>True if save was successful, false otherwise.</returns>
    public Task<bool> SaveStateAsync(ConversationState state)
    {
        AutomatonDebug.Log($"ConversationPersistenceService.SaveStateAsync: Entry - stateId={_stateId}");
        return Task.Run(() =>
        {
            try
            {
                var db = ConfigDatabase.Instance;
                var now = DateTime.UtcNow.ToString("O");

                state.Revision = Math.Max(state.Revision, LoadCurrentRevision(db)) + 1;
                state.LastSavedAt = DateTimeOffset.UtcNow;
                state.ProjectId = _projectId;
                var stateJson = JsonSerializer.Serialize(state, _jsonOptions);
                AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] SaveStateAsync: Serialized stateJson length={stateJson.Length}, messageCount={state.Messages?.Count ?? 0}");

                if (WouldOverwriteLargeConversationWithTinyState(db, stateJson))
                {
                    AutomatonDebug.LogError($"ConversationPersistenceService.SaveStateAsync: Refusing destructive-looking save for stateId={_stateId}");
                    return false;
                }

                // One transaction for the blob and the projection it feeds. They were
                // separate commits, on separate connections, so a failure between them left
                // the blob ahead of the projection permanently: the blob kept all 168
                // messages while the projection held one, and the chat then loaded as that
                // one message. Either both land or neither does.
                db.ExecuteInTransaction((_, _) =>
                {
                    db.Execute(@"
                        INSERT INTO ConversationState (Id, ProjectId, ThreadId, State, Revision, CreatedAt, UpdatedAt)
                        VALUES (@Id, @ProjectId, @ThreadId, @State, @Revision, @CreatedAt, @UpdatedAt)
                        ON CONFLICT(Id) DO UPDATE SET
                            ProjectId = excluded.ProjectId,
                            ThreadId = excluded.ThreadId,
                            State = excluded.State,
                            Revision = MAX(ConversationState.Revision + 1, excluded.Revision),
                            UpdatedAt = excluded.UpdatedAt",
                        new Microsoft.Data.Sqlite.SqliteParameter("@Id", _stateId),
                        new Microsoft.Data.Sqlite.SqliteParameter("@ProjectId", _projectId ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@ThreadId", state.SessionId ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@State", stateJson),
                        new Microsoft.Data.Sqlite.SqliteParameter("@Revision", state.Revision),
                        new Microsoft.Data.Sqlite.SqliteParameter("@CreatedAt", state.LastSavedAt.ToString("O")),
                        new Microsoft.Data.Sqlite.SqliteParameter("@UpdatedAt", now));

                    SaveNormalizedState(db, state, stateJson);
                });

                OnStateSaved?.Invoke(state);

                // Also raise the global system event for cross-service communication
                SystemEvents.RaiseConversationSaved(_stateId, state);

                AutomatonDebug.Log("ConversationPersistenceService.SaveStateAsync: Save successful");
                return true;
            }
            catch (Exception ex)
            {
                AutomatonDebug.LogError($"ConversationPersistenceService.SaveStateAsync: Failed - {ex.Message}");
                Console.Error.WriteLine($"Failed to save conversation state: {ex.Message}");
                return false;
            }
        });
    }

    /// <summary>
    /// Updates the conversation state efficiently using the normalized schema.
    /// Updates metadata columns directly and upserts messages without re-serializing the entire state.
    /// </summary>
    /// <param name="state">The conversation state to update.</param>
    /// <returns>True if update was successful, false otherwise.</returns>
    public Task<bool> UpdateStateAsync(ConversationState state)
    {
        AutomatonDebug.Log($"ConversationPersistenceService.UpdateStateAsync: Entry - stateId={_stateId}");
        return Task.Run(() =>
        {
            try
            {
                var db = ConfigDatabase.Instance;
                var now = DateTime.UtcNow.ToString("O");

                state.Revision = Math.Max(state.Revision, LoadCurrentRevision(db)) + 1;
                state.LastSavedAt = DateTimeOffset.UtcNow;
                state.ProjectId = _projectId;

                // First, ensure the row exists (create if not)
                var existingRow = db.Query("SELECT Id FROM ConversationState WHERE Id = @Id",
                    new Microsoft.Data.Sqlite.SqliteParameter("@Id", _stateId)).FirstOrDefault();

                if (existingRow == null)
                {
                    var stateJson = JsonSerializer.Serialize(state, _jsonOptions);

                    // Create initial row with all metadata
                    db.Execute(@"
                        INSERT INTO ConversationState (Id, ProjectId, ThreadId, State, SessionId, Agent, Directory, Worktree,
                            UseRolesEnabled, ShowThinkingEnabled, ShowToolCallsEnabled, CollapseThinkingEnabled, CollapseToolCallsEnabled,
                            SelectedProviderKey, SelectedModelId, ReasoningEffort, Verbosity, CompressionSettings, Revision, CreatedAt, UpdatedAt)
                        VALUES (@Id, @ProjectId, @ThreadId, @State, @SessionId, @Agent, @Directory, @Worktree,
                            @UseRolesEnabled, @ShowThinkingEnabled, @ShowToolCallsEnabled, @CollapseThinkingEnabled, @CollapseToolCallsEnabled,
                            @SelectedProviderKey, @SelectedModelId, @ReasoningEffort, @Verbosity, @CompressionSettings, @Revision, @CreatedAt, @UpdatedAt)",
                        new Microsoft.Data.Sqlite.SqliteParameter("@Id", _stateId),
                        new Microsoft.Data.Sqlite.SqliteParameter("@ProjectId", _projectId ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@ThreadId", state.SessionId ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@State", stateJson),
                        new Microsoft.Data.Sqlite.SqliteParameter("@SessionId", state.SessionId ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@Agent", state.Agent ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@Directory", state.Directory ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@Worktree", state.Worktree ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@UseRolesEnabled", state.UseRolesEnabled ? 1 : 0),
                        new Microsoft.Data.Sqlite.SqliteParameter("@ShowThinkingEnabled", state.ShowThinkingEnabled ? 1 : 0),
                        new Microsoft.Data.Sqlite.SqliteParameter("@ShowToolCallsEnabled", state.ShowToolCallsEnabled ? 1 : 0),
                        new Microsoft.Data.Sqlite.SqliteParameter("@CollapseThinkingEnabled", state.CollapseThinkingEnabled ? 1 : 0),
                        new Microsoft.Data.Sqlite.SqliteParameter("@CollapseToolCallsEnabled", state.CollapseToolCallsEnabled ? 1 : 0),
                        new Microsoft.Data.Sqlite.SqliteParameter("@SelectedProviderKey", state.SelectedProviderKey ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@SelectedModelId", state.SelectedModelId ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@ReasoningEffort", state.ReasoningEffort ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@Verbosity", ResponseVerbosity.Normalize(state.Verbosity) ?? (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@CompressionSettings", state.CompressionSettings != null ? JsonSerializer.Serialize(state.CompressionSettings, _jsonOptions) : (object)DBNull.Value),
                        new Microsoft.Data.Sqlite.SqliteParameter("@Revision", state.Revision),
                        new Microsoft.Data.Sqlite.SqliteParameter("@CreatedAt", now),
                        new Microsoft.Data.Sqlite.SqliteParameter("@UpdatedAt", now));

                    // Insert all messages
                    if (state.Messages != null && state.Messages.Count > 0)
                    {
                        var msgDicts = ConvertMessagesToDictionaries(state.Messages);
                        db.ReplaceConversationMessages(_stateId, msgDicts);
                    }

                    PruneNormalizedMessagesToState(db, state);
                    RecordFullTranscript(db, state);
                }
                else
                {
                    // Update metadata columns only
                    db.UpdateConversationStateMetadata(
                        _stateId,
                        sessionId: state.SessionId,
                        agent: state.Agent,
                        directory: state.Directory,
                        worktree: state.Worktree,
                        useRolesEnabled: state.UseRolesEnabled,
                        showThinkingEnabled: state.ShowThinkingEnabled,
                        showToolCallsEnabled: state.ShowToolCallsEnabled,
                        collapseThinkingEnabled: state.CollapseThinkingEnabled,
                        collapseToolCallsEnabled: state.CollapseToolCallsEnabled,
                        selectedProviderKey: state.SelectedProviderKey,
                        selectedModelId: state.SelectedModelId,
                        reasoningEffort: state.ReasoningEffort,
                        compressionSettingsJson: state.CompressionSettings != null ? JsonSerializer.Serialize(state.CompressionSettings, _jsonOptions) : null,
                        projectId: _projectId,
                        threadId: state.SessionId,
                        revision: state.Revision,
                        fullPermissionMode: state.FullPermissionMode,
                        verbosity: ResponseVerbosity.Normalize(state.Verbosity));

                    // Upsert every message, not just brand-new IDs. Streaming can create
                    // placeholder assistant rows before tool-call parts are attached; those
                    // existing rows must be refreshed so later model requests see tool_calls
                    // and matching tool outputs instead of assistant prose alone.
                    if (state.Messages != null && state.Messages.Count > 0)
                    {
                        db.ReplaceConversationMessages(_stateId, ConvertMessagesToDictionaries(state.Messages));
                    }

                    PruneNormalizedMessagesToState(db, state);
                    RecordFullTranscript(db, state);
                }

                OnStateSaved?.Invoke(state);
                SystemEvents.RaiseConversationSaved(_stateId, state);

                AutomatonDebug.Log($"ConversationPersistenceService.UpdateStateAsync: Update successful");
                return true;
            }
            catch (Exception ex)
            {
                AutomatonDebug.LogError($"ConversationPersistenceService.UpdateStateAsync: Failed - {ex.Message}");
                Console.Error.WriteLine($"Failed to update conversation state: {ex.Message}");
                return false;
            }
        });
    }

    private void SaveNormalizedState(ConfigDatabase db, ConversationState state, string stateJson)
    {
        db.UpdateConversationStateMetadata(
            _stateId,
            sessionId: state.SessionId,
            agent: state.Agent,
            directory: state.Directory,
            worktree: state.Worktree,
            useRolesEnabled: state.UseRolesEnabled,
            showThinkingEnabled: state.ShowThinkingEnabled,
            showToolCallsEnabled: state.ShowToolCallsEnabled,
            collapseThinkingEnabled: state.CollapseThinkingEnabled,
            collapseToolCallsEnabled: state.CollapseToolCallsEnabled,
            selectedProviderKey: state.SelectedProviderKey,
            selectedModelId: state.SelectedModelId,
            reasoningEffort: state.ReasoningEffort,
            compressionSettingsJson: state.CompressionSettings != null ? JsonSerializer.Serialize(state.CompressionSettings, _jsonOptions) : null,
            projectId: _projectId,
            threadId: state.SessionId,
            revision: state.Revision,
            fullPermissionMode: state.FullPermissionMode,
            verbosity: ResponseVerbosity.Normalize(state.Verbosity));

        if (state.Messages is { Count: > 0 })
        {
            db.ReplaceConversationMessages(_stateId, ConvertMessagesToDictionaries(state.Messages));
        }

        PruneNormalizedMessagesToState(db, state);
        RecordFullTranscript(db, state);
    }

    /// <summary>
    /// Keeps the full transcript beside the live one. The live table is the model's context
    /// and loses whatever compaction dropped on the next save; this call writes the same
    /// messages into the append-only full table and records any compaction summary the state
    /// carries, so nothing is lost and the compressed view sits beside the raw one. A failure
    /// here is logged and never fails the save - the live save already succeeded.
    /// </summary>
    private void RecordFullTranscript(ConfigDatabase db, ConversationState state)
    {
        AutomatonDebug.Log($"ConversationPersistenceService.RecordFullTranscript: Entry - stateId={_stateId}, messages={state?.Messages?.Count ?? 0}");
        if (db == null || state?.Messages == null || state.Messages.Count == 0)
        {
            AutomatonDebug.Log("ConversationPersistenceService.RecordFullTranscript: Exit - nothing to record");
            return;
        }

        try
        {
            var summaries = state.Messages
                .Where(message => string.Equals(message.Role, "system", StringComparison.OrdinalIgnoreCase))
                .Select(message => CompressionSupport.ExtractSummary(message.Content))
                .Where(summary => summary != null)
                .Select(summary => summary!)
                .ToList();

            // One transaction: the full table stores the system prompt without its summary, so
            // a summary recorded in a second transaction could be lost between the two.
            var (inserted, recorded) = db.RecordFullTranscript(_stateId, ConvertMessagesToDictionaries(state.Messages), summaries);

            AutomatonDebug.Log($"ConversationPersistenceService.RecordFullTranscript: Exit - stateId={_stateId}, newMessages={inserted}, newSummaries={recorded}");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationPersistenceService.RecordFullTranscript: Failed for stateId={_stateId} - {ex.Message}");
        }
    }

    /// <summary>
    /// Whether a projection of this size, set against a blob of that size, has plainly lost
    /// messages rather than legitimately shed them.
    ///
    /// The blob is the authority, so compaction - which genuinely shortens a conversation -
    /// shortens the blob too and does not trip this. What does trip it is the projection
    /// alone falling behind, which is how a 168-message chat came to be stored as one row.
    /// </summary>
    public static bool ProjectionLooksTruncated(int blobMessages, int projectionMessages) =>
        blobMessages > 5 && projectionMessages < blobMessages / 2;

    private void PruneNormalizedMessagesToState(ConfigDatabase db, ConversationState state)
    {
        var incoming = state.Messages?.Count ?? 0;
        var blobMessages = db.CountStateBlobMessages(_stateId);

        // UpdateStateAsync reaches here without rewriting the blob, so before this guard
        // nothing stood between a short in-memory list and the deletion of every other row.
        // The blob keeping 168 messages while the projection was pruned to 1 is exactly the
        // state that made a live conversation load as empty.
        if (ProjectionLooksTruncated(blobMessages, incoming))
        {
            AutomatonDebug.LogError(
                $"ConversationPersistenceService.PruneNormalizedMessagesToState: refusing to prune {_stateId} to {incoming} message(s) " +
                $"while the stored conversation holds {blobMessages}. The in-memory conversation is behind; the projection is left alone.");
            return;
        }

        db.PruneConversationMessagesExcept(
            _stateId,
            state.Messages?
                .Select(message => message.Id ?? string.Empty)
                .Where(id => !string.IsNullOrWhiteSpace(id)) ?? []);
    }

    private static List<Dictionary<string, object?>> ConvertMessagesToDictionaries(List<Automaton.Core.Interfaces.Message> messages)
    {
        var result = new List<Dictionary<string, object?>>();
        var contentChars = 0L;
        var partsChars = 0L;
        foreach (var msg in messages)
        {
            var partsJson = msg.Parts != null ? JsonSerializer.Serialize(msg.Parts, MessagePartsJsonOptions) : null;
            contentChars += msg.Content?.Length ?? 0;
            partsChars += partsJson?.Length ?? 0;
            result.Add(new Dictionary<string, object?>
            {
                ["Id"] = Guid.NewGuid().ToString(),
                ["messageId"] = msg.Id,
                ["role"] = msg.Role,
                ["content"] = msg.Content,
                ["parts"] = partsJson,
                ["attachments"] = StoreAttachments(msg.Attachments)
            });
        }
        AutomatonDebug.Log($"ConversationPersistenceService.ConvertMessagesToDictionaries: {result.Count} message(s), contentChars={contentChars}, partsChars={partsChars}");
        return result;
    }

    /// <summary>
    /// Moves a message's attachment bytes to the attachment store and returns the
    /// metadata to save beside the message, or null when there is none.
    /// </summary>
    /// <remarks>
    /// The payload is deliberately not part of what is returned. An image is a
    /// few hundred kilobytes; carried in the row it would be rewritten on every
    /// save of the whole conversation, and the database is shared with settings
    /// and everything else.
    /// </remarks>
    private static string? StoreAttachments(List<Models.ChatAttachment>? attachments)
    {
        if (attachments is null || attachments.Count == 0)
        {
            return null;
        }

        var saved = new List<Models.ChatAttachment>();

        foreach (var attachment in attachments)
        {
            if (!string.IsNullOrEmpty(attachment.Base64Data))
            {
                try
                {
                    if (!AttachmentStore.Save(attachment.Id, Convert.FromBase64String(attachment.Base64Data!)))
                    {
                        // Nothing to point at afterwards, so it is not recorded -
                        // better than a reference that resolves to nothing.
                        continue;
                    }
                }
                catch (FormatException ex)
                {
                    AutomatonDebug.LogWarning($"[Attachments] '{attachment.Id}' is not valid base64, dropping: {ex.Message}");
                    continue;
                }
            }

            saved.Add(new Models.ChatAttachment
            {
                Id = attachment.Id,
                FileName = attachment.FileName,
                MediaType = attachment.MediaType,
                Kind = attachment.Kind,
                ByteSize = attachment.ByteSize,
                TextContent = attachment.TextContent,
                StagedPath = attachment.StagedPath,
                Error = attachment.Error,
                // Base64Data is left out on purpose - the bytes are on disk now.
            });
        }

        return saved.Count == 0 ? null : JsonSerializer.Serialize(saved, MessagePartsJsonOptions);
    }

    /// <summary>
    /// Rebuilds a message's attachments from stored metadata, reading the bytes
    /// back out of the attachment store.
    /// </summary>
    private static List<Models.ChatAttachment> RestoreAttachments(string? json)
    {
        if (string.IsNullOrWhiteSpace(json))
        {
            return [];
        }

        List<Models.ChatAttachment>? stored;
        try
        {
            stored = JsonSerializer.Deserialize<List<Models.ChatAttachment>>(json, MessagePartsJsonOptions);
        }
        catch (JsonException ex)
        {
            AutomatonDebug.LogWarning($"[Attachments] Could not read stored attachments: {ex.Message}");
            return [];
        }

        if (stored is null)
        {
            return [];
        }

        foreach (var attachment in stored)
        {
            if (!string.IsNullOrEmpty(attachment.Base64Data))
            {
                continue;
            }

            var bytes = AttachmentStore.TryLoad(attachment.Id);
            if (bytes is not null)
            {
                attachment.Base64Data = Convert.ToBase64String(bytes);
            }
            else
            {
                // The file is gone - pruned, or copied between machines without
                // it. The attachment still lists, so the loss is visible rather
                // than silent.
                attachment.Error ??= "The stored file is no longer available.";
            }
        }

        return stored;
    }

    private bool WouldOverwriteLargeConversationWithTinyState(ConfigDatabase db, string newStateJson)
    {
        var row = db.Query("SELECT State FROM ConversationState WHERE Id = @Id",
            new Microsoft.Data.Sqlite.SqliteParameter("@Id", _stateId))
            .FirstOrDefault();

        if (row == null || row["State"] is not string existingStateJson || string.IsNullOrWhiteSpace(existingStateJson))
        {
            return false;
        }

        var existingMessageCount = TryGetMessageCount(existingStateJson);
        var newMessageCount = TryGetMessageCount(newStateJson);

        if (!existingMessageCount.HasValue || !newMessageCount.HasValue)
        {
            return false;
        }

        return existingMessageCount.Value > 20 &&
               newMessageCount.Value <= 2 &&
               existingStateJson.Length > newStateJson.Length * 3;
    }

    private static int? TryGetMessageCount(string stateJson)
    {
        try
        {
            using var document = JsonDocument.Parse(stateJson);
            if (!document.RootElement.TryGetProperty("messages", out var messages) &&
                !document.RootElement.TryGetProperty("Messages", out messages))
            {
                return null;
            }

            return messages.ValueKind == JsonValueKind.Array ? messages.GetArrayLength() : null;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// Loads the conversation state from the database.
    /// Prefers normalized schema, falls back to JSON blob for backward compatibility.
    /// </summary>
    /// <returns>The loaded conversation state, or null if no state exists.</returns>
    public Task<ConversationState?> LoadStateAsync()
    {
        AutomatonDebug.Log($"ConversationPersistenceService.LoadStateAsync: Entry - stateId={_stateId}");
        return Task.Run(() =>
        {
            try
            {
                var db = ConfigDatabase.Instance;

                // Try loading from normalized schema first
                var metadata = db.LoadConversationStateMetadata(_stateId);
                if (metadata != null)
                {
                    var state = new ConversationState
                    {
                        SessionId = metadata.ContainsKey("SessionId") ? metadata["SessionId"]?.ToString() ?? "" : "",
                        Agent = metadata.ContainsKey("Agent") ? metadata["Agent"]?.ToString() ?? "" : "",
                        Directory = metadata.ContainsKey("Directory") ? metadata["Directory"]?.ToString() ?? "" : "",
                        Worktree = metadata.ContainsKey("Worktree") ? metadata["Worktree"]?.ToString() ?? "" : "",
                        ProjectId = metadata.ContainsKey("ProjectId") ? metadata["ProjectId"]?.ToString() : null,
                        UseRolesEnabled = metadata.ContainsKey("UseRolesEnabled") && Convert.ToInt32(metadata["UseRolesEnabled"]) == 1,
                        ShowThinkingEnabled = ReadMetadataBool(metadata, "ShowThinkingEnabled", defaultValue: true),
                        ShowToolCallsEnabled = ReadMetadataBool(metadata, "ShowToolCallsEnabled", defaultValue: true),
                        CollapseThinkingEnabled = ReadMetadataBool(metadata, "CollapseThinkingEnabled", defaultValue: true),
                        CollapseToolCallsEnabled = ReadMetadataBool(metadata, "CollapseToolCallsEnabled", defaultValue: true),
                        SelectedProviderKey = metadata.ContainsKey("SelectedProviderKey") ? metadata["SelectedProviderKey"]?.ToString() : null,
                        SelectedModelId = metadata.ContainsKey("SelectedModelId") ? metadata["SelectedModelId"]?.ToString() : null,
                        ReasoningEffort = metadata.ContainsKey("ReasoningEffort") ? metadata["ReasoningEffort"]?.ToString() : null,
                        Verbosity = ResponseVerbosity.Normalize(metadata.ContainsKey("Verbosity") ? metadata["Verbosity"]?.ToString() : null),
                        FullPermissionMode = ReadMetadataBool(metadata, "FullPermissionMode", defaultValue: false),
                        Revision = ReadMetadataLong(metadata, "Revision"),
                        LastSavedAt = ReadMetadataDateTimeOffset(metadata, "UpdatedAt"),
                        Version = 1
                    };

                    // Load compression settings if present
                    if (metadata.ContainsKey("CompressionSettings") && metadata["CompressionSettings"] is string compJson && !string.IsNullOrEmpty(compJson))
                    {
                        try
                        {
                            state.CompressionSettings = JsonSerializer.Deserialize<ConversationCompressionSettings>(compJson, _jsonOptions);
                        }
                        catch (Exception ex)
                        {
                            AutomatonDebug.LogError("Exception in LoadStateAsync(): " + ex.Message + ex.InnerException + ex.StackTrace);
                        }
                    }

                    // Load messages from the normalized table
                    var msgDicts = db.LoadConversationMessages(_stateId);

                    // An empty projection was always read from the blob instead. A partial one
                    // was not, and that is the worse case: one row standing in for 168 loads as
                    // the whole conversation, and the next save is then refused for trying to
                    // overwrite the blob with it. The blob is the authority either way.
                    var normalizedCount = msgDicts?.Count ?? 0;
                    var blobCount = db.CountStateBlobMessages(_stateId);
                    var truncated = ProjectionLooksTruncated(blobCount, normalizedCount);

                    if (truncated)
                    {
                        AutomatonDebug.LogWarning(
                            $"ConversationPersistenceService.LoadStateAsync: projection holds {normalizedCount} message(s) against {blobCount} in the stored conversation; loading from the blob and rebuilding");
                    }

                    if ((normalizedCount == 0 || truncated) &&
                        TryLoadLegacyStateWithMessages(db, out var legacyStateWithMessages))
                    {
                        if (legacyStateWithMessages == null)
                        {
                            throw new InvalidOperationException("Legacy state is null after migration.");
                        }

                        db.MigrateConversationState(_stateId);
                        OnStateLoaded?.Invoke(legacyStateWithMessages);
                        AutomatonDebug.Log($"ConversationPersistenceService.LoadStateAsync: State loaded from legacy JSON because normalized messages were empty");
                        return legacyStateWithMessages;
                    }

                    if (msgDicts != null && msgDicts.Count > 0)
                    {
                        state.Messages = new List<Automaton.Core.Interfaces.Message>();
                        foreach (var msgDict in msgDicts)
                        {
                            var msgId = msgDict.ContainsKey("MessageId") ? msgDict["MessageId"]?.ToString() ?? Guid.NewGuid().ToString() : Guid.NewGuid().ToString();
                            var role = msgDict.ContainsKey("Role") ? msgDict["Role"]?.ToString() ?? "unknown" : "unknown";
                            var content = msgDict.ContainsKey("Content") ? msgDict["Content"]?.ToString() : null;
                            var partsJson = msgDict.ContainsKey("Parts") ? msgDict["Parts"]?.ToString() : null;
                            var attachmentsJson = msgDict.ContainsKey("Attachments") ? msgDict["Attachments"]?.ToString() : null;

                            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] LoadStateAsync: Loading msg - role={role}, contentLen={content?.Length ?? 0}, partsJsonLen={partsJson?.Length ?? 0}");

                            var parts = new List<Automaton.Core.Interfaces.MessagePart>();
                            if (!string.IsNullOrEmpty(partsJson))
                            {
                                try
                                {
                                    parts = JsonSerializer.Deserialize<List<Automaton.Core.Interfaces.MessagePart>>(partsJson, _jsonOptions) ?? [];
                                    AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] LoadStateAsync: Deserialized {parts.Count} parts for role={role}");
                                }
                                catch (Exception ex)
                                {
                                    AutomatonDebug.LogError("Exception in LoadStateAsync(): " + ex.Message + ex.InnerException + ex.StackTrace);
                                }
                            }

                            state.Messages.Add(new Automaton.Core.Interfaces.Message
                            {
                                Id = msgId,
                                Role = role,
                                Content = content ?? string.Empty,
                                Parts = parts,
                                Attachments = RestoreAttachments(attachmentsJson)
                            });
                        }
                    }
                    else
                    {
                        state.Messages = new List<Automaton.Core.Interfaces.Message>();
                    }

                    OnStateLoaded?.Invoke(state);
                    var totalParts = state.Messages.Sum(m => m.Parts?.Count ?? 0);
                    AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] LoadStateAsync: State loaded from normalized schema with {state.Messages.Count} messages, totalParts={totalParts}");
                    return state;
                }

                // Fall back to JSON blob for backward compatibility
                var row = db.Query("SELECT State FROM ConversationState WHERE Id = @Id",
                    new Microsoft.Data.Sqlite.SqliteParameter("@Id", _stateId))
                    .FirstOrDefault();

                if (row == null)
                {
                    AutomatonDebug.Log("ConversationPersistenceService.LoadStateAsync: No state found");
                    return null;
                }

                var json = row["State"] as string;
                if (string.IsNullOrEmpty(json))
                {
                    AutomatonDebug.Log("ConversationPersistenceService.LoadStateAsync: State is null or empty");
                    return null;
                }

                // Check if this is a legacy JSON blob that needs migration
                var legacyState = JsonSerializer.Deserialize<ConversationState>(json, _jsonOptions);

                if (legacyState != null)
                {
                    // Migrate to normalized schema for future loads
                    db.MigrateConversationState(_stateId);
                    OnStateLoaded?.Invoke(legacyState);
                    AutomatonDebug.Log($"ConversationPersistenceService.LoadStateAsync: State loaded from JSON blob and migrated");
                }

                return legacyState;
            }
            catch (Exception ex)
            {
                AutomatonDebug.LogError($"ConversationPersistenceService.LoadStateAsync: Failed - {ex.Message}");
                return null;
            }
        });
    }

    private bool TryLoadLegacyStateWithMessages(ConfigDatabase db, out ConversationState? state)
    {
        state = null;

        var row = db.Query("SELECT State FROM ConversationState WHERE Id = @Id",
                new Microsoft.Data.Sqlite.SqliteParameter("@Id", _stateId))
            .FirstOrDefault();

        if (row == null || row["State"] is not string json || string.IsNullOrWhiteSpace(json))
        {
            return false;
        }

        try
        {
            var legacyState = JsonSerializer.Deserialize<ConversationState>(json, _jsonOptions);
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[PROVIDERHTTPCHECK] TryLoadLegacyStateWithMessages: Deserialized legacy state, messageCount={legacyState?.Messages?.Count ?? 0}");
            if (legacyState?.Messages is { Count: > 0 })
            {
                state = legacyState;
                return true;
            }
        }
        catch
        {
            return false;
        }

        return false;
    }

    /// <summary>
    /// Clears the saved conversation state by archiving it to a fresh GUID rather than
    /// deleting. The original state row's Id is renamed to a new archive Id and marked
    /// IsArchived=1, ArchivedAt=now, ArchivedReason=<paramref name="reason"/>,
    /// ArchivedFromId=&lt;original stateId&gt;. Messages move with the state row via the
    /// renamed StateId. The original Id slot is now free for a brand-new conversation.
    /// Content stays in the DB and remains searchable via remember/FTS.
    /// </summary>
    public string ArchiveAndClearState(string reason = "cleared")
    {
        AutomatonDebug.Log($"ConversationPersistenceService.ArchiveAndClearState: Entry - stateId={_stateId}, reason={reason}");

        if (string.IsNullOrWhiteSpace(_stateId))
        {
            AutomatonDebug.LogWarning("ConversationPersistenceService.ArchiveAndClearState: Empty stateId, nothing to archive");
            return string.Empty;
        }

        var archiveId = $"archived/{Guid.NewGuid()}";

        try
        {
            var db = ConfigDatabase.Instance;
            var now = DateTime.UtcNow.ToString("O");

            // The ConversationMessages → ConversationState foreign key is
            // ON DELETE CASCADE (no ON UPDATE CASCADE). Renaming the parent's Id
            // would orphan the children mid-statement, and `PRAGMA foreign_keys=OFF`
            // doesn't help us here because ConfigDatabase.Execute opens a fresh
            // connection per call and every fresh connection re-enables FK in
            // CreateOpenConnection.
            //
            // Safe sequence inside one transaction on one connection:
            //   1. INSERT a new state row at archiveId, copying every column from
            //      the original row, with IsArchived/ArchivedAt/ArchivedReason set.
            //   2. UPDATE ConversationMessages.StateId from originalId -> archiveId.
            //   3. DELETE the original parent row. Its children no longer reference
            //      it (step 2) so the ON DELETE CASCADE has nothing to remove.
            db.ExecuteInTransaction((connection, transaction) =>
            {
                var existingRows = ConfigDatabase.QueryOnConnection(connection, transaction,
                    "SELECT * FROM ConversationState WHERE Id = @Id",
                    new Microsoft.Data.Sqlite.SqliteParameter("@Id", _stateId));

                if (existingRows.Count == 0)
                {
                    AutomatonDebug.Log("ConversationPersistenceService.ArchiveAndClearState: no existing state row, nothing to archive");
                    return;
                }

                var existing = existingRows[0];

                // Build column→value pairs from the source row, overlaying the
                // archive metadata. The column list is enumerated dynamically so
                // future schema additions don't break this code path.
                var copy = new Dictionary<string, object?>(existing, StringComparer.OrdinalIgnoreCase)
                {
                    ["Id"] = archiveId,
                    ["IsArchived"] = 1,
                    ["ArchivedAt"] = now,
                    ["ArchivedReason"] = reason,
                    ["ArchivedFromId"] = _stateId,
                    ["UpdatedAt"] = now
                };

                var columns = copy.Keys.ToList();
                var insertSql = $"INSERT INTO ConversationState ({string.Join(", ", columns)}) " +
                                $"VALUES ({string.Join(", ", columns.Select((_, i) => "@p" + i))})";
                var insertParams = columns
                    .Select((c, i) => new Microsoft.Data.Sqlite.SqliteParameter("@p" + i, copy[c] ?? DBNull.Value))
                    .ToArray();

                ConfigDatabase.ExecuteOnConnection(connection, transaction, insertSql, insertParams);

                ConfigDatabase.ExecuteOnConnection(connection, transaction,
                    "UPDATE ConversationMessages SET StateId = @ArchiveId WHERE StateId = @OriginalId",
                    new Microsoft.Data.Sqlite.SqliteParameter("@ArchiveId", archiveId),
                    new Microsoft.Data.Sqlite.SqliteParameter("@OriginalId", _stateId));

                // The full transcript and its summaries move with the conversation, for the
                // same reason and under the same foreign key.
                ConfigDatabase.ExecuteOnConnection(connection, transaction,
                    "UPDATE ConversationMessagesFull SET StateId = @ArchiveId WHERE StateId = @OriginalId",
                    new Microsoft.Data.Sqlite.SqliteParameter("@ArchiveId", archiveId),
                    new Microsoft.Data.Sqlite.SqliteParameter("@OriginalId", _stateId));

                ConfigDatabase.ExecuteOnConnection(connection, transaction,
                    "UPDATE ConversationSummaries SET StateId = @ArchiveId WHERE StateId = @OriginalId",
                    new Microsoft.Data.Sqlite.SqliteParameter("@ArchiveId", archiveId),
                    new Microsoft.Data.Sqlite.SqliteParameter("@OriginalId", _stateId));

                ConfigDatabase.ExecuteOnConnection(connection, transaction,
                    "UPDATE MemoryTags SET StateId = @ArchiveId WHERE StateId = @OriginalId",
                    new Microsoft.Data.Sqlite.SqliteParameter("@ArchiveId", archiveId),
                    new Microsoft.Data.Sqlite.SqliteParameter("@OriginalId", _stateId));

                ConfigDatabase.ExecuteOnConnection(connection, transaction,
                    "DELETE FROM ConversationState WHERE Id = @OriginalId",
                    new Microsoft.Data.Sqlite.SqliteParameter("@OriginalId", _stateId));
            });

            AutomatonDebug.Log($"ConversationPersistenceService.ArchiveAndClearState: Exit - archived stateId={_stateId} as {archiveId}, reason={reason}");
            return archiveId;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationPersistenceService.ArchiveAndClearState: Failed - {ex.Message}");
            Console.Error.WriteLine($"Failed to archive conversation state: {ex.Message}");
            return string.Empty;
        }
    }

    /// <summary>
    /// Backwards-compatible wrapper. Now archives rather than deletes — see
    /// <see cref="ArchiveAndClearState"/>. Kept as <c>ClearState</c> so existing
    /// callers (remote services, etc.) continue to work without change.
    /// </summary>
    public void ClearState()
    {
        ArchiveAndClearState("cleared");
    }

    /// <summary>
    /// Permanently removes the current state and every archived state that originated
    /// from it. Conversation messages are removed by the database foreign-key cascade.
    /// Other conversation families are not affected.
    /// </summary>
    public int DeleteStateAndArchives()
    {
        AutomatonDebug.Log($"ConversationPersistenceService.DeleteStateAndArchives: Entry - stateId={_stateId}");

        if (string.IsNullOrWhiteSpace(_stateId))
        {
            return 0;
        }

        try
        {
            var deleted = ConfigDatabase.Instance.Execute(
                "DELETE FROM ConversationState WHERE Id = @Id OR ArchivedFromId = @Id",
                new Microsoft.Data.Sqlite.SqliteParameter("@Id", _stateId));
            AutomatonDebug.Log($"ConversationPersistenceService.DeleteStateAndArchives: Exit - deleted={deleted}");
            return deleted;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationPersistenceService.DeleteStateAndArchives: Failed - {ex.Message}");
            throw;
        }
    }

    /// <summary>
    /// Sticky-marks this conversation so it is hidden from <c>remember(source="chats")</c>
    /// results. Called when the Dream toggle is turned on for a chat — dream chats are
    /// operational artifacts (dream prompt injections, dream tool calls, mirror writes)
    /// and should not surface as candidate sources for future curation. Idempotent.
    /// </summary>
    public bool MarkExcludedFromDream()
    {
        AutomatonDebug.Log($"ConversationPersistenceService.MarkExcludedFromDream: Entry - stateId={_stateId}");

        if (string.IsNullOrWhiteSpace(_stateId))
        {
            AutomatonDebug.LogWarning("ConversationPersistenceService.MarkExcludedFromDream: Empty stateId, nothing to mark");
            return false;
        }

        try
        {
            var db = ConfigDatabase.Instance;
            var affected = db.Execute(
                "UPDATE ConversationState SET ExcludeFromDream = 1 WHERE Id = @Id",
                new Microsoft.Data.Sqlite.SqliteParameter("@Id", _stateId));
            AutomatonDebug.Log($"ConversationPersistenceService.MarkExcludedFromDream: Exit - rowsAffected={affected}");
            return affected > 0;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationPersistenceService.MarkExcludedFromDream: Failed - {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Gets the path to the database file.
    /// </summary>
    public string SaveFilePath => ConfigDatabase.Instance.DatabasePath;

    /// <summary>
    /// Checks if a saved state exists.
    /// </summary>
    public bool HasSavedState()
    {
        AutomatonDebug.Log($"ConversationPersistenceService.HasSavedState: Entry - stateId={_stateId}");
        var db = ConfigDatabase.Instance;
        var result = db.ExecuteScalar<long>("SELECT COUNT(*) FROM ConversationState WHERE Id = @Id",
            new Microsoft.Data.Sqlite.SqliteParameter("@Id", _stateId)) > 0;
        AutomatonDebug.Log($"ConversationPersistenceService.HasSavedState: Exiting - result={result}");
        return result;
    }

    // ============================================================================
    // STREAMING PERSISTENCE - Save incrementally during streaming to prevent data loss
    // ============================================================================

    /// <summary>
    /// Tracks the state of a streaming message for incremental persistence.
    /// </summary>
    public class StreamingMessageState
    {
        public string MessageId { get; set; } = string.Empty;
        public string Role { get; set; } = string.Empty;
        public string Content { get; set; } = string.Empty;
        public int SaveCounter { get; set; } = 0;
        public string? UserMessage { get; set; }
    }

    /// <summary>
    /// Begins streaming persistence for a new AI response message.
    /// Creates a placeholder message in the database that will be updated incrementally.
    /// </summary>
    /// <param name="messageId">Unique ID for this streaming message</param>
    /// <param name="role">Message role (usually "assistant")</param>
    /// <param name="userMessage">The user message that triggered this streaming response (for broadcast to remote viewers)</param>
    /// <returns>Streaming state tracker, or null on failure</returns>
    public StreamingMessageState? BeginStreamingAsync(string messageId, string role, string? userMessage = null)
    {
        AutomatonDebug.Log($"ConversationPersistenceService.BeginStreamingAsync: Entry - messageId={messageId}, role={role}");

        try
        {
            var db = ConfigDatabase.Instance;

            // Create the message row immediately with empty content
            db.Execute(@"
                INSERT INTO ConversationMessages (Id, StateId, MessageIndex, MessageId, Role, Content, Parts, CreatedAt)
                VALUES (@Id, @StateId, @MessageIndex, @MessageId, @Role, @Content, @Parts, @CreatedAt)",
                new Microsoft.Data.Sqlite.SqliteParameter("@Id", Guid.NewGuid().ToString()),
                new Microsoft.Data.Sqlite.SqliteParameter("@StateId", _stateId),
                new Microsoft.Data.Sqlite.SqliteParameter("@MessageIndex", -1), // Will be set properly on finalize
                new Microsoft.Data.Sqlite.SqliteParameter("@MessageId", messageId),
                new Microsoft.Data.Sqlite.SqliteParameter("@Role", role),
                new Microsoft.Data.Sqlite.SqliteParameter("@Content", string.Empty),
                new Microsoft.Data.Sqlite.SqliteParameter("@Parts", (object)DBNull.Value),
                new Microsoft.Data.Sqlite.SqliteParameter("@CreatedAt", DateTime.UtcNow.ToString("O")));

            var state = new StreamingMessageState
            {
                MessageId = messageId,
                Role = role,
                Content = string.Empty,
                SaveCounter = 0,
                UserMessage = userMessage
            };

            AutomatonDebug.Log($"ConversationPersistenceService.BeginStreamingAsync: Created placeholder for messageId={messageId}");
            return state;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationPersistenceService.BeginStreamingAsync: Failed to create streaming placeholder - {ex.Message}");
            return null;
        }
    }

    /// <summary>
    /// Appends a streaming chunk and saves incrementally.
    /// Saves every N chunks to balance between persistence safety and database load.
    /// </summary>
    /// <param name="streamingState">The streaming message state tracker</param>
    /// <param name="chunk">The new text chunk to append</param>
    /// <param name="saveEveryNChunks">Save to DB every N chunks (default: 5)</param>
    /// <returns>True if saved, false otherwise</returns>
    public bool AppendStreamingChunk(StreamingMessageState streamingState, string chunk, int saveEveryNChunks = 5)
    {
        if (streamingState == null || string.IsNullOrEmpty(chunk))
        {
            AutomatonDebug.Log($"ConversationPersistenceService.AppendStreamingChunk: Skipped - streamingState={streamingState != null}, chunkLength={chunk?.Length ?? 0}");
            return false;
        }

        // Accumulate the content in memory
        streamingState.Content += chunk;
        streamingState.SaveCounter++;

        AutomatonDebug.Log($"ConversationPersistenceService.AppendStreamingChunk: chunk={streamingState.SaveCounter}, contentLen={streamingState.Content.Length}, newChunk='{chunk}', willSave={streamingState.SaveCounter % saveEveryNChunks == 0}");

        // Only save to DB every N chunks to avoid excessive writes
        if (streamingState.SaveCounter % saveEveryNChunks != 0)
        {
            return true; // Not saving this chunk, but still accumulating
        }

        // Update the message content in the database
        try
        {
            var db = ConfigDatabase.Instance;

            db.Execute(@"
                UPDATE ConversationMessages
                SET Content = @Content, CreatedAt = @CreatedAt
                WHERE StateId = @StateId AND MessageId = @MessageId",
                new Microsoft.Data.Sqlite.SqliteParameter("@Content", streamingState.Content),
                new Microsoft.Data.Sqlite.SqliteParameter("@CreatedAt", DateTime.UtcNow.ToString("O")),
                new Microsoft.Data.Sqlite.SqliteParameter("@StateId", _stateId),
                new Microsoft.Data.Sqlite.SqliteParameter("@MessageId", streamingState.MessageId));

            AutomatonDebug.Log($"ConversationPersistenceService.AppendStreamingChunk: SAVED to DB for messageId={streamingState.MessageId}, totalLength={streamingState.Content.Length}");
            SystemEvents.RaiseConversationStreamingUpdated(new ConversationStreamingEventInfo(
                _stateId,
                streamingState.MessageId,
                streamingState.Role,
                streamingState.Content,
                chunk,
                IsFinal: false,
                streamingState.UserMessage));
            return true;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationPersistenceService.AppendStreamingChunk: DB save failed for messageId={streamingState.MessageId} - {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Finalizes a streaming message - sets the final content and proper message index.
    /// </summary>
    /// <param name="streamingState">The streaming message state tracker</param>
    /// <param name="finalContent">The final accumulated content</param>
    /// <param name="messageIndex">The index position for this message</param>
    /// <param name="parts">Optional message parts</param>
    /// <returns>True if finalized successfully</returns>
    public bool FinalizeStreamingMessage(StreamingMessageState streamingState, string finalContent, int messageIndex, List<Automaton.Core.Interfaces.MessagePart>? parts = null)
    {
        if (streamingState == null)
        {
            return false;
        }

        try
        {
            var db = ConfigDatabase.Instance;
            var partsJson = parts != null ? JsonSerializer.Serialize(parts, _jsonOptions) : null;

            // Update with final content and proper index
            db.Execute(@"
                UPDATE ConversationMessages
                SET Content = @Content, MessageIndex = @MessageIndex, Parts = @Parts, CreatedAt = @CreatedAt
                WHERE StateId = @StateId AND MessageId = @MessageId",
                new Microsoft.Data.Sqlite.SqliteParameter("@Content", finalContent),
                new Microsoft.Data.Sqlite.SqliteParameter("@MessageIndex", messageIndex),
                new Microsoft.Data.Sqlite.SqliteParameter("@Parts", partsJson ?? (object)DBNull.Value),
                new Microsoft.Data.Sqlite.SqliteParameter("@CreatedAt", DateTime.UtcNow.ToString("O")),
                new Microsoft.Data.Sqlite.SqliteParameter("@StateId", _stateId),
                new Microsoft.Data.Sqlite.SqliteParameter("@MessageId", streamingState.MessageId));

            AutomatonDebug.Log($"ConversationPersistenceService.FinalizeStreamingMessage: Finalized messageId={streamingState.MessageId} (length: {finalContent.Length})");
            SystemEvents.RaiseConversationStreamingUpdated(new ConversationStreamingEventInfo(
                _stateId,
                streamingState.MessageId,
                streamingState.Role,
                finalContent,
                string.Empty,
                IsFinal: true,
                streamingState.UserMessage));
            return true;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationPersistenceService.FinalizeStreamingMessage: Failed to finalize - {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Removes a streaming placeholder when streaming fails or is interrupted.
    /// Prevents orphaned placeholder rows from accumulating.
    /// </summary>
    /// <param name="messageId">The message ID of the placeholder to remove</param>
    /// <returns>True if cleanup succeeded or placeholder didn't exist, false on error</returns>
    public bool CleanupStreamingPlaceholder(string messageId)
    {
        if (string.IsNullOrEmpty(messageId))
        {
            return false;
        }

        try
        {
            var db = ConfigDatabase.Instance;
            db.Execute(@"
                DELETE FROM ConversationMessages
                WHERE StateId = @StateId AND MessageId = @MessageId AND MessageIndex = -1",
                new Microsoft.Data.Sqlite.SqliteParameter("@StateId", _stateId),
                new Microsoft.Data.Sqlite.SqliteParameter("@MessageId", messageId));

            AutomatonDebug.Log($"ConversationPersistenceService.CleanupStreamingPlaceholder: Removed placeholder messageId={messageId}");
            return true;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationPersistenceService.CleanupStreamingPlaceholder: Failed - {ex.Message}");
            return false;
        }
    }

    private static bool ReadMetadataBool(Dictionary<string, object?> metadata, string key, bool defaultValue = false)
    {
        if (!metadata.TryGetValue(key, out var raw) || raw == null || raw == DBNull.Value)
        {
            return defaultValue;
        }

        return Convert.ToInt32(raw) == 1;
    }

    private long LoadCurrentRevision(ConfigDatabase db)
    {
        var metadata = db.LoadConversationStateMetadata(_stateId);
        return metadata == null ? 0 : ReadMetadataLong(metadata, "Revision");
    }

    private static long ReadMetadataLong(Dictionary<string, object?> metadata, string key)
    {
        if (!metadata.TryGetValue(key, out var raw) || raw == null || raw == DBNull.Value)
        {
            return 0;
        }

        return raw switch
        {
            long longValue => longValue,
            int intValue => intValue,
            _ when long.TryParse(raw.ToString(), out var parsed) => parsed,
            _ => 0
        };
    }

    private static DateTimeOffset ReadMetadataDateTimeOffset(Dictionary<string, object?> metadata, string key)
    {
        if (!metadata.TryGetValue(key, out var raw) ||
            raw == null ||
            raw == DBNull.Value ||
            !DateTimeOffset.TryParse(raw.ToString(), out var parsed))
        {
            return DateTimeOffset.UtcNow;
        }

        return parsed;
    }

    /// <summary>
    /// Updates the conversation state metadata and timestamps (lightweight update).
    /// </summary>
    public bool UpdateMetadataOnly(ConversationState state)
    {
        try
        {
            var db = ConfigDatabase.Instance;
            state.Revision = Math.Max(state.Revision, LoadCurrentRevision(db)) + 1;

            db.UpdateConversationStateMetadata(
                _stateId,
                sessionId: state.SessionId,
                agent: state.Agent,
                directory: state.Directory,
                worktree: state.Worktree,
                useRolesEnabled: state.UseRolesEnabled,
                showThinkingEnabled: state.ShowThinkingEnabled,
                showToolCallsEnabled: state.ShowToolCallsEnabled,
                collapseThinkingEnabled: state.CollapseThinkingEnabled,
                collapseToolCallsEnabled: state.CollapseToolCallsEnabled,
                selectedProviderKey: state.SelectedProviderKey,
                selectedModelId: state.SelectedModelId,
                reasoningEffort: state.ReasoningEffort,
                compressionSettingsJson: state.CompressionSettings != null ? JsonSerializer.Serialize(state.CompressionSettings, _jsonOptions) : null,
                projectId: _projectId,
                threadId: state.SessionId,
                revision: state.Revision,
                fullPermissionMode: state.FullPermissionMode,
                verbosity: ResponseVerbosity.Normalize(state.Verbosity));

            return true;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ConversationPersistenceService.UpdateMetadataOnly: Failed - {ex.Message}");
            return false;
        }
    }

    }
