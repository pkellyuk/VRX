namespace Automaton.Core.Services;

/// <summary>
/// Configuration for the syslog logging service.
/// </summary>
public class SyslogConfig
{
    public bool SyslogEnabled { get; set; } = false;
    public string? SyslogServerIp { get; set; }
    public int SyslogServerPort { get; set; } = 514;
    public string SyslogMinimumSeverity { get; set; } = "Debug";
    public bool SyslogAsyncLogging { get; set; } = true;
    public bool SyslogIncludeTimestamp { get; set; } = true;
    public bool SyslogIncludeSeverity { get; set; } = true;
    public bool SyslogIncludeHostname { get; set; } = true;
    public bool SyslogIncludeThreadId { get; set; } = true;
}
