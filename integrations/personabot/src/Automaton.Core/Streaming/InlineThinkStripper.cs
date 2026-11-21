using System.Text;

namespace Automaton.Core.Streaming;

/// <summary>
/// Removes inline <c>&lt;think&gt;...&lt;/think&gt;</c> spans from a *streamed*
/// content channel, tolerant of the tags being split across delta boundaries.
///
/// Why this exists: some models (e.g. Minimax M3) emit their reasoning on
/// <em>two</em> channels at once &mdash; the out-of-band
/// <c>reasoning_content</c>/<c>reasoning</c> delta fields <em>and</em> a
/// duplicate copy wrapped in literal <c>&lt;think&gt;</c> tags inside the
/// normal <c>content</c> field. <see cref="LlmStreamApplier"/> already turns the
/// out-of-band channel into a single live <c>&lt;think&gt;</c> block, so the
/// inline copy is pure duplication. Worse, the inline <c>&lt;think&gt;</c>
/// opener is a non-whitespace content delta, so it slips past the stray-
/// whitespace guard and triggers an early flush of the still-incomplete
/// out-of-band buffer &mdash; producing the truncated "ghost" block followed by
/// the full duplicate (the duplicated/truncated <c>&lt;think&gt;</c> bug).
///
/// The applier feeds the content channel through this stripper <em>only when
/// out-of-band reasoning has been seen on the same stream</em>, so single-
/// channel models (inline-only or out-of-band-only) are untouched.
///
/// Determinism: output depends solely on the byte sequence fed in and the
/// instance's own carried state. Same bytes + same start state =&gt; same
/// output, which is what the XCHAT record-and-replay convergence relies on.
/// </summary>
public sealed class InlineThinkStripper
{
    private const string Open = "<think>";
    private const string Close = "</think>";

    private bool _insideThink;

    // A tail of the most recent input held back because it *might* be the
    // beginning of an Open/Close tag that continues in the next delta. Never
    // longer than the matching tag minus one char.
    private string _carry = string.Empty;

    /// <summary>True while the stream is between an open and close think tag.</summary>
    public bool InsideThink => _insideThink;

    /// <summary>
    /// Returns <paramref name="delta"/> with any complete or in-progress
    /// inline think span removed. Bytes that could be the start of a tag
    /// straddling the boundary are carried and resolved on the next call.
    /// </summary>
    public string Strip(string? delta)
    {
        if (string.IsNullOrEmpty(delta) && _carry.Length == 0)
        {
            return string.Empty;
        }

        var work = _carry.Length == 0 ? delta! : _carry + delta;
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
                    // No complete opener in the remainder. Emit everything except
                    // a trailing suffix that could grow into "<think>".
                    var remLen = work.Length - i;
                    var hold = PartialTagSuffixLen(work, remLen, Open);
                    output.Append(work, i, remLen - hold);
                    if (hold > 0)
                    {
                        _carry = work.Substring(work.Length - hold);
                    }
                    return output.ToString();
                }

                output.Append(work, i, open - i);
                i = open + Open.Length;
                _insideThink = true;
            }
            else
            {
                var close = work.IndexOf(Close, i, StringComparison.OrdinalIgnoreCase);
                if (close < 0)
                {
                    // Still inside the think span. Discard the remainder, but keep
                    // a trailing suffix that could grow into "</think>".
                    var remLen = work.Length - i;
                    var hold = PartialTagSuffixLen(work, remLen, Close);
                    if (hold > 0)
                    {
                        _carry = work.Substring(work.Length - hold);
                    }
                    return output.ToString();
                }

                i = close + Close.Length;
                _insideThink = false;
            }
        }

        return output.ToString();
    }

    /// <summary>
    /// Flushes any held-back tail at end of stream. If we are outside a think
    /// span, a leftover carry was a false partial (e.g. a literal trailing
    /// <c>"&lt;"</c>) and is real content, so it is returned. If we are inside an
    /// unterminated think span, the carry is reasoning and is dropped.
    /// </summary>
    public string Flush()
    {
        var rest = !_insideThink ? _carry : string.Empty;
        _carry = string.Empty;
        return rest;
    }

    // Length of the longest suffix of the last <paramref name="len"/> chars of
    // <paramref name="s"/> that is a proper prefix of <paramref name="tag"/>
    // (case-insensitive). A full match would have been found by IndexOf, so we
    // only ever hold back a strict prefix.
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
