using System;
using System.Collections.Generic;
using System.Text;

namespace Automaton.Core.Services;

/// <summary>
/// Static debug logging helper similar to BAS.Debug.
/// Provides log level constants and convenient logging methods.
/// </summary>
public static class AutomatonDebug
{
    public const int DEBUG_INFO_NOMONGO = 4;
    public const int DEBUG_INFO = 0;
    public const int DEBUG_WARNING = 1;
    public const int DEBUG_ERROR = 2;
    public const int DEBUG_CRITICAL = 3;

    private static Syslog? _syslog = null;
    private static SyslogConfig? _savedConfig = null;
    private static readonly object _initLock = new();
    private static readonly object _recentLogLock = new();
    private static bool _initialized = false;
    private static readonly StringBuilder _recentLogBuffer = new(capacity: 4096);
    private const int MaxRecentLogLength = 1000000;
    // Trim in chunks so we don't pay Remove(0, 1) on every overflow.
    private const int RecentLogTrimChunk = 64 * 1024;

    private static string StrAppName = "APPNAME:Automaton";

    /// <summary>
    /// Event raised when a log message is written.
    /// UI components can subscribe to display logs in a window.
    /// </summary>
    public static event Action<string>? LogWritten;

    /// <summary>
    /// Notifies subscribers of a new log entry.
    /// </summary>
    internal static void NotifyLogWritten(string formattedMessage)
    {
        LogWritten?.Invoke(formattedMessage);
    }

    public static string GetRecentLogText()
    {
        lock (_recentLogLock)
        {
            return _recentLogBuffer.ToString();
        }
    }

    public static void ClearRecentLogText()
    {
        lock (_recentLogLock)
        {
            _recentLogBuffer.Clear();
        }
    }

    /// <summary>
    /// Initializes the syslog logger with the specified configuration.
    /// Call this once at application startup.
    /// </summary>
    /// <param name="syslogServerIp">Syslog server IP address</param>
    /// <param name="syslogServerPort">Syslog server port</param>
    /// <param name="runMode">Current run mode (e.g., "Debug", "Release")</param>
    [Obsolete("Use Initialize(SyslogConfig config) instead for full configuration support.")]
    public static void Initialize(string syslogServerIp, int syslogServerPort, string runMode = "")
    {
        if (_initialized) return;

        lock (_initLock)
        {
            if (_initialized) return;

            _syslog = new Syslog(
                syslogServerIp,
                syslogServerPort,
                minimumSeverity: Syslog.Severity.Debug,
                asyncLogging: true,
                includeUtcTimestamp: true,
                includeSeverity: true,
                includeHostname: true,
                includeThreadId: true
            );

            _initialized = true;
        }
    }

    /// <summary>
    /// Initializes or updates the syslog logger with the specified configuration.
    /// Can be called multiple times to update configuration at runtime.
    /// </summary>
    /// <param name="config">The syslog configuration</param>
    public static void Initialize(SyslogConfig config)
    {
        lock (_initLock)
        {
            _savedConfig = config;
            ReplaceSyslogInstance(config);
            _initialized = true;
        }
    }

    /// <summary>
    /// Updates the syslog configuration at runtime.
    /// </summary>
    /// <param name="config">The new syslog configuration</param>
    public static void UpdateConfig(SyslogConfig config)
    {
        lock (_initLock)
        {
            _savedConfig = config;
            ReplaceSyslogInstance(config);
            _initialized = true;
        }
    }

    private static void ReplaceSyslogInstance(SyslogConfig config)
    {
        _syslog?.Close();

        var syslogServerIp = config.SyslogEnabled ? config.SyslogServerIp ?? string.Empty : string.Empty;
        var syslogServerPort = config.SyslogEnabled ? config.SyslogServerPort : 0;

        _syslog = new Syslog(
            syslogServerIp,
            syslogServerPort,
            ParseSeverity(config.SyslogMinimumSeverity),
            config.SyslogAsyncLogging,
            config.SyslogIncludeTimestamp,
            config.SyslogIncludeSeverity,
            config.SyslogIncludeHostname,
            config.SyslogIncludeThreadId
        );
    }

    private static Syslog.Severity ParseSeverity(string? severity)
    {
        return severity?.ToLowerInvariant() switch
        {
            "debug" => Syslog.Severity.Debug,
            "info" => Syslog.Severity.Info,
            "warn" => Syslog.Severity.Warn,
            "error" => Syslog.Severity.Error,
            "alert" => Syslog.Severity.Alert,
            "critical" => Syslog.Severity.Critical,
            "emergency" => Syslog.Severity.Emergency,
            _ => Syslog.Severity.Debug
        };
    }

    /// <summary>
    /// Logs a message at INFO level.
    /// </summary>
    public static void Log(string strMessage)
    {
        Log(DEBUG_INFO, strMessage);
    }

    /// <summary>
    /// Logs a message at INFO level, spelled the way the shared Director client sources
    /// spell it.
    ///
    /// Those files are compiled both here and into the Android Helper, which supplies
    /// its own same-namespace logcat <c>AutomatonDebug</c> where <c>Log</c> is verbose-
    /// gated chatter and <c>LogInfo</c> is always written. Here nothing is gated, so
    /// this is <see cref="Log(string)"/> - but the name has to exist for those files to
    /// compile against the real logger.
    /// </summary>
    public static void LogInfo(string strMessage)
    {
        Log(DEBUG_INFO, strMessage);
    }

    /// <summary>
    /// Logs a message at the specified level.
    /// </summary>
    public static void Log(int logLevel, string strMessage)
    {
        string msg;
        try
        {
            msg = FormatMessage(logLevel, strMessage);
            AppendRecentLogText(msg);
            NotifyLogWritten(msg);
        }
        catch (Exception)
        {
            // Silently fail to avoid logging errors during logging failures
            return;
        }

        try
        {
            if (_syslog == null)
            {
                if (_savedConfig != null)
                {
                    Initialize(_savedConfig);
                }
                else
                {
                    Initialize(new SyslogConfig
                    {
                        SyslogEnabled = false,
                        SyslogServerIp = "",
                        SyslogServerPort = 514,
                        SyslogMinimumSeverity = "debug",
                        SyslogAsyncLogging = true,
                        SyslogIncludeTimestamp = true,
                        SyslogIncludeSeverity = true,
                        SyslogIncludeHostname = true,
                        SyslogIncludeThreadId = true
                    });
                }
            }

            if (_syslog != null)
            {
                Syslog.Severity severity = GetSeverity(logLevel);
                _syslog.Log(severity, msg);
            }
        }
        catch (Exception)
        {
            // Silently fail to avoid logging errors during logging failures
        }
    }

    /// <summary>
    /// Waits, up to <paramref name="timeout"/>, for queued syslog lines to be sent. Call before
    /// the process exits after a fatal error, or the report of it never leaves the machine.
    /// </summary>
    public static bool Flush(TimeSpan timeout)
    {
        try
        {
            return _syslog?.Flush(timeout) ?? true;
        }
        catch (Exception)
        {
            // Logging must never be the thing that fails on the way out.
            return false;
        }
    }

    /// <summary>
    /// Logs an error message.
    /// </summary>
    public static void LogError(string strMessage)
    {
        Log(DEBUG_ERROR, strMessage);
    }

    /// <summary>
    /// Logs a warning message.
    /// </summary>
    public static void LogWarning(string strMessage)
    {
        Log(DEBUG_WARNING, strMessage);
    }

    /// <summary>
    /// Logs an exception with full details.
    /// </summary>
    public static void LogException(string module, string method, Exception e)
    {
        if (_syslog == null) return;
        _syslog.LogException(module, method, e);
    }

    /// <summary>
    /// Logs a critical message.
    /// </summary>
    public static void LogCritical(string strMessage)
    {
        Log(DEBUG_CRITICAL, strMessage);
    }

    private static Syslog.Severity GetSeverity(int logLevel)
    {
        return logLevel switch
        {
            DEBUG_INFO => Syslog.Severity.Info,
            DEBUG_WARNING => Syslog.Severity.Warn,
            DEBUG_ERROR => Syslog.Severity.Error,
            DEBUG_CRITICAL => Syslog.Severity.Critical,
            DEBUG_INFO_NOMONGO => Syslog.Severity.Info,
            _ => Syslog.Severity.Debug
        };
    }

    private static string FormatMessage(int logLevel, string strMessage)
    {
        string severityPrefix = logLevel switch
        {
            DEBUG_INFO_NOMONGO => "INFO",
            DEBUG_INFO => "INFO",
            DEBUG_WARNING => "WARNING",
            DEBUG_ERROR => "ERROR",
            DEBUG_CRITICAL => "CRITICAL",
            _ => "DEBUG"
        };

        string runMode = Environment.GetEnvironmentVariable("RUNMODE") ?? string.Empty;

        return $"{severityPrefix}:{StrAppName}{runMode}:{DateTime.UtcNow:yyyy-MM-dd HH:mm:ss}: {strMessage}";
    }

    private static void AppendRecentLogText(string message)
    {
        if (string.IsNullOrEmpty(message))
        {
            return;
        }

        lock (_recentLogLock)
        {
            if (_recentLogBuffer.Length > 0)
            {
                _recentLogBuffer.Append(Environment.NewLine);
            }
            _recentLogBuffer.Append(message);

            // When we exceed the cap, trim a whole chunk so we don't reallocate per-message.
            var overflow = _recentLogBuffer.Length - MaxRecentLogLength;
            if (overflow > 0)
            {
                _recentLogBuffer.Remove(0, overflow + RecentLogTrimChunk);
            }
        }
    }
}
