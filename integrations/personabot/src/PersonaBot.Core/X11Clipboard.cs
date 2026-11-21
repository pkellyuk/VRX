using System.Text;

namespace PersonaBot;

/// <summary>The main Linux desktop's CLIPBOARD selection, independent of persona and AI screens.</summary>
public sealed class X11Clipboard(IGuestShell shell, GuestProfile profile)
{
    public const int MaximumBytes = 16 * 1024;
    private static readonly UTF8Encoding Utf8 = new(false, true);

    private string Desktop(string command) => "exec su -s /bin/sh " + GuestShellText.Quote(profile.DesktopUser)
        + " -c " + GuestShellText.Quote("export DISPLAY=:0 HOME=" + GuestShellText.Quote(profile.DesktopHome)
            + " LANG=C.UTF-8; " + command);

    public async Task EnsureAsync(CancellationToken token)
    {
        var install = profile.AccountStyle == GuestAccountStyle.TinyCore
            ? "su tc -c 'tce-load -wi xclip'"
            : "DEBIAN_FRONTEND=noninteractive apt-get update >/dev/null 2>&1 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends xclip >/dev/null 2>&1";
        var result = await shell.ExecuteAsync("command -v xclip >/dev/null 2>&1 || { " + install + "; }; command -v xclip >/dev/null 2>&1", token);
        Success(result);
    }

    public async Task<string?> ReadAsync(CancellationToken token)
    {
        var result = await shell.ExecuteAsync(Desktop("umask 077; f=$(mktemp) || exit 1; trap 'rm -f \"$f\"' EXIT; "
            + "if timeout 2 xclip -selection clipboard -o -target UTF8_STRING >\"$f\" 2>/dev/null; then "
            + $"if [ $(wc -c <\"$f\") -le {MaximumBytes} ]; then printf 'PB_CLIP:'; base64 <\"$f\" | tr -d '\\r\\n'; printf '\\n'; fi; fi"), token);
        Success(result);
        var line = result.Split('\n').FirstOrDefault(l => l.StartsWith("PB_CLIP:", StringComparison.Ordinal));
        if (line == null) return null; // No text selection, an image, or over the transfer limit.
        var bytes = Convert.FromBase64String(line[8..].TrimEnd('\r'));
        if (bytes.Length > MaximumBytes) throw new IOException("Clipboard text exceeds the transfer limit.");
        return Utf8.GetString(bytes);
    }

    public async Task WriteAsync(string text, CancellationToken token)
    {
        var bytes = Utf8.GetBytes(text);
        if (bytes.Length > MaximumBytes) throw new ArgumentException("Clipboard text exceeds 16 KiB.");
        // The credential-shaped variable is redacted by GuestShellText.Describe, including inside su quoting.
        var command = "clipboard_secret=" + Convert.ToBase64String(bytes)
            + "; printf '%s' \"$clipboard_secret\" | base64 -d | xclip -selection clipboard -i >/dev/null 2>&1";
        Success(await shell.ExecuteAsync(Desktop(command), token));
    }

    public async Task<GuestFileClipboard?> ReadFilesAsync(CancellationToken token)
    {
        var result = await shell.ExecuteAsync(Desktop("umask 077; f=$(mktemp); trap 'rm -f \"$f\"' EXIT; "
            + "for target in x-special/gnome-copied-files x-special/nautilus-clipboard text/uri-list; do "
            + "if timeout 1 xclip -selection clipboard -o -target \"$target\" >\"$f\" 2>/dev/null && test -s \"$f\"; then "
            + "printf 'PB_FILES:'; head -c 16385 \"$f\" | base64 | tr -d '\\r\\n'; printf '\\nPB_TIME:'; "
            + "timeout 1 xclip -selection clipboard -o -target TIMESTAMP 2>/dev/null | base64 | tr -d '\\r\\n'; printf '\\n'; break; fi; done"), token);
        Success(result);
        var lines = result.Split('\n');
        var payload = lines.FirstOrDefault(l => l.StartsWith("PB_FILES:", StringComparison.Ordinal));
        if (payload == null) return null;
        var bytes = Convert.FromBase64String(payload[9..].TrimEnd('\r'));
        if (bytes.Length > MaximumBytes) throw new IOException("Too many files on the guest clipboard.");
        var content = Utf8.GetString(bytes);
        var paths = GuestFileClipboard.ParsePaths(content);
        if (paths.Count == 0) return null;
        return new GuestFileClipboard(string.Join("\n", lines.Where(l => l.StartsWith("PB_FILES:", StringComparison.Ordinal) || l.StartsWith("PB_TIME:", StringComparison.Ordinal))), paths);
    }

    private static void Success(string result)
    {
        if (!result.TrimEnd().EndsWith("\nExit code: 0", StringComparison.Ordinal))
            throw new IOException("The guest clipboard helper is unavailable.");
    }
}

public sealed record GuestFileClipboard(string Signature, IReadOnlyList<string> Paths)
{
    public static IReadOnlyList<string> ParsePaths(string value)
    {
        var paths = new List<string>();
        foreach (var line in value.Replace("\r\n", "\n").Split('\n'))
        {
            if (line.Length == 0 || line.StartsWith('#') || line is "copy" or "cut" or "x-special/nautilus-clipboard") continue;
            if (!Uri.TryCreate(line, UriKind.Absolute, out var uri) || !uri.IsFile || uri.Host is not ("" or "localhost"))
                throw new IOException("Only local Linux files can be copied to Windows.");
            var path = Uri.UnescapeDataString(uri.AbsolutePath).TrimEnd('/');
            if (!path.StartsWith('/') || path.Contains('\0')) throw new IOException("Invalid Linux clipboard path.");
            if (!paths.Contains(path, StringComparer.Ordinal)) paths.Add(path);
            if (paths.Count > 128) throw new IOException("Copy at most 128 top-level items at a time.");
        }
        return paths;
    }
}
