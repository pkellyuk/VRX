using System.IO;

namespace VRX.VrexBridge;

internal static class VrxDataDirectory
{
    internal static string Root { get; } = Environment.GetEnvironmentVariable("VRX_PROFILE_DIRECTORY") is { Length: > 0 } profile
        ? Path.GetFullPath(profile)
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VRX");
}
