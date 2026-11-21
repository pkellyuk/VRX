using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using VRX.VrexBridge;

namespace VRX.Desktop;

// A local overlay, with transparent hit testing so the user's game keeps keyboard and mouse focus.
internal sealed class VrexMainScreenMarks : Window
{
    private readonly VrexMarkCanvas canvas = new();

    internal VrexMainScreenMarks()
    {
        AllowsTransparency = true;
        WindowStyle = WindowStyle.None;
        Background = Brushes.Transparent;
        ShowInTaskbar = false;
        ShowActivated = false;
        Focusable = false;
        ResizeMode = ResizeMode.NoResize;
        Topmost = true;
        Content = canvas;
        if (Application.Current?.MainWindow is { IsLoaded: true } main)
        {
            void CloseWithVrx(object? sender, EventArgs args) => Close();
            main.Closed += CloseWithVrx;
            Closed += (_, _) => main.Closed -= CloseWithVrx;
        }
    }

    internal void Update(VrexMark[] marks, bool replace)
    {
        var bounds = VrexScreenCapture.PrimaryBounds();
        var window = new WindowInteropHelper(this);
        nint hwnd = window.Handle;
        if (hwnd == 0)
        {
            hwnd = window.EnsureHandle();
            const int exStyle = -20, transparent = 0x20, noActivateStyle = 0x08000000, toolWindow = 0x80;
            SetWindowLong(hwnd, exStyle, GetWindowLong(hwnd, exStyle) | transparent | noActivateStyle | toolWindow);
        }
        canvas.Update(marks, replace);
        if (!IsVisible) Show();
        const uint noActivate = 0x0010;
        SetWindowPos(hwnd, new(-1), bounds.X, bounds.Y, bounds.Width, bounds.Height, noActivate);
    }

    internal static void VerifyFirstShow()
    {
        var overlay = new VrexMainScreenMarks();
        try
        {
            overlay.Update([], replace: true);
            nint hwnd = new WindowInteropHelper(overlay).Handle;
            const int overlayStyle = 0x08000000 | 0x80 | 0x20;
            if (!overlay.IsVisible || (GetWindowLong(hwnd, -20) & overlayStyle) != overlayStyle)
                throw new InvalidOperationException("PC annotation overlay must open without taking focus or intercepting input.");
        }
        finally { overlay.Close(); }
    }

    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")] private static extern int GetWindowLong(nint window, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")] private static extern int SetWindowLong(nint window, int index, int value);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(nint window, nint after, int x, int y, int width, int height, uint flags);
}
