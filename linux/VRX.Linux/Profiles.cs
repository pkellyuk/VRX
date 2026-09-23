using System.Globalization;
using System.Text.Json;

namespace Vrx.Linux;

// A named Linux profile. The file format matches the earlier PyQt controller
// (linux-profiles.json, version 2), so saved profiles carry over. Linux
// profiles are independent of the Windows desktop app's.
public sealed record LinuxProfile(
    bool Cuda, double Width, double Distance, double Height, double Horizontal, double Strength,
    int Room, int Glass, int Reflect, int Light, string LightColor, int Timing = LinuxProfile.TimingDelayed)
{
    // Game frame timing (frame_timing.h): which captured frame is shown with the depth.
    // Linux defaults to delayed, which looked smoother than latest on the PICO 4.
    public const int TimingLatest = 0, TimingDelayed = 1, TimingMatched = 2;
    public static readonly string[] TimingNames = { "latest", "delayed", "matched" };

    public static readonly LinuxProfile Default =
        new(true, 2.0, 2.0, 0.0, 0.0, 1.0, 30, 60, 25, 30, "#FFB46B", TimingDelayed);

    // The same ranges the engine accepts (live_settings.h).
    public static readonly (double Low, double High) WidthRange = (0.5, 10.0);
    public static readonly (double Low, double High) DistanceRange = (0.5, 8.0);
    public static readonly (double Low, double High) HeightRange = (-2.0, 2.0);
    public static readonly (double Low, double High) HorizontalRange = (-3.0, 3.0);
    public static readonly (double Low, double High) StrengthRange = (0.0, 2.0);
    public const int PercentLow = 0, PercentHigh = 100;

    // Checks every value; throws ArgumentException naming the first bad one.
    public LinuxProfile Normalized()
    {
        CheckRange(nameof(Width), Width, WidthRange);
        CheckRange(nameof(Distance), Distance, DistanceRange);
        CheckRange(nameof(Height), Height, HeightRange);
        CheckRange(nameof(Horizontal), Horizontal, HorizontalRange);
        CheckRange(nameof(Strength), Strength, StrengthRange);
        foreach (var (name, value) in new[] { (nameof(Room), Room), (nameof(Glass), Glass),
                                              (nameof(Reflect), Reflect), (nameof(Light), Light) })
            if (value < PercentLow || value > PercentHigh)
                throw new ArgumentException($"{name} must be between {PercentLow} and {PercentHigh}");
        if (Timing < TimingLatest || Timing > TimingMatched)
            throw new ArgumentException("Timing must be latest, delayed or matched");
        return this with { LightColor = NormalizeColor(LightColor) };
    }

    public int LightRgb => int.Parse(NormalizeColor(LightColor).AsSpan(1), NumberStyles.HexNumber,
                                     CultureInfo.InvariantCulture);

    public static string NormalizeColor(string color)
    {
        var text = color.Trim();
        if (text.Length != 7 || text[0] != '#' || !text.Skip(1).All(Uri.IsHexDigit))
            throw new ArgumentException("Light colour must be #RRGGBB");
        return text.ToUpperInvariant();
    }

    static void CheckRange(string name, double value, (double Low, double High) range)
    {
        if (!double.IsFinite(value) || value < range.Low || value > range.High)
            throw new ArgumentException(string.Create(CultureInfo.InvariantCulture,
                $"{name} must be between {range.Low} and {range.High}"));
    }
}

public static class ProfileStore
{
    public static string ConfigDirectory
    {
        get
        {
            var xdg = Environment.GetEnvironmentVariable("XDG_CONFIG_HOME");
            var root = string.IsNullOrEmpty(xdg)
                ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".config")
                : xdg;
            return Path.Combine(root, "vrx");
        }
    }

    public static string DefaultPath => Path.Combine(ConfigDirectory, "linux-profiles.json");

    public static SortedDictionary<string, LinuxProfile> Load(string path)
    {
        if (!File.Exists(path))
            return new(StringComparer.Ordinal) { ["Default"] = LinuxProfile.Default };
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            var root = document.RootElement;
            if (!root.TryGetProperty("version", out var version) || version.ValueKind != JsonValueKind.Number ||
                version.GetInt32() is not (1 or 2) ||
                !root.TryGetProperty("profiles", out var profiles) || profiles.ValueKind != JsonValueKind.Object)
                throw new FormatException("unsupported profile format");
            var result = new SortedDictionary<string, LinuxProfile>(StringComparer.Ordinal);
            foreach (var entry in profiles.EnumerateObject())
                result[entry.Name] = Parse(entry.Value);
            if (result.Count == 0) result["Default"] = LinuxProfile.Default;
            return result;
        }
        catch (Exception error) when (error is JsonException or FormatException or ArgumentException
                                          or InvalidOperationException or IOException)
        {
            throw new InvalidDataException($"Cannot load Linux profiles: {error.Message}", error);
        }
    }

    // Missing values take the defaults, as version 1 files lack the room controls.
    static LinuxProfile Parse(JsonElement value)
    {
        if (value.ValueKind != JsonValueKind.Object || !value.TryGetProperty("cuda", out var cuda) ||
            cuda.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
            throw new FormatException("profile must contain a CUDA choice");
        var d = LinuxProfile.Default;
        double Number(string key, double fallback) =>
            !value.TryGetProperty(key, out var v) ? fallback :
            v.ValueKind == JsonValueKind.Number ? v.GetDouble() : throw new FormatException($"{key} must be a number");
        int Percent(string key, int fallback) =>
            !value.TryGetProperty(key, out var v) ? fallback :
            v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var i) ? i :
            throw new FormatException($"{key} must be a whole number");
        int Timing()
        {
            if (!value.TryGetProperty("timing", out var v)) return d.Timing;
            var index = v.ValueKind == JsonValueKind.String ? Array.IndexOf(LinuxProfile.TimingNames, v.GetString()) : -1;
            return index >= 0 ? index : throw new FormatException("timing must be latest, delayed or matched");
        }
        string Color(string key, string fallback) =>
            !value.TryGetProperty(key, out var v) ? fallback :
            v.ValueKind == JsonValueKind.String ? v.GetString()! : throw new FormatException($"{key} must be text");
        return new LinuxProfile(cuda.GetBoolean(),
            Number("width", d.Width), Number("distance", d.Distance), Number("height", d.Height),
            Number("horizontal", d.Horizontal), Number("strength", d.Strength),
            Percent("room", d.Room), Percent("glass", d.Glass), Percent("reflect", d.Reflect),
            Percent("light", d.Light), Color("light_color", d.LightColor), Timing()).Normalized();
    }

    // Written to a temporary file and renamed, so a crash never leaves half a file.
    public static void Save(string path, IReadOnlyDictionary<string, LinuxProfile> profiles)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + ".tmp";
        using (var stream = File.Create(temporary))
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            writer.WriteNumber("version", 2);
            writer.WriteStartObject("profiles");
            foreach (var (name, p) in profiles)
            {
                writer.WriteStartObject(name);
                writer.WriteBoolean("cuda", p.Cuda);
                writer.WriteNumber("width", p.Width);
                writer.WriteNumber("distance", p.Distance);
                writer.WriteNumber("height", p.Height);
                writer.WriteNumber("horizontal", p.Horizontal);
                writer.WriteNumber("strength", p.Strength);
                writer.WriteNumber("room", p.Room);
                writer.WriteNumber("glass", p.Glass);
                writer.WriteNumber("reflect", p.Reflect);
                writer.WriteNumber("light", p.Light);
                writer.WriteString("light_color", p.LightColor);
                writer.WriteString("timing", LinuxProfile.TimingNames[p.Timing]);
                writer.WriteEndObject();
            }
            writer.WriteEndObject();
            writer.WriteEndObject();
        }
        File.Move(temporary, path, overwrite: true);
    }
}
