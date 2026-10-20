using System.IO;

namespace VRX.Desktop;

// Built-in calibration seeds a game's own profile once. Subsequent tuning belongs
// to the user and is never overwritten by selecting Auto or loading the chooser.
internal static class TestedStereoProfiles
{
    public const string SystemShock = "system-shock-vkd3d-v1";
    public static bool Matches(string executable)
    {
        if (!Path.GetFileName(executable).Equals("SystemReShock-Win64-Shipping.exe", StringComparison.OrdinalIgnoreCase)) return false;
        try { return GeometryLaunch.FindSteamAppId(executable) == "482400"; }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return false; }
    }
    public static void Apply(Profile profile)
    {
        if (!string.IsNullOrEmpty(profile.GeometryProfileId) || !Matches(profile.ExecutablePath)) return;
        profile.GeometryProfileId = SystemShock;
        if (profile.GeometrySeparation == .032f && profile.GeometryConvergence == 0) profile.GeometrySeparation = .5f;
        profile.GeometrySteamAppId = string.IsNullOrEmpty(profile.GeometrySteamAppId) ? "482400" : profile.GeometrySteamAppId;
        profile.GeometryLaunchArguments ??= "-dx12 -norhithread -windowed -ResX=1280 -ResY=720";
    }
}
