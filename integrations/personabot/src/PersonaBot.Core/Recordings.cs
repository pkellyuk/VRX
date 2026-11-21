using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using Automaton.Core.Interfaces;
using Automaton.Core.Services.ComputerUse;

namespace PersonaBot;

/// <summary>A screenshot (one frame) or a screen recording (frames over time).</summary>
public enum RecordingKind
{
    Screenshot,
    Video
}

/// <summary>
/// Evidence on the person's timeline: what the desktop showed (with the bots' marks and pointer drawn in),
/// who captured it and why. Frames are JPEG files in the recording's own folder, listed with their time
/// offsets in frames.txt.
/// </summary>
public sealed record Recording(
    string Id, RecordingKind Kind, string? PersonaId, string? ChatId, string MadeBy, string Reason, string? Summary,
    DateTimeOffset StartedAt, DateTimeOffset? EndedAt, string Folder, int FrameCount)
{
    public const string FramesFile = "frames.txt";

    /// <summary>A video still being recorded.</summary>
    public bool IsLive => Kind == RecordingKind.Video && EndedAt == null;

    public TimeSpan Duration => (EndedAt ?? DateTimeOffset.UtcNow) - StartedAt;

    public static string FrameName(int index) => $"frame-{index:D5}.jpg";

    public string FramePath(int index) => Path.Combine(Folder, FrameName(index));

    /// <summary>The frames written so far, in order, with their offsets from the start.</summary>
    public IReadOnlyList<(int Index, TimeSpan Offset)> Frames()
    {
        var list = Path.Combine(Folder, FramesFile);
        if (!File.Exists(list)) return [];
        var frames = new List<(int, TimeSpan)>();
        using var stream = new FileStream(list, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using var reader = new StreamReader(stream);
        while (reader.ReadLine() is { } line)
        {
            var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 2 && int.TryParse(parts[0], CultureInfo.InvariantCulture, out var index) && long.TryParse(parts[1], CultureInfo.InvariantCulture, out var ms))
                frames.Add((index, TimeSpan.FromMilliseconds(ms)));
        }
        return frames;
    }
}

/// <summary>The recordings, in PersonaBot's database (PersonaBotRecordings), with their frames under Recordings/ in the data folder.</summary>
public sealed class RecordingStore
{
    private readonly Automaton.Core.Services.ConfigDatabase _db;

    public RecordingStore()
    {
        Trace.WriteLine($"RecordingStore ctor: recordings in {Directory}");
        _db = PersonaBotDatabase.EnsureReady();
    }

    /// <summary>Where recordings' folders go.</summary>
    public static string Directory => Path.Combine(Settings.Root, "Recordings");

    /// <summary>The newest recordings, oldest first (the timeline's order), for one persona or for everyone.</summary>
    public IReadOnlyList<Recording> List(string? personaId, int limit = 200, string? projectId = null)
    {
        if (limit < 1)
        {
            Trace.TraceWarning($"RecordingStore.List: refused limit {limit}");
            throw new ArgumentOutOfRangeException(nameof(limit));
        }
        Trace.WriteLine($"RecordingStore.List enter: persona {personaId ?? "everyone"}, project {projectId ?? "any"}, limit {limit}");
        var rows = _db.Query(@"SELECT * FROM PersonaBotRecordings
            WHERE (@Persona IS NULL OR PersonaId = @Persona)
              AND (@Project IS NULL OR ChatId IN (SELECT Id FROM Threads WHERE ProjectId = @Project))
            ORDER BY StartedAt DESC LIMIT @Limit", ChatStore.P("@Persona", personaId),
            ChatStore.P("@Project", projectId), ChatStore.P("@Limit", limit));
        var list = rows.Select(Read).Reverse().ToList();
        Trace.WriteLine($"RecordingStore.List exit: {list.Count} recordings ({list.Count(r => r.IsLive)} live)");
        return list;
    }

    public Recording? Get(string id)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(id);
        var recording = _db.Query("SELECT * FROM PersonaBotRecordings WHERE Id = @Id", ChatStore.P("@Id", id)).Select(Read).FirstOrDefault();
        Trace.WriteLine($"RecordingStore.Get: {id} {(recording == null ? "not found" : $"{recording.Kind}, {recording.FrameCount} frames")}");
        return recording;
    }

    /// <summary>Saves a screenshot: one frame, finished at once.</summary>
    public Recording AddScreenshot(byte[] jpeg, string reason, string madeBy, string? personaId, string? chatId)
    {
        ArgumentNullException.ThrowIfNull(jpeg);
        Trace.WriteLine($"RecordingStore.AddScreenshot enter: {jpeg.Length} bytes by {madeBy}, persona {personaId ?? "none"}, chat {chatId ?? "none"}");
        var recording = Begin(RecordingKind.Screenshot, reason, madeBy, personaId, chatId);
        File.WriteAllBytes(recording.FramePath(1), jpeg);
        File.WriteAllText(Path.Combine(recording.Folder, Recording.FramesFile), "1 0\n");
        var done = recording with { EndedAt = recording.StartedAt, FrameCount = 1 };
        Finish(done);
        Trace.WriteLine($"RecordingStore.AddScreenshot: {done.Id} by {madeBy}: {reason}");
        return done;
    }

    /// <summary>Starts a video: registered at once (so the timeline shows it live), frames added by <see cref="ScreenRecording"/>.</summary>
    public ScreenRecording StartVideo(string reason, string madeBy, string? personaId, string? chatId)
    {
        Trace.WriteLine($"RecordingStore.StartVideo enter: by {madeBy}, persona {personaId ?? "none"}, chat {chatId ?? "none"}");
        var recording = Begin(RecordingKind.Video, reason, madeBy, personaId, chatId);
        Trace.WriteLine($"RecordingStore.StartVideo: {recording.Id} by {madeBy}: {reason}");
        return new ScreenRecording(this, recording);
    }

    /// <summary>Deletes a recording and its frames.</summary>
    public void Delete(string id)
    {
        Trace.WriteLine($"RecordingStore.Delete enter: {id}");
        var recording = Get(id) ?? throw new ArgumentException($"There is no recording '{id}'.");
        _db.Execute("DELETE FROM PersonaBotRecordings WHERE Id = @Id", ChatStore.P("@Id", id));
        if (System.IO.Directory.Exists(recording.Folder)) System.IO.Directory.Delete(recording.Folder, recursive: true);
        else Trace.WriteLine($"RecordingStore.Delete: {id} had no folder at {recording.Folder}");
        Trace.WriteLine($"RecordingStore.Delete: {id}");
    }

    internal void Finish(Recording recording)
    {
        Trace.WriteLine($"RecordingStore.Finish: {recording.Id} ({recording.Kind}), {recording.FrameCount} frames, summary {recording.Summary?.Length ?? 0} chars");
        _db.Execute("UPDATE PersonaBotRecordings SET Summary = @Summary, EndedAt = @Ended, FrameCount = @Frames WHERE Id = @Id",
            ChatStore.P("@Summary", (object?)recording.Summary ?? DBNull.Value),
            ChatStore.P("@Ended", recording.EndedAt?.ToString("O", CultureInfo.InvariantCulture) ?? (object)DBNull.Value),
            ChatStore.P("@Frames", recording.FrameCount), ChatStore.P("@Id", recording.Id));
    }

    private Recording Begin(RecordingKind kind, string reason, string madeBy, string? personaId, string? chatId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(madeBy);
        var why = (reason ?? "").Trim();
        if (why.Length == 0)
        {
            Trace.TraceWarning($"RecordingStore.Begin: refused a {kind} by {madeBy}: no reason given");
            throw new ArgumentException("Say why: a recording needs a reason.");
        }
        if (why.Length > 300)
        {
            Trace.TraceWarning($"RecordingStore.Begin: refused a {kind} by {madeBy}: reason of {why.Length} chars");
            throw new ArgumentException("Keep the reason to 300 characters.");
        }
        var started = DateTimeOffset.UtcNow;
        var id = started.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture) + "-" + Guid.NewGuid().ToString("N")[..6];
        var folder = Path.Combine(Directory, id);
        System.IO.Directory.CreateDirectory(folder);
        var recording = new Recording(id, kind, personaId, chatId, madeBy.Trim(), why, null, started, null, folder, 0);
        _db.Execute(@"INSERT INTO PersonaBotRecordings (Id, Kind, PersonaId, ChatId, MadeBy, Reason, StartedAt, Folder, FrameCount)
                VALUES (@Id, @Kind, @Persona, @Chat, @By, @Reason, @Started, @Folder, 0)",
            ChatStore.P("@Id", id), ChatStore.P("@Kind", kind.ToString()), ChatStore.P("@Persona", (object?)personaId ?? DBNull.Value),
            ChatStore.P("@Chat", (object?)chatId ?? DBNull.Value), ChatStore.P("@By", recording.MadeBy), ChatStore.P("@Reason", why),
            ChatStore.P("@Started", started.ToString("O", CultureInfo.InvariantCulture)), ChatStore.P("@Folder", id));
        Trace.WriteLine($"RecordingStore.Begin: {kind} {id} in {folder}");
        return recording;
    }

    private static Recording Read(Dictionary<string, object?> row) => new(
        (string)row["Id"]!, Enum.Parse<RecordingKind>((string)row["Kind"]!), row["PersonaId"] as string, row["ChatId"] as string,
        (string)row["MadeBy"]!, (string)row["Reason"]!, row["Summary"] as string,
        DateTimeOffset.Parse((string)row["StartedAt"]!, CultureInfo.InvariantCulture),
        row["EndedAt"] is string ended ? DateTimeOffset.Parse(ended, CultureInfo.InvariantCulture) : null,
        Path.Combine(Directory, (string)row["Folder"]!), Convert.ToInt32(row["FrameCount"], CultureInfo.InvariantCulture));
}

/// <summary>
/// A video being recorded: frames are written as they arrive, up to <see cref="MaxDuration"/>; then it
/// stops itself. Thread-safe: frames come from the screen feed, Stop from a tool or a button.
/// </summary>
public sealed class ScreenRecording
{
    public static readonly TimeSpan MaxDuration = TimeSpan.FromMinutes(10);

    private readonly RecordingStore _store;
    private readonly object _gate = new();
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private Recording _recording;

    internal ScreenRecording(RecordingStore store, Recording recording)
    {
        _store = store;
        _recording = recording;
        Trace.WriteLine($"ScreenRecording ctor: {recording.Id}, limit {MaxDuration.TotalMinutes:0} minutes");
    }

    public Recording Recording { get { lock (_gate) return _recording; } }

    public TimeSpan Elapsed => _clock.Elapsed;

    public bool IsStopped { get { lock (_gate) return _recording.EndedAt != null; } }

    /// <summary>Raised once when the recording stops (by request or at the time limit), with the finished recording.</summary>
    public event Action<Recording>? Stopped;

    /// <summary>Adds a frame; returns false once the recording has stopped. At the time limit it stops itself.</summary>
    public bool AddFrame(byte[] jpeg)
    {
        ArgumentNullException.ThrowIfNull(jpeg);
        Recording? finishedNow = null;
        lock (_gate)
        {
            if (_recording.EndedAt != null) return false;
            var index = _recording.FrameCount + 1;
            File.WriteAllBytes(_recording.FramePath(index), jpeg);
            File.AppendAllText(Path.Combine(_recording.Folder, Recording.FramesFile), $"{index} {(long)_clock.Elapsed.TotalMilliseconds}\n");
            _recording = _recording with { FrameCount = index };
            if (_clock.Elapsed >= MaxDuration) finishedNow = FinishLocked($"Stopped at the {MaxDuration.TotalMinutes:0}-minute limit.");
        }
        if (finishedNow != null) Stopped?.Invoke(finishedNow);
        return true;
    }

    /// <summary>Stops the recording with a summary of what it shows; returns the finished recording. Stopping twice is harmless.</summary>
    public Recording Stop(string? summary)
    {
        Trace.WriteLine($"ScreenRecording.Stop enter: {Recording.Id}, summary {summary?.Length ?? 0} chars");
        Recording finished;
        lock (_gate)
        {
            if (_recording.EndedAt != null)
            {
                Trace.WriteLine($"ScreenRecording.Stop exit: {_recording.Id} had already stopped");
                return _recording;
            }
            finished = FinishLocked(summary);
        }
        Stopped?.Invoke(finished);
        return finished;
    }

    private Recording FinishLocked(string? summary)
    {
        var clean = string.IsNullOrWhiteSpace(summary) ? null : summary.Trim();
        if (clean is { Length: > 1000 }) clean = clean[..1000];
        _recording = _recording with { Summary = clean, EndedAt = DateTimeOffset.UtcNow };
        _store.Finish(_recording);
        Trace.WriteLine($"ScreenRecording: {_recording.Id} stopped with {_recording.FrameCount} frames after {_clock.Elapsed.TotalSeconds:F0}s");
        return _recording;
    }
}

/// <summary>What the app gives the bots' recording tools: it knows the screen, the persona and the chat.</summary>
public interface IScreenRecorder
{
    /// <summary>The video being recorded, if any.</summary>
    Recording? Active { get; }

    Task<Recording> SnapshotAsync(string reason, string madeBy, CancellationToken token);

    Task<Recording> StartAsync(string reason, string madeBy, CancellationToken token);

    /// <summary>Stops the video being recorded; null when none was.</summary>
    Task<Recording?> StopAsync(string? summary, CancellationToken token);
}

/// <summary>
/// cu_snapshot: a bot keeps one moment of the screen on the person's timeline, with why. Mark what matters
/// with cu_mark first: the screenshot includes the marks.
/// </summary>
public sealed class SnapshotTool(Func<IScreenRecorder?> recorder, string madeBy = "Bot") : ITool
{
    public string Id => "cu_snapshot";

    public string Description =>
        "Save a screenshot of the desktop to the person's timeline, with the reason you took it (for example an issue you found, or a result). " +
        "It includes your cu_mark marks and pointer, so mark what matters first. The person can open it later; it is evidence, so say exactly what it shows.";

    public JsonSchema Parameters { get; } = new()
    {
        Properties = new Dictionary<string, JsonSchemaProperty>
        {
            ["reason"] = new() { Type = "string", Description = "What this screenshot shows and why it matters, in one or two sentences." }
        },
        Required = ["reason"]
    };

    /// <summary>The same tool for one persona, so the timeline says which bot took it.</summary>
    public SnapshotTool For(string persona) => new(recorder, persona);

    public async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        ArgumentNullException.ThrowIfNull(args);
        Trace.WriteLine($"SnapshotTool.ExecuteAsync enter: by {madeBy}, chat {context.SessionId}");
        if (recorder() == null) Trace.TraceWarning("SnapshotTool.ExecuteAsync: no recorder in this window");
        var target = recorder() ?? throw new InvalidOperationException("Screenshots aren't available in this window.");
        using var doc = JsonDocument.Parse(args);
        var reason = doc.RootElement.TryGetProperty("reason", out var r) && r.ValueKind == JsonValueKind.String ? r.GetString() ?? "" : "";
        var saved = await target.SnapshotAsync(reason, madeBy, context.CancellationToken);
        Trace.WriteLine($"SnapshotTool.ExecuteAsync exit: saved {saved.Id}");
        return new ToolResult { Title = "Screenshot saved", Output = $"Saved a screenshot to the person's timeline at {saved.StartedAt.ToLocalTime():HH:mm:ss}: \"{saved.Reason}\"." };
    }
}

/// <summary>
/// cu_record: a bot records the screen while it reproduces a problem or demonstrates something, then
/// stops with a summary. The recording appears live on the person's timeline.
/// </summary>
public sealed class RecordTool(Func<IScreenRecorder?> recorder, string madeBy = "Bot") : ITool
{
    public string Id => "cu_record";

    public string Description =>
        "Record the desktop as a video on the person's timeline. Use action=start with a reason before you reproduce an issue or demonstrate steps, " +
        "do the steps visibly, then action=stop with a summary of what the recording shows. Recordings include your marks and pointer, " +
        $"and stop by themselves after {ScreenRecording.MaxDuration.TotalMinutes:0} minutes.";

    public JsonSchema Parameters { get; } = new()
    {
        Properties = new Dictionary<string, JsonSchemaProperty>
        {
            ["action"] = new() { Type = "string", Description = "start or stop.", Enum = ["start", "stop"] },
            ["reason"] = new() { Type = "string", Description = "For start: what you are about to record and why." },
            ["summary"] = new() { Type = "string", Description = "For stop: what the recording shows (the issue found, or the outcome)." }
        },
        Required = ["action"]
    };

    public RecordTool For(string persona) => new(recorder, persona);

    public async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        ArgumentNullException.ThrowIfNull(args);
        if (recorder() == null) Trace.TraceWarning("RecordTool.ExecuteAsync: no recorder in this window");
        var target = recorder() ?? throw new InvalidOperationException("Recording isn't available in this window.");
        using var doc = JsonDocument.Parse(args);
        string Text(string name) => doc.RootElement.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
        Trace.WriteLine($"RecordTool.ExecuteAsync enter: action \"{Text("action")}\" by {madeBy}, chat {context.SessionId}");
        switch (Text("action"))
        {
            case "start":
                if (target.Active is { } active)
                {
                    Trace.TraceWarning($"RecordTool.ExecuteAsync: refused start: {active.Id} by {active.MadeBy} is already recording");
                    throw new InvalidOperationException($"A recording is already running (\"{active.Reason}\", by {active.MadeBy}). Stop it first.");
                }
                var started = await target.StartAsync(Text("reason"), madeBy, context.CancellationToken);
                Trace.WriteLine($"RecordTool.ExecuteAsync exit: started {started.Id}");
                return new ToolResult { Title = "Recording", Output = $"Recording the screen to the person's timeline: \"{started.Reason}\". Do the steps visibly, then call cu_record with action=stop and a summary." };
            case "stop":
                var stopped = await target.StopAsync(Text("summary"), context.CancellationToken);
                Trace.WriteLine($"RecordTool.ExecuteAsync exit: {(stopped == null ? "nothing was recording" : $"stopped {stopped.Id}, {stopped.FrameCount} frames")}");
                return stopped == null
                    ? new ToolResult { Title = "Recording", Output = "No recording was running." }
                    : new ToolResult { Title = "Recording saved", Output = $"Saved a {stopped.Duration.TotalSeconds:0}-second recording ({stopped.FrameCount} frames) to the person's timeline." };
            default:
                Trace.TraceWarning($"RecordTool.ExecuteAsync: refused unknown action \"{Text("action")}\"");
                throw new ArgumentException("action must be start or stop.");
        }
    }
}
