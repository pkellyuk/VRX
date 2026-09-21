using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace VRX.Desktop;
public sealed class Profile
{
    public int Version { get; set; } = 1;
    public string ExecutablePath { get; set; } = "";
    public string PreferredWindowTitle { get; set; } = "";
    public double Width { get; set; } = 5.7;
    public double Distance { get; set; } = 3;
    public double Height { get; set; }
    public double Horizontal { get; set; }
    public double Strength { get; set; } = 1;
    public bool Follow { get; set; }
    public bool Stereo { get; set; } = true;
    public bool ForegroundRefinement { get; set; } = true;
    // Game frame timing (frame_timing.h): latest frame (both false), delayed to depth
    // (DelayToDepth, v6 snapshot) or matched to depth (MatchFrameToDepth, which wins if
    // both are set). Profiles saved before DelayToDepth existed load as false.
    public bool MatchFrameToDepth { get; set; }
    public bool DelayToDepth { get; set; }
    // ZipDepth (default) or Depth Anything V2. Sent in the v4 control snapshot; applies live.
    // Profiles saved before this setting existed load as true.
    public bool FastDepthModel { get; set; } = true;
    // Which GPU runs depth (Gpus.cs): "same", "auto" or "name:<GPU name>#<n>".
    // Passed to the engine as --depth-gpu; takes effect at Attach / Play.
    public string DepthGpu { get; set; } = Gpus.Same;
    // Replaced by DepthGpu; read only so profiles from earlier xgpu builds migrate.
    public bool DepthOnSecondGpu { get; set; }
    // XMMODEL.md; sent in the v5 control snapshot, apply live. Steadying blends in the
    // previous depth moved by the GPU's motion estimator: on by default, including for
    // profiles saved before it existed. Fusion runs Depth Anything V2 alongside ZipDepth
    // (needs the fast model, best with a second GPU): off by default.
    public bool SteadyDepth { get; set; } = true;
    public bool FuseModels { get; set; }
    // Sub-pixel stereo warp (v7 snapshot, applies live). Whole-pixel shifts quantise
    // smoothly receding surfaces into ~25 depth bands, which look like ridges on
    // uniform texture ("ploughed field"); sub-pixel removes them at the cost of a
    // very slight softening. On by default, including for older profiles.
    public bool SubpixelWarp { get; set; } = true;

    // Curved screen: 0 flat (default) .. 100 fully wrapped. The wrap this stands for
    // lives in bench/native/openxr/screen_curve.h (kCurveMaxWrap).
    public int ScreenCurve { get; set; }

    // Ambilight: the picture's edge colours spread around the screen.
    public bool Ambilight { get; set; }

    // The glow's brightness next to the screen, percent.
    public int AmbilightStrength { get; set; } = 85;

    // The colour around the screen, #RRGGBB. Black shows nothing, as before.
    public string WorldColor { get; set; } = "#000000";

    // The room lit by the screen (room.h): 0 off, 1..100 how pale (reflective) its walls
    // are. Needs the fixed screen.
    public int Room { get; set; }

    // With the room (v11 snapshot, all live, all off by default): glass walls, 0 solid .. 100
    // clear; reflections in the glass and the floor, 0 off .. 100; a soft ceiling panel light,
    // 0 off .. 100, and its colour, #RRGGBB (3000 K by default). Glass or reflections above 0
    // also show the frames and the 1 m floor tiles.
    public const string DefaultRoomLightColor = "#FFB46B";
    public int RoomGlass { get; set; }
    public int RoomReflections { get; set; }
    public int RoomLight { get; set; }
    public string RoomLightColor { get; set; } = DefaultRoomLightColor;

    // "#RRGGBB" or "RRGGBB" (any case) -> 0xRRGGBB.
    public static bool TryParseColor(string? text, out int rgb)
    {
        rgb = 0;
        if (string.IsNullOrWhiteSpace(text)) return false;
        string hex = text.Trim();
        if (hex.StartsWith('#')) hex = hex[1..];
        if (hex.Length != 6) return false;
        return int.TryParse(hex, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out rgb);
    }

    public static string FormatColor(int rgb) => "#" + (rgb & 0xFFFFFF).ToString("X6", CultureInfo.InvariantCulture);
    public bool AutoDismiss { get; set; } = true;
    public int RecenterKey { get; set; } = 0xBB;
    public int MenuKey { get; set; } = 0x77;

    public bool Valid() => Version == 1 && Range(Width, 1, 10) && Range(Distance, 1, 8) &&
        Range(Height, -2, 2) && Range(Horizontal, -3, 3) && Range(Strength, 0, 2) &&
        ScreenCurve is >= 0 and <= 100 && AmbilightStrength is >= 0 and <= 100 && TryParseColor(WorldColor, out _) && Room is >= 0 and <= 100 &&
        RoomGlass is >= 0 and <= 100 && RoomReflections is >= 0 and <= 100 && RoomLight is >= 0 and <= 100 && TryParseColor(RoomLightColor, out _) &&
        RecenterKey is > 0 and < 255 && MenuKey is > 0 and < 255 && RecenterKey != MenuKey && Gpus.ValidId(DepthGpu);
    private static bool Range(double value, double min, double max) => double.IsFinite(value) && value >= min && value <= max;
    public string Control(uint recenter, uint menu, bool stop) => FormattableString.Invariant(
        $"VRX 11 {Width:F3} {Distance:F3} {Height:F3} {Horizontal:F3} {Strength:F3} {(Follow ? 1 : 0)} {(Stereo ? 1 : 0)} {(AutoDismiss ? 1 : 0)} {RecenterKey} {MenuKey} {recenter} {menu} {(stop ? 1 : 0)} {(ForegroundRefinement ? 1 : 0)} {(MatchFrameToDepth ? 1 : 0)} {(FastDepthModel ? 1 : 0)} {(SteadyDepth ? 1 : 0)} {(FuseModels ? 1 : 0)} {(DelayToDepth && !MatchFrameToDepth ? 1 : 0)} {(SubpixelWarp ? 1 : 0)} {ScreenCurve} {(Ambilight ? 1 : 0)} {AmbilightStrength} {(TryParseColor(WorldColor, out int world) ? world : 0)} {Room} {RoomGlass} {RoomReflections} {RoomLight} {(TryParseColor(RoomLightColor, out int light) ? light : 0xFFB46B)}\n");
}

public sealed class ProfileStore(string root)
{
    public string Root { get; } = root;
    // Settings a game with no profile starts from ("Make base settings"). The game's
    // own path and window are never part of it.
    public string BaseFile => Path.Combine(Root, "base-settings.json");
    public bool HasBase => File.Exists(BaseFile);
    public void SaveBase(Profile profile)
    {
        if (!profile.Valid()) throw new InvalidDataException("Settings are outside the allowed range or shortcut keys conflict.");
        var copy = JsonSerializer.Deserialize<Profile>(JsonSerializer.Serialize(profile))!;
        copy.ExecutablePath = "";
        copy.PreferredWindowTitle = "";
        AtomicWrite(BaseFile, JsonSerializer.Serialize(copy, new JsonSerializerOptions { WriteIndented = true }));
    }
    // The saved base, or VRX's own defaults if there is none (or it is unreadable).
    public Profile BaseSettings()
    {
        try
        {
            if (File.Exists(BaseFile))
            {
                var saved = JsonSerializer.Deserialize<Profile>(File.ReadAllText(BaseFile));
                if (saved != null)
                {
                    saved.ExecutablePath = "";
                    saved.PreferredWindowTitle = "";
                    saved.DepthGpu ??= Gpus.Same;
                    saved.WorldColor ??= "#000000";
                    saved.RoomLightColor ??= Profile.DefaultRoomLightColor;
                    if (saved.Valid()) return saved;
                }
            }
        }
        catch (Exception ex) when (ex is IOException or JsonException) { }
        return new Profile();
    }
    // Every game that has its own saved settings.
    public IReadOnlyList<string> SavedProfileFiles()
    {
        string folder = Path.Combine(Root, "profiles");
        return Directory.Exists(folder) ? Directory.GetFiles(folder, "*.json") : [];
    }

    // "Apply to all": every saved game takes these settings, keeping its own path and
    // window. A file that cannot be read, or does not belong to the game it names, is
    // left exactly as it was and counted in `skipped`; one that cannot be written
    // (read-only, locked by another program) is counted in `failed`, and the rest
    // still go ahead. Games not set up yet are not touched: they still start from the
    // base settings.
    public int ApplyToAll(Profile settings, out int skipped, out int failed)
    {
        ArgumentNullException.ThrowIfNull(settings);
        skipped = 0;
        failed = 0;
        if (!settings.Valid()) throw new InvalidDataException("Settings are outside the allowed range or shortcut keys conflict.");

        int applied = 0;
        foreach (string file in SavedProfileFiles())
        {
            Profile? saved;
            try
            {
                saved = JsonSerializer.Deserialize<Profile>(File.ReadAllText(file));
                if (saved == null || string.IsNullOrWhiteSpace(saved.ExecutablePath) ||
                    !string.Equals(Path.GetFullPath(FileFor(saved.ExecutablePath)), Path.GetFullPath(file), StringComparison.OrdinalIgnoreCase))
                {
                    skipped++;
                    continue;
                }
            }
            catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
            {
                skipped++;
                continue;
            }
            try
            {
                var copy = JsonSerializer.Deserialize<Profile>(JsonSerializer.Serialize(settings))!;
                copy.ExecutablePath = saved.ExecutablePath;
                copy.PreferredWindowTitle = saved.PreferredWindowTitle;
                Save(copy);
                applied++;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { failed++; }
        }
        return applied;
    }

    public string FileFor(string executable) => Path.Combine(Root, "profiles",
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(executable).ToUpperInvariant()))) + ".json");
    public Profile Load(string executable)
    {
        string file = FileFor(executable);
        if (!File.Exists(file))
        {
            var start = BaseSettings();
            start.ExecutablePath = executable;
            return start;
        }
        var profile = JsonSerializer.Deserialize<Profile>(File.ReadAllText(file));
        if (profile != null)
        {
            profile.DepthGpu ??= Gpus.Same;
            profile.WorldColor ??= "#000000";
            profile.RoomLightColor ??= Profile.DefaultRoomLightColor;
            // Earlier xgpu builds stored a checkbox; it meant "any other GPU".
            if (profile.DepthOnSecondGpu && profile.DepthGpu == Gpus.Same) profile.DepthGpu = Gpus.Auto;
            profile.DepthOnSecondGpu = false;
        }
        if (profile == null || !profile.Valid() || !string.Equals(profile.ExecutablePath, executable, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("This game's saved settings are invalid. They have not been overwritten.");
        return profile;
    }
    public void Save(Profile profile)
    {
        if (!profile.Valid()) throw new InvalidDataException("Settings are outside the allowed range or shortcut keys conflict.");
        AtomicWrite(FileFor(profile.ExecutablePath), JsonSerializer.Serialize(profile, new JsonSerializerOptions { WriteIndented = true }));
    }
    public static void AtomicWrite(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllText(temp, text, new UTF8Encoding(false)); File.Move(temp, path, true); }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }
}
