using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace VRX.Desktop;

public static class StereoPaths
{
    public const string Auto = "auto", GeometryAuto = "geometry", Depth = "depth", Vulkan = "vulkan", Dxvk = "dxvk", Vkd3d = "vkd3d", Dx12 = "dx12";
    public static bool Valid(string? id) => id is Auto or GeometryAuto or Depth or Vulkan or Dxvk or Vkd3d or Dx12;
    // The native DX12 proxy is a development experiment: a release never treats it as geometry
    // (a profile that saved it loads as depth, see Profile.SelectedStereoBackend).
    public static bool Geometry(string id) => id is Vulkan or Dxvk or Vkd3d
#if DEBUG
        or Dx12
#endif
        ;
    public static bool Renderer(string? id) => id is GeometryAuto or Vulkan or Dxvk or Vkd3d;
    public static string Mode(string id) => id is Auto or Depth ? id : GeometryAuto;
    public static string FromGeometryChoice(bool enabled,string backend) => enabled ? Geometry(backend) ? backend : Vulkan : Depth;
    public static string Label(string id) => Loc.Get(id switch { Auto => "PathAuto", GeometryAuto => "PathGeometry", Vulkan => "PathVulkan", Dxvk => "PathDxvk", Vkd3d => "PathVkd3d", Dx12 => "PathDx12", _ => "PathDepth" });
    public static string Session(string executable) => "game-" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(executable).ToUpperInvariant())))[..12].ToLowerInvariant();
}
