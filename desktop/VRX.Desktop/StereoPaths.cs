using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace VRX.Desktop;

public static class StereoPaths
{
    public const string Depth = "depth", Vulkan = "vulkan", Dxvk = "dxvk", Dx12 = "dx12";
    public static bool Valid(string? id) => id is Depth or Vulkan or Dxvk or Dx12;
    public static bool Geometry(string id) => id is Vulkan or Dxvk or Dx12;
    public static string FromGeometryChoice(bool enabled,string backend) => enabled ? Geometry(backend) ? backend : Vulkan : Depth;
    public static string Label(string id) => Loc.Get(id switch { Vulkan => "PathVulkan", Dxvk => "PathDxvk", Dx12 => "PathDx12", _ => "PathDepth" });
    public static string Session(string executable) => "game-" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(executable).ToUpperInvariant())))[..12].ToLowerInvariant();
}
