using System.Net.Sockets;
using System.Text;

namespace PersonaBot;

/// <summary>Shell commands go exclusively to the VM's second emulated serial port.</summary>
public sealed class GuestShell : IGuestShell, IAsyncDisposable
{
    private TcpClient _socket = new();
    private readonly SemaphoreSlim _gate = new(1);
    private StreamReader? _reader;
    private StreamWriter? _writer;
    public async Task ConnectAsync(int port, CancellationToken token)
    {
        if (port is < 1 or > 65535) throw new ArgumentOutOfRangeException(nameof(port), "Use a valid loopback port.");
        System.Diagnostics.Trace.WriteLine($"GuestShell.ConnectAsync enter: port={port}, reconnect={_reader != null}");
        await _gate.WaitAsync(token);
        try
        {
            // A QEMU started again (after the guest rebooted under WHPX) needs a new connection.
            if (_reader != null)
            {
                _socket.Dispose();
                _socket = new TcpClient();
            }
            await _socket.ConnectAsync("127.0.0.1", port, token);
            _reader = new StreamReader(_socket.GetStream(), Encoding.UTF8, false, leaveOpen: true);
            _writer = new StreamWriter(_socket.GetStream(), new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true, NewLine = "\n" };
        }
        finally { _gate.Release(); }
        System.Diagnostics.Trace.WriteLine($"GuestShell.ConnectAsync: connected to port {port}; quietening the shell");
        await _writer.WriteLineAsync("stty -echo; export PS1=''\n".AsMemory(), token);
        await ExecuteAsync("echo PERSONABOT_GUEST_READY", token);
        System.Diagnostics.Trace.WriteLine($"GuestShell.ConnectAsync exit: shell on port {port} answered");
    }
    public async Task<string> ExecuteAsync(string script, CancellationToken token = default)
    {
        ArgumentNullException.ThrowIfNull(script);
        var watch = System.Diagnostics.Stopwatch.StartNew();
        // Every command run in the guest, in one line: who it runs as, and what (password hashes hidden).
        System.Diagnostics.Trace.WriteLine("GuestShell.ExecuteAsync: " + GuestShellText.Describe(script));
        await _gate.WaitAsync(token);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromMinutes(3));
        try
        {
            var id = Guid.NewGuid().ToString("N");
            var bytes = Encoding.UTF8.GetBytes("export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin LANG=C LC_ALL=C\nunset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY\n" + script.Replace("\r\n", "\n") + "\n");
            if (bytes.Length > 128000)
            {
                System.Diagnostics.Trace.TraceWarning($"GuestShell.ExecuteAsync: script of {bytes.Length} bytes exceeds the 128000-byte transfer limit");
                throw new ArgumentException("Guest script exceeds the transfer limit; write smaller chunks.");
            }
            async Task TransferLine(string command)
            {
                var ack = "ACK_" + Guid.NewGuid().ToString("N");
                await _writer!.WriteLineAsync((command + "; printf '\\n" + ack + "\\n'").AsMemory(), timeout.Token);
                while (true)
                {
                    var response = await _reader!.ReadLineAsync(timeout.Token);
                    if (response is null)
                    {
                        System.Diagnostics.Trace.TraceError($"GuestShell.ExecuteAsync: guest disconnected while transferring the script ({bytes.Length} bytes)");
                        throw new IOException("Guest disconnected during transfer.");
                    }
                    if (response.Trim() == ack) break;
                }
            }
            await TransferLine($": > /tmp/personabot-{id}.sh");
            foreach (var chunk in bytes.Chunk(128))
            {
                await TransferLine($"printf '%b' '{Octal(chunk)}' >> /tmp/personabot-{id}.sh");
            }
            var line = $"printf '\\nSTART_{id}\\n'; timeout -s KILL 150 sh /tmp/personabot-{id}.sh 2>&1; r=$?; printf '\\nEND_{id}:%s\\n' \"$r\"; rm -f /tmp/personabot-{id}.sh";
            await _writer!.WriteLineAsync(line.AsMemory(), timeout.Token);
            var result = new StringBuilder();
            var started = false;
            while (true)
            {
                var output = await _reader!.ReadLineAsync(timeout.Token);
                if (output is null)
                {
                    System.Diagnostics.Trace.TraceError($"GuestShell.ExecuteAsync: guest shell disconnected after {watch.Elapsed.TotalSeconds:0.0}s (started={started}, {result.Length} characters read)");
                    throw new IOException("Guest serial shell disconnected.");
                }
                if (output.Trim() == "START_" + id) { started = true; continue; }
                if (!started) continue;
                if (output.StartsWith("END_" + id + ":"))
                {
                    var code = output.Split(':').Last().Trim();
                    System.Diagnostics.Trace.WriteLine($"GuestShell.ExecuteAsync: exit {code} after {watch.Elapsed.TotalSeconds:0.0}s, {result.Length} characters out");
                    return result + "\nExit code: " + code;
                }
                if (result.Length < 64000) result.AppendLine(output);
            }
        }
        catch (OperationCanceledException)
        {
            System.Diagnostics.Trace.WriteLine($"GuestShell.ExecuteAsync: cancelled after {watch.Elapsed.TotalSeconds:0.0}s (callerCancelled={token.IsCancellationRequested}, limit 3 min)");
            if (_writer != null)
            {
                try { await _writer.WriteAsync("\u0003"); await _writer.FlushAsync(); }
                catch (IOException ex) { System.Diagnostics.Trace.TraceWarning($"GuestShell.ExecuteAsync: couldn't send Ctrl-C: {ex.Message}"); }
            }
            throw;
        }
        finally { _gate.Release(); }
    }
    public async Task SynchronizeAsync(CancellationToken token)
    {
        System.Diagnostics.Trace.WriteLine("GuestShell.SynchronizeAsync enter");
        var watch = System.Diagnostics.Stopwatch.StartNew();
        await _gate.WaitAsync(token);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(15));
        try
        {
            var marker = "PERSONABOT_SYNC_" + Guid.NewGuid().ToString("N");
            // Confirm the serial shell has handled a cancelled foreground command before
            // checkpointing. The acknowledgement must come from the shell, not just QMP.
            await _writer!.WriteAsync("\u0003\n".AsMemory(), timeout.Token);
            await _writer.WriteLineAsync(("printf '\\n" + marker + "\\n'").AsMemory(), timeout.Token);
            while (true)
            {
                var line = await _reader!.ReadLineAsync(timeout.Token);
                if (line is null)
                {
                    System.Diagnostics.Trace.TraceError("GuestShell.SynchronizeAsync: guest disconnected while synchronising");
                    throw new IOException("Guest disconnected while preparing its checkpoint.");
                }
                if (line.Trim() == marker)
                {
                    System.Diagnostics.Trace.WriteLine($"GuestShell.SynchronizeAsync exit: in step after {watch.ElapsedMilliseconds}ms");
                    return;
                }
            }
        }
        catch (OperationCanceledException)
        {
            System.Diagnostics.Trace.TraceWarning($"GuestShell.SynchronizeAsync: no marker after {watch.ElapsedMilliseconds}ms (callerCancelled={token.IsCancellationRequested})");
            throw;
        }
        finally { _gate.Release(); }
    }
    public ValueTask DisposeAsync()
    {
        System.Diagnostics.Trace.WriteLine($"GuestShell.DisposeAsync: closing the serial shell (connected={_socket.Connected})");
        _socket.Dispose();
        return ValueTask.CompletedTask;
    }
    public static string Quote(string text) => GuestShellText.Quote(text);
    public static string Octal(byte[] bytes) => GuestShellText.Octal(bytes);
}
