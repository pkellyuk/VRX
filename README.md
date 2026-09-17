# VRX

Play flat PC games on a stereo screen in VR using your keyboard and mouse.
VRX captures the selected game window, estimates depth, and creates a stereo
view. All settings live in a desktop app; no in-headset UI or motion controllers
are needed. This provides a virtual 3D screen, not native VR camera controls.

## Get started

Download the installer or portable ZIP from [GitHub Releases](https://github.com/pkellyuk/VRX/releases).
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
- The packaged release includes the desktop runtime, native dependencies, and depth model.
- Depth is estimated using Depth Anything V2 Small. Fast motion and foreground edges can distort.
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
The model must exist at `bench/models/model_fixed_686x392.onnx`; model files
are not tracked in Git.

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
