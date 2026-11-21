using System.Text;
using Automaton.Core.Services;

namespace Automaton.Core.Streaming;

/// <summary>
/// Watches a model's reasoning as it streams and reports the moment it starts
/// restating itself, so the caller can abort the response instead of paying for
/// a spiral that runs to the token limit.
///
/// Why this is separate from the post-hoc <c>LoopDetector</c> (Automaton.AI):
/// the post-hoc check only runs between tool round trips, so a single response
/// that loops for thousands of tokens is fully generated, streamed and billed
/// before anything notices. This runs per delta.
///
/// Why this is NOT part of <see cref="LlmStreamApplier"/>: the applier's contract
/// is "same bytes in => same state out", which the XCHAT record-and-replay
/// convergence depends on. An abort decision is a property of the *live* IO loop
/// (the replay side simply replays fewer recorded lines), so it lives in the loop
/// and never in the apply step.
///
/// Detection is the same shingle-hashing idea used for post-hoc analysis: hash
/// every overlapping window of <see cref="ShingleSize"/> words and count how
/// often a window recurs. Thresholds here are deliberately stricter than the
/// post-hoc ones because the consequence is destructive — a false positive kills
/// a legitimate response, where post-hoc it only injects a nudge.
///
/// Only reasoning text is analyzed, never the visible answer. Answer content is
/// legitimately repetitive all the time (tables, boilerplate, generated code) and
/// must not be abortable.
/// </summary>
public sealed class StreamingLoopMonitor
{
    /// <summary>Consecutive words forming one shingle.</summary>
    private const int ShingleSize = 12;

    /// <summary>
    /// Reasoning tokens that must accumulate before an abort is even considered.
    /// Short reasoning cannot carry a trustworthy repetition signal, and aborting
    /// early would be the most damaging kind of false positive.
    /// </summary>
    private const int MinTokensBeforeAbort = 1_000;

    /// <summary>
    /// Fraction of shingle positions that must be repeats of an earlier shingle.
    /// </summary>
    private const double RepeatRatioThreshold = 0.75;

    /// <summary>
    /// A shingle must recur at least this many times before the ratio alone can
    /// abort. Restating something exactly once — a tool inventory listed twice, a
    /// command block quoted then repeated — scores around 50% with a max repeat of
    /// 2, which is ordinary writing rather than a loop. A genuine spiral drives
    /// the same longer phrase through several occurrences.
    /// </summary>
    private const int MinShingleRepeatForRatio = 5;

    /// <summary>
    /// Recurrences of a single shingle that declare a loop on their own.
    /// </summary>
    private const int MaxShingleRepeatThreshold = 16;

    /// <summary>
    /// Upper bound on tracked shingles. A runaway response would otherwise grow
    /// the table without limit. Past this point analysis stops rather than
    /// continuing to allocate — by then a loop would already have been reported.
    /// </summary>
    private const int MaxTrackedShingles = 50_000;

    /// <summary>Environment kill switch. Set to "1"/"true" to disable aborts.</summary>
    public const string DisableEnvVar = "AUTOMATON_DISABLE_LOOP_ABORT";

    private readonly Dictionary<string, int> _shingleCounts = new();
    private readonly string[] _window = new string[ShingleSize];
    private readonly StringBuilder _currentWord = new();
    private readonly ThinkSpanTracker _thinkSpans = new();
    private readonly bool _enabled;

    private int _windowFilled;
    private int _windowHead;
    private int _tokenCount;
    private int _shingleTotal;
    private int _repeatedPositions;
    private int _maxShingleRepeat;
    private bool _trackingCapped;

    public StreamingLoopMonitor()
    {
        var disable = Environment.GetEnvironmentVariable(DisableEnvVar);
        _enabled = !string.Equals(disable, "1", StringComparison.OrdinalIgnoreCase)
            && !string.Equals(disable, "true", StringComparison.OrdinalIgnoreCase);

        if (!_enabled)
        {
            AutomatonDebug.Log($"StreamingLoopMonitor: Disabled via {DisableEnvVar}={disable}");
        }
    }

    /// <summary>True once a loop has been reported. Latches — never clears.</summary>
    public bool LoopDetected { get; private set; }

    /// <summary>Human-readable reason, set alongside <see cref="LoopDetected"/>.</summary>
    public string? Diagnosis { get; private set; }

    /// <summary>Reasoning tokens observed so far.</summary>
    public int TokenCount => _tokenCount;

    /// <summary>
    /// Observe an out-of-band reasoning delta. All of it is reasoning by
    /// definition. Returns true only on the delta that first trips detection.
    /// </summary>
    public bool ObserveReasoning(string? delta)
    {
        if (string.IsNullOrEmpty(delta))
        {
            return false;
        }

        return Observe(delta);
    }

    /// <summary>
    /// Observe a content delta, analyzing only the parts inside inline
    /// <c>&lt;think&gt;</c> spans. Models that carry reasoning inline put it here;
    /// everything outside the tags is answer text and is ignored.
    ///
    /// Callers must not route content through this once the stream is known to
    /// deliver reasoning out-of-band — those models mirror the same reasoning on
    /// both channels, and counting it twice would manufacture a false loop.
    /// </summary>
    public bool ObserveContent(string? delta)
    {
        if (string.IsNullOrEmpty(delta))
        {
            return false;
        }

        var thinkText = _thinkSpans.ExtractThinkText(delta);
        if (thinkText.Length == 0)
        {
            return false;
        }

        return Observe(thinkText);
    }

    private bool Observe(string text)
    {
        if (!_enabled || LoopDetected || _trackingCapped)
        {
            return false;
        }

        var tripped = false;
        for (var i = 0; i < text.Length; i++)
        {
            var c = char.ToLowerInvariant(text[i]);
            if (char.IsLetterOrDigit(c) || c == '_')
            {
                _currentWord.Append(c);
                continue;
            }

            if (CompleteWord())
            {
                tripped = true;
                break;
            }
        }

        return tripped;
    }

    /// <summary>
    /// Closes the in-progress word and folds it into the rolling window.
    /// Returns true if adding it tripped detection.
    /// </summary>
    private bool CompleteWord()
    {
        if (_currentWord.Length < 2)
        {
            // Sub-two-character runs are dropped, matching the post-hoc tokenizer.
            _currentWord.Clear();
            return false;
        }

        var token = _currentWord.ToString();
        _currentWord.Clear();
        _tokenCount++;

        _window[_windowHead] = token;
        _windowHead = (_windowHead + 1) % ShingleSize;
        if (_windowFilled < ShingleSize)
        {
            _windowFilled++;
            if (_windowFilled < ShingleSize)
            {
                return false; // Not enough words for a full shingle yet.
            }
        }

        return RecordShingle();
    }

    /// <summary>
    /// Hashes the current window and folds it into the running counts, then
    /// evaluates the thresholds.
    /// </summary>
    private bool RecordShingle()
    {
        var builder = new StringBuilder();
        for (var k = 0; k < ShingleSize; k++)
        {
            if (k > 0)
            {
                builder.Append(' ');
            }

            // _windowHead points at the oldest slot once the window is full.
            builder.Append(_window[(_windowHead + k) % ShingleSize]);
        }

        var shingle = builder.ToString();
        _shingleTotal++;

        var seen = _shingleCounts.GetValueOrDefault(shingle);
        if (seen == 0 && _shingleCounts.Count >= MaxTrackedShingles)
        {
            _trackingCapped = true;
            AutomatonDebug.LogWarning($"StreamingLoopMonitor: Tracking cap reached at {MaxTrackedShingles} shingles ({_tokenCount} reasoning tokens); stopping analysis for this stream.");
            return false;
        }

        _shingleCounts[shingle] = seen + 1;
        if (seen > 0)
        {
            _repeatedPositions++;
        }

        if (seen + 1 > _maxShingleRepeat)
        {
            _maxShingleRepeat = seen + 1;
        }

        if (_tokenCount < MinTokensBeforeAbort)
        {
            return false;
        }

        var ratio = _shingleTotal == 0 ? 0.0 : (double)_repeatedPositions / _shingleTotal;
        var ratioTrips = ratio >= RepeatRatioThreshold && _maxShingleRepeat >= MinShingleRepeatForRatio;
        if (!ratioTrips && _maxShingleRepeat < MaxShingleRepeatThreshold)
        {
            return false;
        }

        LoopDetected = true;
        Diagnosis = $"{ratio:P0} of {ShingleSize}-word phrases recur, one repeated {_maxShingleRepeat} times, across {_tokenCount} reasoning tokens";
        AutomatonDebug.LogWarning($"StreamingLoopMonitor: Loop detected mid-stream - {Diagnosis}");
        return true;
    }

    /// <summary>
    /// Tracks inline <c>&lt;think&gt;</c> spans across delta boundaries and yields
    /// the text *inside* them.
    ///
    /// This is the mirror image of <see cref="InlineThinkStripper"/>, which yields
    /// the text outside. It is a separate implementation on purpose: the stripper
    /// sits in the determinism-critical apply path that record-and-replay
    /// convergence depends on, and must not grow a second output mode to serve a
    /// diagnostic.
    /// </summary>
    private sealed class ThinkSpanTracker
    {
        private const string Open = "<think>";
        private const string Close = "</think>";

        private bool _insideThink;
        private string _carry = string.Empty;

        public string ExtractThinkText(string delta)
        {
            if (string.IsNullOrEmpty(delta) && _carry.Length == 0)
            {
                return string.Empty;
            }

            var work = _carry.Length == 0 ? delta : _carry + delta;
            _carry = string.Empty;

            var output = new StringBuilder(work.Length);
            var i = 0;

            while (i < work.Length)
            {
                if (!_insideThink)
                {
                    var open = work.IndexOf(Open, i, StringComparison.OrdinalIgnoreCase);
                    if (open < 0)
                    {
                        // Outside a think span: nothing to emit. Hold back a tail
                        // that could still grow into an opener.
                        HoldPartialTag(work, work.Length - i, Open);
                        return output.ToString();
                    }

                    i = open + Open.Length;
                    _insideThink = true;
                    continue;
                }

                var close = work.IndexOf(Close, i, StringComparison.OrdinalIgnoreCase);
                if (close < 0)
                {
                    // Still inside: everything up to a possible partial closer is
                    // reasoning. The held-back tail is re-examined next delta, so
                    // it is not emitted here to avoid counting it twice.
                    var remLen = work.Length - i;
                    var hold = PartialTagSuffixLen(work, remLen, Close);
                    output.Append(work, i, remLen - hold);
                    HoldPartialTag(work, remLen, Close);
                    return output.ToString();
                }

                output.Append(work, i, close - i);
                i = close + Close.Length;
                _insideThink = false;
            }

            return output.ToString();
        }

        private void HoldPartialTag(string work, int len, string tag)
        {
            var hold = PartialTagSuffixLen(work, len, tag);
            if (hold > 0)
            {
                _carry = work.Substring(work.Length - hold);
            }
        }

        private static int PartialTagSuffixLen(string s, int len, string tag)
        {
            var max = Math.Min(tag.Length - 1, len);
            for (var k = max; k >= 1; k--)
            {
                if (string.Compare(s, s.Length - k, tag, 0, k, StringComparison.OrdinalIgnoreCase) == 0)
                {
                    return k;
                }
            }

            return 0;
        }
    }
}
