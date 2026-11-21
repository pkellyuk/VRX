using System.Drawing;
using System.Globalization;
using System.Text;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>A place in recognised text that matches what was searched for.</summary>
public sealed class TextMatch
{
    /// <summary>The recognised words the match covers, as OCR read them.</summary>
    public string Text { get; set; } = string.Empty;

    /// <summary>The whole line the match sits in, for telling identical matches apart.</summary>
    public string LineText { get; set; } = string.Empty;

    /// <summary>Union of the matched words' boxes, in the lines' coordinates.</summary>
    public RectangleF Bounds { get; set; }

    /// <summary>1 for a match of the text itself; below 1 for a close reading of it.</summary>
    public double Similarity { get; set; } = 1;

    public bool Approximate => Similarity < 1;
}

/// <summary>
/// Finds a phrase in OCR output, allowing for the ways OCR gets screen text wrong.
///
/// Three passes, each only if the one before found nothing, so an exact hit is never
/// buried under near misses:
/// <list type="number">
/// <item>The phrase itself, ignoring case, spacing and typographic quote and dash styles.</item>
/// <item>Letters and digits only, with the confusions Windows OCR actually made folded
/// together — "£4,000.00" was read as "f4,OOO.OO", and "16 TB" can come back split or
/// joined.</item>
/// <item>Close readings, by edit distance, flagged as approximate.</item>
/// </list>
/// A match always covers whole words, and its box is the union of theirs: a query that
/// is part of a word ("Studio" in "Studio,") selects that word.
/// </summary>
public static class TextMatcher
{
    /// <summary>Close readings below this similarity are not offered.</summary>
    public const double ApproximateThreshold = 0.75;

    /// <summary>Shorter queries match too much by edit distance to be worth trying.</summary>
    private const int ApproximateMinimumLength = 4;

    public static IReadOnlyList<TextMatch> Find(IReadOnlyList<OcrTextLine> lines, string? query)
    {
        ArgumentNullException.ThrowIfNull(lines);

        if (string.IsNullOrWhiteSpace(query))
        {
            return Array.Empty<TextMatch>();
        }

        var matches = FindExact(lines, Loose(query), Loose, " ");
        if (matches.Count > 0)
        {
            return matches;
        }

        matches = FindExact(lines, Compact(query), Compact, string.Empty);
        if (matches.Count > 0)
        {
            return matches;
        }

        return FindApproximate(lines, query);
    }

    private static List<TextMatch> FindExact(IReadOnlyList<OcrTextLine> lines, string wanted, Func<string, string> normalise, string joiner)
    {
        var matches = new List<TextMatch>();

        if (wanted.Length == 0)
        {
            return matches;
        }

        foreach (var line in lines)
        {
            if (line.Words.Count == 0)
            {
                continue;
            }

            // The line as one string, remembering which word each character came from,
            // so a match found in the string maps straight back to word boxes.
            var text = new StringBuilder();
            var owner = new List<int>();

            for (var i = 0; i < line.Words.Count; i++)
            {
                if (i > 0)
                {
                    text.Append(joiner);
                    owner.AddRange(Enumerable.Repeat(-1, joiner.Length));
                }

                var word = normalise(line.Words[i].Text);
                text.Append(word);
                owner.AddRange(Enumerable.Repeat(i, word.Length));
            }

            var haystack = text.ToString();
            var start = 0;

            while (start <= haystack.Length - wanted.Length)
            {
                var index = haystack.IndexOf(wanted, start, StringComparison.Ordinal);
                if (index < 0)
                {
                    break;
                }

                var covered = owner.Skip(index).Take(wanted.Length).Where(w => w >= 0).ToList();
                if (covered.Count > 0)
                {
                    matches.Add(BuildMatch(line, covered.Min(), covered.Max(), similarity: 1));
                }

                start = index + wanted.Length;
            }
        }

        return matches;
    }

    private static List<TextMatch> FindApproximate(IReadOnlyList<OcrTextLine> lines, string query)
    {
        var matches = new List<TextMatch>();
        var wanted = Compact(query);

        if (wanted.Length < ApproximateMinimumLength)
        {
            return matches;
        }

        var queryWords = query.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries).Length;

        foreach (var line in lines)
        {
            TextMatch? best = null;

            // Windows one word either side of the query's own length, because OCR both
            // splits words ("Mac Studio" -> "Mac Stu dio") and runs them together.
            for (var count = Math.Max(1, queryWords - 1); count <= queryWords + 1; count++)
            {
                for (var first = 0; first + count <= line.Words.Count; first++)
                {
                    var candidate = Compact(string.Concat(line.Words.Skip(first).Take(count).Select(w => w.Text)));
                    if (candidate.Length == 0)
                    {
                        continue;
                    }

                    var similarity = Similarity(candidate, wanted);
                    if (similarity >= ApproximateThreshold && (best is null || similarity > best.Similarity))
                    {
                        best = BuildMatch(line, first, first + count - 1, similarity);
                    }
                }
            }

            if (best is not null)
            {
                matches.Add(best);
            }
        }

        return matches.OrderByDescending(m => m.Similarity).ToList();
    }

    private static TextMatch BuildMatch(OcrTextLine line, int firstWord, int lastWord, double similarity)
    {
        var words = line.Words.Skip(firstWord).Take(lastWord - firstWord + 1).ToList();

        var bounds = words[0].Bounds;
        foreach (var word in words.Skip(1))
        {
            bounds = RectangleF.Union(bounds, word.Bounds);
        }

        return new TextMatch
        {
            Text = string.Join(" ", words.Select(w => w.Text)),
            LineText = line.Text,
            Bounds = bounds,
            Similarity = similarity
        };
    }

    /// <summary>Case, spacing, and typographic punctuation styles ignored.</summary>
    internal static string Loose(string text)
    {
        var builder = new StringBuilder(text.Length);
        var pendingSpace = false;

        foreach (var raw in text.Normalize(NormalizationForm.FormKC))
        {
            var c = raw switch
            {
                '‘' or '’' or '‚' or '′' => '\'',
                '“' or '”' or '„' or '″' => '"',
                '‐' or '‑' or '‒' or '–' or '—' or '−' => '-',
                '​' or '‌' or '‍' or '﻿' => '\0',
                _ => raw
            };

            if (c == '\0')
            {
                continue;
            }

            if (char.IsWhiteSpace(c))
            {
                pendingSpace = builder.Length > 0;
                continue;
            }

            if (pendingSpace)
            {
                builder.Append(' ');
                pendingSpace = false;
            }

            builder.Append(char.ToLowerInvariant(c));
        }

        return builder.ToString();
    }

    /// <summary>
    /// Letters and digits only, with OCR's usual letter-for-digit confusions folded
    /// together: O and 0, and I, l and 1. Applied to both sides, so "Move" still matches
    /// "Move" — both become "m0ve".
    /// </summary>
    internal static string Compact(string text)
    {
        var builder = new StringBuilder(text.Length);

        foreach (var raw in text.Normalize(NormalizationForm.FormKC))
        {
            if (!char.IsLetterOrDigit(raw))
            {
                continue;
            }

            var c = char.ToLowerInvariant(raw);
            builder.Append(c switch
            {
                'o' => '0',
                'i' or 'l' => '1',
                _ => c
            });
        }

        return builder.ToString();
    }

    private static double Similarity(string a, string b)
    {
        var longest = Math.Max(a.Length, b.Length);
        return longest == 0 ? 1 : 1 - (double)Levenshtein(a, b) / longest;
    }

    private static int Levenshtein(string a, string b)
    {
        var previous = new int[b.Length + 1];
        var current = new int[b.Length + 1];

        for (var j = 0; j <= b.Length; j++)
        {
            previous[j] = j;
        }

        for (var i = 1; i <= a.Length; i++)
        {
            current[0] = i;

            for (var j = 1; j <= b.Length; j++)
            {
                var cost = a[i - 1] == b[j - 1] ? 0 : 1;
                current[j] = Math.Min(Math.Min(current[j - 1] + 1, previous[j] + 1), previous[j - 1] + cost);
            }

            (previous, current) = (current, previous);
        }

        return previous[b.Length];
    }
}
