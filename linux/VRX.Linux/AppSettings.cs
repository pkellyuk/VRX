using System.Text.Json;
using System.Text.Json.Serialization;

namespace Vrx.Linux;

// App-wide choices, as the WPF app keeps them: Easy or Expert, the profile in use,
// what the desktop chooser offers, the base settings for new profiles, and which
// sections are open. Stored beside the profiles (linux-app.json).
public sealed class AppSettings
{
    public const string EasyMode = "easy", ExpertMode = "expert";
    public const string SourceAny = "any", SourceWindow = "window", SourceScreen = "screen";

    public string Mode { get; set; } = EasyMode;
    public string Profile { get; set; } = "Default";
    public string Source { get; set; } = SourceAny;
    public LinuxProfile? Base { get; set; }
    public List<string> OpenSections { get; set; } = new() { "GameSection", "ScreenSection" };

    [JsonIgnore] public bool IsExpert => Mode == ExpertMode;

    public static string DefaultPath => Path.Combine(ProfileStore.ConfigDirectory, "linux-app.json");

    static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    // A missing or unreadable file gives the defaults: these are conveniences, not data.
    public static AppSettings Load(string path)
    {
        try
        {
            var settings = File.Exists(path) ? JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(path), Json) : null;
            if (settings == null) return new AppSettings();
            if (settings.Mode is not (EasyMode or ExpertMode)) settings.Mode = EasyMode;
            if (settings.Source is not (SourceAny or SourceWindow or SourceScreen)) settings.Source = SourceAny;
            try { settings.Base = settings.Base?.Normalized(); } catch (ArgumentException) { settings.Base = null; }
            return settings;
        }
        catch (Exception error) when (error is JsonException or IOException or NotSupportedException)
        {
            return new AppSettings();
        }
    }

    public void Save(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(this, Json));
        File.Move(temporary, path, overwrite: true);
    }
}
