using System.ComponentModel;
using System.Diagnostics;
using System.Text.Json;
using SkiaSharp;

namespace PersonaBot;

public sealed record QemuGraphicsSupport(bool Available, string? Reason);

/// <summary>
/// A shared GPU, with no PCI assignment or host window. egl-headless copies GL scanout back to
/// QEMU's local framebuffer; the live viewer uses RFB because QMP cannot dump GL scanout.
/// </summary>
public static class QemuGraphics
{
    public const string Device = "virtio-gpu-gl-pci";
    public const string Display = "egl-headless,gl=on";

    public static IReadOnlyList<string> Arguments(int width, int height, bool shared) =>
        ["-device", $"{(shared ? Device : "virtio-gpu-pci")},xres={width},yres={height}",
         "-display", shared ? Display : "none"];

    internal static void ConfigureEnvironment(ProcessStartInfo info, bool shared)
    {
        // ANGLE's native GL backend presents the complete X11 desktop on Windows. Its D3D
        // backend can render GL clients while losing the desktop's 2D scanout updates.
        if (OperatingSystem.IsWindows() && shared)
        {
            Trace.WriteLine($"QemuGraphics.ConfigureEnvironment: shared GPU for {Path.GetFileName(info.FileName)}, ANGLE_DEFAULT_PLATFORM=gl");
            info.Environment["ANGLE_DEFAULT_PLATFORM"] = "gl";
        }
    }

    /// <summary>
    /// Test the actual renderer and PNG readback on a paused, diskless VM. A device in -device help
    /// alone does not establish that its DLLs, EGL implementation and host driver can initialize.
    /// The probe never boots guest code or opens a network listener.
    /// </summary>
    public static Task<QemuGraphicsSupport> ProbeAsync(string qemuPath, string architecture, CancellationToken token)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(qemuPath);
        Trace.WriteLine($"QemuGraphics.ProbeAsync: probing {qemuPath} for {architecture}, limit 15s");
        return ProbeAsync(new ProcessStartInfo(qemuPath), architecture, TimeSpan.FromSeconds(15), token);
    }

    internal static async Task<QemuGraphicsSupport> ProbeAsync(ProcessStartInfo startInfo, string architecture, TimeSpan limit, CancellationToken token)
    {
        if (architecture is not ("x86_64" or "aarch64"))
        {
            Trace.TraceWarning($"QemuGraphics.ProbeAsync: unsupported architecture '{architecture}'");
            throw new ArgumentException("Unsupported QEMU architecture.", nameof(architecture));
        }
        token.ThrowIfCancellationRequested();
        Trace.WriteLine($"QemuGraphics.ProbeAsync enter: {startInfo.FileName}, {architecture}, limit {limit.TotalSeconds}s");
        var watch = Stopwatch.StartNew();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(limit);
        var folder = Path.Combine(Path.GetTempPath(), "personabot-gpu-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        var image = Path.Combine(folder, "probe.png");
        startInfo.UseShellExecute = false;
        startInfo.CreateNoWindow = true;
        startInfo.RedirectStandardInput = true;
        startInfo.RedirectStandardOutput = true;
        startInfo.RedirectStandardError = true;
        using var process = new Process { StartInfo = startInfo };
        ConfigureEnvironment(startInfo, shared: true);
        Task<string>? errors = null;
        var started = false;
        try
        {
            foreach (var arg in new[] { "-machine", architecture == "aarch64" ? "virt" : "q35",
                "-accel", "tcg", "-m", "128", "-nodefaults", "-S", "-monitor", "none",
                "-serial", "none", "-nic", "none", "-qmp", "stdio" }.Concat(Arguments(640, 480, true)))
                process.StartInfo.ArgumentList.Add(arg);
            started = process.Start();
            Trace.WriteLine($"QemuGraphics.ProbeAsync: probe QEMU started={started}, pid {(started ? process.Id : 0)}");
            errors = process.StandardError.ReadToEndAsync();
            var greeting = await process.StandardOutput.ReadLineAsync(timeout.Token);
            if (greeting == null) throw new IOException("QEMU exited before the graphics probe was ready.");
            using (var doc = JsonDocument.Parse(greeting))
                if (!doc.RootElement.TryGetProperty("QMP", out _)) throw new IOException("QEMU did not send a QMP greeting.");
            Trace.WriteLine($"QemuGraphics.ProbeAsync: QMP greeting after {watch.ElapsedMilliseconds}ms");
            await CommandAsync(process, "qmp_capabilities", new { }, timeout.Token);
            await CommandAsync(process, "screendump", new { filename = image, format = "png" }, timeout.Token);
            var png = await File.ReadAllBytesAsync(image, timeout.Token);
            var probeSize = WindowsPlatform.PngSize(png);
            Trace.WriteLine($"QemuGraphics.ProbeAsync: readback {probeSize.Width}x{probeSize.Height}, {png.Length} bytes");
            if (probeSize != (640, 480)) throw new IOException("GPU framebuffer readback returned the wrong size.");
            await CommandAsync(process, "quit", new { }, timeout.Token);
            await process.WaitForExitAsync(timeout.Token);
            if (process.ExitCode != 0)
            {
                Trace.TraceWarning($"QemuGraphics.ProbeAsync: probe QEMU exited with code {process.ExitCode}");
                throw new IOException("QEMU's graphics probe failed.");
            }
            Trace.WriteLine($"QemuGraphics.ProbeAsync exit: shared GPU available ({architecture}) after {watch.ElapsedMilliseconds}ms");
            return new(true, null);
        }
        catch (Exception ex) when (ex is IOException or Win32Exception or JsonException or OperationCanceledException)
        {
            if (started) await StopAsync(process);
            token.ThrowIfCancellationRequested();
            var detail = errors == null ? "" : (await errors).Trim();
            var reason = ex is OperationCanceledException ? "GPU initialization timed out. " + detail
                : detail.Length > 0 ? detail : ex.Message;
            // Keep the UI readable; the trace retains QEMU's complete diagnostic.
            Trace.TraceWarning($"QemuGraphics.ProbeAsync exit: shared GPU unavailable ({ex.GetType().Name}) after {watch.ElapsedMilliseconds}ms: {reason}");
            return new(false, reason.Length > 500 ? reason[..500] + "…" : reason);
        }
        finally
        {
            if (started) await StopAsync(process);
            if (errors != null) await errors;
            try { Directory.Delete(folder, recursive: true); }
            catch (IOException ex) { Trace.WriteLine($"QemuGraphics: couldn't remove probe files: {ex.Message}"); }
            catch (UnauthorizedAccessException ex) { Trace.WriteLine($"QemuGraphics: couldn't remove probe files: {ex.Message}"); }
        }
    }

    private static async Task StopAsync(Process process)
    {
        if (!process.HasExited)
        {
            Trace.WriteLine($"QemuGraphics.StopAsync: killing probe QEMU pid {process.Id}");
            try { process.Kill(entireProcessTree: true); }
            catch (InvalidOperationException) when (process.HasExited)
            {
                // The process can finish between HasExited and Kill.
            }
        }
        await process.WaitForExitAsync(CancellationToken.None);
    }

    private static async Task CommandAsync(Process process, string operation, object arguments, CancellationToken token)
    {
        Trace.WriteLine($"QemuGraphics.CommandAsync enter: {operation}");
        await process.StandardInput.WriteLineAsync(JsonSerializer.Serialize(new { execute = operation, arguments, id = operation }).AsMemory(), token);
        await process.StandardInput.FlushAsync(token);
        while (true)
        {
            var line = await process.StandardOutput.ReadLineAsync(token);
            if (line is null)
            {
                Trace.TraceWarning($"QemuGraphics.CommandAsync: QEMU disconnected during '{operation}'");
                throw new IOException("QEMU disconnected during the graphics probe.");
            }
            using var doc = JsonDocument.Parse(line);
            var root = doc.RootElement;
            if (!root.TryGetProperty("id", out var id) || id.GetString() != operation) continue;
            if (root.TryGetProperty("error", out var error))
            {
                Trace.TraceWarning($"QemuGraphics.CommandAsync: '{operation}' failed: {error.GetRawText()}");
                throw new IOException(error.GetRawText());
            }
            if (!root.TryGetProperty("return", out _))
            {
                Trace.TraceWarning($"QemuGraphics.CommandAsync: '{operation}' returned no 'return' member");
                throw new IOException("QEMU returned an invalid probe response.");
            }
            Trace.WriteLine($"QemuGraphics.CommandAsync exit: {operation} ok");
            return;
        }
    }

    public static bool IsHardwareVirgl(string output)
    {
        var virgl = output.Contains("OpenGL renderer string: virgl", StringComparison.OrdinalIgnoreCase);
        var direct = output.Contains("direct rendering: Yes", StringComparison.OrdinalIgnoreCase);
        var software = new[] { "llvmpipe", "softpipe", "swrast", "SwiftShader", "GDI Generic", "software rasterizer", "Microsoft Basic Render", "WARP" }
            .FirstOrDefault(name => output.Contains(name, StringComparison.OrdinalIgnoreCase));
        var hardware = virgl && direct && software == null;
        Trace.WriteLine($"QemuGraphics.IsHardwareVirgl: {hardware} (virgl={virgl}, direct={direct}, software renderer={software ?? "none"})");
        return hardware;
    }

    /// <summary>At startup Firefox and the window manager must produce more than an empty scanout.</summary>
    internal static bool HasDesktopContent(byte[] png)
    {
        using var bitmap = SKBitmap.Decode(png);
        if (bitmap == null || bitmap.Width < 1 || bitmap.Height < 1) return false;
        var background = bitmap.GetPixel(0, 0);
        for (var y = 0; y < bitmap.Height; y += Math.Max(1, bitmap.Height / 64))
            for (var x = 0; x < bitmap.Width; x += Math.Max(1, bitmap.Width / 64))
                if (bitmap.GetPixel(x, y) != background) return true;
        return false;
    }
}
