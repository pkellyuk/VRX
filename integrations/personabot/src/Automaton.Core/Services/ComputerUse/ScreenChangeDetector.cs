using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// Answers "did that do anything?" after a click or keystroke.
///
/// Without it the model's only evidence was the next grid view, and a change can be
/// easy to miss in one. The case that prompted this: a click on a small list row left
/// the row looking highlighted but the main panel unchanged, and the model could not
/// tell whether it had selected the item or missed, so it clicked three more times.
/// Comparing the screen before and after turns that into a sentence — "a thin band
/// around the click changed, nothing else did".
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class ScreenChangeDetector
{
    /// <summary>How long to rest between samples while waiting for the screen to settle.</summary>
    private static readonly TimeSpan SampleInterval = TimeSpan.FromMilliseconds(150);

    /// <summary>
    /// How long to wait for anything to happen at all. Long enough for a panel that
    /// loads in a few hundred milliseconds; any longer and a click that genuinely did
    /// nothing costs more than it tells. A 4K capture takes about 100ms, so this is
    /// four or five samples.
    /// </summary>
    public static readonly TimeSpan ResponseWindow = TimeSpan.FromMilliseconds(1200);

    private readonly IComputerScreen _capture;
    private readonly IComputerWindows _windows;

    public ScreenChangeDetector(IComputerScreen capture, IComputerWindows windows)
    {
        _capture = capture ?? throw new ArgumentNullException(nameof(capture));
        _windows = windows ?? throw new ArgumentNullException(nameof(windows));
    }

    /// <summary>
    /// Takes the "before" picture: the screen, and the list of windows so the report can
    /// name any that opened, closed or came to the front.
    /// </summary>
    public ScreenSnapshot CaptureBefore()
    {
        var snapshot = Capture();
        snapshot.Windows = _windows.Capture();
        return snapshot;
    }

    /// <summary>One sample of the screen alone. Windows are only compared at the ends.</summary>
    public ScreenSnapshot Capture()
    {
        var desktop = _capture.GetVirtualDesktopBounds();
        using var bitmap = _capture.CaptureRegion(desktop);
        return ScreenSnapshot.FromBitmap(bitmap, desktop);
    }

    /// <summary>
    /// Waits for the screen to respond to an action and reports what changed.
    ///
    /// Every sample is compared both with the "before" picture, to collect what the
    /// action changed, and with the previous sample, to learn what keeps changing on
    /// its own. The second matters because a desktop is never still: a spinner in
    /// another app and a blinking caret changed on a desktop where nothing was done at
    /// all, and without telling them apart every action was reported as "6 areas
    /// changed" and the wait never settled.
    ///
    /// Settling is only trusted once something has changed. Identical frames straight
    /// after a click usually mean the app has not reacted yet, not that it never will.
    /// </summary>
    /// <param name="actionPoint">Where the pointer acted. Animation there is kept, not ignored.</param>
    /// <param name="shouldStop">Checked between samples, so the kill chord is not held up.</param>
    public ScreenChangeReport WaitForChange(ScreenSnapshot before, ScreenPoint? actionPoint, Func<bool>? shouldStop = null)
    {
        ArgumentNullException.ThrowIfNull(before);

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ScreenChangeDetector] WaitForChange: Entry - actionPoint={actionPoint?.ToString() ?? "none"}");

        var clock = Stopwatch.StartNew();
        var changes = new ScreenChangeHistory(before);
        var previous = before;
        var settled = false;
        var sawChange = false;
        var samples = 0;

        while (clock.Elapsed < ResponseWindow)
        {
            Thread.Sleep(SampleInterval);

            if (shouldStop?.Invoke() == true)
            {
                break;
            }

            var current = Capture();
            samples++;

            var unsettled = changes.Add(previous, current);
            previous = current;

            if (!sawChange)
            {
                sawChange = changes.Analyse(actionPoint).AnythingChanged;
            }
            else if (!unsettled)
            {
                settled = true;
                break;
            }
        }

        var report = changes.Analyse(actionPoint);
        report.StillChanging = sawChange && !settled;
        report.Waited = clock.Elapsed;

        if (before.Windows is not null)
        {
            report.Windows = WindowChanges.Compare(before.Windows, _windows.Capture());
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ScreenChangeDetector] WaitForChange: Exit - {report.Areas.Count} area(s), {report.ChangedFraction:P2} of the screen, {report.IgnoredSmallChanges} small change(s) away from the action left out, windows changed={report.Windows?.HasChanges.ToString() ?? "not compared"}, settled={settled}, {samples} sample(s) in {report.Waited.TotalMilliseconds:0}ms");
        return report;
    }
}

/// <summary>
/// The desktop reduced to the average colour of each small block.
///
/// Block averages rather than pixels, for two reasons. A 4K frame is 33MB of pixels
/// and a response wait takes several; the averages are a few hundred KB. And averaging
/// is a noise filter: sub-pixel anti-aliasing shifts barely move an 8x8 average, while
/// a highlighted row, a changed number or a new panel moves it a lot.
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class ScreenSnapshot
{
    public const int DefaultBlockSize = 8;

    private ScreenSnapshot(ScreenRect region, int blockSize, int blocksX, int blocksY, byte[] means)
    {
        Region = region;
        BlockSize = blockSize;
        BlocksX = blocksX;
        BlocksY = blocksY;
        Means = means;
    }

    public ScreenRect Region { get; }

    public int BlockSize { get; }

    public int BlocksX { get; }

    public int BlocksY { get; }

    public int BlockCount => BlocksX * BlocksY;

    /// <summary>B, G, R average per block, row by row.</summary>
    internal byte[] Means { get; }

    /// <summary>The windows at the same moment, on a "before" picture only.</summary>
    public WindowSnapshot? Windows { get; internal set; }

    /// <summary>GDI+ overload of <see cref="FromBitmap(SkiaSharp.SKBitmap, ScreenRect, int)"/>.</summary>
    public static ScreenSnapshot FromBitmap(Bitmap bitmap, ScreenRect region, int blockSize = DefaultBlockSize)
    {
        ArgumentNullException.ThrowIfNull(bitmap);
        using var skia = SkiaImages.FromGdi(bitmap);
        return FromBitmap(skia, region, blockSize);
    }

    /// <summary>
    /// Builds a snapshot from a bitmap covering <paramref name="region"/> exactly.
    /// </summary>
    public static ScreenSnapshot FromBitmap(SkiaSharp.SKBitmap bitmap, ScreenRect region, int blockSize = DefaultBlockSize)
    {
        ArgumentNullException.ThrowIfNull(bitmap);

        if (blockSize < 2)
        {
            throw new ArgumentOutOfRangeException(nameof(blockSize), blockSize, "Block size must be at least 2 pixels.");
        }

        var width = bitmap.Width;
        var height = bitmap.Height;
        var blocksX = (width + blockSize - 1) / blockSize;
        var blocksY = (height + blockSize - 1) / blockSize;

        var sums = new long[blocksX * blocksY * 3];
        var counts = new int[blocksX * blocksY];

        // Bgra8888 is the layout every screen hands over; anything else is converted once here.
        using var converted = bitmap.ColorType == SkiaSharp.SKColorType.Bgra8888 ? null : SkiaImages.Normalise(bitmap);
        var pixels = converted ?? bitmap;
        var stride = pixels.RowBytes;
        var all = pixels.GetPixelSpan();

        {
            // Every other pixel on both axes. A block still gets sixteen samples, which
            // is plenty for an average, and the pass is four times cheaper.
            for (var y = 0; y < height; y += 2)
            {
                var row = all.Slice(y * stride, width * 4);
                var blockRow = (y / blockSize) * blocksX;

                for (var x = 0; x < width; x += 2)
                {
                    var block = blockRow + x / blockSize;
                    var pixel = x * 4;
                    var sum = block * 3;

                    sums[sum] += row[pixel];
                    sums[sum + 1] += row[pixel + 1];
                    sums[sum + 2] += row[pixel + 2];
                    counts[block]++;
                }
            }
        }

        var means = new byte[sums.Length];

        for (var block = 0; block < counts.Length; block++)
        {
            var count = Math.Max(1, counts[block]);
            means[block * 3] = (byte)(sums[block * 3] / count);
            means[block * 3 + 1] = (byte)(sums[block * 3 + 1] / count);
            means[block * 3 + 2] = (byte)(sums[block * 3 + 2] / count);
        }

        return new ScreenSnapshot(region, blockSize, blocksX, blocksY, means);
    }
}

/// <summary>
/// What has changed since the "before" picture, and what keeps changing by itself,
/// accumulated over the samples of one response wait.
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class ScreenChangeHistory
{
    /// <summary>
    /// A block that has changed between consecutive samples this many times is
    /// animating — a spinner, a caret, a video — rather than responding once.
    /// </summary>
    public const int AnimatingChanges = 2;

    private readonly ScreenSnapshot _before;

    public ScreenChangeHistory(ScreenSnapshot before)
    {
        _before = before ?? throw new ArgumentNullException(nameof(before));
        EverChanged = new bool[before.BlockCount];
        SampleChanges = new byte[before.BlockCount];
    }

    /// <summary>Blocks that differed from the "before" picture in any sample.</summary>
    internal bool[] EverChanged { get; }

    /// <summary>Per block, how many consecutive sample pairs it changed between.</summary>
    internal byte[] SampleChanges { get; }

    /// <summary>
    /// Folds in one new sample. Returns true when that sample still shows the screen
    /// responding — a change since the last sample in a block not already known to be
    /// animating.
    /// </summary>
    public bool Add(ScreenSnapshot previous, ScreenSnapshot current)
    {
        ScreenChangeAnalysis.EnsureComparable(_before, current);
        ScreenChangeAnalysis.EnsureComparable(previous, current);

        var unsettled = false;

        for (var block = 0; block < EverChanged.Length; block++)
        {
            if (ScreenChangeAnalysis.BlockChanged(_before, current, block))
            {
                EverChanged[block] = true;
            }

            if (!ScreenChangeAnalysis.BlockChanged(previous, current, block))
            {
                continue;
            }

            // Judged on the count before this change: a block's first changes are the
            // screen responding, only its later ones are animation.
            if (SampleChanges[block] < AnimatingChanges)
            {
                unsettled = true;
            }

            if (SampleChanges[block] < byte.MaxValue)
            {
                SampleChanges[block]++;
            }
        }

        return unsettled;
    }

    public ScreenChangeReport Analyse(ScreenPoint? actionPoint)
    {
        return ScreenChangeAnalysis.Analyse(_before, EverChanged, SampleChanges, actionPoint);
    }
}

/// <summary>One connected patch of the screen that changed.</summary>
public sealed class ScreenChangeArea
{
    /// <summary>Bounding box of the changed blocks, in screen coordinates.</summary>
    public ScreenRect Bounds { get; set; }

    /// <summary>Screen pixels that actually changed, which can be far less than the bounding box.</summary>
    public long ChangedPixels { get; set; }

    /// <summary>True when it kept changing between samples, rather than changing once.</summary>
    public bool Animating { get; set; }
}

/// <summary>What changed between two snapshots.</summary>
[SupportedOSPlatform("windows")]
public sealed class ScreenChangeReport
{
    /// <summary>At most this many areas are described; the rest are summarised in one clause.</summary>
    private const int DescribedAreas = 3;

    /// <summary>Past this many areas, only where the bulk of the change was is described.</summary>
    private const int ManyAreas = 8;

    /// <summary>
    /// The box holding the largest areas that together make up nine tenths of the changed
    /// pixels. The union of every area would usually be the whole screen, because one
    /// small change far away is enough to stretch it.
    /// </summary>
    private ScreenRect BulkOfTheChange()
    {
        var total = Areas.Sum(a => a.ChangedPixels);
        var covered = 0L;
        ScreenRect? box = null;

        foreach (var area in Areas)
        {
            box = box is null ? area.Bounds : Union(box.Value, area.Bounds);
            covered += area.ChangedPixels;

            if (covered * 10 >= total * 9)
            {
                break;
            }
        }

        return box ?? Desktop;
    }

    private static ScreenRect Union(ScreenRect a, ScreenRect b)
    {
        var x = Math.Min(a.X, b.X);
        var y = Math.Min(a.Y, b.Y);
        return new ScreenRect(x, y, Math.Max(a.Right, b.Right) - x, Math.Max(a.Bottom, b.Bottom) - y);
    }

    /// <summary>How close a change must be to the action point to count as "where you acted".</summary>
    internal const int NearActionMargin = 16;

    public IReadOnlyList<ScreenChangeArea> Areas { get; set; } = Array.Empty<ScreenChangeArea>();

    public ScreenRect Desktop { get; set; }

    /// <summary>Share of the desktop that changed, 0 to 1.</summary>
    public double ChangedFraction { get; set; }

    /// <summary>Small changes away from the action — spinners, carets, clocks — left out of <see cref="Areas"/>.</summary>
    public int IgnoredSmallChanges { get; set; }

    /// <summary>True when the screen had not stopped changing by the end of the wait.</summary>
    public bool StillChanging { get; set; }

    public TimeSpan Waited { get; set; }

    /// <summary>Windows that opened, closed, retitled or came to the front; null when not compared.</summary>
    public WindowChanges? Windows { get; set; }

    /// <summary>
    /// A window change counts on its own. Focus moving to a window that was already fully
    /// visible can change almost no pixels, and "NOTHING changed" would be false.
    /// </summary>
    public bool AnythingChanged => Areas.Count > 0 || Windows?.HasChanges == true;

    /// <summary>
    /// The sentence or two the model gets. Names each changed area by position and
    /// size, and says whether it is where the action happened and whether it overlaps
    /// the view the model was looking at — the two facts that separate "it worked"
    /// from "something else moved".
    /// </summary>
    /// <param name="action">"the click", "the key press" and so on.</param>
    /// <param name="actionPoint">Where the pointer acted, if it did.</param>
    /// <param name="view">The region of the view the model was looking at, if any.</param>
    public string Describe(string action, ScreenPoint? actionPoint, ScreenRect? view)
    {
        var ignored = IgnoredSmallChanges > 0
            ? $" ({IgnoredSmallChanges} small change(s) away from where you acted — spinners, carets, clocks and the like — were left out.)"
            : string.Empty;

        if (!AnythingChanged)
        {
            var hint = actionPoint.HasValue
                ? "It may have missed, landed on something that does not respond, or the app is slow."
                : "Focus may be somewhere that ignores this input, or the app is slow.";

            return $"NOTHING on screen changed within {Waited.TotalSeconds:0.#}s of {action}. {hint} Check with cu_grid before repeating it, rather than repeating it blind.{ignored}";
        }

        var builder = new StringBuilder();

        // Windows first: "a new window 'Properties' opened" is the most direct evidence
        // an action worked, and it is what the pixel areas below would otherwise leave
        // the model to infer.
        var windows = Windows?.Describe();
        if (!string.IsNullOrEmpty(windows))
        {
            builder.Append(windows).Append(' ');
        }

        if (Areas.Count == 0)
        {
            builder.Append($"No area of the screen visibly changed after {action}.");
            builder.Append(ignored);
            return builder.ToString();
        }

        // Many areas means a whole panel or list repainted, and listing them one by one
        // is noise: typing into the Start menu produced "91 areas changed … plus 88
        // smaller changes". Say where the bulk of the change was instead.
        if (Areas.Count > ManyAreas)
        {
            builder.Append($"After {action}, {Areas.Count} areas of the screen changed — mostly within {GridOverlayRenderer.DescribePosition(BulkOfTheChange(), Desktop)}");

            var acted = actionPoint.HasValue ? Areas.FirstOrDefault(a => IsNear(a.Bounds, actionPoint.Value)) : null;
            builder.Append(acted is null
                ? (actionPoint.HasValue ? ". Nothing changed at the point you acted on itself." : ".")
                : $", including where you acted ({GridOverlayRenderer.DescribePosition(acted.Bounds, Desktop)}).");

            if (StillChanging)
            {
                builder.Append(" The screen was still changing when checked, so something may be loading or animating.");
            }

            builder.Append(" changed:1 is the largest area; cu_grid region=\"changed:1\" puts labels straight onto it.");
            builder.Append(ignored);
            return builder.ToString();
        }

        // No overall percentage here: each area's description already gives its share,
        // measured by bounding box, and a total measured by changed pixels disagreed
        // with the sum of those, which read as an error.
        builder.Append($"After {action}, {Areas.Count} area(s) of the screen changed:");

        for (var i = 0; i < Math.Min(DescribedAreas, Areas.Count); i++)
        {
            var area = Areas[i];
            var tags = new List<string>();

            if (actionPoint.HasValue && IsNear(area.Bounds, actionPoint.Value))
            {
                tags.Add("where you acted");
            }

            if (view.HasValue && Overlaps(area.Bounds, view.Value))
            {
                tags.Add("inside the view you were looking at");
            }

            if (area.Animating)
            {
                tags.Add("still animating");
            }

            var tagText = tags.Count == 0 ? string.Empty : $" [{string.Join(", ", tags)}]";
            builder.Append($" ({i + 1}) {GridOverlayRenderer.DescribePosition(area.Bounds, Desktop)}{tagText}.");
        }

        if (Areas.Count > DescribedAreas)
        {
            builder.Append($" Plus {Areas.Count - DescribedAreas} smaller change(s) elsewhere.");
        }

        if (StillChanging)
        {
            builder.Append(" The screen was still changing when checked, so something may be loading or animating.");
        }

        if (actionPoint.HasValue && !Areas.Any(a => IsNear(a.Bounds, actionPoint.Value)))
        {
            builder.Append(" Nothing changed at the point you acted on itself.");
        }

        builder.Append(" To act on one of these areas, cu_grid region=\"changed:N\" puts labels straight onto area N.");

        builder.Append(ignored);
        return builder.ToString();
    }

    internal static bool IsNear(ScreenRect rect, ScreenPoint point) =>
        point.X >= rect.X - NearActionMargin && point.X < rect.Right + NearActionMargin &&
        point.Y >= rect.Y - NearActionMargin && point.Y < rect.Bottom + NearActionMargin;

    private static bool Overlaps(ScreenRect a, ScreenRect b) =>
        a.X < b.Right && b.X < a.Right && a.Y < b.Bottom && b.Y < a.Bottom;
}

/// <summary>
/// The comparison itself, kept free of any capture so it can be tested on bitmaps.
/// </summary>
[SupportedOSPlatform("windows")]
public static class ScreenChangeAnalysis
{
    /// <summary>
    /// How far a block's average colour must move, on its most-changed channel, to
    /// count. Raw captures carry no compression noise, so this can be low — and it has
    /// to be, because the changes that matter most are often the subtlest: a selected
    /// row in a dark theme can differ from its neighbours by twenty levels.
    /// </summary>
    public const int ChangeThreshold = 16;

    /// <summary>
    /// Changes no larger than this on either side count as small: a spinner, a caret,
    /// a clock's digits, a badge count — at high DPI too.
    /// </summary>
    public const int SmallChangeMaxEdge = 64;

    /// <summary>
    /// Changed blocks this many blocks apart join into one area, so the glyphs of a
    /// changed line of text read as one change rather than twenty.
    /// </summary>
    private const int JoinDistance = 2;

    /// <summary>
    /// A single before/after comparison, with no history of which blocks animate. For
    /// tests, and for any caller that has only two pictures.
    /// </summary>
    public static ScreenChangeReport Compare(ScreenSnapshot before, ScreenSnapshot after, ScreenPoint? actionPoint = null)
    {
        EnsureComparable(before, after);

        var changed = new bool[before.BlockCount];
        for (var block = 0; block < changed.Length; block++)
        {
            changed[block] = BlockChanged(before, after, block);
        }

        return Analyse(before, changed, sampleChanges: null, actionPoint);
    }

    /// <summary>
    /// Groups changed blocks into areas and decides which to report.
    /// </summary>
    /// <param name="changed">Blocks that differ from the "before" picture.</param>
    /// <param name="sampleChanges">
    /// Per block, how often it changed between consecutive samples; null when there was
    /// only one comparison and so no way to tell animation from response.
    /// </param>
    public static ScreenChangeReport Analyse(ScreenSnapshot before, bool[] changed, byte[]? sampleChanges, ScreenPoint? actionPoint)
    {
        ArgumentNullException.ThrowIfNull(before);
        ArgumentNullException.ThrowIfNull(changed);

        var blocksX = before.BlocksX;
        var blocksY = before.BlocksY;
        var region = before.Region;
        var size = before.BlockSize;

        var areas = new List<ScreenChangeArea>();
        var visited = new bool[changed.Length];
        var queue = new Queue<int>();
        var ignored = 0;

        for (var start = 0; start < changed.Length; start++)
        {
            if (!changed[start] || visited[start])
            {
                continue;
            }

            var minX = int.MaxValue;
            var minY = int.MaxValue;
            var maxX = int.MinValue;
            var maxY = int.MinValue;
            var blocks = 0;
            var animatingBlocks = 0;

            visited[start] = true;
            queue.Enqueue(start);

            while (queue.Count > 0)
            {
                var block = queue.Dequeue();
                var bx = block % blocksX;
                var by = block / blocksX;

                blocks++;
                if (sampleChanges is not null && sampleChanges[block] >= ScreenChangeHistory.AnimatingChanges)
                {
                    animatingBlocks++;
                }

                minX = Math.Min(minX, bx);
                minY = Math.Min(minY, by);
                maxX = Math.Max(maxX, bx);
                maxY = Math.Max(maxY, by);

                // Neighbours within the join distance, so nearby changes merge without
                // a separate dilation pass.
                for (var ny = Math.Max(0, by - JoinDistance); ny <= Math.Min(blocksY - 1, by + JoinDistance); ny++)
                {
                    for (var nx = Math.Max(0, bx - JoinDistance); nx <= Math.Min(blocksX - 1, bx + JoinDistance); nx++)
                    {
                        var neighbour = ny * blocksX + nx;
                        if (changed[neighbour] && !visited[neighbour])
                        {
                            visited[neighbour] = true;
                            queue.Enqueue(neighbour);
                        }
                    }
                }
            }

            var x = region.X + minX * size;
            var y = region.Y + minY * size;
            var right = Math.Min(region.Right, region.X + (maxX + 1) * size);
            var bottom = Math.Min(region.Bottom, region.Y + (maxY + 1) * size);

            var area = new ScreenChangeArea
            {
                Bounds = new ScreenRect(x, y, right - x, bottom - y),
                ChangedPixels = (long)blocks * size * size,
                Animating = animatingBlocks * 2 >= blocks && animatingBlocks > 0
            };

            // A desktop is never still. On one where nothing was done at all, a spinner,
            // a caret and a few digits of text in other apps all changed, and reporting
            // them buried the one change that mattered. So small changes are left out
            // when they are away from the action — but what counts as "away" depends on
            // the action:
            //  - A pointer action has a point. Its response is at that point or large,
            //    so any small change elsewhere is noise, animating or not.
            //  - Keyboard input has no point, and a typed character is itself a small
            //    change somewhere unknown. Only small changes that animate are dropped.
            // Anything small AT the action point is always kept: a caret appearing in a
            // clicked text field is how that click shows it worked.
            var small = area.Bounds.Width <= SmallChangeMaxEdge && area.Bounds.Height <= SmallChangeMaxEdge;
            var atAction = actionPoint.HasValue && ScreenChangeReport.IsNear(area.Bounds, actionPoint.Value);

            if (small && !atAction && (actionPoint.HasValue || area.Animating))
            {
                ignored++;
                continue;
            }

            areas.Add(area);
        }

        var totalPixels = Math.Max(1L, (long)region.Width * region.Height);

        return new ScreenChangeReport
        {
            Areas = areas.OrderByDescending(a => a.ChangedPixels).ToList(),
            Desktop = region,
            ChangedFraction = Math.Min(1.0, (double)areas.Sum(a => a.ChangedPixels) / totalPixels),
            IgnoredSmallChanges = ignored
        };
    }

    internal static bool BlockChanged(ScreenSnapshot a, ScreenSnapshot b, int block)
    {
        var offset = block * 3;
        return Math.Abs(a.Means[offset] - b.Means[offset]) > ChangeThreshold ||
               Math.Abs(a.Means[offset + 1] - b.Means[offset + 1]) > ChangeThreshold ||
               Math.Abs(a.Means[offset + 2] - b.Means[offset + 2]) > ChangeThreshold;
    }

    internal static void EnsureComparable(ScreenSnapshot a, ScreenSnapshot b)
    {
        ArgumentNullException.ThrowIfNull(a);
        ArgumentNullException.ThrowIfNull(b);

        if (a.BlocksX != b.BlocksX || a.BlocksY != b.BlocksY || a.BlockSize != b.BlockSize)
        {
            // A monitor was attached, removed or rescaled between the two captures.
            // There is no honest block-by-block comparison across that.
            throw new InvalidOperationException(
                $"The desktop changed size between captures ({a.Region} to {b.Region}), so the screens cannot be compared.");
        }
    }
}
