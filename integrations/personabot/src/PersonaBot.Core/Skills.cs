using System.Diagnostics;
using System.Text;

namespace PersonaBot;

/// <summary>One of a bot's skills: a folder in its home's skills folder, described by the header of its SKILL.md.</summary>
public sealed record Skill(string Folder, string Name, string Description);

/// <summary>
/// Bots' skills: know-how a bot keeps for itself in its Linux home, one folder per skill under ~/skills, each with a
/// SKILL.md that starts with a short header (name and "use this when …") and then says what it does, what it needs and
/// how to install it, how to run it, known problems and when it last worked, plus any scripts. The app shows each bot
/// the list at the start of a chat, and copies the folder to the PC after each task so skills survive a machine
/// being wiped or replaced. Everything here runs as the bot's own Linux user (through its AccountShell).
/// </summary>
public static class SkillLibrary
{
    /// <summary>The folder in a bot's home.</summary>
    public const string FolderName = "skills";

    /// <summary>The most skills listed in a bot's instructions.</summary>
    public const int MaxListed = 60;

    /// <summary>Files larger than this are not copied to the PC (installed tools and downloads belong elsewhere).</summary>
    public const long MaxFileBytes = 1024 * 1024;

    /// <summary>The most a bot's skills folder may hold, in total, for the copy on the PC.</summary>
    public const long MaxTotalBytes = 20L * 1024 * 1024;

    private const int ChunkCharacters = 40000;
    private const string SkillLine = "PERSONABOT_SKILL";
    private const string FileLine = "PERSONABOT_SKILLFILE";
    private const string Manifest = ".personabot-skills";

    /// <summary>Folders inside a skill that hold installed or generated things, never copied.</summary>
    private static readonly string[] Skipped = ["node_modules", ".venv", "venv", "__pycache__", ".git", ".cache"];

    /// <summary>Where a bot's skills are kept on this PC.</summary>
    public static string HostFolder(string personaId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(personaId);
        if (personaId.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || personaId.Contains("..", StringComparison.Ordinal))
            throw new ArgumentException($"'{personaId}' can't name a folder.", nameof(personaId));
        return Path.Combine(Settings.Root, "Skills", personaId);
    }

    /// <summary>Whether the PC holds a copy of a bot's skills.</summary>
    public static bool HasBackup(string hostFolder)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(hostFolder);
        return File.Exists(Path.Combine(hostFolder, Manifest));
    }

    /// <summary>The bot's own words about skills, for its instructions, with the skills it has now.</summary>
    public static string PromptText(string home, IReadOnlyList<Skill> skills)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(home);
        ArgumentNullException.ThrowIfNull(skills);
        var folder = home.TrimEnd('/') + "/" + FolderName;
        var builder = new StringBuilder();
        builder.Append($"Your skills: know-how you keep for yourself in {folder}, one folder per skill, each with a SKILL.md that starts with this header:\n");
        builder.Append("---\nname: short-name\ndescription: Use this when … (one line)\n---\n");
        builder.Append("then what it does, what it needs and how to install that, how to run it, known problems and fixes, and when it last worked; scripts go beside it. ");
        builder.Append("Before a task, check whether one of your skills fits, read its SKILL.md and follow it rather than working it out again. ");
        builder.Append("When you set up something you'll want again (software, a script, a way of doing a job), save it as a skill; when a skill breaks or you improve it, update its SKILL.md. ");
        builder.Append("Never put a password, token or key in a skill: keep them in your browser profile or in a file under ~/.config that only you can read (chmod 600), and say where in SKILL.md. ");
        builder.Append("PersonaBot keeps a copy of your skills folder on the person's PC (files up to 1 MB; installed software and downloads belong elsewhere).\n");
        if (skills.Count == 0) builder.Append("You have no skills yet.\n");
        else
        {
            builder.Append("Your skills now:\n");
            foreach (var skill in skills.Take(MaxListed))
            {
                builder.Append($"- {skill.Name}" + (skill.Folder != skill.Name ? $" ({folder}/{skill.Folder})" : "") + (skill.Description.Length > 0 ? ": " + skill.Description : "") + "\n");
            }
            if (skills.Count > MaxListed) builder.Append($"… and {skills.Count - MaxListed} more in {folder}.\n");
        }
        return builder.ToString();
    }

    /// <summary>Lists the skills: each folder with a SKILL.md, and its header's name and description.</summary>
    public static async Task<IReadOnlyList<Skill>> ListAsync(IGuestShell shell, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(shell);
        Trace.WriteLine("SkillLibrary.ListAsync enter");
        var output = await shell.ExecuteAsync(ListScript, token);
        var skills = Parse(output);
        Trace.WriteLine($"SkillLibrary.ListAsync exit: {skills.Count} skill(s)");
        return skills;
    }

    /// <summary>Prints one tab-separated line per skill: its folder, and its header's name and description.</summary>
    public static string ListScript => $$"""
        for personabot_skill in "$HOME/{{FolderName}}"/*/SKILL.md; do
            [ -f "$personabot_skill" ] || continue
            personabot_folder=$(basename "$(dirname "$personabot_skill")")
            printf '{{SkillLine}}\t%s\t' "$personabot_folder"
            awk 'NR == 1 && $0 != "---" { exit } NR > 1 && $0 == "---" { exit } /^name:/ { sub(/^name:[ \t]*/, ""); n = $0 } /^description:/ { sub(/^description:[ \t]*/, ""); d = $0 } END { gsub(/\t/, " ", n); gsub(/\t/, " ", d); printf "%s\t%s\n", n, d }' "$personabot_skill"
        done
        """;

    /// <summary>The skills in <see cref="ListScript"/>'s output, sorted by name; anything else is ignored.</summary>
    public static IReadOnlyList<Skill> Parse(string output)
    {
        ArgumentNullException.ThrowIfNull(output);
        var skills = new List<Skill>();
        foreach (var raw in output.Replace("\r\n", "\n").Split('\n'))
        {
            var parts = raw.Split('\t');
            if (parts.Length < 4 || parts[0] != SkillLine || parts[1].Trim().Length == 0) continue;
            var folder = parts[1].Trim();
            var name = Clean(parts[2], 60);
            var description = Clean(parts[3], 200);
            skills.Add(new Skill(folder, name.Length > 0 ? name : folder, description));
        }
        return skills.OrderBy(s => s.Name, StringComparer.OrdinalIgnoreCase).ToList();
    }

    private static string Clean(string text, int length)
    {
        var clean = new string(text.Where(c => !char.IsControl(c)).ToArray()).Trim().Trim('"', '\'');
        return clean.Length <= length ? clean : clean[..(length - 1)] + "…";
    }

    /// <summary>The files copied to the PC: path within the skills folder, size, and mode, one per line.</summary>
    public static string FilesScript => $$"""
        if [ -d "$HOME/{{FolderName}}" ]; then
            cd "$HOME/{{FolderName}}" && find . -type f -size -{{MaxFileBytes / 1024 + 1}}k {{string.Join(" ", Skipped.Select(s => $"! -path '*/{s}/*'"))}} -printf '{{FileLine}}\t%P\t%s\t%m\n' | sort
        fi
        """;

    /// <summary>A fingerprint of the skills folder (paths, sizes and times), so it is copied to the PC only when it changed.</summary>
    public static async Task<string> FingerprintAsync(IGuestShell shell, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(shell);
        var output = await shell.ExecuteAsync($"if [ -d \"$HOME/{FolderName}\" ]; then cd \"$HOME/{FolderName}\" && find . -type f -printf '%P %s %T@\\n' | sort | md5sum | cut -c1-32; else echo none; fi", token);
        var fingerprint = output.Replace("\r\n", "\n").Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Length > 0 && !l.StartsWith("Exit code:", StringComparison.Ordinal)) ?? "none";
        Trace.WriteLine($"SkillLibrary.FingerprintAsync: {fingerprint}");
        return fingerprint;
    }

    /// <summary>
    /// Copies the bot's skills folder to <paramref name="hostFolder"/>, replacing the copy there (files it no longer has
    /// go too). Files over <see cref="MaxFileBytes"/> and installed folders (node_modules, virtual environments) are left
    /// out. Returns the number of files copied.
    /// </summary>
    public static async Task<int> BackupAsync(IGuestShell shell, string hostFolder, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(shell);
        ArgumentException.ThrowIfNullOrWhiteSpace(hostFolder);
        Trace.WriteLine($"SkillLibrary.BackupAsync enter: to {hostFolder}");
        var watch = Stopwatch.StartNew();
        var files = ParseFiles(await shell.ExecuteAsync(FilesScript, token));
        var total = files.Sum(f => f.Size);
        if (total > MaxTotalBytes)
        {
            Trace.TraceWarning($"SkillLibrary.BackupAsync: {total} bytes is over the {MaxTotalBytes} limit; not copied");
            throw new IOException($"The skills folder holds {total / (1024 * 1024)} MB of small files, more than the {MaxTotalBytes / (1024 * 1024)} MB PersonaBot copies.");
        }
        var staging = hostFolder + ".new";
        if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
        Directory.CreateDirectory(staging);
        var manifest = new StringBuilder();
        foreach (var file in files)
        {
            var target = HostPath(staging, file.Path);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            var text = new StringBuilder();
            for (var start = 1; ; start += ChunkCharacters)
            {
                var chunk = await shell.ExecuteAsync($"base64 -w0 {GuestShellText.Quote(FolderName + "/" + file.Path)} | cut -c{start}-{start + ChunkCharacters - 1}", token);
                var data = Body(chunk).Trim();
                text.Append(data);
                if (data.Length < ChunkCharacters) break;
            }
            await File.WriteAllBytesAsync(target, Convert.FromBase64String(text.ToString()), token);
            manifest.Append(file.Path).Append('\t').Append(file.Mode).Append('\n');
        }
        await File.WriteAllTextAsync(Path.Combine(staging, Manifest), manifest.ToString(), token);
        // The new copy replaces the old only once it is whole.
        var old = hostFolder + ".old";
        if (Directory.Exists(old)) Directory.Delete(old, recursive: true);
        if (Directory.Exists(hostFolder)) Directory.Move(hostFolder, old);
        Directory.Move(staging, hostFolder);
        if (Directory.Exists(old)) Directory.Delete(old, recursive: true);
        Trace.WriteLine($"SkillLibrary.BackupAsync exit: {files.Count} file(s), {total} bytes in {watch.Elapsed.TotalSeconds:0.0}s");
        return files.Count;
    }

    /// <summary>
    /// Puts the PC's copy back into the bot's skills folder on a machine that has none (a new or wiped one), keeping each
    /// file's mode. Files already there are left as they are. Returns the number of files written.
    /// </summary>
    public static async Task<int> RestoreAsync(IGuestShell shell, string hostFolder, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(shell);
        ArgumentException.ThrowIfNullOrWhiteSpace(hostFolder);
        Trace.WriteLine($"SkillLibrary.RestoreAsync enter: from {hostFolder}");
        if (!HasBackup(hostFolder)) { Trace.WriteLine("SkillLibrary.RestoreAsync exit: no copy on the PC"); return 0; }
        var written = 0;
        foreach (var line in (await File.ReadAllTextAsync(Path.Combine(hostFolder, Manifest), token)).Split('\n'))
        {
            var parts = line.Split('\t');
            if (parts.Length != 2 || parts[0].Length == 0) continue;
            var relative = parts[0];
            var source = HostPath(hostFolder, relative);
            if (!File.Exists(source)) continue;
            var mode = System.Text.RegularExpressions.Regex.IsMatch(parts[1], "^[0-7]{3,4}$") ? parts[1] : "644";
            var guest = GuestShellText.Quote(FolderName + "/" + relative);
            var staged = GuestShellText.Quote(FolderName + "/" + relative + ".personabot-b64");
            var data = Convert.ToBase64String(await File.ReadAllBytesAsync(source, token));
            var exists = await shell.ExecuteAsync($"test -e {guest} && echo present || echo absent", token);
            if (exists.Contains("present", StringComparison.Ordinal)) continue;
            await shell.ExecuteAsync($"mkdir -p \"$(dirname {guest})\" && : > {staged}", token);
            for (var start = 0; start < data.Length; start += ChunkCharacters)
            {
                await shell.ExecuteAsync($"printf '%s' '{data.Substring(start, Math.Min(ChunkCharacters, data.Length - start))}' >> {staged}", token);
            }
            var result = await shell.ExecuteAsync($"base64 -d {staged} > {guest} && rm -f {staged} && chmod {mode} {guest}", token);
            if (!result.TrimEnd().EndsWith("Exit code: 0", StringComparison.Ordinal))
            {
                Trace.TraceWarning($"SkillLibrary.RestoreAsync: {relative} wasn't written: {Body(result).Trim()}");
                continue;
            }
            written++;
        }
        Trace.WriteLine($"SkillLibrary.RestoreAsync exit: {written} file(s) written");
        return written;
    }

    /// <summary>The files in <see cref="FilesScript"/>'s output; paths that would leave the folder are refused.</summary>
    public static IReadOnlyList<(string Path, long Size, string Mode)> ParseFiles(string output)
    {
        ArgumentNullException.ThrowIfNull(output);
        var files = new List<(string, long, string)>();
        foreach (var raw in output.Replace("\r\n", "\n").Split('\n'))
        {
            var parts = raw.Split('\t');
            if (parts.Length != 4 || parts[0] != FileLine || !long.TryParse(parts[2], out var size)) continue;
            var path = parts[1];
            if (!SafeRelative(path)) { Trace.TraceWarning($"SkillLibrary.ParseFiles: skipped an unsafe path '{path}'"); continue; }
            files.Add((path, size, parts[3]));
        }
        return files;
    }

    /// <summary>Whether a path stays inside the skills folder: relative, no "..", no backslashes or odd characters.</summary>
    public static bool SafeRelative(string path) =>
        path.Length is > 0 and < 400 && !path.StartsWith('/') && !path.Contains('\\') && !path.Contains('\'') && !path.Contains('\n')
        && path.Split('/').All(part => part.Length > 0 && part != "." && part != "..")
        && path.IndexOfAny(Path.GetInvalidPathChars()) < 0 && !path.Contains(':');

    private static string HostPath(string root, string relative)
    {
        if (!SafeRelative(relative)) throw new IOException($"'{relative}' isn't a path inside the skills folder.");
        var full = Path.GetFullPath(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
        if (!full.StartsWith(Path.GetFullPath(root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new IOException($"'{relative}' would leave the skills folder.");
        return full;
    }

    /// <summary>A command's output without the shell's "Exit code" line.</summary>
    private static string Body(string output)
    {
        var at = output.LastIndexOf("\nExit code:", StringComparison.Ordinal);
        return at >= 0 ? output[..at] : output;
    }
}
