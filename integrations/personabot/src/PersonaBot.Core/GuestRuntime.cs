using Automaton.Core.Services.ComputerUse;

namespace PersonaBot;

/// <summary>
/// One screen inside the guest, with its own picture, pointer and keyboard: the machine's main display,
/// or a bot's own display (XMEETING.md §3). The computer-use tools are given a screen, not a machine.
/// </summary>
public interface IGuestScreen
{
    /// <summary>The screen's size; tools and viewers use these coordinates.</summary>
    ScreenRect ScreenBounds { get; }

    /// <summary>A PNG of the whole screen.</summary>
    Task<byte[]> CaptureAsync(CancellationToken token);

    /// <summary>A new input channel with its own pointer state (the AI and the operator each get one).</summary>
    IGuestInput CreateInput();
}

/// <summary>
/// One Linux testing computer that PersonaBot's tools target: a QEMU virtual machine on Windows, a
/// proot userland on Android. Everything the agent can reach goes through this interface, so a
/// host's tools can never be offered by accident (ANDROID.md §4). The runtime is itself the machine's
/// main display.
/// </summary>
public interface IGuestRuntime : IGuestScreen, IAsyncDisposable
{
    /// <summary>Short words for the status line and the model, e.g. "QEMU/TCG" or "proot on Android".</summary>
    string Description { get; }

    /// <summary>The current graphics mode and any fallback reason, when the host offers a choice.</summary>
    string? GraphicsStatus => null;

    /// <summary>Whether the guest's screen includes its own cursor, so the person needs no pointer of the PC's over it.</summary>
    bool DrawsOwnCursor => false;

    /// <summary>True when the runtime came back from a saved RAM checkpoint rather than a fresh start.</summary>
    bool Restored { get; }

    IGuestShell Shell { get; }

    /// <summary>Reads on-screen text for cu_find; null uses the platform default (Windows OCR).</summary>
    ITextRecognizer? TextRecognizer { get; }

    /// <summary>Raised once the framebuffer can be captured.</summary>
    event Action? DisplayReady;

    /// <summary>
    /// Raised, with a reason for the person, when the running machine stops without being asked to (its kernel
    /// crashed, it shut itself down, QEMU ended). Never raised for a stop the app asked for. Hosts that can't
    /// tell never raise it.
    /// </summary>
    event Action<string>? Stopped { add { } remove { } }

    Task StartAsync(Settings settings, CancellationToken token, SessionState? resume = null);

    /// <summary>Whether <see cref="SuspendAsync"/> can save RAM and devices, not just files.</summary>
    bool SupportsCheckpoints { get; }

    /// <summary>The CPU and memory the running guest was started with; saved with checkpoints.</summary>
    int CpuCount { get; }

    int MemoryMb { get; }

    Task SuspendAsync(string tag, CancellationToken token);

    Task ResumeAsync();

    Task DeleteCheckpointAsync(string tag);

    /// <summary>The RAM checkpoints saved in the guest's disk (empty where <see cref="SupportsCheckpoints"/> is false).</summary>
    Task<IReadOnlyList<GuestCheckpoint>> ListCheckpointsAsync(CancellationToken token);

    /// <summary>Replaces the running guest's state with a saved checkpoint (the snapshot manager's Restore).</summary>
    Task RestoreCheckpointAsync(string tag, CancellationToken token);
}

/// <summary>A saved RAM checkpoint: its internal tag, size as QEMU reports it (e.g. "1.6 GiB") and when it was taken.</summary>
public sealed record GuestCheckpoint(string Tag, string Size, DateTimeOffset? Saved);

public static class GuestCheckpoints
{
    // ID  TAG  VM SIZE  DATE  VM CLOCK [ICOUNT]; the size is a number and a unit ("1.68 GiB", "0 B").
    private static readonly System.Text.RegularExpressions.Regex Row = new(
        @"^\s*(?<id>\S+)\s+(?<tag>personabot-[a-f0-9]{32})\s+(?<size>[\d.]+\s*[KMGT]?i?B)\s+(?<date>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})",
        System.Text.RegularExpressions.RegexOptions.Multiline);

    /// <summary>
    /// Reads QEMU's "info snapshots" table. Only PersonaBot's own tags (personabot-…) are listed; QEMU prints
    /// the date in the host's local time.
    /// </summary>
    public static IReadOnlyList<GuestCheckpoint> ParseQemuInfoSnapshots(string output)
    {
        ArgumentNullException.ThrowIfNull(output);
        System.Diagnostics.Trace.WriteLine($"GuestCheckpoints.ParseQemuInfoSnapshots enter: {output.Length} chars");
        var checkpoints = Row.Matches(output)
            .Select(m => new GuestCheckpoint(m.Groups["tag"].Value, m.Groups["size"].Value.Trim(),
                DateTime.TryParse(m.Groups["date"].Value, System.Globalization.CultureInfo.InvariantCulture, System.Globalization.DateTimeStyles.AssumeLocal, out var saved)
                    ? new DateTimeOffset(saved) : null))
            .ToList();
        System.Diagnostics.Trace.WriteLine($"GuestCheckpoints.ParseQemuInfoSnapshots exit: {checkpoints.Count} PersonaBot checkpoints ({checkpoints.Count(c => c.Saved == null)} without a readable date)");
        return checkpoints;
    }
}

/// <summary>A root shell inside the guest. Never a host shell.</summary>
public interface IGuestShell
{
    /// <summary>Runs a script and returns its output followed by "\nExit code: n".</summary>
    Task<string> ExecuteAsync(string script, CancellationToken token = default);

    /// <summary>Waits until no command is running, so a checkpoint sees a quiet shell.</summary>
    Task SynchronizeAsync(CancellationToken token);
}

/// <summary>The guest's pointer and keyboard, at the lowest level each runtime offers.</summary>
public interface IGuestInput
{
    void Wheel(bool positive, bool horizontal, GuestKeyModifiers modifiers)
    {
        if (modifiers != GuestKeyModifiers.None) throw new NotSupportedException("Modified scrolling is unavailable on this guest.");
        Wheel(positive, horizontal);
    }
    /// <summary>Moves the pointer to an absolute, already clamped guest position.</summary>
    void MoveTo(ScreenPoint point);

    void Button(MouseButton button, bool down);

    /// <summary>One wheel notch; positive is up (or left when horizontal).</summary>
    void Wheel(bool positive, bool horizontal);

    /// <summary>A key combination such as "ctrl+l", "alt+f4" or "enter".</summary>
    void PressKeys(string keys);

    void TypeText(string text, Func<bool>? shouldStop);
}

[Flags]
public enum GuestKeyModifiers { None = 0, Control = 1, Shift = 2, Alt = 4 }

/// <summary>Shell quoting shared by the guest tools and runtimes.</summary>
public static class GuestShellText
{
    /// <summary>A single-quoted POSIX shell word.</summary>
    public static string Quote(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        return "'" + text.Replace("'", "'\"'\"'") + "'";
    }

    /// <summary>Bytes as printf %b octal escapes, safe inside single quotes.</summary>
    public static string Octal(byte[] bytes)
    {
        ArgumentNullException.ThrowIfNull(bytes);
        return string.Concat(bytes.Select(b => "\\0" + Convert.ToString(b, 8).PadLeft(3, '0')));
    }

    /// <summary>The checkpoint tags PersonaBot writes (<see cref="GuestNames.CheckpointTagPrefix"/> and 32 hex digits).</summary>
    public static bool IsCheckpointTag(string tag) =>
        GuestNames.IsCheckpointTag(tag);

    private static readonly System.Text.RegularExpressions.Regex AsAccount = new(@"exec su -s /bin/sh ([a-z][a-z0-9]*) -c ", System.Text.RegularExpressions.RegexOptions.CultureInvariant);
    private static readonly System.Text.RegularExpressions.Regex PasswordHash = new(@"\$6\$[./0-9A-Za-z=$]+", System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    /// <summary>name=value, name: value and --name value for words that name a credential, and Bearer/Basic auth values.</summary>
    private static readonly System.Text.RegularExpressions.Regex Credential = new(
        @"(?<name>(?:--?)?[A-Za-z_\-]*(?:pass(?:word|wd)?|pwd|token|secret|api[_-]?key|auth(?:orization)?|bearer|basic)[A-Za-z_\-]*)(?<sep>\s*[=:]\s*|\s+)(?<value>""[^""]*""|'[^']*'|[^\s""']+)",
        System.Text.RegularExpressions.RegexOptions.CultureInvariant | System.Text.RegularExpressions.RegexOptions.IgnoreCase);

    /// <summary>An HTTP auth value: Bearer or Basic and its token.</summary>
    private static readonly System.Text.RegularExpressions.Regex AuthValue = new(@"\b(?:Bearer|Basic)\s+[^\s'""]+", System.Text.RegularExpressions.RegexOptions.CultureInvariant | System.Text.RegularExpressions.RegexOptions.IgnoreCase);

    /// <summary>scheme://user:password@ in a URL.</summary>
    private static readonly System.Text.RegularExpressions.Regex UrlCredential = new(@"(?<scheme>[a-z][a-z0-9+.\-]*://)[^\s/@:]+:[^\s/@]+@", System.Text.RegularExpressions.RegexOptions.CultureInvariant | System.Text.RegularExpressions.RegexOptions.IgnoreCase);

    /// <summary>curl's -u / --user user:password.</summary>
    private static readonly System.Text.RegularExpressions.Regex CurlUser = new(@"(?<flag>(?:\s|^)(?:-u|--user)\s+)\S+", System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    /// <summary>
    /// A one-line description of a script run in the guest, for the log: who it runs as (a bot's account, or root),
    /// and its start, with whitespace folded and password hashes hidden. A bot's command is shown without the
    /// account wrapper around it (GuestAccount.Wrap).
    /// </summary>
    public static string Describe(string script, int length = 240)
    {
        ArgumentNullException.ThrowIfNull(script);
        if (length < 20) throw new ArgumentOutOfRangeException(nameof(length));
        var who = "root";
        var body = script;
        if (AsAccount.Match(script) is { Success: true } match)
        {
            who = match.Groups[1].Value;
            // The command follows the account's environment, after its "cd" into the home folder.
            const string start = "cd \"$HOME\" || exit 1\n";
            var at = script.IndexOf(start, match.Index, StringComparison.Ordinal);
            var inner = (at >= 0 ? script[(at + start.Length)..] : script[(match.Index + match.Length)..]).TrimEnd();
            // The wrapper's own closing quote, and its escaped quotes inside.
            if (inner.EndsWith('\'')) inner = inner[..^1];
            body = inner.Replace("'\"'\"'", "'", StringComparison.Ordinal);
        }
        // Credentials a command may carry (a password, token, secret, API key, auth header, user:password in curl or a
        // URL) are masked: the log may go to a syslog server, and bots work in mail and web accounts.
        var masked = AuthValue.Replace(PasswordHash.Replace(body, "$6$…"), "…");
        masked = Credential.Replace(masked, match => match.Groups["name"].Value + match.Groups["sep"].Value + "…");
        masked = UrlCredential.Replace(masked, "${scheme}…@");
        masked = CurlUser.Replace(masked, "${flag}…");
        var oneLine = System.Text.RegularExpressions.Regex.Replace(masked, @"\s+", " ").Trim();
        if (oneLine.Length > length) oneLine = oneLine[..(length - 1)] + "…";
        return $"as {who}: {oneLine}";
    }
}
