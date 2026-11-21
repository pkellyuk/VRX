using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using Automaton.Core.Interfaces;

namespace PersonaBot;

/// <summary>The tags of one recorded frame.</summary>
public sealed record FrameTags(int Frame, long Ms, List<ScreenTag> Tags);

/// <summary>Where one label appears in one recording: its spans of time, each with the frame to show and how many there were at most.</summary>
public sealed record RecordingMatch(Recording Recording, string Label, IReadOnlyList<(TimeSpan From, TimeSpan To, int Frame, int Most)> Spans);

/// <summary>
/// xidentify for the timeline: each recording's frames tagged by the local model into tags.jsonl beside them
/// (one line per frame), so recordings can be searched for what they show ("dog", "person", "laptop").
/// Tagging resumes where it stopped: frames already in the file are skipped.
/// </summary>
public static class RecordingTags
{
    public const string TagsFile = "tags.jsonl";

    /// <summary>Two sightings of a label closer than this are one span.</summary>
    public static readonly TimeSpan SpanGap = TimeSpan.FromSeconds(1.5);

    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);

    public static string PathOf(Recording recording)
    {
        ArgumentNullException.ThrowIfNull(recording);
        return Path.Combine(recording.Folder, TagsFile);
    }

    /// <summary>The frames tagged so far.</summary>
    public static IReadOnlyList<FrameTags> Read(Recording recording)
    {
        var path = PathOf(recording);
        if (!File.Exists(path)) return [];
        var frames = new List<FrameTags>();
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using var reader = new StreamReader(stream);
        while (reader.ReadLine() is { } line)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            try
            {
                if (JsonSerializer.Deserialize<FrameTags>(line, Json) is { } frame) frames.Add(frame);
            }
            catch (JsonException ex)
            {
                // A line cut short when the app stopped mid-write: that frame is tagged again next time.
                Trace.WriteLine($"RecordingTags.Read: {recording.Id}: skipped a broken line ({ex.Message})");
            }
        }
        return frames;
    }

    /// <summary>Whether every frame of a finished recording is tagged.</summary>
    public static bool IsComplete(Recording recording)
    {
        ArgumentNullException.ThrowIfNull(recording);
        if (recording.IsLive) return false;
        var tagged = Read(recording).Select(f => f.Frame).ToHashSet();
        return recording.Frames().All(f => tagged.Contains(f.Index));
    }

    /// <summary>Tags the frames not tagged yet; returns how many it tagged.</summary>
    public static async Task<int> TagAsync(Recording recording, IScreenTagger tagger, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(recording);
        ArgumentNullException.ThrowIfNull(tagger);
        var watch = Stopwatch.StartNew();
        Trace.WriteLine($"RecordingTags.TagAsync enter: {recording.Id}, {recording.FrameCount} frames");
        var done = Read(recording).Select(f => f.Frame).ToHashSet();
        var count = 0;
        var missing = 0;
        Trace.WriteLine($"RecordingTags.TagAsync: {recording.Id} has {done.Count} frames tagged already");
        // A line cut short (the app stopped mid-write) has no newline: start afresh after it, or the next line joins it.
        var file = PathOf(recording);
        if (File.Exists(file) && new FileInfo(file).Length > 0)
        {
            await using var check = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            check.Seek(-1, SeekOrigin.End);
            if (check.ReadByte() != '\n')
            {
                await check.DisposeAsync();
                await File.AppendAllTextAsync(file, "\n", token);
            }
        }
        foreach (var (index, offset) in recording.Frames())
        {
            token.ThrowIfCancellationRequested();
            if (done.Contains(index)) continue;
            var path = recording.FramePath(index);
            if (!File.Exists(path))
            {
                missing++;
                continue;
            }
            var result = await ScreenTags.TagImageAsync(tagger, await File.ReadAllBytesAsync(path, token), new ScreenTagRequest(), token);
            var line = JsonSerializer.Serialize(new FrameTags(index, (long)offset.TotalMilliseconds, result.Tags.ToList()), Json);
            await File.AppendAllTextAsync(PathOf(recording), line + "\n", token);
            count++;
        }
        if (missing > 0) Trace.TraceWarning($"RecordingTags.TagAsync: {recording.Id}: {missing} frame file(s) missing");
        Trace.WriteLine($"RecordingTags.TagAsync exit: {recording.Id}, tagged {count} frames in {watch.Elapsed.TotalSeconds:F1} s");
        return count;
    }

    /// <summary>
    /// Where the recordings show <paramref name="query"/>: each word is matched against tag names ("person",
    /// "dog"; "control" for controls). Results are per recording and label, newest recording first.
    /// </summary>
    public static IReadOnlyList<RecordingMatch> Search(IEnumerable<Recording> recordings, string query)
    {
        ArgumentNullException.ThrowIfNull(recordings);
        if (string.IsNullOrWhiteSpace(query))
        {
            Trace.WriteLine("RecordingTags.Search: empty query, no matches");
            return [];
        }
        Trace.WriteLine($"RecordingTags.Search enter: a query of {query.Length} characters");
        var searched = 0;
        var matches = new List<RecordingMatch>();
        foreach (var recording in recordings.OrderByDescending(r => r.StartedAt))
        {
            if (new[] { recording.Reason, recording.Summary, recording.MadeBy }.Any(text => text?.Contains(query.Trim(), StringComparison.OrdinalIgnoreCase) == true))
                matches.Add(new RecordingMatch(recording, recording.Reason, [(TimeSpan.Zero, TimeSpan.Zero, 1, 1)]));
            searched++;
            var frames = Read(recording).OrderBy(f => f.Ms).ToList();
            var byLabel = new Dictionary<string, List<(TimeSpan Time, int Frame, int Count)>>(StringComparer.OrdinalIgnoreCase);
            foreach (var frame in frames)
            {
                foreach (var group in ScreenTags.Filter(frame.Tags, query).GroupBy(t => t.Kind == "ui" ? "control" : t.Label))
                {
                    if (!byLabel.TryGetValue(group.Key, out var seen)) byLabel[group.Key] = seen = [];
                    seen.Add((TimeSpan.FromMilliseconds(frame.Ms), frame.Frame, group.Count()));
                }
            }
            foreach (var (label, seen) in byLabel)
            {
                var spans = new List<(TimeSpan From, TimeSpan To, int Frame, int Most)>();
                foreach (var sighting in seen)
                {
                    if (spans.Count > 0 && sighting.Time - spans[^1].To <= SpanGap)
                    {
                        var last = spans[^1];
                        spans[^1] = (last.From, sighting.Time, sighting.Count > last.Most ? sighting.Frame : last.Frame, Math.Max(last.Most, sighting.Count));
                    }
                    else spans.Add((sighting.Time, sighting.Time, sighting.Frame, sighting.Count));
                }
                matches.Add(new RecordingMatch(recording, label, spans));
            }
        }
        Trace.WriteLine($"RecordingTags.Search exit: {matches.Count} match(es) in {searched} recording(s)");
        return matches;
    }

    public static string Clock(TimeSpan time) => time.TotalHours >= 1 ? time.ToString(@"h\:mm\:ss", CultureInfo.InvariantCulture) : time.ToString(@"m\:ss", CultureInfo.InvariantCulture);

    /// <summary>The matches as the bot reads them.</summary>
    public static string Describe(IReadOnlyList<RecordingMatch> matches, string query, int untagged)
    {
        ArgumentNullException.ThrowIfNull(matches);
        var text = new StringBuilder();
        if (matches.Count == 0) text.Append(CultureInfo.InvariantCulture, $"No recordings show \"{query}\".");
        foreach (var match in matches)
        {
            var r = match.Recording;
            var when = string.Join(", ", match.Spans.Select(s => s.From == s.To ? Clock(s.From) : $"{Clock(s.From)}-{Clock(s.To)}"));
            text.AppendLine(CultureInfo.InvariantCulture,
                $"{match.Label}: {(r.Kind == RecordingKind.Screenshot ? "screenshot" : "recording")} {r.Id} by {r.MadeBy} ({r.StartedAt.ToLocalTime():d MMM HH:mm}, \"{r.Reason}\") at {when}; up to {match.Spans.Max(s => s.Most)} at once");
        }
        if (untagged > 0) text.Append(CultureInfo.InvariantCulture, $"\n{untagged} recording{(untagged == 1 ? " is" : "s are")} still being tagged and not searched yet.");
        return text.ToString().TrimEnd();
    }
}

/// <summary>What find_in_recordings needs from the app: the recordings it may search, and the tagger to finish any not yet tagged.</summary>
public interface IRecordingSearch
{
    IReadOnlyList<Recording> Recordings(bool everyone);

    IScreenTagger? Tagger { get; }
}

/// <summary>find_in_recordings: a bot asks which recordings and screenshots show something, and when.</summary>
public sealed class FindInRecordingsTool(Func<IRecordingSearch?> search) : ITool
{
    public string Id => "find_in_recordings";

    public string Description =>
        "Search the screenshots and recordings on the timeline for what they show, using the local vision model's tags: " +
        "everyday objects by name (\"person\", \"dog\", \"car\", \"laptop\", \"cell phone\": the 80 COCO classes) or \"control\". " +
        "Returns each recording with the times it appears. Recordings are tagged as they are saved; one not yet tagged is tagged now.";

    public JsonSchema Parameters { get; } = new()
    {
        Properties = new Dictionary<string, JsonSchemaProperty>
        {
            ["query"] = new() { Type = "string", Description = "What to look for: one or more names, comma or space separated." },
            ["everyone"] = new() { Type = "boolean", Description = "Search every bot's recordings, not just yours (default false)." }
        },
        Required = ["query"]
    };

    public async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        ArgumentNullException.ThrowIfNull(args);
        ArgumentNullException.ThrowIfNull(context);
        Trace.WriteLine($"FindInRecordingsTool.ExecuteAsync enter: {args}");
        if (search() is not { } source)
        {
            Trace.WriteLine("FindInRecordingsTool.ExecuteAsync exit: no timeline here");
            return new ToolResult { Title = "No timeline", Output = "There is no timeline to search here." };
        }
        using var doc = JsonDocument.Parse(args);
        var query = doc.RootElement.TryGetProperty("query", out var q) ? q.GetString() ?? "" : "";
        if (string.IsNullOrWhiteSpace(query))
        {
            Trace.TraceWarning("FindInRecordingsTool.ExecuteAsync: refused: no query");
            throw new ArgumentException("Say what to look for in query.");
        }
        var everyone = doc.RootElement.TryGetProperty("everyone", out var e) && e.ValueKind == JsonValueKind.True;
        var recordings = source.Recordings(everyone);
        var untagged = 0;
        foreach (var recording in recordings.Where(r => !r.IsLive && !RecordingTags.IsComplete(r)))
        {
            if (source.Tagger is { } tagger && recording.FrameCount <= 60) await RecordingTags.TagAsync(recording, tagger, context.CancellationToken);
            else untagged++;
        }
        Trace.WriteLine($"FindInRecordingsTool.ExecuteAsync: {recordings.Count} recording(s) (everyone={everyone}), {untagged} left untagged, tagger {(source.Tagger == null ? "none" : "on")}");
        var matches = RecordingTags.Search(recordings, query);
        Trace.WriteLine($"FindInRecordingsTool.ExecuteAsync exit: {matches.Count} matches in {recordings.Count} recordings");
        return new ToolResult { Title = $"{matches.Count} matches", Output = RecordingTags.Describe(matches, query, untagged) };
    }
}
