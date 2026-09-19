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
    public bool MatchFrameToDepth { get; set; }
    // ZipDepth (default) or Depth Anything V2. Sent in the v4 control snapshot; applies live.
    // Profiles saved before this setting existed load as true.
    public bool FastDepthModel { get; set; } = true;
    // Which GPU runs depth (Gpus.cs): "same", "auto" or "name:<GPU name>#<n>".
    // Passed to the engine as --depth-gpu; takes effect at Attach / Play.
    public string DepthGpu { get; set; } = Gpus.Same;
    // Replaced by DepthGpu; read only so profiles from earlier xgpu builds migrate.
    public bool DepthOnSecondGpu { get; set; }
    // Experimental (XMMODEL.md), sent in the v5 control snapshot, apply live:
    // steadying blends in the previous depth moved by the GPU's motion estimator;
    // fusion runs Depth Anything V2 alongside ZipDepth (needs the fast model).
    // Profiles saved before these existed load as false.
    public bool SteadyDepth { get; set; }
    public bool FuseModels { get; set; }
    public bool AutoDismiss { get; set; } = true;
    public int RecenterKey { get; set; } = 0xBB;
    public int MenuKey { get; set; } = 0x77;

    public bool Valid() => Version == 1 && Range(Width, 1, 10) && Range(Distance, 1, 8) &&
        Range(Height, -2, 2) && Range(Horizontal, -3, 3) && Range(Strength, 0, 2) &&
        RecenterKey is > 0 and < 255 && MenuKey is > 0 and < 255 && RecenterKey != MenuKey && Gpus.ValidId(DepthGpu);
    private static bool Range(double value, double min, double max) => double.IsFinite(value) && value >= min && value <= max;
    public string Control(uint recenter, uint menu, bool stop) => FormattableString.Invariant(
        $"VRX 5 {Width:F3} {Distance:F3} {Height:F3} {Horizontal:F3} {Strength:F3} {(Follow ? 1 : 0)} {(Stereo ? 1 : 0)} {(AutoDismiss ? 1 : 0)} {RecenterKey} {MenuKey} {recenter} {menu} {(stop ? 1 : 0)} {(ForegroundRefinement ? 1 : 0)} {(MatchFrameToDepth ? 1 : 0)} {(FastDepthModel ? 1 : 0)} {(SteadyDepth ? 1 : 0)} {(FuseModels ? 1 : 0)}\n");
}

public sealed class ProfileStore(string root)
{
    public string Root { get; } = root;
    public string FileFor(string executable) => Path.Combine(Root, "profiles",
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(executable).ToUpperInvariant()))) + ".json");
    public Profile Load(string executable)
    {
        string file = FileFor(executable);
        if (!File.Exists(file)) return new Profile { ExecutablePath = executable };
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
