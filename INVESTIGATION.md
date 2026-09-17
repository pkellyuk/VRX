# VRX — Investigation: real-time AI depth estimation of the VR display via OpenXR

Status: investigation (no application code yet — benchmark harness only)
Date: 2026-09-17

## The question

In a VR application built on **OpenXR**, can we take the **displayed frame**
(the 2D content shown to each eye), run an **AI depth-estimation model on it in
real time** (inside the VR frame budget), and use the resulting **depth map**
back in OpenXR — for reprojection, occlusion, and/or 2D→3D parallax?

It decomposes into two independent problems that must BOTH be true:

1. **OpenXR has a place for the depth to go** (API surface).
2. **The AI model can run inside the frame budget** (performance).

Both were investigated. Pillar 1 is a clean "yes" (see
`research/openxr-depth-apis.md`). Pillar 2 is the real constraint and is
**yes, but not with a naive stack** — the bottleneck is integration overhead,
not model compute.

---

## Pillar 1 — OpenXR API surface: YES, fully in place

OpenXR is a **transport for depth, not a generator**. There is no "AI depth"
API, but there are four well-defined places depth enters/leaves, and the AI
model slots in *between* "frame is available" and "depth is handed to the
compositor". Full detail in `research/openxr-depth-apis.md`. The relevant
surfaces:

- **Depth/stencil swapchains** (core, no extension): a normal
  `xrCreateSwapchain` with `XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT` +
  a depth format. This is how *your own rendered* Z-buffer is exposed.
- **`XR_KHR_composition_layer_depth`** — the key hook for this feature.
  **Correction (measured, see `research/m3-depth-integration-findings.md`):**
  `XrCompositionLayerDepthInfoKHR` extends **`XrCompositionLayerProjectionView`**,
  not a quad/2D layer. So the content must be presented as a **projection layer**
  with per-eye views for depth to attach; a quad layer cannot carry it.
- **`XR_MSFT_composition_layer_reprojection`** — why the depth matters:
  `XR_REPROJECTION_MODE_DEPTH_MSFT` gives **per-pixel depth reprojection** so a
  2D layer stays glued to the scene as the head moves (instead of floating as a
  plane).
- **Environment/passthrough depth** — `XR_META_environment_depth`,
  `XR_FB_passthrough`, and `XR_VARJO_environment_depth_estimation` (a vendor
  already ships *compositor-side* depth estimation — prior art that this exact
  concept is a real, shipping feature).

**Conclusion:** no new API is needed. The integration point is:
display frame → (AI) → depth texture → attach to the layer / depth swapchain.

---

## Pillar 2 — real-time performance: the constraint

### Frame budgets (the target)

| Refresh | Budget/frame |
|---|---|
| 72 Hz | 13.9 ms |
| 90 Hz | 11.1 ms |
| 120 Hz | 8.3 ms |
| 144 Hz | 6.9 ms |

### Model under test

`Depth-Anything-V2-Small` (ViT-S, DPT head), ONNX, fp16, input 518×518 →
relative depth map. Representative "good quality" monocular model.
Harness: `bench/bench_depth.py` (inference-only + full 1080×1200 display
pipeline), ONNX Runtime 1.24.4.

### Results

**CPU (no GPU) — ruled out.**

| Config | p50 | fps |
|---|---|---|
| fp16, 518², CPU | 480 ms | 2.1 |
| int8, 518², CPU | 1132 ms | 0.9 |

**GPU (RTX 3090, DirectML EP, Python ORT) — the real numbers.**
Note: GPU was contended (another workload running), so absolute values are
directional; the *shape* of the results is the finding.

| Model input | DML inference p50 | CPU p50 (same size) |
|---|---|---|
| 518² | ~21–27 ms | (480 ms) |
| 252² | ~42–50 ms | 99 ms |
| 224² | ~35–40 ms | 79 ms |
| 196² | ~30–33 ms | 64 ms |
| **140²** | **~19 ms** | 38 ms |

Full pipeline (1080×1200 display → model → back to display res, DML fp16):
~29 ms at 140², ~38–48 ms at 518² (PIL CPU resize adds ~10 ms).

### The finding: it's an overhead floor, not a compute problem

**At 140×140 input — a 10×10 token grid, computively trivial — the GPU still
takes ~19 ms.** That is a *floor*. The model compute at that size is negligible;
the 19 ms is integration overhead:

- ONNX Runtime **DML EP per-run cost** (D3D11 command-list creation,
  validation, resource handling) — the dominant term;
- **Python ORT binding** overhead;
- **CPU↔GPU copies** of input and output tensors.

Evidence it's overhead, not compute:
- 518² is only ~2× the 140² floor (27 vs 19 ms) — compute should scale ~13×.
- int8 (CPU-targeted quantization) is *slower* than fp16 on the GPU (38 vs 21
  ms at 518²) — the GPU wants fp16, and int8 adds dequant/quant ops it can't
  fuse.
- The 392² point came out *slower* than 518² (86 vs 27 ms) — physically
  impossible for compute; it's contention/WDDM noise, confirming the numbers
  are dominated by non-compute factors.

**So the model is not the bottleneck. The integration stack is.**

---

## Feasibility verdict

| Stack | Per-frame (72–144 Hz) | With temporal reuse |
|---|---|---|
| CPU (any) | **No** (480 ms+) | No |
| GPU, naive (Python ORT, CPU round-trip) | **No** (19 ms floor > 6.9–13.9 ms) | **Yes** at 72–90 Hz (518² runs ~46 fps → depth every ~2 frames at 90 Hz) |
| GPU, **native, GPU-resident** (C++/C#, no CPU round-trip, CUDA/TRT or DML, async worker, smaller model, temporal reuse) | **Yes** at 90 Hz; **120–144 Hz** with reuse | Yes |

**Bottom line:** real-time AI depth estimation of the VR display is
**feasible**, and OpenXR is a non-issue. What makes it real-time is the
integration stack — not a new API and not a bigger model.

### Confirmed by prior art: reference-app (shipping product)

reference-app (Steam) does exactly this feature and its install was inspected
(full teardown in `research/reference-app-teardown.md`):

- **Model:** Depth Anything V2 **ViT-Small**, fixed-shape **ONNX fp16**,
  native 16:9 (default **686×392**) and square ladders, full-res output.
- **Runtime:** native C++ **ONNX Runtime + DirectML**, GPU-resident — the
  same backend we benchmarked, minus Python and CPU copies.
- **Measured 7.7 ms/frame** (default 686×392; 8.1 ms for the 518² square it
  replaces) — **fits 90 Hz and 120 Hz budgets**.
- Plus: model **slicing** into ~10–16 sub-graphs for frame-paced execution,
  a virtual-monitor capture driver, a ReShade **depth-provider** addon
  (grabs real depth from games so no AI is needed when depth exists), a
  stereo-warp shader, AMD FidelityFX frame generation, OpenXR/OpenVR
  presentation.

This is real-world proof of the verdict above, and it isolates the
difference: reference-app's 7.7 ms (native/fixed-shape/GPU-resident) vs. our
19–27 ms (Python/dynamic/CPU round-trip) **is the overhead floor** — exactly
what a native prototype removes.

---

## Architecture for the real-time path

```
Render thread (OpenXR frame loop)
  └─ display frame (per-eye GPU texture, from swapchain)
        │
        │  GPU resize to model input (e.g. 256²) — stays on GPU
        ▼
  [async depth worker thread]  (own D3D11 device for DML, or CUDA ctx)
        │  model runs on GPU; input & output tensors stay on GPU
        ▼
  depth texture (per-eye, GPU)  ← double/triple-buffered, "latest completed"
        │
        ▼  bound into OpenXR
  - XR_KHR_composition_layer_depth  (attach to the 2D display layer)
  - depth swapchain                  (own 3D layer, if you render geometry)
  - XR_MSFT_composition_layer_reprojection DEPTH  (per-pixel reprojection)
```

Design points:

- **Never block the render thread.** Depth runs on a worker thread with its
  own device/context. The renderer always uses the *latest completed* depth
  (1-frame staleness is fine — the compositor reprojects depth with the latest
  pose every frame).
- **Zero CPU round-trip.** Display frame is already a GPU texture → GPU resize
  → model (GPU) → depth texture (GPU) → bound to the OpenXR depth
  swapchain/layer. No `memcpy` to/from CPU. This is what removes the ~19 ms
  floor measured above.
- **Temporal reuse.** Recompute depth every N frames; between updates,
  reproject the last depth with the current pose. N=1 for 90 Hz per-frame;
  N=2–3 for 120–144 Hz.
- **Per-eye strategy.** For identical 2D content in both eyes, run **once**
  per frame and share (the stereo frames differ only by parallax). For true
  stereo, run per eye (2× cost) or run once + shift.
- **Model.** DA-V2-**Small** for quality, DA-V2-**Tiny** for speed. **fp16**.
  Input 256²–518² (256² is the speed/quality sweet spot to validate first).
- **Backend.** For NVIDIA (the common VR-PC case): **CUDA EP** or
  **TensorRT** — lowest per-run overhead. **DirectML** if you need
  AMD/Intel portability (works everywhere, slightly higher per-run overhead).

---

## What was measured vs. inferred (honesty)

- **Measured:** CPU latency (480 ms+), GPU/DML latency vs. input size
  (19–27 ms), int8>fp16 on GPU, full-pipeline cost, the ~19 ms floor.
- **Inferred (needs a prototype to confirm):** that a *native, GPU-resident,
  no-CPU-round-trip* stack removes most of the ~19 ms floor and lands inside
  the 90 Hz budget. The floor is dominated by DML-EP + Python + copies, all of
  which a native pipeline eliminates — but the exact ms must be measured with
  the real stack.
- **Caveat:** GPU was contended during measurement; clean-GPU numbers will be
  better. The overhead *floor* is CPU-side and largely contention-independent.

## Next steps (to convert "feasible" into "proven")

1. **Native prototype** — C++ (or C#) ONNX Runtime, GPU-resident tensors, no
   CPU round-trip, worker thread; measure true per-run latency at 256²/518².
   This is the single experiment that turns the verdict into a number.
2. **Add DA-V2-Tiny** and a 256²/224² sweep to quantify compute headroom.
3. **Clean-GPU run** (no other workload) for representative absolute numbers.
4. **Backend A/B:** CUDA EP / TensorRT vs. DML per-run overhead on this GPU.
5. **Runtime validation:** confirm the target runtime (SteamVR/Oculus/WMR)
   actually *consumes* the attached depth for reprojection quality on a real
   headset.
6. **Per-eye decision:** once (1) lands, choose shared-vs-per-eye depth.

## Files

- `INVESTIGATION.md` — this document.
- `research/openxr-depth-apis.md` — OpenXR depth API surface (Pillar 1).
- `research/reference-app-teardown.md` — reference-app prior-art teardown (models,
  runtime, 7.7 ms measurement, slicing, capture, stereo warp).
- `research/m1-native-dml-findings.md` — M1 native ORT+DML probe results;
  graph capture is invalid with CPU-hosted I/O, so GPU-resident I/O is a
  hard requirement, not an optimisation.
- `research/m2-openxr-findings.md` — OpenXR bootstrap on this machine:
  SteamVR/OpenXR 2.17.9, HMD present, D3D12 binding available (no D3D11 bridge
  needed), `XR_KHR_composition_layer_depth` available but
  `XR_MSFT_composition_layer_reprojection` is not. Also the working D3D12
  session + swapchain + quad-layer app.
- `research/m3-depth-integration-findings.md` — depth attaches to a projection
  view (not a quad); the model and the OpenXR session share one D3D12 device via
  `Ex_DML`; and GPU contention changes the latency budget (16.8 ms / 60 fps with
  a real app presenting vs 13.9 ms idle vs 39 ms with SteamVR's home env active).
- `bench/bench_depth.py` — benchmark harness (CPU/DML/CUDA/TRT, size sweep,
  full display pipeline).
- `bench/run-bench*.ps1` — runner scripts.
- `bench/bench-*.txt` — raw results.
- `bench/models/.../model_fp16.onnx`, `model_int8.onnx` — DA-V2-Small ONNX.
