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
/// Service for calendar task CRUD and recurrence computation.
/// Backed by CalendarTasks and CalendarTaskOccurrences tables in config.db.
/// </summary>
public partial class CalendarService
{
    private readonly ConfigDatabase? _database;
    private ConfigDatabase Database => _database ?? ConfigDatabase.Instance;
    public CalendarService(ConfigDatabase? database = null) { _database = database; }
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = false,
        DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull
    };

    // ── CRUD ──────────────────────────────────────────────────────────

    public Task<CalendarTask> CreateTaskAsync(CalendarTask task)
    {
        return Task.Run(() =>
        {
            var db = Database;
            var now = DateTime.UtcNow;

            if (string.IsNullOrWhiteSpace(task.Id))
                task.Id = Guid.NewGuid().ToString();

            task.CreatedAt = now;
            task.UpdatedAt = now;

            db.Execute(@"
                INSERT INTO CalendarTasks (Id, TaskType, Title, Description, DueDate, DueTime,
                    Recurrence, RecurrenceInterval, EndAfter, EndDate, Tags, Status, CreatedAt, UpdatedAt,
                    TaskSessionId, IsWatch, SourceSessionId, WatchSessionId, Objective, IntervalMinutes,
                    NotifyCriteria, StopCondition, LastObservation, NextRunAt, LastRunAt,
                    RunCount, LastRunError, ScheduleJson, RunWithoutAsking)
                VALUES (@Id, @TaskType, @Title, @Description, @DueDate, @DueTime,
                    @Recurrence, @RecurrenceInterval, @EndAfter, @EndDate, @Tags, @Status, @CreatedAt, @UpdatedAt,
                    @TaskSessionId, @IsWatch, @SourceSessionId, @WatchSessionId, @Objective, @IntervalMinutes,
                    @NotifyCriteria, @StopCondition, @LastObservation, @NextRunAt, @LastRunAt,
                    @RunCount, @LastRunError, @ScheduleJson, @RunWithoutAsking)",
                new SqliteParameter("@Id", task.Id),
                new SqliteParameter("@TaskType", task.TaskType.ToString()),
                new SqliteParameter("@Title", task.Title),
                new SqliteParameter("@Description", (object?)task.Description ?? DBNull.Value),
                new SqliteParameter("@DueDate", task.DueDate.ToString("yyyy-MM-dd")),
                new SqliteParameter("@DueTime", (object?)task.DueTime ?? DBNull.Value),
                new SqliteParameter("@Recurrence", (object?)task.Recurrence?.ToString() ?? DBNull.Value),
                new SqliteParameter("@RecurrenceInterval", task.RecurrenceInterval < 1 ? 1 : task.RecurrenceInterval),
                new SqliteParameter("@EndAfter", (object?)task.EndAfter ?? DBNull.Value),
                new SqliteParameter("@EndDate", (object?)task.EndDate?.ToString("yyyy-MM-dd") ?? DBNull.Value),
                new SqliteParameter("@Tags", JsonSerializer.Serialize(task.Tags, JsonOptions)),
                new SqliteParameter("@Status", task.Status.ToString()),
                new SqliteParameter("@CreatedAt", task.CreatedAt.ToString("O")),
                new SqliteParameter("@UpdatedAt", task.UpdatedAt.ToString("O")),
                new SqliteParameter("@TaskSessionId", (object?)task.TaskSessionId ?? DBNull.Value),
                new SqliteParameter("@IsWatch", task.IsWatch ? 1 : 0),
                new SqliteParameter("@SourceSessionId", (object?)task.SourceSessionId ?? DBNull.Value),
                new SqliteParameter("@WatchSessionId", (object?)task.WatchSessionId ?? DBNull.Value),
                new SqliteParameter("@Objective", (object?)task.Objective ?? DBNull.Value),
                new SqliteParameter("@IntervalMinutes", (object?)task.IntervalMinutes ?? DBNull.Value),
                new SqliteParameter("@NotifyCriteria", (object?)task.NotifyCriteria ?? DBNull.Value),
                new SqliteParameter("@StopCondition", (object?)task.StopCondition ?? DBNull.Value),
                new SqliteParameter("@LastObservation", (object?)task.LastObservation ?? DBNull.Value),
                new SqliteParameter("@NextRunAt", (object?)task.NextRunAt?.ToString("O") ?? DBNull.Value),
                new SqliteParameter("@LastRunAt", (object?)task.LastRunAt?.ToString("O") ?? DBNull.Value),
                new SqliteParameter("@RunCount", task.RunCount),
                new SqliteParameter("@LastRunError", (object?)task.LastRunError ?? DBNull.Value),
                new SqliteParameter("@ScheduleJson", JsonSerializer.Serialize(task.Schedule)),
                new SqliteParameter("@RunWithoutAsking", task.RunWithoutAsking ? 1 : 0)
            );

            RaiseChanged();
            return task;
        });
    }

    public Task<List<CalendarTask>> ListTasksAsync(DateTime? from = null, DateTime? to = null,
        string? status = null, string? taskType = null, string? taskId = null)
    {
        return Task.Run(() =>
        {
            var db = Database;
            var results = new List<CalendarTask>();

            var sql = "SELECT * FROM CalendarTasks WHERE 1=1";
            var parameters = new List<SqliteParameter>();

            if (from.HasValue)
            {
                sql += " AND (DueDate >= @From OR TaskType = 'Recurring')";
                parameters.Add(new SqliteParameter("@From", from.Value.ToString("yyyy-MM-dd")));
            }
            if (to.HasValue)
            {
                sql += " AND DueDate <= @To";
                parameters.Add(new SqliteParameter("@To", to.Value.ToString("yyyy-MM-dd")));
            }
            var normalizedStatus = NormalizeStatusFilter(status);
            if (!string.IsNullOrWhiteSpace(normalizedStatus))
            {
                sql += " AND Status = @Status COLLATE NOCASE";
                parameters.Add(new SqliteParameter("@Status", normalizedStatus));
            }
            var normalizedTaskType = NormalizeTaskTypeFilter(taskType);
            if (!string.IsNullOrWhiteSpace(normalizedTaskType))
            {
                sql += " AND TaskType = @TaskType COLLATE NOCASE";
                parameters.Add(new SqliteParameter("@TaskType", normalizedTaskType));
            }
            if (!string.IsNullOrWhiteSpace(taskId))
            {
                sql += " AND Id = @Id";
                parameters.Add(new SqliteParameter("@Id", taskId));
            }

            sql += " ORDER BY DueDate ASC, DueTime ASC";

            var rows = db.Query(sql, parameters.ToArray());

            foreach (var row in rows)
            {
                var task = RowToTask(row);
                results.Add(task);
            }

            // Expand recurring tasks into individual occurrences for the date range. Only
            // date-based units (Daily/Weekly/Monthly) are expanded onto the calendar grid;
            // sub-day units (Minutely/Hourly) are execution-only and would generate thousands
            // of entries (and hang) if expanded, so they surface as a single entry on their
            // start date instead.
            var expanded = new List<CalendarTask>();
            foreach (var task in results)
            {
                if (task.TaskType == CalendarTaskType.Recurring
                    && task.Recurrence.HasValue
                    && IsDateBasedRecurrence(task.Recurrence.Value))
                {
                    var occurrences = ComputeOccurrences(task, from ?? DateTime.Today,
                        to ?? (from ?? DateTime.Today).AddDays(30));
                    foreach (var occDate in occurrences)
                    {
                        var occTask = CloneForOccurrence(task, occDate);
                        expanded.Add(occTask);
                    }
                }
                else
                {
                    expanded.Add(task);
                }
            }

            return expanded.OrderBy(t => t.DueDate).ThenBy(t => t.DueTime).ToList();
        });
    }

    public Task<CalendarTask?> GetTaskAsync(string taskId)
    {
        return Task.Run(() =>
        {
            var db = Database;
            var rows = db.Query("SELECT * FROM CalendarTasks WHERE Id = @Id",
                new SqliteParameter("@Id", taskId));

            if (rows.Count == 0)
                return null;

            return RowToTask(rows[0]);
        });
    }

    public Task DeleteTaskAsync(string taskId)
        => DeleteTaskAsync(taskId, onlyIfIdle: false);

    /// <summary>Optional atomic idle guard for editors racing a scheduler claim.</summary>
    public Task DeleteTaskAsync(string taskId, bool onlyIfIdle)
    {
        if (string.IsNullOrWhiteSpace(taskId))
        {
            throw new ArgumentException("taskId is required", nameof(taskId));
        }

        return Task.Run(() =>
        {
            AutomatonDebug.Log($"CalendarService.DeleteTaskAsync: Entry - taskId={taskId}");
            var db = Database;
            // Occurrences cascade-delete via FK
            var changed = db.Execute("DELETE FROM CalendarTasks WHERE Id = @Id AND (@OnlyIfIdle=0 OR ClaimedRunId IS NULL)",
                new SqliteParameter("@Id", taskId), new SqliteParameter("@OnlyIfIdle", onlyIfIdle ? 1 : 0));
            if (onlyIfIdle && changed != 1) throw new InvalidOperationException("The calendar entry is running or no longer exists. Stop its run and refresh before deleting.");
            RaiseChanged();
            AutomatonDebug.Log($"CalendarService.DeleteTaskAsync: Exit - taskId={taskId}");
        });
    }

    public Task CompleteTaskAsync(string taskId, DateTime? occurrenceDate = null)
    {
        return Task.Run(() =>
        {
            var db = Database;
            var now = DateTime.UtcNow;

            db.ExecuteInTransaction((_, _) =>
            {
                var row = db.Query("SELECT * FROM CalendarTasks WHERE Id=@Id", new SqliteParameter("@Id", taskId)).FirstOrDefault();
                if (row == null) return;
                var task = RowToTask(row);
                if (task.TaskType == CalendarTaskType.Recurring && occurrenceDate.HasValue)
                {
                    // Mark individual occurrence
                    db.Execute(@"
                        INSERT OR REPLACE INTO CalendarTaskOccurrences (TaskId, OccurrenceDate, Status, CompletedAt)
                        VALUES (@TaskId, @OccurrenceDate, @Status, @CompletedAt)",
                        new SqliteParameter("@TaskId", taskId),
                        new SqliteParameter("@OccurrenceDate", occurrenceDate.Value.ToString("yyyy-MM-dd")),
                        new SqliteParameter("@Status", CalendarTaskStatus.Completed.ToString()),
                        new SqliteParameter("@CompletedAt", now.ToString("O")));
                    ReconcileCompletedOccurrences(taskId, out var occurrenceAdvanced);
                }
                else
                {
                    // Mark one-off task as completed
                    db.Execute("UPDATE CalendarTasks SET Status = @Status, CompletedAt = @CompletedAt, UpdatedAt = @UpdatedAt WHERE Id = @Id",
                        new SqliteParameter("@Id", taskId),
                        new SqliteParameter("@Status", CalendarTaskStatus.Completed.ToString()),
                        new SqliteParameter("@CompletedAt", now.ToString("O")),
                        new SqliteParameter("@UpdatedAt", now.ToString("O")));
                }
            });
            RaiseChanged();
        });
    }

    public Task CancelTaskAsync(string taskId)
    {
        return Task.Run(() =>
        {
            var db = Database;
            var now = DateTime.UtcNow;

            db.Execute("UPDATE CalendarTasks SET Status = @Status, UpdatedAt = @UpdatedAt WHERE Id = @Id",
                new SqliteParameter("@Id", taskId),
                new SqliteParameter("@Status", CalendarTaskStatus.Cancelled.ToString()),
                new SqliteParameter("@UpdatedAt", now.ToString("O")));
        });
    }

    // ── Watch scheduling ──────────────────────────────────────────────

    /// <summary>
    /// Returns watches that are due to fire: IsWatch, still Pending, with a NextRunAt
    /// at or before <paramref name="now"/>. Returned tasks are raw rows (no recurrence
    /// expansion).
    /// </summary>
    public Task<List<CalendarTask>> ListDueWatchesAsync(DateTime now)
    {
        return Task.Run(() =>
        {
            var db = Database;
            var rows = db.Query(@"
                SELECT * FROM CalendarTasks
                WHERE IsWatch = 1
                  AND Status = @Status COLLATE NOCASE
                  AND NextRunAt IS NOT NULL
                  AND NextRunAt <= @Now
                ORDER BY NextRunAt ASC",
                new SqliteParameter("@Status", CalendarTaskStatus.Pending.ToString()),
                new SqliteParameter("@Now", now.ToString("O")));

            return rows.Select(RowToTask).ToList();
        });
    }

    /// <summary>
    /// Returns all watches (IsWatch=1) as raw rows, with no recurrence expansion. Use this
    /// for the calendar UI's watch list — never ListTasksAsync() without date bounds, which
    /// would expand unbounded recurring tasks from DateTime.MinValue to MaxValue and hang.
    /// </summary>
    public Task<List<CalendarTask>> ListWatchesAsync()
    {
        return Task.Run(() =>
        {
            var db = Database;
            var rows = db.Query(
                "SELECT * FROM CalendarTasks WHERE IsWatch = 1 ORDER BY NextRunAt ASC, UpdatedAt DESC");
            return rows.Select(RowToTask).ToList();
        });
    }

    /// <summary>
    /// Returns tasks that have TaskSessionId set (i.e., have been executed at least once),
    /// for viewing historic task conversations.
    /// </summary>
    public Task<List<CalendarTask>> ListExecutedTasksAsync(DateTime? from = null, DateTime? to = null)
    {
        return Task.Run(() =>
        {
            var db = Database;
            // Filter for non-watch tasks that have executed at least once
            var sql = "SELECT * FROM CalendarTasks WHERE TaskSessionId IS NOT NULL AND TaskSessionId != '' AND IsWatch = 0";
            var parameters = new List<SqliteParameter>();

            if (from.HasValue)
            {
                sql += " AND DueDate >= @From";
                parameters.Add(new SqliteParameter("@From", from.Value.ToString("yyyy-MM-dd")));
            }
            if (to.HasValue)
            {
                sql += " AND DueDate <= @To";
                parameters.Add(new SqliteParameter("@To", to.Value.ToString("yyyy-MM-dd")));
            }

            sql += " ORDER BY DueDate DESC, DueTime DESC";

            var rows = db.Query(sql, parameters.ToArray());
            return rows.Select(RowToTask).ToList();
        });
    }

    /// <summary>
    /// Records the outcome of a watch fire. Increments RunCount, sets LastRunAt to now,
    /// sets LastRunError (null clears it), and sets NextRunAt (null stops the watch from
    /// firing again). Expresses writes that UpdateTaskAsync cannot (null=skip semantics).
    /// </summary>
    public Task RecordWatchRunAsync(string taskId, DateTime? nextRunAt, string? error)
    {
        if (string.IsNullOrWhiteSpace(taskId))
        {
            throw new ArgumentException("taskId is required", nameof(taskId));
        }

        return Task.Run(() =>
        {
            var db = Database;
            var now = DateTime.UtcNow;

            db.Execute(@"
                UPDATE CalendarTasks
                SET RunCount = RunCount + 1,
                    LastRunAt = @LastRunAt,
                    LastRunError = @LastRunError,
                    NextRunAt = @NextRunAt,
                    UpdatedAt = @UpdatedAt
                WHERE Id = @Id",
                new SqliteParameter("@Id", taskId),
                new SqliteParameter("@LastRunAt", now.ToString("O")),
                new SqliteParameter("@LastRunError", (object?)error ?? DBNull.Value),
                new SqliteParameter("@NextRunAt", (object?)nextRunAt?.ToString("O") ?? DBNull.Value),
                new SqliteParameter("@UpdatedAt", now.ToString("O")));
        });
    }

    /// <summary>
    /// Returns all recurring, non-watch, pending tasks as raw rows (no recurrence expansion).
    /// The scheduler uses this to decide which recurring tasks are due to fire — date expansion
    /// would both hang on sub-day units and lose the per-task LastRunAt/RunCount bookkeeping.
    /// </summary>
    public Task<List<CalendarTask>> ListRecurringTasksAsync()
    {
        return Task.Run(() =>
        {
            var db = Database;
            var rows = db.Query(@"
                SELECT * FROM CalendarTasks
                WHERE IsWatch = 0
                  AND TaskType = @TaskType COLLATE NOCASE
                  AND Status = @Status COLLATE NOCASE
                ORDER BY DueDate ASC, DueTime ASC",
                new SqliteParameter("@TaskType", CalendarTaskType.Recurring.ToString()),
                new SqliteParameter("@Status", CalendarTaskStatus.Pending.ToString()));

            return rows.Select(RowToTask).ToList();
        });
    }

    /// <summary>
    /// Records a task fire: increments RunCount, sets LastRunAt to now and LastRunError
    /// (null clears it). Unlike <see cref="RecordWatchRunAsync"/> this never touches NextRunAt —
    /// recurring task scheduling is derived from LastRunAt + interval, not a stored NextRunAt.
    /// </summary>
    public Task RecordTaskRunAsync(string taskId, string? error)
    {
        if (string.IsNullOrWhiteSpace(taskId))
        {
            throw new ArgumentException("taskId is required", nameof(taskId));
        }

        return Task.Run(() =>
        {
            var db = Database;
            var now = DateTime.UtcNow;

            db.Execute(@"
                UPDATE CalendarTasks
                SET RunCount = RunCount + 1,
                    LastRunAt = @LastRunAt,
                    LastRunError = @LastRunError,
                    UpdatedAt = @UpdatedAt
                WHERE Id = @Id",
                new SqliteParameter("@Id", taskId),
                new SqliteParameter("@LastRunAt", now.ToString("O")),
                new SqliteParameter("@LastRunError", (object?)error ?? DBNull.Value),
                new SqliteParameter("@UpdatedAt", now.ToString("O")));
        });
    }

    /// <summary>
    /// Full update of the human-editable fields from the task editor dialog. Unlike
    /// <see cref="UpdateTaskAsync"/> (null = skip, used by the AI tool and scheduler), this writes
    /// every field explicitly — including NULLs — so the editor can clear recurrence/end conditions
    /// and flip a task between one-off and recurring. Watch/scheduler bookkeeping fields are left alone.
    /// </summary>
    public Task UpdateTaskFromEditorAsync(CalendarTask task)
    {
        if (task == null)
        {
            throw new ArgumentNullException(nameof(task));
        }
        if (string.IsNullOrWhiteSpace(task.Id))
        {
            throw new ArgumentException("task.Id is required", nameof(task));
        }

        return Task.Run(() =>
        {
            var db = Database;
            var now = DateTime.UtcNow;

            db.Execute(@"
                UPDATE CalendarTasks
                SET TaskType = @TaskType,
                    Title = @Title,
                    Description = @Description,
                    DueDate = @DueDate,
                    DueTime = @DueTime,
                    Recurrence = @Recurrence,
                    RecurrenceInterval = @RecurrenceInterval,
                    EndAfter = @EndAfter,
                    EndDate = @EndDate,
                    UpdatedAt = @UpdatedAt
                WHERE Id = @Id",
                new SqliteParameter("@Id", task.Id),
                new SqliteParameter("@TaskType", task.TaskType.ToString()),
                new SqliteParameter("@Title", task.Title ?? string.Empty),
                new SqliteParameter("@Description", (object?)task.Description ?? DBNull.Value),
                new SqliteParameter("@DueDate", task.DueDate.ToString("yyyy-MM-dd")),
                new SqliteParameter("@DueTime", (object?)task.DueTime ?? DBNull.Value),
                new SqliteParameter("@Recurrence", (object?)task.Recurrence?.ToString() ?? DBNull.Value),
                new SqliteParameter("@RecurrenceInterval", task.RecurrenceInterval < 1 ? 1 : task.RecurrenceInterval),
                new SqliteParameter("@EndAfter", (object?)task.EndAfter ?? DBNull.Value),
                new SqliteParameter("@EndDate", (object?)task.EndDate?.ToString("yyyy-MM-dd") ?? DBNull.Value),
                new SqliteParameter("@UpdatedAt", now.ToString("O")));
        });
    }

    public Task<List<CalendarTaskOccurrence>> GetOccurrencesAsync(string taskId)
    {
        return Task.Run(() =>
        {
            var db = Database;
            var rows = db.Query(
                "SELECT * FROM CalendarTaskOccurrences WHERE TaskId = @TaskId ORDER BY OccurrenceDate ASC",
                new SqliteParameter("@TaskId", taskId));

            return rows.Select(r => new CalendarTaskOccurrence
            {
                TaskId = r["TaskId"] as string ?? "",
                OccurrenceDate = DateTime.Parse(r["OccurrenceDate"] as string ?? ""),
                Status = Enum.TryParse<CalendarTaskStatus>(r["Status"] as string, out var s) ? s : CalendarTaskStatus.Pending,
                CompletedAt = r["CompletedAt"] is string ca ? DateTime.Parse(ca) : null
            }).ToList();
        });
    }

    // ── Recurrence Computation ────────────────────────────────────────

    public static List<DateTime> ComputeOccurrences(CalendarTask template, DateTime from, DateTime to)
    {
        var results = new List<DateTime>();
        if (!template.Recurrence.HasValue || !IsDateBasedRecurrence(template.Recurrence.Value)) return results;
        var step = Math.Max(1, template.RecurrenceInterval);
        for (int n = 0; n < (template.EndAfter ?? int.MaxValue); n++)
        {
            DateTime date;
            try
            {
                date = template.Recurrence.Value switch
                {
                    RecurrenceType.Daily => template.DueDate.Date.AddDays((long)n * step),
                    RecurrenceType.Weekly => template.DueDate.Date.AddDays((long)n * 7 * step),
                    _ => template.DueDate.Date.AddMonths(checked(n * step))
                };
            }
            catch (ArgumentOutOfRangeException) { break; }
            catch (OverflowException) { break; }
            if (date > to || date > (template.EndDate ?? DateTime.MaxValue)) break;
            if (date >= from && (!template.Schedule.WeekdaysOnly || date.DayOfWeek is not (DayOfWeek.Saturday or DayOfWeek.Sunday))) results.Add(date);
        }
        return results;
    }

    /// <summary>
    /// True for recurrence units that map onto calendar days (Daily/Weekly/Monthly) and can be
    /// expanded into per-day occurrences. Sub-day units (Minutely/Hourly) are execution-only.
    /// </summary>
    public static bool IsDateBasedRecurrence(RecurrenceType recurrence) =>
        recurrence is RecurrenceType.Daily or RecurrenceType.Weekly or RecurrenceType.Monthly;

    /// <summary>
    /// Adds one recurrence period (unit * interval) to <paramref name="from"/>.
    /// </summary>
    public static DateTime AddRecurrencePeriod(DateTime from, RecurrenceType unit, int interval)
    {
        var step = interval < 1 ? 1 : interval;
        return unit switch
        {
            RecurrenceType.Minutely => from.AddMinutes(step),
            RecurrenceType.Hourly => from.AddHours(step),
            RecurrenceType.Daily => from.AddDays(step),
            RecurrenceType.Weekly => from.AddDays(7 * step),
            RecurrenceType.Monthly => from.AddMonths(step),
            _ => from.AddDays(step)
        };
    }

    // ── Helpers ───────────────────────────────────────────────────────

    private static CalendarTask RowToTask(Dictionary<string, object?> row)
    {
        var task = new CalendarTask
        {
            Id = row["Id"] as string ?? "",
            TaskType = Enum.TryParse<CalendarTaskType>(row["TaskType"] as string, out var tt) ? tt : CalendarTaskType.OneOff,
            Title = row["Title"] as string ?? "",
            Description = row["Description"] as string,
            DueDate = DateTime.Parse(row["DueDate"] as string ?? DateTime.Today.ToString("yyyy-MM-dd")),
            DueTime = row["DueTime"] as string,
            Recurrence = Enum.TryParse<RecurrenceType>(row["Recurrence"] as string, out var rt) ? rt : null,
            RecurrenceInterval = row.TryGetValue("RecurrenceInterval", out var ri) && ri is long ril && ril >= 1 ? (int)ril : 1,
            EndAfter = row["EndAfter"] as long? is long ea ? (int)ea : null,
            EndDate = row["EndDate"] is string ed ? DateTime.Parse(ed) : null,
            Status = Enum.TryParse<CalendarTaskStatus>(row["Status"] as string, out var st) ? st : CalendarTaskStatus.Pending,
            CompletedAt = row["CompletedAt"] is string ca ? ParseTimestamp(ca) : null,
            CreatedAt = ParseTimestamp(row["CreatedAt"] as string) ?? DateTime.UtcNow,
            UpdatedAt = ParseTimestamp(row["UpdatedAt"] as string) ?? DateTime.UtcNow,
            IsWatch = row.TryGetValue("IsWatch", out var iw) && iw is long iwl && iwl != 0,
            RunWithoutAsking = row.TryGetValue("RunWithoutAsking", out var rwa) && rwa is long rwal && rwal != 0,
            SourceSessionId = row.TryGetValue("SourceSessionId", out var ssi) ? ssi as string : null,
            TaskSessionId = row.TryGetValue("TaskSessionId", out var tsi) ? tsi as string : null,
            WatchSessionId = row.TryGetValue("WatchSessionId", out var wsi) ? wsi as string : null,
            Objective = row.TryGetValue("Objective", out var obj) ? obj as string : null,
            IntervalMinutes = row.TryGetValue("IntervalMinutes", out var im) && im is long iml ? (int)iml : null,
            NotifyCriteria = row.TryGetValue("NotifyCriteria", out var nc) ? nc as string : null,
            StopCondition = row.TryGetValue("StopCondition", out var sc) ? sc as string : null,
            LastObservation = row.TryGetValue("LastObservation", out var lo) ? lo as string : null,
            NextRunAt = row.TryGetValue("NextRunAt", out var nra) && nra is string nras ? ParseTimestamp(nras) : null,
            LastRunAt = row.TryGetValue("LastRunAt", out var lra) && lra is string lras ? ParseTimestamp(lras) : null,
            RunCount = row.TryGetValue("RunCount", out var rcv) && rcv is long rcl ? (int)rcl : 0,
            LastRunError = row.TryGetValue("LastRunError", out var lre) ? lre as string : null,
            ClaimedRunId = row.TryGetValue("ClaimedRunId", out var cri) ? cri as string : null,
            ClaimedBy = row.TryGetValue("ClaimedBy", out var cby) ? cby as string : null,
            ClaimedAt = row.TryGetValue("ClaimedAt", out var cat) && cat is string cats ? ParseTimestamp(cats) : null
        };

        if (row.TryGetValue("ScheduleJson", out var settings) && settings is string settingsJson)
        {
            try { task.Schedule = JsonSerializer.Deserialize<CalendarScheduleOptions>(settingsJson) ?? new(); }
            catch (JsonException) { task.Schedule = new(); }
        }

        if (row["Tags"] is string tagsJson && !string.IsNullOrEmpty(tagsJson))
        {
            try { task.Tags = JsonSerializer.Deserialize<List<string>>(tagsJson) ?? new(); }
            catch { task.Tags = new(); }
        }

        return task;
    }

    /// <summary>
    /// Parses an ISO-8601 timestamp back to a UTC DateTime. Timestamps are stored with
    /// ToString("O") from UTC values (trailing 'Z'); plain DateTime.Parse would convert the
    /// 'Z' to local time and drop Kind, shifting every value by the local UTC offset. Using
    /// RoundtripKind preserves the encoded kind, and we normalise any local value back to UTC
    /// so comparisons against DateTime.UtcNow are correct.
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
            return null;

        return Enum.TryParse<CalendarTaskStatus>(status, ignoreCase: true, out var parsed)
            ? parsed.ToString()
            : status;
    }

    private static string? NormalizeTaskTypeFilter(string? taskType)
    {
        if (string.IsNullOrWhiteSpace(taskType))
            return null;

        return taskType.Trim().ToLowerInvariant() switch
        {
            "one_off" or "oneoff" => CalendarTaskType.OneOff.ToString(),
            "recurring" => CalendarTaskType.Recurring.ToString(),
            _ when Enum.TryParse<CalendarTaskType>(taskType, ignoreCase: true, out var parsed) => parsed.ToString(),
            _ => taskType
        };
    }

    private static CalendarTask CloneForOccurrence(CalendarTask template, DateTime date)
    {
        return new CalendarTask
        {
            Id = template.Id,
            TaskType = template.TaskType,
            Title = template.Title,
            Description = template.Description,
            DueDate = date,
            DueTime = template.DueTime,
            Recurrence = template.Recurrence,
            RecurrenceInterval = template.RecurrenceInterval,
            EndAfter = template.EndAfter,
            EndDate = template.EndDate,
            Status = template.Status,
            Tags = template.Tags,
            Schedule = template.Schedule,
            IsWatch = template.IsWatch,
            RunWithoutAsking = template.RunWithoutAsking,
            NextRunAt = template.NextRunAt,
            LastRunError = template.LastRunError,
            TaskSessionId = template.TaskSessionId,
            SourceSessionId = template.SourceSessionId,
            LastRunAt = template.LastRunAt,
            RunCount = template.RunCount,
            CreatedAt = template.CreatedAt,
            UpdatedAt = template.UpdatedAt
        };
    }
}
