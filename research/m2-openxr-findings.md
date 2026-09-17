# M2 findings — OpenXR bootstrap on this machine

Date: 2026-09-17
Probe: `bench/native/openxr/xrprobe.cpp` (C++, headers from KhronosGroup/OpenXR-SDK,
`openxr_loader.dll` loaded dynamically — no import library needed)

## Result: the full chain works, and there is a headset

```
loader : ...\SteamVR\bin\win64\openxr_loader.dll
extensions: 43
  [x] XR_KHR_D3D11_enable (v11)
  [x] XR_KHR_D3D12_enable (v11)
  [x] XR_KHR_composition_layer_depth (v6)
  [x] XR_KHR_vulkan_enable (v10)
  [x] XR_KHR_win32_convert_performance_counter_time (v1)
xrCreateInstance : XR_SUCCESS
runtime          : SteamVR/OpenXR 2.17.9
xrGetSystem(HMD) : XR_SUCCESS
system           : "SteamVR/OpenXR : rayneo"  vendor=0x28de  maxSwapchain=8192x8192  layers=16
view config      : PRIMARY_STEREO
  blend modes    : OPAQUE
```

So: instance creation, extension negotiation, system enumeration and view
configuration all work, and an HMD is actually present (a RayNeo device presented
through SteamVR). M2's remaining work is the session + swapchain + frame loop, not
a hardware hunt.

## Three constraints this settles

1. **Use D3D12 throughout.** `XR_KHR_D3D12_enable` is available, and our depth
   module is DirectML — which is D3D12-only. So the planned
   D3D11 (OpenXR) ↔ D3D12 (DML) shared-handle bridge is **not needed**:
   one D3D12 device can serve both the swapchain and the depth EP. This removes
   the most awkward piece of the architecture.
2. **`XR_MSFT_composition_layer_reprojection` is not offered by SteamVR.** The
   per-pixel reprojection mode that `research/openxr-depth-apis.md` identified
   cannot be requested here. `XR_KHR_composition_layer_depth` (v6) *is*
   available, so the depth still has a place to go — but M3's verification must
   be "the layer behaves correctly in-headset", not "the MSFT mode is active".
3. **PRIMARY_STEREO + OPAQUE**, 16 layers, 8192² max swapchain — ample headroom
   for a full-screen quad layer plus a depth image.

## Pitfall worth remembering

Instance-level entry points (`xrGetSystem`, `xrGetInstanceProperties`,
`xrEnumerateViewConfigurations`, ...) must be resolved through
`xrGetInstanceProcAddr` **with a valid `XrInstance`**, after `xrCreateInstance`.
Resolving them with `XR_NULL_HANDLE` returns null function pointers; the first
call then crashes. The OpenXR validation layer names this exactly:

```
Error [SPEC | xrGetInstanceProcAddr | VUID-...] : XR_NULL_HANDLE for instance but
query for xrGetSystem requires a valid instance
```

Only `xrEnumerateInstanceExtensionProperties`, `xrCreateInstance` and
`xrGetInstanceProcAddr` itself are queryable before an instance exists.

## Session + swapchain + frame loop: working (M2 core complete)

`bench/native/openxr/xrapp.cpp` — D3D12 throughout, presenting a 1280x720 quad
layer 1.5 m ahead carrying a time-varying pattern.

```
system : SteamVR/OpenXR : playstation_vr2
adapter: NVIDIA GeForce RTX 3090        (adapter chosen via xrGetD3D12GraphicsRequirementsKHR LUID)
xrCreateSession: XR_SUCCESS
swapchain format: 29 of 9 offered       (DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
xrCreateSwapchain: XR_SUCCESS -> 1280x720, 3 images

state 0 -> 1 (IDLE)
state 1 -> 2 (READY)
session READY -> xrBeginSession: XR_SUCCESS
state 2 -> 3 (SYNCHRONIZED)
state 3 -> 4 (VISIBLE)
state 4 -> 5 (FOCUSED)

frames: 1433   frames drawn: 1085   ~119 fps   xrEndFrame errors: none
```

So the full path works: instance → system → D3D12 device on the runtime's
adapter → session → swapchain → acquire/paint/release → quad layer → `xrEndFrame`,
sustained at ~119 fps with the layer attached on every rendered frame.

### A diagnostic worth keeping

The first run reported **`frames drawn: 1` out of 1636** with the session parked
in state 3 (`SYNCHRONIZED`). That is not a bug: `shouldRender` is false until the
compositor grants `VISIBLE`/`FOCUSED`, which it will not do while the headset is
asleep. Adding session-state transition logging made the difference immediately
visible, and the second run walked IDLE→READY→SYNCHRONIZED→VISIBLE→FOCUSED and
drew 1085 frames. **Any future "nothing appears in the headset" report should
start by checking the session state**, not the rendering code.

### Note on the measured frame cost

`cpu/frame` is 4.75 ms in the rendering run (vs 1.36 ms while merely idling in
SYNCHRONIZED). That is inflated by the probe's fully synchronous structure — it
signals a fence and waits for the GPU every frame. A real implementation
pipelines frames with multiple in-flight command lists; the number to watch
instead is whether the depth work fits the display period with the depth on a
worker thread.

### Still to confirm

Whether the quad is actually *visible* — that needs eyes in the headset. The
compositor accepted 1085 frames with the layer attached and raised no error,
which is strong evidence, but it is not the same as someone seeing it.

## Next steps for M2

1. ~~Create a D3D12 device + direct command queue; `xrCreateSession` with
   `XrGraphicsBindingD3D12KHR`.~~ **done**
2. ~~Create a swapchain and the per-image texture array.~~ **done**
3. ~~Frame loop: wait/begin → acquire/compose/release → quad layer → end.~~ **done**
4. Eyeball confirmation of the quad in-headset.
5. Replace the test pattern with the display frame, then M3 attaches depth via
   `XR_KHR_composition_layer_depth`.

Instrumentation to add with the loop: per-frame CPU time, the runtime's predicted
display period, and whether we are consistently hitting the compositor deadline —
that is what tells us whether the depth work fits alongside rendering.
