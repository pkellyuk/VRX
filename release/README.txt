VRX 1.2.0 - Windows x64

Install with VRX-Setup-1.2.0.exe, or extract the complete portable ZIP and run
VRX.Desktop.exe. Keep the engine and model folders with the desktop application.
The .NET desktop runtime, native dependencies and depth model are included.
No Python, development checkout or separate .NET installation is required.

Requirements: Windows 10 2004 or later / Windows 11, a DirectX 12 GPU, a supported
VR headset and an installed active OpenXR runtime. Start SteamVR and your game
before attaching. SteamVR is the runtime tested for this release; set SteamVR as
the active OpenXR runtime in its settings. Hardware tested: RTX 3090 (+ RTX 3060
for depth) and PSVR2.

Choose the game and window, then Attach / Play. All setup stays on the desktop.
Settings save per executable under %LOCALAPPDATA%\VRX\profiles. Logs are under
%LOCALAPPDATA%\VRX\sessions. Stop VR or close the desktop app to end playback.
Uninstalling preserves those settings and logs.

The screen stays in place by default. Equals (=) recenters it; F8 requests closing
the SteamVR menu. Keys are configurable and also reach the game. Screen size,
position, stereo strength and head-follow mode can be changed during playback.

Extra foreground depth passes are experimental and default on. Their benefit
varies and no clear improvement was seen in the initial Helldivers 2 test.
Match game frames to depth defaults off. It gives cleaner moving outlines in
the tested game, but adds delay and limits game-image motion to the depth rate.
Neither option guarantees 60 fps. Disable extra foreground passes for comparisons.

Depth is estimated by ZipDepth by default: about 6x less GPU time than Depth
Anything V2 Small, so depth keeps closer to the game. Untick "Fast depth model
- ZipDepth" (per game, applies live) to use Depth Anything V2 instead.
"Depth GPU" (per game, applies at Attach / Play) can run the depth model on
another graphics card so it no longer competes with the game. Cards are chosen by
name; if the chosen card is missing, the game's GPU is used. Foreground crop
passes still run on the game's GPU.
No game injection, motion-controller controls, or in-headset UI.
SteamVR dashboard dismissal is best-effort. Other games/headsets need validation.

The installer is not code-signed and Windows may display an unknown-publisher
warning. Download only from the project's GitHub release and compare SHA256SUMS.txt
if verifying the download. Do not disable Windows security features.

Third-party notices and license texts are in the licenses folder.
Project: https://github.com/pkellyuk/VRX
