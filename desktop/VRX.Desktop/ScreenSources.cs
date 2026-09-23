using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace VRX.Desktop;

// One display, in the engine's order: --monitor=N counts EnumDisplayMonitors from 0, and so
// does Index here.
public sealed record ScreenSource(int Index, nint Handle, string Device, bool Primary, int PixelWidth, int PixelHeight,
    int Left, int Top, int Width, int Height)
{
    public string Title => Loc.Format(Primary ? "ScreenTitlePrimary" : "ScreenTitle", Index + 1, PixelWidth, PixelHeight);
}

// "Whole screen": every display as something to play, like a game. It is one entry in the
// game list (an executable-less RunningApp whose windows are the displays) with one set of
// settings shared by all displays, so everything that works for games works for it.
public static class ScreenSources
{
    private delegate bool MonitorEnumProc(nint monitor, nint hdc, nint rect, nint data);
    [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(nint hdc, nint clip, MonitorEnumProc callback, nint data);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] private struct MonitorInfoEx
    {
        public int Size; public RunningApps.Rect Monitor, Work; public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Device;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool GetMonitorInfo(nint monitor, ref MonitorInfoEx info);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] private struct DevMode
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        public short SpecVersion, DriverVersion, Size, DriverExtra; public int Fields;
        public int PositionX, PositionY, DisplayOrientation, DisplayFixedOutput;
        public short Color, Duplex, YResolution, TTOption, Collate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
        public short LogPixels; public int BitsPerPel, PelsWidth, PelsHeight, DisplayFlags, DisplayFrequency;
        public int IcmMethod, IcmIntent, MediaType, DitherType, Reserved1, Reserved2, PanningWidth, PanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool EnumDisplaySettings(string device, int mode, ref DevMode devMode);
    [DllImport("user32.dll")] private static extern nint GetDC(nint hwnd);
    [DllImport("user32.dll")] private static extern int ReleaseDC(nint hwnd, nint hdc);
    [DllImport("gdi32.dll")] private static extern nint CreateCompatibleDC(nint hdc);
    [DllImport("gdi32.dll")] private static extern nint CreateCompatibleBitmap(nint hdc, int width, int height);
    [DllImport("gdi32.dll")] private static extern nint SelectObject(nint hdc, nint obj);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(nint obj);
    [DllImport("gdi32.dll")] private static extern bool DeleteDC(nint hdc);
    [DllImport("gdi32.dll")] private static extern int SetStretchBltMode(nint hdc, int mode);
    [DllImport("gdi32.dll")] private static extern bool SetBrushOrgEx(nint hdc, int x, int y, nint previous);
    [DllImport("gdi32.dll")] private static extern bool StretchBlt(nint dest, int x, int y, int width, int height,
        nint source, int sx, int sy, int sourceWidth, int sourceHeight, uint rop);

    private const int CurrentSettings = -1;          // ENUM_CURRENT_SETTINGS
    private const uint PrimaryFlag = 1;               // MONITORINFOF_PRIMARY
    private const int Halftone = 4;
    private const uint SourceCopy = 0x00CC0020, CaptureBlt = 0x40000000;

    // The displays now, in EnumDisplayMonitors order (the engine's --monitor=N order).
    public static List<ScreenSource> List()
    {
        System.Diagnostics.Debug.WriteLine("[Screens] List enter");
        var handles = new List<nint>();
        EnumDisplayMonitors(0, 0, (monitor, _, _, _) => { handles.Add(monitor); return true; }, 0);
        var screens = new List<ScreenSource>();
        for (int i = 0; i < handles.Count; i++)
        {
            var info = new MonitorInfoEx { Size = Marshal.SizeOf<MonitorInfoEx>() };
            if (!GetMonitorInfo(handles[i], ref info))
            {
                System.Diagnostics.Debug.WriteLine($"[Screens] monitor {i}: GetMonitorInfo failed, skipped");
                continue;
            }
            int width = info.Monitor.Right - info.Monitor.Left, height = info.Monitor.Bottom - info.Monitor.Top;
            // The display's real resolution (the rectangle above can be scaled for DPI).
            var mode = new DevMode { Size = (short)Marshal.SizeOf<DevMode>() };
            int pixelWidth = width, pixelHeight = height;
            if (EnumDisplaySettings(info.Device, CurrentSettings, ref mode) && mode.PelsWidth > 0 && mode.PelsHeight > 0)
            {
                pixelWidth = mode.PelsWidth; pixelHeight = mode.PelsHeight;
            }
            var screen = new ScreenSource(i, handles[i], info.Device, (info.Flags & PrimaryFlag) != 0, pixelWidth, pixelHeight,
                info.Monitor.Left, info.Monitor.Top, width, height);
            System.Diagnostics.Debug.WriteLine($"[Screens] monitor {i}: {screen.Device} {pixelWidth}x{pixelHeight} at {screen.Left},{screen.Top} ({width}x{height}), primary {screen.Primary}");
            screens.Add(screen);
        }
        System.Diagnostics.Debug.WriteLine($"[Screens] List exit: {screens.Count} display(s)");
        return screens;
    }

    // Where the shared "Whole screen" settings live: a profile keyed like a game's, by a path
    // no executable can have (it is inside VRX's own data folder).
    public static string ProfileKey(string dataRoot)
    {
        ArgumentException.ThrowIfNullOrEmpty(dataRoot);
        return Path.Combine(dataRoot, "screens", "whole-screen");
    }

    public static bool IsScreen(RunningApp? app) => app != null && app.Pid == 0 && app.Windows.Count > 0 && app.Windows.All(w => w.Monitor >= 0);

    // The "Whole screen" entry for the game list: one window per display. Null with no display.
    public static RunningApp? App(string dataRoot, IReadOnlyList<ScreenSource> screens)
    {
        ArgumentException.ThrowIfNullOrEmpty(dataRoot);
        ArgumentNullException.ThrowIfNull(screens);
        if (screens.Count == 0) return null;
        return new RunningApp(0, Loc.Get("WholeScreen"), ProfileKey(dataRoot),
            screens.Select(s => new GameWindow(s.Handle, s.Title, s.Index)).ToList(), Icon);
    }

    // A picture of the display for its tile, about `width` pixels wide; null if Windows
    // does not give one. (Games in exclusive full screen can show black here.)
    public static BitmapSource? Snapshot(ScreenSource screen, int width)
    {
        ArgumentNullException.ThrowIfNull(screen);
        if (width <= 0 || screen.Width <= 0 || screen.Height <= 0) return null;
        int height = Math.Max(1, (int)Math.Round(width * (double)screen.Height / screen.Width));
        nint desktop = GetDC(0);
        if (desktop == 0) return null;
        nint memory = CreateCompatibleDC(desktop), bitmap = CreateCompatibleBitmap(desktop, width, height);
        try
        {
            if (memory == 0 || bitmap == 0) return null;
            nint old = SelectObject(memory, bitmap);
            SetStretchBltMode(memory, Halftone);
            SetBrushOrgEx(memory, 0, 0, 0);
            bool copied = StretchBlt(memory, 0, 0, width, height, desktop, screen.Left, screen.Top, screen.Width, screen.Height, SourceCopy | CaptureBlt);
            SelectObject(memory, old);
            if (!copied) { System.Diagnostics.Debug.WriteLine($"[Screens] Snapshot {screen.Index}: StretchBlt failed"); return null; }
            // GDI leaves the alpha byte 0 (fully transparent to WPF): read it as opaque BGR.
            var image = new FormatConvertedBitmap(Imaging.CreateBitmapSourceFromHBitmap(bitmap, 0, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions()),
                PixelFormats.Bgr32, null, 0);
            image.Freeze();
            return image;
        }
        finally
        {
            if (bitmap != 0) DeleteObject(bitmap);
            if (memory != 0) DeleteDC(memory);
            ReleaseDC(0, desktop);
        }
    }

    // A small display glyph, for the game card and the list (there is no executable icon).
    public static ImageSource Icon { get; } = MakeIcon();
    private static ImageSource MakeIcon()
    {
        var accent = new SolidColorBrush(Color.FromRgb(0x76, 0xDC, 0xC6)); accent.Freeze();
        var pen = new Pen(accent, 2.2) { LineJoin = PenLineJoin.Round, StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round }; pen.Freeze();
        var group = new DrawingGroup();
        group.Children.Add(new GeometryDrawing(null, pen, new RectangleGeometry(new Rect(3, 5, 26, 17), 2.5, 2.5)));
        group.Children.Add(new GeometryDrawing(null, pen, Geometry.Parse("M 16,22 L 16,27 M 10,27 L 22,27")));
        group.Children.Add(new GeometryDrawing(Brushes.Transparent, null, new RectangleGeometry(new Rect(0, 0, 32, 32))));
        var image = new DrawingImage(group); image.Freeze();
        return image;
    }
}
