# VRX

Play flat PC games on a stereo screen in VR using your keyboard and mouse.
VRX captures the selected game window, estimates depth, and creates a stereo
view. All settings live in a desktop app; no in-headset UI or motion controllers
are needed. This provides a virtual 3D screen, not native VR camera controls.

## Get started

Download VRX v1.2.0: the [installer](https://github.com/pkellyuk/VRX/releases/download/v1.2.0/VRX-Setup-1.2.0.exe)
or the [portable ZIP](https://github.com/pkellyuk/VRX/releases/download/v1.2.0/VRX-1.2.0-win-x64.zip)
([release notes and checksums](https://github.com/pkellyuk/VRX/releases/tag/v1.2.0); older versions on
[GitHub Releases](https://github.com/pkellyuk/VRX/releases)).
For the portable version, extract the entire ZIP and run `VRX.Desktop.exe`.
Keep the bundled folders alongside the application.

1. Start SteamVR and set it as the active OpenXR runtime.
2. Start your game.
3. Open VRX, select the game process and window, and click **Attach / Play**.
4. Adjust screen placement, size, and stereo strength on the desktop.
5. Use **Stop VR** to finish.

The screen stays fixed in the room by default. **Equals (=)** recenters it;
**F8** requests dismissal of the SteamVR dashboard. Both keys are configurable
and also reach the game. An optional head-follow mode is available.

Settings save per game executable under `%LOCALAPPDATA%\VRX\profiles`.
Session logs are under `%LOCALAPPDATA%\VRX\sessions`.

## Requirements and limitations

- Windows 10 2004 or later / Windows 11, a DirectX 12 GPU, and an active OpenXR runtime.
- Tested with Helldivers 2, an RTX 3090, PSVR2, and SteamVR. Other combinations need validation.
- The packaged release includes the desktop runtime, native dependencies, and both depth models.
- Depth is estimated by [ZipDepth](https://github.com/fabiotosi92/ZipDepth) by default: about 6x less GPU time per estimate than Depth Anything V2 Small (2.0 vs 13.1 ms on an RTX 3090), which keeps depth much closer to the game's frame rate. Untick **Fast depth model — ZipDepth** (per game, applies live) to use Depth Anything V2 instead. Fast motion and foreground edges can still distort.
- **Depth GPU** (per game, applies at Attach / Play) chooses which graphics card runs the depth model: the game's own GPU (default), any other GPU automatically, or a specific card from the list. Cards are remembered by name, and identical cards by their order, because Windows' GPU numbering can change after driver updates; if the chosen card is missing, VRX uses the game's GPU and the setting shows it as not found. With another card, the capture step makes a small model-size copy of each frame and the game GPU's copy engine sends it across, so depth no longer competes with the game. Measured with the RTX 3090 saturated by other work: ZipDepth 7.7 ms per estimate on an RTX 3060 (vs 19.9 ms on the busy 3090), Depth Anything V2 25.6 ms (vs 120.5 ms). Foreground crop passes still use the game GPU.
- **Match game frames to depth** improves alignment but adds latency and limits motion to the depth update rate. It defaults off.
- **Extra foreground depth passes** are experimental and default on; their benefit varies.
- GPU contention can reduce depth update speed. A steady 60 depth updates per second is not guaranteed.
- SteamVR dashboard dismissal is best-effort. The installer is unsigned.

## Build locally

The current build scripts expect Visual Studio Community at
`C:\Program Files\Microsoft Visual Studio\18\Community`, with C++ tools and a
Windows SDK, plus the .NET 10 SDK and SteamVR. Adjust `VCVARS` in
`bench/native/openxr/build.bat` if Visual Studio is installed elsewhere.

Native dependencies must be available in the user NuGet cache:
`Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 and `Microsoft.AI.DirectML` 1.15.4.
Both models must exist in `bench/models`; model files are not tracked in Git.
The default ZipDepth model (`zipdepth_faithful_fp16_672x384.onnx`) is generated
by `bench/zipdepth_export.py`, whose header lists the one-time setup. The
Depth Anything V2 model is `model_fixed_686x392.onnx`.

From the repository root in PowerShell, run each build step and confirm it succeeds:

```powershell
cmd /c "bench\native\openxr\build.bat --desktop"
$env:LIB = ''
dotnet build desktop/VRX.Desktop/VRX.Desktop.csproj -c Release
```

Launch `desktop/VRX.Desktop/bin/Release/net10.0-windows/VRX.Desktop.exe`.
The native renderer is `bench/native/openxr/out/xrplayer.exe`.
The local renderer enables a best-effort GPU-priority boost; its
`--normal-gpu-priority` command-line option disables the experiment for comparisons.

To package an installer and portable ZIP, install Inno Setup 6 and run
`./release/build-release.ps1`. Outputs go into a fresh folder under `release/out`.
Third-party notices and license texts are retained under `release` and bundled
with the release.
