using System.Diagnostics;

namespace PersonaBot;

/// <summary>
/// The folder that holds the virtual machines (<see cref="Settings.VmDirectory"/>): checking a new one,
/// its free space, and moving every machine's files there when the person changes it.
/// </summary>
public static class MachineFiles
{
    /// <summary>
    /// Why <paramref name="to"/> can't become the VM folder in place of <paramref name="from"/>, or null when
    /// it can. It must be a full path, not inside the old folder (or the old one inside it), and either not
    /// exist yet or be empty.
    /// </summary>
    public static string? Problem(string from, string to)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(from);
        if (string.IsNullOrWhiteSpace(to)) return "Choose a folder.";
        if (!Path.IsPathFullyQualified(to)) return "Use a full path, such as D:\\PersonaBot VMs.";
        string oldFull, newFull;
        try
        {
            oldFull = Path.TrimEndingDirectorySeparator(Path.GetFullPath(from));
            newFull = Path.TrimEndingDirectorySeparator(Path.GetFullPath(to));
        }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return $"'{to}' isn't a usable folder: {ex.Message}";
        }
        if (string.Equals(oldFull, newFull, StringComparison.OrdinalIgnoreCase)) return "That is already the VM folder.";
        if (newFull.StartsWith(oldFull + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) return "Choose a folder outside the current VM folder.";
        if (oldFull.StartsWith(newFull + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) return "Choose a folder that doesn't contain the current VM folder.";
        if (File.Exists(newFull)) return "That is a file, not a folder.";
        if (Directory.Exists(newFull) && Directory.EnumerateFileSystemEntries(newFull).Any()) return "Choose an empty folder, or one that doesn't exist yet.";
        return null;
    }

    /// <summary>Free space on the drive that holds <paramref name="path"/> (or would), in bytes; null when unknown.</summary>
    public static long? FreeBytes(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        try
        {
            var root = Path.GetPathRoot(Path.GetFullPath(path));
            return string.IsNullOrEmpty(root) ? null : new DriveInfo(root).AvailableFreeSpace;
        }
        catch (Exception ex) when (ex is ArgumentException or IOException or UnauthorizedAccessException)
        {
            Trace.WriteLine($"MachineFiles.FreeBytes: {path}: {ex.Message}");
            return null;
        }
    }

    /// <summary>The bytes under a folder; 0 when it doesn't exist.</summary>
    public static long SizeOf(string folder)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(folder);
        var bytes = Directory.Exists(folder) ? Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length) : 0;
        Trace.WriteLine($"MachineFiles.SizeOf: {folder}: {(Directory.Exists(folder) ? Gb(bytes) : "no folder")}");
        return bytes;
    }

    /// <summary>
    /// Moves the whole VM folder (every machine in it) to <paramref name="to"/>, points the machines there
    /// and saves it as the VM folder. No machine may be running. On the same drive it is a rename; across
    /// drives the files are copied with progress, the machines repointed, and only then the old copy deleted,
    /// so a failure or cancel part-way leaves everything where it was.
    /// </summary>
    public static async Task MoveAsync(string to, MachineStore store, Settings settings, Action<string> progress, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(store);
        ArgumentNullException.ThrowIfNull(settings);
        ArgumentNullException.ThrowIfNull(progress);
        var from = Path.TrimEndingDirectorySeparator(Path.GetFullPath(Settings.VmDirectory));
        if (Problem(from, to) is { } problem)
        {
            Trace.TraceWarning($"MachineFiles.MoveAsync: refused {from} -> {to}: {problem}");
            throw new ArgumentException(problem);
        }
        var moveWatch = Stopwatch.StartNew();
        var target = Path.TrimEndingDirectorySeparator(Path.GetFullPath(to));
        Trace.WriteLine($"MachineFiles.MoveAsync enter: {from} -> {target}");
        if (!Directory.Exists(from))
        {
            Trace.WriteLine($"MachineFiles.MoveAsync: {from} doesn't exist yet: creating {target} only");
            Directory.CreateDirectory(target);
        }
        else if (string.Equals(Path.GetPathRoot(from), Path.GetPathRoot(target), StringComparison.OrdinalIgnoreCase))
        {
            Trace.WriteLine($"MachineFiles.MoveAsync: same drive ({Path.GetPathRoot(target)}): renaming");
            progress("Moving the machines' files…");
            Directory.CreateDirectory(Path.GetDirectoryName(target) ?? target);
            if (Directory.Exists(target)) Directory.Delete(target); // empty, checked above
            // Several attempts: antivirus and the search indexer can hold a file for a moment after QEMU exits.
            for (var attempt = 1; ; attempt++)
            {
                try
                {
                    Directory.Move(from, target);
                    break;
                }
                catch (IOException ex) when (attempt < 5)
                {
                    Trace.WriteLine($"MachineFiles.MoveAsync: move attempt {attempt} failed: {ex.Message}");
                    await Task.Delay(1000, token);
                }
            }
        }
        else
        {
            Trace.WriteLine($"MachineFiles.MoveAsync: across drives ({Path.GetPathRoot(from)} -> {Path.GetPathRoot(target)}): copying");
            await CopyAsync(from, target, progress, token);
        }
        store.Relocate(from, target);
        Trace.WriteLine($"MachineFiles.MoveAsync: machines repointed; saving the VM folder as {(string.Equals(target, Path.TrimEndingDirectorySeparator(Path.GetFullPath(Settings.DefaultVmDirectory)), StringComparison.OrdinalIgnoreCase) ? "the default" : target)}");
        settings.VmFolder = string.Equals(target, Path.TrimEndingDirectorySeparator(Path.GetFullPath(Settings.DefaultVmDirectory)), StringComparison.OrdinalIgnoreCase) ? null : target;
        settings.Save();
        if (Directory.Exists(from))
        {
            progress("Removing the old copy…");
            try
            {
                Directory.Delete(from, recursive: true);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                // The machines already use the new folder; the old copy is only wasted space.
                Trace.WriteLine($"MachineFiles.MoveAsync: old copy left at {from}: {ex.Message}");
                progress($"The machines now use {target}, but the old copy in {from} couldn't be deleted ({ex.Message}). You can delete it yourself.");
            }
        }
        Trace.WriteLine($"MachineFiles.MoveAsync exit: now {target}, {moveWatch.ElapsedMilliseconds} ms");
    }

    /// <summary>Copies a folder to another drive with byte progress; a failure or cancel removes the partial copy.</summary>
    public static async Task CopyAsync(string from, string to, Action<string> progress, CancellationToken token)
    {
        var files = Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories).Select(f => new FileInfo(f)).ToList();
        var total = files.Sum(f => f.Length);
        if (FreeBytes(to) is { } free && free < total)
        {
            Trace.TraceWarning($"MachineFiles.CopyAsync: refused {from} -> {to}: {Gb(free)} free, {Gb(total)} needed");
            throw new IOException($"The drive has {Gb(free)} free; the machines need {Gb(total)}.");
        }
        Trace.WriteLine($"MachineFiles.CopyAsync enter: {files.Count} files, {total} bytes");
        var createdTarget = !Directory.Exists(to);
        long done = 0;
        var buffer = new byte[4 * 1024 * 1024];
        var lastReport = Stopwatch.StartNew();
        try
        {
            foreach (var file in files)
            {
                var destination = Path.Combine(to, Path.GetRelativePath(from, file.FullName));
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                await using var source = new FileStream(file.FullName, FileMode.Open, FileAccess.Read, FileShare.Read, 1, FileOptions.Asynchronous | FileOptions.SequentialScan);
                await using var target = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1, FileOptions.Asynchronous);
                int read;
                while ((read = await source.ReadAsync(buffer, token)) > 0)
                {
                    await target.WriteAsync(buffer.AsMemory(0, read), token);
                    done += read;
                    if (lastReport.ElapsedMilliseconds > 500)
                    {
                        progress($"Copying the machines' files: {Gb(done)} of {Gb(total)} ({(total == 0 ? 100 : done * 100 / total)}%)…");
                        lastReport.Restart();
                    }
                }
            }
            // Empty folders too (a machine's seed folder, say).
            foreach (var folder in Directory.EnumerateDirectories(from, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, folder)));
            Trace.WriteLine($"MachineFiles.CopyAsync exit: {files.Count} files, {Gb(done)} copied to {to}");
        }
        catch
        {
            Trace.WriteLine("MachineFiles.CopyAsync: failed or cancelled; removing the partial copy");
            try
            {
                if (createdTarget) Directory.Delete(to, recursive: true);
                else foreach (var entry in Directory.EnumerateFileSystemEntries(to)) { if (Directory.Exists(entry)) Directory.Delete(entry, true); else File.Delete(entry); }
            }
            catch (Exception cleanup) when (cleanup is IOException or UnauthorizedAccessException)
            {
                Trace.WriteLine($"MachineFiles.CopyAsync: partial copy left: {cleanup.Message}");
            }
            throw;
        }
    }

    public static string Gb(long bytes) => bytes < 1024L * 1024 * 1024 ? $"{bytes / (1024.0 * 1024):0} MB" : $"{bytes / (1024.0 * 1024 * 1024):0.0} GB";
}
