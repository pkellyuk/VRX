VRX v1.0.0 is the first packaged Windows x64 release: play flat games on a stereo
screen in VR, with setup on the desktop and normal keyboard/mouse game controls.

Download **VRX-Setup-1.0.0.exe** for the installer, or extract the complete
**VRX-1.0.0-win-x64.zip** for a portable copy. The desktop runtime, renderer and
Depth Anything V2 Small model are included. Start SteamVR and your game, open VRX,
choose the game window and click **Attach / Play**.

Includes per-game saved settings, live screen placement and size controls,
fixed/head-follow modes, configurable recenter/menu keys, capture recovery,
experimental foreground refinement and a frame/depth matching comparison toggle.

Tested game/hardware: Helldivers 2, RTX 3090 and PSVR2 with SteamVR/OpenXR.
Requires Windows 10 2004+ or Windows 11, a DirectX 12 GPU and an installed active
OpenXR runtime. Other games and hardware have not been broadly validated.

Known limitations:

- Frame/depth matching gives cleaner moving outlines in the tested game, but
  adds latency and can make game motion less smooth. It defaults off.
- Extra foreground passes are experimental; the initial game test showed little
  visible improvement. They default on and can be disabled per game.
- GPU contention can substantially reduce depth update speed; 60 fps depth is
  not guaranteed. ZipDepth and second-GPU inference are not included.
- SteamVR menu dismissal is best-effort. No in-headset settings UI or motion
  controller controls are required.
- The installer is unsigned, so Windows may show an unknown-publisher warning.

SHA-256 checksums are provided in **SHA256SUMS.txt**. Third-party licenses,
component provenance and setup instructions are bundled. Profiles and session
logs stay under `%LOCALAPPDATA%\VRX` and are preserved when uninstalling.
