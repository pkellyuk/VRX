# M1 findings — native ONNX Runtime + DirectML, GPU-resident depth

Date: 2026-09-17
Probes: `bench/native/ortprobe` (C#/.NET 10, managed API) and
`bench/native/dmlgpu` (C++ ORT C API + D3D12 interop, `build.bat`)
GPU: RTX 3090. All C++ numbers are fence-synchronised, and every number is
accompanied by a live-input check (below).

## The live-input check (why it exists)

A latency figure is worthless if the pipeline is not consuming new input. Three
separate times in this work a fast number turned out to be measuring nothing:

| Symptom | Cause |
|---|---|
| graph capture: 0.59 ms | captured graph never re-read the CPU input |
| IOBinding: 2.90 ms | device-allocated output; run returned before the GPU finished |
| bound D3D12 output: stale | output buffer bound but never written by ORT |

The check is therefore mandatory. It also has to be *strong*: filling the input
with a uniform 0.25 vs 0.75 is a weak test for a depth model, because a flat
field produces a flat depth map whatever its brightness — at 686x392 that pair
differed by only 0.006% of the output sum, which a stale buffer could easily
mimic. The probe now fills a horizontal ramp and its inverse, which separates the
outputs by maxDiff 1.63 across 99.9% of elements.

## Measured (518x518 unless noted)

| Configuration | p50 | Live | Trustworthy |
|---|---|---|---|
| CPU EP (native) | **515 ms** | – | yes (matches Python's 480 ms) |
| DirectML, dynamic shape, CPU input | **21.9 ms** | – | yes |
| DirectML, dynamic, 252x252 | **44.8 ms** | – | yes (slower than 518! see below) |
| DirectML, fixed shape, CPU input | 14.7–34.8 ms | – | yes, but unstable |
| DirectML, fixed, **D3D12 GPU input** (518x518, 1369 patches) | **13.64 ms** (73 fps) | **YES** | **yes** |
| DirectML, fixed, D3D12 GPU input (**392x686 — reference-app's default**, 1372 patches) | **13.89 ms** (72 fps) | **YES** | **yes** |
| ...same, with `ep.dml.enable_cpu_sync_spinning=1` | 13.87 ms | YES | yes — no measurable gain |
| DirectML, fixed, D3D12 GPU input, no CPU readback | 14.62 ms | YES | yes (readback is free) |
| DirectML + `ep.dml.enable_graph_capture=1` | 0.59 ms | **NO** | **no — invalid** |
| DirectML + IOBinding, device output | 2.90 ms | **NO** | **no — enqueue only** |

Best verified result: **13.6–13.9 ms / ~72 fps**, hit by feeding the model a D3D12
resource on the DML EP's own device, wrapped with
`OrtDmlApi::CreateGPUAllocationFromD3DResource`.

**Geometry is not the missing factor.** 392x686 (1372 patches) and 518x518
(1369 patches) land within 2% of each other — 13.89 vs 13.64 ms — as they should
for equal patch counts. So the 1.8x to reference-app is not about square vs 16:9.

At 72 fps the depth path suits a 72 Hz headset per-frame, and 90 Hz with the
depth on a worker thread plus temporal reuse (depth refreshes roughly 4 frames in
5 at 90 Hz).

## What worked, in order (each step was forced by a failure of the previous one)

1. **The DML EP is not in the base ORT package.** `Microsoft.ML.OnnxRuntime` has
   no DirectML: `AppendExecutionProvider_DML` throws
   `EntryPointNotFoundException: ...OrtSessionOptionsAppendExecutionProvider_DML`.
   The DML build is `Microsoft.ML.OnnxRuntime.DirectML` (and it lags: 1.24.4 vs
   1.30.0). That package also ships `dml_provider_factory.h`, which is where
   `OrtDmlApi` is actually declared — it is absent from the base package and
   from the ORT repo's dml directory listing.
2. **The DML entry points live in a struct, not as exports.**
   `OrtApi::GetExecutionProviderApi("DML", ORT_API_VERSION, &api)` returns
   `OrtDmlApi*`. Members, in order: `SessionOptionsAppendExecutionProvider_DML`,
   `..._DML1`, `CreateGPUAllocationFromD3DResource`, `FreeGPUAllocation`,
   `GetD3D12ResourceFromAllocation`, `..._DML2`, `GetDMLDevice`,
   `GetDMLCommandQueue`.
3. **`GetDMLCommandQueue` returns a borrowed reference.** Calling `Release()` on
   it frees the queue the EP still uses; `CreateSession` then dies with an
   access violation during graph compilation. (Cost: one debugging cycle.)
4. **A D3D12 resource for DML must allow UAV access.** With
   `D3D12_RESOURCE_FLAG_NONE`, every `Run` failed with
   `DML ... DmlGraphFusionHelper.cpp(1078) ... 80070057 E_INVALIDARG`. Adding
   `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` (plus a CUSTOM/L0/WRITE_BACK
   heap so the CPU can rewrite it each frame) fixed it. Heap type alone was not
   the issue — UPLOAD heap failed identically.
5. **`CreateMemoryInfo` is deprecated** in 1.24: it fails with "Specified device
   is not supported. Try CreateMemoryInfo_V2". Use
   `SessionGetMemoryInfoForInputs/Outputs` where possible.

## The two things that did not work, and why they matter

**Graph capture is unusable with CPU-hosted I/O.** `ep.dml.enable_graph_capture=1`
gives 0.59 ms with the correct checksum — but the output is *identical for any
input*. Capture binds its I/O at capture time; a CPU `OrtValue` is never
re-read. An early version of the probe reported this as a success, which is why
the live-input check now exists.

**Device-allocated outputs cannot be set up at all.** `BindOutputToDevice` and
even a CPU-bound output with `SynchronizeBoundOutputs` both fail with:
```
BatchOrCopyMLValue allocator != nullptr was false. Failed to find allocator for
device Device:[DeviceType:1 MemoryType:0 VendorId:0 DeviceId:0 Alignment:0]
```
ORT cannot resolve an allocator for the DML device, so a run with IOBinding (and
hence capture) cannot be formed. Binding our own pre-allocated D3D12 buffer as
the output is accepted but never written.

Consequence: ORT's graph-capture route is blocked from this API surface. Either
reference-app does not use it, or it is reachable only through a code path we have not
found (an `OrtEpDevice`-based session?).

## Model forensics — the most actionable finding

Both models were compared directly against reference-app's shipped files.

- Our `model_fixed_518.onnx` and reference-app's `depth_anything_v2_vits_fp16_252.onnx`
  are **the same export**: 838 nodes, identical op histogram
  (`Add` 150, `Mul` 90, `Gather` 73, `MatMul` 72, `ReduceMean` 50...), identical
  cast counts, identical weight dtypes. Not merely similar — the same graph.
- reference-app's **default** (686x392) model is a *different* export:
  **565 nodes**, built from fused ops (`Conv` 31, `LayerNormalization` 28,
  `Relu` 16) rather than the vits export's decomposed
  `Gather`/`ReduceMean`/`Concat`/`Unsqueeze` chains, with **16 int64
  initializers instead of 178** and 4 casts instead of 15.
- All reference-app models are **FLOAT I/O** with fp16 weights — so the "fp16" in the
  filename refers to weights only, and the fp32-vs-fp16-I/O hypothesis is dead.

So the decomposed vits export — the one both we and reference-app ship — is the slow
variant; reference-app's default is a better-fused, 16:9 export with a third fewer
nodes and almost no shape churn. Adopting an equivalently fused graph is the
clearest concrete optimisation available.

## Where the remaining 1.8x to reference-app's 7.7 ms could be

reference-app reports 7.7 ms for 686x392 (1372 patches) and 8.1 ms for the 518 square
(1369 patches) — i.e. their *vits* 518 also runs at ~8 ms, while ours runs at
14.6 ms with the same model, same ORT, same DML EP. Unexplained candidates, in
order of how testable they are:

1. **Their hardware** is unknown; the numbers in `settings.json` may simply come
   from a faster GPU than our 3090 (or one where DML is the native path).
2. **Their sliced execution** (10–16 sub-graphs, `s0..s9`, linear encoder→decoder
   DAG). Possibly required for DML graph fusion to succeed per slice, or for
   frame-paced execution. Not yet reproduced.
3. **Graph capture**, which we could not form (above).
4. **A fused-op model variant** — see forensics; cheap to adopt and worth its own
   measurement.

Given the same model and runtime, (1) is as likely as (2)/(3), and we cannot
distinguish them without knowing their hardware.

## Next steps

1. **Adopt a fused-op model export** (Conv/LayerNorm, ~565 nodes like reference-app's
   default) and measure it — the one change that plausibly closes the gap and has
   not yet been tried. The decomposed vits export is the slow variant.
2. **Sliced execution** — reproduce reference-app's 10–16 sub-graph split and see whether
   it enables DML graph fusion per slice or otherwise cuts per-run overhead.
3. Investigate the `OrtEpDevice` session path for a device allocator, the only
   remaining route to ORT graph capture.
4. Proceed to **M2 (OpenXR layer)**: at ~72 fps the depth path is already fast
   enough to be useful, so headset work should not wait on the last 1.8x.

Done since the first draft: geometry-for-geometry comparison (392x686 vs
518x518 — within 2%, so shape is not the factor) and `cpu_sync_spinning`
(no measurable gain).
