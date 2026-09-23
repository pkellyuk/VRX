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
