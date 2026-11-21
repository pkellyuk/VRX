using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using SkiaSharp;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// The image helpers computer use shares across platforms. Screens hand over
/// <see cref="SKBitmap"/>s in <see cref="SKColorType.Bgra8888"/>, which is byte for byte the
/// layout GDI+ uses for Format32bppArgb, so converting at the Windows edge is a row copy.
/// </summary>
public static class SkiaImages
{
    /// <summary>The colour layout every screen capture is normalised to.</summary>
    public static SKImageInfo Info(int width, int height) =>
        new(width, height, SKColorType.Bgra8888, SKAlphaType.Premul);

    /// <summary>Decodes PNG/JPEG bytes into a Bgra8888 bitmap the caller owns.</summary>
    public static SKBitmap Decode(byte[] encoded)
    {
        ArgumentNullException.ThrowIfNull(encoded);
        if (encoded.Length == 0)
        {
            throw new ArgumentException("There are no image bytes to decode.", nameof(encoded));
        }

        using var decoded = SKBitmap.Decode(encoded) ?? throw new InvalidDataException("The image bytes could not be decoded.");
        return Normalise(decoded);
    }

    /// <summary>A Bgra8888 copy of <paramref name="source"/>, whatever its layout. The caller owns it.</summary>
    public static SKBitmap Normalise(SKBitmap source)
    {
        ArgumentNullException.ThrowIfNull(source);
        var copy = new SKBitmap(Info(source.Width, source.Height));
        using var canvas = new SKCanvas(copy);
        canvas.Clear(SKColors.Black);
        canvas.DrawBitmap(source, 0, 0);
        return copy;
    }

    /// <summary>A copy of one rectangle of <paramref name="source"/>, clamped inside it. The caller owns it.</summary>
    public static SKBitmap Crop(SKBitmap source, ScreenRect region)
    {
        ArgumentNullException.ThrowIfNull(source);
        var clipped = region.ClampInside(new ScreenRect(0, 0, source.Width, source.Height));
        if (clipped.IsEmpty)
        {
            throw new ArgumentException($"Region {region} does not overlap the {source.Width}x{source.Height} image.", nameof(region));
        }

        var crop = new SKBitmap(Info(clipped.Width, clipped.Height));
        using var canvas = new SKCanvas(crop);
        canvas.DrawBitmap(source, new SKRect(clipped.X, clipped.Y, clipped.Right, clipped.Bottom), new SKRect(0, 0, clipped.Width, clipped.Height));
        return crop;
    }

    /// <summary>Resamples to an exact size with high-quality cubic filtering. The caller owns the result.</summary>
    public static SKBitmap Resize(SKBitmap source, int width, int height)
    {
        ArgumentNullException.ThrowIfNull(source);
        if (width < 1 || height < 1)
        {
            throw new ArgumentOutOfRangeException(nameof(width), $"Cannot resize to {width}x{height}.");
        }

        var resized = new SKBitmap(Info(width, height));
        using var canvas = new SKCanvas(resized);
        DrawScaled(canvas, source, new SKRect(0, 0, width, height));
        return resized;
    }

    /// <summary>Draws <paramref name="source"/> into <paramref name="destination"/>, the way GDI+'s HighQualityBicubic did.</summary>
    public static void DrawScaled(SKCanvas canvas, SKBitmap source, SKRect destination)
    {
        ArgumentNullException.ThrowIfNull(canvas);
        ArgumentNullException.ThrowIfNull(source);
        using var image = SKImage.FromBitmap(source);
        using var paint = new SKPaint { IsAntialias = true };
        canvas.DrawImage(image, destination, new SKSamplingOptions(SKCubicResampler.Mitchell), paint);
    }

    /// <summary>Encodes as PNG.</summary>
    public static byte[] EncodePng(SKBitmap bitmap)
    {
        ArgumentNullException.ThrowIfNull(bitmap);
        using var image = SKImage.FromBitmap(bitmap);
        using var data = image.Encode(SKEncodedImageFormat.Png, 100) ?? throw new InvalidOperationException("PNG encoding failed.");
        return data.ToArray();
    }

    /// <summary>Encodes as JPEG at one quality (0-100).</summary>
    public static byte[] EncodeJpeg(SKBitmap bitmap, int quality)
    {
        ArgumentNullException.ThrowIfNull(bitmap);
        using var image = SKImage.FromBitmap(bitmap);
        using var data = image.Encode(SKEncodedImageFormat.Jpeg, Math.Clamp(quality, 1, 100)) ?? throw new InvalidOperationException("JPEG encoding failed.");
        return data.ToArray();
    }

    /// <summary>
    /// A bold monospace typeface for grid chips: Consolas where Windows has it, the platform
    /// monospace face elsewhere (DejaVu Sans Mono on Linux, Droid Sans Mono on Android).
    /// </summary>
    public static SKTypeface ChipTypeface => ChipTypefaceLazy.Value;

    private static readonly Lazy<SKTypeface> ChipTypefaceLazy = new(() =>
    {
        foreach (var family in new[] { "Consolas", "Cascadia Mono", "DejaVu Sans Mono", "Droid Sans Mono", "Noto Sans Mono", "monospace" })
        {
            var match = SKFontManager.Default.MatchFamily(family, SKFontStyle.Bold);
            if (match != null)
            {
                AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[SkiaImages] ChipTypeface: using '{match.FamilyName}' for '{family}'");
                return match;
            }
        }

        AutomatonDebug.LogWarning("[SkiaImages] ChipTypeface: no monospace family found; using the default typeface");
        return SKTypeface.FromFamilyName(null, SKFontStyle.Bold) ?? SKTypeface.Default;
    });

    /// <summary>Copies a GDI+ bitmap into a new Bgra8888 Skia bitmap. The caller owns it.</summary>
    [SupportedOSPlatform("windows")]
    public static SKBitmap FromGdi(System.Drawing.Bitmap source)
    {
        ArgumentNullException.ThrowIfNull(source);
        var result = new SKBitmap(Info(source.Width, source.Height));
        var data = source.LockBits(
            new System.Drawing.Rectangle(0, 0, source.Width, source.Height),
            System.Drawing.Imaging.ImageLockMode.ReadOnly,
            System.Drawing.Imaging.PixelFormat.Format32bppPArgb);
        try
        {
            var rowBytes = source.Width * 4;
            var row = new byte[rowBytes];
            var target = result.GetPixels();
            for (var y = 0; y < source.Height; y++)
            {
                Marshal.Copy(data.Scan0 + y * data.Stride, row, 0, rowBytes);
                Marshal.Copy(row, 0, target + y * result.RowBytes, rowBytes);
            }
        }
        finally
        {
            source.UnlockBits(data);
        }

        return result;
    }

    /// <summary>Copies a Skia bitmap into a new GDI+ bitmap. The caller owns it.</summary>
    [SupportedOSPlatform("windows")]
    public static System.Drawing.Bitmap ToGdi(SKBitmap source)
    {
        ArgumentNullException.ThrowIfNull(source);
        using var normalised = source.ColorType == SKColorType.Bgra8888 && source.AlphaType == SKAlphaType.Premul ? null : Normalise(source);
        var pixels = normalised ?? source;
        var result = new System.Drawing.Bitmap(pixels.Width, pixels.Height, System.Drawing.Imaging.PixelFormat.Format32bppPArgb);
        var data = result.LockBits(
            new System.Drawing.Rectangle(0, 0, pixels.Width, pixels.Height),
            System.Drawing.Imaging.ImageLockMode.WriteOnly,
            System.Drawing.Imaging.PixelFormat.Format32bppPArgb);
        try
        {
            var rowBytes = pixels.Width * 4;
            var row = new byte[rowBytes];
            var sourcePixels = pixels.GetPixels();
            for (var y = 0; y < pixels.Height; y++)
            {
                Marshal.Copy(sourcePixels + y * pixels.RowBytes, row, 0, rowBytes);
                Marshal.Copy(row, 0, data.Scan0 + y * data.Stride, rowBytes);
            }
        }
        finally
        {
            result.UnlockBits(data);
        }

        return result;
    }
}
