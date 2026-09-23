using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;

namespace Vrx.Linux;

// Where the Linux engine, its model and the repository are.
public sealed record EnginePaths(string Root, string Engine, string Model)
{
    public const string ModelFile = "zipdepth_faithful_fp16_672x384.onnx";

    // VRX_LINUX_ENGINE overrides the engine; otherwise the repository is found
    // above this program or the working directory, and its release build is
    // preferred over the debug build.
    public static EnginePaths Find()
    {
        string? root = null;
        foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
        {
            for (var directory = new DirectoryInfo(start); directory != null; directory = directory.Parent)
                if (File.Exists(Path.Combine(directory.FullName, "linux", "CMakeLists.txt")))
                {
                    root = directory.FullName;
                    break;
                }
            if (root != null) break;
        }
        root ??= Environment.CurrentDirectory;
        var engine = Environment.GetEnvironmentVariable("VRX_LINUX_ENGINE");
        if (string.IsNullOrEmpty(engine))
        {
            var release = Path.Combine(root, "build", "linux-release", "vrx-xr-synthetic");
            engine = File.Exists(release) ? release : Path.Combine(root, "build", "linux", "vrx-xr-synthetic");
        }
        return new EnginePaths(root, Path.GetFullPath(engine), Path.Combine(root, "bench", "models", ModelFile));
    }
}

// Live settings as the engine reads them (live_settings.h): one atomic
// "VRXL 2" line, culture-independent.
public static class LiveSettings
{
    public static string Snapshot(LinuxProfile profile)
    {
        var p = profile.Normalized();
        return string.Create(CultureInfo.InvariantCulture,
            $"VRXL 2 {p.Width:F3} {p.Distance:F3} {p.Height:F3} {p.Horizontal:F3} {p.Strength:F3} " +
            $"{p.Room} {p.Glass} {p.Reflect} {p.Light} {p.LightRgb}\n");
    }

    public static void Write(string path, LinuxProfile profile)
    {
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, Snapshot(profile));
        File.Move(temporary, path, overwrite: true);
    }
}

// One run of the engine: started with the live settings file, stopped with
// SIGTERM (the engine's clean shutdown) and killed if it does not stop.
public sealed class EngineSession : IDisposable
{
    readonly Process process;
    public event Action<string>? Output;
    public event Action<int>? Exited;

    public EngineSession(EnginePaths paths, string settingsPath, bool cuda, string source = "--source=any")
    {
        var start = new ProcessStartInfo(paths.Engine)
        {
            WorkingDirectory = paths.Root,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (var argument in Arguments(paths, settingsPath, cuda, source)) start.ArgumentList.Add(argument);
        process = new Process { StartInfo = start, EnableRaisingEvents = true };
        process.OutputDataReceived += (_, e) => { if (e.Data != null) Output?.Invoke(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data != null) Output?.Invoke(e.Data); };
        process.Exited += (_, _) =>
        {
            process.WaitForExit();   // drains the output readers
            Exited?.Invoke(process.ExitCode);
        };
    }

    // source: --source=window, --source=screen or --source=any (what the desktop chooser offers).
    public static IReadOnlyList<string> Arguments(EnginePaths paths, string settingsPath, bool cuda,
                                                  string source = "--source=any")
    {
        var arguments = new List<string> { "--until-stop", "--live", "--room", source, $"--settings={settingsPath}" };
        if (cuda) arguments.AddRange(new[] { "--cuda", $"--model={paths.Model}" });
        return arguments;
    }

    public string CommandLine => string.Join(' ', new[] { process.StartInfo.FileName }.Concat(process.StartInfo.ArgumentList));
    public bool Running { get; private set; }

    public void Start()
    {
        process.Start();
        Running = true;
        process.Exited += (_, _) => Running = false;
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
    }

    [DllImport("libc", SetLastError = true)]
    static extern int kill(int pid, int signal);
    const int SIGTERM = 15;

    // Asks the engine to stop cleanly; returns false if it had already exited.
    public bool RequestStop()
    {
        if (!Running || process.HasExited) return false;
        return kill(process.Id, SIGTERM) == 0;
    }

    public void ForceStop()
    {
        if (Running && !process.HasExited) process.Kill();
    }

    public bool WaitForExit(TimeSpan timeout) => process.WaitForExit(timeout);

    public void Dispose() => process.Dispose();
}
