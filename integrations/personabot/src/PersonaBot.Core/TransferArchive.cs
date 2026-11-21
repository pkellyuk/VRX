using System.Formats.Tar;

namespace PersonaBot;

/// <summary>Copies regular files and directories only; never follows links or overwrites an existing destination.</summary>
public static class TransferArchive
{
    public const long MaximumBytes = 1024L * 1024 * 1024;
    public const int MaximumEntries = 10000;

    public static void Pack(IReadOnlyList<string> sources, string archive, CancellationToken token)
    {
        if (sources.Count == 0) throw new ArgumentException("Choose a file or folder.");
        using var output = new FileStream(archive, FileMode.CreateNew);
        using var writer = new TarWriter(output);
        long bytes = 0; var entries = 0;
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Add(string path, string name)
        {
            token.ThrowIfCancellationRequested();
            if (++entries > MaximumEntries) throw new IOException("A transfer can contain at most 10,000 entries.");
            var attributes = File.GetAttributes(path);
            if (attributes.HasFlag(FileAttributes.ReparsePoint)) throw new IOException("Links and junctions cannot be transferred.");
            if (attributes.HasFlag(FileAttributes.Directory))
            {
                writer.WriteEntry(new PaxTarEntry(TarEntryType.Directory, name));
                foreach (var child in Directory.EnumerateFileSystemEntries(path)) Add(child, name + "/" + Path.GetFileName(child));
            }
            else
            {
                using var input = File.OpenRead(path);
                bytes = checked(bytes + input.Length);
                if (bytes > MaximumBytes) throw new IOException("A transfer can contain at most 1 GiB.");
                writer.WriteEntry(new PaxTarEntry(TarEntryType.RegularFile, name) { DataStream = input });
            }
            if (output.Length > MaximumBytes) throw new IOException("The transfer archive exceeds 1 GiB.");
        }
        foreach (var source in sources)
        {
            var path = Path.TrimEndingDirectorySeparator(Path.GetFullPath(source));
            var name = Path.GetFileName(path);
            if (string.IsNullOrEmpty(name) || !names.Add(name)) throw new IOException("Choose items with different names; drive roots cannot be copied.");
            Add(path, name);
        }
    }

    internal static string SafeRelativeName(string name)
    {
        while (name.StartsWith("./", StringComparison.Ordinal)) name = name[2..];
        name = name.TrimEnd('/');
        var parts = name.Split('/');
        if (parts.Any(p => string.IsNullOrEmpty(p) || p is "." or ".." || p.EndsWith('.') || p.EndsWith(' ')
            || p.Any(c => c < 32 || "<>:\"\\|?*".Contains(c))
            || System.Text.RegularExpressions.Regex.IsMatch(p.Split('.')[0], @"^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])$", System.Text.RegularExpressions.RegexOptions.IgnoreCase)))
            throw new IOException("A copied item has a path that Windows cannot represent safely.");
        return Path.Combine(parts);
    }

    public static IReadOnlyList<string> Unpack(string archive, string destination, CancellationToken token)
    {
        if (File.Exists(destination) || Directory.Exists(destination)) throw new IOException("The transfer destination already exists.");
        if (new FileInfo(archive).Length > MaximumBytes) throw new IOException("The transfer archive exceeds 1 GiB.");
        // Validate the entire manifest first. Links, special files and case collisions never reach the filesystem.
        var seen = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
        var roots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var input = File.OpenRead(archive))
        using (var reader = new TarReader(input))
        {
            long size = 0; var count = 0;
            while (reader.GetNextEntry() is { } entry)
            {
                token.ThrowIfCancellationRequested();
                if (++count > MaximumEntries || (size = checked(size + entry.Length)) > MaximumBytes)
                    throw new IOException("The transfer exceeds 1 GiB or 10,000 entries.");
                if (entry.EntryType is not (TarEntryType.RegularFile or TarEntryType.V7RegularFile or TarEntryType.Directory))
                    throw new IOException("Links and special files cannot be transferred.");
                var name = SafeRelativeName(entry.Name);
                if (!seen.TryAdd(name, entry.EntryType == TarEntryType.Directory)) throw new IOException("The copied items have duplicate or case-conflicting paths.");
                roots.Add(name.Split(Path.DirectorySeparatorChar)[0]);
            }
        }
        if (seen.Count == 0) throw new IOException("The transfer contains no files or folders.");
        foreach (var name in seen.Keys)
            for (var parent = Path.GetDirectoryName(name); !string.IsNullOrEmpty(parent); parent = Path.GetDirectoryName(parent))
                if (seen.TryGetValue(parent, out var directory) && !directory) throw new IOException("A file conflicts with a folder in this transfer.");
        Directory.CreateDirectory(destination);
        using (var input = File.OpenRead(archive))
        using (var reader = new TarReader(input))
            while (reader.GetNextEntry() is { } entry)
            {
                token.ThrowIfCancellationRequested();
                var path = Path.Combine(destination, SafeRelativeName(entry.Name));
                if (entry.EntryType == TarEntryType.Directory) Directory.CreateDirectory(path);
                else
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                    using var file = new FileStream(path, FileMode.CreateNew);
                    entry.DataStream?.CopyTo(file);
                }
            }
        return roots.Select(n => Path.Combine(destination, n)).ToArray();
    }
}
