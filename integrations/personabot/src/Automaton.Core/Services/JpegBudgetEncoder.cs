using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.Versioning;
using Encoder = System.Drawing.Imaging.Encoder;

namespace Automaton.Core.Services;

/// <summary>
/// Encodes an image as JPEG, stepping the quality down until it fits a byte budget.
///
/// Lifted out of <c>GridOverlayRenderer</c> when the room watch needed the same thing.
/// The problem it solves is not "make the file smaller", it is "make the request fit":
/// base64 inflates a payload by four thirds, several images can be in one conversation at
/// once, and nginx defaults <c>client_max_body_size</c> to a megabyte. A request over that
/// is rejected outright rather than degrading, so the image has to be sized against the
/// proxy at the far end and not against what looks acceptable locally.
///
/// A fixed quality cannot do this. How well a picture compresses depends entirely on what
/// is in it - an empty desk and a cluttered room differ by an order of magnitude at the
/// same setting - so any single choice is either wasteful on the one or over budget on
/// the other. Stepping down until it fits is the only rule that holds for both.
///
/// The <see cref="SkiaSharp.SKBitmap"/> overload runs everywhere (computer use on Android uses it);
/// the GDI+ overloads are Windows-only.
/// </summary>
public static class JpegBudgetEncoder
{
    /// <summary>
    /// Byte cap for one encoded image, chosen from the other end of the wire.
    ///
    /// Two computer-use views at the old fixed quality 92 came to 771KB of base64 on a
    /// busy desktop and, with tool schemas and history on top, the request was refused.
    /// 180KB each keeps a pair near 480KB with room left for the conversation.
    /// </summary>
    public const int DefaultMaxBytes = 180 * 1024;

    /// <summary>
    /// Qualities tried in order. Stops at the first that fits, so a picture that
    /// compresses well is never degraded for the sake of the ladder.
    /// </summary>
    private static readonly long[] QualityLadder = [92L, 85L, 80L, 72L, 65L, 55L];

    /// <summary>Encodes a Skia bitmap as JPEG within the budget, stepping down the same ladder.</summary>
    public static byte[] Encode(SkiaSharp.SKBitmap image, int maxBytes = DefaultMaxBytes, string context = "image")
    {
        AutomatonDebug.Log(
            AutomatonDebug.DEBUG_INFO,
            $"[JpegBudgetEncoder] Encode(Skia): Entry - context={context}, budget={maxBytes / 1024}KB");

        ArgumentNullException.ThrowIfNull(image);

        if (maxBytes <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maxBytes), maxBytes, "The budget must be positive.");
        }

        byte[]? encoded = null;
        foreach (var quality in QualityLadder)
        {
            encoded = ComputerUse.SkiaImages.EncodeJpeg(image, (int)quality);
            if (encoded.Length <= maxBytes)
            {
                AutomatonDebug.Log(
                    AutomatonDebug.DEBUG_INFO,
                    $"[JpegBudgetEncoder] Encode(Skia): Exit - {context} at quality {quality} is " +
                    $"{encoded.Length / 1024}KB, within the {maxBytes / 1024}KB budget");
                return encoded;
            }

            AutomatonDebug.Log(
                AutomatonDebug.DEBUG_INFO,
                $"[JpegBudgetEncoder] Encode(Skia): {context} at quality {quality} is {encoded.Length / 1024}KB, " +
                $"over the {maxBytes / 1024}KB budget, stepping down");
        }

        AutomatonDebug.LogWarning(
            $"[JpegBudgetEncoder] Encode(Skia): Exit - {context} is {encoded!.Length / 1024}KB at the lowest quality, " +
            $"still over the {maxBytes / 1024}KB budget; sending anyway.");
        return encoded;
    }

    /// <summary>
    /// Encodes as JPEG within the budget, or as PNG when this machine has no JPEG codec.
    /// </summary>
    /// <param name="image">The image to encode.</param>
    /// <param name="maxBytes">The budget. Defaults to <see cref="DefaultMaxBytes"/>.</param>
    /// <param name="context">Names the caller in the log, so two callers can be told apart.</param>
    [SupportedOSPlatform("windows")]
    public static byte[] Encode(Image image, int maxBytes = DefaultMaxBytes, string context = "image")
    {
        AutomatonDebug.Log(
            AutomatonDebug.DEBUG_INFO,
            $"[JpegBudgetEncoder] Encode: Entry - context={context}, budget={maxBytes / 1024}KB");

        ArgumentNullException.ThrowIfNull(image);

        if (maxBytes <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maxBytes), maxBytes, "The budget must be positive.");
        }

        using var stream = new MemoryStream();

        var jpegCodec = ImageCodecInfo.GetImageEncoders()
            .FirstOrDefault(codec => codec.FormatID == ImageFormat.Jpeg.Guid);

        if (jpegCodec is null)
        {
            AutomatonDebug.LogWarning(
                $"[JpegBudgetEncoder] Encode: no JPEG encoder is installed, falling back to PNG for {context}");
            image.Save(stream, ImageFormat.Png);
            return stream.ToArray();
        }

        byte[]? encoded = null;

        foreach (var quality in QualityLadder)
        {
            stream.SetLength(0);

            using var parameters = new EncoderParameters(1);
            using var qualityParameter = new EncoderParameter(Encoder.Quality, quality);
            parameters.Param[0] = qualityParameter;

            image.Save(stream, jpegCodec, parameters);
            encoded = stream.ToArray();

            if (encoded.Length <= maxBytes)
            {
                AutomatonDebug.Log(
                    AutomatonDebug.DEBUG_INFO,
                    $"[JpegBudgetEncoder] Encode: Exit - {context} at quality {quality} is " +
                    $"{encoded.Length / 1024}KB, within the {maxBytes / 1024}KB budget");
                return encoded;
            }

            AutomatonDebug.Log(
                AutomatonDebug.DEBUG_INFO,
                $"[JpegBudgetEncoder] Encode: {context} at quality {quality} is {encoded.Length / 1024}KB, " +
                $"over the {maxBytes / 1024}KB budget, stepping down");
        }

        // Even the bottom of the ladder can be over budget for a very large or very
        // detailed image. Send it rather than fail: an oversized request is a clear error
        // the caller can act on, where no image at all is a silent blind spot.
        AutomatonDebug.LogWarning(
            $"[JpegBudgetEncoder] Encode: Exit - {context} is {encoded!.Length / 1024}KB at the lowest quality, " +
            $"still over the {maxBytes / 1024}KB budget; sending anyway. If requests are being rejected as too " +
            "large, reduce the pixel budget rather than the quality.");
        return encoded;
    }

    /// <summary>
    /// Decodes, downscales to fit a pixel budget, and re-encodes within a byte budget.
    ///
    /// This is the shape a camera frame needs: a webcam hands over a 1920-wide JPEG at
    /// whatever quality its firmware chose, which is both larger than any model needs and
    /// larger than the wire wants. Doing it in one call keeps the intermediate bitmap's
    /// lifetime in one place - it is a native GDI+ handle, and leaking those is how a
    /// long-running capture loop eventually stops capturing.
    /// </summary>
    /// <param name="encodedImage">The image as it arrived, in any format GDI+ can read.</param>
    /// <param name="maximumEdge">Longest edge of the result, in pixels. Never upscales.</param>
    /// <param name="maxBytes">The byte budget for the result.</param>
    /// <param name="context">Names the caller in the log.</param>
    [SupportedOSPlatform("windows")]
    public static byte[] Reencode(
        byte[] encodedImage,
        int maximumEdge,
        int maxBytes = DefaultMaxBytes,
        string context = "image")
    {
        AutomatonDebug.Log(
            AutomatonDebug.DEBUG_INFO,
            $"[JpegBudgetEncoder] Reencode: Entry - context={context}, in={encodedImage?.Length ?? -1} bytes, " +
            $"maximumEdge={maximumEdge}");

        ArgumentNullException.ThrowIfNull(encodedImage);

        if (encodedImage.Length == 0)
        {
            throw new ArgumentException("There are no bytes to re-encode.", nameof(encodedImage));
        }

        if (maximumEdge <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maximumEdge), maximumEdge, "The edge budget must be positive.");
        }

        using var stream = new MemoryStream(encodedImage, writable: false);
        using var source = Image.FromStream(stream);

        var scale = Math.Min(1f, (float)maximumEdge / Math.Max(source.Width, source.Height));
        if (scale >= 1f)
        {
            // Already small enough. Re-encoding it anyway is what brings a camera's own
            // generous quality setting inside the byte budget.
            var asIs = Encode(source, maxBytes, context);
            AutomatonDebug.Log(
                AutomatonDebug.DEBUG_INFO,
                $"[JpegBudgetEncoder] Reencode: Exit - {context} kept at {source.Width}x{source.Height}, " +
                $"{asIs.Length / 1024}KB");
            return asIs;
        }

        var width = Math.Max(1, (int)(source.Width * scale));
        var height = Math.Max(1, (int)(source.Height * scale));

        using var resized = new Bitmap(width, height, PixelFormat.Format24bppRgb);
        using (var graphics = Graphics.FromImage(resized))
        {
            // High quality on purpose: the model is being asked to notice small things in
            // this picture, and a cheap resample removes exactly the detail it would have
            // noticed them by.
            graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            graphics.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
            graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.HighQuality;
            graphics.DrawImage(source, 0, 0, width, height);
        }

        var result = Encode(resized, maxBytes, context);

        AutomatonDebug.Log(
            AutomatonDebug.DEBUG_INFO,
            $"[JpegBudgetEncoder] Reencode: Exit - {context} from {source.Width}x{source.Height} to " +
            $"{width}x{height}, {encodedImage.Length / 1024}KB to {result.Length / 1024}KB");
        return result;
    }
}
