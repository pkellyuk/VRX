using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;

namespace Automaton.Core.Services;

/// <summary>
/// Syslog client for sending log messages to a remote syslog server.
/// Similar to BAS LoggingModule.
/// </summary>
public class Syslog
{
    #region Public-Enums

    public enum Severity
    {
        Debug = 0,
        Info = 1,
        Warn = 2,
        Error = 3,
        Alert = 4,
        Critical = 5,
        Emergency = 6
    }

    #endregion

    #region Public-Members

    public string SyslogServerIp { get; set; } = "";
    public int SyslogServerPort { get; set; } = 0;
    public Severity MinimumSeverity { get; set; } = Severity.Debug;
    public bool AsyncLogging { get; set; } = false;
    public bool IncludeUtcTimestamp { get; set; } = true;
    public bool IncludeSeverity { get; set; } = true;
    public bool IncludeHostname { get; set; } = true;
    public bool IncludeThreadId { get; set; } = true;

    #endregion

    #region Private-Members

    private UdpClient? _udp = null;
    private string? _hostname = null;
    private object _sendLock = new();

    /// <summary>
    /// The asynchronous path. One background thread drains this; a full queue drops the oldest
    /// line rather than blocking the caller or growing without bound.
    /// </summary>
    /// <remarks>
    /// This used to be a <c>Task.Run</c> per line. Under a burst - the search index rebuilding
    /// every conversation produced 28,000 lines a minute - that put hundreds of pool work items
    /// a second behind one send lock, and the thread pool the rest of the application runs on
    /// (streaming continuations, saves, remote polling) starved behind them. The desktop's UI
    /// appeared to lock. A dedicated thread cannot do that to anyone else, whatever the volume.
    /// </remarks>
    private const int QueueCapacity = 20_000;
    private readonly Channel<string> _queue = Channel.CreateBounded<string>(new BoundedChannelOptions(QueueCapacity)
    {
        FullMode = BoundedChannelFullMode.DropOldest,
        SingleReader = true,
        SingleWriter = false,
    });
    private Thread? _sender;
    private readonly object _senderLock = new();
    private long _dropped;
    private int _baseDepth = 0;

    #endregion

    #region Constructor

    /// <summary>
    /// Initializes the syslog client.
    /// </summary>
    /// <param name="syslogServerIp">Syslog server IP address</param>
    /// <param name="syslogServerPort">Syslog server port</param>
    /// <param name="minimumSeverity">Minimum severity to send</param>
    /// <param name="asyncLogging">Use async logging (start a task)</param>
    /// <param name="includeUtcTimestamp">Include timestamp in log message</param>
    /// <param name="includeSeverity">Include severity in log message</param>
    /// <param name="includeHostname">Include hostname in log message</param>
    /// <param name="includeThreadId">Include thread ID in log message</param>
    public Syslog(
        string syslogServerIp,
        int syslogServerPort,
        Severity minimumSeverity,
        bool asyncLogging,
        bool includeUtcTimestamp,
        bool includeSeverity,
        bool includeHostname,
        bool includeThreadId)
    {
        SyslogServerIp = syslogServerIp;
        SyslogServerPort = syslogServerPort;
        MinimumSeverity = minimumSeverity;
        AsyncLogging = asyncLogging;
        IncludeUtcTimestamp = includeUtcTimestamp;
        IncludeSeverity = includeSeverity;
        IncludeHostname = includeHostname;
        IncludeThreadId = includeThreadId;

        if (!string.IsNullOrEmpty(SyslogServerIp) && SyslogServerPort > 0)
        {
            try
            {
                _udp = new UdpClient(SyslogServerIp, SyslogServerPort);
            }
            catch (Exception e)
            {
                Console.WriteLine("---");
                Console.WriteLine("");
                Console.WriteLine("NOTICE:");
                Console.WriteLine("");
                Console.WriteLine($"Exception while initializing UDP syslog client: {e.Message}");
                Console.WriteLine($"Syslog logging to server {SyslogServerIp}:{SyslogServerPort} is DISABLED as a result");
                Console.WriteLine("");
                Console.WriteLine("---");
            }
        }

        _hostname = Dns.GetHostName();
        StackTrace st = new StackTrace();
        _baseDepth = st.FrameCount - 1;
    }

    #endregion

    #region Public-Methods

    /// <summary>
    /// Closes the UDP connection.
    /// </summary>
    public void Close()
    {
        _queue.Writer.TryComplete();
        _udp?.Close();
    }

    /// <summary>
    /// Logs a message at Debug severity.
    /// </summary>
    public void Log(string msg)
    {
        Log(Severity.Debug, msg);
    }

    /// <summary>
    /// Logs a message at the specified severity.
    /// </summary>
    public void Log(Severity sev, string msg)
    {
        if (string.IsNullOrEmpty(msg)) return;
        if (sev < MinimumSeverity) return;
        if (string.IsNullOrEmpty(SyslogServerIp) || SyslogServerPort <= 0) return;

        if (_udp == null)
        {
            try
            {
                _udp = new UdpClient(SyslogServerIp, SyslogServerPort);
            }
            catch
            {
                return;
            }
        }

        var message = FormatLogLine(sev, msg);

        if (!AsyncLogging)
        {
            SendToSyslog(message);
            return;
        }

        EnsureSender();
        if (!_queue.Writer.TryWrite(message))
        {
            // Only happens once the channel is completed on Close; a full queue drops the
            // oldest line inside TryWrite and still returns true.
            Interlocked.Increment(ref _dropped);
        }
    }

    /// <summary>Starts the one sender thread, the first time a line is queued.</summary>
    private void EnsureSender()
    {
        if (_sender != null)
        {
            return;
        }

        lock (_senderLock)
        {
            if (_sender != null)
            {
                return;
            }

            _sender = new Thread(SenderLoop)
            {
                IsBackground = true,
                Name = "syslog-sender",
                Priority = ThreadPriority.BelowNormal,
            };
            _sender.Start();
        }
    }

    private void SenderLoop()
    {
        try
        {
            var reader = _queue.Reader;
            while (reader.WaitToReadAsync().AsTask().GetAwaiter().GetResult())
            {
                while (reader.TryRead(out var message))
                {
                    SendToSyslog(message);
                }
            }
        }
        catch (Exception e)
        {
            Console.WriteLine($"Syslog sender stopped: {e.Message}");
        }
    }

    /// <summary>
    /// Waits, up to <paramref name="timeout"/>, for the queued lines to be sent. For a process
    /// about to exit: the sender is a background thread, so without this the last lines before
    /// a crash - the ones that say what crashed - die with it. Returns whether the queue emptied.
    /// </summary>
    public bool Flush(TimeSpan timeout)
    {
        if (!AsyncLogging || _sender == null)
        {
            return true;
        }

        var deadline = DateTime.UtcNow + timeout;
        while (_queue.Reader.Count > 0)
        {
            if (DateTime.UtcNow >= deadline)
            {
                return false;
            }

            Thread.Sleep(20);
        }

        // The last line has been taken off the queue; give its send a moment to finish.
        Thread.Sleep(50);
        return true;
    }

    /// <summary>Lines the bounded queue has discarded because the sender fell behind. For diagnostics.</summary>
    public long DroppedLines => Interlocked.Read(ref _dropped);

    private string FormatLogLine(Severity sev, string msg)
    {
        if (!IncludeUtcTimestamp && !IncludeSeverity && !IncludeHostname && !IncludeThreadId)
        {
            return msg;
        }

        var sb = new StringBuilder(64 + msg.Length);
        if (IncludeUtcTimestamp)
        {
            sb.Append(DateTime.UtcNow.ToString("MM/dd/yyyy HH:mm:ss")).Append(' ');
        }
        if (IncludeSeverity)
        {
            sb.Append(sev.ToString()).Append(' ');
        }
        if (IncludeHostname)
        {
            sb.Append(_hostname).Append(' ');
        }
        if (IncludeThreadId)
        {
            sb.Append("thr-").Append(Thread.CurrentThread.ManagedThreadId).Append(' ');
        }
        sb.Append(msg);
        return sb.ToString();
    }

    /// <summary>
    /// Logs an exception with full details.
    /// </summary>
    public void LogException(string module, string method, Exception e)
    {
        var st = new StackTrace(e, true);
        var frame = st.GetFrame(0);
        if (frame == null)
        {
            throw new InvalidOperationException("Stack frame is null.");
        }

        int fileLine = frame.GetFileLineNumber();
        string? filename = frame.GetFileName();

        string message =
            Environment.NewLine +
            "---" + Environment.NewLine +
            "An exception was encountered which triggered this message" + Environment.NewLine +
            $"  Module     : {module}" + Environment.NewLine +
            $"  Method     : {method}" + Environment.NewLine +
            $"  Type       : {e.GetType().ToString()}" + Environment.NewLine +
            $"  Data       : {e.Data}" + Environment.NewLine +
            $"  Inner      : {e.InnerException}" + Environment.NewLine +
            $"  Message    : {e.Message}" + Environment.NewLine +
            $"  Source     : {e.Source}" + Environment.NewLine +
            $"  StackTrace : {e.StackTrace}" + Environment.NewLine +
            $"  Stack      : {StackToString()}" + Environment.NewLine +
            $"  Line       : {fileLine}" + Environment.NewLine +
            $"  File       : {filename}" + Environment.NewLine +
            $"  ToString   : {e.ToString()}" + Environment.NewLine +
            $"  Servername : {Dns.GetHostName()}" + Environment.NewLine +
            "---";

        Log(Severity.Alert, message);
    }

    #endregion

    #region Private-Methods

    private void SendToSyslog(string msg)
    {
        if (string.IsNullOrEmpty(msg)) return;

        if (!string.IsNullOrEmpty(SyslogServerIp) && SyslogServerPort > 0)
        {
            lock (_sendLock)
            {
                if(_udp == null)
                {
                    _udp = new UdpClient(SyslogServerIp, SyslogServerPort);
                }

                if (_udp != null)
                {
                    byte[] data = Encoding.UTF8.GetBytes(msg);

                    try
                    {
                        _udp.Send(data, data.Length);
                    }
                    catch (Exception e)
                    {
                        Console.WriteLine($"Exception while sending to syslog server {SyslogServerIp}:{SyslogServerPort}, disabling: {e.Message}");
                        _udp = null;
                    }
                }
            }
        }
    }

    private string StackToString()
    {
        var t = new StackTrace();
        var frameCount = t.FrameCount;
        if (frameCount <= 0)
        {
            return string.Empty;
        }

        var sb = new StringBuilder(frameCount * 32);
        for (int i = 0; i < frameCount; i++)
        {
            if (i > 0)
            {
                sb.Append(" <= ");
            }
            sb.Append(t.GetFrame(i)?.GetMethod()?.Name ?? "unknown");
        }
        return sb.ToString();
    }

    #endregion
}
