using System.Diagnostics;
using Automaton.Core.Services.ComputerUse;
using SkiaSharp;

namespace PersonaBot;

/// <summary>
/// How the helper bot looks (Observer): from a frame of the desktop, the area around the person's pointer and the
/// window's top bar (where a browser shows the address), read by OCR on this PC, plus a tiny greyscale picture of the
/// area to tell change by. Nothing here leaves the PC.
/// </summary>
public static class ObserverEyes
{
    /// <summary>The area read around the pointer, at most (pixels).</summary>
    public const int AreaWidth = 900, AreaHeight = 560;

    /// <summary>The top bar's height (pixels): a browser's tabs and address bar.</summary>
    public const int TopBarHeight = 110;

    /// <summary>The thumbnail's size.</summary>
    public const int ThumbnailWidth = 40, ThumbnailHeight = 25;

    private static readonly Lazy<ITextRecognizer> Ocr = new(TextRecognizers.Default);

    /// <summary>A look at <paramref name="png"/> around (<paramref name="x"/>, <paramref name="y"/>) in its pixels; null when the frame can't be read.</summary>
    public static ObserverLook? Look(byte[] png, double x, double y, ITextRecognizer? recognizer = null, bool wholeFrame = false)
    {
        ArgumentNullException.ThrowIfNull(png);
        var watch = Stopwatch.StartNew();
        using var frame = SKBitmap.Decode(png);
        if (frame == null || frame.Width < 64 || frame.Height < 64)
        {
            Trace.TraceWarning("ObserverEyes.Look: the frame couldn't be read");
            return null;
        }
        var width = wholeFrame ? frame.Width : Math.Min(AreaWidth, frame.Width);
        var height = wholeFrame ? frame.Height : Math.Min(AreaHeight, frame.Height);
        var left = (int)Math.Clamp(x - width / 2.0, 0, frame.Width - width);
        var top = (int)Math.Clamp(y - height / 2.0, 0, frame.Height - height);
        using var area = Crop(frame, new SKRectI(left, top, left + width, top + height));
        using var bar = Crop(frame, new SKRectI(0, 0, frame.Width, Math.Min(TopBarHeight, frame.Height)));
        var ocr = recognizer ?? Ocr.Value;
        var near = Read(ocr, area);
        // The address bar's text is small: read at twice the size (within what the OCR takes) it comes out far truer.
        var scale = Math.Min(2.0, ocr.MaxImageDimension / (double)Math.Max(bar.Width, 1));
        using var bigBar = scale > 1.2 ? bar.Resize(new SKImageInfo((int)(bar.Width * scale), (int)(bar.Height * scale), SKColorType.Bgra8888, SKAlphaType.Premul), new SKSamplingOptions(SKCubicResampler.Mitchell)) : null;
        var topBar = Read(ocr, bigBar ?? bar);
        var text = (topBar.Length > 0 ? topBar + "\n---\n" : "") + near;
        var place = Observer.Place(topBar, near);
        Trace.WriteLine($"ObserverEyes.Look: {near.Length} chars near {x:0},{y:0}, {topBar.Length} in the top bar (read at {(bigBar == null ? 1 : scale):0.0}x), place {place ?? "?"}, in {watch.ElapsedMilliseconds} ms");
        return new ObserverLook(text, place, Thumbnail(area));
    }

    private static SKBitmap Crop(SKBitmap source, SKRectI rect)
    {
        var copy = new SKBitmap(rect.Width, rect.Height, SKColorType.Bgra8888, SKAlphaType.Premul);
        using var canvas = new SKCanvas(copy);
        canvas.DrawBitmap(source, rect, new SKRect(0, 0, rect.Width, rect.Height));
        return copy;
    }

    private static string Read(ITextRecognizer ocr, SKBitmap image)
    {
        try
        {
            return string.Join("\n", ocr.Recognize(image).Select(l => l.Text.Trim()).Where(t => t.Length > 0));
        }
        catch (Exception ex) when (ex is InvalidOperationException or NotSupportedException or System.Runtime.InteropServices.COMException or PlatformNotSupportedException)
        {
            Trace.TraceWarning($"ObserverEyes.Read: OCR failed: {ex.GetType().Name}: {ex.Message}");
            return "";
        }
    }

    /// <summary>A small greyscale picture of <paramref name="image"/>, to compare with the last (Observer.Difference).</summary>
    public static byte[] Thumbnail(SKBitmap image)
    {
        ArgumentNullException.ThrowIfNull(image);
        var grey = new byte[ThumbnailWidth * ThumbnailHeight];
        using var small = image.Resize(new SKImageInfo(ThumbnailWidth, ThumbnailHeight, SKColorType.Bgra8888, SKAlphaType.Premul), new SKSamplingOptions(SKFilterMode.Linear));
        if (small == null) return grey;
        for (var row = 0; row < ThumbnailHeight; row++)
        {
            for (var column = 0; column < ThumbnailWidth; column++)
            {
                var c = small.GetPixel(column, row);
                grey[row * ThumbnailWidth + column] = (byte)((c.Red * 299 + c.Green * 587 + c.Blue * 114) / 1000);
            }
        }
        return grey;
    }
}
