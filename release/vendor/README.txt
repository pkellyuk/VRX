Pinned binaries a VRX release is built with, so that a release can be built on a machine
without SteamVR or a particular Windows SDK (the GitHub Actions build) and every build uses
exactly the files that were tested. bench\native\openxr\build.bat copies them next to the
engine, and release\build-release.ps1 checks their hashes and signatures. VRX 1.7.6 shipped
these same files.

openxr_loader.dll - the Khronos OpenXR loader (Apache-2.0), version 1.1.58, exactly as
SteamVR ships it (Steam\steamapps\common\SteamVR\bin\win64), Authenticode-signed by Valve Corp.
SHA-256 9dae7f85dcff14352df31d699153cea72100d39ba3f3ba86c236291ef9265baf
Source: https://github.com/KhronosGroup/OpenXR-SDK-Source

d3dcompiler_47.dll - Microsoft's HLSL shader compiler, version 10.0.26100.7705, the
redistributable from the Windows SDK (Windows Kits\10\Redist\D3D\x64), Authenticode-signed
by Microsoft. Distributable code under the Windows SDK license terms. The engine loads this
copy instead of Windows' own, and the pre-compiled shader cache shipped in engine\shader-cache
is keyed by its version, so a different compiler would change every shipped shader.
SHA-256 a05f99734f7c4822fefc12b367af21fd0976ed6608752fb1e1e80b6ece7ecbbb

Geometry stereo runtime - NOT committed here, fetched at build time. release\build-release.ps1
downloads the official release archives once into release\out\vendor-cache (git-ignored),
refuses any archive whose SHA-256 differs from its pin, extracts only the x64 DLLs, checks
each against geometry-runtime.sha256 (this folder) and ships them under geometry\. The same
file goes into the payload as geometry\geometry-runtime.sha256, for the desktop app to
verify the DLLs at run time. They ship unmodified and unsigned, as their projects release them.

DXVK 2.7.1 (zlib) - geometry\dxvk\d3d11.dll, dxgi.dll
https://github.com/doitsujin/dxvk/releases/download/v2.7.1/dxvk-2.7.1.tar.gz
archive SHA-256 d85ce7c79f57ecd765aaa1b9e7007cb875e6fde9f6d331df799bce73d513ce87
(the same pin as the desktop app's DXVK download in desktop\VRX.Desktop\GeometryLaunch.cs,
and the digest GitHub publishes for the release asset)

vkd3d-proton 3.0.1 (LGPL-2.1-or-later), the latest stable release (2026-05-06) -
geometry\vkd3d\d3d12.dll, d3d12core.dll
https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v3.0.1/vkd3d-proton-3.0.1.tar.zst
archive SHA-256 3cf2315522af5e43605ef6d3c41dad91387040bf97199934f3f7ab76caaa2f0c
Source: https://github.com/HansKristian-Work/vkd3d-proton/tree/v3.0.1 (with its submodules)

To move to a new version: change the URL and archive pin in build-release.ps1, the DLL
pins in geometry-runtime.sha256, the licence texts in release\licenses if they changed,
and THIRD-PARTY-NOTICES.txt.

VRX's own Vulkan layer (geometry\layer) is built from bench\native\xgstereo by the same
script with CMake (Release, layer target only); its Vulkan and SPIR-V headers are fetched
at the commits pinned in that CMakeLists.txt. The native DX12 proxy (bench\native\dx12stereo)
is not shipped.