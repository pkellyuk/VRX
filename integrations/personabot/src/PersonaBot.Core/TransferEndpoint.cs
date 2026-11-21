using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;

namespace PersonaBot;

/// <summary>A one-shot, token-protected loopback endpoint. QEMU reaches it through its 10.0.2.2 NAT gateway.</summary>
internal sealed class TransferEndpoint : IDisposable
{
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly string _path = "/" + Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
    internal int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;
    internal string PathToken => _path;
    internal string GuestUrl => $"http://10.0.2.2:{Port}{_path}";
    public TransferEndpoint() => _listener.Start();

    internal async Task ServeAsync(string archive, bool receive, CancellationToken token)
    {
        while (true)
        {
            using var client = await _listener.AcceptTcpClientAsync(token);
            using var limit = CancellationTokenSource.CreateLinkedTokenSource(token);
            limit.CancelAfter(TimeSpan.FromMinutes(15));
            var stream = client.GetStream();
            var header = new List<byte>();
            try
            {
                using var headerLimit = CancellationTokenSource.CreateLinkedTokenSource(limit.Token);
                headerLimit.CancelAfter(TimeSpan.FromSeconds(10));
                var one = new byte[1];
                while (header.Count < 8192)
                {
                    await stream.ReadExactlyAsync(one, headerLimit.Token); header.Add(one[0]);
                    if (header.Count >= 4 && header.TakeLast(4).SequenceEqual(new byte[] { 13, 10, 13, 10 })) break;
                }
                var lines = Encoding.ASCII.GetString(header.ToArray()).Split("\r\n");
                var expected = (receive ? "POST " : "GET ") + _path + " HTTP/1.1";
                var lengths = lines.Where(l => l.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase)).ToArray();
                if (lines[0] != expected || header.Count >= 8192 || lines.Any(l => l.StartsWith("Transfer-Encoding:", StringComparison.OrdinalIgnoreCase))
                    || receive && (lengths.Length != 1 || !long.TryParse(lengths[0][15..].Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out var n) || n < 0 || n > TransferArchive.MaximumBytes))
                {
                    await stream.WriteAsync(Encoding.ASCII.GetBytes("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"), limit.Token);
                    continue;
                }
                if (receive)
                {
                    var length = long.Parse(lengths[0][15..].Trim(), CultureInfo.InvariantCulture);
                    await using var output = new FileStream(archive, FileMode.CreateNew);
                    var buffer = new byte[128 * 1024];
                    while (length > 0)
                    {
                        var read = await stream.ReadAsync(buffer.AsMemory(0, (int)Math.Min(buffer.Length, length)), limit.Token);
                        if (read == 0) throw new EndOfStreamException("The guest transfer ended early.");
                        await output.WriteAsync(buffer.AsMemory(0, read), limit.Token); length -= read;
                    }
                    await stream.WriteAsync(Encoding.ASCII.GetBytes("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"), limit.Token);
                }
                else
                {
                    await using var input = File.OpenRead(archive);
                    await stream.WriteAsync(Encoding.ASCII.GetBytes($"HTTP/1.1 200 OK\r\nContent-Length: {input.Length}\r\nContent-Type: application/x-tar\r\nConnection: close\r\n\r\n"), limit.Token);
                    await input.CopyToAsync(stream, limit.Token);
                }
                return;
            }
            catch (OperationCanceledException) when (!token.IsCancellationRequested) { }
            catch (EndOfStreamException) when (header.Count < 4) { }
        }
    }

    public void Dispose() => _listener.Stop();
}
