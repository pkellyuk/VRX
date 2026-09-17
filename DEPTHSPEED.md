# Depth speed: review brief for Claude Fable

Date: 2026-09-17
Project baseline: `34cde9a` — desktop controls, per-game profiles and reliable capture.

## Goal and current recommendation

VRX turns ordinary flat PC games into a stereoscopic virtual screen in a VR
headset. Priorities are simple desktop setup, game compatibility and normal
keyboard/mouse input. No in-game VR controls or game injection are required.
Helldivers 2 is the current test game; hardware is an RTX 3090 plus an available
RTX 3060. The existing path uses the 3090 for capture, inference and presentation.

The immediate goal is to preserve the cleaner result of **matching each game
frame to its depth estimate**, while getting close to 60 distinct matched game
frames per second with acceptable latency.

My recommendation is a contained ZipDepth feasibility experiment using ONNX and
the existing DirectML backend, followed by integration only if quality and speed
justify it. This is not a recommendation to replace the current working model
blindly. TensorRT is a separate, more substantial backend experiment.

## What we know locally

Current model: Depth Anything V2 Small, fixed 686×392, ONNX Runtime DirectML
1.24.4, FP16 weights with float input/output. RGB input stays on the GPU; the
depth output returns to CPU memory for normalization before being uploaded for
stereo warping.

| Measurement or observation | Result | Limits |
| --- | --- | --- |
| Native GPU-input inference, SteamVR stopped | 13.89 ms p50, about 72 updates/s | Idle-GPU benchmark with a verified changing input |
| Static-image VR test with headset awake | 16.3–16.7 ms, about 60 depth updates/s; about 120 presentations/s | No demanding game competing for GPU resources |
| Recent Helldivers 2 session | Commonly 80–100 ms model time and 10–12 depth updates/s; roughly 40–50 captured frames/s | Workload-dependent session telemetry, not a controlled benchmark |
| User's paired-frame comparison | “Clearly far better and cleaner”, but much slower | Strong evidence for temporal mismatch; does not prove it is the only artifact source |

The default renderer presents the latest colour frame with the latest completed
depth. In paired mode, valid stereo uses the exact colour frame retained with
that depth result. Game-image motion therefore advances at approximately the
depth-update rate, even while headset pose and presentation run faster.

Important measurement distinctions:

- 60 updates/s means a new result every 16.7 ms in steady state. This is a
  throughput target, not automatically 16.7 ms capture-to-display latency.
- We should target inference comfortably below 16.7 ms under game load, leaving
  budget for preprocessing, transfers, normalization and presentation.
- A model cannot supply 60 distinct matched game frames if capture supplies only
  40–50. The game's frame rate and capture delivery must be measured separately.
- The existing log's `age` measures time since depth completion, excluding the
  preceding inference and capture delay. It is not end-to-end latency.
- Overall GPU utilization, including the earlier observation of about 70%, does
  not establish that inference has sufficient compute/bandwidth/scheduling room.

Local evidence: [M1 native inference](research/m1-native-dml-findings.md),
[INVESTIGATION.md](INVESTIGATION.md), and session
`4dac534750824c809e59207f6ee8154c` under `%LOCALAPPDATA%\VRX\sessions`.
Session logs are local and are not committed.

## Why extra foreground passes did not settle this

An experimental, default-on checkbox now identifies persistent nearby depth
regions, runs occasional magnified crop inference, aligns it to the same frame's
global estimate and conservatively blends it. It has age, consistency and cost
gates. It does not perform optical flow or carry refined detail across frames.

The log recorded 32 accepted refinements during roughly two minutes of visible
playback; the user noticed little difference. Acceptance means the blend passed
its checks, not that it visibly improved the avatar. Each refined result is also
replaced by a subsequent ordinary full-scene estimate. Extra inference does not
solve the timing mismatch and can delay later full-scene updates.

Disable foreground refinement during baseline model/backend comparisons. Keep
paired mode and all other visual settings identical across candidates.

## Candidate models and backends

These are externally reported results, not measurements made in VRX. Hardware,
resolution, precision, timing scope and workload differ; do not compare the
figures as a controlled ranking or extrapolate them directly to gameplay.

| Candidate | Published evidence | Why test it / unresolved concern |
| --- | --- | --- |
| ZipDepth | Authors report about 0.8 ms median forward-pass latency on RTX 3090, TensorRT FP16, 384×384, over 200 passes | Particularly relevant hardware and substantial apparent headroom; smaller input than ours, no game contention, no VRX pipeline costs |
| YOLO26 nano/small depth | Official documentation reports about 2.7/3.8 ms inference-only on Tesla T4, TensorRT FP16, 768×768, batch 1 | Another fast-model option; game-domain boundary quality and temporal stability remain untested |
| Current Depth Anything family through TensorRT | A C++ implementation reports 3 ms for Depth Anything Small at 518×518 on RTX 4090, including pre/postprocessing | May preserve familiar quality; not a verified V2/3090/VRX result and needs a different backend |

Sources:

- [ZipDepth author project and benchmark](https://zipdepth.github.io/)
- [ZipDepth official code, checkpoints and export instructions](https://github.com/fabiotosi92/ZipDepth)
- [Ultralytics depth models and benchmark conditions](https://docs.ultralytics.com/tasks/depth)
- [Depth Anything TensorRT implementation and benchmark](https://github.com/spacewalk01/depth-anything-tensorrt)

ZipDepth's official repository provides GPU and NPU-oriented variants and ONNX
export instructions. That supports trying our existing backend, but does not
prove that its exported operators run efficiently on DirectML. Validate the
actual graph, preprocessing and output conventions. Do not assume its published
TensorRT speed survives a DirectML conversion.

Our earlier model investigation also identified an opportunity to test a more
efficiently fused export of the current model. That remains a useful control:
poor export/backend execution should not be mistaken for a fundamental model
speed limit. Previously observed suspiciously fast graph-capture/output-binding
results failed live-input checks and must not be reused as evidence.

## How much code would ZipDepth require?

**Contained but nontrivial.** The capture architecture, source-frame ownership,
desktop controls, keyboard handling, screen placement and OpenXR presentation
can remain. It is not a drop-in filename replacement.

Relevant implementation points:

| Area | Current assumption | Likely adaptation |
| --- | --- | --- |
| `xr_common.h` | Shared `W=686`, `H=392` constants | Separate model input/output dimensions from renderer depth-grid dimensions |
| `xrapp5.cpp`: `InitModel` | Hard-coded model path, NCHW shape and GPU input allocation | Select a model and validate its tensor names, types and dimensions |
| `SubmitPrep`, `PrepConstants`, `kPrepHlsl` | Current model's resize and ImageNet normalization | Implement and verify the selected model's actual preprocessing |
| `RunModelRaw` | `pixel_values` / `predicted_depth`, float output of `W*H` elements | Validate output metadata and adapt shape/type/name handling |
| Depth normalization and upload | Relative near/far mapping, buffers and dispatches tied to the fixed grid | Confirm output direction/scale and deliberately resample or resize buffers |
| Foreground refinement and self-tests | Same model/grid assumptions | Initially disable refinement; extend tests before re-enabling it |
| Desktop profiles | No model selector yet | Add a small per-game model choice after feasibility is established |

A low-disruption prototype could retain the existing renderer depth grid and
resample ZipDepth output into it. This keeps the experiment contained, but cannot
recover detail absent from a lower-resolution model and may soften boundaries.
Changing the global constants alone would be too crude because they are shared
by preprocessing, buffers, shaders, CPU reference code and tests.

For ONNX/DirectML, we can reuse the existing device/queue integration. For
TensorRT, we must additionally design CUDA/D3D resource sharing, synchronization,
engine creation/caching and dependency packaging. It would be NVIDIA-specific;
the existing DirectML path should remain as a compatibility fallback.

No ZipDepth model has yet been downloaded, exported, benchmarked or integrated
in this project. Implementation effort and output quality remain estimates.

## Proposed experiment

1. Establish a repeatable baseline with current depth, paired mode on, foreground
   passes off, fixed game settings and repeatable character/camera movement.
   Record game FPS separately from capture and headset presentation rates.
2. Export a fixed-shape ZipDepth ONNX model in an isolated benchmark environment.
   Validate it against the author's inference path on the same inputs. Check
   shape, normalization, depth direction, finite values and changing-frame output.
3. Benchmark at its documented baseline size and an aspect-preserving game input
   size supported by the export. Include preprocessing, completion synchronization
   and output handling. Report warm-up separately and p50/p95/p99 latency.
4. Repeat with the game and VR running. Check for CPU fallback and bottlenecks;
   an idle benchmark alone is insufficient. Compare the same boundary-heavy
   frames: avatar, weapon, cape, thin geometry, explosions and moving camera.
5. Integrate a selectable model only if quality is acceptable and measured speed
   is useful. Verify source pairing, resize, stale-depth fallback and switching.
6. If quality succeeds but DirectML speed does not, benchmark TensorRT or a better
   export before committing to a new production backend.

Success means cleaner moving silhouettes at an acceptable sustained rate and
latency, with enough game/capture frames to feed it. A high average FPS with
latency spikes, flickering depth or degraded foreground edges is not success.
Measure actual capture-to-depth-completion and capture-to-submission times;
physical display latency requires additional measurement.

## Other options worth keeping open

- **RTX 3060 for inference:** worth reconsidering because the measured slowdown
  under game load gives a concrete reason. It is a slower GPU but may be less
  contended. Transfers, synchronization and return of depth to the rendering GPU
  must be included; do not assume a speedup or direct cross-adapter texture reuse.
- **Motion-aligned depth:** if a sufficiently fast model loses too much quality,
  track depth between slower high-quality estimates. Optical flow has its own
  cost and failure cases around disocclusion, effects and thin objects. It needs
  explicit invalid-region handling rather than blindly warping old depth.
- **Reserve GPU capacity:** a controlled game FPS cap or graphics-setting change
  could reveal how much slowdown is contention. Treat this as an experiment,
  not a permanent requirement or an automatic user-settings change.

## Questions for Claude Fable

Please assess this proposal against the code and challenge its assumptions:

1. Is ONNX/DirectML the right first ZipDepth test, or do known operator/export
   constraints justify going directly to a standalone TensorRT benchmark?
2. Are the quoted model benchmarks comparable enough to select candidates, and
   are any timing or quality claims misleading or missing crucial conditions?
3. What is the smallest sound separation of model dimensions, output semantics
   and renderer depth-grid dimensions in this codebase?
4. Would keeping the current depth grid for an initial adapter unduly undermine
   the boundary-quality comparison?
5. How should we distinguish model compute cost, GPU contention, queue stalls,
   capture limits and output synchronization in the measurements?
6. Is a faster lightweight model, an optimized current-model backend, dedicated
   3060 inference, or motion-aligned reuse the most promising first investment?
7. What tests or failure modes are missing, particularly for moving silhouettes,
   temporal flicker, source pairing and latency under real game load?

Requested feedback: a ranked recommendation, corrections to this brief, and a
small first experiment with measurable acceptance criteria. This document is a
proposal for review, not evidence that ZipDepth already works in VRX.

---

# Feedback from Claude Fable — INCOMPLETE (review interrupted)

Date: 2026-09-17. **Status: partial.** A multi-agent review (web verification of
every external claim, GPU-scheduling research, full session-log analysis, a local
DirectML scaling benchmark, second-GPU and motion-aligned-depth studies) was started
and stopped early to save usage; **no investigator finished**. What follows is only
what I verified myself in the code and logs, plus reasoning that is labelled as
unverified. Nothing about ZipDepth, YOLO26 or the TensorRT repo has been checked
against its source yet.

## Verified locally (code and session log)

1. **"Model ms" cannot separate model compute from contention.** In
   `ComputeAndPublish` (`xrapp5.cpp` ~1607–1611) it is CPU wall-clock time from
   `SubmitPrep` to `RunModelRaw` returning. That spans: the ml queue's GPU-side wait
   on the capture fence, the prep dispatch, every ORT/DirectML submission, all time
   those submissions sit behind the game's GPU work, and the CPU readback of the
   output. The brief's 80–100 ms is therefore an upper bound on "inference", not a
   measurement of it.
2. **The slowdown pattern looks like scheduling contention, not model cost.** Session
   `4dac5347…`: model 82–129 ms, 10–11 depth updates/s, capture ~39 fps, while VRX's
   own render loop holds 118–119 fps at 0.2 ms CPU/frame. The same model measured
   13.9 ms idle and 16.3–16.7 ms with VR presenting. A 5–8x inflation that hits the
   long multi-dispatch ml workload but not the tiny per-frame gfx workload is what
   time-slicing behind another process's long frames would produce. (Inference, not
   proof: no GPU timestamps exist yet.)
3. **VRX asks for no GPU priority at all.** Both D3D12 queues are created from a
   zero-initialised `D3D12_COMMAND_QUEUE_DESC` (`xrapp5.cpp` ~632), i.e.
   `PRIORITY_NORMAL`. There is no process-level GPU priority call anywhere in the
   file (grep for `Priority`, `D3DKMT`, `SetGPUThreadPriority`: no hits).
4. **The game is captured at 3844x2207** (log: `InitCaptureItem: exit ok, 3844x2207`,
   "presented at 1920x1102, depth at 686x392"). VRX never shows more than 1920 px
   across and the model sees 686 px. Rendering the game at 4K is GPU load spent on
   pixels the headset path discards — and that load is exactly what starves
   inference.
5. **The worker idles in 2 ms sleeps** when no new frame exists (`Sleep(2)`, ~1672);
   the capture loop polls with `Sleep(1)` (~854). Minor next to 90 ms, but it is
   added latency that becomes visible once inference is fast. Windows timer
   granularity can make these far longer than requested unless the timer
   resolution is raised.
6. **Foreground refinement runs a second full-cost inference on the same thread**
   (`ComputeAndPublish` ~1633–1645) before the next full-frame pass can start. The
   model is fixed-shape, so a crop pass costs the same as a full pass. Under
   contention that directly lowers the full-scene update rate, which agrees with the
   brief's own conclusion to disable it for comparisons.
7. **Paired-mode latency, estimated from the logged figures** (not measured):
   ~25 ms capture interval + ~95 ms model + ~48 ms mean slot age ≈ 150–170 ms from
   game present to submission, before compositor latency. That is far outside what
   mouse-aimed play tolerates, independent of smoothness.

## Corrections and cautions for the brief

- **The target is mis-framed as a model-speed problem.** The evidence above says the
  first question is "why does a 14 ms model take 90 ms here?", not "which model is
  faster?". A model that is 10x lighter still has to get its submissions scheduled
  behind the same game frames; it would shrink the GPU time needed per pass, but the
  queueing delay per submission may dominate. A lighter model helps most *after* the
  contention is understood.
- **Published TensorRT figures do not transfer to DirectML.** The earlier Python
  benchmark in `INVESTIGATION.md` history showed ~19 ms at 140x140 input — a
  near-constant per-run floor on the DirectML path — and M1 measured 518² and 686x392
  within 2% of each other. ORT+DirectML here looks at least partly overhead-bound
  (hundreds of small dispatches per run). ZipDepth's 0.8 ms is a TensorRT number; on
  this stack I would expect milliseconds, not sub-millisecond, until measured. The
  interrupted benchmark (time vs patch count: 686x392, 518x294, 392x224, …, fit
  `overhead + k·patches`) is cheap and should be run before any ZipDepth work: it
  tells you what *any* lighter model can reach on DirectML.
- **Licensing (from memory, not re-verified today):** Ultralytics code and weights
  are AGPL-3.0, which is incompatible with shipping or auto-downloading them from an
  MIT-licensed project. Depth Anything V2 *Small* is Apache-2.0; Base/Large are
  CC-BY-NC. ZipDepth's code and weight licences are **unchecked** and must be
  confirmed before any integration effort.
- **Unverified lead from the interrupted run:** an issue (#12) on
  `spacewalk01/depth-anything-tensorrt` reportedly has an RTX 3090 user measuring
  ~35 ms, versus the README's 3 ms on a 4090. If accurate, the brief's TensorRT row is
  considerably less comparable than it looks. Check before relying on either number.
- **"60 matched frames/s" is not reachable by pairing alone** while capture delivers
  ~40 fps; the brief says this, but it should be the headline, not a bullet. Either
  the game must present faster (less load) or the goal becomes "matched at the
  capture rate".

## Provisional answers to the seven questions

1. **ONNX/DirectML first or straight to TensorRT?** Neither first. Run the DirectML
   scaling benchmark on the *existing* model at smaller fixed shapes (one afternoon,
   no new model, no new backend). If DirectML shows a multi-millisecond floor, a
   standalone TensorRT benchmark is the right ZipDepth test and a DirectML port is
   wasted effort; if time scales with patches, DirectML is fine for a first test.
2. **Are the quoted benchmarks comparable?** No — different GPUs (3090 / T4 / 4090),
   resolutions (384² / 768² / 518²), timing scope (forward-only vs incl. pre/post),
   and all idle-GPU. They are adequate for *shortlisting* only. None says anything
   about behaviour beside a game, which is the actual failure here.
3. **Smallest separation of dimensions.** (From my knowledge of the code; the
   detailed file:line audit did not complete.) Introduce a `ModelSpec`
   {input W/H, input/output tensor names, normalisation mean/std, output W/H,
   output semantics: inverse-depth vs depth, larger-is-nearer flag} owned by
   `InitModel`/`SubmitPrep`/`RunModelRaw`; keep `W`,`H` as the *renderer depth grid*
   used by `nearBuf`, `DepthSlot`, `WarpConstants.dw/dh`, `NearAt`, dilation,
   `MakeTruth`/self-tests. Add one CPU resample (model output → grid) right after
   readback, where the data already is; convert semantics there too. The warp shader
   and its CPU reference then need no change, so the existing self-tests stay valid.
4. **Does keeping the current grid bias the comparison?** Somewhat, in a known
   direction: a lower-resolution model is upsampled (softer edges), then dilation of
   2 grid px and bilinear `NearAt` are applied equally to both candidates. Edges can
   only look *worse* for the smaller model than they would natively, so a pass is
   trustworthy and a marginal fail is not. Acceptable for a feasibility test if that
   asymmetry is written down; compare on dumped eye images (`--dump`), not by eye in
   the headset alone.
5. **Separating compute, contention, stalls, capture limits, sync.** Add D3D12
   timestamp queries on the ml queue around the prep dispatch and immediately after
   ORT's work (a marker list submitted after `Run`) → *GPU busy span*; keep the CPU
   wall clock → *total*; the difference is queue wait. Log per pass: source-fence
   wait, prep GPU time, model GPU span, readback+normalise CPU time, slot age at
   first use, and capture-timestamp → depth-complete → `xrEndFrame`. Use PresentMon
   for the game's true present rate (capture fps is not game fps). One GPUView/ETW
   trace of 5 seconds would show directly whether ml packets sit queued behind the
   game's.
6. **Most promising first investment (ranked, provisional):**
   1. **Reduce and measure contention** — nearly free, reversible, and it attacks
      the measured cause: (a) run the game at 1920x1080/2560x1440 and/or with a
      frame cap, (b) raise VRX's GPU priority, (c) add the timestamps from Q5.
   2. **Inference on the idle RTX 3060.** Input is ~3 MB and output ~1 MB per pass;
      the output already travels via the CPU and compositor depth submission is off,
      so the "model and session must share a device" constraint from M3 no longer
      binds. An uncontended slower GPU plausibly beats a contended fast one (my
      guess: 25–35 ms per pass, i.e. 30–40 updates/s, unmeasured).
   3. **Cheaper passes on the current stack** — smaller fixed shape (518x294 is 777
      patches vs 1372), fused export — guided by the scaling benchmark.
   4. **Lighter model / TensorRT backend** — real engineering (CUDA–D3D12 interop,
      engine caching, NVIDIA-only), justified only once 1–3 show the remaining gap
      is genuinely model compute.
   5. **Motion-aligned depth reuse** — the only option that fixes mismatch when
      depth is inherently slower than colour, but the hardest to make artifact-free
      (disocclusion, effects, thin geometry). Keep for after the rate is as high as
      cheap measures allow.
7. **Missing tests / failure modes.** A headset-free temporal test: replay a recorded
   capture sequence with a known moving silhouette through depth-lag simulation
   (depth delayed by N frames) and score edge misalignment between colour and depth
   per frame — this makes "cleaner silhouettes" a number and lets paired / unpaired /
   flow-warped be compared offline. A flicker metric: per-pixel depth variance on a
   static capture. Pairing identity under artificial worker delay (the source-ring
   test already has the machinery). Latency percentiles under a synthetic GPU load
   generator, so contention can be reproduced without the game. Also: behaviour when
   the game's present rate drops below the depth rate, and at scene cuts with range
   smoothing active.

## Proposed first experiment (small, one session, no new model)

**Hypothesis:** most of the 80–130 ms is GPU scheduling contention with the game.

Setup: Helldivers 2, same mission area and camera routine, foreground passes off,
paired off, 90 s per condition, read the 2-second `render … | capture … | depth …`
lines from the engine log.

| Condition | Change |
| --- | --- |
| A (baseline) | current settings, game at 4K |
| B | game at 1920x1080 (or 2560x1440), everything else identical |
| C | B + in-game frame cap at 60 |
| D | A + VRX ml queue created with `D3D12_COMMAND_QUEUE_PRIORITY_HIGH` |
| E | best of B/C + D |

Condition D is a one-line change to the queue descriptor. (From memory, unverified
today: `GLOBAL_REALTIME` needs an elevated privilege and fails without it, so HIGH is
the safe first step; OBS solves the same "capture starves beside a game" problem by
raising its process GPU scheduling priority, which is why it recommends running as
administrator. Both points need checking against Microsoft's and OBS's sources.)

**Acceptance:** the hypothesis is supported if any condition brings median model time
under 33 ms (at least 30 depth updates/s) with capture fps not reduced and no new
stale-depth fallbacks. If B–E all leave model time above ~60 ms, contention with this
game cannot be bought off cheaply on one GPU, and the RTX 3060 experiment becomes the
next step ahead of any model change. Either way, add the Q5 timestamps first if time
allows — they turn every later experiment from inference into measurement.

## Not done (would complete this review)

Verification of the ZipDepth, YOLO26 and TensorRT claims and licences against their
sources; GPU-scheduling semantics and the OBS precedent from primary sources; analysis
of all seven session logs; the DirectML scaling benchmark; cross-adapter transfer
design for the 3060; NVIDIA Optical Flow feasibility from D3D12; the file:line audit
for the dimension split.
