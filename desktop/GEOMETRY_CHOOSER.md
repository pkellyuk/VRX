# Stereo methods in the window chooser

Each game window offers Auto, Depth estimate and Geometry stereo. Clicking the
picture keeps that executable's saved selection. Whole displays use depth capture.

Auto always means estimated depth, for every game, including games with a built-in
tested geometry profile: true geometry is only used when it was chosen for that game.
A tested profile is shown as a suggestion next to the geometry choice ("Tested
true-geometry settings are available for this game") and only fills in the geometry
settings. Background auto-attach only connects an existing stereo stream or uses
depth: it never closes a game or installs a graphics DLL.

Advanced geometry settings offers Automatic renderer, Native Vulkan, DirectX 11
through DXVK, and DirectX 12 through vkd3d-proton. The DXVK installer currently
supports D3D11, not D3D9/10. Unknown games' automatic geometry renderer tries native
Vulkan. Native D3D12 command replay remains developer-only.

The main window keeps these levels separate: Stereo method has Auto, Depth
estimate and Geometry stereo. Geometry stereo reveals a secondary Geometry
renderer dropdown. The per-game renderer is remembered when switching methods,
and is shared with the window chooser's advanced geometry selection.

The chooser separates **Launch a game** from **Attach to something running**.
Launch contains stopped recent games and installed Steam games; running titles
appear only as windows under Attach, alongside whole displays. Cards and stereo
method buttons explicitly label Launch versus Attach. The launch group starts
collapsed when attachable windows exist, and expanded when no windows exist.
Geometry's attach hint explains that a reviewed game restart may be needed.
Running Steam windows retain their Steam identity for that restart path. DWM
thumbnails hide when their containing group is collapsed.

Recent games appears within Launch. A successful attachment records its
title, executable, launch arguments and Steam identity in `recent-games.json`.
History keeps 24 entries, deduplicated by executable, and imports an existing
user's last game on first use. Panels retain their method/renderer settings after
the game closes; missing executables remain visible with launch controls disabled.

Installed Steam games is populated locally from Steam's registered installation,
`steamapps/libraryfolders.vdf` and each library's `appmanifest_*.acf` files.
Only manifests marked installed with an existing folder are listed. Libraries
are deduplicated; a malformed or inaccessible file does not hide other libraries.
No account login, API key or Steam configuration writes are involved. Reopening
the chooser refreshes the list. SteamVR and the shared redistributables entry are
excluded. This discovers installed applications, not ownership or compatibility.

Each installed card has Auto, Depth estimate and Geometry stereo, plus the saved
geometry renderer. Executable profiles and recent games seed the Steam-to-exe
binding, preserving existing calibration (including System Shock). Before an
executable is known, choices use an App-ID-specific provisional profile. A normal
Steam launch discovers the game window inside its installation and binds its real
executable. A first geometry launch asks for that rendering executable if unknown;
VRX does not guess which of multiple game/launcher executables should be patched.

Steam cards, and recent Steam games with a discovered binding, launch with
`steam://rungameid/<appid>`. Geometry uses a one-time, user-visible Steam Launch
Options entry: `"<stable VRX.Desktop.exe path>" --steam-game <appid> -- %command%`.
The setup dialog supplies the exact line and a copy button; existing game arguments
belong after `%command%`. Existing wrappers require manual compatibility review.
VRX deliberately does not edit Steam's live `localconfig.vdf` or restart Steam.
Remove that line in Steam to undo setup, and update it if VRX moves.

For a geometry launch the UI writes an expiring, single-use request under its own
`steam` data folder. Steam starts the wrapper with the original command; the
wrapper retains Steam arguments, applies the saved renderer and stereo settings,
installs scoped graphics libraries, and starts Steam's supplied executable with
a child-only environment. If that executable bootstraps a separate renderer, VRX
waits for the selected rendering process and its matching GPU eye stream. Both
executables must be in the discovered game installation. External launchers and
games which remove the environment from their children need a tested profile.

The wrapper stays alive until the rendering process exits so owned libraries are
cleaned up. Ordinary Steam launches with no pending request pass through unchanged.
Expired/cancelled requests block one delayed launch instead of silently launching
without the requested geometry configuration. Launch results are matched by nonce;
timeouts point to Steam launch options/dialogs rather than claiming geometry works.
Once setup has produced a ready stream, subsequent cold launches reuse the saved
settings without another review. Restarting a running game still requires the
normal launch review and a graceful close; the game is never forcibly killed.

Selecting a stopped history game launches it directly. Geometry's launch review
prepares the selected backend before the first process starts, so there is no
baseline launch followed by a restart. Depth uses Steam's launch URI for Steam
games or the recorded executable/arguments otherwise. Already-running games reuse
their window. The chooser is also available while VR is playing: cancelling it
keeps the current session, and choosing a replacement stops that viewer before
attaching or reviewing a geometry restart.

The System Shock remake Steam profile (482400) uses vkd3d-proton with separation
0.5, convergence 0.002, and `-dx12 -norhithread`. The layer shifts clip space by
`x += shift - convergence * w`, so the screen plane (zero parallax) sits at
`w = shift / convergence` = 250 units, about 2.5 m in Unreal's centimetres; the
earlier convergence 0 put the screen plane at infinity and the unshifted HUD behind
the scene. Profiles seeded with v3's convergence 0 move to 0.002 once. Resolution and display mode come from
the game's saved settings; the earlier 1280×720 and forced-windowed test defaults
are migrated away without changing custom launch arguments.
The System Shock child process also uses `HIGHDPIAWARE` compatibility so a scaled
desktop does not turn a 4K borderless window into a stretched 1440p swapchain.
This environment setting affects that launch only; desktop scaling and registry
compatibility settings are not changed.
The Steam manifest and shipping executable identify it; a matching window title
alone does not. Calibration is seeded once and later user edits are preserved.
Base settings and Apply to all never copy another game's renderer or calibration.

The translated route needs the Vulkan layer, matching DXVK D3D11/DXGI DLLs, and x64
vkd3d-proton `d3d12.dll` and `d3d12core.dll`. A release looks only in its own folder:
`geometry\layer\vrx_xgstereo.dll` and `VkLayer_vrx_xgstereo.json`, `geometry\dxvk\d3d11.dll`
and `dxgi.dll` (DXVK 2.7.1), `geometry\vkd3d\d3d12.dll` and `d3d12core.dll`. Nothing is
downloaded. Before use, and again right before a copy into a game folder, every
third-party DLL is checked against the pinned SHA-256 table in `GeometryRuntime.cs`;
the layer is checked against the release's `FILES.sha256.txt`. A missing or mismatched
file refuses geometry with a "reinstall VRX" message (the vkd3d-proton pins are still a
TODO for the packaging, so a release refuses DirectX 12 geometry until they are set).
Development builds (DEBUG) also find the bench builds, `VRX_VKD3D_DIRECTORY` and the
sibling `vkd3d-proton/build-win-local/libs` build. The native DX12 proxy, the
smoke-test fixture switches, the bench trial record and verbose vkd3d logging exist only
in development builds. The managed launcher reproduces the tested single-queue and
shader-cache settings. Installed files are owned by hash,
never overwrite existing game DLLs, and are removed after the owning process exits.

Cleanup does not depend on VRX staying open. Each launch writes its run record and
starts a small detached guard (`VRX.Desktop.exe --geometry-guard=<run folder>`, no
window) before anything is copied. Each DLL is added to `installed.json` (with the
name of its temporary copy) before the copy starts, so a crash at any moment leaves
only files the manifest names. The guard waits until neither the game nor anything
else started from the game's folder has run for 10 seconds (a Steam DRM stub that
restarts the same executable is followed, both by the guard and by the launch's
first-frame wait), then removes exactly the recorded files that are still unchanged
by hash. VRX's in-process observer and `Recover` at startup remain as further safety
nets; nothing is ever injected into a process. Retired run folders (logs only) are
pruned to the newest five, never one still owning files or watched by a guard.
DXVK and vkd3d-proton shader caches persist per game in `geometry\cache\<session>`,
so a launch does not recompile everything.

Renderer/launch settings take effect on restart. The geometry session field is
an advanced connection name managed by VRX, not a game title or launch command.
The desktop separates estimated-depth controls into **3D & Depth Estimation**
and geometry controls into **3D and Depth Geometry**, with independently remembered
expanded/collapsed states. The geometry panel exposes separation and convergence whenever
geometry is selected. They save with that executable's profile, accept the user's
decimal separator, and retain the existing calibration when switching methods.
Sliders below the numeric fields allow continuous live adjustment. The normal
slider ranges are 0.001–2 separation and 0–0.05 convergence; larger existing or
typed values expand the ranges so profiles are never clamped. Numeric fields
remain available for exact values, and both inputs share the same save/live path.
Invalid or incomplete input is not saved and reverts to the saved values on focus
loss. Separation and convergence update live on the same-frame Vulkan backend,
including DXVK and vkd3d-proton. The UI matches the recorded game's executable,
PID/start time and session, checks the layer's versioned live-capability record,
and atomically writes both values to that run's `stereo.txt` in invariant format.
It reports applying/applied only from the renderer's acknowledgement. Other or
older backends keep launch-time tuning; an already-running older Vulkan layer
needs one restart to load the live-capable build.

The renderer polls tuning at most every 100 ms while presenting. It queues a
transfer command updating both eye buffers after the current frame, with shader
read/transfer write barriers before and after the writes. A fence protects command
buffer reuse and acknowledges GPU completion; neither CPU writes to in-flight
shader data nor device-idle waits are used. Invalid/partial files retain the last
valid pair of values. A paused/non-presenting game may leave the UI at applying
until presentation resumes. Values continue to save per game for later launches.
Graphics layers and translation DLLs can be blocked by anti-cheat; the restart
review explains this before it changes the game.

Validation: desktop `--smoke-test` covers route/profile persistence, calibration,
chooser controls and rendered UI. Add `--geometry-launch-integration-test` and
`--vkd3d-geometry-launch-integration-test` for two managed restarts of isolated
D3D11/D3D12 fixtures, distinct same-frame GPU eyes, VRX consumer checks, stream
reuse and owned-file cleanup. These tests do not restart an installed game or
create an OpenXR session. `VRX_GEOMETRY_TEST_ENGINE` can select a freshly built
consumer when the normal engine binary is in use.

Geometry viewers close when their original game process exits, including a game
restart. Session records permit cooperative recovery if a desktop UI crashed and
left its viewer behind. Recovery checks the game executable, viewer start time,
engine location and exact geometry/control arguments. A viewer owned by another
live desktop UI is left alone. The recorded earlier System Shock command-line
trial can also be released when attaching from the desktop chooser.

The GPU viewer's initial connection tolerates splash-to-game swapchain changes.
For up to 15 seconds it reopens closed/replaced generations until a completed pair
arrives. The first accepted producer PID remains pinned (or the explicit launch
PID is used), and every reopened reader checks the headset adapter. Invalid data
without a valid replacement still fails closed; another producer cannot take over.
The GPU transport test forces replacement after startup registration to cover the
same race as System Shock's first-frame swapchain recreation.

Only one interactive desktop UI runs per Windows session. C# holds the named
mutex `Local\VRX.Desktop.UI` until application exit; a second launch signals an
activation event and exits before constructing a window or loading profiles.
The existing window restores from minimised state and comes forward. Steam's
`--steam-game` wrapper and isolated smoke/fixture processes bypass the UI mutex,
so they can still launch games while the desktop is open. An abandoned mutex is
recovered on the next launch. This avoids two control windows competing for the
same headset and reporting another session during method changes.
