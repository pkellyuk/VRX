using System.Runtime.Versioning;
using SkiaSharp;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// Draws the labelled grid the model navigates by.
///
/// The grid is drawn onto the captured bitmap, not onto the real desktop. That
/// keeps it deterministic — no repaint races, no chance of the overlay window
/// appearing in its own screenshot, and cropping a region for a zoom step is just
/// a smaller capture. What the model sees and what the chat shows are the same
/// bytes.
///
/// Drawn with SkiaSharp so the same renderer runs on Windows and Android (ANDROID.md Phase 1).
/// The <see cref="System.Drawing.Bitmap"/> overloads remain for Windows-only callers.
/// </summary>
public sealed class GridOverlayRenderer
{
    /// <summary>
    /// Pixel budget for a rendered view — 1536x864, the size a 16:9 region used to be
    /// given.
    ///
    /// A budget on total pixels rather than on the long edge, because a cap on the
    /// long edge punishes wide regions for their shape. A full-width taskbar strip is
    /// nearly all long edge, so under a long-edge cap it was squeezed to 0.4x and a
    /// 40px icon rendered at 16px — unreadable, which defeated the whole point of
    /// having a "look at the taskbar" region. Under a pixel budget the same strip
    /// gets 1.2x, exactly like every other named area, because area is what actually
    /// costs vision tokens.
    ///
    /// A 16:9 region renders identically under both rules, so this changes nothing
    /// for the ordinary case.
    /// </summary>
    public const int DefaultTargetPixels = 1536 * 864;

    /// <summary>
    /// Hard cap on either dimension. Stops an extreme aspect ratio turning into an
    /// image thousands of pixels along one edge and a sliver along the other.
    /// </summary>
    public const int MaxImageEdge = 4608;

    public const int DefaultRows = 3;
    public const int DefaultColumns = 4;

    /// <summary>
    /// Past this the chips crowd each other and the model starts misreading codes,
    /// which costs more round trips than the extra density saves.
    /// </summary>
    public const int MaxAxisDivisions = 12;

    /// <summary>
    /// Byte cap for one encoded view.
    ///
    /// Chosen from the other end of the wire. Base64 inflates by 4/3, two views are
    /// retained at a time, and nginx defaults <c>client_max_body_size</c> to 1MB — so
    /// two views at the old fixed quality 92 came to 771KB of base64 on a busy
    /// desktop and, with tool schemas and history on top, the request was rejected
    /// outright. 180KB each keeps the pair near 480KB with room for the conversation.
    /// </summary>
    public const int MaxEncodedBytes = JpegBudgetEncoder.DefaultMaxBytes;

    private static readonly SKColor GridLineColor = new(0, 255, 102, 150);
    private static readonly SKColor ChipFillColor = new(0, 58, 24, 205);
    private static readonly SKColor ChipBorderColor = new(0, 255, 102, 235);
    private static readonly SKColor ChipTextColor = new(255, 255, 255, 255);
    private static readonly SKColor CrosshairColor = new(0, 255, 102, 255);
    private static readonly SKColor CrosshairCoreColor = new(255, 255, 255, 255);
    private static readonly SKColor PointerHaloColor = new(0, 0, 0, 190);
    private static readonly SKColor PointerInkColor = new(255, 40, 90, 235);

    /// <summary>Renders one view from a GDI+ bitmap (Windows callers that capture with GDI+).</summary>
    [SupportedOSPlatform("windows")]
    public GridView Render(
        System.Drawing.Bitmap source,
        ScreenRect region,
        int rows = DefaultRows,
        int columns = DefaultColumns,
        int targetPixels = DefaultTargetPixels,
        GridImageEncoding encoding = GridImageEncoding.Jpeg,
        ScreenPoint? pointer = null)
    {
        ArgumentNullException.ThrowIfNull(source);
        using var skia = SkiaImages.FromGdi(source);
        return Render(skia, region, rows, columns, targetPixels, encoding, pointer);
    }

    /// <summary>GDI+ overload of <see cref="RenderTextTargets(SKBitmap, ScreenRect, IReadOnlyList{TextTarget}, int)"/>.</summary>
    [SupportedOSPlatform("windows")]
    public GridView RenderTextTargets(System.Drawing.Bitmap source, ScreenRect region, IReadOnlyList<TextTarget> targets, int targetPixels = DefaultTargetPixels)
    {
        ArgumentNullException.ThrowIfNull(source);
        using var skia = SkiaImages.FromGdi(source);
        return RenderTextTargets(skia, region, targets, targetPixels);
    }

    /// <summary>GDI+ overload of <see cref="RenderPeek(SKBitmap, ScreenRect, ScreenPoint, int)"/>.</summary>
    [SupportedOSPlatform("windows")]
    public GridView RenderPeek(System.Drawing.Bitmap source, ScreenRect region, ScreenPoint point, int outputEdge = DefaultPeekEdge)
    {
        ArgumentNullException.ThrowIfNull(source);
        using var skia = SkiaImages.FromGdi(source);
        return RenderPeek(skia, region, point, outputEdge);
    }

    /// <summary>
    /// Renders one view.
    /// </summary>
    /// <param name="source">
    /// Bitmap covering exactly <paramref name="region"/> at native screen resolution.
    /// </param>
    /// <param name="region">The screen rect the bitmap covers.</param>
    /// <param name="rows">Cell rows; produces rows+1 labelled intersection rows.</param>
    /// <param name="columns">Cell columns; produces columns+1 labelled intersection columns.</param>
    /// <param name="targetPixels">Pixel budget for the output image; area, not edge length.</param>
    public GridView Render(
        SKBitmap source,
        ScreenRect region,
        int rows = DefaultRows,
        int columns = DefaultColumns,
        int targetPixels = DefaultTargetPixels,
        GridImageEncoding encoding = GridImageEncoding.Jpeg,
        ScreenPoint? pointer = null)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] Render: Entry - region={region}, rows={rows}, columns={columns}, targetPixels={targetPixels}");

        ArgumentNullException.ThrowIfNull(source);

        if (region.IsEmpty)
        {
            throw new ArgumentException($"Region must have a positive size, got {region}.", nameof(region));
        }

        if (rows < 1 || rows > MaxAxisDivisions)
        {
            throw new ArgumentOutOfRangeException(nameof(rows), rows, $"Rows must be between 1 and {MaxAxisDivisions}.");
        }

        if (columns < 1 || columns > MaxAxisDivisions)
        {
            throw new ArgumentOutOfRangeException(nameof(columns), columns, $"Columns must be between 1 and {MaxAxisDivisions}.");
        }

        if (targetPixels < 64 * 64 || targetPixels > 4096 * 4096)
        {
            throw new ArgumentOutOfRangeException(nameof(targetPixels), targetPixels, "Target pixel budget must be between 64x64 and 4096x4096.");
        }

        if (source.Width != region.Width || source.Height != region.Height)
        {
            throw new ArgumentException(
                $"Source bitmap is {source.Width}x{source.Height} but region is {region.Width}x{region.Height}; the bitmap must cover the region exactly or every label resolves to the wrong screen point.",
                nameof(source));
        }

        // Area-preserving: every region gets the same number of output pixels, and so
        // the same magnification, whatever its shape.
        var scale = Math.Sqrt(targetPixels / ((double)region.Width * region.Height));

        // ...unless the shape is extreme enough that the budget would produce a very
        // long, very thin image. Then the edge cap wins and the scale comes down.
        var longestEdge = Math.Max(region.Width, region.Height) * scale;
        if (longestEdge > MaxImageEdge)
        {
            scale *= MaxImageEdge / longestEdge;
        }

        var imageWidth = Math.Max(1, (int)Math.Round(region.Width * scale));
        var imageHeight = Math.Max(1, (int)Math.Round(region.Height * scale));

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] Render: scale={scale:0.###}, output={imageWidth}x{imageHeight}");

        var view = new GridView
        {
            Region = region,
            Rows = rows,
            Columns = columns,
            ImageWidth = imageWidth,
            ImageHeight = imageHeight,
            Magnification = scale,
            ScreenPitchX = (double)region.Width / columns,
            ScreenPitchY = (double)region.Height / rows
        };

        using var canvas = new SKBitmap(SkiaImages.Info(imageWidth, imageHeight));
        using (var graphics = new SKCanvas(canvas))
        {
            SkiaImages.DrawScaled(graphics, source, new SKRect(0, 0, imageWidth, imageHeight));

            BuildLabels(view, scale);
            DrawGridLines(graphics, view, scale, imageWidth, imageHeight);
            DrawLabelChips(graphics, view, scale, imageWidth, imageHeight);

            // Off unless a caller asks. The grid descent never does: an arrow in
            // those images is noise in exactly the picture whose whole job is
            // "which marker is on the target".
            if (pointer.HasValue)
            {
                DrawPointerMarker(graphics, region, scale, pointer.Value);
            }
        }

        view.ImageBytes = Encode(canvas, encoding);
        view.MediaType = encoding == GridImageEncoding.Png ? "image/png" : "image/jpeg";

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] Render: Exit - {view.Labels.Count} labels, screenPitch={view.ScreenPitchX:0.#}x{view.ScreenPitchY:0.#}px, {view.MediaType} {view.ImageBytes.Length} bytes");
        return view;
    }

    /// <summary>
    /// Renders a region with each text match boxed and tagged with its label, for when
    /// several matches read alike and the model needs to see which is which. No grid:
    /// the labels here are the matches themselves.
    /// </summary>
    public GridView RenderTextTargets(SKBitmap source, ScreenRect region, IReadOnlyList<TextTarget> targets, int targetPixels = DefaultTargetPixels)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] RenderTextTargets: Entry - region={region}, {targets?.Count ?? 0} target(s)");

        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(targets);

        if (source.Width != region.Width || source.Height != region.Height)
        {
            throw new ArgumentException($"Source bitmap is {source.Width}x{source.Height} but region is {region.Width}x{region.Height}.", nameof(source));
        }

        var scale = Math.Sqrt(targetPixels / ((double)region.Width * region.Height));
        var longestEdge = Math.Max(region.Width, region.Height) * scale;
        if (longestEdge > MaxImageEdge)
        {
            scale *= MaxImageEdge / longestEdge;
        }

        var imageWidth = Math.Max(1, (int)Math.Round(region.Width * scale));
        var imageHeight = Math.Max(1, (int)Math.Round(region.Height * scale));

        using var canvas = new SKBitmap(SkiaImages.Info(imageWidth, imageHeight));
        using (var graphics = new SKCanvas(canvas))
        {
            SkiaImages.DrawScaled(graphics, source, new SKRect(0, 0, imageWidth, imageHeight));

            var fontSize = Math.Clamp(imageHeight / 42f, 11f, 22f);
            using var font = ChipFont(fontSize);
            using var boxPen = Stroke(CrosshairColor, 2.5f);

            foreach (var target in targets)
            {
                var box = SKRect.Create(
                    (float)((target.Bounds.X - region.X) * scale) - 3f,
                    (float)((target.Bounds.Y - region.Y) * scale) - 3f,
                    (float)(target.Bounds.Width * scale) + 6f,
                    (float)(target.Bounds.Height * scale) + 6f);

                if (box.Right < 0 || box.Bottom < 0 || box.Left > imageWidth || box.Top > imageHeight)
                {
                    continue;
                }

                graphics.DrawRect(box, boxPen);

                // The tag sits above the box, so it never covers the text it names.
                var size = MeasureChipText(target.Label, font);
                var chipWidth = size.Width + 8f;
                var chipHeight = size.Height + 2f;
                var chipX = box.Left;
                var chipY = box.Top - size.Height - 4f;
                if (chipY < 0)
                {
                    chipY = box.Bottom + 2f;
                }

                chipX = Math.Clamp(chipX, 0f, Math.Max(0f, imageWidth - chipWidth));
                chipY = Math.Clamp(chipY, 0f, Math.Max(0f, imageHeight - chipHeight));
                DrawChip(graphics, font, target.Label, SKRect.Create(chipX, chipY, chipWidth, chipHeight));
            }
        }

        var view = new GridView
        {
            Region = region,
            ImageWidth = imageWidth,
            ImageHeight = imageHeight,
            Magnification = scale,
            ScreenPitchX = region.Width,
            ScreenPitchY = region.Height,
            ImageBytes = Encode(canvas, GridImageEncoding.Jpeg),
            MediaType = "image/jpeg"
        };

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] RenderTextTargets: Exit - {imageWidth}x{imageHeight}, {view.ImageBytes.Length} bytes");
        return view;
    }

    /// <summary>Longest edge of a peek image. About a tenth of a full view in tokens.</summary>
    public const int DefaultPeekEdge = 360;

    /// <summary>
    /// A small, magnified crop around one point with a crosshair on it — the cheapest
    /// possible look at what a label is actually sitting on.
    ///
    /// Not <see cref="Render"/>: that gives every region the same pixel budget, so a
    /// 120px crop would come back as a 1536x864 image costing as much for the model to
    /// read as a whole screen. A peek is a few hundred tokens, which is the point of it:
    /// checking a marker before a click should cost less than a wrong click.
    /// </summary>
    public GridView RenderPeek(SKBitmap source, ScreenRect region, ScreenPoint point, int outputEdge = DefaultPeekEdge)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] RenderPeek: Entry - region={region}, point={point}, outputEdge={outputEdge}");

        ArgumentNullException.ThrowIfNull(source);

        if (source.Width != region.Width || source.Height != region.Height)
        {
            throw new ArgumentException($"Source bitmap is {source.Width}x{source.Height} but region is {region.Width}x{region.Height}.", nameof(source));
        }

        var scale = (double)outputEdge / Math.Max(region.Width, region.Height);
        var imageWidth = Math.Max(1, (int)Math.Round(region.Width * scale));
        var imageHeight = Math.Max(1, (int)Math.Round(region.Height * scale));

        using var canvas = new SKBitmap(SkiaImages.Info(imageWidth, imageHeight));
        using (var graphics = new SKCanvas(canvas))
        {
            SkiaImages.DrawScaled(graphics, source, new SKRect(0, 0, imageWidth, imageHeight));
            DrawPointerMarker(graphics, region, scale, point);
        }

        var view = new GridView
        {
            Region = region,
            ImageWidth = imageWidth,
            ImageHeight = imageHeight,
            Magnification = scale,
            ScreenPitchX = region.Width,
            ScreenPitchY = region.Height,
            ImageBytes = Encode(canvas, GridImageEncoding.Jpeg),
            MediaType = "image/jpeg"
        };

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] RenderPeek: Exit - {imageWidth}x{imageHeight} at {scale:0.##}x, {view.ImageBytes.Length} bytes");
        return view;
    }

    /// <summary>
    /// Encodes the annotated canvas.
    ///
    /// JPEG by default, and at a high quality rather than a default one: the whole
    /// image is base64 on the wire to llama-server, and a 4K desktop as PNG is over
    /// half a megabyte per step — enough to trip a reverse proxy body limit partway
    /// through a descent. Quality 92 keeps the thin grid lines and small chip text
    /// clean while cutting the payload by roughly an order of magnitude.
    /// </summary>
    private static byte[] Encode(SKBitmap canvas, GridImageEncoding encoding)
    {
        ArgumentNullException.ThrowIfNull(canvas);

        if (encoding == GridImageEncoding.Png)
        {
            return SkiaImages.EncodePng(canvas);
        }

        // The ladder and the budget moved to JpegBudgetEncoder when the room watch needed
        // the same rule. One implementation of "shrink until the proxy will take it" is
        // the point; two would drift apart the first time either budget was retuned.
        return JpegBudgetEncoder.Encode(canvas, MaxEncodedBytes, "grid view");
    }

    /// <summary>
    /// A one-line description of the view for the model, alongside the image.
    ///
    /// The pitch is the useful number: it tells the model how precise the current
    /// view is, so it knows whether one more zoom is needed to land on a 24 px icon.
    /// </summary>
    public static string BuildLegend(GridView view, ScreenRect? desktop = null)
    {
        ArgumentNullException.ThrowIfNull(view);

        var first = GridLabelNaming.Build(0, 0);
        var last = GridLabelNaming.Build(view.Rows, view.Columns);

        var where = desktop.HasValue
            ? $"This view covers {DescribePosition(view.Region, desktop.Value)}. "
            : string.Empty;

        return
            $"{where}Grid {view.Rows}x{view.Columns} cells, {view.Labels.Count} labelled intersections from {first} (top-left) to {last} (bottom-right). " +
            $"Rows are letters top to bottom, columns are numbers left to right. " +
            $"Magnification {view.Magnification:0.##}x; adjacent labels are {view.ScreenPitchX:0} x {view.ScreenPitchY:0} screen pixels apart. " +
            $"{DescribeScale(view)} " +
            $"{DescribeResolvableDetail(view)}" +
            $"Pick the label whose marker sits nearest the target, then zoom to it if the marker is not clearly on top of it.";
    }

    /// <summary>
    /// The image's own size and how it maps onto the screen.
    ///
    /// Grid descent is built so the model never has to measure, but models measure
    /// anyway: they estimate where something sits in the image in pixels to decide which
    /// area or labels to look at next, and without the image size they converted wrongly
    /// and gridded empty page beside the target. Given that it happens regardless, the
    /// numbers might as well be right. Clicks still go through labels, so a wrong
    /// estimate costs a view, never a misclick.
    ///
    /// The size stated is the image as sent. A provider that resizes images on the way
    /// to the model changes the pixels it sees, but not the proportions, which is why
    /// the mapping is also given as fractions of the image.
    /// </summary>
    public static string DescribeScale(GridView view)
    {
        ArgumentNullException.ThrowIfNull(view);

        var screenPerImage = view.Magnification > 0 ? 1 / view.Magnification : 1;

        return
            $"The image is {view.ImageWidth}x{view.ImageHeight} pixels; one image pixel is {screenPerImage:0.##} screen pixels. " +
            $"Its left edge is screen x={view.Region.X} and right edge x={view.Region.Right}; its top is screen y={view.Region.Y} and bottom y={view.Region.Bottom}, " +
            $"so a point a fraction f of the way across is at screen x={view.Region.X}+f*{view.Region.Width}. Use this to choose where to look next; click by label, by text, or with cu_click_at at fractions of this image " +
            $"(pixels you read off it are \"picture x,y\", not screen pixels{(Math.Abs(view.Magnification - 1) > 0.01 ? $": here they differ by {screenPerImage:0.##}x" : "")}).";
    }

    /// <summary>
    /// Warns when the view is too shrunken to identify small things in.
    ///
    /// This is the failure that sank the first live descent. On a 4K desktop the
    /// whole-screen view is drawn at about a third scale, which renders a taskbar
    /// icon at ~13 image pixels — too few for the model to actually read. It did not
    /// know that, so instead of zooming to look properly it answered from prior
    /// knowledge, put the Windows 11 centred Start button at the far left where
    /// Windows 10 keeps it, and then spent a dozen round trips zooming into empty
    /// taskbar.
    ///
    /// Telling it the floor turns "I think I see it" into "I cannot see this yet".
    /// </summary>
    public static string DescribeResolvableDetail(GridView view)
    {
        ArgumentNullException.ThrowIfNull(view);

        if (view.Magnification >= 0.9)
        {
            return string.Empty;
        }

        // Below roughly 24 rendered pixels an icon is a smudge. Converted back to
        // screen pixels, that is the smallest thing worth trusting in this view.
        var floor = (int)Math.Ceiling(24 / view.Magnification);

        return
            $"CAUTION: this view is shrunk to {view.Magnification:0.##}x, so anything smaller than about {floor} screen pixels — icons, small buttons, individual toolbar items — is NOT identifiable here and you must not guess at it from memory of how such an application usually looks. " +
            $"Zoom into the area you believe it is in and identify it there. ";
    }

    /// <summary>
    /// Describes where a region sits on the desktop in words.
    ///
    /// A deep zoom hands the model a picture with no landmarks in it, and it has no
    /// way to tell a crop of the taskbar from a crop of a window three quarters up
    /// the screen. Saying "the bottom-left corner of the desktop" costs a few tokens
    /// and lets it notice when a zoom went somewhere it did not intend.
    /// </summary>
    public static string DescribePosition(ScreenRect region, ScreenRect desktop)
    {
        if (desktop.IsEmpty)
        {
            return $"{region.Width}x{region.Height} pixels at ({region.X},{region.Y})";
        }

        if (region.Width >= desktop.Width && region.Height >= desktop.Height)
        {
            return "the whole desktop";
        }

        var centreX = region.X + region.Width / 2.0;
        var centreY = region.Y + region.Height / 2.0;

        var horizontal = Fraction(centreX - desktop.X, desktop.Width) switch
        {
            0 => "left",
            1 => "centre",
            _ => "right"
        };

        var vertical = Fraction(centreY - desktop.Y, desktop.Height) switch
        {
            0 => "top",
            1 => "middle",
            _ => "bottom"
        };

        var share = 100.0 * region.Width * region.Height / ((double)desktop.Width * desktop.Height);

        return $"the {vertical}-{horizontal} of the desktop — {region.Width}x{region.Height} screen pixels at ({region.X},{region.Y}), {share:0.##}% of the screen area";
    }

    private static int Fraction(double offset, int total)
    {
        if (total <= 0)
        {
            return 1;
        }

        var third = offset / total * 3.0;
        return Math.Clamp((int)third, 0, 2);
    }

    private static void BuildLabels(GridView view, double scale)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] BuildLabels: Entry - {view.Rows + 1}x{view.Columns + 1} intersections");

        for (var row = 0; row <= view.Rows; row++)
        {
            for (var column = 0; column <= view.Columns; column++)
            {
                var screenX = view.Region.X + (int)Math.Round(column * view.ScreenPitchX);
                var screenY = view.Region.Y + (int)Math.Round(row * view.ScreenPitchY);

                // The far edge rounds to exactly Right/Bottom, which is one pixel
                // outside the region. Clicking there can land on the next window.
                screenX = Math.Min(screenX, view.Region.Right - 1);
                screenY = Math.Min(screenY, view.Region.Bottom - 1);

                var label = new GridLabel
                {
                    Label = GridLabelNaming.Build(row, column),
                    Row = row,
                    Column = column,
                    Point = new ScreenPoint(screenX, screenY)
                };

                view.Labels[label.Label] = label;
            }
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] BuildLabels: Exit - built {view.Labels.Count} labels");
    }

    private static void DrawGridLines(SKCanvas graphics, GridView view, double scale, int imageWidth, int imageHeight)
    {
        using var pen = Stroke(GridLineColor, 1.5f);

        for (var column = 0; column <= view.Columns; column++)
        {
            var x = ToImageX(view, column * view.ScreenPitchX, scale, imageWidth);
            graphics.DrawLine(x, 0, x, imageHeight, pen);
        }

        for (var row = 0; row <= view.Rows; row++)
        {
            var y = ToImageY(view, row * view.ScreenPitchY, scale, imageHeight);
            graphics.DrawLine(0, y, imageWidth, y, pen);
        }
    }

    /// <summary>
    /// Draws a crosshair where the mouse pointer actually is.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Deliberately a fixed size in output pixels rather than a scaled one. The
    /// crop is magnified by the column count at every descent step, so a marker
    /// sized in screen pixels would be several times larger after two zooms and
    /// would cover the very thing it is meant to help you judge.
    /// </para>
    /// <para>
    /// The position comes from the OS, not from the label arithmetic, which is
    /// what makes it worth drawing at all: it is evidence about where the pointer
    /// really ended up, including any DPI or multi-monitor origin error in the
    /// chain that put it there. A marker derived from the same maths as the click
    /// would only ever agree with itself.
    /// </para>
    /// </remarks>
    private static void DrawPointerMarker(SKCanvas graphics, ScreenRect region, double scale, ScreenPoint pointer)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] DrawPointerMarker: Entry - pointer=({pointer.X},{pointer.Y}), region={region}");

        if (pointer.X < region.X || pointer.Y < region.Y ||
            pointer.X >= region.X + region.Width || pointer.Y >= region.Y + region.Height)
        {
            // Outside the captured region, so there is nothing honest to draw. Say
            // so in the log rather than clamping it to an edge, which would put a
            // marker somewhere the pointer is not.
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[GridOverlayRenderer] DrawPointerMarker: Exit - pointer is outside this region, nothing drawn");
            return;
        }

        var x = (float)((pointer.X - region.X) * scale);
        var y = (float)((pointer.Y - region.Y) * scale);

        const float arm = 11f;
        const float gap = 3f;

        // Black underneath, colour on top: the marker has to stay visible over a
        // white dialog and a dark toolbar alike.
        using var halo = Stroke(PointerHaloColor, 3.4f);
        using var ink = Stroke(PointerInkColor, 1.6f);

        foreach (var pen in new[] { halo, ink })
        {
            graphics.DrawLine(x - arm, y, x - gap, y, pen);
            graphics.DrawLine(x + gap, y, x + arm, y, pen);
            graphics.DrawLine(x, y - arm, x, y - gap, pen);
            graphics.DrawLine(x, y + gap, x, y + arm, pen);
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[GridOverlayRenderer] DrawPointerMarker: Exit - drawn at image ({x:0.#},{y:0.#})");
    }

    private static void DrawLabelChips(SKCanvas graphics, GridView view, double scale, int imageWidth, int imageHeight)
    {
        var fontSize = Math.Clamp(imageHeight / 42f, 11f, 22f);
        using var font = ChipFont(fontSize);
        using var crosshairPen = Stroke(CrosshairColor, 2f);
        using var crosshairBrush = Fill(CrosshairColor);
        using var coreBrush = Fill(CrosshairCoreColor);

        var arm = Math.Max(6f, fontSize * 0.8f);

        foreach (var label in view.Labels.Values)
        {
            var x = ToImageX(view, label.Column * view.ScreenPitchX, scale, imageWidth);
            var y = ToImageY(view, label.Row * view.ScreenPitchY, scale, imageHeight);

            // The chip sits beside the intersection rather than on it, so the exact
            // click point stays visible. A label centred on the point would hide the
            // very pixels the model is trying to judge it against.
            var size = MeasureChipText(label.Label, font);
            var chipWidth = size.Width + 8f;
            var chipHeight = size.Height + 2f;

            var chipX = x + arm + 2f;
            var chipY = y + arm + 2f;

            if (chipX + chipWidth > imageWidth)
            {
                chipX = x - arm - 2f - chipWidth;
            }

            if (chipY + chipHeight > imageHeight)
            {
                chipY = y - arm - 2f - chipHeight;
            }

            chipX = Math.Clamp(chipX, 0f, Math.Max(0f, imageWidth - chipWidth));
            chipY = Math.Clamp(chipY, 0f, Math.Max(0f, imageHeight - chipHeight));

            DrawChip(graphics, font, label.Label, SKRect.Create(chipX, chipY, chipWidth, chipHeight));

            // The crosshair, not the chip, marks where a click lands, so it has to
            // read at a glance even against busy pixels: long green arms for the
            // axis, a white core so it stays visible on a green or bright target.
            graphics.DrawLine(x - arm, y, x + arm, y, crosshairPen);
            graphics.DrawLine(x, y - arm, x, y + arm, crosshairPen);
            graphics.DrawOval(SKRect.Create(x - 3.5f, y - 3.5f, 7f, 7f), crosshairBrush);
            graphics.DrawOval(SKRect.Create(x - 1.5f, y - 1.5f, 3f, 3f), coreBrush);
        }
    }

    private static SKFont ChipFont(float size) => new(SkiaImages.ChipTypeface, size) { Edging = SKFontEdging.Antialias };

    private static SKPaint Stroke(SKColor color, float width) =>
        new() { Color = color, StrokeWidth = width, Style = SKPaintStyle.Stroke, IsAntialias = true };

    private static SKPaint Fill(SKColor color) =>
        new() { Color = color, Style = SKPaintStyle.Fill, IsAntialias = true };

    /// <summary>
    /// The chip's text size, laid out the way GDI+'s MeasureString did: the advance width plus
    /// a sixth of an em of padding each side, and the font's full line height.
    /// </summary>
    private static (float Width, float Height) MeasureChipText(string text, SKFont font)
    {
        var width = font.MeasureText(text) + font.Size / 3f;
        var height = font.Spacing;
        return (width, height);
    }

    private static void DrawChip(SKCanvas graphics, SKFont font, string text, SKRect chip)
    {
        using var chipBrush = Fill(ChipFillColor);
        using var chipPen = Stroke(ChipBorderColor, 1f);
        using var textBrush = Fill(ChipTextColor);
        graphics.DrawRect(chip, chipBrush);
        graphics.DrawRect(chip, chipPen);

        // GDI+ placed text by its top-left plus a sixth of an em; Skia places by baseline.
        var x = chip.Left + 4f + font.Size / 6f;
        var baseline = chip.Top + 1f - font.Metrics.Ascent;
        graphics.DrawText(text, x, baseline, SKTextAlign.Left, font, textBrush);
    }

    private static float ToImageX(GridView view, double offsetFromRegionLeft, double scale, int imageWidth)
    {
        var x = (float)(offsetFromRegionLeft * scale);
        return Math.Clamp(x, 0.5f, imageWidth - 0.5f);
    }

    private static float ToImageY(GridView view, double offsetFromRegionTop, double scale, int imageHeight)
    {
        var y = (float)(offsetFromRegionTop * scale);
        return Math.Clamp(y, 0.5f, imageHeight - 0.5f);
    }
}
