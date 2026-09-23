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

Colour resolution now follows Windows: live colour is kept at the source's size
up to 1920 pixels wide and warped at that size, with the 686×392 depth map
sampled bilinearly. The largest remaining frame-time gap is the CPU capture
scaling and the per-frame fence wait.

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
| 1d | Colour resolution | **Fixed; headset check pending.** Capture keeps colour at the source size shrunk to at most 1920 wide (even dimensions, as `MAX_COLOR_W`); later sizes are letterboxed into it. The warp runs at colour size and samples the 686×392 depth bilinearly (`NearAt`); the swapchain, quads and room picture use the colour size. The depth model sees the colour texture stretched to its grid, as on Windows. GPU/CPU warp checks match exactly at 686×392 and 1920×1080 (`--color=WxH` runs the synthetic scene at a chosen colour size). | Confirm sharpness and depth alignment with live 4K capture in the PICO 4. |
| 1i | Focal length | **Fixed.** The warp uses `cw × distance / width` colour pixels, as xrapp5 does. Linux previously used a fixed 686, which was only correct when width equalled distance. | Include Width changes in the headset depth check. |
| 1e | Capture downscale filter | **Improved.** `ScaleCapture` uses up to 4×4 averaged source samples when shrinking and bilinear filtering when enlarging. A 4× checkerboard regression now averages to grey. | The shrink filter uses point samples in its box, so it is not pixel-identical to Windows' bilinear-sample box. Full-resolution colour should remove most of this downscale. |
| 1f | Source aspect ratio | **Fixed.** The colour texture takes the first frame's aspect ratio; the quads and the room opening use it. Later frames of a different shape are letterboxed into it, as on Windows. | None beyond the 1d headset check. |
| 1g | Depth range stability | **Fixed for live ZipDepth.** `RangeSmoother` applies the Windows 0.5/99.5 percentile range, 0.4 s time constant and scene-cut snap before dilation. Still-image inference remains unsmoothed by design. | Check video with changing scenes in the headset. |
| 1h | Stale or mismatched depth | **Fixed.** The live renderer requires matching layout generations and applies the portable `UsableDepth` 250 ms source-timestamp policy; otherwise it shows flat colour until matching depth arrives. | Exercise a source resize/layout change and capture interruption in an end-to-end run. |

The implementations are in [vulkan_warp.cpp](linux/vulkan_warp.cpp),
[xr_synthetic.cpp](linux/xr_synthetic.cpp), [capture_scale.h](linux/capture_scale.h),
[depth_range.h](linux/depth_range.h) and [live_depth.cpp](linux/live_depth.cpp).
The CPU/GPU warp parity check validates the output math, but it does not replace
a headset judgement of the new screen-plane correction.

## 2. Frame path and performance

The current Linux render path:

1. receives a shared-memory PipeWire frame and scales it on the capture thread
   to the colour texture, then to the depth grid (about 2.1 + 0.7 ms for a 4K
   source on four threads of the reference Ryzen 7 5700X);
2. copies packed RGBA into a host staging buffer, then to device-local memory
   (1.5 ms GPU copy at 1920×1080);
3. dispatches the stereo warp over device-local buffers, one invocation per row
   and eight rows per workgroup (1.2 ms GPU at 1920×1080, 0.4 ms at 686×392);
4. computes the room ambilight reference on the CPU, then runs EMIT, MIRROR,
   LIGHT and the room eye shader on the GPU;
5. copies outputs into OpenXR swapchains and submits without waiting: two
   frames are in flight, and each frame's results (model input, room glow,
   DMA-BUF release) are handled when its fence signals.

The warp's working buffers are now device-local; the room's remain host-visible. In the latest live run, CPU model preparation was 16.79/18.58 ms median/p95;
arrival-to-finished-depth was 26.77/31.20 ms. Arrival is stamped after capture
copy and scaling, so these figures exclude upstream capture time and headset
display latency. The standalone 3840×2160 capture-scaling benchmark took about
3 ms per frame after the latest filter change.

Windows uses shared GPU capture textures, GPU model preparation, device-local
render targets and a ring of command lists/fences. Its render thread does not
wait on a GPU fence every frame. The Linux work should proceed as follows:

- **2a. Decouple colour and depth resolution.** Done (see 1d). Measure the
  room picture and swapchain costs on the PICO 4.
- **2b. Reduce host-memory traffic.** Done for the warp (device-local scene,
  depth, output and scratch, with staging uploads). Room buffers remain
  host-visible. Upload depth only when a new map arrives.
- **2c. Remove the render-thread fence wait.** Done: two command buffers and
  fences; per-frame staging and model-input buffers; room constants, glow and
  emitters written with `vkCmdUpdateBuffer`; a barrier orders GPU frames. The
  CPU wait fell from 4–6 ms to 0.01 ms. With depth and the full-size room,
  shared memory rose from 52 to 89 frames/s and DMA-BUF holds 89 frames/s with
  22.8 ms arrival-to-depth. Checked frames still wait for their results.
- **2d. Improve warp occupancy.** Rows now run eight to a workgroup, as on
  Windows. Each row is still serial; parallelise scatter/fill only if timing
  shows the warp matters.
- **2d′. Capture scaling.** The CPU resampler now splits rows across four
  threads with bit-identical output: 4K scaling fell from about 10 ms to 2.7 ms.
  Uploading shared-memory 4K frames for GPU scaling is not worthwhile on the
  reference host: its PCIe 3.0 x8 link takes 8.5 ms of GPU time per 33 MB
  frame, against 1.5 ms for the scaled 1920x1080 colour. GPU scaling should
  wait for DMA-BUF import (2e).
- **2e. Import PipeWire DMA-BUFs where available.** `vrx-capture-probe --dmabuf`
  proves it on the reference host (KDE Wayland, NVIDIA 595.71): Vulkan can
  import six NVIDIA block-linear modifiers and LINEAR for B8G8R8A8; KWin offered
  the same block-linear set; the probe fixed `0x0300000000606015`, streamed
  3840x2160 BGRA DMA-BUFs, imported all three buffers (about 1 ms each, once
  per buffer) and read back a correct desktop image. `vulkan_dmabuf.h` holds the
  reusable importer. **The engine now uses it by default for live capture**:
  it offers the OpenXR GPU's modifiers, holds each PipeWire buffer until the
  frame using it has completed, imports each buffer once, and scales it into
  the colour buffer with `capture_scale.comp` (bit-identical to the CPU scaler;
  `vrx-capture-gpu-test` covers 2x2..4x4 box, bilinear and letterboxed cases).
  Shared memory remains the fallback (`--no-dmabuf`). A 60 s PICO 4 session
  captured 3,114 4K frames with no drops. Explicit sync
  (`SPA_META_SyncTimeline`) is negotiated when a DRM render node supports
  syncobjs: the renderer waits (up to 20 ms, on the CPU) for each buffer's
  acquire point and signals its release point when the frame using it
  completes. KWin 6.6 accepts it; a 20 s monitor capture delivered a steady
  60 frames/s with no drops and unchanged 22.7 ms arrival-to-depth.
  Implicit sync remains the fallback.
- **2f. Model input and ambilight.** Done for model input: live frames are
  prepared by `model_prep.comp` from the colour buffer (xrapp5's taps,
  `ceil(width / 672)`), and the depth worker receives the tensor. This removed
  about 17 ms of CPU preparation; arrival-to-depth fell to about 18 ms median.
  The live room glow uses a quarter-size copy of that input, and its float
  history now lives in CPU memory (the 8-bit history stopped converging).
  Ambilight is still computed on the CPU (about 1 ms) at 64x45.
- **2g. Separate queues and latency policy.** Delayed/Matched timing is done
  (below). Depth preparation still runs on the render queue; it costs about
  2 ms of the 5 ms GPU frame and has not needed a queue of its own yet.
- **Frame timing (Latest, Delayed, Matched).** Done, as xrapp5 with the shared
  `frame_timing.h`: each new live frame is kept in an 8-frame GPU history
  after depth is prepared from it; Delayed shows the newest frame at least the
  smoothed depth delay old (never older than the depth's own frame) and
  Matched the depth's own frame. The choice is per profile and applies live
  (`VRXL 4`). On the PICO 4, Delayed looked smoother than Latest, so Linux
  makes it the default (Windows defaults to Latest).

DMA-BUF import is valuable, but it is not the only route to a higher-resolution
first image. A staged upload path can establish the visual and performance
baseline before zero-copy capture is available.

## 3. Room and runtime behaviour

| Feature | Linux status | Remaining work |
|---|---|---|
| Room EMIT / MIRROR / LIGHT | Runs on the GPU each room frame; reference probes match within 1e-5. | Keep parity checks as shader/resources change. |
| Room eye pass | Built from the checked-in Windows HLSL; `--room-dump` compares sampled output with `RoomPixel`. Only the "look" variant is built. | Build the plain variant if it materially reduces cost with effects disabled. |
| Room GPU timing | **Done.** The first room frame reports EMIT, MIRROR, LIGHT, upload, eye and copy; every frame is timed as prep+warp, room and copy (below). On the PICO 4 with the full-size room: room 2.3 ms, prep+warp 2.3 ms, 5.1 ms per frame p50. | None. |
| Room eye size | **Differs from Windows on purpose.** The runtime's recommended size (2644×2644 on the PICO 4); `VRX_ROOM_EYE_SCALE` lowers it. At Windows' half size, SteamVR/Steam Link on Linux showed the screen quads at the room layer's resolution, so the picture looked as soft as the reflections. The full-size room holds about 87 frames/s at 9.5 ms CPU work per frame. | Recheck if the runtime or streaming path changes. |
| Turning Room on live | The controller always launches with `--room`, so its Room slider can activate the already-created room resources from zero. | A direct CLI launch without `--room` and with Room initially zero still cannot create room resources later. Add lazy creation if that mode matters. |
| Recenter / `ScreenAnchor` | **Done.** The shared `screen_anchor.h` places the screen in front of the headset on the first tracked frame and on each Recenter (a `VRXL 3` counter from the desktop app), keeps only the heading while the room is on, and applies the placement sliders relative to that point. The room is built in the screen's frame, as in xrapp5. | Keyboard shortcuts remain undecided (LINUX.md). |
| STAGE floor | **Done.** Read once per placement and latched; re-read after a reference-space change. | None. |
| Tracking loss | Room projection is skipped but screen quads continue. | Decide whether to keep this documented behaviour or match Windows' empty-frame policy. |
| Standalone ambilight | Glow is only part of the room. | Add the separate glow/world-colour layers after GPU ambilight is implemented. |
| Running performance log | **Done.** Every 2 s, as xrapp5: frames/s (drawn), CPU ms/frame, capture frames/s and drops, depth updates/s, model ms, depth age and shown-frame age, and GPU p50/p95 per frame with prep+warp, room and copy p50 (timestamps read when each frame's fence signals). The desktop app shows the frame rate on its card. A live session held 90 frames/s at 1.5 ms CPU and 5.1 ms GPU per frame, with depth for every captured frame (about 58/s), depth age about 31 ms and frame age about 8.5 ms. | None; the depth range is not reported (Windows prints it). |
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

Curved screen, fusion/steady processing, a motion
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
