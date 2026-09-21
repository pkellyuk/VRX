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
    public bool AutoDismiss { get; set; } = true;
    public int RecenterKey { get; set; } = 0xBB;
    public int MenuKey { get; set; } = 0x77;

    public bool Valid() => Version == 1 && Range(Width, 1, 10) && Range(Distance, 1, 8) &&
        Range(Height, -2, 2) && Range(Horizontal, -3, 3) && Range(Strength, 0, 2) &&
        ScreenCurve is >= 0 and <= 100 &&
        RecenterKey is > 0 and < 255 && MenuKey is > 0 and < 255 && RecenterKey != MenuKey && Gpus.ValidId(DepthGpu);
    private static bool Range(double value, double min, double max) => double.IsFinite(value) && value >= min && value <= max;
    public string Control(uint recenter, uint menu, bool stop) => FormattableString.Invariant(
        $"VRX 8 {Width:F3} {Distance:F3} {Height:F3} {Horizontal:F3} {Strength:F3} {(Follow ? 1 : 0)} {(Stereo ? 1 : 0)} {(AutoDismiss ? 1 : 0)} {RecenterKey} {MenuKey} {recenter} {menu} {(stop ? 1 : 0)} {(ForegroundRefinement ? 1 : 0)} {(MatchFrameToDepth ? 1 : 0)} {(FastDepthModel ? 1 : 0)} {(SteadyDepth ? 1 : 0)} {(FuseModels ? 1 : 0)} {(DelayToDepth && !MatchFrameToDepth ? 1 : 0)} {(SubpixelWarp ? 1 : 0)} {ScreenCurve} {(Ambilight ? 1 : 0)}\n");
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
                    if (saved.Valid()) return saved;
                }
            }
        }
        catch (Exception ex) when (ex is IOException or JsonException) { }
        return new Profile();
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
