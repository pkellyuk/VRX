using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace PersonaBot;

/// <summary>
/// When a machine stops by itself, everything that might say why, gathered into crashes\&lt;time&gt;\ in its folder
/// before a restart can overwrite it: the guest's console (guest-boot.log) and QEMU's log as they were, and
/// report.txt with the reason, QEMU's exit code, the host's last samples (HostWatch), the diary's last lines (QEMU's
/// events among them), the programs started on the PC in the minutes before, what else was running, and Windows'
/// own warnings and errors from around then. It also leaves a note so the next start collects the guest's own
/// journal of the boot that died (<see cref="PendingFileName"/>). The last <see cref="Kept"/> reports are kept.
/// </summary>
public static class CrashReport
{
    public const string FolderName = "crashes";
    public const string ReportName = "report.txt";
    public const string PendingFileName = "post-mortem-pending.txt";
    public const string PostMortemName = "previous-boot.txt";
    public const int Kept = 10;

    /// <summary>Writes the report for a stop at <paramref name="stoppedAt"/> and returns its folder.</summary>
    public static string Write(string machineFolder, string machineId, string why, int? exitCode, IReadOnlyList<string> hostSamples, DateTimeOffset stoppedAt)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(machineFolder);
        ArgumentException.ThrowIfNullOrWhiteSpace(machineId);
        ArgumentException.ThrowIfNullOrWhiteSpace(why);
        ArgumentNullException.ThrowIfNull(hostSamples);
        Trace.WriteLine($"CrashReport.Write enter: {machineId}: {why}");
        var folder = Path.Combine(machineFolder, FolderName, stoppedAt.ToString("yyyy-MM-dd_HH-mm-ss", CultureInfo.InvariantCulture));
        Directory.CreateDirectory(folder);
        foreach (var name in new[] { "guest-boot.log", "qemu.log", "guest-boot.previous.log" }) CopyShared(Path.Combine(machineFolder, name), Path.Combine(folder, name));
        var report = new StringBuilder();
        report.AppendLine($"PersonaBot crash report for {machineId}");
        report.AppendLine($"Stopped:   {stoppedAt:yyyy-MM-dd HH:mm:ss zzz}");
        report.AppendLine($"Why:       {why}");
        report.AppendLine($"QEMU exit: {exitCode?.ToString(CultureInfo.InvariantCulture) ?? "unknown"}");
        report.AppendLine($"App:       {Brand.WindowTitle}, pid {Environment.ProcessId}");
        report.AppendLine();
        Section(report, "The guest's last words (end of guest-boot.log)", LastLines(Path.Combine(folder, "guest-boot.log"), 60));
        Section(report, "QEMU's last words (end of qemu.log)", LastLines(Path.Combine(folder, "qemu.log"), 30));
        Section(report, "The PC's view of the machine, every 30 seconds before the stop (HostWatch)", hostSamples);
        Section(report, "The machine diary's last lines (QEMU events, host samples, starts)", MachineDiary.Tail(machineFolder, 60));
        Section(report, "Programs started on the PC in the 5 minutes before", StartedSince(stoppedAt.AddMinutes(-5)));
        Section(report, "QEMUs and PersonaBots running now", Running());
        Section(report, "Windows System log: warnings and errors in the 15 minutes before", WindowsEvents("System", 40));
        Section(report, "Windows Application log: errors in the 15 minutes before", WindowsEvents("Application", 20, errorsOnly: true));
        Section(report, "The GPU now", [HostWatch.Gpu() ?? "no nvidia-smi"]);
        var path = Path.Combine(folder, ReportName);
        File.WriteAllText(path, report.ToString());
        // The next start reads the guest's own record of the boot that died (its journal), into this folder.
        File.WriteAllText(Path.Combine(machineFolder, FolderName, PendingFileName), folder);
        Prune(Path.Combine(machineFolder, FolderName));
        MachineDiary.Write(machineFolder, machineId, "crash", $"{why}; report in {folder}");
        Trace.WriteLine($"CrashReport.Write exit: {path} ({report.Length} characters)");
        return folder;
    }

    /// <summary>The crash folder still waiting for the guest's journal of the boot that died, or null.</summary>
    public static string? PendingPostMortem(string machineFolder)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(machineFolder);
        var note = Path.Combine(machineFolder, FolderName, PendingFileName);
        if (!File.Exists(note)) return null;
        var folder = File.ReadAllText(note).Trim();
        return Directory.Exists(folder) ? folder : null;
    }

    /// <summary>Saves the guest's journal of the boot that died beside its report, and clears the note.</summary>
    public static void CompletePostMortem(string machineFolder, string crashFolder, string journal)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(machineFolder);
        ArgumentException.ThrowIfNullOrWhiteSpace(crashFolder);
        ArgumentNullException.ThrowIfNull(journal);
        Trace.WriteLine($"CrashReport.CompletePostMortem: {journal.Length} characters into {crashFolder}");
        File.WriteAllText(Path.Combine(crashFolder, PostMortemName), journal);
        File.Delete(Path.Combine(machineFolder, FolderName, PendingFileName));
    }

    private static void Section(StringBuilder report, string title, IEnumerable<string> lines)
    {
        report.AppendLine("== " + title + " ==");
        var any = false;
        foreach (var line in lines)
        {
            report.AppendLine(line.TrimEnd());
            any = true;
        }
        if (!any) report.AppendLine("(nothing)");
        report.AppendLine();
    }

    private static void CopyShared(string from, string to)
    {
        try
        {
            if (!File.Exists(from)) return;
            using var source = new FileStream(from, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var target = File.Create(to);
            source.CopyTo(target);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Trace.WriteLine($"CrashReport.CopyShared: couldn't copy {from}: {ex.Message}");
        }
    }

    private static IEnumerable<string> LastLines(string path, int count)
    {
        try
        {
            if (!File.Exists(path)) return [];
            var lines = File.ReadAllLines(path);
            return lines.Length <= count ? lines : lines[^count..];
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return ["couldn't read: " + ex.Message];
        }
    }

    /// <summary>Programs that started on the PC since <paramref name="since"/>, oldest first (ones Windows won't say are left out).</summary>
    public static IReadOnlyList<string> StartedSince(DateTimeOffset since)
    {
        var started = new List<(DateTime At, string Line)>();
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                try
                {
                    var at = process.StartTime;
                    if (at < since.LocalDateTime) continue;
                    started.Add((at, $"{at:HH:mm:ss} {process.ProcessName} (pid {process.Id})"));
                }
                catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException or NotSupportedException) { }
            }
        }
        return started.OrderBy(s => s.At).Select(s => s.Line).ToList();
    }

    private static IEnumerable<string> Running()
    {
        foreach (var name in new[] { "qemu-system-x86_64", "qemu-system-aarch64", "PersonaBot" })
        {
            foreach (var process in Process.GetProcessesByName(name))
            {
                using (process)
                {
                    string started;
                    try { started = process.StartTime.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture); }
                    catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException) { started = "unknown"; }
                    yield return $"{name} pid {process.Id}, started {started}";
                }
            }
        }
    }

    /// <summary>Recent warnings and errors (or errors only) from a Windows event log, newest first, through wevtutil.</summary>
    private static IReadOnlyList<string> WindowsEvents(string log, int count, bool errorsOnly = false)
    {
        if (!OperatingSystem.IsWindows()) return ["not Windows"];
        var levels = errorsOnly ? "Level=1 or Level=2" : "Level=1 or Level=2 or Level=3";
        try
        {
            var info = new ProcessStartInfo("wevtutil") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (var arg in new[] { "qe", log, $"/q:*[System[({levels}) and TimeCreated[timediff(@SystemTime) <= 900000]]]", "/f:text", "/rd:true", $"/c:{count}" }) info.ArgumentList.Add(arg);
            using var process = Process.Start(info);
            if (process == null) return ["wevtutil didn't start"];
            var output = process.StandardOutput.ReadToEndAsync();
            if (!process.WaitForExit(15000))
            {
                try { process.Kill(); } catch (InvalidOperationException) { }
                return ["wevtutil didn't answer within 15 s"];
            }
            // wevtutil's text form is a block of "Name: value" lines per event: keep the useful ones, one event per line.
            var events = new List<string>();
            var current = new StringBuilder();
            var inDescription = false;
            foreach (var raw in output.Result.Split('\n'))
            {
                var line = raw.Trim();
                if (line.StartsWith("Event[", StringComparison.Ordinal))
                {
                    if (current.Length > 0) events.Add(current.ToString());
                    current.Clear();
                    inDescription = false;
                    continue;
                }
                // The description's text runs on the lines after its label, to the next event.
                if (inDescription)
                {
                    if (line.Length > 0 && current.Length < 600) current.Append(' ').Append(line);
                    continue;
                }
                foreach (var field in new[] { "Date:", "Source:", "Event ID:", "Level:", "Description:" })
                {
                    if (!line.StartsWith(field, StringComparison.Ordinal)) continue;
                    current.Append(current.Length > 0 ? " | " : "").Append(line);
                    inDescription = field == "Description:";
                }
            }
            if (current.Length > 0) events.Add(current.ToString());
            return events;
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or IOException or InvalidOperationException)
        {
            return ["wevtutil failed: " + ex.Message];
        }
    }

    /// <summary>Keeps the newest <see cref="Kept"/> crash folders.</summary>
    private static void Prune(string crashes)
    {
        var folders = Directory.GetDirectories(crashes).OrderByDescending(f => f, StringComparer.Ordinal).Skip(Kept).ToList();
        foreach (var old in folders)
        {
            try
            {
                Directory.Delete(old, recursive: true);
                Trace.WriteLine($"CrashReport.Prune: removed {old}");
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                Trace.WriteLine($"CrashReport.Prune: couldn't remove {old}: {ex.Message}");
            }
        }
    }
}
