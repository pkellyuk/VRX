using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// One-shot capture of the Windows desktop for computer use.
///
/// Deliberately not <see cref="ScreenViewService"/>: that one is a 15 fps
/// streaming service for remote screen view, captures the primary monitor only
/// through <c>CreateDC("DISPLAY")</c>, and compiles to a no-op stub outside the
/// <c>WINDOWS</c> define. Grid descent needs a single frame covering the whole
/// virtual desktop, at native resolution, whenever it asks — and it needs to work
/// from any host that references Core, so the platform check is at runtime.
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class DesktopCaptureService : IComputerScreen
{
    private const int SM_XVIRTUALSCREEN = 76;
    private const int SM_YVIRTUALSCREEN = 77;
    private const int SM_CXVIRTUALSCREEN = 78;
    private const int SM_CYVIRTUALSCREEN = 79;

    private const int MONITORINFOF_PRIMARY = 1;

    /// <summary>DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.</summary>
    private static readonly IntPtr PerMonitorAwareV2 = new(-4);

    private static bool _dpiAwarenessAttempted;

    /// <summary>
    /// Makes the process per-monitor DPI aware, once.
    ///
    /// Without this, Windows virtualises coordinates for the process: the captured
    /// bitmap comes back at a scaled size and <c>SetCursorPos</c> lands somewhere
    /// other than where the capture said. Every pixel in the descent is then wrong
    /// by the display scale factor. Avalonia already sets this for the desktop app,
    /// and the call is harmless when awareness is already set (it simply fails).
    /// </summary>
    public static void EnsureDpiAwareness()
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[DesktopCaptureService] EnsureDpiAwareness: Entry");

        if (_dpiAwarenessAttempted)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[DesktopCaptureService] EnsureDpiAwareness: Exit - already attempted");
            return;
        }

        _dpiAwarenessAttempted = true;

        if (!OperatingSystem.IsWindows())
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[DesktopCaptureService] EnsureDpiAwareness: Exit - not Windows");
            return;
        }

        try
        {
            var applied = SetProcessDpiAwarenessContext(PerMonitorAwareV2);
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopCaptureService] EnsureDpiAwareness: Exit - SetProcessDpiAwarenessContext(PerMonitorV2) returned {applied} (false usually means awareness was already set by the host)");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"[DesktopCaptureService] EnsureDpiAwareness: SetProcessDpiAwarenessContext threw, continuing without it: {ex.GetType().Name}: {ex.Message}");
        }
    }

    /// <summary>
    /// The full virtual desktop rect. The origin is negative when a monitor sits
    /// above or to the left of the primary one, which is why nothing in this stack
    /// may assume screen space starts at 0,0.
    /// </summary>
    public ScreenRect GetVirtualDesktopBounds()
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[DesktopCaptureService] GetVirtualDesktopBounds: Entry");

        EnsureDpiAwareness();

        var bounds = new ScreenRect(
            GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN));

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopCaptureService] GetVirtualDesktopBounds: Exit - bounds={bounds}");
        return bounds;
    }

    /// <summary>
    /// Enumerates the attached displays, in the order Windows reports them, with a
    /// 1-based index the model can name.
    /// </summary>
    public IReadOnlyList<MonitorInfo> GetMonitors()
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[DesktopCaptureService] GetMonitors: Entry");

        EnsureDpiAwareness();

        var monitors = new List<MonitorInfo>();

        var enumerated = EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr hMonitor, IntPtr hdc, ref RECT rect, IntPtr data) =>
        {
            var info = new MONITORINFOEX
            {
                cbSize = Marshal.SizeOf<MONITORINFOEX>()
            };

            if (!GetMonitorInfo(hMonitor, ref info))
            {
                AutomatonDebug.LogWarning($"[DesktopCaptureService] GetMonitors: GetMonitorInfo failed for handle 0x{hMonitor:X}, skipping this display");
                return true;
            }

            monitors.Add(new MonitorInfo
            {
                Index = monitors.Count + 1,
                DeviceName = info.szDevice ?? string.Empty,
                Bounds = ToScreenRect(info.rcMonitor),
                WorkArea = ToScreenRect(info.rcWork),
                IsPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0
            });

            return true;
        }, IntPtr.Zero);

        if (!enumerated)
        {
            AutomatonDebug.LogError("[DesktopCaptureService] GetMonitors: EnumDisplayMonitors failed");
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopCaptureService] GetMonitors: Exit - found {monitors.Count} monitor(s): {string.Join(", ", monitors.Select(m => $"{m.Index}:{m.DeviceName}@{m.Bounds}{(m.IsPrimary ? " primary" : string.Empty)}"))}");
        return monitors;
    }

    /// <summary>
    /// Captures a screen region at native resolution.
    ///
    /// The caller owns the returned bitmap. Capturing only the region asked for
    /// rather than the whole desktop every time keeps a deep zoom cheap: the last
    /// step of a descent copies a few thousand pixels, not eight million.
    /// </summary>
    public Bitmap CaptureRegion(ScreenRect region)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopCaptureService] CaptureRegion: Entry - region={region}");

        if (region.IsEmpty)
        {
            throw new ArgumentException($"Capture region must have a positive size, got {region}.", nameof(region));
        }

        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException("Desktop capture is only implemented on Windows.");
        }

        EnsureDpiAwareness();

        var bitmap = new Bitmap(region.Width, region.Height, PixelFormat.Format32bppArgb);

        try
        {
            using var graphics = Graphics.FromImage(bitmap);
            graphics.CopyFromScreen(
                region.X,
                region.Y,
                0,
                0,
                new Size(region.Width, region.Height),
                CopyPixelOperation.SourceCopy);
        }
        catch (Exception ex)
        {
            bitmap.Dispose();
            AutomatonDebug.LogError($"[DesktopCaptureService] CaptureRegion: CopyFromScreen failed for region={region}: {ex.GetType().Name}: {ex.Message}");
            throw;
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopCaptureService] CaptureRegion: Exit - captured {bitmap.Width}x{bitmap.Height}");
        return bitmap;
    }

    /// <summary>
    /// Captures the whole virtual desktop, all monitors included.
    /// </summary>
    public Bitmap CaptureVirtualDesktop()
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[DesktopCaptureService] CaptureVirtualDesktop: Entry");

        var bitmap = CaptureRegion(GetVirtualDesktopBounds());

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[DesktopCaptureService] CaptureVirtualDesktop: Exit - {bitmap.Width}x{bitmap.Height}");
        return bitmap;
    }

    /// <summary>
    /// Computer use's view of the same capture, in Skia's layout. The GDI+ methods above stay for
    /// the Windows-only callers that draw on the bitmap directly.
    /// </summary>
    SkiaSharp.SKBitmap IComputerScreen.CaptureRegion(ScreenRect region)
    {
        using var bitmap = CaptureRegion(region);
        return SkiaImages.FromGdi(bitmap);
    }

    SkiaSharp.SKBitmap IComputerScreen.CaptureVirtualDesktop()
    {
        using var bitmap = CaptureVirtualDesktop();
        return SkiaImages.FromGdi(bitmap);
    }

    private static ScreenRect ToScreenRect(RECT rect)
    {
        return new ScreenRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }

    private delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdcMonitor, ref RECT lprcMonitor, IntPtr dwData);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT
    {
        public int left;
        public int top;
        public int right;
        public int bottom;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MONITORINFOEX
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public int dwFlags;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string szDevice;
    }

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int nIndex);

    [DllImport("user32.dll")]
    private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr lprcClip, MonitorEnumProc lpfnEnum, IntPtr dwData);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetMonitorInfoW")]
    private static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFOEX lpmi);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetProcessDpiAwarenessContext(IntPtr value);
}
