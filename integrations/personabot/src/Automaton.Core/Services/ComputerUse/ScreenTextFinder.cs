using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.Versioning;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// One piece of text found on screen, clickable by its label.
/// </summary>
public sealed class TextTarget
{
    /// <summary>"T1", "T2" … — never a grid label, whose rows stop at M.</summary>
    public string Label { get; set; } = string.Empty;

    public string Text { get; set; } = string.Empty;

    public string LineText { get; set; } = string.Empty;

    public ScreenRect Bounds { get; set; }

    /// <summary>The middle of the matched text, which is where a click on the label lands.</summary>
    public ScreenPoint Point { get; set; }

    public double Similarity { get; set; } = 1;

    /// <summary>The window the text is showing in, when it is a listed window.</summary>
    public DesktopWindowInfo? Window { get; set; }

    public bool InForegroundWindow { get; set; }
}

/// <summary>The outcome of one text search.</summary>
public sealed class ScreenTextSearch
{
    public string Query { get; set; } = string.Empty;

    public ScreenRect Region { get; set; }

    /// <summary>Every line read, outside Automaton's own windows, in screen coordinates.</summary>
    public IReadOnlyList<OcrTextLine> Lines { get; set; } = Array.Empty<OcrTextLine>();

    /// <summary>The matches, numbered, at most <see cref="ScreenTextFinder.MaxTargets"/>.</summary>
    public IReadOnlyList<TextTarget> Targets { get; set; } = Array.Empty<TextTarget>();

    /// <summary>All matches, including any beyond the numbered ones.</summary>
    public int TotalMatches { get; set; }

    /// <summary>Matches left out because they were showing inside Automaton's own windows.</summary>
    public int IgnoredInAutomaton { get; set; }

    public TimeSpan Elapsed { get; set; }

    /// <summary>True when the first read found nothing and the screen was read again.</summary>
    public bool Retried { get; set; }

    public bool Approximate => Targets.Count > 0 && Targets[0].Similarity < 1;

    /// <summary>
    /// When nothing matched: lines sharing a word of three letters or more with the
    /// query, so the model can see what the screen actually says and search for that.
    /// </summary>
    public IReadOnlyList<OcrTextLine> LinesSharingAWord(int most)
    {
        var words = TextMatcher.Loose(Query)
            .Split(' ', StringSplitOptions.RemoveEmptyEntries)
            .Where(w => w.Length >= 3)
            .ToList();

        if (words.Count == 0)
        {
            return Array.Empty<OcrTextLine>();
        }

        return Lines
            .Where(line => words.Any(w => TextMatcher.Loose(line.Text).Contains(w, StringComparison.Ordinal)))
            .Take(most)
            .ToList();
    }
}

/// <summary>
/// Finds text on screen and turns each match into a click target.
///
/// The screen is read with Windows OCR, matched with <see cref="TextMatcher"/>, and each
/// match is checked against the window really showing at its middle. That check does
/// two jobs: it names the window the text is in, which tells two identical "Install"
/// buttons apart, and it drops matches inside Automaton's own windows — the chat is
/// usually on screen, and it contains the very words the model is searching for.
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class ScreenTextFinder
{
    /// <summary>Numbered matches returned. Past this, the region is too broad to be useful.</summary>
    public const int MaxTargets = 10;

    /// <summary>
    /// Regions up to this long edge are read at double size. Windows OCR misses small
    /// UI text at 100% scaling; doubling a 1080p screen costs about what reading a 4K one
    /// does, which measured a quarter of a second.
    /// </summary>
    private const int UpscaleBelowEdge = 2048;

    /// <summary>The usual enlargement for regions under <see cref="UpscaleBelowEdge"/>.</summary>
    public const int DefaultUpscale = 2;

    private int? _maxImageDimension;

    private readonly IComputerScreen _capture;
    private readonly IComputerWindows _windows;
    private readonly int _upscale;
    private readonly ITextRecognizer _recognizer;

    /// <param name="upscale">
    /// How much to enlarge a region before reading it. The VPS draws its text at about half the
    /// pixel size of Paul's monitors: at 2x Windows OCR read its "Home" icon as "HOfl", at 4x
    /// correctly (22 Oct 2026).
    /// </param>
    /// <param name="recognizer">
    /// Reads text from a picture. Windows OCR when omitted on Windows; other platforms must pass one
    /// (the Android guest uses Tesseract inside its Linux rootfs).
    /// </param>
    public ScreenTextFinder(IComputerScreen capture, IComputerWindows windows, int upscale = DefaultUpscale, ITextRecognizer? recognizer = null)
    {
        _capture = capture ?? throw new ArgumentNullException(nameof(capture));
        _windows = windows ?? throw new ArgumentNullException(nameof(windows));
        _recognizer = recognizer ?? TextRecognizers.Default();
        if (upscale < 1)
        {
            throw new ArgumentOutOfRangeException(nameof(upscale), upscale, "The upscale must be at least 1.");
        }

        _upscale = upscale;
    }

    /// <summary>
    /// Pause before the one retry a no-match gets. A page mid-transition — a header
    /// fading in, a button repainting — reads as nothing for a moment. An INSTALL button
    /// that was plainly on screen went unread twice in one run and could not be
    /// reproduced afterwards; a second look a beat later is the cheapest defence, and
    /// it only ever costs anything on the failure path.
    /// </summary>
    private static readonly TimeSpan RetryDelay = TimeSpan.FromMilliseconds(300);

    public ScreenTextSearch Find(string query, ScreenRect region)
    {
        AutomatonDebug.Log($"ScreenTextFinder.Find: Entry - query='{query}', region={region}");

        if (string.IsNullOrWhiteSpace(query))
        {
            throw new ArgumentException("The text to find is required.", nameof(query));
        }

        var clock = Stopwatch.StartNew();
        var search = FindOnce(query, region);

        if (search.TotalMatches == 0)
        {
            Thread.Sleep(RetryDelay);
            search = FindOnce(query, region);
            search.Retried = true;
        }

        search.Elapsed = clock.Elapsed;

        AutomatonDebug.Log($"ScreenTextFinder.Find: Exit - {search.TotalMatches} match(es){(search.Approximate ? " (approximate)" : string.Empty)}, retried={search.Retried}, {clock.ElapsedMilliseconds}ms");
        return search;
    }

    private ScreenTextSearch FindOnce(string query, ScreenRect region)
    {
        var clock = Stopwatch.StartNew();

        using var bitmap = _capture.CaptureRegion(region);
        var scale = ChooseScale(region, _upscale);

        IReadOnlyList<OcrTextLine> read;
        if (Math.Abs(scale - 1) < 0.001)
        {
            read = _recognizer.Recognize(bitmap);
        }
        else
        {
            using var resized = Resize(bitmap, scale);
            read = _recognizer.Recognize(resized);
        }

        var self = (uint)Environment.ProcessId;
        var windows = _windows.Capture();

        // Lines inside Automaton's windows are set aside before matching, not filtered
        // out after: the matcher only falls back to close readings when nothing matches
        // exactly, and an exact hit in the chat must not stop it looking properly at the
        // app. It also keeps chat text out of the "lines sharing a word" hints.
        var lines = new List<OcrTextLine>();
        var automatonLines = new List<OcrTextLine>();

        foreach (var line in read.Select(line => ToScreen(line, region, scale)).Where(line => line.Words.Count > 0))
        {
            var inAutomaton = _windows.TopLevelWindowAt(Centre(Union(line.Words))).ProcessId == self;
            (inAutomaton ? automatonLines : lines).Add(line);
        }

        var matches = TextMatcher.Find(lines, query);
        var ignored = automatonLines.Count == 0 ? 0 : TextMatcher.Find(automatonLines, query).Count;

        var targets = matches
            .Select(match =>
            {
                var bounds = ToScreenRect(match.Bounds);
                var point = Centre(match.Bounds);
                var (handle, _) = _windows.TopLevelWindowAt(point);

                return new TextTarget
                {
                    Text = match.Text,
                    LineText = match.LineText,
                    Bounds = bounds,
                    Point = point,
                    Similarity = match.Similarity,
                    Window = windows.Windows.FirstOrDefault(w => w.Handle == handle),
                    InForegroundWindow = handle != IntPtr.Zero && handle == windows.ForegroundHandle
                };
            })
            // Close readings best first; otherwise the app in front first, then reading order.
            .OrderByDescending(t => t.Similarity)
            .ThenByDescending(t => t.InForegroundWindow)
            .ThenBy(t => t.Bounds.Y / 16)
            .ThenBy(t => t.Bounds.X)
            .ToList();

        for (var i = 0; i < targets.Count; i++)
        {
            targets[i].Label = $"T{i + 1}";
        }

        var search = new ScreenTextSearch
        {
            Query = query.Trim(),
            Region = region,
            Lines = lines,
            Targets = targets.Take(MaxTargets).ToList(),
            TotalMatches = targets.Count,
            IgnoredInAutomaton = ignored,
            Elapsed = clock.Elapsed
        };

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ScreenTextFinder] FindOnce: {lines.Count} line(s), {search.TotalMatches} match(es), {search.IgnoredInAutomaton} inside Automaton, scale {scale:0.##}, {clock.ElapsedMilliseconds}ms");
        return search;
    }

    private double ChooseScale(ScreenRect region, int upscale)
    {
        _maxImageDimension ??= _recognizer.MaxImageDimension;

        var longest = Math.Max(region.Width, region.Height);

        if (longest <= UpscaleBelowEdge && longest * upscale <= _maxImageDimension)
        {
            return upscale;
        }

        if (longest <= UpscaleBelowEdge && longest * DefaultUpscale <= _maxImageDimension)
        {
            return DefaultUpscale;
        }

        return longest > _maxImageDimension ? (double)_maxImageDimension.Value / longest : 1;
    }

    private static SkiaSharp.SKBitmap Resize(SkiaSharp.SKBitmap source, double scale)
    {
        var width = Math.Max(1, (int)Math.Round(source.Width * scale));
        var height = Math.Max(1, (int)Math.Round(source.Height * scale));
        return SkiaImages.Resize(source, width, height);
    }

    private static OcrTextLine ToScreen(OcrTextLine line, ScreenRect region, double scale)
    {
        return new OcrTextLine
        {
            Text = line.Text,
            Words = line.Words.Select(word => new OcrTextWord
            {
                Text = word.Text,
                Bounds = new RectangleF(
                    (float)(region.X + word.Bounds.X / scale),
                    (float)(region.Y + word.Bounds.Y / scale),
                    (float)(word.Bounds.Width / scale),
                    (float)(word.Bounds.Height / scale))
            }).ToList()
        };
    }

    private static RectangleF Union(IReadOnlyList<OcrTextWord> words)
    {
        var bounds = words[0].Bounds;
        foreach (var word in words.Skip(1))
        {
            bounds = RectangleF.Union(bounds, word.Bounds);
        }

        return bounds;
    }

    private static ScreenPoint Centre(RectangleF bounds) =>
        new((int)Math.Round(bounds.X + bounds.Width / 2), (int)Math.Round(bounds.Y + bounds.Height / 2));

    private static ScreenRect ToScreenRect(RectangleF bounds) =>
        new((int)Math.Floor(bounds.X), (int)Math.Floor(bounds.Y), (int)Math.Ceiling(bounds.Width), (int)Math.Ceiling(bounds.Height));
}
