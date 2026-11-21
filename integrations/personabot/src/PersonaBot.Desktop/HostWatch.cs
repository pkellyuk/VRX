using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;

namespace PersonaBot;

/// <summary>
/// While a machine runs, samples the Windows side every <see cref="Interval"/> into its diary: QEMU's memory, CPU,
/// threads and handles, the PC's free memory, the graphics card (through nvidia-smi when it's there), and how many
/// QEMUs and PersonaBots are running. A guest that dies of memory trouble or a GPU hiccup leaves the host's view of
/// the minutes before in the diary and the crash report. The last samples are kept for the report.
/// </summary>
public sealed class HostWatch : IDisposable
{
    public static readonly TimeSpan Interval = TimeSpan.FromSeconds(30);
    private const int Kept = 20;

    private readonly Process _qemu;
    private readonly string _folder;
    private readonly string _machineId;
    private readonly CancellationTokenSource _stop = new();
    private readonly Queue<string> _recent = new();
    private readonly object _gate = new();
    private TimeSpan _lastCpu;
    private DateTime _lastAt;
    private int _samples;
    private static string? _nvidiaSmi;
    private static bool _nvidiaLooked;

    private HostWatch(Process qemu, string folder, string machineId)
    {
        _qemu = qemu;
        _folder = folder;
        _machineId = machineId;
    }

    /// <summary>Starts sampling <paramref name="qemu"/> for the machine in <paramref name="folder"/>; dispose to stop.</summary>
    public static HostWatch Start(Process qemu, string folder, string machineId)
    {
        ArgumentNullException.ThrowIfNull(qemu);
        ArgumentException.ThrowIfNullOrWhiteSpace(folder);
        ArgumentException.ThrowIfNullOrWhiteSpace(machineId);
        Trace.WriteLine($"HostWatch.Start: {machineId}, QEMU pid {qemu.Id}, every {Interval.TotalSeconds:0}s");
        var watch = new HostWatch(qemu, folder, machineId);
        _ = Task.Run(watch.RunAsync);
        return watch;
    }

    /// <summary>The latest samples, oldest first.</summary>
    public IReadOnlyList<string> Recent
    {
        get { lock (_gate) return _recent.ToArray(); }
    }

    private async Task RunAsync()
    {
        Trace.WriteLine($"HostWatch.RunAsync enter: {_machineId}");
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                var line = Sample();
                if (line == null) break;
                lock (_gate)
                {
                    _recent.Enqueue(DateTimeOffset.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture) + " " + line);
                    while (_recent.Count > Kept) _recent.Dequeue();
                }
                MachineDiary.Write(_folder, _machineId, "host", line);
                await Task.Delay(Interval, _stop.Token);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            Trace.TraceWarning($"HostWatch.RunAsync: {_machineId} sampling stopped: {ex.GetType().Name}: {ex.Message}");
        }
        Trace.WriteLine($"HostWatch.RunAsync exit: {_machineId} after {_samples} samples");
    }

    /// <summary>One line describing the host now, or null once QEMU has gone.</summary>
    private string? Sample()
    {
        try
        {
            _qemu.Refresh();
            if (_qemu.HasExited) return null;
            var now = DateTime.UtcNow;
            var cpu = _qemu.TotalProcessorTime;
            var cpuText = "";
            if (_samples > 0 && now > _lastAt)
            {
                var percent = (cpu - _lastCpu).TotalMilliseconds / (now - _lastAt).TotalMilliseconds / Environment.ProcessorCount * 100;
                cpuText = $", CPU {percent:0}% of the PC";
            }
            _lastCpu = cpu;
            _lastAt = now;
            _samples++;
            var qemu = $"QEMU {_qemu.WorkingSet64 / 1048576} MB in use (peak {_qemu.PeakWorkingSet64 / 1048576}), {_qemu.PrivateMemorySize64 / 1048576} MB private{cpuText}, {_qemu.Threads.Count} threads, {_qemu.HandleCount} handles";
            var memory = HostMemory() is { } m ? $"; PC {m.AvailableMb} MB free of {m.TotalMb} MB ({m.LoadPercent}% used), commit {m.CommitUsedMb}/{m.CommitLimitMb} MB" : "";
            var gpu = Gpu() is { Length: > 0 } g ? "; GPU " + g : "";
            var others = $"; {Count("qemu-system-x86_64") + Count("qemu-system-aarch64")} QEMU(s), {Count("PersonaBot")} PersonaBot(s) running";
            return qemu + memory + gpu + others;
        }
        catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            Trace.WriteLine($"HostWatch.Sample: {_machineId}: {ex.Message}");
            return null;
        }
    }

    private static int Count(string name)
    {
        var processes = Process.GetProcessesByName(name);
        foreach (var p in processes) p.Dispose();
        return processes.Length;
    }

    /// <summary>The graphics card's memory, load, temperature, power and state from nvidia-smi, or null without one.</summary>
    public static string? Gpu()
    {
        if (!_nvidiaLooked)
        {
            _nvidiaLooked = true;
            var candidate = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "nvidia-smi.exe");
            _nvidiaSmi = OperatingSystem.IsWindows() && File.Exists(candidate) ? candidate : null;
            Trace.WriteLine($"HostWatch.Gpu: nvidia-smi {(_nvidiaSmi ?? "not found; no GPU samples")}");
        }
        if (_nvidiaSmi == null) return null;
        try
        {
            var info = new ProcessStartInfo(_nvidiaSmi, "--query-gpu=name,memory.used,memory.total,utilization.gpu,temperature.gpu,power.draw,pstate --format=csv,noheader")
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            using var process = Process.Start(info);
            if (process == null) return null;
            var output = process.StandardOutput.ReadToEndAsync();
            if (!process.WaitForExit(5000))
            {
                try { process.Kill(); } catch (InvalidOperationException) { }
                return "nvidia-smi didn't answer within 5 s";
            }
            return output.Result.Trim().ReplaceLineEndings("; ");
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or IOException or InvalidOperationException)
        {
            return "nvidia-smi failed: " + ex.Message;
        }
    }

    public readonly record struct Memory(long AvailableMb, long TotalMb, int LoadPercent, long CommitUsedMb, long CommitLimitMb);

    /// <summary>The PC's physical memory and commit charge, or null off Windows.</summary>
    public static Memory? HostMemory()
    {
        if (!OperatingSystem.IsWindows()) return null;
        var status = new MemoryStatusEx { Length = (uint)Marshal.SizeOf<MemoryStatusEx>() };
        if (!GlobalMemoryStatusEx(ref status)) return null;
        const long Mb = 1048576;
        return new Memory((long)(status.AvailPhys / Mb), (long)(status.TotalPhys / Mb), (int)status.MemoryLoad,
            (long)((status.TotalPageFile - status.AvailPageFile) / Mb), (long)(status.TotalPageFile / Mb));
    }

    public void Dispose()
    {
        Trace.WriteLine($"HostWatch.Dispose: {_machineId}");
        _stop.Cancel();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatusEx
    {
        public uint Length;
        public uint MemoryLoad;
        public ulong TotalPhys;
        public ulong AvailPhys;
        public ulong TotalPageFile;
        public ulong AvailPageFile;
        public ulong TotalVirtual;
        public ulong AvailVirtual;
        public ulong AvailExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx buffer);
}
