# M4 findings — live capture, GPU-resident input, smoothed depth range

Date: 2026-09-17
Probe: `bench/native/openxr/xrapp5.cpp` (builds as C++20: C++/WinRT needs `<coroutine>`)

## Result — live 3D desktop video in the headset (eyeball-confirmed)

`xrapp5 120` capturing the primary monitor while playing YouTube in a browser:
the video is seen in stereo 3D on the PS VR2, live. This is the end-to-end goal of
the project — arbitrary 2D screen content turned into 3D in real time — working.

## What xrapp5 is

xrapp4's structure (worker-thread model, compute-shader warp, pipelined 120 fps
render loop) with a real, GPU-resident input:

```
capture thread (D3D11, Windows.Graphics.Capture, monitor or window, cursor included)
    frame -> CopySubresourceRegion into a ring of 8 SHARED textures -> shared fence
worker thread (ml queue)                       render thread (gfx queue, OpenXR)
    prep shader: source tex -> model input        latest source tex (full res)
      (NCHW, box-filtered, normalised, VRAM)      + latest COMPLETED depth (686x392)
    model (ORT + DirectML)                        -> forward warp at COLOUR resolution,
    percentile + time-smoothed range                 depth sampled bilinearly
    -> publish depth slot                         -> colour swapchain (up to 1920 wide)
```

## Settled by measurement

- **D3D11 <-> D3D12 sharing works with plain NT handles.** Textures are created in
  D3D12 (`HEAP_FLAG_SHARED`, `ALLOW_RENDER_TARGET | ALLOW_SIMULTANEOUS_ACCESS`,
  `B8G8R8A8_UNORM`), opened in D3D11 with `OpenSharedResource1`; a
  `D3D12_FENCE_FLAG_SHARED` fence is opened with `ID3D11Device5::OpenSharedFence`.
  The capture thread signals it after each copy and both D3D12 queues
  `Wait()` on it — GPU-timeline ordering, no CPU stall. The D3D11 device must be on
  the same adapter (the one the OpenXR runtime demands). No D3D11On12 needed.
- **Simultaneous-access textures need no barriers** (one writer, readers on two
  queues and another device). D3D12 debug layer: nothing from our code.
- **The picture never touches the CPU.** The model input is now a DEFAULT-heap
  buffer written by a compute shader (it was a CPU-written L0 buffer). Only the
  model's 1 MB output comes back (M1 limitation).
- **Both shaders are self-verified at every start**: prep vs CPU preprocessing —
  0 of 806,736 values differ (worst 3.6e-7); warp vs `WarpEye` — 0 colour
  mismatches in 4 cases (2 depth px of 537,824 at 4x disparity: rounding ties).
  The colour-resolution warp with bilinear depth degenerates exactly to the CPU
  reference when colour and depth sizes match, which is what makes this testable.
- **Measured (3840x2160 desktop, HMD asleep):** capture 60 fps (the monitor's
  rate), presented at 1920x1080 = 22 px/deg across the 87 deg shared FOV, worker 54
  runs/s alongside, render loop 120 fps.

## Depth-range smoothing

DA-V2's output is relative, and its scale moves from frame to frame; per-frame
min/max normalisation makes the whole picture's depth "breathe", and a single hot
pixel rescales everything. Now: 0.5/99.5 percentiles (1024-bin histogram), smoothed
exponentially with a time constant (`--tau`, default 0.4 s), snapping immediately on
a large jump (scene cut). `--no-smooth` restores per-frame min/max for an A/B.
Not addressed: per-pixel temporal flicker of the depth map itself.

## Known limitations / next

- A static desktop delivers no new frames, so the worker idles (by design). Flat
  windows give flat depth — the model sees a window as a near panel. Video and
  games are where depth is meaningful.
- ~~Streaks beside near edges~~ — fixed, see "Disocclusion fill" below.
- Ring of 8 source textures is a time-based guarantee (a reader lags the writer by
  a few frames at most), not reference counting.
- Window capture keeps the initial size; a resized window is copied top-left.

## Disocclusion fill — streaks fixed (mirror fill + foreground depth dilation)

When a near object shifts for one eye it uncovers a strip of background the source
never contained. At 1920 wide the nearest content moves ~30 px per eye, so these
holes are up to ~27 px wide beside every near edge (and along the image border).

**Two separate defects, measured on GPU-warped eye dumps** (`xrapp5 --dump`, which
warps exactly as the frame loop does and writes both eyes for both fill modes):

1. **Streaks** — the original fill repeated the one background pixel beside the
   hole across its whole width, so every row became a flat band: a blocky streak
   column next to each near object, and a smeared border. Now **mirror fill**
   (default, `--fill=mirror`): the hole reflects the background texture outward
   about its edge (`dest anchor+k <- source anchorSrc -/+ k`), so the texture
   continues across the seam. A guard stops the reflection pulling in anything
   nearer than the anchor (another object) by holding the last good pixel
   (`MIRROR_TOL` = 0.08 nearness). On the test composite (a near panel over a pine
   forest) the old fill shows an obvious striped band; with mirror the trees and
   hills continue through it and the fill is hard to find at 1:1.
2. **Ghost edge lines** — depth is 686 wide and colour 1920, so the model's (soft)
   depth edge lands a few colour pixels off the colour edge. Foreground-coloured
   pixels that receive background depth stay behind when the object moves and
   draw a thin high-contrast line inside the hole. Fix: **dilate the near field
   horizontally** in the worker before upload (`--dilate=N` depth px, running max).
   Measured at 4x zoom on both eyes: N=0 clear ghost lines; N=1 still a line at
   the softest corner; **N=2 clean** apart from a 1-2 px tick (default); N=3 clean.
   Cost: ~5.6 colour px of background travel with each object — far less visible
   than a ghost line. Horizontal only, because disparity is horizontal.

Verification: the CPU reference (`FillHole` / `WarpEyeFill` in `xr_common.h`) and
the shader implement the same fill; the self-test now runs both modes including 4x
disparity (wide holes that exercise the guard) — 0 colour mismatches in every case.
xrapp3/xrapp4 keep the stretch fill via the unchanged `WarpEye` (their self-test is
still 0 mismatches). Not yet re-checked in the headset; the shader's extra work is
one bilinear depth lookup per hole pixel (~1-2% of pixels).

Remaining visible compromises: the reflection can show as a mirrored pattern in
strongly structured texture, and hole width varies row to row so the fill has no
vertical coherence. A 2D inpaint (push-pull) would address both at more cost.
