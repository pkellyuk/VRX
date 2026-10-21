using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace VRX.Desktop;

// "Automatic renderer" for true geometry: which graphics API a game uses, read from the
// executable's PE import tables (normal and delay-load) and, when those say nothing, from a
// few well-known engine renderer DLLs next to it (UnityPlayer.dll, Unreal/CryEngine/Source
// renderer modules). Nothing is loaded or run. The answer picks the route VRX supports:
// Vulkan -> the plain layer, Direct3D 11 -> DXVK, Direct3D 12 -> vkd3d-proton. When the
// API is unknown, unsupported (Direct3D 9/10, OpenGL) or ambiguous, it says so instead of
// guessing; the player can then pick a renderer in Expert.
internal static class GraphicsApi
{
    public const string D3D9 = "Direct3D 9", D3D10 = "Direct3D 10", D3D11 = "Direct3D 11", D3D12 = "Direct3D 12", Vulkan = "Vulkan", OpenGL = "OpenGL";

    // Renderer: StereoPaths.Vulkan, Dxvk or Vkd3d, or "" when VRX will not guess. Apis: what
    // was found, for the explanation.
    public sealed record Detection(string Renderer, IReadOnlyList<string> Apis, string Source);

    private static readonly ConcurrentDictionary<string, Detection> Cache = new(StringComparer.OrdinalIgnoreCase);

    // Imported DLL -> API. dxgi.dll alone says nothing (Direct3D 10, 11 and 12 all use it).
    private static readonly Dictionary<string, string> ImportApis = new(StringComparer.OrdinalIgnoreCase)
    {
        ["d3d9.dll"] = D3D9, ["d3d10.dll"] = D3D10, ["d3d10_1.dll"] = D3D10, ["d3d11.dll"] = D3D11,
        ["d3d12.dll"] = D3D12, ["vulkan-1.dll"] = Vulkan, ["opengl32.dll"] = OpenGL,
    };

    // Engine renderer modules whose presence alone names an API.
    private static readonly Dictionary<string, string> EngineModules = new(StringComparer.OrdinalIgnoreCase)
    {
        ["D3D11RHI.dll"] = D3D11, ["D3D12RHI.dll"] = D3D12, ["VulkanRHI.dll"] = Vulkan,                // Unreal (modular builds)
        ["CryRenderD3D11.dll"] = D3D11, ["CryRenderD3D12.dll"] = D3D12, ["CryRenderVulkan.dll"] = Vulkan, // CryEngine
        ["rendersystemdx11.dll"] = D3D11, ["rendersystemvulkan.dll"] = Vulkan,                          // Source 2
        ["shaderapidx9.dll"] = D3D9,                                                                     // Source
    };

    // Engine DLLs whose own imports are read (the executable is only a small launcher).
    private static readonly string[] EngineLibraries = ["UnityPlayer.dll"];

    public static Detection Detect(string executable)
    {
        ArgumentException.ThrowIfNullOrEmpty(executable);
        string key;
        try
        {
            var info = new FileInfo(executable);
            if (!info.Exists) return new Detection("", [], "missing");
            key = $"{info.FullName}|{info.LastWriteTimeUtc.Ticks}|{info.Length}";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            Debug.WriteLine($"[Graphics API] {executable}: {ex.Message}");
            return new Detection("", [], "unreadable");
        }
        return Cache.GetOrAdd(key, _ => DetectUncached(executable));
    }

    private static Detection DetectUncached(string executable)
    {
        Debug.WriteLine($"[Graphics API] Detect enter: {executable}");
        var apis = new SortedSet<string>(StringComparer.Ordinal);
        string source = "imports";
        foreach (string import in Imports(executable))
            if (ImportApis.TryGetValue(import, out string? api)) apis.Add(api);
        string folder = Path.GetDirectoryName(Path.GetFullPath(executable))!;
        if (apis.Count == 0)
        {
            source = "engine";
            foreach (string library in EngineLibraries)
            {
                string path = Path.Combine(folder, library);
                if (!File.Exists(path)) continue;
                foreach (string import in Imports(path))
                    if (ImportApis.TryGetValue(import, out string? api)) apis.Add(api);
            }
            foreach (var (module, api) in EngineModules)
                if (File.Exists(Path.Combine(folder, module))) apis.Add(api);
        }
        if (apis.Count > 1) apis.Remove(OpenGL);   // also imported by many Direct3D programs for small things
        bool unreal = Path.GetFileName(executable).EndsWith("-Shipping.exe", StringComparison.OrdinalIgnoreCase);
        string renderer = Choose(apis, unreal);
        Debug.WriteLine($"[Graphics API] Detect exit: {executable}: {string.Join(", ", apis)} ({source}) -> '{renderer}'");
        return new Detection(renderer, apis.ToArray(), source);
    }

    // One supported API decides the route. Unreal takes a command-line switch for its API
    // (GeometryLaunch.ApiArguments), so of several it prefers DXVK's Direct3D 11 (the most
    // mature translation), then Direct3D 12, then Vulkan. Anything else with several APIs is
    // left to the player.
    internal static string Choose(IReadOnlyCollection<string> apis, bool unreal)
    {
        ArgumentNullException.ThrowIfNull(apis);
        var supported = apis.Where(a => a is D3D11 or D3D12 or Vulkan).ToArray();
        if (unreal && supported.Length > 0)
            return supported.Contains(D3D11) ? StereoPaths.Dxvk : supported.Contains(D3D12) ? StereoPaths.Vkd3d : StereoPaths.Vulkan;
        if (apis.Count != 1 || supported.Length != 1) return "";
        return supported[0] switch { D3D11 => StereoPaths.Dxvk, D3D12 => StereoPaths.Vkd3d, _ => StereoPaths.Vulkan };
    }

    // Why automatic geometry cannot pick a renderer, in plain words.
    public static string Explain(Detection detection)
    {
        ArgumentNullException.ThrowIfNull(detection);
        if (detection.Apis.Count == 0) return Loc.Get("ErrorGeometryApiUnknown");
        var unsupported = detection.Apis.Where(a => a is D3D9 or D3D10 or OpenGL).ToArray();
        if (unsupported.Length == detection.Apis.Count) return Loc.Format("ErrorGeometryApiUnsupported", string.Join(", ", unsupported));
        return Loc.Format("ErrorGeometryApiSeveral", string.Join(", ", detection.Apis));
    }

    // The DLL names in a PE file's import and delay-load import tables. Empty when the file is
    // not a readable PE image. Only headers and tables are read, never the whole file.
    internal static IReadOnlyList<string> Imports(string path)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        var names = new List<string>();
        try
        {
            using var file = File.OpenRead(path);
            using var reader = new BinaryReader(file);
            if (file.Length < 0x40 || reader.ReadUInt16() != 0x5A4D) return names;
            file.Position = 0x3C;
            uint pe = reader.ReadUInt32();
            if (pe > file.Length - 24) return names;
            file.Position = pe;
            if (reader.ReadUInt32() != 0x00004550) return names;
            reader.ReadUInt16();                                    // machine
            int sections = reader.ReadUInt16();
            file.Position += 12;                                    // time stamp, symbols
            int optionalSize = reader.ReadUInt16();
            reader.ReadUInt16();                                    // characteristics
            long optional = file.Position;
            ushort magic = reader.ReadUInt16();
            bool plus = magic == 0x20B;
            if (!plus && magic != 0x10B) return names;
            ulong imageBase;
            file.Position = optional + (plus ? 24 : 28);
            imageBase = plus ? reader.ReadUInt64() : reader.ReadUInt32();
            file.Position = optional + (plus ? 108 : 92);
            uint directories = reader.ReadUInt32();
            long directoryTable = file.Position;
            (uint Rva, uint Size) Directory(int index)
            {
                if (index >= directories) return (0, 0);
                file.Position = directoryTable + index * 8;
                return (reader.ReadUInt32(), reader.ReadUInt32());
            }
            var imports = Directory(1);
            var delay = Directory(13);
            long sectionTable = optional + optionalSize;
            var table = new List<(uint Va, uint Size, uint Raw)>();
            for (int i = 0; i < sections && i < 96; i++)
            {
                file.Position = sectionTable + i * 40 + 8;
                uint virtualSize = reader.ReadUInt32(), va = reader.ReadUInt32(), rawSize = reader.ReadUInt32(), raw = reader.ReadUInt32();
                table.Add((va, Math.Max(virtualSize, rawSize), raw));
            }
            long Offset(uint rva)
            {
                foreach (var (va, size, raw) in table)
                    if (rva >= va && rva < va + size) return raw + (rva - va);
                return -1;
            }
            string? Name(uint rva)
            {
                long at = Offset(rva);
                if (at < 0 || at >= file.Length) return null;
                file.Position = at;
                var text = new StringBuilder();
                for (int c; text.Length < 260 && (c = file.ReadByte()) > 0;) text.Append((char)c);
                return text.Length > 0 ? text.ToString() : null;
            }
            if (imports.Rva != 0)
            {
                long at = Offset(imports.Rva);
                for (int i = 0; at >= 0 && i < 4096; i++, at += 20)
                {
                    file.Position = at + 12;
                    uint name = reader.ReadUInt32();
                    if (name == 0) break;
                    if (Name(name) is { } dll) names.Add(dll);
                }
            }
            if (delay.Rva != 0)
            {
                long at = Offset(delay.Rva);
                for (int i = 0; at >= 0 && i < 4096; i++, at += 32)
                {
                    file.Position = at;
                    uint attributes = reader.ReadUInt32(), name = reader.ReadUInt32();
                    if (name == 0) break;
                    // Old delay-load tables hold addresses rather than RVAs.
                    if ((attributes & 1) == 0 && name >= imageBase) name = (uint)(name - imageBase);
                    if (Name(name) is { } dll) names.Add(dll);
                }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or EndOfStreamException or ArgumentException)
        {
            Debug.WriteLine($"[Graphics API] {path}: {ex.Message}");
        }
        return names;
    }
}
