using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace PersonaBot;

public sealed class QmpClient : IAsyncDisposable
{
    private TcpClient _socket = new();
    private readonly SemaphoreSlim _gate = new(1);
    private StreamReader? _reader;
    private StreamWriter? _writer;
    /// <summary>
    /// Connects and enters command mode. <paramref name="readyTimeout"/> bounds how long QEMU may take
    /// to answer its first command (default 15 s). A VM started with -loadvm answers only after it has
    /// read the whole saved RAM image, which can take minutes, so the resume path passes a longer one.
    /// </summary>
    public async Task ConnectAsync(int port, CancellationToken token, TimeSpan? readyTimeout = null)
    {
        if (port is < 1 or > 65535) throw new ArgumentOutOfRangeException(nameof(port), "Use a valid loopback port.");
        if (readyTimeout is { } limit && limit <= TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(readyTimeout), "The ready timeout must be positive.");
        System.Diagnostics.Trace.WriteLine($"QmpClient.ConnectAsync enter: port={port}, readyTimeout={readyTimeout?.TotalSeconds ?? 15}s, reconnect={_reader != null}");
        var watch = System.Diagnostics.Stopwatch.StartNew();
        await _gate.WaitAsync(token);
        try
        {
            // A QEMU started again (after the guest rebooted under WHPX) needs a new connection.
            if (_reader != null)
            {
                System.Diagnostics.Trace.WriteLine($"QmpClient.ConnectAsync: replacing the previous QMP connection before connecting to port {port}");
                _socket.Dispose();
                _socket = new TcpClient();
            }
            await _socket.ConnectAsync("127.0.0.1", port, token);
            System.Diagnostics.Trace.WriteLine($"QmpClient.ConnectAsync: TCP connected to 127.0.0.1:{port} after {watch.ElapsedMilliseconds}ms");
            _reader = new StreamReader(_socket.GetStream(), Encoding.UTF8, false, leaveOpen: true);
            _writer = new StreamWriter(_socket.GetStream(), new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true, NewLine = "\n" };
        }
        finally { _gate.Release(); }
        using var greetingTimeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        greetingTimeout.CancelAfter(readyTimeout ?? TimeSpan.FromSeconds(15));
        var greeting = await _reader.ReadLineAsync(greetingTimeout.Token);
        if (greeting is null || !greeting.Contains("QMP")) System.Diagnostics.Trace.TraceError($"QmpClient.ConnectAsync: port {port} sent no QMP greeting (got {(greeting is null ? "end of stream" : greeting.Length + " chars")})");
        if (greeting is null || !greeting.Contains("QMP")) throw new IOException("QEMU did not send a QMP greeting.");
        System.Diagnostics.Trace.WriteLine($"QmpClient.ConnectAsync: greeting received after {watch.ElapsedMilliseconds}ms; entering command mode");
        await ExecuteAsync("qmp_capabilities", null, token, readyTimeout);
        System.Diagnostics.Trace.WriteLine($"QmpClient.ConnectAsync exit: QMP ready on port {port} after {watch.ElapsedMilliseconds}ms");
    }
    public async Task<JsonElement> ExecuteAsync(string operation, object? arguments = null, CancellationToken token = default, TimeSpan? timeoutDuration = null)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(timeoutDuration ?? TimeSpan.FromSeconds(15));
        try
        {
            await _gate.WaitAsync(timeout.Token);
        }
        catch (OperationCanceledException)
        {
            System.Diagnostics.Trace.TraceWarning($"QmpClient.ExecuteAsync: '{operation}' timed out or was cancelled waiting for the QMP gate (callerCancelled={token.IsCancellationRequested})");
            throw;
        }
        try
        {
            var id = Guid.NewGuid().ToString("N");
            await _writer!.WriteLineAsync(JsonSerializer.Serialize(new { execute = operation, arguments = arguments ?? new { }, id }).AsMemory(), timeout.Token);
            while (true)
            {
                var line = await _reader!.ReadLineAsync(timeout.Token);
                if (line is null)
                {
                    System.Diagnostics.Trace.TraceError($"QmpClient.ExecuteAsync: QEMU disconnected while waiting for '{operation}'");
                    throw new IOException("QEMU disconnected.");
                }
                using var doc = JsonDocument.Parse(line);
                var root = doc.RootElement;
                if (!root.TryGetProperty("id", out var receivedId) || receivedId.GetString() != id) continue;
                if (root.TryGetProperty("error", out var error))
                {
                    System.Diagnostics.Trace.TraceWarning($"QmpClient.ExecuteAsync: '{operation}' failed: {error.GetRawText()}");
                    throw new IOException(error.GetRawText());
                }
                return root.GetProperty("return").Clone();
            }
        }
        catch (OperationCanceledException)
        {
            System.Diagnostics.Trace.TraceWarning($"QmpClient.ExecuteAsync: '{operation}' timed out or was cancelled (callerCancelled={token.IsCancellationRequested}, limit={(timeoutDuration ?? TimeSpan.FromSeconds(15)).TotalSeconds}s)");
            throw;
        }
        finally { _gate.Release(); }
    }
    public void Execute(string name, object args) => ExecuteAsync(name, args).GetAwaiter().GetResult();
    public ValueTask DisposeAsync()
    {
        System.Diagnostics.Trace.WriteLine($"QmpClient.DisposeAsync: closing QMP connection (connected={_socket.Connected})");
        _socket.Dispose();
        return ValueTask.CompletedTask;
    }
}
