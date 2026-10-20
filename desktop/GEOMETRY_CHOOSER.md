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

The System Shock remake Steam profile (482400) uses vkd3d-proton with separation
0.5, convergence 0, and `-dx12 -norhithread -windowed -ResX=1280 -ResY=720`.
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
