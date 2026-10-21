using System.Diagnostics;
using System.IO;

namespace VRX.Desktop;

// Built-in calibration seeds a game's own profile once. Subsequent tuning belongs
// to the user and is never overwritten by selecting Auto or loading the chooser.
// A tested profile only fills in the geometry settings: it never turns geometry on.
// Auto always means estimated depth; the chooser and the main window show that tested
// settings exist next to the geometry choice.
internal static class TestedStereoProfiles
{
    public const string SystemShock = "system-shock-vkd3d-v4";

    // System Shock (Unreal, centimetre world units): with separation 0.5 the screen plane
    // (zero parallax, w = separation / convergence) sits at 250 units, about 2.5 m in the game,
    // so the HUD (left at screen depth by the layer) is no longer behind the scene. The
    // farthest scenery is pushed apart by 0.002 x the screen width: about 1 cm on a 5.7 m
    // screen. See Profile.MaxGeometryConvergence.
    public const float SystemShockSeparation = .5f, SystemShockConvergence = .002f;

    public static bool Matches(string executable)
    {
        if (string.IsNullOrEmpty(executable)) return false;
        if (!Path.GetFileName(executable).Equals("SystemReShock-Win64-Shipping.exe", StringComparison.OrdinalIgnoreCase)) return false;
        try { return GeometryLaunch.FindSteamAppId(executable) == "482400"; }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException) { return false; }
    }

    public static void Apply(Profile profile)
    {
        ArgumentNullException.ThrowIfNull(profile);
        if (profile.GeometryProfileId is "system-shock-vkd3d-v1" or "system-shock-vkd3d-v2") {
            var previous=GeometryLaunch.ParseArguments(profile.GeometryLaunchArguments ?? "");
            var baseline=new[]{"-dx12","-norhithread","-windowed","-ResX=1280","-ResY=720"};
            var windowed=new[]{"-dx12","-norhithread","-windowed"};
            if (previous.Order(StringComparer.OrdinalIgnoreCase).SequenceEqual(baseline.Order(StringComparer.OrdinalIgnoreCase),StringComparer.OrdinalIgnoreCase) ||
                previous.Order(StringComparer.OrdinalIgnoreCase).SequenceEqual(windowed.Order(StringComparer.OrdinalIgnoreCase),StringComparer.OrdinalIgnoreCase))
                profile.GeometryLaunchArguments = "-dx12 -norhithread";
            profile.GeometryProfileId = "system-shock-vkd3d-v3";
        }
        if (profile.GeometryProfileId == "system-shock-vkd3d-v3")
        {
            // v3 seeded convergence 0, which left the HUD behind the scene. Move only that
            // seeded value; a convergence the player chose is kept.
            if (profile.GeometryConvergence == 0)
            {
                Debug.WriteLine($"[Tested profile] {profile.ExecutablePath}: v3 convergence 0 -> {SystemShockConvergence}");
                profile.GeometryConvergence = SystemShockConvergence;
            }
            profile.GeometryProfileId = SystemShock;
            return;
        }
        if (!string.IsNullOrEmpty(profile.GeometryProfileId) || !Matches(profile.ExecutablePath)) return;
        Debug.WriteLine($"[Tested profile] seeding {SystemShock} for {profile.ExecutablePath}");
        profile.GeometryProfileId = SystemShock;
        bool untouched = profile.GeometrySeparation == Profile.DefaultGeometrySeparation &&
            (profile.GeometryConvergence == Profile.DefaultGeometryConvergence || profile.GeometryConvergence == 0);
        if (untouched)
        {
            profile.GeometrySeparation = SystemShockSeparation;
            profile.GeometryConvergence = SystemShockConvergence;
        }
        profile.GeometrySteamAppId = string.IsNullOrEmpty(profile.GeometrySteamAppId) ? "482400" : profile.GeometrySteamAppId;
        profile.GeometryLaunchArguments ??= "-dx12 -norhithread";
    }
}
