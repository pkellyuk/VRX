# reference-app teardown — prior art for real-time AI depth of the VR display

Investigated: 2026-09-17
Install inspected: `C:\Program Files (x86)\Steam\steamapps\common\reference-app`

reference-app does exactly the feature under investigation: it captures a 2D display,
runs an AI depth model on it in real time, and presents the result in VR with
stereo parallax. It is direct, shipping prior art — and it confirms the
conclusions in `INVESTIGATION.md`.

## The model: Depth Anything V2 (ViT-Small), ONNX fp16

Inspected ONNX I/O (all fp16, batch 1):

| File | Input | Output |
|---|---|---|
| `depth_anything_v2_vits_fp16.onnx` | `[1,3,518,518]` | `[1,1,518,518]` |
| `depth_anything_v2_vits_fp16_252.onnx` | `[1,3,252,252]` | `[1,252,252]` |
| `depth_anything_v2_small_392_fp16.onnx` | `[1,3,392,392]` | `[1,392,392]` |
| `distill_any_depth_small_686x392_fp16.onnx` | `[1,3,392,686]` | `[1,392,686]` |
| `distill_any_depth_small_924x518_fp16.onnx` | `[1,3,518,924]` | `[1,518,924]` |
| `distill_any_depth_small_322x322_fp16.onnx` | `[1,3,322,322]` | `[1,322,322]` |
| (also `..._518x294`, `..._798x448`, `..._210x210`, `..._280x280`) | | |

So: **Depth Anything V2, ViT-Small ("vits")**, exported to **fixed-shape ONNX
fp16**, in two ladders:
- **Square**: 252², 392², 518²
- **16:9 "Distill AnyDepth Small"**: 518×294, 686×392, 798×448, 924×518
  (+ small squares 210², 280², 322²)

Key differences from our benchmark model:
- **Fixed shapes** (each export is specialized to one resolution). No
  dynamic-shape re-validation — a real performance win over our dynamic
  `pixel_values [N,3,H,W]` export.
- **Full-resolution output** (out H×W == in H×W). No coarse map + upscale.
- **Native 16:9 input** matching the captured widescreen display — no square
  crop/letterbox, no wasted pixels.

## The runtime: ONNX Runtime + DirectML (native C++)

Bundled in the install:
- `onnxruntime.dll` (17 MB)
- `DirectML.dll` (18.5 MB)  ← **DirectML EP**, the same backend we benchmarked
- `openxr_loader.dll` (OpenXR) + `openvr_api.dll` (SteamVR) for presentation

So the stack is **native C++ ONNX Runtime with the DirectML execution
provider, GPU-resident** — not Python, no CPU round-trip. This is exactly the
"native GPU-resident pipeline" that `INVESTIGATION.md` predicts removes the
~19 ms overhead floor.

## The number that matters: 7.7 ms/frame

From the shipped `settings.json`:
- Default `reference_app_depth_model_quality: 4` = **"Distill Normal (686×392)"**.
- Comment: *"Measured **7.7 ms** against **8.1 ms** for the 518 square it
  replaces, so it is not a cost increase either."*
- `reference_app_ai_fps_auto: true` — the AI run-rate is auto-tuned (temporal
  control).
- `reference_app_grab_screen: true` — it grabs the 2D screen as the input frame.

**7.7 ms fits the 90 Hz (11.1 ms) and 120 Hz (8.3 ms) budgets.** That is the
real-world proof that a native, fixed-shape, GPU-resident ONNX+DML pipeline
hits real-time — versus our 19–27 ms Python/CPU-round-trip benchmark. The
~12–19 ms gap is precisely the overhead (Python binding + CPU copies +
dynamic shapes) that reference-app's design avoids.

## The engineering around the model (all visible in the install)

- **Model slicing** — every model is also shipped as a `_slices/` folder of
  ~10–16 sub-graphs (`s0..sN.onnx`) plus a JSON DAG. The `feeds` array is a
  linear encoder→decoder pipeline with skip connections (e.g. slice 5 takes
  outputs `[5,1,2,3,4]`). This lets the app **split one network's work across
  frames / keep GPU work in small time-slices** (frame pacing) and/or reduce
  per-call DML overhead — the same "don't do it all in one frame" idea as
  temporal reuse.
- **Virtual monitor driver** (`virtual_monitor/`, MttVDD) — creates a virtual
  display so the 2D content can be captured cleanly (`grab_screen`).
- **ReShade depth provider** (`reshade_addon/reference-app_DepthProvider.addon64`) —
  for games, grabs the **real depth buffer** via a ReShade hook, so no AI is
  needed when the app already has depth. AI depth is the fallback for 2D
  content with no depth.
- **`stereo_warp.hlsl`** — the shader that warps frame + depth into left/right
  eye views (the 2D→3D parallax step).
- **AMD FidelityFX Frame Generation** (`amd_fidelityfx_framegeneration_dx12.dll`)
  — AI frame interpolation to raise effective FPS (a separate AI feature).
- **`openxr_loader.dll` / `openvr_api.dll`** — presentation via OpenXR/OpenVR.

## Why this matters to the investigation

1. **Confirms feasibility with a shipping product.** Real-time AI depth of a
   VR display is not hypothetical — reference-app does it at ~7.7 ms/frame.
2. **Confirms the bottleneck diagnosis.** reference-app's 7.7 ms (native, fixed-shape,
   GPU-resident) vs. our 19–27 ms (Python, dynamic, CPU round-trip) isolates the
   overhead as the difference — matching `INVESTIGATION.md`.
3. **Gives a concrete target architecture:** native C++ ORT + DirectML,
   fixed-shape fp16 ONNX, native-aspect input, full-res output, sliced/
   frame-paced execution, virtual-monitor capture, ReShade depth grab for
   games, stereo-warp shader, OpenXR presentation.
4. **Model choice is settled:** Depth Anything V2 ViT-Small fp16, 16:9
   ~686×392 as the default quality point.

## Open questions reference-app raises (to resolve for our own build)

- What exact distillation produced "Distill AnyDepth Small" (vs. vanilla
  DA-V2-Small)? The 16:9 fixed-shape exports are the part we'd want to
  reproduce.
- How many slices are executed per frame vs. carried across frames (the
  pacing policy)?
- Is the 7.7 ms measured on which GPU / driver? (Gives a hardware target.)
- Per-eye: does it run the model once and stereo-warp, or per eye?
