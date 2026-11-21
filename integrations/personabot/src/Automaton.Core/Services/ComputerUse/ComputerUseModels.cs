using System.Text.Json.Serialization;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// A rectangle in physical screen coordinates.
///
/// Screen space is the virtual desktop, whose origin can be negative when a
/// secondary monitor sits above or to the left of the primary one. Every rect in
/// the computer-use stack is in this space; image-space coordinates never leave
/// the renderer.
/// </summary>
public readonly struct ScreenRect
{
    public ScreenRect(int x, int y, int width, int height)
    {
        X = x;
        Y = y;
        Width = width;
        Height = height;
    }

    [JsonPropertyName("x")]
    public int X { get; }

    [JsonPropertyName("y")]
    public int Y { get; }

    [JsonPropertyName("width")]
    public int Width { get; }

    [JsonPropertyName("height")]
    public int Height { get; }

    [JsonIgnore]
    public int Right => X + Width;

    [JsonIgnore]
    public int Bottom => Y + Height;

    [JsonIgnore]
    public bool IsEmpty => Width <= 0 || Height <= 0;

    /// <summary>
    /// Clamps this rect so it lies inside <paramref name="bounds"/>, preserving the
    /// size where possible and shrinking only when the rect is larger than bounds.
    ///
    /// Used when a zoom region centred on an edge intersection would otherwise run
    /// off the screen: the model asked for a region around a point, and sliding the
    /// region back inside keeps that point visible where clipping it would not.
    /// </summary>
    public ScreenRect ClampInside(ScreenRect bounds)
    {
        var width = Math.Min(Width, bounds.Width);
        var height = Math.Min(Height, bounds.Height);

        var x = Math.Max(bounds.X, Math.Min(X, bounds.Right - width));
        var y = Math.Max(bounds.Y, Math.Min(Y, bounds.Bottom - height));

        return new ScreenRect(x, y, width, height);
    }

    public override string ToString() => $"{Width}x{Height}+{X}+{Y}";
}

/// <summary>
/// A point in physical screen coordinates.
/// </summary>
public readonly struct ScreenPoint
{
    public ScreenPoint(int x, int y)
    {
        X = x;
        Y = y;
    }

    [JsonPropertyName("x")]
    public int X { get; }

    [JsonPropertyName("y")]
    public int Y { get; }

    public override string ToString() => $"({X},{Y})";
}

/// <summary>
/// One physical display.
/// </summary>
public sealed class MonitorInfo
{
    /// <summary>1-based index, the form the model refers to a monitor by ("monitor:1").</summary>
    [JsonPropertyName("index")]
    public int Index { get; set; }

    [JsonPropertyName("deviceName")]
    public string DeviceName { get; set; } = string.Empty;

    /// <summary>Full monitor rect in virtual-desktop coordinates.</summary>
    [JsonPropertyName("bounds")]
    public ScreenRect Bounds { get; set; }

    /// <summary>Monitor rect excluding the taskbar and other appbars.</summary>
    [JsonPropertyName("workArea")]
    public ScreenRect WorkArea { get; set; }

    [JsonPropertyName("isPrimary")]
    public bool IsPrimary { get; set; }
}

/// <summary>
/// One labelled grid intersection: the code the model sees and the screen point
/// it resolves to.
/// </summary>
public sealed class GridLabel
{
    /// <summary>Row letter plus column number, e.g. "C4".</summary>
    [JsonPropertyName("label")]
    public string Label { get; set; } = string.Empty;

    /// <summary>0-based row of the intersection.</summary>
    [JsonPropertyName("row")]
    public int Row { get; set; }

    /// <summary>0-based column of the intersection.</summary>
    [JsonPropertyName("column")]
    public int Column { get; set; }

    /// <summary>The exact screen point a click on this label lands on.</summary>
    [JsonPropertyName("point")]
    public ScreenPoint Point { get; set; }
}

/// <summary>
/// The annotated view handed to the model for one step of a grid descent.
///
/// <see cref="Labels"/> is the whole contract: the model picks a label, the host
/// looks the screen point back up here. The model is never given a coordinate and
/// never asked for one.
/// </summary>
public sealed class GridView
{
    /// <summary>The screen region this view covers.</summary>
    [JsonPropertyName("region")]
    public ScreenRect Region { get; set; }

    [JsonPropertyName("rows")]
    public int Rows { get; set; }

    [JsonPropertyName("columns")]
    public int Columns { get; set; }

    /// <summary>Label code to intersection, keyed case-insensitively.</summary>
    [JsonPropertyName("labels")]
    public Dictionary<string, GridLabel> Labels { get; set; } =
        new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Encoded image of the region with the grid drawn over it.</summary>
    [JsonIgnore]
    public byte[] ImageBytes { get; set; } = Array.Empty<byte>();

    /// <summary>IANA media type of <see cref="ImageBytes"/>.</summary>
    [JsonPropertyName("mediaType")]
    public string MediaType { get; set; } = "image/jpeg";

    /// <summary>Output image size in pixels.</summary>
    [JsonPropertyName("imageWidth")]
    public int ImageWidth { get; set; }

    [JsonPropertyName("imageHeight")]
    public int ImageHeight { get; set; }

    /// <summary>
    /// Output pixels per screen pixel. Above 1 the view is magnified; the first
    /// full-screen view is normally below 1 because the desktop is downscaled to fit
    /// the target image size.
    /// </summary>
    [JsonPropertyName("magnification")]
    public double Magnification { get; set; }

    /// <summary>
    /// Screen pixels between adjacent intersections. This is the descent's precision:
    /// once it drops below the size of the target, an intersection lands on it.
    /// </summary>
    [JsonPropertyName("screenPitchX")]
    public double ScreenPitchX { get; set; }

    [JsonPropertyName("screenPitchY")]
    public double ScreenPitchY { get; set; }

    /// <summary>
    /// Resolves a label the model supplied. Returns null when the label is not part
    /// of this view, which is the case the caller must report back rather than
    /// guessing a nearby point.
    /// </summary>
    public GridLabel? Resolve(string? label)
    {
        if (string.IsNullOrWhiteSpace(label))
        {
            return null;
        }

        return Labels.TryGetValue(label.Trim(), out var found) ? found : null;
    }

    /// <summary>
    /// The label whose point is closest to a screen point. Used after a zoom to tell
    /// the model which of the fresh labels now sits on the point it zoomed around.
    /// </summary>
    public GridLabel? NearestLabel(ScreenPoint point)
    {
        GridLabel? nearest = null;
        var best = double.MaxValue;

        foreach (var label in Labels.Values)
        {
            var dx = (double)label.Point.X - point.X;
            var dy = (double)label.Point.Y - point.Y;
            var distance = dx * dx + dy * dy;

            if (distance < best)
            {
                best = distance;
                nearest = label;
            }
        }

        return nearest;
    }

    /// <summary>
    /// Rounds a grid division count up to an even number.
    ///
    /// A zoom crops a region centred on a point, so the point lands in the middle of the
    /// new view — and with an odd number of cells along an axis, the middle of that axis
    /// falls between two marker lines. The default 3 rows did exactly that: the point the
    /// model zoomed around ended up halfway between two markers with nothing on it, which
    /// read as the zoom having gone somewhere other than where the label was.
    /// </summary>
    public static int EvenDivisions(int divisions)
    {
        return divisions % 2 == 0 ? divisions : divisions + 1;
    }

    /// <summary>
    /// The region one cell wide and one cell tall centred on a label, which is what
    /// a zoom step crops.
    ///
    /// One cell rather than the 2x2 block of neighbouring cells: cropping a single
    /// cell makes the magnification factor equal to the column count, so a 4-column
    /// grid gains 4x per step instead of the 1.5x a 2x2 crop would give.
    /// </summary>
    public ScreenRect CellAround(GridLabel label, ScreenRect bounds)
    {
        ArgumentNullException.ThrowIfNull(label);

        var width = Math.Max(8, (int)Math.Round(ScreenPitchX));
        var height = Math.Max(8, (int)Math.Round(ScreenPitchY));

        var rect = new ScreenRect(
            label.Point.X - width / 2,
            label.Point.Y - height / 2,
            width,
            height);

        return rect.ClampInside(bounds);
    }
}

/// <summary>
/// Wire format for a rendered grid view.
/// </summary>
public enum GridImageEncoding
{
    /// <summary>Smaller payload; the default, because every view crosses the wire.</summary>
    Jpeg,

    /// <summary>Lossless, for eyeballing the renderer's output during development.</summary>
    Png
}

/// <summary>
/// Naming for grid intersections: row letter then column number, e.g. "C4".
///
/// Letters for one axis and digits for the other means a misread character can
/// never silently resolve to a different valid point on the wrong axis.
/// </summary>
public static class GridLabelNaming
{
    /// <summary>
    /// Builds the label for a 0-based intersection. Rows run A, B, C ... and past Z
    /// continue AA, AB, so an absurdly dense grid still produces unique codes.
    /// </summary>
    public static string Build(int row, int column)
    {
        if (row < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(row), row, "Row must not be negative.");
        }

        if (column < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(column), column, "Column must not be negative.");
        }

        return $"{RowLetters(row)}{column + 1}";
    }

    private static string RowLetters(int row)
    {
        var letters = string.Empty;
        var value = row;

        while (true)
        {
            letters = (char)('A' + (value % 26)) + letters;
            value = (value / 26) - 1;

            if (value < 0)
            {
                break;
            }
        }

        return letters;
    }
}
