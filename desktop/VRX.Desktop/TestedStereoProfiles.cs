using System.IO;

namespace VRX.Desktop;

// Built-in calibration seeds a game's own profile once. Subsequent tuning belongs
// to the user and is never overwritten by selecting Auto or loading the chooser.
internal static class TestedStereoProfiles
{
    public const string SystemShock = "system-shock-vkd3d-v2";
    public static bool Matches(string executable)
    {
        if (!Path.GetFileName(executable).Equals("SystemReShock-Win64-Shipping.exe", StringComparison.OrdinalIgnoreCase)) return false;
        try { return GeometryLaunch.FindSteamAppId(executable) == "482400"; }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return false; }
    }
    public static void Apply(Profile profile)
    {
        if (profile.GeometryProfileId == "system-shock-vkd3d-v1") {
            var previous=GeometryLaunch.ParseArguments(profile.GeometryLaunchArguments ?? "");
            var baseline=new[]{"-dx12","-norhithread","-windowed","-ResX=1280","-ResY=720"};
            if (previous.Order(StringComparer.OrdinalIgnoreCase).SequenceEqual(baseline.Order(StringComparer.OrdinalIgnoreCase),StringComparer.OrdinalIgnoreCase))
                profile.GeometryLaunchArguments = "-dx12 -norhithread -windowed";
            profile.GeometryProfileId = SystemShock;
            return;
        }
        if (!string.IsNullOrEmpty(profile.GeometryProfileId) || !Matches(profile.ExecutablePath)) return;
        profile.GeometryProfileId = SystemShock;
        if (profile.GeometrySeparation == .032f && profile.GeometryConvergence == 0) profile.GeometrySeparation = .5f;
        profile.GeometrySteamAppId = string.IsNullOrEmpty(profile.GeometrySteamAppId) ? "482400" : profile.GeometrySteamAppId;
        profile.GeometryLaunchArguments ??= "-dx12 -norhithread -windowed";
    }
}
