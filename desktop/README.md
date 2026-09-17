# VRX desktop application

Open `Open-VRX.cmd` in the repository root. Start SteamVR and the game, choose the
running application and its window, then select **Attach / Play**. Stop any older
command-line VRX session first. Playback continues until **Stop VR**, source
closure, a runtime error, or closing the desktop application.

The desktop panel controls screen width, distance, height, left/right offset,
fixed or head-following placement, stereo enablement, 3D strength, automatic
SteamVR-menu dismissal, and separate recenter/menu shortcut keys. The top-down
screen line can be dragged to adjust distance and horizontal position. Width and
distance are independent; image aspect is preserved. Placement changes use the
last recentered headset coordinate frame. Switching to head-following uses the
same controls and refreshes that frame continuously.

All exposed playback settings and the preferred capture-window title save to a
profile keyed by the executable's full path. PIDs and HWNDs are session identity,
not profile identity. The default list shows accessible processes with candidate
windows; **Show all processes** also lists those without a usable capture window.
When several windows exist, choose one explicitly unless a saved title matches.
Terminal/VRX windows are excluded as capture candidates.

Profiles: `%LOCALAPPDATA%\VRX\profiles`. Last selected executable:
`%LOCALAPPDATA%\VRX\last-game.txt`. Per-session settings snapshots and renderer
logs: `%LOCALAPPDATA%\VRX\sessions`. Corrupt or unsupported profiles are reported
without overwriting them; move the affected JSON aside to restore defaults.

No controls are drawn in VR. Shortcut keys also reach the game: choose bindings
that do not conflict with its controls. Stereo disablement is a visual flat-view
mode; inference still runs. Automatic menu dismissal is SteamVR-specific and
best-effort. The manual button/key may be used repeatedly.

**Extra foreground depth passes (experimental)** is enabled by default, including
when loading older profiles. The checkbox saves per executable and can change
during playback. It tracks a connected nearby region using depth, waits at least
250 ms/three observations, then magnifies a crop from that same captured frame
for another model pass. This is region tracking, not character recognition.
Global depth publishes first. The crop is aligned to that frame's global depth;
poor agreement or results older than 200 ms are discarded. Fusion is feathered,
bounded, and limited to nearby surfaces. Tracking resets on disappearance, large
depth changes, long capture gaps and source-layout changes.

Extra passes use the same GPU/model and are limited to five per second, with a
cost-based cooldown targeting at most 20% additional wall time spent on crops.
They are skipped when predicted source age exceeds 180 ms. This is a heuristic
budget, not a GPU-utilization guarantee. Refinement can still reduce full-scene
depth update rate, and does not fix colour/depth motion mismatch or reconstruct
hidden background. Output depth resolution remains 686x392. Toggle it off to
compare visual quality and responsiveness. CLI: `--no-foreground` disables it.

**Match game frames to depth (comparison)** defaults off and saves per game.
It applies the existing `--paired` behaviour live: while valid stereo depth is
available, render the exact captured colour frame used to estimate that depth.
This removes frame-to-depth timing mismatch at the cost of older colour frames
and game motion advancing at the depth-update rate. Headset pose still updates
normally. Flat viewing and missing/stale depth retain the latest-colour fallback.
For a useful comparison, turn foreground refinement off in both cases, then
toggle frame matching while repeating the same camera/character movement.
Cleaner outlines would support investigating motion alignment; no clear change
would point toward depth-edge quality and disocclusion filling instead.

## Build and verification

Requires Windows, .NET 10 SDK, and the project's existing C++/DirectML/OpenXR build
dependencies. No third-party desktop UI packages are required.

```powershell
.\desktop\build-desktop.cmd
```

The UI uses `bench/native/openxr/out/xrplayer.exe`, built from `xrapp5.cpp` with
`build.bat --desktop`. This keeps an older running `xrapp5.exe` session separate.
The app must remain inside this repository for model/engine discovery.

```powershell
.\desktop\VRX.Desktop\bin\Release\net10.0-windows\VRX.Desktop.exe --smoke-test
.\bench\native\openxr\test-playback.bat
.\bench\native\openxr\out\playback_test.exe desktop/out/control-contract.txt
```

The smoke test opens off-screen without activation, enumerates processes,
checks profile isolation/round-trip and shortcut conflicts, renders preview PNGs,
and writes a native-readable settings fixture. It never starts VR. Outputs and
fixture profiles remain under ignored `desktop/out`, separate from real profiles.
Native tests also check snapshot validation and screen adjustment relative to a
stationary origin. These do not replace testing live sliders and Start/Stop with
a worn headset.

Foreground verification (2026-09-17): CPU tests cover persistence/reacquisition,
crop bounds, cost/age gating, relative-depth alignment, bounded blending and bad
output rejection. WPF smoke checks cover default-on migration, saved opt-outs,
checkbox binding and the v2 native contract. RTX 3090 tests pass for full/cropped
preprocessing against CPU samples, actual full/cropped model inference (fixture
alignment accepted), and existing stereo-warp checks. A synthetic runtime test
confirmed live off/on transitions and orderly desktop Stop. No foreground
quality improvement or frame-rate benefit in Helldivers 2 has yet been verified.

## Desktop-to-renderer contract

The UI atomically replaces a small complete text snapshot after settings changes
(updates coalesced at 180 ms while dragging). The renderer reads it at most every 100 ms on its render
thread. Invalid/incomplete snapshots are rejected without changing active values.
Command counters deliver recenter/dismiss requests without repeating on every
read; Stop is a latched flag. Selected PID, HWND, executable filename, and full
path are revalidated before capture. The UI disables source switching during a
session and returns keyboard focus to the chosen game on launch.

The desktop owns only its child renderer. Stop/closing asks for orderly shutdown;
if it has not exited after eight seconds, that renderer is terminated. The game
and SteamVR are never killed. An already-running separate VRX session is reported
instead of starting a competing renderer.

Snapshot v3 field order:
`VRX 3 width distance height horizontal strength follow stereo autoDismiss recenterVK menuVK recenterCounter menuCounter stop foreground paired`.
The native reader also accepts v1 (neither final field) and v2 (foreground only).
Missing flags in old snapshots remain off. JSON profile schema remains v1:
missing `ForegroundRefinement` defaults to true, missing `MatchFrameToDepth`
defaults to false, and explicitly saved choices are retained.

Current limits: Windows-only/repository build; single-key shortcuts (no modifier
chords); no game injection or background-process rendering support; capture
resize/closure and runtime recovery retain the engine's existing limits. The
simple preview shows configured placement, not a live room map or headset tracker.
