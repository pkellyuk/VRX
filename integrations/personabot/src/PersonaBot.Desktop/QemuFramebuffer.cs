using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using SkiaSharp;

namespace PersonaBot;

/// <summary>
/// Reads QEMU's EGL readback through its loopback-only RFB listener. Accelerated scanout has no
/// QMP screendump surface once Linux enables VirGL. The main display's input still goes exclusively
/// through QMP; a bot's own display takes input over RFB (<see cref="RfbInput"/>).
/// Only raw, true-colour pixels and desktop-size updates are negotiated.
/// </summary>
internal sealed class QemuFramebuffer(bool swapRedAndBlue = false, Action<byte[]>? audio = null) : IAsyncDisposable
{
    private const int AudioEncoding = -259;
    private bool _audioStarted;
    private readonly TcpClient _client = new() { NoDelay = true };
    private NetworkStream? _stream;
    private byte[] _pixels = [];
    private byte[] _rectangle = [];
    private int _width;
    private int _height;

    /// <summary>The negotiated connection, for a client that only sends input on it (<see cref="RfbInput"/>).</summary>
    internal Stream Stream => _stream ?? throw new InvalidOperationException("QEMU's framebuffer isn't connected.");

    public async Task ConnectAsync(int port, CancellationToken token)
    {
        System.Diagnostics.Trace.WriteLine($"QemuFramebuffer.ConnectAsync enter: loopback port {port}");
        var watch = System.Diagnostics.Stopwatch.StartNew();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(15));
        token = timeout.Token;
        await _client.ConnectAsync(IPAddress.Loopback, port, token);
        _stream = _client.GetStream();
        System.Diagnostics.Trace.WriteLine($"QemuFramebuffer.ConnectAsync: TCP connected to port {port} after {watch.ElapsedMilliseconds}ms; reading RFB version");
        var version = Encoding.ASCII.GetString(await ReadAsync(12, token));
        if (version != "RFB 003.008\n")
        {
            System.Diagnostics.Trace.TraceError($"QemuFramebuffer.ConnectAsync: unsupported RFB version '{version.Trim()}' on port {port}");
            throw new IOException("QEMU's framebuffer protocol is unsupported.");
        }
        await _stream.WriteAsync(Encoding.ASCII.GetBytes(version), token);
        var count = (await ReadAsync(1, token))[0];
        if (count == 0)
        {
            System.Diagnostics.Trace.TraceError($"QemuFramebuffer.ConnectAsync: QEMU offered no security types on port {port}");
            throw new IOException("QEMU refused the framebuffer connection.");
        }
        var security = await ReadAsync(count, token);
        if (!security.Contains((byte)1))
        {
            System.Diagnostics.Trace.TraceError($"QemuFramebuffer.ConnectAsync: QEMU on port {port} offered only security types [{string.Join(',', security)}], no 'None'");
            throw new IOException("QEMU's local framebuffer requires unsupported authentication.");
        }
        await _stream.WriteAsync(new byte[] { 1 }, token);
        var securityResult = BinaryPrimitives.ReadUInt32BigEndian(await ReadAsync(4, token));
        if (securityResult != 0)
        {
            System.Diagnostics.Trace.TraceError($"QemuFramebuffer.ConnectAsync: QEMU on port {port} refused the handshake, security result {securityResult}");
            throw new IOException("QEMU refused the framebuffer handshake.");
        }
        await _stream.WriteAsync(new byte[] { 1 }, token); // Shared client; never evict another viewer.
        var init = await ReadAsync(24, token);
        Resize(BinaryPrimitives.ReadUInt16BigEndian(init), BinaryPrimitives.ReadUInt16BigEndian(init.AsSpan(2)));
        var nameLength = BinaryPrimitives.ReadUInt32BigEndian(init.AsSpan(20));
        if (nameLength > 65536)
        {
            System.Diagnostics.Trace.TraceError($"QemuFramebuffer.ConnectAsync: desktop name length {nameLength} is too long");
            throw new IOException("QEMU's framebuffer name is too long.");
        }
        await ReadAsync((int)nameLength, token);
        // Request B G R X, or ask QEMU to swap red and blue for a guest with reversed scanout channels.
        byte redShift = swapRedAndBlue ? (byte)0 : (byte)16;
        byte blueShift = swapRedAndBlue ? (byte)16 : (byte)0;
        await _stream.WriteAsync(new byte[] { 0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, redShift, 8, blueShift, 0, 0, 0 }, token);
        var encodings = new byte[audio == null ? 12 : 16];
        encodings[0] = 2;
        BinaryPrimitives.WriteUInt16BigEndian(encodings.AsSpan(2), (ushort)(audio == null ? 2 : 3));
        BinaryPrimitives.WriteInt32BigEndian(encodings.AsSpan(4), 0); // Raw.
        BinaryPrimitives.WriteInt32BigEndian(encodings.AsSpan(8), -223); // DesktopSize.
        if (audio != null) BinaryPrimitives.WriteInt32BigEndian(encodings.AsSpan(12), AudioEncoding);
        await _stream.WriteAsync(encodings, token);
        System.Diagnostics.Trace.WriteLine($"QemuFramebuffer.ConnectAsync exit: port {port} ready, desktop {_width}x{_height}, after {watch.ElapsedMilliseconds}ms");
    }

    public async Task<byte[]> CaptureAsync(CancellationToken token)
    {
        // Captures are polled by the viewer and recordings: log failures only.
        if (_stream == null)
        {
            System.Diagnostics.Trace.TraceWarning("QemuFramebuffer.CaptureAsync: framebuffer isn't connected");
            throw new InvalidOperationException("QEMU's framebuffer isn't connected.");
        }
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(15));
        token = timeout.Token;
        var frame = await ReadFrameAsync(incremental: false, token);
        using var bitmap = new SKBitmap(new SKImageInfo(frame.Width, frame.Height, SKColorType.Bgra8888, SKAlphaType.Opaque));
        Marshal.Copy(_pixels, 0, bitmap.GetPixels(), _pixels.Length);
        using var image = SKImage.FromBitmap(bitmap);
        using var png = image.Encode(SKEncodedImageFormat.Png, 100);
        return png.ToArray();
    }

    /// <summary>One outstanding update request. The first frame and every resize request all pixels.</summary>
    internal async Task<GuestDisplayFrame> ReadFrameAsync(bool incremental, CancellationToken token)
    {
        if (_stream == null) throw new InvalidOperationException("QEMU's framebuffer isn't connected.");
        for (var attempt = 0; incremental || attempt < 4; attempt++)
        {
            var request = new byte[10];
            request[0] = 3;
            request[1] = incremental ? (byte)1 : (byte)0;
            BinaryPrimitives.WriteUInt16BigEndian(request.AsSpan(6), (ushort)_width);
            BinaryPrimitives.WriteUInt16BigEndian(request.AsSpan(8), (ushort)_height);
            await _stream.WriteAsync(request, token);
            var resized = false;
            var updated = false;
            while (true)
            {
                var message = (await ReadAsync(1, token))[0];
                if (message == 255 && audio != null)
                {
                    var extension = await ReadAsync(3, token);
                    int operation = BinaryPrimitives.ReadUInt16BigEndian(extension.AsSpan(1));
                    if (extension[0] != 1 || operation > 2) throw new IOException("QEMU sent an unsupported audio message.");
                    if (operation == 2)
                    {
                        uint size = BinaryPrimitives.ReadUInt32BigEndian(await ReadAsync(4, token));
                        if (size > 262144 || size % 4 != 0) throw new IOException("QEMU sent an invalid audio packet.");
                        if (size > 0) audio(await ReadAsync((int)size, token));
                    }
                    continue;
                }
                if (message == 2) continue; // Bell.
                if (message == 3)
                {
                    var text = await ReadAsync(7, token);
                    var length = BinaryPrimitives.ReadUInt32BigEndian(text.AsSpan(3));
                    if (length > 1048576)
                    {
                        System.Diagnostics.Trace.TraceError($"QemuFramebuffer.ReadFrameAsync: oversized cut-text message of {length} bytes");
                        throw new IOException("QEMU sent an oversized framebuffer message.");
                    }
                    await ReadAsync((int)length, token); // Never synchronize the clipboard with Windows.
                    continue;
                }
                if (message != 0)
                {
                    System.Diagnostics.Trace.TraceError($"QemuFramebuffer.ReadFrameAsync: unsupported server message type {message}");
                    throw new IOException($"Unsupported framebuffer message {message}.");
                }
                var update = await ReadAsync(3, token);
                var rectangles = BinaryPrimitives.ReadUInt16BigEndian(update.AsSpan(1));
                for (var i = 0; i < rectangles; i++)
                {
                    var header = await ReadAsync(12, token);
                    var x = BinaryPrimitives.ReadUInt16BigEndian(header);
                    var y = BinaryPrimitives.ReadUInt16BigEndian(header.AsSpan(2));
                    var width = BinaryPrimitives.ReadUInt16BigEndian(header.AsSpan(4));
                    var height = BinaryPrimitives.ReadUInt16BigEndian(header.AsSpan(6));
                    var encoding = BinaryPrimitives.ReadInt32BigEndian(header.AsSpan(8));
                    if (encoding == AudioEncoding && audio != null)
                    {
                        if (!_audioStarted)
                        {
                            // QEMU extension: signed 16-bit little-endian stereo at 48 kHz, then enable capture.
                            var format = new byte[] { 255, 1, 0, 2, 3, 2, 0, 0, 0, 0 };
                            BinaryPrimitives.WriteInt32BigEndian(format.AsSpan(6), 48000);
                            await _stream.WriteAsync(format, token);
                            await _stream.WriteAsync(new byte[] { 255, 1, 0, 0 }, token);
                            _audioStarted = true;
                        }
                        continue;
                    }
                    if (encoding == -223)
                    {
                        System.Diagnostics.Trace.WriteLine($"QemuFramebuffer.ReadFrameAsync: desktop resized from {_width}x{_height} to {width}x{height}");
                        Resize(width, height);
                        incremental = false;
                        resized = true;
                        continue;
                    }
                    if (encoding != 0 || width == 0 || height == 0 || x + width > _width || y + height > _height)
                    {
                        System.Diagnostics.Trace.TraceError($"QemuFramebuffer.ReadFrameAsync: invalid rectangle encoding={encoding} at {x},{y} size {width}x{height} on desktop {_width}x{_height}");
                        throw new IOException("QEMU sent an invalid framebuffer rectangle.");
                    }
                    if (x == 0 && width == _width)
                        await _stream.ReadExactlyAsync(_pixels.AsMemory(y * _width * 4, height * _width * 4), token);
                    else
                    {
                        // Receive a rectangle in bulk, then scatter its rows into the desktop.
                        // Reading each row from NetworkStream separately costs thousands of recv
                        // calls per frame when a browser animates inside a larger guest display.
                        var rowBytes = width * 4;
                        var length = rowBytes * height;
                        if (_rectangle.Length < length) _rectangle = new byte[length];
                        await _stream.ReadExactlyAsync(_rectangle.AsMemory(0, length), token);
                        for (var row = 0; row < height; row++)
                            _rectangle.AsSpan(row * rowBytes, rowBytes).CopyTo(_pixels.AsSpan(((y + row) * _width + x) * 4, rowBytes));
                    }
                    updated = true;
                }
                break;
            }
            if (resized || !updated) continue;
            return new GuestDisplayFrame(_width, _height, _pixels);
        }
        System.Diagnostics.Trace.TraceWarning($"QemuFramebuffer.ReadFrameAsync: no complete desktop image after 4 full requests ({_width}x{_height})");
        throw new IOException("QEMU's framebuffer did not provide a complete desktop image.");
    }

    private void Resize(int width, int height)
    {
        if (width < 1 || height < 1 || width > 8192 || height > 8192 || (long)width * height > 16777216)
        {
            System.Diagnostics.Trace.TraceError($"QemuFramebuffer.Resize: invalid framebuffer size {width}x{height}");
            throw new IOException("QEMU sent an invalid framebuffer size.");
        }
        _width = width;
        _height = height;
        _pixels = new byte[width * height * 4];
        System.Diagnostics.Trace.WriteLine($"QemuFramebuffer.Resize: desktop now {width}x{height} ({_pixels.Length} bytes)");
    }

    private async Task<byte[]> ReadAsync(int count, CancellationToken token)
    {
        var bytes = new byte[count];
        await _stream!.ReadExactlyAsync(bytes, token);
        return bytes;
    }

    public ValueTask DisposeAsync()
    {
        System.Diagnostics.Trace.WriteLine($"QemuFramebuffer.DisposeAsync: closing framebuffer connection, desktop {_width}x{_height}");
        _client.Dispose();
        return ValueTask.CompletedTask;
    }
}
