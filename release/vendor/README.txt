openxr_loader.dll - the Khronos OpenXR loader (Apache-2.0), version 1.1.58, exactly as
SteamVR ships it (Steam\steamapps\common\SteamVR\bin\win64), Authenticode-signed by Valve Corp.
SHA-256 9dae7f85dcff14352df31d699153cea72100d39ba3f3ba86c236291ef9265baf

It is kept here so that a release can be built on a machine without SteamVR (the GitHub
Actions build). bench\native\openxr\build.bat copies it next to the engine, and
release\build-release.ps1 checks its hash and signature. VRX 1.7.6 shipped this same
file. Source: https://github.com/KhronosGroup/OpenXR-SDK-Source
