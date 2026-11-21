using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace PersonaBot.Voice;

/// <summary>
/// Turns what a bot writes into words Kokoro can say. Since sherpa-onnx 2.0, Kokoro only says words in its
/// pronunciation lexicon and silently skips the rest (it no longer falls back to espeak-ng), so everything has to
/// arrive as lexicon words: markdown and symbols go, money, times, ordinals, years and numbers become words, links
/// become their site's name, acronyms are spelled out, joined-up names ("PersonaBot", "DeepSeek") are split at their
/// capitals, and any other unknown word is split into known pieces ("qemu" → "q emu") or, failing that, spelled.
/// </summary>
public sealed partial class SpeechText
{
    private readonly Func<string, bool> _known;

    /// <param name="known">Whether a lower-case word is in the lexicon.</param>
    public SpeechText(Func<string, bool> known)
    {
        ArgumentNullException.ThrowIfNull(known);
        _known = known;
    }

    /// <summary>Short words often written in capitals for emphasis: said as words, not spelled.</summary>
    private static readonly HashSet<string> CapitalWords = new(StringComparer.Ordinal)
    {
        "OK", "NO", "YES", "NOT", "THE", "AND", "ALL", "NEW", "NOW", "YOU", "CAN", "OFF", "ON", "IT", "IS", "DO", "DON'T",
        "STOP", "WAIT", "DONE", "NOTE", "TIP", "HELP", "GO", "BUY", "PAY", "SAVE", "SEND", "NEXT", "BACK", "OR", "BUT",
        "TO", "OF", "IN", "AT", "BY", "FOR", "MY", "ME", "WE", "US", "SO", "UP", "IF", "AN", "AS", "BE", "HE", "SHE", "THEY",
        "FREE", "SALE", "OPEN", "LIVE", "WARNING", "IMPORTANT", "NEVER", "ALWAYS", "ONLY", "MUST", "VERY", "JUST", "ALSO"
    };

    /// <summary>The whole text, ready to speak; "" when nothing in it can be said.</summary>
    public string Prepare(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return "";
        var s = Markdown(text);
        s = Symbols(s);
        s = PhonePattern().Replace(s, m => " " + Digits(new string([.. m.Value.Where(char.IsDigit)])) + ", ");
        s = MoneyPattern().Replace(s, m => Money(m));
        s = TimePattern().Replace(s, m => Time(m));
        s = OrdinalPattern().Replace(s, m => long.TryParse(m.Groups[1].Value, out var n) && n < 1_000_000 ? Ordinal(n) : m.Value);
        s = LetterDigitPattern().Replace(s, " ");
        s = YearPattern().Replace(s, m => Year(int.Parse(m.Value, CultureInfo.InvariantCulture)));
        s = DecimalPattern().Replace(s, m => Decimal(m));
        s = IntegerPattern().Replace(s, m => Integer(m));
        s = WordPattern().Replace(s, m => Word(m.Value));
        s = SpacePattern().Replace(s, " ").Trim();
        // Tidy punctuation the steps above leave: "three, ." → "three.", "com , about" → "com, about".
        s = CommaBeforeStopPattern().Replace(s, "$1");
        s = SpaceBeforePunctuationPattern().Replace(s, "$1");
        return s.Any(char.IsLetter) ? s : "";
    }

    // ---- markdown and symbols ----

    private static string Markdown(string text)
    {
        var s = text.Replace("\r\n", "\n");
        s = CodeBlockPattern().Replace(s, " (some code) ");
        s = LinkPattern().Replace(s, "$1");
        s = UrlPattern().Replace(s, m => " " + Site(m.Value) + " ");
        s = EmailPattern().Replace(s, m => " " + m.Groups[1].Value + " at " + m.Groups[2].Value.Replace(".", " dot ") + " ");
        s = HeadingPattern().Replace(s, "");
        s = BulletPattern().Replace(s, "");
        s = s.Replace("**", "").Replace("__", "").Replace("`", "").Replace("~~", "");
        s = EmphasisPattern().Replace(s, "$1");
        // Each line is its own sentence (lists, headings), which also lets speech start sooner.
        var lines = s.Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0)
            .Select(l => ".!?:;,…".Contains(l[^1]) ? l : l + ".");
        return string.Join(" ", lines);
    }

    /// <summary>A link said as its site: "https://www.apple.com/shop/buy" → "apple dot com".</summary>
    private static string Site(string url)
    {
        var host = url;
        if (Uri.TryCreate(url.StartsWith("www.", StringComparison.OrdinalIgnoreCase) ? "https://" + url : url, UriKind.Absolute, out var uri)) host = uri.Host;
        if (host.StartsWith("www.", StringComparison.OrdinalIgnoreCase)) host = host[4..];
        return host.Replace(".", " dot ");
    }

    private static string Symbols(string s)
    {
        var b = new StringBuilder(s.Length + 16);
        foreach (var c in s.Normalize(NormalizationForm.FormC))
        {
            switch (c)
            {
                case '&': b.Append(" and "); break;
                case '%': b.Append(" percent "); break;
                case '+': b.Append(" plus "); break;
                case '@': b.Append(" at "); break;
                case '=': b.Append(" equals "); break;
                case '×': b.Append(" times "); break;
                case '°': b.Append(" degrees "); break;
                case '→': case '⇒': b.Append(" to "); break;
                case '~': b.Append(" about "); break;
                case '/': case '\\': case '|': b.Append(" or "); break;
                case '_': case '*': case '#': case '^': case '<': case '>': case '[': case ']': case '{': case '}': b.Append(' '); break;
                case '‘': case '’': b.Append('\''); break;
                case '–': case '—': b.Append(", "); break;
                case '-': b.Append('-'); break;
                default:
                    var category = char.GetUnicodeCategory(c);
                    // Emoji and other pictures (and their surrogate halves and joiners) aren't said.
                    if (category is UnicodeCategory.OtherSymbol or UnicodeCategory.Surrogate or UnicodeCategory.Format or UnicodeCategory.PrivateUse) b.Append(' ');
                    else b.Append(c);
                    break;
            }
        }
        // "e.g." and friends, before the full stops in them end sentences.
        return b.ToString().Replace("e.g.", "for example").Replace("i.e.", "that is").Replace("etc.", "et cetera.").Replace("vs.", "versus").Replace(" vs ", " versus ");
    }

    // ---- numbers ----

    private static readonly string[] Units =
    [
        "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen",
        "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"
    ];

    private static readonly string[] Tens = ["", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"];

    /// <summary>A whole number in British English words: 105 → "one hundred and five".</summary>
    public static string Number(long n)
    {
        if (n < 0) return "minus " + Number(-n);
        if (n < 20) return Units[n];
        if (n < 100) return Tens[n / 10] + (n % 10 > 0 ? " " + Units[n % 10] : "");
        if (n < 1000) return Units[n / 100] + " hundred" + (n % 100 > 0 ? " and " + Number(n % 100) : "");
        foreach (var (size, name) in new[] { (1_000_000_000_000L, "trillion"), (1_000_000_000L, "billion"), (1_000_000L, "million"), (1000L, "thousand") })
        {
            if (n < size) continue;
            var rest = n % size;
            return Number(n / size) + " " + name + (rest == 0 ? "" : rest < 100 ? " and " + Number(rest) : " " + Number(rest));
        }
        return n.ToString(CultureInfo.InvariantCulture);
    }

    /// <summary>An ordinal in words: 21 → "twenty first".</summary>
    public static string Ordinal(long n)
    {
        var words = Number(n);
        var space = words.LastIndexOf(' ');
        var last = words[(space + 1)..];
        var head = space < 0 ? "" : words[..(space + 1)];
        var ordinal = last switch
        {
            "one" => "first", "two" => "second", "three" => "third", "five" => "fifth", "eight" => "eighth", "nine" => "ninth",
            "twelve" => "twelfth",
            _ when last.EndsWith('y') => last[..^1] + "ieth",
            _ => last + "th"
        };
        return head + ordinal;
    }

    /// <summary>A year as it's said: 1996 → "nineteen ninety six", 2005 → "two thousand and five", 2026 → "twenty twenty six".</summary>
    public static string Year(int year)
    {
        if (year is >= 2000 and < 2010) return Number(year);
        var (high, low) = (year / 100, year % 100);
        if (low == 0) return Number(high) + " hundred";
        return Number(high) + " " + (low < 10 ? "oh " + Units[low] : Number(low));
    }

    private static string Digits(string digits) => string.Join(" ", digits.Select(d => Units[d - '0']));

    private static string Money(Match m)
    {
        var symbol = m.Groups[1].Value;
        var whole = m.Groups[2].Value.Replace(",", "");
        var fraction = m.Groups[3].Success ? m.Groups[3].Value : "";
        var scale = m.Groups[4].Value.ToLowerInvariant();
        var (one, many) = symbol switch
        {
            "£" => ("pound", "pounds"),
            "€" => ("euro", "euros"),
            _ => ("dollar", "dollars")
        };
        if (!long.TryParse(whole, NumberStyles.None, CultureInfo.InvariantCulture, out var amount)) return m.Value;
        var scaleWord = scale switch { "k" => "thousand", "m" => "million", "bn" or "b" => "billion", _ => "" };
        if (scaleWord.Length > 0)
        {
            var number = Number(amount) + (fraction.Length > 0 ? " point " + Digits(fraction) : "");
            return $" {number} {scaleWord} {many} ";
        }
        var said = Number(amount) + " " + (amount == 1 ? one : many);
        if (fraction.Length == 2 && fraction != "00")
        {
            var cents = int.Parse(fraction, CultureInfo.InvariantCulture);
            // £29.99 is said "twenty nine pounds ninety nine"; $5.50 "five dollars fifty".
            said += " " + Number(cents);
        }
        else if (fraction.Length > 0 && fraction != "00") said = Number(amount) + " point " + Digits(fraction) + " " + many;
        return " " + said + " ";
    }

    private static string Time(Match m)
    {
        var hour = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
        var minute = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
        var suffix = m.Groups[3].Success ? m.Groups[3].Value.ToLowerInvariant().Replace(".", "") : "";
        if (hour > 23 || minute > 59) return m.Value;
        var said = minute == 0 ? Number(hour) + (suffix.Length == 0 ? " o'clock" : "") : Number(hour) + " " + (minute < 10 ? "oh " + Units[minute] : Number(minute));
        if (suffix.Length > 0) said += " " + string.Join(" ", suffix.ToUpperInvariant().ToCharArray());
        return " " + said + " ";
    }

    private static string Decimal(Match m)
    {
        var whole = m.Groups[1].Value.Replace(",", "");
        if (!long.TryParse(whole, NumberStyles.None, CultureInfo.InvariantCulture, out var n)) return m.Value;
        return " " + Number(n) + " point " + Digits(m.Groups[2].Value) + " ";
    }

    private static string Integer(Match m)
    {
        var digits = m.Groups[2].Value.Replace(",", "");
        var minus = m.Groups[1].Success && m.Groups[1].Value == "-";
        // Long digit strings (phone numbers, codes) are said digit by digit.
        if (digits.Length > 12 || digits.Length > 1 && digits[0] == '0') return " " + Digits(digits) + " ";
        if (!long.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out var n)) return " " + Digits(digits) + " ";
        return " " + (minus ? "minus " : "") + Number(n) + " ";
    }

    // ---- words ----

    private string Word(string word)
    {
        var lower = word.ToLowerInvariant();
        var allCaps = word.Length > 1 && word.All(c => !char.IsLetter(c) || char.IsUpper(c));
        // A plural acronym: "VMs" → "V M's", said "vee emz".
        if (PluralAcronymPattern().IsMatch(word)) return Spell(word[..^1].TrimEnd('\'')) + "'s";
        if (allCaps)
        {
            if (CapitalWords.Contains(word)) return word;
            // Short capitals are acronyms (AI, VM, GPU, URL); longer ones are words if the lexicon knows them (NASA).
            if (word.Length <= 4 && !word.Contains('\'') || !_known(lower)) return Spell(word);
            return word;
        }
        if (_known(lower)) return word;
        if (word.Contains('-')) return string.Join(" ", word.Split('-', StringSplitOptions.RemoveEmptyEntries).Select(Word));
        var parts = CamelPattern().Split(word).Where(p => p.Length > 0).ToArray();
        if (parts.Length > 1) return string.Join(" ", parts.Select(Word));
        // Anything else unknown is left for the phonemizer, which builds it from known words or spells it.
        return word;
    }

    /// <summary>Spelled letter by letter: "GPU" → "G P U".</summary>
    private static string Spell(string word) =>
        string.Join(" ", word.Where(char.IsLetter).Select(c => char.ToUpperInvariant(c).ToString()));

    [GeneratedRegex(@"```.*?```", RegexOptions.Singleline)] private static partial Regex CodeBlockPattern();
    [GeneratedRegex(@"!?\[([^\]]*)\]\([^)]*\)")] private static partial Regex LinkPattern();
    [GeneratedRegex(@"\b(?:https?://|www\.)[^\s)>\]]+", RegexOptions.IgnoreCase)] private static partial Regex UrlPattern();
    [GeneratedRegex(@"\b([A-Za-z0-9._%+-]+)@([A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+)\b")] private static partial Regex EmailPattern();
    [GeneratedRegex(@"^\s{0,3}#{1,6}\s*", RegexOptions.Multiline)] private static partial Regex HeadingPattern();
    [GeneratedRegex(@"^\s*(?:[-*•]|\d+[.)])\s+", RegexOptions.Multiline)] private static partial Regex BulletPattern();
    [GeneratedRegex(@"(?<![\w*])[*_]([^*_\n]+)[*_](?![\w*])")] private static partial Regex EmphasisPattern();
    [GeneratedRegex(@"([£$€])\s?(\d{1,3}(?:,\d{3})+|\d+)(?:\.(\d+))?\s?(k|m|bn|b)?\b", RegexOptions.IgnoreCase)] private static partial Regex MoneyPattern();
    [GeneratedRegex(@"\b(\d{1,2}):(\d{2})\s?(am|pm|a\.m\.|p\.m\.)?(?![\d:])", RegexOptions.IgnoreCase)] private static partial Regex TimePattern();
    [GeneratedRegex(@"\b(\d+)(?:st|nd|rd|th)\b", RegexOptions.IgnoreCase)] private static partial Regex OrdinalPattern();
    [GeneratedRegex(@"(?<=[A-Za-z])(?=\d)|(?<=\d)(?=[A-Za-z])")] private static partial Regex LetterDigitPattern();
    [GeneratedRegex(@"(?<![\d.,£$€])\b(1[1-9]\d\d|20\d\d)\b(?![\d,]|\.\d)")] private static partial Regex YearPattern();
    [GeneratedRegex(@"\b(\d{1,3}(?:,\d{3})+|\d+)\.(\d+)\b")] private static partial Regex DecimalPattern();
    [GeneratedRegex(@"(?:(?<=^|\s)(-))?\b(\d{1,3}(?:,\d{3})+|\d+)\b")] private static partial Regex IntegerPattern();
    [GeneratedRegex(@"[A-Za-z][A-Za-z']*(?:-[A-Za-z][A-Za-z']*)*")] private static partial Regex WordPattern();
    [GeneratedRegex(@"(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])")] private static partial Regex CamelPattern();
    [GeneratedRegex(@"\s+")] private static partial Regex SpacePattern();
    [GeneratedRegex(@"^[A-Z]{2,}'?s$")] private static partial Regex PluralAcronymPattern();
    [GeneratedRegex(@"\s*,\s*([.!?;:])")] private static partial Regex CommaBeforeStopPattern();
    [GeneratedRegex(@"\s+([,.!?;:])")] private static partial Regex SpaceBeforePunctuationPattern();
    [GeneratedRegex(@"(?<![\w£$€.])(?:\+\d{1,3}[\s-]?(?:\(0\))?\d{2,5}|0\d{2,4})(?:[\s-]?\d{3,4}){1,3}\b")] private static partial Regex PhonePattern();
}
