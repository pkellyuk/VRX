using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Serialization;
using System.Windows.Media;

namespace VRX.Desktop;

// Where a window was, in WPF units (device-independent pixels), remembered app-wide in
// app-settings.json: Expert's bounds (and whether it was maximized) and Easy's position
// (Easy sizes itself to its content). System.Text.Json writes the numbers the same way in
// every culture.
public sealed class WindowPlace
{
    public const double Limit = 100000;

    public double Left { get; set; }
    public double Top { get; set; }
    public double Width { get; set; }
    public double Height { get; set; }
    public bool Maximized { get; set; }

    [JsonIgnore]
    public bool HasPosition => double.IsFinite(Left) && double.IsFinite(Top) && Math.Abs(Left) < Limit && Math.Abs(Top) < Limit;

    [JsonIgnore]
    public bool HasSize => double.IsFinite(Width) && double.IsFinite(Height) && Width >= 1 && Height >= 1 && Width < Limit && Height < Limit;

    public WindowPlace Copy() => new() { Left = Left, Top = Top, Width = Width, Height = Height, Maximized = Maximized };

    public override string ToString() =>
        FormattableString.Invariant($"{Left:F0},{Top:F0} {Width:F0}x{Height:F0}{(Maximized ? " maximized" : "")}");
}

// Keeping the window on a monitor that exists, and the native parts of the skin.
public static class WindowPlacement
{
    [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(nint hdc, nint clip, MonitorEnumProc callback, nint data);
    [DllImport("user32.dll")] private static extern bool GetMonitorInfo(nint monitor, ref MonitorInfo info);
    [DllImport("user32.dll")] private static extern nint MonitorFromWindow(nint hwnd, uint flags);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(nint hwnd, out NativeRect rect);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(nint hwnd, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("dwmapi.dll")] private static extern int DwmSetWindowAttribute(nint hwnd, int attribute, ref int value, int size);

    private delegate bool MonitorEnumProc(nint monitor, nint hdc, ref NativeRect rect, nint data);

    [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo
    {
        public int Size;
        public NativeRect Monitor, Work;
        public uint Flags;
    }

    private const uint MonitorInfoPrimary = 1, MonitorDefaultToNearest = 2;
    private const uint SwpNoSize = 0x1, SwpNoZOrder = 0x4, SwpNoActivate = 0x10;
    private const int DwmUseImmersiveDarkMode = 20, DwmWindowCornerPreference = 33, DwmBorderColor = 34, DwmCornerRound = 2;

    // The window moved (and, if allowed, shrunk) to lie wholly inside one work area: the
    // one showing most of it, or, when none does (its monitor is gone), the nearest.
    // Physical pixels throughout, so monitors with different scaling need no conversion.
    public static ScreenRect Clamp(ScreenRect window, IReadOnlyList<ScreenRect> workAreas, bool allowResize)
    {
        ArgumentNullException.ThrowIfNull(workAreas);
        var areas = workAreas.Where(a => a.Area > 0).ToList();
        if (areas.Count == 0 || window.Width <= 0 || window.Height <= 0) return window;
        ScreenRect best = areas[0];
        long bestOverlap = 0;
        double bestDistance = double.MaxValue;
        foreach (var area in areas)
        {
            long overlap = Overlap(window, area);
            if (overlap > bestOverlap) { bestOverlap = overlap; best = area; continue; }
            if (bestOverlap > 0) continue;
            double distance = Distance(window, area);
            if (distance < bestDistance) { bestDistance = distance; best = area; }
        }
        int width = allowResize ? Math.Min(window.Width, best.Width) : window.Width;
        int height = allowResize ? Math.Min(window.Height, best.Height) : window.Height;
        // Too big to fit (only without resizing): keep the top-left corner, with the title
        // bar, on screen.
        int left = Math.Max(best.Left, Math.Min(window.Left, best.Right - width));
        int top = Math.Max(best.Top, Math.Min(window.Top, best.Bottom - height));
        return new ScreenRect(left, top, left + width, top + height);
    }

    private static long Overlap(ScreenRect a, ScreenRect b)
    {
        long width = Math.Min(a.Right, b.Right) - Math.Max(a.Left, b.Left);
        long height = Math.Min(a.Bottom, b.Bottom) - Math.Max(a.Top, b.Top);
        return width > 0 && height > 0 ? width * height : 0;
    }

    // Distance between centres.
    private static double Distance(ScreenRect a, ScreenRect b)
    {
        double dx = (a.Left + a.Right) / 2.0 - (b.Left + b.Right) / 2.0;
        double dy = (a.Top + a.Bottom) / 2.0 - (b.Top + b.Bottom) / 2.0;
        return Math.Sqrt(dx * dx + dy * dy);
    }

    // Every monitor's work area (without the taskbar), the primary monitor first.
    public static IReadOnlyList<ScreenRect> WorkAreas()
    {
        var primary = new List<ScreenRect>();
        var others = new List<ScreenRect>();
        bool Collect(nint monitor, nint hdc, ref NativeRect rect, nint data)
        {
            var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
            if (!GetMonitorInfo(monitor, ref info)) return true;
            var work = new ScreenRect(info.Work.Left, info.Work.Top, info.Work.Right, info.Work.Bottom);
            ((info.Flags & MonitorInfoPrimary) != 0 ? primary : others).Add(work);
            return true;
        }
        EnumDisplayMonitors(0, 0, Collect, 0);
        var all = primary.Concat(others).ToList();
        Debug.WriteLine($"[Placement] WorkAreas: {string.Join("; ", all)}");
        return all;
    }

    // The work area of the monitor the window is on.
    public static ScreenRect WorkAreaOf(nint hwnd)
    {
        if (hwnd == 0) return default;
        var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        nint monitor = MonitorFromWindow(hwnd, MonitorDefaultToNearest);
        if (monitor == 0 || !GetMonitorInfo(monitor, ref info)) return default;
        return new ScreenRect(info.Work.Left, info.Work.Top, info.Work.Right, info.Work.Bottom);
    }

    public static bool TryGetBounds(nint hwnd, out ScreenRect bounds)
    {
        bounds = default;
        if (hwnd == 0 || !GetWindowRect(hwnd, out var rect)) return false;
        bounds = new ScreenRect(rect.Left, rect.Top, rect.Right, rect.Bottom);
        return true;
    }

    // Moves the window back onto a monitor that exists. Easy's height follows its content,
    // so it is only moved; Expert may also be shrunk to fit.
    public static void KeepOnScreen(nint hwnd, bool allowResize)
    {
        Debug.WriteLine($"[Placement] KeepOnScreen enter: {hwnd:X}, resize {allowResize}");
        if (!TryGetBounds(hwnd, out var bounds)) return;
        var clamped = Clamp(bounds, WorkAreas(), allowResize);
        if (clamped == bounds)
        {
            Debug.WriteLine($"[Placement] KeepOnScreen exit: {bounds} is on screen");
            return;
        }
        uint flags = SwpNoZOrder | SwpNoActivate | (allowResize ? 0 : SwpNoSize);
        SetWindowPos(hwnd, 0, clamped.Left, clamped.Top, clamped.Width, clamped.Height, flags);
        Debug.WriteLine($"[Placement] KeepOnScreen exit: moved {bounds} to {clamped}");
    }

    // Windows 11: rounded corners, a dark frame and a border in the skin's edge colour.
    // True when Windows draws the rounded corners and the border (Windows 11), so the window
    // draws no border of its own; false on older Windows.
    public static bool ApplyDwmSkin(nint hwnd, Color edge)
    {
        Debug.WriteLine($"[Placement] ApplyDwmSkin enter: {hwnd:X}, edge {edge}");
        if (hwnd == 0) return false;
        int dark = 1;
        int darkResult = DwmSetWindowAttribute(hwnd, DwmUseImmersiveDarkMode, ref dark, sizeof(int));
        int round = DwmCornerRound;
        int roundResult = DwmSetWindowAttribute(hwnd, DwmWindowCornerPreference, ref round, sizeof(int));
        int colour = edge.R | (edge.G << 8) | (edge.B << 16);        // COLORREF: 0x00BBGGRR
        int borderResult = roundResult == 0 ? DwmSetWindowAttribute(hwnd, DwmBorderColor, ref colour, sizeof(int)) : -1;
        bool windows11 = roundResult == 0 && borderResult == 0;
        Debug.WriteLine($"[Placement] ApplyDwmSkin exit: dark {darkResult:X}, corners {roundResult:X}, border {borderResult:X}, Windows 11 {windows11}");
        return windows11;
    }
}
