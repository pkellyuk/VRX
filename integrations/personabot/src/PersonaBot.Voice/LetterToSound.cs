using System.Text;

namespace PersonaBot.Voice;

/// <summary>
/// English letter-to-sound rules for words Kokoro's lexicon doesn't have (names, brands: "Paul", "Tesco", "Kodos"):
/// the Naval Research Laboratory's rules (Elovitz, Johnson, McHugh and Shore, "Automatic translation of English text to
/// phonetics by means of letter-to-sound rules", NRL Report 7948, 1976: a US government work, in the public domain),
/// as in John Wasser's public-domain english.c. Each rule is left[match]right=sounds: the first rule for the current
/// letter whose match and contexts fit is used, then the next unmatched letter. The sounds (ARPAbet) are turned into
/// Kokoro's phonemes for the accent, with British English dropping an r that isn't followed by a vowel.
/// </summary>
public static class LetterToSound
{
    // Contexts: ' ' a word edge; '#' one or more vowels; ':' zero or more consonants; '^' one consonant;
    // '.' one voiced consonant (B D V G J L M N R W Z); '+' a front vowel (E I Y); '%' a suffix (ER E ES ED ING ELY).
    // "*" as a context means anything. Sounds: two capitals are a vowel or digraph (AX, IY, NG, TH…); a small letter is a consonant.
    private static readonly string[] Table =
    [
        // A
        " [A] =AX", " [ARE] =AAr", " [AR]O=AXr", "*[AR]#=EHr", "^[AS]#=EYs", "*[A]WA=AX", "*[AW]*=AO", " :[ANY]*=EHnIY",
        "*[A]^+#=EY", "#:[ALLY]*=AXlIY", " [AL]#=AXl", "*[AGAIN]*=AXgEHn", "#:[AG]E=IHj", "*[A]^+:#=AE", " :[A]^+ =EY",
        "*[A]^%=EY", " [ARR]*=AXr", "*[ARR]*=AEr", " :[AR] =AAr", "*[AR] =ER", "*[AR]*=AAr", "*[AIR]*=EHr", "*[AI]*=EY",
        "*[AY]*=EY", "*[AU]*=AO", "#:[AL] =AXl", "#:[ALS] =AXlz", "*[ALK]*=AOk", "*[AL]^=AOl", " :[ABLE]*=EYbAXl",
        "*[ABLE]*=AXbAXl", "*[ANG]+=EYnj", "*[A]*=AE",
        // B
        " [BE]^#=bIH", "*[BEING]*=bIYIHNG", " [BOTH] =bOWTH", " [BUS]#=bIHz", "*[BUIL]*=bIHl", "*[B]*=b",
        // C
        " [CH]^=k", "^E[CH]*=k", "*[CH]*=CH", " S[CI]#=sAY", "*[CI]A=SH", "*[CI]O=SH", "*[CI]EN=SH", "*[C]+=s",
        "*[CK]*=k", "*[COM]%=kAHm", "*[C]*=k",
        // D
        "#:[DED] =dIHd", ".E[D] =d", "#:^E[D] =t", " [DE]^#=dIH", " [DO] =dUW", " [DOES]*=dAHz", " [DOING]*=dUWIHNG",
        " [DOW]*=dAW", "*[DU]A=jUW", "*[D]*=d",
        // E
        "#:[E] =", "':^[E] =", " :[E] =IY", "#[ED] =d", "#:[E]D =", "*[EV]ER=EHv", "*[E]^%=IY", "*[ERI]#=IYrIY",
        "*[ERI]*=EHrIH", "#:[ER]#=ER", "*[ER]#=EHr", "*[ER]*=ER", " [EVEN]*=IYvEHn", "#:[E]W=", "T[EW]*=UW", "S[EW]*=UW",
        "R[EW]*=UW", "D[EW]*=UW", "L[EW]*=UW", "Z[EW]*=UW", "N[EW]*=UW", "J[EW]*=UW", "TH[EW]*=UW", "CH[EW]*=UW",
        "SH[EW]*=UW", "*[EW]*=yUW", "*[E]O=IY", "#:S[ES] =IHz", "#:C[ES] =IHz", "#:G[ES] =IHz", "#:Z[ES] =IHz",
        "#:X[ES] =IHz", "#:J[ES] =IHz", "#:CH[ES] =IHz", "#:SH[ES] =IHz", "#:[E]S =", "#:[ELY] =lIY", "#:[EMENT]*=mEHnt",
        "*[EFUL]*=fUHl", "*[EE]*=IY", "*[EARN]*=ERn", " [EAR]^=ER", "*[EAD]*=EHd", "#:[EA] =IYAX", "*[EA]SU=EH",
        "*[EA]*=IY", "*[EIGH]*=EY", "*[EI]*=IY", " [EYE]*=AY", "*[EY]*=IY", "*[EU]*=yUW", "*[E]*=EH",
        // F
        "*[FUL]*=fUHl", "*[F]*=f",
        // G
        "*[GIV]*=gIHv", " [G]I^=g", "*[GE]T=gEH", "SU[GGES]*=gjEHs", "*[GG]*=g", " B#[G]*=g", "*[G]+=j",
        "*[GREAT]*=grEYt", "#[GH]*=", "*[G]*=g",
        // H
        " [HAV]*=hAEv", " [HERE]*=hIYr", " [HOUR]*=AWER", "*[HOW]*=hAW", "*[H]#=h", "*[H]*=",
        // I
        " [IN]*=IHn", " [I] =AY", "*[IN]D=AYn", "*[IER]*=IYER", "#:R[IED]*=IYd", "*[IED] =AYd", "*[IEN]*=IYEHn",
        "*[IE]T=AYEH", " :[I]%=AY", "*[I]%=IY", "*[IE]*=IY", "*[I]^+:#=IH", "*[IR]#=AYr", "*[IZ]%=AYz", "*[IS]%=AYz",
        "*[I]D%=AY", "+^[I]^+=IH", "*[I]T%=AY", "#:^[I]^+=IH", "*[I]^+=AY", "*[IR]*=ER", "*[IGH]*=AY", "*[ILD]*=AYld",
        "*[IGN] =AYn", "*[IGN]^=AYn", "*[IGN]%=AYn", "*[IQUE]*=IYk", "*[I]*=IH",
        // J, K
        "*[J]*=j", " [K]N=", "*[K]*=k",
        // L, M
        "*[LO]C#=lOW", "L[L]*=", "#:^[L]%=AXl", "*[LEAD]*=lIYd", "*[L]*=l", "*[MOV]*=mUWv", "*[M]*=m",
        // N
        "E[NG]+=nj", "*[NG]R=NGg", "*[NG]#=NGg", "*[NGL]%=NGgAXl", "*[NG]*=NG", "*[NK]*=NGk", " [NOW] =nAW", "*[N]*=n",
        // O
        "*[OF] =AXv", "*[OROUGH]*=EROW", "#:[OR] =ER", "#:[ORS] =ERz", "*[OR]*=AOr", " [ONE]*=wAHn", "*[OW]*=OW",
        " [OVER]*=OWvER", "*[OV]*=AHv", "*[O]^%=OW", "*[O]^EN=OW", "*[O]^I#=OW", "*[OL]D=OWl", "*[OUGHT]*=AOt",
        "*[OUGH]*=AHf", " [OU]*=AW", "H[OU]S#=AW", "*[OUS]*=AXs", "*[OUR]*=AOr", "*[OULD]*=UHd", "^[OU]^L=AH",
        "*[OUP]*=UWp", "*[OU]*=AW", "*[OY]*=OY", "*[OING]*=OWIHNG", "*[OI]*=OY", "*[OOR]*=AOr", "*[OOK]*=UHk",
        "*[OOD]*=UHd", "*[OO]*=UW", "*[O]E=OW", "*[O] =OW", "*[OA]*=OW", " [ONLY]*=OWnlIY", " [ONCE]*=wAHns",
        "*[ON'T]*=OWnt", "C[O]N=AA", "*[O]NG=AO", " :^[O]N=AH", "I[ON]*=AXn", "#:[ON] =AXn", "#^[ON]*=AXn",
        "*[O]ST =OW", "*[OF]^=AOf", "*[OTHER]*=AHDHER", "*[OSS] =AOs", "#:^[OM]*=AHm", "*[O]*=AA",
        // P, Q, R
        "*[PH]*=f", "*[PEOP]*=pIYp", "*[POW]*=pAW", "*[PUT] =pUHt", "*[P]*=p", "*[QUAR]*=kwAOr", "*[QU]*=kw", "*[Q]*=k",
        " [RE]^#=rIY", "*[R]*=r",
        // S
        "*[SH]*=SH", "#[SION]*=ZHAXn", "*[SOME]*=sAHm", "#[SUR]#=ZHER", "*[SUR]#=SHER", "#[SU]#=ZHUW", "#[SSU]#=SHUW",
        "#[SED] =zd", "#[S]#=z", "*[SAID]*=sEHd", "^[SION]*=SHAXn", "*[S]S=", ".[S] =z", "#:.E[S] =z", "#:^##[S] =z",
        "#:^#[S] =s", "U[S] =s", " :#[S] =z", " [SCH]*=sk", "*[S]C+=", "#[SM]*=zm", "#[SN]'=zAXn", "*[S]*=s",
        // T
        " [THE] =DHAX", "*[TO] =tUW", "*[THAT] =DHAEt", " [THIS] =DHIHs", " [THEY]*=DHEY", " [THERE]*=DHEHr",
        "*[THER]*=DHER", "*[THEIR]*=DHEHr", " [THAN] =DHAEn", " [THEM] =DHEHm", "*[THESE] =DHIYz", " [THEN]*=DHEHn",
        "*[THROUGH]*=THrUW", "*[THOSE]*=DHOWz", "*[THOUGH] =DHOW", " [THUS]*=DHAHs", "*[TH]*=TH", "#:[TED] =tIHd",
        "S[TI]#N=CH", "*[TI]O=SH", "*[TI]A=SH", "*[TIEN]*=SHAXn", "*[TUR]#=CHER", "*[TU]A=CHUW", " [TWO]*=tUW", "*[T]*=t",
        // U
        " [UN]I=yUWn", " [UN]*=AHn", " [UPON]*=AXpAOn", "T[UR]#=UHr", "S[UR]#=UHr", "R[UR]#=UHr", "D[UR]#=UHr",
        "L[UR]#=UHr", "Z[UR]#=UHr", "N[UR]#=UHr", "J[UR]#=UHr", "TH[UR]#=UHr", "CH[UR]#=UHr", "SH[UR]#=UHr",
        "*[UR]#=yUHr", "*[UR]*=ER", "*[U]^ =AH", "*[U]^^=AH", "*[UY]*=AY", " G[U]#=", "G[U]%=", "G[U]#=w", "#N[U]*=yUW",
        "T[U]*=UW", "S[U]*=UW", "R[U]*=UW", "D[U]*=UW", "L[U]*=UW", "Z[U]*=UW", "N[U]*=UW", "J[U]*=UW", "TH[U]*=UW",
        "CH[U]*=UW", "SH[U]*=UW", "*[U]*=yUW",
        // V, W, X, Y, Z
        "*[VIEW]*=vyUW", "*[V]*=v", " [WERE]*=wER", "*[WA]S=wAA", "*[WA]T=wAA", "*[WHERE]*=WHEHr", "*[WHAT]*=WHAAt",
        "*[WHOL]*=hOWl", "*[WHO]*=hUW", "*[WH]*=WH", "*[WAR]*=wAOr", "*[WOR]^=wER", "*[WR]*=r", "*[W]*=w", "*[X]*=ks",
        "*[YOUNG]*=yAHNG", " [YOU]*=yUW", " [YES]*=yEHs", " [Y]*=y", "#:^[Y] =IY", "#:^[Y]I=IY", " :[Y] =AY",
        " :[Y]#=AY", " :[Y]^+:#=IH", " :[Y]^#=AY", "*[Y]*=IH", "*[Z]*=z"
    ];

    /// <summary>The consonant clusters an English word can start with.</summary>
    private static readonly HashSet<string> Onsets =
    [
        "BL", "BR", "CH", "CL", "CR", "DR", "DW", "FL", "FR", "GL", "GR", "GN", "KL", "KN", "KR", "PH", "PL", "PR", "PS", "QU",
        "SC", "SH", "SK", "SL", "SM", "SN", "SP", "ST", "SW", "TH", "TR", "TW", "WH", "WR", "SCH", "SCR", "SHR", "SPL",
        "SPR", "STR", "THR", "SQU", "CHR", "PHR", "SKR", "Q"
    ];

    private sealed record Rule(string Left, string Match, string Right, string Sounds);

    private static readonly Dictionary<char, Rule[]> Rules = Table
        .Select(Parse)
        .GroupBy(r => r.Match[0])
        .ToDictionary(g => g.Key, g => g.ToArray());

    private static Rule Parse(string text)
    {
        var open = text.IndexOf('[');
        var close = text.IndexOf(']');
        var equals = text.IndexOf('=', close);
        var left = text[..open];
        var right = text[(close + 1)..equals];
        return new Rule(left == "*" ? "" : left, text[(open + 1)..close], right == "*" ? "" : right, text[(equals + 1)..]);
    }

    /// <summary>Whether a word looks sayable by rule (it has a vowel and no long run of consonants), not an abbreviation to spell.</summary>
    public static bool Pronounceable(string word)
    {
        ArgumentNullException.ThrowIfNull(word);
        var letters = word.ToUpperInvariant().Where(char.IsLetter).ToArray();
        if (letters.Length < 2 || !letters.Any(c => "AEIOUY".Contains(c))) return false;
        // It must start as an English word can: a vowel, one consonant, or a consonant cluster English allows
        // ("gpu", "npm", "pdf" can't, so they're abbreviations).
        var start = new string([.. letters.TakeWhile(c => !"AEIOUY".Contains(c))]);
        if (start.Length >= 2 && !Onsets.Contains(start)) return false;
        var run = 0;
        foreach (var c in letters)
        {
            run = "AEIOUY".Contains(c) ? 0 : run + 1;
            if (run >= 5) return false;
        }
        return true;
    }

    /// <summary>The ARPAbet sounds for <paramref name="word"/> (letters only, any case), as the rules give them.</summary>
    public static List<string> Sounds(string word)
    {
        ArgumentNullException.ThrowIfNull(word);
        var text = " " + new string([.. word.ToUpperInvariant().Where(c => char.IsLetter(c) || c == '\'')]) + " ";
        var sounds = new List<string>();
        var i = 1;
        while (i < text.Length - 1)
        {
            var letter = text[i];
            if (!Rules.TryGetValue(letter, out var rules))
            {
                i++;
                continue;
            }
            var matched = false;
            foreach (var rule in rules)
            {
                if (string.CompareOrdinal(text, i, rule.Match, 0, rule.Match.Length) != 0) continue;
                if (!LeftFits(text, i - 1, rule.Left) || !RightFits(text, i + rule.Match.Length, rule.Right)) continue;
                sounds.AddRange(Split(rule.Sounds));
                i += rule.Match.Length;
                matched = true;
                break;
            }
            if (!matched) i++;
        }
        return sounds;
    }

    /// <summary>
    /// Kokoro's phonemes for <paramref name="word"/>, for British ("gb") or American English, stressed on its first
    /// vowel (most names and brands are).
    /// </summary>
    public static string Phonemes(string word, string accent)
    {
        ArgumentNullException.ThrowIfNull(word);
        var british = accent != "us";
        var sounds = Sounds(word);
        var b = new StringBuilder();
        var stressed = false;
        for (var k = 0; k < sounds.Count; k++)
        {
            var sound = sounds[k];
            // Non-rhotic British English: an r is only said before a vowel.
            if (british && sound == "r" && (k + 1 >= sounds.Count || !IsVowel(sounds[k + 1]))) continue;
            if (IsVowel(sound) && !stressed && sound != "AX")
            {
                b.Append('ˈ');
                stressed = true;
            }
            b.Append(Phoneme(sound, british, stressedNow: b.Length > 0 && b[^1] == 'ˈ'));
        }
        return b.ToString();
    }

    private static bool IsVowel(string sound) => sound.Length == 2 && "AEIOU".Contains(sound[0]) && sound is not ("DH" or "NG" or "SH" or "ZH" or "TH" or "CH" or "WH");

    private static string Phoneme(string sound, bool british, bool stressedNow) => sound switch
    {
        "IY" => "iː", "IH" => "ɪ", "EY" => "A", "EH" => "ɛ", "AE" => british ? "a" : "æ", "AA" => british ? "ɑː" : "ɑ",
        "AO" => british ? "ɔː" : "ɔ", "OW" => british ? "Q" : "O", "UH" => "ʊ", "UW" => british ? "uː" : "u",
        "ER" => british ? (stressedNow ? "ɜː" : "ə") : (stressedNow ? "ɜɹ" : "ɚ"), "AX" => "ə", "AH" => "ʌ", "AY" => "I",
        "AW" => "W", "OY" => "Y", "DH" => "ð", "TH" => "θ", "SH" => "ʃ", "ZH" => "ʒ", "CH" => "ʧ", "NG" => "ŋ", "WH" => "w",
        "j" => "ʤ", "y" => "j", "r" => "ɹ", "g" => "ɡ",
        _ => sound
    };

    /// <summary>"AXlIY" → AX, l, IY: two capitals make one sound, a small letter is one.</summary>
    private static IEnumerable<string> Split(string sounds)
    {
        for (var k = 0; k < sounds.Length; k++)
        {
            if (char.IsUpper(sounds[k]) && k + 1 < sounds.Length && char.IsUpper(sounds[k + 1]))
            {
                yield return sounds.Substring(k, 2);
                k++;
            }
            else yield return sounds[k].ToString();
        }
    }

    private static bool IsVowelLetter(char c) => c is 'A' or 'E' or 'I' or 'O' or 'U';

    private static bool IsConsonantLetter(char c) => char.IsLetter(c) && !IsVowelLetter(c);

    /// <summary>Whether the left context matches, read leftwards from <paramref name="at"/>.</summary>
    private static bool LeftFits(string text, int at, string pattern)
    {
        for (var p = pattern.Length - 1; p >= 0; p--)
        {
            var c = pattern[p];
            switch (c)
            {
                case ' ':
                    if (at >= 0 && char.IsLetter(text[at])) return false;
                    at--;
                    break;
                case '#':
                    if (at < 0 || !IsVowelLetter(text[at])) return false;
                    while (at >= 0 && IsVowelLetter(text[at])) at--;
                    break;
                case ':':
                    while (at >= 0 && IsConsonantLetter(text[at])) at--;
                    break;
                case '^':
                    if (at < 0 || !IsConsonantLetter(text[at])) return false;
                    at--;
                    break;
                case '.':
                    if (at < 0 || !"BDVGJLMNRWZ".Contains(text[at])) return false;
                    at--;
                    break;
                case '+':
                    if (at < 0 || !"EIY".Contains(text[at])) return false;
                    at--;
                    break;
                default:
                    if (at < 0 || text[at] != c) return false;
                    at--;
                    break;
            }
        }
        return true;
    }

    /// <summary>Whether the right context matches, read rightwards from <paramref name="at"/>.</summary>
    private static bool RightFits(string text, int at, string pattern)
    {
        for (var p = 0; p < pattern.Length; p++)
        {
            var c = pattern[p];
            switch (c)
            {
                case ' ':
                    if (at < text.Length && char.IsLetter(text[at])) return false;
                    at++;
                    break;
                case '#':
                    if (at >= text.Length || !IsVowelLetter(text[at])) return false;
                    while (at < text.Length && IsVowelLetter(text[at])) at++;
                    break;
                case ':':
                    while (at < text.Length && IsConsonantLetter(text[at])) at++;
                    break;
                case '^':
                    if (at >= text.Length || !IsConsonantLetter(text[at])) return false;
                    at++;
                    break;
                case '.':
                    if (at >= text.Length || !"BDVGJLMNRWZ".Contains(text[at])) return false;
                    at++;
                    break;
                case '+':
                    if (at >= text.Length || !"EIY".Contains(text[at])) return false;
                    at++;
                    break;
                case '%':
                    // A suffix: ER, E, ES, ED, ING or ELY, then the word's end.
                    var rest = text[at..].TrimEnd();
                    var suffix = new[] { "ING", "ELY", "ER", "ES", "ED", "E" }.FirstOrDefault(s => rest.StartsWith(s, StringComparison.Ordinal) && (at + s.Length >= text.Length || !char.IsLetter(text[at + s.Length])));
                    if (suffix == null) return false;
                    at += suffix.Length;
                    break;
                default:
                    if (at >= text.Length || text[at] != c) return false;
                    at++;
                    break;
            }
        }
        return true;
    }
}
