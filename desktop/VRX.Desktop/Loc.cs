using System.Collections;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Resources;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Data;
using System.Windows.Markup;

namespace VRX.Desktop;

// Every user-visible string lives in Strings.resx (neutral = English). The UI culture
// follows Windows (CurrentUICulture); a missing translation falls back to English. To add
// a language, copy Strings.resx to Strings.<culture>.resx (e.g. Strings.de.resx) and
// translate the values, keeping each {0}, {1}... placeholder.
//
// XAML uses {l:Tr Key} for text and {l:TrValue Key, Element=SliderName} for a slider's value
// ("{0:F2} m"); code uses Loc.Get(key) and Loc.Format(key, args...) with the key as a string
// literal (the smoke test checks them) - numbered placeholders, never sentences glued together. Numbers are formatted in the user's
// culture. --pseudo-locale turns every string into an accented, ~35 % longer, bracketed
// version so untranslated or clipped text stands out.
public sealed partial class Loc : INotifyPropertyChanged
{
    public static Loc Instance { get; } = new();
    private static readonly ResourceManager Resources = new("VRX.Desktop.Strings", typeof(Loc).Assembly);
    private static readonly HashSet<string> missing = new(StringComparer.Ordinal);

    public event PropertyChangedEventHandler? PropertyChanged;

    public static bool Pseudo { get; private set; }

    // Bumped when the strings change (pseudo-locale on/off), so value bindings re-format.
    public int Version { get; private set; }

    // Keys asked for that Strings.resx does not have (the smoke test requires none).
    public static IReadOnlyCollection<string> Missing
    {
        get { lock (missing) return missing.ToArray(); }
    }

    public string this[string key] => Get(key);

    public static void SetPseudo(bool on)
    {
        Debug.WriteLine($"[Loc] SetPseudo enter: {Pseudo} -> {on}");
        if (Pseudo == on) return;
        Pseudo = on;
        Instance.Version++;
        Instance.PropertyChanged?.Invoke(Instance, new PropertyChangedEventArgs(Binding.IndexerName));
        Instance.PropertyChanged?.Invoke(Instance, new PropertyChangedEventArgs(nameof(Version)));
        Debug.WriteLine($"[Loc] SetPseudo exit: version {Instance.Version}");
    }

    // The string for `key` in the UI culture (English fallback), pseudo-transformed when on.
    public static string Get(string key)
    {
        string? text = Raw(key);
        if (text == null) return "!" + key + "!";
        return Pseudo ? PseudoTransform(text) : text;
    }

    // The neutral (English) string for `key`, or null when there is none.
    public static string? Raw(string key)
    {
        if (string.IsNullOrEmpty(key))
        {
            Debug.WriteLine("[Loc] Raw: empty key");
            lock (missing) missing.Add("(empty)");
            return null;
        }
        string? text = null;
        try { text = Resources.GetString(key, CultureInfo.CurrentUICulture); }
        catch (MissingManifestResourceException ex) { Debug.WriteLine("[Loc] no resources: " + ex.Message); }
        if (text != null) return text;
        Debug.WriteLine($"[Loc] missing key '{key}'");
        lock (missing) missing.Add(key);
        return null;
    }

    // Formats `key` with the user's culture (numbers, dates).
    public static string Format(string key, params object?[] args) => FormatWith(CultureInfo.CurrentCulture, key, args);

    public static string FormatWith(IFormatProvider? culture, string key, params object?[] args)
    {
        string pattern = Get(key);
        if (args == null || args.Length == 0) return pattern;
        try { return string.Format(culture ?? CultureInfo.CurrentCulture, pattern, args); }
        catch (FormatException ex)
        {
            Debug.WriteLine($"[Loc] bad format for '{key}' with {args.Length} argument(s): {ex.Message}");
            lock (missing) missing.Add(key + " (format)");
            return pattern;
        }
    }

    // Every key in the neutral resources.
    public static IReadOnlyDictionary<string, string> NeutralStrings()
    {
        var all = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var set = Resources.GetResourceSet(CultureInfo.InvariantCulture, true, false);
        if (set == null) return all;
        foreach (DictionaryEntry entry in set)
            if (entry.Key is string key && entry.Value is string value) all[key] = value;
        return all;
    }

    [GeneratedRegex(@"\{\{|\}\}|\{(\d+)(,[-+]?\d+)?(:[^{}]*)?\}")]
    private static partial Regex PlaceholderPattern();

    // The placeholder indexes in a format string ({0}, {1:F2}...), escaped braces ignored.
    public static IReadOnlyList<int> Placeholders(string text)
    {
        var found = new List<int>();
        if (string.IsNullOrEmpty(text)) return found;
        foreach (Match m in PlaceholderPattern().Matches(text))
            if (m.Groups[1].Success) found.Add(int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture));
        return found;
    }

    // "Screen width" -> "[Šćŕééñ ŵíðţĥ ·······]": accented, ~35 % longer and bracketed, with
    // placeholders and escaped braces kept exactly.
    public static string PseudoTransform(string text)
    {
        if (string.IsNullOrEmpty(text)) return text ?? "";
        var result = new StringBuilder(text.Length * 2);
        result.Append('[');
        int last = 0, letters = 0;
        foreach (Match m in PlaceholderPattern().Matches(text))
        {
            letters += Accent(text, last, m.Index, result);
            result.Append(m.Value);
            last = m.Index + m.Length;
        }
        letters += Accent(text, last, text.Length, result);
        int pad = Math.Max(2, (int)Math.Ceiling(Math.Max(letters, 1) * 0.35));
        result.Append(' ').Append('·', pad).Append(']');
        return result.ToString();
    }

    private const string Plain = "aceinosuyzACEINOSUYZdghlrtwDGHLRTWbfkmpvBFKMPVjxJX";
    private const string Accented = "áçéíñóšúýžÅÇÉÍÑÓŠÚÝŽðĝĥļŕţŵÐĜĤĻŔŢŴƀƒķḿƥṽƁƑĶḾƤṼĵẋĴẊ";

    private static int Accent(string text, int from, int to, StringBuilder into)
    {
        int count = 0;
        for (int i = from; i < to; i++)
        {
            char c = text[i];
            int at = Plain.IndexOf(c);
            into.Append(at >= 0 ? Accented[at] : c);
            count++;
        }
        return count;
    }
}

// {l:Tr Key}: the string for Key, updating if the strings change.
[MarkupExtensionReturnType(typeof(object))]
public sealed class TrExtension : MarkupExtension
{
    public TrExtension() { }
    public TrExtension(string key) { Key = key; }

    [ConstructorArgument("key")]
    public string Key { get; set; } = "";

    public override object ProvideValue(IServiceProvider serviceProvider)
    {
        var binding = new Binding("[" + Key + "]") { Source = Loc.Instance, Mode = BindingMode.OneWay };
        return binding.ProvideValue(serviceProvider);
    }
}

// {l:TrValue ValueMetres, Element=WidthSlider}: the element's Value formatted by the
// string for Key ("{0:F2} m") in the window's culture (its Language).
[MarkupExtensionReturnType(typeof(object))]
public sealed class TrValueExtension : MarkupExtension
{
    public TrValueExtension() { }
    public TrValueExtension(string key) { Key = key; }

    [ConstructorArgument("key")]
    public string Key { get; set; } = "";
    public string Element { get; set; } = "";

    public override object ProvideValue(IServiceProvider serviceProvider)
    {
        var binding = new MultiBinding { Converter = LocFormatConverter.Instance, ConverterParameter = Key, Mode = BindingMode.OneWay };
        binding.Bindings.Add(new Binding("Value") { ElementName = Element });
        binding.Bindings.Add(new Binding(nameof(Loc.Version)) { Source = Loc.Instance });
        return binding.ProvideValue(serviceProvider);
    }
}

public sealed class LocFormatConverter : IMultiValueConverter
{
    public static LocFormatConverter Instance { get; } = new();

    public object Convert(object[] values, Type targetType, object parameter, CultureInfo culture)
    {
        if (parameter is not string key || values == null || values.Length == 0 || values[0] == DependencyProperty.UnsetValue) return "";
        return Loc.FormatWith(culture, key, values[0]);
    }

    public object[] ConvertBack(object value, Type[] targetTypes, object parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
