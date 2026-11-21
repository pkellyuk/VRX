using System.Diagnostics;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace PersonaBot;

/// <summary>
/// Listens to QEMU's events on a QMP connection of its own and logs each one as it happens: the guest panicking
/// (GUEST_PANICKED, through the pvpanic device), resets, shutdowns, stops and the rest. The commands' own QMP
/// connection only reads events while a command waits for its answer, so a machine's last event was never seen.
/// </summary>
public static class QmpEventLog
{
    /// <summary>Events that mean the machine is in trouble, logged as errors.</summary>
    private static readonly HashSet<string> Trouble = new(StringComparer.Ordinal) { "GUEST_PANICKED", "GUEST_CRASHLOADED", "BLOCK_IO_ERROR", "WATCHDOG" };

    /// <summary>Starts listening on a loopback port for one QEMU; cancel the result to stop.</summary>
    public static CancellationTokenSource Listen(int port, string machineId, string machineFolder)
    {
        if (port is < 1 or > 65535) throw new ArgumentOutOfRangeException(nameof(port));
        ArgumentException.ThrowIfNullOrWhiteSpace(machineId);
        ArgumentException.ThrowIfNullOrWhiteSpace(machineFolder);
        var stop = new CancellationTokenSource();
        Trace.WriteLine($"QmpEventLog.Listen: {machineId} events on port {port}");
        _ = Task.Run(() => RunAsync(port, machineId, machineFolder, stop.Token));
        return stop;
    }

    private static async Task RunAsync(int port, string machineId, string machineFolder, CancellationToken token)
    {
        Trace.WriteLine($"QmpEventLog.RunAsync enter: {machineId} on port {port}");
        try
        {
            using var socket = new TcpClient();
            // QEMU is already up when this starts (its command connection works), but give it a moment.
            for (var attempt = 0; ; attempt++)
            {
                try { await socket.ConnectAsync("127.0.0.1", port, token); break; }
                catch (SocketException ex) when (attempt < 20)
                {
                    // The first refusal only: QEMU may need a moment to listen.
                    if (attempt == 0) Trace.WriteLine($"QmpEventLog.RunAsync: {machineId} events port {port} not listening yet ({ex.SocketErrorCode}); retrying");
                    await Task.Delay(250, token);
                }
            }
            using var reader = new StreamReader(socket.GetStream(), Encoding.UTF8, false, leaveOpen: true);
            await using var writer = new StreamWriter(socket.GetStream(), new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true, NewLine = "\n" };
            await reader.ReadLineAsync(token);
            await writer.WriteLineAsync("{\"execute\":\"qmp_capabilities\"}".AsMemory(), token);
            Trace.WriteLine($"QmpEventLog: listening to {machineId}'s QEMU events");
            while (await reader.ReadLineAsync(token) is { } line)
            {
                if (Describe(line) is not { } described) continue;
                var (name, text) = described;
                if (Trouble.Contains(name)) Trace.TraceError($"QEMU event on {machineId}: {text}");
                else Trace.WriteLine($"QEMU event on {machineId}: {text}");
                // Always into the machine's diary (and syslog when machine logs are on).
                MachineDiary.Write(machineFolder, machineId, "qemu-event", text);
            }
            Trace.WriteLine($"QmpEventLog.RunAsync exit: {machineId}'s QEMU closed the connection");
        }
        catch (OperationCanceledException) { Trace.WriteLine($"QmpEventLog.RunAsync exit: {machineId} stopped listening"); }
        catch (Exception ex) when (ex is IOException or SocketException or ObjectDisposedException)
        {
            Trace.WriteLine($"QmpEventLog.RunAsync exit: {machineId}'s events connection ended: {ex.Message}");
        }
    }

    /// <summary>An event line as its name and one line for the log ("GUEST_PANICKED {"action":"pause",…}"); null for anything else.</summary>
    public static (string Name, string Text)? Describe(string line)
    {
        ArgumentNullException.ThrowIfNull(line);
        try
        {
            using var doc = JsonDocument.Parse(line);
            if (!doc.RootElement.TryGetProperty("event", out var name) || name.GetString() is not { Length: > 0 } eventName) return null;
            var data = doc.RootElement.TryGetProperty("data", out var details) ? " " + details.GetRawText() : "";
            return (eventName, eventName + data);
        }
        catch (JsonException) { return null; }
    }
}
