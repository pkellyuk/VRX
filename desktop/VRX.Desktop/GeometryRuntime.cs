using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;

namespace VRX.Desktop;

// The optional true-geometry components a release ships next to VRX.Desktop.exe:
//   geometry\layer\vrx_xgstereo.dll, VkLayer_vrx_xgstereo.json  (VRX's Vulkan layer)
//   geometry\dxvk\d3d11.dll, dxgi.dll                            (DXVK 2.7.1, D3D11 -> Vulkan)
//   geometry\vkd3d\d3d12.dll, d3d12core.dll                       (vkd3d-proton, D3D12 -> Vulkan)
// A release looks only there and never downloads anything. Every third-party DLL is
// checked against the pinned SHA-256 below before it is used or copied into a game folder;
// the layer is checked against the release's FILES.sha256.txt. A missing file, a mismatch,
// or a component whose hash is not pinned refuses geometry with a plain "reinstall" message.
// Development builds (DEBUG) may also use the bench and sibling-checkout builds.
internal static class GeometryRuntime
{
    public const string DxvkVersion = "2.7.1";

    // Unknown until the packaging pins a vkd3d-proton release (release\vendor). While null a
    // release refuses the DirectX 12 route instead of loading an unverified DLL.
    // TODO(packaging): set these to the SHA-256 of the pinned vkd3d-proton d3d12.dll and
    // d3d12core.dll that release\build-release.ps1 copies to geometry\vkd3d.
    public const string? Vkd3dD3d12Sha256 = null, Vkd3dD3d12CoreSha256 = null;

    // The one table of pinned hashes, by file name within its component folder. DXVK's are
    // the x64 DLLs of the official dxvk-2.7.1.tar.gz (archive SHA-256 D85CE7C7...13CE87).
    private static readonly Dictionary<string, string?> Pinned = new(StringComparer.OrdinalIgnoreCase)
    {
        [@"dxvk\d3d11.dll"] = "523DA2CD765DD51D99FE1644B24F9EE7EE6247E4C98953BCC449CE84CD303405",
        [@"dxvk\dxgi.dll"] = "EC02EB37620FF52361CB45376A4611FC4210D96E71D0363F1CC9807F151C01BE",
        [@"vkd3d\d3d12.dll"] = Vkd3dD3d12Sha256,
        [@"vkd3d\d3d12core.dll"] = Vkd3dD3d12CoreSha256,
    };

    public static readonly string[] DxvkFiles = ["d3d11.dll", "dxgi.dll"];
    public static readonly string[] Vkd3dFiles = ["d3d12.dll", "d3d12core.dll"];
    public const string LayerLibrary = "vrx_xgstereo.dll", LayerManifest = "VkLayer_vrx_xgstereo.json";

    public static string AppRoot => AppContext.BaseDirectory;
    public static string Root => Path.Combine(AppRoot, "geometry");

    public static string Hash(string path)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        using var file = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(file));
    }

    // The pinned hash for a component file ("dxvk\d3d11.dll"); null when it is not pinned.
    public static string? PinnedHash(string component, string name)
    {
        ArgumentException.ThrowIfNullOrEmpty(component);
        ArgumentException.ThrowIfNullOrEmpty(name);
        return Pinned.GetValueOrDefault(component + "\\" + name);
    }

    // True when `file` is the pinned build of that component file.
    public static bool MatchesPin(string component, string name, string file)
    {
        string? pinned = PinnedHash(component, name);
        if (pinned == null || !File.Exists(file)) return false;
        return Hash(file).Equals(pinned, StringComparison.OrdinalIgnoreCase);
    }

    // The folder holding Vulkan layer files, or a FileNotFoundException with the plain message.
    public static string LayerDirectory()
    {
        Debug.WriteLine("[Geometry runtime] LayerDirectory enter");
        string bundled = Path.Combine(Root, "layer");
        if (LayerPresent(bundled))
        {
            VerifyListed(bundled, LayerLibrary);
            VerifyListed(bundled, LayerManifest);
            Debug.WriteLine($"[Geometry runtime] LayerDirectory exit: bundled {bundled}");
            return bundled;
        }
#if DEBUG
        foreach (string folder in DevelopmentFolders("layer"))
        {
            if (!LayerPresent(folder)) continue;
            Debug.WriteLine($"[Geometry runtime] LayerDirectory exit: development {folder}");
            return folder;
        }
#endif
        Debug.WriteLine("[Geometry runtime] LayerDirectory: missing");
        throw new FileNotFoundException(Loc.Format("ErrorGeometryLayerMissing", Path.Combine("geometry", "layer")));
    }

    // DXVK's folder, every DLL verified against the pin.
    public static string DxvkDirectory()
    {
        Debug.WriteLine("[Geometry runtime] DxvkDirectory enter");
        string bundled = Path.Combine(Root, "dxvk");
        if (DxvkFiles.All(n => File.Exists(Path.Combine(bundled, n))))
        {
            VerifyPinned("dxvk", bundled, DxvkFiles);
            Debug.WriteLine($"[Geometry runtime] DxvkDirectory exit: bundled {bundled}");
            return bundled;
        }
#if DEBUG
        foreach (string folder in DevelopmentFolders("dxvk"))
        {
            if (!DxvkFiles.All(n => File.Exists(Path.Combine(folder, n)))) continue;
            VerifyPinned("dxvk", folder, DxvkFiles);
            Debug.WriteLine($"[Geometry runtime] DxvkDirectory exit: development {folder}");
            return folder;
        }
#endif
        Debug.WriteLine("[Geometry runtime] DxvkDirectory: missing");
        throw new FileNotFoundException(Loc.Format("ErrorGeometryLayerMissing", Path.Combine("geometry", "dxvk")));
    }

    // vkd3d-proton's folder, every DLL verified against the pin. A release refuses while the
    // pin is unknown; a development build accepts a local build without one (and logs it).
    public static string Vkd3dDirectory()
    {
        Debug.WriteLine("[Geometry runtime] Vkd3dDirectory enter");
        string bundled = Path.Combine(Root, "vkd3d");
        if (Vkd3dFiles.All(n => File.Exists(Vkd3dFile(bundled, n))))
        {
            VerifyPinned("vkd3d", bundled, Vkd3dFiles);
            Debug.WriteLine($"[Geometry runtime] Vkd3dDirectory exit: bundled {bundled}");
            return bundled;
        }
#if DEBUG
        foreach (string folder in DevelopmentFolders("vkd3d"))
        {
            if (!Vkd3dFiles.All(n => File.Exists(Vkd3dFile(folder, n)) && GeometryLaunch.IsX64(Vkd3dFile(folder, n)))) continue;
            Debug.WriteLine($"[Geometry runtime] Vkd3dDirectory exit: development {folder} (local build, not pinned)");
            return folder;
        }
#endif
        Debug.WriteLine("[Geometry runtime] Vkd3dDirectory: missing");
        throw new FileNotFoundException(Loc.Format("ErrorVkd3dMissing", Path.Combine("geometry", "vkd3d")));
    }

    // A vkd3d-proton build tree keeps each DLL in a folder of its own name.
    public static string Vkd3dFile(string folder, string name)
    {
        ArgumentException.ThrowIfNullOrEmpty(folder);
        ArgumentException.ThrowIfNullOrEmpty(name);
        string flat = Path.Combine(folder, name);
        return File.Exists(flat) ? flat : Path.Combine(folder, Path.GetFileNameWithoutExtension(name), name);
    }

    // The source file for a component DLL, checked against its pin when the file is in the
    // bundled folder (always, in a release). Used again right before a copy into a game folder.
    public static void VerifySource(string component, string name, string file)
    {
        ArgumentException.ThrowIfNullOrEmpty(component);
        ArgumentException.ThrowIfNullOrEmpty(name);
        ArgumentException.ThrowIfNullOrEmpty(file);
#if DEBUG
        // A development build may use a local, unpinned vkd3d-proton build (see Vkd3dDirectory).
        if (PinnedHash(component, name) == null && !IsInside(file, Root))
        {
            Debug.WriteLine($"[Geometry runtime] VerifySource: {file} is a development build without a pin");
            return;
        }
#endif
        if (MatchesPin(component, name, file)) return;
        Debug.WriteLine($"[Geometry runtime] VerifySource: {file} does not match the pin for {component}\\{name}");
        bool translator = component == "vkd3d";
        throw new InvalidDataException(Loc.Format(translator ? "ErrorVkd3dMissing" : "ErrorGeometryLayerMissing", Path.Combine("geometry", component)));
    }

    private static void VerifyPinned(string component, string folder, IEnumerable<string> names)
    {
        foreach (string name in names)
        {
            string file = component == "vkd3d" ? Vkd3dFile(folder, name) : Path.Combine(folder, name);
            VerifySource(component, name, file);
        }
        Debug.WriteLine($"[Geometry runtime] {component} in {folder} matches its pins");
    }

    private static bool LayerPresent(string folder) =>
        File.Exists(Path.Combine(folder, LayerLibrary)) && File.Exists(Path.Combine(folder, LayerManifest));

    // The layer is VRX's own build: a release checks it against the payload's FILES.sha256.txt
    // (written by release\build-release.ps1). Without that listing it is accepted as installed.
    private static void VerifyListed(string folder, string name)
    {
        string listing = Path.Combine(AppRoot, "FILES.sha256.txt");
        if (!File.Exists(listing))
        {
            Debug.WriteLine($"[Geometry runtime] VerifyListed: no {listing}; {name} accepted as installed");
            return;
        }
        string relative = Path.GetRelativePath(AppRoot, Path.Combine(folder, name));
        string? expected = null;
        foreach (string line in File.ReadLines(listing))
        {
            int gap = line.IndexOf("  ", StringComparison.Ordinal);
            if (gap <= 0) continue;
            if (!line[(gap + 2)..].Trim().Equals(relative, StringComparison.OrdinalIgnoreCase)) continue;
            expected = line[..gap].Trim();
            break;
        }
        if (expected != null && Hash(Path.Combine(folder, name)).Equals(expected, StringComparison.OrdinalIgnoreCase)) return;
        Debug.WriteLine($"[Geometry runtime] VerifyListed: {relative} {(expected == null ? "is not listed" : "does not match its listing")}");
        throw new InvalidDataException(Loc.Format("ErrorGeometryLayerMissing", Path.Combine("geometry", "layer")));
    }

    public static bool IsInside(string path, string directory)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        ArgumentException.ThrowIfNullOrEmpty(directory);
        return Path.GetFullPath(path).StartsWith(Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory)) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
    }

#if DEBUG
    // Development builds: the repository's own geometry folder and bench builds, the DXVK
    // download cache of earlier development builds, and a sibling vkd3d-proton build.
    private static IEnumerable<string> DevelopmentFolders(string component)
    {
        string repository;
        try { repository = EngineSession.RepositoryRoot(); }
        catch (DirectoryNotFoundException) { yield break; }
        switch (component)
        {
            case "layer":
                yield return Path.Combine(repository, "geometry", "layer");
                yield return Path.Combine(repository, "bench", "native", "xgstereo", "out", "Release");
                break;
            case "dxvk":
                yield return Path.Combine(repository, "geometry", "dxvk");
                yield return Path.Combine(repository, "geometry", "dxvk", "x64");
                yield return Path.Combine(repository, "bench", "native", "xgstereo", "out", "dxvk-" + DxvkVersion, "x64");
                yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VRX", "geometry", "dxvk-" + DxvkVersion, "x64");
                break;
            case "vkd3d":
                if (Environment.GetEnvironmentVariable("VRX_VKD3D_DIRECTORY") is { Length: > 0 } variable) yield return variable;
                yield return Path.Combine(repository, "geometry", "vkd3d");
                yield return Path.GetFullPath(Path.Combine(repository, "..", "vkd3d-proton", "build-win-local", "libs"));
                break;
        }
    }
#endif
}
