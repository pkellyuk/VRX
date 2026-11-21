using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using VRX.VrexBridge;

namespace VRX.Desktop;

internal static class VrexScreenCapture
{
    internal sealed record Frame(byte[] Png, int PointerX, int PointerY);

    internal static Int32Rect PrimaryBounds()
    {
        var monitor = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        if (!GetMonitorInfoW(MonitorFromPoint(default, 1), ref monitor))
            throw new IOException("Windows could not find the main display.");
        var bounds = monitor.Monitor;
        return new(bounds.Left, bounds.Top, bounds.Right - bounds.Left, bounds.Bottom - bounds.Top);
    }

    // A hidden cursor (common in games) deliberately selects the full screen.
    internal static Frame Capture()
    {
        IntPtr display = GetDC(IntPtr.Zero);
        if (display == IntPtr.Zero) throw new IOException("Windows could not open the main display.");
        IntPtr memory = IntPtr.Zero, image = IntPtr.Zero, previous = IntPtr.Zero;
        try
        {
            var bounds = PrimaryBounds();
            int x = bounds.X, y = bounds.Y, width = bounds.Width, height = bounds.Height;
            if (width < 64 || height < 64) throw new IOException("The main display is unavailable.");
            var cursor = new CursorInfo { Size = Marshal.SizeOf<CursorInfo>() };
            bool visible = GetCursorInfo(ref cursor) && (cursor.Flags & 1) != 0 &&
                cursor.Point.X >= x && cursor.Point.X < x + width &&
                cursor.Point.Y >= y && cursor.Point.Y < y + height;
            memory = CreateCompatibleDC(display);
            image = CreateCompatibleBitmap(display, width, height);
            if (memory == IntPtr.Zero || image == IntPtr.Zero) throw new IOException("Windows could not allocate a screen capture.");
            previous = SelectObject(memory, image);
            if (previous == IntPtr.Zero || previous == new IntPtr(-1) ||
                !BitBlt(memory, 0, 0, width, height, display, x, y, 0x40CC0020))
                throw new IOException("Windows could not capture the main display.");
            BitmapSource bitmap = Imaging.CreateBitmapSourceFromHBitmap(image, IntPtr.Zero, Int32Rect.Empty,
                BitmapSizeOptions.FromEmptyOptions());
            return Encode(bitmap, visible ? (cursor.Point.X - x) / (double)width : -1,
                visible ? (cursor.Point.Y - y) / (double)height : -1);
        }
        finally
        {
            if (previous != IntPtr.Zero && previous != new IntPtr(-1)) SelectObject(memory, previous);
            if (image != IntPtr.Zero) DeleteObject(image);
            if (memory != IntPtr.Zero) DeleteDC(memory);
            ReleaseDC(IntPtr.Zero, display);
        }
    }

    internal static Frame Encode(BitmapSource bitmap, double u, double v)
    {
        double scale = Math.Min(1, Math.Min(1280d / bitmap.PixelWidth, 720d / bitmap.PixelHeight));
        if (scale < 1) bitmap = new TransformedBitmap(bitmap, new ScaleTransform(scale, scale));
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var output = new MemoryStream();
        encoder.Save(output);
        if (output.Length > VrexProtocol.MaxPcImageBytes) throw new IOException("The screenshot is too large to send.");
        bool pointer = u is >= 0 and <= 1 && v is >= 0 and <= 1;
        return new(output.ToArray(), pointer ? Math.Min(bitmap.PixelWidth - 1, (int)(u * bitmap.PixelWidth)) : -1,
            pointer ? Math.Min(bitmap.PixelHeight - 1, (int)(v * bitmap.PixelHeight)) : -1);
    }

    [StructLayout(LayoutKind.Sequential)] private struct NativePoint { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo { public int Size; public Rect Monitor, Work; public uint Flags; }
    [StructLayout(LayoutKind.Sequential)] private struct CursorInfo { public int Size; public uint Flags; public IntPtr Cursor; public NativePoint Point; }
    [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromPoint(NativePoint point, uint flags);
    [DllImport("user32.dll")] private static extern bool GetMonitorInfoW(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll")] private static extern bool GetCursorInfo(ref CursorInfo info);
    [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int width, int height);
    [DllImport("gdi32.dll")] private static extern IntPtr SelectObject(IntPtr dc, IntPtr item);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(IntPtr item);
    [DllImport("gdi32.dll")] private static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")] private static extern bool BitBlt(IntPtr destination, int x, int y, int width, int height, IntPtr source, int sourceX, int sourceY, uint operation);
}
