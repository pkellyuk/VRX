# VRX3D store page text

Draft for Steamworks > Edit Store Page (App 5458290). Paste each block into the field
named in its heading. Steam's description editor uses BBCode ([h2], [b], [list], [*]).
Store review rules that shaped this text: no links to other websites in the
description, only features that exist in the build, no review quotes or awards.

## Short description (max 300 characters)

Play your flat PC games on a giant 3D screen in VR. VRX3D captures the game window,
works out the depth of every frame live on your graphics card, and shows it in stereo on
a big screen in a dark room lit by the picture. Keep your keyboard, mouse or controller.

## About This Software (long description)

[h2]Your flat games, in 3D[/h2]
VRX3D takes any game in a window on your PC and shows it on a huge screen in VR, in
stereo 3D. A depth model runs on your own graphics card and estimates how far away
everything in each frame is, then draws a slightly different picture for each eye.
Cockpits, corridors, landscapes and characters stand out with real depth.

You keep playing exactly as before, with your keyboard and mouse or a gamepad. There is
no in-headset menu to learn and no motion controllers are needed: every setting lives in
VRX3D's window on your desktop.

[h2]Easy to start[/h2]
[list]
[*][b]Easy mode:[/b] start your game and bring it to the front. VRX3D counts down and
attaches to it by itself.
[*][b]Choose…[/b] shows live tiles of your open windows and displays: click one to play it.
[*][b]Whole screen:[/b] play an entire display, such as the desktop or a video player.
[*][b]Settings per game:[/b] screen size, distance, curve and depth strength are saved for
each game.
[/list]

[h2]A screen worth sitting in front of[/h2]
[list]
[*][b]Room:[/b] a dark room whose walls, floor and ceiling are lit by the picture, like a
cinema. Optional glass walls, reflections and a soft ceiling light.
[*][b]Ambilight:[/b] the colours at the edge of the picture glow softly around the screen.
[*][b]Screen curve:[/b] wrap the screen around you like a curved television, drawn as a
real curved surface for each eye.
[*][b]Desktop when you leave the game:[/b] press the Windows key or Alt+Tab and VR shows
your desktop; go back and the game returns.
[/list]

[h2]Depth that keeps up[/h2]
[list]
[*]A fast depth model (ZipDepth) keeps depth close to the game's frame rate; Depth
Anything V2 is available too.
[*]Steady depth uses your graphics card's video engine to cut shimmer between frames.
[*]Have a second graphics card? Make it the depth GPU so depth never competes with the
game.
[/list]

[h2]Not only games[/h2]
Attach VRX3D to a browser or video player and 2D video becomes 3D on the same screen.
Copy-protected streaming services (such as Netflix and Disney+) show as a black picture:
Windows blanks protected video in every screen capture, and VRX3D respects that.

[h2]Good to know[/h2]
[list]
[*]VRX3D estimates depth from a flat picture. It is convincing, especially with clear
near and far objects, but it is not the same as a game built for VR. Fast motion and the
edges of foreground objects can sometimes distort.
[*]It needs a SteamVR headset and a DirectX 12 graphics card. Depth runs alongside your
game, so a game that uses the whole graphics card leaves less for depth: running the game
at 1080p usually helps, and you will not see the difference in the headset.
[*]VRX3D is not code-signed yet. If Windows 11 Smart App Control is turned on, it blocks
VRX3D.
[/list]

[h2]Open source[/h2]
VRX3D is free and open source under the GPL-3.0 licence; its source code is on GitHub as
pkellyuk/VRX. Buying it on Steam gets you automatic updates and supports its development.

## Store page fields

- **Website:** https://vrx3d.uk
- **Support:** https://github.com/pkellyuk/VRX/issues (support URL) ; no support email
  needed if the URL is given.
- **Developer / Publisher:** Paul Kelly (or a studio name, if you prefer one)
- **Genre (software):** Utilities
- **Tags (suggested, in order):** VR, Utilities, 3D, Software, Immersive
- **VR support:** VR Only. SteamVR. Headsets: any SteamVR headset; tick the ones it has
  actually run on: PlayStation VR2, Meta Quest 2 (Link), Pico 4 (Steam Link), Oculus Rift
  (a user, with Screen curve at 1 %). Play area: Seated, Standing.
  Input: Keyboard / Mouse, Gamepad (the user's own game controls; VRX3D needs no tracked
  controllers).
- **Languages:** English (interface).
- **Supported features:** none of the Steamworks features (no achievements, cloud,
  overlay); optionally Steam Cloud via Auto-Cloud if set up (see ../README.md).

## System requirements

The minimum graphics card is a guess until someone tests a lower card: VRX3D has been
measured on an RTX 3090 and an RTX 3060 (as the depth GPU), and run on a ROG Ally X.

| | Minimum | Recommended |
|---|---|---|
| OS | Windows 10 version 2004 (64-bit) | Windows 11 24H2 (64-bit) |
| Processor | 4-core 64-bit CPU | 6-core or better |
| Memory | 8 GB RAM | 16 GB RAM |
| Graphics | DirectX 12 GPU, NVIDIA RTX 2060 / AMD RX 6600 class | NVIDIA RTX 3070 / AMD RX 6800 or better, or a second GPU for depth |
| DirectX | Version 12 | Version 12 |
| Storage | 1 GB available space | 1 GB available space |
| VR Support | SteamVR | SteamVR |
| Additional notes | A SteamVR-compatible headset with SteamVR as the OpenXR runtime. | Windows 11 24H2 lets VRX3D keep the depth of a still picture (no twitch on a still desktop). |

## AI disclosure (Content Survey)

Two things to declare, honestly:

- **Live-generated AI content:** VRX3D runs depth-estimation models (ZipDepth and Depth
  Anything V2) on the player's GPU to estimate depth from each frame. They do not generate
  text, images or audio for the player; they estimate distance. Suggested wording:
  "VRX3D uses machine-learning depth-estimation models, running locally on the player's
  graphics card, to estimate the depth of each captured frame and draw it in stereo 3D.
  They do not generate new images, text or sound, and nothing is sent online."
- **Pre-generated (development):** much of VRX3D's code and documentation was written
  with AI coding assistants. Declare it if the survey asks about AI used in development
  ("Parts of the code were written with the help of AI coding tools and reviewed and
  tested by the developer.").
