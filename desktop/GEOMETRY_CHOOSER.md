# Stereo methods in the window chooser

Each game window offers Auto, Depth estimate and Geometry stereo. Clicking the
picture keeps that executable's saved selection. Whole displays use depth capture.

Auto uses a built-in tested profile when available; unknown games use depth.
Background auto-attach only connects an existing stereo stream or uses depth: it
never closes a game or installs a graphics DLL. Manual Play can show the concrete
restart review for an available tested profile. Missing local components fall back
to depth in Auto. Explicit Geometry can prepare DXVK and then show that review.

Advanced geometry settings offers Automatic renderer, Native Vulkan, DirectX 11
through DXVK, and DirectX 12 through vkd3d-proton. The DXVK installer currently
supports D3D11, not D3D9/10. Unknown games' automatic geometry renderer tries native
Vulkan. Native D3D12 command replay remains developer-only.

The main window keeps these levels separate: Stereo method has Auto, Depth
estimate and Geometry stereo. Geometry stereo reveals a secondary Geometry
renderer dropdown. The per-game renderer is remembered when switching methods,
and is shared with the window chooser's advanced geometry selection.

Recent games appears above running windows. A successful attachment records its
title, executable, launch arguments and Steam identity in `recent-games.json`.
History keeps 24 entries, deduplicated by executable, and imports an existing
user's last game on first use. Panels retain their method/renderer settings after
the game closes; missing executables remain visible with launch controls disabled.

Selecting a stopped history game launches it directly. Geometry's launch review
prepares the selected backend before the first process starts, so there is no
baseline launch followed by a restart. Depth uses Steam's launch URI for Steam
games or the recorded executable/arguments otherwise. Already-running games reuse
their window. The chooser is also available while VR is playing: cancelling it
keeps the current session, and choosing a replacement stops that viewer before
attaching or reviewing a geometry restart.

The System Shock remake Steam profile (482400) uses vkd3d-proton with separation
0.5, convergence 0, and `-dx12 -norhithread -windowed`. Resolution comes from the
game's saved settings; the earlier 1280×720 test override is migrated away.
The Steam manifest and shipping executable identify it; a matching window title
alone does not. Calibration is seeded once and later user edits are preserved.
Base settings and Apply to all never copy another game's renderer or calibration.

The translated route needs the local Vulkan layer, matching DXVK D3D11/DXGI DLLs,
and x64 vkd3d-proton `d3d12.dll` and `d3d12core.dll`. The runtime can be placed in
`geometry/vkd3d`, selected with `VRX_VKD3D_DIRECTORY`, or discovered from the sibling
`vkd3d-proton/build-win-local/libs` build. The managed launcher reproduces the
tested single-queue and shader-cache settings. Installed files are owned by hash,
never overwrite existing game DLLs, and are removed after the owning process exits.

Geometry settings currently take effect on restart. The geometry session field is
an advanced connection name managed by VRX, not a game title or launch command.
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
