using System.Diagnostics;
using System.Runtime.InteropServices;

namespace PersonaBot;

/// <summary>
/// The Windows Hypervisor Platform (WHPX), which lets QEMU run a guest of the PC's own architecture at
/// close to native speed (DISTROS.md §1). PersonaBot only detects it and uses it; switching the Windows
/// feature on is the person's choice, so it explains how instead.
/// </summary>
public static class WindowsHypervisor
{
    public const string Name = "Windows Hypervisor Platform";

    public const string Advice =
        "For full speed on this PC's own (x86-64) machines, turn on \"Windows Hypervisor Platform\" in Turn Windows features on or off (optionalfeatures.exe), then restart Windows. "
        + "PersonaBot doesn't change Windows settings itself.";

    private static readonly Lazy<bool> Present = new(Detect);
    private static string? _failure;

    /// <summary>True when the hypervisor is running and its API answers.</summary>
    public static bool IsPresent => Present.Value;

    /// <summary>True when it is present and QEMU hasn't failed with it in this session.</summary>
    public static bool IsAvailable => Present.Value && _failure == null;

    /// <summary>Why QEMU couldn't use it this session, when it fell back to emulation; null otherwise.</summary>
    public static string? Failure => _failure;

    /// <summary>Stops using the hypervisor for the rest of this session after QEMU failed with it.</summary>
    public static void MarkFailed(string reason)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(reason);
        Trace.WriteLine($"WindowsHypervisor.MarkFailed: {reason}");
        _failure = reason;
    }

    /// <summary>
    /// WHvGetCapability(WHvCapabilityCodeHypervisorPresent), as QEMU itself checks. The DLL ships with
    /// Windows 10 1803 and later even when the feature is off; the capability says whether it is running.
    /// </summary>
    private static bool Detect()
    {
        Trace.WriteLine("WindowsHypervisor.Detect enter");
        if (!OperatingSystem.IsWindows())
        {
            Trace.WriteLine("WindowsHypervisor.Detect exit: not Windows");
            return false;
        }
        try
        {
            // WHV_CAPABILITY is a union; the HypervisorPresent member is a BOOL at its start.
            var buffer = new byte[64];
            var result = WHvGetCapability(HypervisorPresentCode, buffer, (uint)buffer.Length, out var written);
            var present = result >= 0 && written >= 4 && BitConverter.ToInt32(buffer, 0) != 0;
            Trace.WriteLine($"WindowsHypervisor.Detect exit: HRESULT 0x{result:X8}, written {written}, present {present}");
            return present;
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            Trace.WriteLine($"WindowsHypervisor.Detect exit: API unavailable ({ex.GetType().Name})");
            return false;
        }
    }

    private const int HypervisorPresentCode = 0;

    [DllImport("WinHvPlatform.dll", ExactSpelling = true)]
    private static extern int WHvGetCapability(int capabilityCode, byte[] capabilityBuffer, uint capabilityBufferSizeInBytes, out uint writtenSizeInBytes);
}

/// <summary>QEMU's own words about why it stopped: the last line of its log.</summary>
public static class QemuLog
{
    public static string LastLine(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        try
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            using var reader = new StreamReader(stream);
            var last = reader.ReadToEnd().Trim().Split('\n').LastOrDefault()?.Trim() ?? "";
            Trace.WriteLine($"QemuLog.LastLine: {path}: '{last}'");
            return last;
        }
        catch (IOException ex)
        {
            Trace.TraceWarning($"QemuLog.LastLine: couldn't read {path}: {ex.Message}");
            return "";
        }
    }
}
