using System.Diagnostics;

namespace PersonaBot;

internal static class VmImage
{
    internal static async Task<string> RunAsync(string executable, string directory, CancellationToken token, params string[] args)
    {
        var info = new ProcessStartInfo(executable) { WorkingDirectory = directory, UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true };
        foreach (var arg in args) info.ArgumentList.Add(arg);
        Trace.WriteLine($"VmImage.RunAsync enter: {Path.GetFileName(executable)} {string.Join(' ', args)} in {directory}");
        var watch = Stopwatch.StartNew();
        using var process = Process.Start(info) ?? throw new IOException("Could not start " + Path.GetFileName(executable));
        var stderr = process.StandardError.ReadToEndAsync(token);
        var stdout = process.StandardOutput.ReadToEndAsync(token);
        try { await process.WaitForExitAsync(token); }
        catch (Exception ex)
        {
            Trace.TraceWarning($"VmImage.RunAsync: {Path.GetFileName(executable)} interrupted after {watch.Elapsed.TotalSeconds:0}s ({ex.GetType().Name}); killing it");
            if (!process.HasExited) process.Kill(true);
            throw;
        }
        var output = await stdout;
        if (process.ExitCode != 0)
        {
            var errorText = await stderr;
            Trace.TraceError($"VmImage.RunAsync: {Path.GetFileName(executable)} exited with code {process.ExitCode}: {errorText.Trim()}");
            throw new IOException(errorText);
        }
        Trace.WriteLine($"VmImage.RunAsync exit: {Path.GetFileName(executable)} succeeded in {watch.Elapsed.TotalSeconds:0.0}s, {output.Length} characters of output");
        return output;
    }
}
