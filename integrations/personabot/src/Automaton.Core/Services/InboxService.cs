using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using Automaton.Core.Models;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services;

/// <summary>
/// Service for inbox alert CRUD. The inbox is a single place where AI-posted alerts
/// from any conversation are collected for the human to monitor. Backed by the Inbox
/// table in config.db. Adding an item raises <see cref="SystemEvents.RaiseInboxItemAdded"/>
/// so open UIs can update a badge live.
/// </summary>
public class InboxService
{
    private readonly ConfigDatabase? _database;

    /// <summary>Uses the shared config.db unless a specific database is supplied (tests).</summary>
    public InboxService(ConfigDatabase? database = null)
    {
        _database = database;
    }

    private ConfigDatabase Database => _database ?? ConfigDatabase.Instance;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = false,
        DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull
    };

    public Task<InboxItem> AddAsync(InboxItem item)
    {
        if (item == null)
        {
            throw new ArgumentNullException(nameof(item));
        }

        return Task.Run(() =>
        {
            AutomatonDebug.Log($"InboxService.AddAsync: Entry - sourceSessionId={item.SourceSessionId}, sourceTaskId={item.SourceTaskId}");

            var db = Database;

            if (string.IsNullOrWhiteSpace(item.Id))
            {
                item.Id = Guid.NewGuid().ToString();
            }

            if (item.CreatedAt == default)
            {
                item.CreatedAt = DateTime.UtcNow;
            }

            db.Execute(@"
                INSERT INTO Inbox (Id, SourceSessionId, SourceTaskId, Title, Message, Options, Status, CreatedAt, ReadAt)
                VALUES (@Id, @SourceSessionId, @SourceTaskId, @Title, @Message, @Options, @Status, @CreatedAt, @ReadAt)",
                new SqliteParameter("@Id", item.Id),
                new SqliteParameter("@SourceSessionId", (object?)item.SourceSessionId ?? DBNull.Value),
                new SqliteParameter("@SourceTaskId", (object?)item.SourceTaskId ?? DBNull.Value),
                new SqliteParameter("@Title", (object?)item.Title ?? DBNull.Value),
                new SqliteParameter("@Message", item.Message),
                new SqliteParameter("@Options", JsonSerializer.Serialize(item.Options, JsonOptions)),
                new SqliteParameter("@Status", item.Status.ToString()),
                new SqliteParameter("@CreatedAt", item.CreatedAt.ToString("O")),
                new SqliteParameter("@ReadAt", (object?)item.ReadAt?.ToString("O") ?? DBNull.Value));

            SystemEvents.RaiseInboxItemAdded(item);

            AutomatonDebug.Log($"InboxService.AddAsync: Exit - id={item.Id}");
            return item;
        });
    }

    public Task<List<InboxItem>> ListAsync(string? status = null)
    {
        return Task.Run(() =>
        {
            var db = Database;
            var sql = "SELECT * FROM Inbox WHERE 1=1";
            var parameters = new List<SqliteParameter>();

            var normalizedStatus = NormalizeStatusFilter(status);
            if (!string.IsNullOrWhiteSpace(normalizedStatus))
            {
                sql += " AND Status = @Status COLLATE NOCASE";
                parameters.Add(new SqliteParameter("@Status", normalizedStatus));
            }

            sql += " ORDER BY CreatedAt DESC";

            var rows = db.Query(sql, parameters.ToArray());
            return rows.Select(RowToItem).ToList();
        });
    }

    public Task<InboxItem?> GetAsync(string id)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("id is required", nameof(id));
        }

        return Task.Run(() =>
        {
            var db = Database;
            var rows = db.Query("SELECT * FROM Inbox WHERE Id = @Id", new SqliteParameter("@Id", id));
            return rows.Count == 0 ? null : RowToItem(rows[0]);
        });
    }

    public Task MarkReadAsync(string id)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("id is required", nameof(id));
        }

        return Task.Run(() =>
        {
            var db = Database;
            // A stale notification tap must never bring an already archived item back into the inbox.
            db.Execute("UPDATE Inbox SET Status = @Status, ReadAt = @ReadAt WHERE Id = @Id AND Status = 'Unread' COLLATE NOCASE",
                new SqliteParameter("@Id", id),
                new SqliteParameter("@Status", InboxItemStatus.Read.ToString()),
                new SqliteParameter("@ReadAt", DateTime.UtcNow.ToString("O")));
            SystemEvents.RaiseInboxStatusChanged();
        });
    }

    public Task ArchiveAsync(string id)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("id is required", nameof(id));
        }

        return Task.Run(() =>
        {
            var db = Database;
            db.Execute("UPDATE Inbox SET Status = @Status WHERE Id = @Id",
                new SqliteParameter("@Id", id),
                new SqliteParameter("@Status", InboxItemStatus.Archived.ToString()));
            SystemEvents.RaiseInboxStatusChanged();
        });
    }

    public Task<int> GetUnreadCountAsync()
    {
        return Task.Run(() =>
        {
            var db = Database;
            return (int)db.ExecuteScalar<long>(
                "SELECT COUNT(*) FROM Inbox WHERE Status = @Status COLLATE NOCASE",
                new SqliteParameter("@Status", InboxItemStatus.Unread.ToString()));
        });
    }

    private static InboxItem RowToItem(Dictionary<string, object?> row)
    {
        var item = new InboxItem
        {
            Id = row["Id"] as string ?? "",
            SourceSessionId = row.TryGetValue("SourceSessionId", out var ssi) ? ssi as string : null,
            SourceTaskId = row.TryGetValue("SourceTaskId", out var sti) ? sti as string : null,
            Title = row.TryGetValue("Title", out var title) ? title as string : null,
            Message = row["Message"] as string ?? "",
            Status = Enum.TryParse<InboxItemStatus>(row["Status"] as string, ignoreCase: true, out var st)
                ? st
                : InboxItemStatus.Unread,
            CreatedAt = ParseTimestamp(row["CreatedAt"] as string) ?? DateTime.UtcNow,
            ReadAt = row.TryGetValue("ReadAt", out var ra) && ra is string ras ? ParseTimestamp(ras) : null
        };

        if (row.TryGetValue("Options", out var opt) && opt is string optJson && !string.IsNullOrEmpty(optJson))
        {
            try { item.Options = JsonSerializer.Deserialize<List<string>>(optJson) ?? new(); }
            catch { item.Options = new(); }
        }

        return item;
    }

    /// <summary>
    /// Parses an ISO-8601 timestamp back to a UTC DateTime, preserving the encoded kind.
    /// See CalendarService.ParseTimestamp for the rationale (plain DateTime.Parse shifts
    /// 'Z' values by the local UTC offset).
    /// </summary>
    private static DateTime? ParseTimestamp(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }

        if (!DateTime.TryParse(value, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind, out var parsed))
        {
            return null;
        }

        return parsed.Kind == DateTimeKind.Local ? parsed.ToUniversalTime() : parsed;
    }

    private static string? NormalizeStatusFilter(string? status)
    {
        if (string.IsNullOrWhiteSpace(status))
        {
            return null;
        }

        return Enum.TryParse<InboxItemStatus>(status, ignoreCase: true, out var parsed)
            ? parsed.ToString()
            : status;
    }
}
