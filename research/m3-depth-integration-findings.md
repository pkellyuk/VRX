# M3 findings — depth into OpenXR

Date: 2026-09-17
Probes: `bench/native/dmlgpu/dmlgpu.cpp` (model + device sharing),
`bench/native/openxr/xrapp.cpp` (OpenXR session)

## Constraint 1 — depth attaches to a *projection view*, not a quad

Read straight out of the shipped header rather than assumed:

```
// XrCompositionLayerDepthInfoKHR extends XrCompositionLayerProjectionView
```

`research/openxr-depth-apis.md` described "attaching a depth image to a 2D
composition layer". That is wrong: `XrCompositionLayerDepthInfoKHR` chains into
`XrCompositionLayerProjectionView`. A quad layer cannot carry depth.

**Consequence for M3:** the content must be presented as a **projection layer**
with per-eye views, not the quad layer M2 uses. This matches how the shipping
prior art works (reference-app warps per eye with `stereo_warp.hlsl` and presents
projection output), and it means M3 involves per-eye rendering, not just bolting
a depth image onto the existing quad.

A useful side effect: because the projection view's colour and depth sub-images
must have identical rects, the depth swapchain can be sized to the model's output
(392x686). That gives a 1:1 depth write with **no resize shader**.

## Constraint 2 — the model and the OpenXR session must share one device

The depth swapchain's images are D3D12 resources owned by the OpenXR session's
device. For DirectML to write into them, ORT must run on that same device.

Verified working via `--own-device`: we create the D3D12 device and a DIRECT
queue, call `DMLCreateDevice` on it (resolved from `DirectML.dll` with
`GetProcAddress`, so no import library), then hand both to ORT with
`OrtDmlApi::SessionOptionsAppendExecutionProvider_DML1` (the header's name for
the `Ex_DML` entry point):

```
device: NVIDIA GeForce RTX 3090 (created here)
ORT   : shares our device + queue (Ex_DML)
live check : maxDiff=1.6250 over 268681/268912 elements
```

Identical results to the default path (checksums match to the digit), so sharing
the device costs nothing in correctness. This was M3's main technical risk and it
is now retired.

## Constraint 3 — GPU contention dominates the latency budget

The M1 figures were measured with SteamVR **not running**. A VR app never runs
in that condition. Measured, same model, same geometry (392x686, 1372 patches):

| GPU state | p50 | fps |
|---|---|---|
| SteamVR not running (idle GPU) | 13.89 ms | 72 |
| **Our VR app presenting (foreground)** | **16.76 ms** | **60** |
| SteamVR home environment rendering (`steamtours.exe`, GPU at 83%) | 39.05 ms | 26 |

So the honest deployment figure is **~16.8 ms / 60 fps**, about 20% worse than
the idle number — acceptable. The 39 ms case is SteamVR's *home environment*
burning 83% of the GPU while no app owns the session; it disappears as soon as a
real app presents, so it is not the number to plan against. But it is a sharp
reminder: **any latency figure for this feature is meaningless without stating
what else the GPU is doing.** The M1 numbers should be read as idle-GPU figures.

At 60 fps the depth path is viable at 60 Hz per-frame, and at 90 Hz it needs the
depth on a worker thread with temporal reuse (refreshing roughly 2 frames in 3).

## Result — the 2D->3D effect works in-headset (eyeball-confirmed)

`bench/native/openxr/xrapp3.cpp`, run with
`xrapp3 60 --image=C:\Windows\Web\Wallpaper\Spotlight\img14.jpg` on a PS VR2 via
SteamVR: a real photo, depth from the model, fuses into a single 3D image.
Model 15.9–16.1 ms, CPU warp+pack 5–8 ms. Three bugs had to be fixed to get
there, each worth remembering:

1. **Same image in both eyes => declare the SAME symmetric FOV and orientation.**
   The runtime's per-eye FOVs are asymmetric and mirrored (measured: eye 0
   -61.5/+43.4 deg, eye 1 -43.4/+61.5 deg). Declaring them for a shared image
   puts the image centre ~9 deg outward in each eye: ~18 deg of *divergent*
   disparity, unfusable ("left image is too far left"). Fix: both projection
   views declare one symmetric FOV (smallest half-angle of either eye, vertical
   from the image aspect so pixels stay square) and one nlerp'd orientation. The
   compositor reprojects the declared FOV onto the display. Unwarped content then
   sits at infinity and the warp adds only convergent disparity.
2. **The stereo warp must be a forward warp with a z-test.** The first version
   looked up disparity at the *destination* pixel. On a flat-coloured object that
   only trims one edge instead of moving it, and lets far content overwrite near
   content — a weak, wrong-looking effect. Now every source pixel moves by its own
   disparity (`focal * eyeOffset / Z`, eye offset = +-IPD/2 from the measured eye
   positions), nearer wins, holes are filled from the farther neighbour. The depth
   image is warped per eye identically.
3. **Flat synthetic scenes are useless as model input.** For the rectangle test
   scene the model returned nearness backdrop 0.43 / panel 0.33 / marker 0.44
   against a truth of 0 / 0.5 / 1 — i.e. arbitrary, because there are no monocular
   cues. It looked like a sign bug in the warp and was not. `--truth` (ground-truth
   depth, tests the warp alone), `--image=` (real content, tests the model) and
   `--dump` exist to keep those two questions separate. The pre-loop model check
   prints the model's opinion even if the HMD never wakes.

## Result — SteamVR ignores `XR_KHR_composition_layer_depth` (measured)

Attaching and detaching the depth chain on a head-locked picture shows nothing, so
the experiment forces the compositor's hand (`xrapp4 --freeze-pose --ab=6
--depth-lie`): the layer is submitted with a deliberately **stale pose**, so the
compositor must reproject it to the live head pose; the depth chain toggles every
6 s (green/red block in the picture); and the depth is a gross **lie** (left half
0.5 m, right half 10 m). A compositor that uses depth would shear the two halves
apart as the head translates during green phases only.

**Observed (SteamVR 2.17.9, PS VR2): identical behaviour in green and red phases.**
SteamVR advertises the extension and accepts the chain, but does not use it —
reprojection is rotation-only either way. Consequences:

- The visible 3D is **entirely** the stereo warp. Depth submission buys nothing on
  SteamVR, so `xrapp4` now has it **off by default** — no depth swapchain, no
  per-frame depth copy. `--submit-depth` keeps the path alive for runtimes that do
  consume depth (e.g. Oculus/WMR positional timewarp) — untested there.
- This also closes INVESTIGATION.md's "runtime validation" step for SteamVR: the
  `XR_MSFT_composition_layer_reprojection` route was already unavailable (M2), and
  the KHR depth layer is a no-op. Head-motion parallax, if wanted, has to be done
  by us in the warp (re-render from the live pose), not by the compositor.
- Found on the way: the depth image was written as linear `1 - nearness`, which
  with nearZ 0.1 / farZ 30 decodes to ~0.1-1 m for nearly every value. It is now a
  real projective depth, `farZ/(farZ-nearZ) * (1 - nearZ/Z)`, for the same Z the
  warp uses (CPU reference and shader; self-test still 0 mismatches).

## M3b — worker-thread model + compute-shader warp (`xrapp4.cpp`)

`xrapp3` stays as the CPU reference; `xrapp4` is the target structure:

- **One device, two queues.** ORT/DirectML submits to an `ml` queue from a worker
  thread; the OpenXR session and the warp use a `gfx` queue on the render thread.
  The render thread never waits for the model — it takes the latest *completed*
  depth through a lock-free triple buffer. Frames are pipelined (3-frame ring of
  allocators/upload buffers, fence-guarded), with no per-frame CPU wait.
- **The warp is a compute shader.** A forward warp is a scatter with a z-test,
  which races per pixel — but rows are independent, so one GPU thread runs one
  whole row of one eye (`Dispatch(1, H/8, 2)`), the same algorithm as the CPU
  `WarpEye`. It writes RGBA8/R32F typeless intermediates that are copied into the
  swapchain images (SteamVR's are `R8G8B8A8_TYPELESS` / `R32_TYPELESS`, so the
  copies are same-format; a D32 image cannot be a UAV, hence the intermediate).
- **Verified without a headset:** `--selftest` compares the GPU warp with the CPU
  reference pixel-for-pixel — 4 cases (truth, model depth, 4x disparity, no-warp),
  **0 mismatches of 537,824 px** in each. `--debug` (D3D12 debug layer) reports
  nothing from our code; the only errors are SteamVR's own D3D11-interop
  `ReflectSharedProperties` messages.
- **Measured (HMD asleep, so only one frame presented):** render loop 120 fps at
  ~2 ms CPU/frame while the worker ran the model 58.7 times/s (17 ms) — i.e. the
  two rates are decoupled. GPU warp of both eyes incl. upload + readback ~2 ms vs
  5–8 ms CPU warp+pack in xrapp3. **Still to measure with the HMD awake:** drawn
  fps, depth updates/s and depth age under real compositor load.
- **Measured with the HMD awake (PS VR2, 120 Hz, eyeball-confirmed stereo image):**
  **119.9 fps presented, every frame drawn, 0.13 ms CPU per frame**, while the
  worker delivered **59.9 depth updates/s** (model 16.3-16.7 ms) with an average
  depth age of ~10 ms at use. So the display runs at full rate and depth refreshes
  every second frame - the temporal-reuse design from INVESTIGATION.md, measured.
  (Static image, so depth lag was not visible; moving content is the next test.)
- Spec check that also fixed xrapp3: acquired D3D12 swapchain images are in
  `RENDER_TARGET` (colour) / `DEPTH_WRITE` (depth) and must be released in that
  state — not `COMMON`, which xrapp3 had assumed.
- Still CPU-side by necessity: the model *output* (M1: ORT cannot bind a DML
  device output here) — 1 MB read back and re-uploaded as a structured buffer,
  plus min/max normalisation. `--paired` warps the colour frame the depth came
  from, as an A/B against the default (latest colour + latest completed depth).

## Remaining M3 work

Items 1-5 below are done (`xrapp3.cpp` on the CPU, `xrapp4.cpp` with the worker
thread and compute-shader warp). Still open: in-headset run of xrapp4, real
display capture as the input, and temporal smoothing of the per-frame min/max
normalisation.

1. Switch `xrapp` from a quad layer to a **projection layer** with two views, and
   enable `XR_KHR_composition_layer_depth` on the instance.
2. Create a depth swapchain (`D32_FLOAT`, `arraySize` = view count,
   `XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`), sized to the model output.
3. Run the model on a **worker thread**, latest-completed handoff, and write its
   output into the depth swapchain image each update.
   - Writing float depth into a depth texture needs care: a buffer->texture copy
     requires a 256-byte-aligned source row pitch, and 686 x 4 = 2744 is not
     aligned. Either pad rows to 2816 and use `CopyTextureRegion`, or write with a
     compute shader (`RWTexture2D<float>`) which is the GPU-only path and the one
     to prefer.
4. Convert the model's *relative* depth into the depth range the runtime expects
   (`minDepth`/`nearZ`/`farZ`). DA-V2 outputs relative disparity, not metric
   depth, so this is a normalisation, not a calibration — a real limitation to
   document, not paper over.
5. Then the actual effect: warp the content per eye using the depth (the
   `stereo_warp` step) so the flat content gains parallax.

## Note on the DML output → depth texture route

The model's output is a linear `float` buffer. Writing it into an OpenXR depth
texture is the one genuinely awkward step left, and the row-pitch alignment rule
(above) is why a naive `CopyTextureRegion` will fail. Worth budgetting a
debugging cycle for it.
