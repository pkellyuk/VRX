using System.Diagnostics;
using PersonaBot;

namespace VRX.VrexHost;

internal static class VrexVmStorage
{
    private static readonly string[] DiskFiles =
        [CloudInstaller.DiskName, CloudInstaller.ReadyName, CloudInstaller.ProvisionedName,
            "desktop.qcow2.partial", "ubuntu-24.04-minimal-cloudimg-amd64.img", "seed.iso", "seed.revision"];
    private static readonly string[] Logs =
        ["image-prepare.log", "guest-boot.log", "guest-gpu.log", "guest-sound.log", "qemu.log"];
    private static readonly StringComparison PathComparison = StringComparison.OrdinalIgnoreCase;

    internal static bool HasDisks(string path) => DiskFiles.Any(name => File.Exists(Path.Combine(path, name)));

    internal static string CheckPath(string path, string dataRoot)
    {
        path = path.Trim();
        if (!Path.IsPathFullyQualified(path) || path.StartsWith(@"\\", StringComparison.Ordinal))
            throw new ArgumentException("Enter a full path on a local drive, such as D:\\Vrex Ubuntu.");
        string full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(dataRoot));
        if (full.Equals(Path.GetPathRoot(full), PathComparison) ||
            (Within(full, root) && !full.Equals(Path.Combine(root, "Ubuntu"), PathComparison)) ||
            Within(root, full))
            throw new ArgumentException("Choose a dedicated folder outside VRX’s data folder, or use its default Ubuntu folder.");
        for (DirectoryInfo? dir = new(full); dir != null; dir = dir.Parent)
            if (dir.Exists && dir.Attributes.HasFlag(FileAttributes.ReparsePoint))
                throw new ArgumentException("Choose a folder that does not use a shortcut, junction or symbolic link.");
        return full;
    }

    private static bool Within(string path, string folder) =>
        path.Equals(folder, PathComparison) ||
        path.StartsWith(Path.EndsInDirectorySeparator(folder) ? folder : folder + Path.DirectorySeparatorChar, PathComparison);

    internal static async Task<string> ChangeAsync(string from, string to, bool move, Action savePath,
        Action<string> progress, CancellationToken token)
    {
        if (MachineFiles.Problem(from, to) is { } problem) throw new ArgumentException(problem);
        using var lease = MachineLease.Acquire(from);
        bool installed = HasDisks(from);
        if (!move && installed) throw new InvalidOperationException("Use Move VM to keep the existing Ubuntu disk and files.");
        if (move && !installed) throw new InvalidOperationException("There is no Ubuntu disk to move. Use Set folder instead.");
        token.ThrowIfCancellationRequested();
        if (!move)
        {
            Directory.CreateDirectory(to);
            savePath();
            return "Storage folder saved. Ubuntu will install here on first start.";
        }

        var files = DiskFiles.Concat(Logs).Select(name => new FileInfo(Path.Combine(from, name)))
            .Where(file => file.Exists).ToArray();
        if (files.Any(file => file.Attributes.HasFlag(FileAttributes.ReparsePoint)))
            throw new IOException("The VM contains linked files and cannot be moved safely.");
        long total = files.Sum(file => file.Length);
        if (MachineFiles.FreeBytes(to) is long free && free < total)
            throw new IOException($"The destination has {MachineFiles.Gb(free)} free. The VM needs {MachineFiles.Gb(total)}.");

        string parent = Path.GetDirectoryName(to)!;
        Directory.CreateDirectory(parent);
        string stage = Path.Combine(parent, $".vrex-move-{Guid.NewGuid():N}");
        Directory.CreateDirectory(stage);
        string copy = stage;
        try
        {
            progress("Copying Ubuntu disks and data…");
            long written = 0;
            byte[] buffer = new byte[1024 * 1024];
            var timer = Stopwatch.StartNew();
            foreach (var file in files)
            {
                await using var input = new FileStream(file.FullName, FileMode.Open, FileAccess.Read, FileShare.Read, 1, useAsync: true);
                await using var output = new FileStream(Path.Combine(stage, file.Name), FileMode.CreateNew, FileAccess.Write, FileShare.None, 1, useAsync: true);
                int read;
                while ((read = await input.ReadAsync(buffer, token)) != 0)
                {
                    await output.WriteAsync(buffer.AsMemory(0, read), token);
                    written += read;
                    if (timer.ElapsedMilliseconds < 500) continue;
                    progress($"Copying Ubuntu disks · {MachineFiles.Gb(written)} of {MachineFiles.Gb(total)}");
                    timer.Restart();
                }
                await output.FlushAsync(token);
            }
            token.ThrowIfCancellationRequested();
            if (Directory.Exists(to)) Directory.Delete(to); // Only an empty destination can be replaced.
            Directory.Move(stage, to);
            copy = to;
            savePath();
        }
        catch
        {
            foreach (var file in files) TryDelete(Path.Combine(copy, file.Name));
            try { Directory.Delete(copy); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { Trace.WriteLine(ex.Message); }
            throw;
        }

        bool oldCopyRemains = false;
        foreach (var file in files) oldCopyRemains |= !TryDelete(file.FullName);
        return oldCopyRemains
            ? "VM moved. Some old files could not be removed from " + from
            : "VM moved. Ubuntu will use the new folder next time it starts.";
    }

    private static bool TryDelete(string path)
    {
        try { File.Delete(path); return true; }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        { Trace.WriteLine(ex.Message); return false; }
    }
}
