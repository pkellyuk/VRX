using System.Diagnostics;
using System.Text;

namespace PersonaBot;

/// <summary>
/// The Bot terminal: every command a bot runs with its bash tool, and every file it reads or writes, is
/// shown with its output in that bot's own terminal window on the desktop, so the person watching sees
/// the shell work as it happens instead of nothing. The command's output goes to a log in the bot's home
/// folder (~/.personabot/terminal.log); the window follows the log, and the bot gets back exactly what
/// its command printed, as before. These scripts run as the bot's own Linux user (inside
/// <see cref="GuestAccount.Wrap"/>), never as root.
/// </summary>
public static class BotTerminal
{
    /// <summary>The window's helper, installed by <see cref="GuestAccount.ProvisionScript"/>.</summary>
    public const string Helper = GuestAccount.BinFolder + "/personabot-terminal";

    /// <summary>The X class the window gets, so window managers can be told not to give it focus.</summary>
    public const string WindowName = "personabot-terminal";

    /// <summary>The longest command shown in full in the window; longer ones show their first lines.</summary>
    public const int MaxShownLines = 40;

    /// <summary>Above this the log is cut to its last part, so it never grows without bound.</summary>
    public const int MaxLogBytes = 2_000_000;

    private const string Setup = """
        pb_dir="$HOME/.personabot"; pb_log="$pb_dir/terminal.log"
        mkdir -p "$pb_dir" && touch "$pb_log"
        if [ "$(wc -c < "$pb_log")" -gt MAXLOG ]; then tail -c 200000 "$pb_log" > "$pb_log.cut" && cat "$pb_log.cut" > "$pb_log"; rm -f "$pb_log.cut"; fi

        """;

    /// <summary>
    /// A script that shows <paramref name="command"/> in the bot's terminal window, runs it with its output
    /// going to the window, then prints that same output for the bot and exits with the command's status.
    /// Standard input is /dev/null, so a command that asks a question fails instead of hanging.
    /// </summary>
    public static string VisibleCommand(string command, GuestAccount account)
    {
        ArgumentNullException.ThrowIfNull(command);
        ArgumentNullException.ThrowIfNull(account);
        if (account.Style == GuestAccountStyle.Android)
        {
            Trace.WriteLine($"BotTerminal.VisibleCommand: Android has no terminal window; {command.Length} chars run as they are");
            return command;
        }
        var shown = ShownCommand(command, account.User);
        Trace.WriteLine($"BotTerminal.VisibleCommand: {account.User}, {command.Length} chars");
        return Setup.Replace("MAXLOG", MaxLogBytes.ToString(System.Globalization.CultureInfo.InvariantCulture), StringComparison.Ordinal)
            + $"{Helper} {GuestShellText.Quote(Title(account))} >/dev/null 2>&1\n"
            + $"printf '%s\\n' {GuestShellText.Quote(shown)} >> \"$pb_log\"\n"
            + "pb_start=$(wc -c < \"$pb_log\")\n"
            + "(\n" + command.Replace("\r\n", "\n") + "\n) < /dev/null >> \"$pb_log\" 2>&1\n"
            + "pb_rc=$?\n"
            + "tail -c +$((pb_start + 1)) \"$pb_log\"\n"
            + "if [ \"$pb_rc\" -ne 0 ]; then printf '\\033[31m[exit %s]\\033[0m\\n' \"$pb_rc\" >> \"$pb_log\"; fi\n"
            + "exit $pb_rc";
    }

    /// <summary>A script that adds one line, such as "[read] notes.txt", to the bot's terminal window, then runs <paramref name="script"/>.</summary>
    public static string WithNote(string note, string script, GuestAccount account)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(note);
        ArgumentNullException.ThrowIfNull(script);
        ArgumentNullException.ThrowIfNull(account);
        if (account.Style == GuestAccountStyle.Android)
        {
            Trace.WriteLine("BotTerminal.WithNote: Android has no terminal window; script runs as it is");
            return script;
        }
        Trace.WriteLine($"BotTerminal.WithNote: {account.User}: {note}");
        return Setup.Replace("MAXLOG", MaxLogBytes.ToString(System.Globalization.CultureInfo.InvariantCulture), StringComparison.Ordinal)
            + $"{Helper} {GuestShellText.Quote(Title(account))} >/dev/null 2>&1\n"
            + $"printf '\\033[36m%s\\033[0m\\n' {GuestShellText.Quote(note)} >> \"$pb_log\"\n"
            + script;
    }

    /// <summary>The window's title: the bot's name, so several bots' windows can be told apart.</summary>
    public static string Title(GuestAccount account)
    {
        ArgumentNullException.ThrowIfNull(account);
        return $"{account.DisplayName} · terminal";
    }

    /// <summary>
    /// The command as the window shows it: a green prompt, then the command, continuation lines marked
    /// "&gt;" as a shell shows them, at most <see cref="MaxShownLines"/> lines.
    /// </summary>
    public static string ShownCommand(string command, string user)
    {
        ArgumentNullException.ThrowIfNull(command);
        ArgumentException.ThrowIfNullOrWhiteSpace(user);
        var lines = command.Replace("\r\n", "\n").Trim('\n').Split('\n');
        var text = new StringBuilder();
        text.Append("\n\u001b[1;32m").Append(user).Append("@personabot:~$\u001b[0m ").Append(lines[0]);
        foreach (var line in lines.Skip(1).Take(MaxShownLines - 1)) text.Append("\n> ").Append(line);
        if (lines.Length > MaxShownLines) text.Append($"\n> … ({lines.Length - MaxShownLines} more lines)");
        return text.ToString();
    }

    /// <summary>
    /// personabot-terminal [title]: opens this bot's terminal window, following its log, unless it is
    /// already open. It hands focus back to the window that had it, so a bot typing into Firefox isn't
    /// interrupted when its terminal first appears.
    /// </summary>
    public const string HelperScript = """
        #!/bin/sh
        # personabot-terminal [title]: shows this bot's commands and their output in a window on the desktop.
        dir="$HOME/.personabot"; log="$dir/terminal.log"; pidfile="$dir/terminal.pid"
        mkdir -p "$dir" && touch "$log"
        if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then exit 0; fi
        term=$(command -v xterm || command -v aterm || command -v x-terminal-emulator || true)
        if [ -z "$term" ]; then echo "No terminal program is installed; commands are still logged in $log."; exit 0; fi
        export DISPLAY="${DISPLAY:-:0}"
        previous=$(xdotool getactivewindow 2>/dev/null || true)
        title="${1:-PersonaBot terminal}"
        case "$term" in
            *xterm) nohup "$term" -name personabot-terminal -T "$title" -geometry 110x16-6-36 -bg '#0b1020' -fg '#dce5f5' -sl 5000 -e tail -n 300 -F "$log" >/dev/null 2>&1 & ;;
            *) nohup "$term" -name personabot-terminal -title "$title" -geometry 110x16-6-36 -e tail -n 300 -F "$log" >/dev/null 2>&1 & ;;
        esac
        echo $! > "$pidfile"
        if [ -n "$previous" ]; then sleep 1; xdotool windowactivate "$previous" >/dev/null 2>&1 || true; fi
        exit 0

        """;
}
