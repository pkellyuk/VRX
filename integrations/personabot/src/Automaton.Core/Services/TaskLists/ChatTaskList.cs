using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Automaton.Core.Interfaces;
using Automaton.Core.Services.RequestContext;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services.TaskLists;

/// <summary>One line of a chat's task list.</summary>
public sealed class TaskListItem
{
    public const string Pending = "pending";
    public const string InProgress = "in_progress";
    public const string Completed = "completed";

    public static readonly IReadOnlyList<string> Statuses = new[] { Pending, InProgress, Completed };

    [JsonPropertyName("text")]
    public string Text { get; set; } = string.Empty;

    [JsonPropertyName("status")]
    public string Status { get; set; } = Pending;

    /// <summary>The stored form of a status; anything unrecognised is pending.</summary>
    public static string NormalizeStatus(string? status)
    {
        var value = (status ?? string.Empty).Trim().ToLowerInvariant().Replace(' ', '_').Replace('-', '_');
        return value switch
        {
            InProgress or "doing" or "active" or "inprogress" => InProgress,
            Completed or "done" or "complete" => Completed,
            _ => Pending
        };
    }
}

/// <summary>
/// The short working list a chat's model keeps for the job in hand - the "task_list" tool writes
/// it, the chat shows it pinned above the message box, and every request carries it so the model
/// still has it after compaction. Lighter than a goal: one list per chat, replaced whole each time,
/// and gone when the model clears it.
///
/// Keyed by the chat's session id, which is what a tool sees of its chat. Kept in its own table so
/// it is not part of the conversation and is never compacted away.
/// </summary>
public sealed class ChatTaskListStore
{
    /// <summary>The most lines a list may have; a longer one is a plan, which belongs in a goal.</summary>
    public const int MaxItems = 25;

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = false };

    private static ChatTaskListStore? _shared;

    private readonly ConfigDatabase _database;
    private bool _tableReady;

    /// <summary>The store over the app's own database.</summary>
    public static ChatTaskListStore Shared => _shared ??= new ChatTaskListStore(ConfigDatabase.Instance);

    /// <summary>Raised after a list is written or cleared, with the session id and the new list (empty when cleared).</summary>
    public static event Action<string, IReadOnlyList<TaskListItem>>? Changed;

    public ChatTaskListStore(ConfigDatabase database)
    {
        _database = database ?? throw new ArgumentNullException(nameof(database));
    }

    /// <summary>
    /// The chat's list from the shared store, for a transcript: never throws, since a list that
    /// cannot be read must not stop the conversation from loading.
    /// </summary>
    public static List<TaskListItem> ReadSafely(string? sessionId)
    {
        try
        {
            return Shared.Get(sessionId).ToList();
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"ChatTaskListStore.ReadSafely: {sessionId} - {ex.GetType().Name}: {ex.Message}");
            return new List<TaskListItem>();
        }
    }

    /// <summary>The chat's list, or an empty one.</summary>
    public IReadOnlyList<TaskListItem> Get(string? sessionId)
    {
        if (string.IsNullOrWhiteSpace(sessionId))
        {
            return Array.Empty<TaskListItem>();
        }

        EnsureTable();
        var row = _database.Query("SELECT ItemsJson FROM ChatTaskLists WHERE SessionId = @SessionId",
            new SqliteParameter("@SessionId", sessionId)).FirstOrDefault();
        if (row == null || row["ItemsJson"] is not string json || string.IsNullOrWhiteSpace(json))
        {
            return Array.Empty<TaskListItem>();
        }

        try
        {
            return JsonSerializer.Deserialize<List<TaskListItem>>(json, JsonOptions) ?? new List<TaskListItem>();
        }
        catch (JsonException ex)
        {
            AutomatonDebug.LogError($"ChatTaskListStore.Get: unreadable list for {sessionId} - {ex.Message}");
            return Array.Empty<TaskListItem>();
        }
    }

    /// <summary>Replaces the chat's list. An empty list clears it.</summary>
    public IReadOnlyList<TaskListItem> Set(string sessionId, IEnumerable<TaskListItem>? items)
    {
        if (string.IsNullOrWhiteSpace(sessionId))
        {
            throw new ArgumentException("A session id is required.", nameof(sessionId));
        }

        var cleaned = Clean(items);
        if (cleaned.Count == 0)
        {
            Clear(sessionId);
            return cleaned;
        }

        EnsureTable();
        _database.Execute(@"
            INSERT INTO ChatTaskLists (SessionId, ItemsJson, UpdatedAt) VALUES (@SessionId, @ItemsJson, @UpdatedAt)
            ON CONFLICT(SessionId) DO UPDATE SET ItemsJson = excluded.ItemsJson, UpdatedAt = excluded.UpdatedAt",
            new SqliteParameter("@SessionId", sessionId),
            new SqliteParameter("@ItemsJson", JsonSerializer.Serialize(cleaned, JsonOptions)),
            new SqliteParameter("@UpdatedAt", DateTime.UtcNow.ToString("O")));

        AutomatonDebug.Log($"ChatTaskListStore.Set: {sessionId} - {Describe(cleaned)}");
        Changed?.Invoke(sessionId, cleaned);
        return cleaned;
    }

    /// <summary>Removes the chat's list.</summary>
    public void Clear(string sessionId)
    {
        if (string.IsNullOrWhiteSpace(sessionId))
        {
            throw new ArgumentException("A session id is required.", nameof(sessionId));
        }

        EnsureTable();
        _database.Execute("DELETE FROM ChatTaskLists WHERE SessionId = @SessionId", new SqliteParameter("@SessionId", sessionId));
        AutomatonDebug.Log($"ChatTaskListStore.Clear: {sessionId}");
        Changed?.Invoke(sessionId, Array.Empty<TaskListItem>());
    }

    /// <summary>Drops blank lines, trims text, normalises statuses and caps the length.</summary>
    public static List<TaskListItem> Clean(IEnumerable<TaskListItem>? items)
    {
        return (items ?? Enumerable.Empty<TaskListItem>())
            .Where(item => item != null && !string.IsNullOrWhiteSpace(item.Text))
            .Take(MaxItems)
            .Select(item => new TaskListItem { Text = item.Text.Trim(), Status = TaskListItem.NormalizeStatus(item.Status) })
            .ToList();
    }

    /// <summary>"3 items: 1 done, 1 in progress, 1 to do" - for logs and tool output.</summary>
    public static string Describe(IReadOnlyCollection<TaskListItem> items)
    {
        if (items == null || items.Count == 0)
        {
            return "no items";
        }

        var done = items.Count(item => item.Status == TaskListItem.Completed);
        var doing = items.Count(item => item.Status == TaskListItem.InProgress);
        return $"{items.Count} item(s): {done} done, {doing} in progress, {items.Count - done - doing} to do";
    }

    /// <summary>The list as the model reads it: one line per item with a status mark.</summary>
    public static string Render(IReadOnlyList<TaskListItem> items)
    {
        var builder = new StringBuilder();
        for (var index = 0; index < items.Count; index++)
        {
            var item = items[index];
            var mark = item.Status switch
            {
                TaskListItem.Completed => "[x]",
                TaskListItem.InProgress => "[>]",
                _ => "[ ]"
            };
            builder.Append(index + 1).Append(". ").Append(mark).Append(' ').AppendLine(item.Text);
        }

        return builder.ToString().TrimEnd();
    }

    private void EnsureTable()
    {
        if (_tableReady)
        {
            return;
        }

        _database.Execute(@"
            CREATE TABLE IF NOT EXISTS ChatTaskLists (
                SessionId TEXT PRIMARY KEY,
                ItemsJson TEXT NOT NULL,
                UpdatedAt TEXT NOT NULL
            )");
        _tableReady = true;
    }
}

/// <summary>
/// Puts the chat's task list on each request, so the model keeps track of it through compaction and
/// is reminded to tick items off. Nothing when the chat has no list.
/// </summary>
public sealed class TaskListContextProvider : IRequestContextProvider
{
    private readonly Func<ChatTaskListStore> _store;

    public TaskListContextProvider()
        : this(() => ChatTaskListStore.Shared)
    {
    }

    public TaskListContextProvider(Func<ChatTaskListStore> store)
    {
        _store = store ?? throw new ArgumentNullException(nameof(store));
    }

    public string? BuildTransientContext(ToolContext context, IReadOnlyCollection<string> toolIds)
    {
        if (context == null || string.IsNullOrWhiteSpace(context.SessionId))
        {
            return null;
        }

        // A chat without the tool cannot update a list, so it is not shown one.
        if (toolIds != null && toolIds.Count > 0 && !toolIds.Contains(TaskListToolInfo.ToolId, StringComparer.OrdinalIgnoreCase))
        {
            return null;
        }

        var items = _store().Get(context.SessionId);
        if (items.Count == 0)
        {
            return null;
        }

        return "[Your task list - update it with task_list as each step starts and finishes; clear it when the work is done]\n" +
               ChatTaskListStore.Render(items);
    }
}

/// <summary>The task list tool's id, shared by the tool, the reminder and the chat view.</summary>
public static class TaskListToolInfo
{
    public const string ToolId = "task_list";

    /// <summary>The key in the tool result's metadata that carries the new list as JSON.</summary>
    public const string ItemsMetadataKey = "taskListItems";
}
