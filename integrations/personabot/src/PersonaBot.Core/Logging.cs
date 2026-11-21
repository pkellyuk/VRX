using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;
using Automaton.Core.Services;

namespace PersonaBot;

/// <summary>
/// Where PersonaBot's log goes, as Automaton's syslog settings: off by default; when on, every line PersonaBot and
/// Automaton log is sent to a syslog server over UDP. <see cref="MachineLogs"/> adds the machines' own words: QEMU's
/// errors and each guest's serial console (its kernel, systemd and desktop messages), so a crash arrives with its cause.
/// </summary>
public sealed class LogSettings
{
    public bool SyslogEnabled { get; set; }
    public string SyslogServer { get; set; } = "";
    public int SyslogPort { get; set; } = 514;

    /// <summary>The least severe lines sent: Debug, Info, Warn, Error or Critical.</summary>
    public string MinimumSeverity { get; set; } = "Debug";

    /// <summary>Lines are queued and sent on a thread of their own, so logging never slows the app.</summary>
    public bool AsyncLogging { get; set; } = true;
    public bool IncludeTimestamp { get; set; } = true;
    public bool IncludeSeverity { get; set; } = true;
    public bool IncludeHostname { get; set; } = true;
    public bool IncludeThreadId { get; set; } = true;

    /// <summary>Also send QEMU's errors and every guest's serial console, line by line.</summary>
    public bool MachineLogs { get; set; } = true;

    public static IReadOnlyList<string> Severities { get; } = ["Debug", "Info", "Warn", "Error", "Critical"];

    /// <summary>Why these settings can't be used, or null when they can.</summary>
    public string? Problem()
    {
        if (!SyslogEnabled) return null;
        if (string.IsNullOrWhiteSpace(SyslogServer)) return "Type the syslog server's address, or switch syslog off.";
        if (SyslogServer.Trim().Any(char.IsWhiteSpace)) return "The syslog server's address can't contain spaces.";
        if (SyslogPort is < 1 or > 65535) return "The syslog port must be between 1 and 65535.";
        if (!Severities.Contains(MinimumSeverity, StringComparer.OrdinalIgnoreCase)) return "Choose a minimum severity from the list.";
        return null;
    }
}

/// <summary>
/// PersonaBot's logging. Everything PersonaBot writes with <see cref="Trace"/> (its whole code base) is passed to
/// Automaton's logger, <see cref="AutomatonDebug"/>, which Automaton's own code already uses; with syslog on,
/// that logger sends every line to the server. Machines' logs come in through <see cref="Machine"/>.
/// </summary>
public static class PersonaBotLogging
{
    private static readonly object Gate = new();
    private static TraceToAutomaton? _listener;
    private static LogSettings _current = new();

    /// <summary>Whether the machines' own logs are being sent (syslog on, and their option on).</summary>
    public static bool MachineLogsOn
    {
        get { lock (Gate) return _current.SyslogEnabled && _current.MachineLogs; }
    }

    /// <summary>Applies the settings now: at start-up and whenever they are saved.</summary>
    public static void Apply(LogSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        lock (Gate)
        {
            _current = settings;
            var config = new SyslogConfig
            {
                SyslogEnabled = settings.SyslogEnabled && settings.Problem() == null,
                SyslogServerIp = settings.SyslogServer.Trim(),
                SyslogServerPort = settings.SyslogPort,
                SyslogMinimumSeverity = settings.MinimumSeverity.ToLowerInvariant(),
                SyslogAsyncLogging = settings.AsyncLogging,
                SyslogIncludeTimestamp = settings.IncludeTimestamp,
                SyslogIncludeSeverity = settings.IncludeSeverity,
                SyslogIncludeHostname = settings.IncludeHostname,
                SyslogIncludeThreadId = settings.IncludeThreadId
            };
            AutomatonDebug.UpdateConfig(config);
            // PersonaBot's Trace lines join Automaton's log only while syslog is on: otherwise they would only be kept in memory.
            if (config.SyslogEnabled && _listener == null)
            {
                _listener = new TraceToAutomaton();
                Trace.Listeners.Add(_listener);
            }
            else if (!config.SyslogEnabled && _listener != null)
            {
                Trace.Listeners.Remove(_listener);
                _listener = null;
            }
        }
        AutomatonDebug.Log($"PersonaBot logging: syslog {(settings.SyslogEnabled ? $"on, to {settings.SyslogServer.Trim()}:{settings.SyslogPort} from {settings.MinimumSeverity}" : "off")}, machine logs {(settings.SyslogEnabled && settings.MachineLogs ? "on" : "off")}");
    }

    private static readonly Regex TerminalCodes = new(@"\x1B(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1B]*(?:\x07|\x1B\\))|[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]", RegexOptions.CultureInvariant);

    /// <summary>
    /// A line from a machine (QEMU, or its guest's serial console), sent when <see cref="MachineLogsOn"/>: tagged
    /// with where it came from, without terminal colour codes. A line saying the kernel panicked, or naming a
    /// segfault, is sent as an error.
    /// </summary>
    public static void Machine(string machineId, string source, string line)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(machineId);
        ArgumentException.ThrowIfNullOrWhiteSpace(source);
        ArgumentNullException.ThrowIfNull(line);
        if (!MachineLogsOn) return;
        var clean = TerminalCodes.Replace(line, "").Trim();
        if (clean.Length == 0) return;
        var level = clean.Contains("Kernel panic", StringComparison.Ordinal) || clean.Contains("segfault", StringComparison.OrdinalIgnoreCase)
                    || clean.Contains("Segmentation fault", StringComparison.Ordinal) || clean.Contains("Out of memory", StringComparison.Ordinal)
            ? AutomatonDebug.DEBUG_ERROR
            : AutomatonDebug.DEBUG_INFO;
        AutomatonDebug.Log(level, $"[{source} {machineId}] {clean}");
    }

    /// <summary>The level for a PersonaBot line: failures as warnings, the rest as information.</summary>
    internal static int LevelOf(string message) =>
        message.Contains("Exception", StringComparison.Ordinal) || message.Contains(" failed", StringComparison.OrdinalIgnoreCase)
        || message.Contains("couldn't", StringComparison.OrdinalIgnoreCase) || message.Contains("stopped unexpectedly", StringComparison.OrdinalIgnoreCase)
            ? AutomatonDebug.DEBUG_WARNING
            : AutomatonDebug.DEBUG_INFO;

    /// <summary>Passes Trace output to Automaton's logger, a whole line at a time.</summary>
    private sealed class TraceToAutomaton : TraceListener
    {
        private readonly StringBuilder _partial = new();
        [ThreadStatic] private static bool _inside;

        public override void Write(string? message)
        {
            if (message == null) return;
            lock (_partial) _partial.Append(message);
        }

        public override void WriteLine(string? message)
        {
            string line;
            lock (_partial)
            {
                _partial.Append(message);
                line = _partial.ToString();
                _partial.Clear();
            }
            if (_inside || line.Length == 0) return;
            _inside = true;
            try { AutomatonDebug.Log(LevelOf(line), "[PersonaBot] " + line); }
            finally { _inside = false; }
        }

        public override void TraceEvent(TraceEventCache? eventCache, string source, TraceEventType eventType, int id, string? message)
        {
            if (_inside || string.IsNullOrEmpty(message)) return;
            _inside = true;
            try
            {
                var level = eventType switch
                {
                    TraceEventType.Critical => AutomatonDebug.DEBUG_CRITICAL,
                    TraceEventType.Error => AutomatonDebug.DEBUG_ERROR,
                    TraceEventType.Warning => AutomatonDebug.DEBUG_WARNING,
                    _ => AutomatonDebug.DEBUG_INFO
                };
                AutomatonDebug.Log(level, "[PersonaBot] " + message);
            }
            finally { _inside = false; }
        }
    }
}
