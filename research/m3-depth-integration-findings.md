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

## Remaining M3 work

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
