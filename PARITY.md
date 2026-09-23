# Linux renderer parity with Windows

Assessment updated: 2026-09-23, at `36e3d3c` ("Improve Linux stereo and live
capture parity") on `xlinux`. The original assessment was made at `0effa26`.

This compares the Linux renderer in `linux/` with the checked-in Windows
renderer in `bench/native/openxr/xrapp5.cpp`. The controller is mentioned where
it changes engine behaviour. Features explicitly deferred to L5 in [LINUX.md](LINUX.md)
are listed separately from the first-release gaps.

## Current position

The Linux port presents live PipeWire capture and CUDA ZipDepth in SteamVR via
OpenXR, with a Windows-derived room eye shader, glass, floor, ceiling light and
reflections. The PICO 4 user confirmed that the room and screen aligned before
the latest stereo corrections. The corrected stereo run matched the CPU
reference exactly; its appearance after that change still needs a headset check.

The highest-impact remaining visual gap is **colour resolution**. Linux downsizes
capture to 686×392 before warping and uses that size for both eyes and the room
picture. Windows can warp colour at up to 1920 pixels wide while sampling a
686×392 depth map. The largest frame-time gap is the Linux CPU/host-memory path,
including a per-frame fence wait.

At `36e3d3c`, a 20-second live PICO 4/Steam Link run rendered 876 OpenXR images
with no runtime skips. It captured 746 frames with no ring drops and completed
673 CUDA depth updates. Median/p95 work after `xrBeginFrame` was 15.77/21.16 ms.
These counts describe this host and run, not an L4 latency acceptance result.
All eight Linux CTest checks passed. The first live GPU/CPU warp comparison had
zero differing colour channels and zero differing depth pixels.

## 1. Stereo picture

| # | Difference | Status at `36e3d3c` | Remaining work |
|---|---|---|---|
| 1a | Screen-plane disparity | **Fixed.** `VulkanWarp` subtracts `1/settings.distance` from both inverse-depth bounds, as the Windows quad path does. The CPU reference uses the same values. | Check the perceived depth centre while changing Distance in the controller. |
| 1b | Depth range | **Fixed.** Near/far inverse-depth bounds are now `1/1.2` and `1/12`, matching Windows. | Include these parameters in the visual check. |
| 1c | Eye separation | **Fixed.** `xrLocateViews` runs on rendered frames with or without the room; the warp uses half the measured IPD. Implausible readings retain the last valid value, initially 64 mm. | Confirm comfort and eye orientation in the headset. |
| 1d | Colour resolution | **Open.** The source, warp output, swapchain and room picture still use 686×392 colour. Depth also uses 686×392. | Separate colour dimensions from depth dimensions and keep capture colour at a higher resolution. This is the main picture-quality task. |
| 1e | Capture downscale filter | **Improved.** `ScaleCapture` uses up to 4×4 averaged source samples when shrinking and bilinear filtering when enlarging. A 4× checkerboard regression now averages to grey. | The shrink filter uses point samples in its box, so it is not pixel-identical to Windows' bilinear-sample box. Full-resolution colour should remove most of this downscale. |
| 1f | Source aspect ratio | **Partly fixed.** `FitCapture` now letterboxes narrow and wide sources into the fixed texture instead of stretching them. | The OpenXR quad and room opening still use the fixed 686:392 geometry. Propagate source aspect through screen sizing and room inputs when the colour path becomes dynamic. |
| 1g | Depth range stability | **Fixed for live ZipDepth.** `RangeSmoother` applies the Windows 0.5/99.5 percentile range, 0.4 s time constant and scene-cut snap before dilation. Still-image inference remains unsmoothed by design. | Check video with changing scenes in the headset. |
| 1h | Stale or mismatched depth | **Fixed.** The live renderer requires matching layout generations and applies the portable `UsableDepth` 250 ms source-timestamp policy; otherwise it shows flat colour until matching depth arrives. | Exercise a source resize/layout change and capture interruption in an end-to-end run. |

The implementations are in [vulkan_warp.cpp](linux/vulkan_warp.cpp),
[xr_synthetic.cpp](linux/xr_synthetic.cpp), [capture_scale.h](linux/capture_scale.h),
[depth_range.h](linux/depth_range.h) and [live_depth.cpp](linux/live_depth.cpp).
The CPU/GPU warp parity check validates the output math, but it does not replace
a headset judgement of the new screen-plane correction.

## 2. Frame path and performance

The current Linux render path still:

1. receives a shared-memory PipeWire frame and fits/downscales it on the capture
   thread;
2. copies RGB into a host-mapped Vulkan buffer and packs it to RGBA on the CPU;
3. dispatches the stereo warp as `(1, 392, 2)` one-thread workgroups, each
   serially processing a full scanline;
4. computes the room ambilight reference on the CPU, then runs EMIT, MIRROR,
   LIGHT and the room eye shader on the GPU;
5. copies outputs into OpenXR swapchains and waits for the command fence on
   the CPU before `xrEndFrame`.

The host-visible/coherent working buffers remain in system memory on a discrete
GPU. In the latest live run, CPU model preparation was 16.79/18.58 ms median/p95;
arrival-to-finished-depth was 26.77/31.20 ms. Arrival is stamped after capture
copy and scaling, so these figures exclude upstream capture time and headset
display latency. The standalone 3840×2160 capture-scaling benchmark took about
3 ms per frame after the latest filter change.

Windows uses shared GPU capture textures, GPU model preparation, device-local
render targets and a ring of command lists/fences. Its render thread does not
wait on a GPU fence every frame. The Linux work should proceed as follows:

- **2a. Decouple colour and depth resolution.** Make warp inputs and outputs
  carry independent colour (`cw×ch`) and depth (`dw×dh`) dimensions, then raise
  colour resolution without increasing the depth model size. Measure the room
  picture and swapchain costs on the PICO 4.
- **2b. Reduce host-memory traffic.** Move scene, depth, stereo output and room
  images into device-local resources where supported, staging only new data.
  Keep a working copy path while changing the resource lifetime.
- **2c. Remove the render-thread fence wait.** Use a small command-buffer/fence
  ring and a swapchain-compatible copy or direct write path. Respect OpenXR
  image acquisition and release ordering.
- **2d. Improve warp occupancy.** The current one-thread scanline preserves the
  deterministic CPU reference but wastes GPU lanes. Parallelise rows and then
  the scatter/fill work without losing occlusion or the reference check.
- **2e. Import PipeWire DMA-BUFs where available.** Negotiate buffer types and
  modifiers, import supported frames into Vulkan, and retain shared-memory
  capture as the fallback. Validate actual compositor/driver support before
  relying on this path for the first release.
- **2f. Move ambilight and model input preparation to the GPU.** Keep glow
  history in a format that converges under small changes; Linux currently uses
  an 8-bit 64×45 history while Windows uses a larger floating-point history.
  The existing Vulkan model-prep shader already has CPU-reference coverage,
  but live CUDA currently prepares its tensor on the CPU.
- **2g. Separate queues and latency policy.** Schedule depth preparation away
  from the render queue and add GPU source-image history before adding Windows'
  Delayed/Matched timing modes.

DMA-BUF import is valuable, but it is not the only route to a higher-resolution
first image. A staged upload path can establish the visual and performance
baseline before zero-copy capture is available.

## 3. Room and runtime behaviour

| Feature | Linux status | Remaining work |
|---|---|---|
| Room EMIT / MIRROR / LIGHT | Runs on the GPU each room frame; reference probes match within 1e-5. | Keep parity checks as shader/resources change. |
| Room eye pass | Built from the checked-in Windows HLSL; `--room-dump` compares sampled output with `RoomPixel`. Only the "look" variant is built. | Build the plain variant if it materially reduces cost with effects disabled. |
| Room GPU timing | **Partly done.** Vulkan timestamp queries report first-frame EMIT, MIRROR, LIGHT, upload, eye and copy times. A live sample totalled 2.97 ms for these room passes. | Add warp timing and rolling p50/p95 per-pass logs; first-frame timing alone does not establish steady-state cost. |
| Room eye size | Fixed at 1322×1322, half this PICO 4 runtime's recommended 2644×2644. | Derive width and height from OpenXR recommendations for other headsets. |
| Turning Room on live | The controller always launches with `--room`, so its Room slider can activate the already-created room resources from zero. | A direct CLI launch without `--room` and with Room initially zero still cannot create room resources later. Add lazy creation if that mode matters. |
| Recenter / `ScreenAnchor` | Missing; room and screen are fixed to the `LOCAL` origin. | Add a yaw-only recenter control and a versioned live-settings counter. |
| STAGE floor | Located each room frame when available. | Latch a valid floor estimate and handle reference-space change events. |
| Tracking loss | Room projection is skipped but screen quads continue. | Decide whether to keep this documented behaviour or match Windows' empty-frame policy. |
| Standalone ambilight | Glow is only part of the room. | Add the separate glow/world-colour layers after GPU ambilight is implemented. |
| Running performance log | CPU room prep, submit/wait, endFrame and total work report median/p95 at exit; capture/depth totals also report at exit. | Add a compact two-second running report and depth/source-age measurements. |
| Self-test before presenting | Warp is checked on the first frame; room eye check requires `--room-dump`; failure affects exit status. | Run required GPU/CPU checks before presenting frames and refuse presentation on failure. |
| Session loss | Stops. | No known parity gap. |

The current room-on synthetic run produced about 43 rendered frames/s while
focused; without the room it produced about 86 frames/s in the earlier probe.
The latest live run was about 44 rendered frames/s. Room GPU passes themselves
are roughly 2–3 ms in the sampled first frames, while median total work after
`xrBeginFrame` was 15.77 ms live. More timing is needed before attributing the
remaining cost to any one stage.

## 4. Structure and release scope

[xr_synthetic.cpp](linux/xr_synthetic.cpp) still combines OpenXR session setup,
source selection, depth policy, Vulkan submission and layer assembly in one
large `Run()` function. Before adding multiple colour sizes, imported images and
a command ring, split those responsibilities into session, source, depth and
renderer components. Preserve a synthetic mode and run the same parity checks
through the production renderer. [vulkan_room.cpp](linux/vulkan_room.cpp) should
be reformatted before its resource and pass logic expands further.

The first usable release still needs an end-to-end controller/session check,
recenter and runtime geometry decisions, continuous latency/frame timing,
portable packaging and clean recovery checks as described by the L4 gate in
[LINUX.md](LINUX.md). The Windows-style room is required for that release and is
already present, but its performance and portability need further work.

Curved screen, Delayed/Matched timing, fusion/steady processing, a motion
estimator, a second GPU, Depth Anything and foreground crop remain L5 work
under [LINUX.md](LINUX.md).

## Suggested next sequence

1. Check the corrected stereo placement in the PICO 4 while changing Distance;
   check a resized or portrait capture for aspect and depth recovery.
2. Refactor the engine boundary, then prototype higher-resolution colour with
   independent depth size. Record image quality, frame time and memory use.
3. Make room eye size runtime-derived, add recenter and floor-space handling,
   and add rolling GPU/CPU/source-age timing for the L4 acceptance run.
4. Improve Vulkan memory, submission and warp parallelism; import DMA-BUF where
   supported; move ambilight and model preparation off the CPU.
5. Complete L4 packaging and acceptance, then take up L5 features.
