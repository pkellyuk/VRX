using System.Runtime.Versioning;
using SkiaSharp;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// Reads lines of text, with word boxes in the image's pixels, from a picture of the screen.
/// <see cref="ScreenTextFinder"/> does the matching; this only does the reading.
/// </summary>
public interface ITextRecognizer
{
    /// <summary>The largest edge this recogniser accepts; larger regions are shrunk first.</summary>
    int MaxImageDimension { get; }

    IReadOnlyList<OcrTextLine> Recognize(SKBitmap image);
}

public static class TextRecognizers
{
    /// <summary>Windows OCR on Windows; elsewhere a recogniser that says none was configured.</summary>
    public static ITextRecognizer Default()
    {
        if (OperatingSystem.IsWindows())
        {
            return new WindowsTextRecognizer();
        }

        return UnavailableTextRecognizer.Instance;
    }
}

/// <summary>Windows' built-in OCR, given a Skia bitmap.</summary>
[SupportedOSPlatform("windows")]
public sealed class WindowsTextRecognizer : ITextRecognizer
{
    public int MaxImageDimension => WindowsOcr.MaxImageDimension();

    public IReadOnlyList<OcrTextLine> Recognize(SKBitmap image)
    {
        ArgumentNullException.ThrowIfNull(image);
        using var gdi = SkiaImages.ToGdi(image);
        return WindowsOcr.Recognize(gdi);
    }
}

/// <summary>Used where no recogniser was supplied; reading text fails with a clear message.</summary>
public sealed class UnavailableTextRecognizer : ITextRecognizer
{
    public static readonly UnavailableTextRecognizer Instance = new();

    public int MaxImageDimension => 4096;

    public IReadOnlyList<OcrTextLine> Recognize(SKBitmap image) =>
        throw new PlatformNotSupportedException("No text recogniser is configured on this platform, so on-screen text cannot be read.");
}
