using System.Text;

namespace PersonaBot.Voice;

/// <summary>
/// Turns prepared text (<see cref="SpeechText"/>: words and punctuation only) into Kokoro's phonemes, a chunk of a
/// sentence or two at a time, for sherpa-onnx's phoneme input. Words come from Kokoro's own lexicon. The lexicon
/// lacks some everyday forms ("options", "tasks", "emails"), and sherpa-onnx 2.0 skips any word it doesn't have,
/// so a missing word is built here from one it does have: plurals and possessives (+s/z/ɪz), past tenses
/// (+t/d/ɪd), -ing, -er, -est, -ly, -ness, -ment, -ful, -less, -able, and un-/re-/pre-; then joined-up words split into
/// known ones ("deepseek" → "deep" "seek"); then a sayable word (a name, a brand: "Paul", "Tesco") by English
/// letter-to-sound rules (<see cref="LetterToSound"/>); and only what can't be said (an abbreviation) is spelled.
/// </summary>
public sealed class Phonemizer
{
    private readonly IReadOnlyDictionary<string, string> _lexicon;
    private readonly HashSet<char> _tokens;
    private readonly string _accent;

    /// <param name="lexicon">Lower-case word → its phonemes (no spaces), as in Kokoro's lexicon files.</param>
    /// <param name="tokens">The phoneme and punctuation characters the model knows (tokens.txt); others are dropped.</param>
    /// <param name="accent">"gb" or "us": how words built by rule are said.</param>
    public Phonemizer(IReadOnlyDictionary<string, string> lexicon, IEnumerable<char> tokens, string accent = "gb")
    {
        ArgumentNullException.ThrowIfNull(lexicon);
        ArgumentNullException.ThrowIfNull(tokens);
        _lexicon = lexicon;
        _tokens = [.. tokens];
        _accent = accent == "us" ? "us" : "gb";
    }

    /// <summary>Reads a Kokoro lexicon file: each line is a word then its phonemes, separated by spaces.</summary>
    public static Dictionary<string, string> ReadLexicon(string file)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(file);
        var lexicon = new Dictionary<string, string>(200_000, StringComparer.Ordinal);
        foreach (var line in File.ReadLines(file))
        {
            var space = line.IndexOf(' ');
            if (space <= 0) continue;
            var word = line[..space].ToLowerInvariant();
            if (lexicon.ContainsKey(word)) continue;
            lexicon[word] = line[(space + 1)..].Replace(" ", "");
        }
        return lexicon;
    }

    /// <summary>Reads the characters in a tokens.txt file (each line: a character, a space, its id).</summary>
    public static IEnumerable<char> ReadTokens(string file)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(file);
        foreach (var line in File.ReadLines(file))
        {
            if (line.Length >= 1 && (line.Length == 1 || line[1] == ' ' || line.LastIndexOf(' ') == 1)) yield return line[0];
        }
    }

    /// <summary>Whether the lexicon has <paramref name="word"/> (lower case).</summary>
    public bool Knows(string word) => _lexicon.ContainsKey(word);

    private const string ChunkEnds = ".!?;:";
    private const string Punctuation = ",.!?;:";

    /// <summary>
    /// The phonemes of <paramref name="text"/>, in chunks of whole sentences: short sentences are joined (at least
    /// <paramref name="minWords"/> words) and long ones split at commas or every <paramref name="maxWords"/> words, as
    /// sherpa-onnx does for text. Each chunk is said in one go.
    /// </summary>
    public List<string> Chunks(string text, int minWords = 5, int maxWords = 24)
    {
        ArgumentNullException.ThrowIfNull(text);
        var chunks = new List<string>();
        var current = new StringBuilder();
        var words = 0;
        var wordsSinceComma = 0;
        foreach (var token in Tokens(text))
        {
            if (token.Length == 1 && Punctuation.Contains(token[0]))
            {
                // Punctuation goes straight after the word, then a space before the next word.
                if (current.Length > 0 && current[^1] == ' ') current.Length--;
                if (current.Length == 0) continue;
                current.Append(token).Append(' ');
                // The first chunk ends at the first pause, so the voice starts while the rest is being made.
                var end = ChunkEnds.Contains(token[0]) && (words >= minWords || chunks.Count == 0) ||
                          token[0] == ',' && (words >= maxWords / 2 && wordsSinceComma > 3 || chunks.Count == 0 && words >= 3);
                if (token[0] == ',') wordsSinceComma = 0;
                if (!end) continue;
                Flush();
                continue;
            }
            var phonemes = Word(token);
            if (phonemes.Length == 0) continue;
            current.Append(phonemes).Append(' ');
            words++;
            wordsSinceComma++;
            if (words >= maxWords || chunks.Count == 0 && words >= 10) Flush();
        }
        Flush();
        return chunks;

        void Flush()
        {
            var chunk = Clean(current.ToString().Trim());
            if (chunk.Any(c => !Punctuation.Contains(c) && c != ' ')) chunks.Add(chunk);
            current.Clear();
            words = 0;
            wordsSinceComma = 0;
        }
    }

    /// <summary>Words and single punctuation marks, in order.</summary>
    private static IEnumerable<string> Tokens(string text)
    {
        var word = new StringBuilder();
        foreach (var c in text)
        {
            if (char.IsLetter(c) || c == '\'' && word.Length > 0)
            {
                word.Append(c);
                continue;
            }
            if (word.Length > 0)
            {
                yield return word.ToString().TrimEnd('\'');
                word.Clear();
            }
            if (Punctuation.Contains(c)) yield return c.ToString();
            else if (c == '…') yield return ".";
        }
        if (word.Length > 0) yield return word.ToString().TrimEnd('\'');
    }

    /// <summary>Only characters the model has tokens for.</summary>
    private string Clean(string phonemes) => new([.. phonemes.Where(c => c == ' ' || _tokens.Contains(c))]);

    /// <summary>The phonemes for one word (any case); "" when it has no letters.</summary>
    public string Word(string word)
    {
        ArgumentNullException.ThrowIfNull(word);
        var lower = word.ToLowerInvariant().Trim('\'');
        if (lower.Length == 0 || !lower.Any(char.IsLetter)) return "";
        if (_lexicon.TryGetValue(lower, out var known)) return known;
        if ((Derived(lower, depth: 0) ?? Compound(lower)) is { } built) return built;
        return LetterToSound.Pronounceable(lower) ? LetterToSound.Phonemes(lower, _accent) : Spelled(lower);
    }

    /// <summary>A missing word built from a known one by its ending or beginning; null when none fits.</summary>
    private string? Derived(string w, int depth)
    {
        if (depth > 2 || w.Length < 3) return null;
        string? Known(string stem) => _lexicon.TryGetValue(stem, out var p) ? p : depth < 2 ? Derived(stem, depth + 1) : null;

        if (w.EndsWith("'s") && Known(w[..^2]) is { } owner) return owner + PluralSound(owner);
        if (w.EndsWith("s'") && Known(w[..^2]) is { } owners) return owners;
        if (w.EndsWith("ies") && Known(w[..^3] + "y") is { } y) return y + "z";
        if (w.EndsWith("es") && (w.EndsWith("ses") || w.EndsWith("xes") || w.EndsWith("zes") || w.EndsWith("ches") || w.EndsWith("shes")) && Known(w[..^2]) is { } sibilant)
            return sibilant + "ɪz";
        if (w.EndsWith('s') && !w.EndsWith("ss") && Known(w[..^1]) is { } single) return single + PluralSound(single);

        if (w.EndsWith("ied") && Known(w[..^3] + "y") is { } tried) return tried + "d";
        if (w.EndsWith("ed"))
        {
            foreach (var stem in new[] { w[..^2], w[..^1], Undoubled(w[..^2]) })
            {
                if (stem != null && stem.Length >= 2 && Known(stem) is { } past) return past + PastSound(past);
            }
        }
        if (w.EndsWith("ing"))
        {
            foreach (var stem in new[] { w[..^3], w[..^3] + "e", Undoubled(w[..^3]) })
            {
                if (stem != null && stem.Length >= 2 && Known(stem) is { } doing) return doing + "ɪŋ";
            }
        }
        foreach (var (ending, sound) in Endings)
        {
            if (!w.EndsWith(ending) || w.Length - ending.Length < 3) continue;
            var stem = w[..^ending.Length];
            if (Known(stem) is { } p) return p + sound;
            if (ending is "er" or "est" or "able" && Known(stem + "e") is { } e) return e + sound;
            if (ending is "ly" or "ness" or "less" or "ful" && stem.EndsWith('i') && Known(stem[..^1] + "y") is { } i) return i[..^1] + "i" + sound;
            if (Undoubled(stem) is { } undoubled && Known(undoubled) is { } d) return d + sound;
        }
        foreach (var (prefix, sound) in Prefixes)
        {
            if (w.StartsWith(prefix) && w.Length - prefix.Length >= 3 && Known(w[prefix.Length..]) is { } rest) return sound + rest;
        }
        return null;
    }

    private static readonly (string Ending, string Sound)[] Endings =
    [
        ("ers", "əz"), ("er", "ə"), ("est", "ɪst"), ("ly", "li"), ("ness", "nəs"), ("ment", "mənt"), ("ments", "mənts"),
        ("ful", "fʊl"), ("less", "ləs"), ("able", "əbᵊl"), ("ish", "ɪʃ")
    ];

    private static readonly (string Prefix, string Sound)[] Prefixes =
    [
        ("un", "ʌn"), ("re", "ɹiː"), ("pre", "pɹiː"), ("non", "nɒn"), ("over", "ˌQvə"), ("under", "ˌʌndə"), ("auto", "ˌɔːtQ"),
        ("multi", "mˌʌlti"), ("sub", "sˌʌb"), ("co", "kˌQ")
    ];

    /// <summary>"stopp" → "stop": a stem whose last consonant was doubled before an ending; null when it wasn't.</summary>
    private static string? Undoubled(string stem) =>
        stem.Length >= 3 && stem[^1] == stem[^2] && !"aeiou".Contains(stem[^1]) ? stem[..^1] : null;

    /// <summary>The last sound of some phonemes, skipping stress and length marks.</summary>
    private static char LastSound(string phonemes)
    {
        for (var i = phonemes.Length - 1; i >= 0; i--)
        {
            if (phonemes[i] is 'ˈ' or 'ˌ' or 'ː' or ' ') continue;
            return phonemes[i];
        }
        return ' ';
    }

    /// <summary>The sound a plural adds: "ɪz" after s/z/sh/zh/ch/j sounds, "s" after other voiceless sounds, else "z".</summary>
    public static string PluralSound(string phonemes)
    {
        var last = LastSound(phonemes);
        if ("szʃʒʧʤ".Contains(last)) return "ɪz";
        return "ptkfθ".Contains(last) ? "s" : "z";
    }

    /// <summary>The sound a past tense adds: "ɪd" after t/d, "t" after other voiceless sounds, else "d".</summary>
    public static string PastSound(string phonemes)
    {
        var last = LastSound(phonemes);
        if ("td".Contains(last)) return "ɪd";
        return "pkfθsʃʧ".Contains(last) ? "t" : "d";
    }

    /// <summary>
    /// A joined-up word as the fewest known words, each at least three letters (a single first letter is allowed:
    /// "qemu" → "q" "emu"), at most four; null when there's no such split.
    /// </summary>
    private string? Compound(string word)
    {
        if (word.Length < 4 || word.Length > 30 || word.Any(c => c is < 'a' or > 'z')) return null;
        var n = word.Length;
        var cost = new double[n + 1];
        var from = new int[n + 1];
        Array.Fill(cost, double.MaxValue);
        cost[0] = 0;
        for (var end = 1; end <= n; end++)
        {
            for (var start = Math.Max(0, end - 20); start < end; start++)
            {
                if (cost[start] == double.MaxValue) continue;
                var length = end - start;
                // Pieces of three letters or more (two-letter words would split anything: "paul" → "pa" "ul"), but a
                // single first letter is allowed, the common shape of product names.
                if (length == 1 && start != 0 || length == 2) continue;
                if (!_lexicon.ContainsKey(word[start..end])) continue;
                var c = cost[start] + 1 + (length == 1 ? 1.5 : 0);
                if (c >= cost[end]) continue;
                cost[end] = c;
                from[end] = start;
            }
        }
        if (cost[n] == double.MaxValue) return null;
        var pieces = new List<string>();
        for (var end = n; end > 0; end = from[end]) pieces.Insert(0, word[from[end]..end]);
        if (pieces.Count > 4) return null;
        return string.Join(" ", pieces.Select(p => _lexicon[p]));
    }

    /// <summary>Spelled out: each letter's name.</summary>
    private string Spelled(string word) =>
        string.Join(" ", word.Where(char.IsLetter).Select(c => _lexicon.TryGetValue(c.ToString(), out var p) ? p : "").Where(p => p.Length > 0));
}
