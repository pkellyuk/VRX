VRX for Linux - x86_64

VRX plays a flat game in 3D in a VR headset: it captures a window or your screen,
estimates depth with the ZipDepth model on your NVIDIA GPU and shows the picture
in stereo on a screen in VR, optionally in a room lit by the game.

Everything VRX runs is in this folder: the desktop app (with its own .NET
runtime), the engine, its shaders, ONNX Runtime and the depth model. The -cuda
package also includes NVIDIA's CUDA 12 and cuDNN 9 libraries; the smaller package
uses the ones installed on your PC. Nothing is installed outside this folder, and
it can live anywhere.

Start:        ./vrx
Check a PC:   ./vrx --check           (says what is missing and how to install it)
App menu:     ./vrx --install-desktop (adds VRX with its icon; run again after moving the folder)

Requirements
  - An NVIDIA GPU with the proprietary driver (Vulkan and CUDA). Depth needs CUDA;
    without it untick "Depth from ZipDepth on the GPU (CUDA)" for a flat screen.
  - The small package: CUDA 12 runtime libraries and cuDNN 9 on the PC.
  - SteamVR, set as the active OpenXR runtime, with your headset connected.
  - A Wayland or X11 desktop with PipeWire and xdg-desktop-portal screen casting
    (KDE Plasma and GNOME have them). Tested on KDE Plasma with a PICO 4 over
    Steam Link.

Use
  Start SteamVR and your game, then VRX. Easy mode shows the main choices; Expert
  shows every setting in sections. Choose "A window" or "A whole screen" and press
  Attach / Play: your desktop asks which one to share. Recenter puts the screen in
  front of you again; Stop VR ends playback. Settings apply live and are saved as profiles in
  ~/.config/vrx/linux-profiles.json; the app's own choices are in
  ~/.config/vrx/linux-app.json.

Extra environment for VRX (for example a different LD_LIBRARY_PATH) can be put
in ~/.config/vrx/environment as shell assignments.

VRX is free software under the GNU GPL version 3 (LICENSE.txt). Third-party
components and their licences are listed in THIRD-PARTY-NOTICES.txt and
licenses/. BUILD.txt records how this package was built and FILES.sha256.txt the
checksum of every file.
