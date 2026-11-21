using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace PersonaBot;

/// <summary>
/// A machine's own always-on log, machine-diary.log in its folder: QEMU's events, the host's samples of QEMU and the
/// GPU, starts, stops and crashes, one timestamped line each. Unlike syslog (off unless chosen) it is always written,
/// so after a crash there is a record to read. It rolls over to machine-diary.1.log at <see cref="MaxBytes"/>. Each
/// line also goes to syslog when machine logs are on.
/// </summary>
public static class MachineDiary
{
    public const string FileName = "machine-diary.log";
    public const string OldFileName = "machine-diary.1.log";
    public const long MaxBytes = 8 * 1024 * 1024;

    private static readonly ConcurrentDictionary<string, object> Locks = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Appends one line from <paramref name="source"/> (qemu-event, host, crash…) to the diary in <paramref name="folder"/>.</summary>
    public static void Write(string folder, string machineId, string source, string line)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(folder);
        ArgumentException.ThrowIfNullOrWhiteSpace(machineId);
        ArgumentException.ThrowIfNullOrWhiteSpace(source);
        ArgumentNullException.ThrowIfNull(line);
        PersonaBotLogging.Machine(machineId, source, line);
        var path = Path.Combine(folder, FileName);
        var stamped = $"{DateTimeOffset.Now.ToString("yyyy-MM-ddTHH:mm:ss.fffzzz", CultureInfo.InvariantCulture)} {source}: {line.ReplaceLineEndings(" | ")}\n";
        lock (Locks.GetOrAdd(path, _ => new object()))
        {
            try
            {
                var info = new FileInfo(path);
                if (info.Exists && info.Length > MaxBytes)
                {
                    File.Move(path, Path.Combine(folder, OldFileName), overwrite: true);
                    Trace.WriteLine($"MachineDiary.Write: {machineId}'s diary passed {MaxBytes / 1024 / 1024} MB; rolled over");
                }
                using var stream = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
                var bytes = Encoding.UTF8.GetBytes(stamped);
                stream.Write(bytes, 0, bytes.Length);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                // The diary must never take the machine down with it.
                Trace.WriteLine($"MachineDiary.Write: couldn't write {path}: {ex.Message}");
            }
        }
    }

    /// <summary>The diary's last <paramref name="count"/> lines (for a crash report), or none.</summary>
    public static IReadOnlyList<string> Tail(string folder, int count)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(folder);
        if (count <= 0) return [];
        var path = Path.Combine(folder, FileName);
        try
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            // The last 512 KB is plenty for a few hundred lines.
            if (stream.Length > 512 * 1024) stream.Position = stream.Length - 512 * 1024;
            using var reader = new StreamReader(stream, Encoding.UTF8);
            var lines = reader.ReadToEnd().Split('\n', StringSplitOptions.RemoveEmptyEntries);
            return lines.Length <= count ? lines : lines[^count..];
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Trace.WriteLine($"MachineDiary.Tail: couldn't read {path}: {ex.Message}");
            return [];
        }
    }
}
