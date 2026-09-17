# VRX — Make flat games 3D in VR

Updated: 2026-09-17

## Goal and initial scope

Enable a player to run an ordinary flat PC game, view it in stereo 3D in a VR
headset, and keep playing with the game's normal keyboard and mouse controls.
The priorities are **simplicity, compatibility, and responsive gameplay**.

The initial product should be a seated Windows application: choose a running
game or monitor, select **Play in VR**, and return keyboard/mouse focus to the
game. Sensible defaults should make most sessions need no further adjustment.
Touch/motion-controller controls, hand tracking, and VR interaction conversion
are outside the initial scope. Head tracking positions the virtual screen; it
should not drive the game's camera or replace mouse aiming by default.

The intended experience is a stereoscopic game on a comfortable virtual screen.
AI depth estimates relative scene structure from one image; it does not recover
hidden scenery, reliable metric depth, or the game's original geometry. This
should not be presented as a conversion into a native six-degree-of-freedom VR
game. Head movement around a virtual screen and movement inside the game's
world are separate features.

## Current state

**The core visual pipeline works as a prototype. A reliable flat-game player is
not yet demonstrated.** The current application is
[`bench/native/openxr/xrapp5.cpp`](bench/native/openxr/xrapp5.cpp), with shared
helpers in [`xr_common.h`](bench/native/openxr/xr_common.h). Earlier executables
are development milestones, not separate product entry points.

Evidence comes from the checked-in research notes and the current working tree,
including the recent mirror-fill changes. The code review compiled and linked
xrapp5 successfully; it did not repeat the hardware measurements below.

| Capability | State and practical limit |
|---|---|
| Desktop and window capture | Implemented using Windows.Graphics.Capture; primary monitor, monitor index, or window-title selection. Live desktop video has been seen in stereo on PS VR2 through SteamVR. |
| AI depth | Depth Anything V2 Small, fixed 686×392 input, ONNX Runtime DirectML 1.24.4. The model has fp16 weights with float input/output. |
| Capture → model input | GPU-resident capture texture, resizing/filtering, and normalization. No CPU image readback on this path. |
| Depth output | Approximately 1 MB is read back to the CPU, normalized and dilated, then uploaded. The complete pipeline is **not** zero-copy. |
| Stereo rendering | Per-eye forward warp, foreground occlusion handling, and GPU hole filling. Colour output preserves source aspect and is capped at 1920 pixels wide; depth remains 686×392. |
| Frame pacing | Separate capture, inference, and render work; latest colour plus latest completed depth by default. `--paired` is an experimental comparison mode, not yet safe against source-ring reuse. |
| Depth stability | Percentile range normalization with time smoothing and scene-cut detection. Per-pixel temporal stabilization is not implemented. |
| Edge quality | Mirror fill and horizontal foreground-depth dilation implemented; current dilation default is 2 depth pixels. Eye-dump and CPU/GPU comparison results are documented; headset revalidation remains open. |
| Runtime depth submission | Off by default. The tested SteamVR setup showed no visible use of submitted depth; the stereo warp supplies the 3D effect. Other runtimes remain unvalidated. |
| User experience | Command-line prototype with a default 75-second run, diagnostic switches, and machine-specific paths. No finished launcher, persistent game profiles, or keyboard shortcut interface. |
| Game compatibility | No systematic game, graphics-API, fullscreen, anti-cheat, GPU-vendor, or headset compatibility matrix yet. Desktop video success does not establish broad game support. |

### What the performance evidence actually establishes

| Recorded experiment | Result | Interpretation |
|---|---|---|
| Early Python/CPU, 518² fp16 model | About 480 ms per inference | Unsuitable for the intended live depth path on the tested machine. |
| Early Python/DirectML | About 21–27 ms at 518²; about 19 ms at 140² | Historical baseline under contention; does not isolate Python overhead, copies, scheduling, or model cost. |
| Native DirectML, GPU input, fixed 686×392, SteamVR stopped | 13.89 ms p50 on RTX 3090 | Verified live input; an idle-GPU inference result, not a gameplay budget. |
| Native model with VR application presenting | 16.76 ms p50 | Rendering and inference compete for GPU time. A demanding game adds further load. |
| xrapp4, static image, headset awake at 120 Hz | 119.9 presented/drawn fps; 59.9 depth updates/s; model 16.3–16.7 ms | Demonstrates asynchronous rendering with depth reuse on the tested setup. |
| xrapp5, 3840×2160 desktop, headset asleep | Capture 60 fps; worker about 54 runs/s; render loop 120 fps | A loop-rate result, not proof of 120 drawn frames/s during gameplay. |
| xrapp5, live desktop video, headset worn | Stereo effect visually confirmed | End-to-end visual feasibility; sustained game-load timing remains unmeasured. |

Sources: [M1 native inference](research/m1-native-dml-findings.md),
[M3 rendering and runtime depth](research/m3-depth-integration-findings.md), and
[M4 capture and edge quality](research/m4-capture-findings.md).

A 120 Hz headset does not imply 120 new game frames or 120 new depth maps per
second. At 90/120 Hz the display intervals are 11.1/8.3 ms; inference may refresh
less frequently while rendering continues. Current reuse is not motion-compensated
reprojection of moving game objects. Stale depth can produce incorrect edges
during fast mouse turns, scene cuts, or object movement.

The old conclusion that a roughly 19 ms floor was entirely CPU-side integration
overhead was too strong. Native GPU input improves the measured path, but GPU
contention and graph structure remain material. reference-app's approximately 7.7 ms
figure came from inspected settings with unknown hardware; it is not a controlled
comparison against VRX. See [the teardown](research/reference-app-teardown.md) and M1.

## Current architecture and design direction

```text
Flat game renders normally; keyboard and mouse remain connected to the game
    |
Windows.Graphics.Capture on a D3D11 device
    |
Shared source textures + producer fence
    |                                      |
Depth worker: D3D12 / DirectML queue         Render: D3D12 / OpenXR queue
    GPU resize + normalize                     latest source colour
    Depth Anything V2                          + latest completed depth
    CPU depth readback                         per-eye stereo warp + hole fill
    range smoothing + dilation                 colour swapchain -> headset
    publish depth upload slot -----------------^
```

The model and OpenXR renderer share one D3D12 device with separate queues. The
capture device uses the same adapter. Depth buffers use a triple-buffer handoff;
source textures currently use an eight-slot ring without reader ownership.

Keep **Windows capture + DirectML + OpenXR** as the baseline while measuring its
compatibility. The game's graphics API should not require a matching inference
backend, because VRX consumes captured pixels. This is a design advantage to test,
not a guarantee that every game or display mode can be captured.

The initial path should require no game modification, DLL injection, administrator
rights, virtual display driver, or per-game shader setup. Prefer windowed or
borderless play initially. Treat exclusive fullscreen, protected content, HDR,
and hybrid-GPU laptops as explicit test cases with clear fallback guidance.
Do not promise anti-cheat compatibility or attempt to bypass capture restrictions.

OpenXR depth submission is optional for the product. If used,
`XrCompositionLayerDepthInfoKHR` attaches to projection views, not quad layers.
The app must synthesize the stereo images itself. The tested SteamVR result is
specific to that runtime version and setup, not a universal statement about every
runtime. The historical [API investigation](research/openxr-depth-apis.md) should
be read alongside the later M3 corrections.

## Fixes before expanding the feature set

These are open findings from source review, not newly reproduced hardware
failures. Function names below refer to xrapp5 unless stated otherwise.

| Priority | Fix | Acceptance check |
|---|---|---|
| P0 | **Protect source textures from reuse.** `CaptureMain` and `RecordCpuSource` advance the ring without waiting for readers. Introduce frame IDs, explicit ownership, and GPU completion fences for both readers. Retain paired colour until its final use; drop incoming frames when no safe slot is available. | Artificially delay inference and rendering beyond a full ring rotation. No texture is overwritten while referenced, no paired frame changes identity, and shutdown does not deadlock. |
| P1 | **Handle tracking and frame errors.** `RunFrameLoop` continues after failed `xrLocateViews` and does not inspect view validity. Check results, view count, validity flags, and swapchain acquire/wait/release state. | Tracking loss submits no invalid projection; pause/resume and session loss follow valid OpenXR call order. |
| P1 | **Handle resize and capture lifetime.** `CaptureMain` only copies the top-left region after resizing. Rebuild or safely rescale source resources, clear unused regions, and handle minimize, window closure, display changes, and capture exceptions. | Shrink/grow a game window, change resolution, minimize, close, and reopen it without stale borders, crashes, or manual process termination. |
| P1 | **Propagate worker failures and depth age.** `WorkerMain` currently logs failure and exits while rendering continues with old depth. Publish health and source timestamps; fall back to flat viewing when changing content lacks usable depth, and offer retry. A static unchanged source should not expire merely because no new depth is needed. | Inject inference/capture failure and a slow worker. The app reports the fault, avoids indefinitely applying stale depth to new frames, and recovers or exits with an error. |
| P1 | **Remove unnecessary runtime requirements.** `InitXrInstance` requests the depth extension even when depth submission is disabled and prefers a Steam installation path for the loader. Use a packaged standard loader, the active runtime, and capability-based extension/format selection. | Start on a compatible runtime without the optional depth extension or the assumed Steam directory; unsupported required capabilities receive a clear explanation. |
| P1 | **Make setup reproducible.** Resolve models relative to the installation or explicit configuration; pin dependencies and provide a repeatable build/package process. Validate model tensor names, shapes, types, and finite depth values before consuming them. | Build/run from another directory or Windows account with no edits to source paths. Missing or incompatible assets fail clearly. |
| P2 | **Tighten resource and error ownership.** Check ignored API results, release ORT resources and allocation wrappers, bound failed GPU waits, and make partial initialization safe to clean up. | Repeated start/stop and injected initialization/device failures do not leak resources, hang, or report false success. |

Do not solve the ring race by simply enlarging the ring. A time assumption is not
an ownership guarantee. Similarly, preserve the non-blocking inference design
without claiming the render thread never waits: it currently waits for in-flight
render/upload resources to become reusable.

## Proposed first-release features

These are proposals, not current capabilities. Keep the main interface small;
put experimental and diagnostic options behind an advanced panel.

1. **One simple launcher.** Show running windows and monitors with recognizable
   names, remember the last source, and provide Play/Stop. Detect missing runtime,
   headset, model, and capture availability before starting. Normal play continues
   until stopped instead of ending after a benchmark duration.
2. **Preserve keyboard and mouse gameplay.** Return focus to the selected game
   after launch. VRX should not consume ordinary gameplay keys, alter raw mouse
   input, or repeatedly take focus. Support configurable, conflict-checked global
   shortcuts for recenter, 2D/3D toggle, and stop; no controller interaction should
   be needed. Handle Alt-Tab and restoring game focus predictably.
3. **A comfortable virtual screen.** Start with a stable screen anchored in the
   user's space, adjustable size/distance, recenter, and conservative 3D strength.
   Keep the current head-following presentation as an optional mode. Spatial
   screen placement requires new rendering work; `--freeze-pose` is a diagnostic,
   not a finished implementation. Head tracking must not steer the game camera.
4. **Instant flat-view fallback.** A toggle should remove stereo disparity for
   menus, troublesome scenes, or user preference without restarting the session.
   Flat viewing should also work when AI initialization fails; today model setup
   is a prerequisite even for `--no-warp`.
5. **Minimal quality controls.** Expose 3D strength plus Auto/Performance/Quality
   only after presets are measured. Keep rendering responsive by limiting depth
   update rate and dropping obsolete work; do not queue every captured frame.
   Reduce depth workload under contention before reducing headset presentation
   rate. Add model-size variants only with validated quality and tensor geometry.
6. **Remember settings per game.** Persist source, screen placement, strength,
   quality, and shortcuts with a reset-to-defaults option. Prefer process identity
   plus user confirmation over the current first matching window-title substring.
7. **Make cursor and HUD behavior usable.** Offer cursor inclusion/exclusion and
   check for duplicate cursors. Test aiming reticles, subtitles, minimaps, and text
   menus explicitly. Begin with lower strength and the flat toggle; a manual HUD
   depth-exclusion region is a later option if testing justifies it. Automatic HUD
   separation is not a solved capability.
8. **Clear status and diagnostics.** Keep routine operation quiet. Show actionable
   capture/AI/runtime errors and an optional performance panel; provide an exportable
   diagnostic report without saving captured game images by default.

## Compatibility and validation plan

The first supported configuration should be the existing Windows, RTX 3090,
PS VR2, and SteamVR setup. Expand the supported list only after tests, while
retaining DirectML as the baseline for investigating AMD and Intel support.

| Area | Required coverage |
|---|---|
| Games and graphics APIs | Several actual games spanning DX11, DX12, Vulkan, and OpenGL; fast mouse-look, racing, strategy/text-heavy, and older titles. Record tested versions and results rather than declaring API-wide support. |
| Capture modes | Window and monitor capture; borderless first, then exclusive fullscreen. Alt-Tab, minimize/restore, launchers, changing titles, resolution switches, multi-monitor, and mixed DPI. |
| Image formats | 16:9, ultrawide, letterboxing, small windows, scaling, and SDR. Detect HDR and explain an SDR fallback until an HDR conversion path is verified. |
| Input | Keyboard combinations, raw/relative mouse motion, cursor capture, clicks, text entry, shortcut conflicts, and focus recovery. Test while the headset is worn. |
| Hardware/runtime | Additional GPU vendors, active OpenXR runtimes, headset refresh rates, and capture/headset adapter combinations. Cross-adapter capture needs explicit handling or a clear unsupported message. |
| Visual quality | Rapid turns, scene cuts, particles/transparency, thin geometry, foreground weapons, HUD, subtitles, and dark scenes. Compare flat and 3D viewing in-headset. |
| Reliability | At least a 30-minute game session per initial reference title; tracking loss, source closure, worker failure, ring-pressure stress, repeated start/stop, and runtime restart. |

Measure with the **headset awake and a real game running**:

- Baseline game fps/frame times without VRX, then with flat capture, then with AI
  stereo enabled, using the same scene and settings.
- Capture fps, actually drawn/submitted headset frames, depth updates/s, dropped
  frames, and p50/p95/p99 inference and render times. Loop rate alone is insufficient.
- Source-to-submit latency, colour/depth timestamp mismatch, and the age of the
  source used for depth. The current age-since-inference-completion log excludes
  capture and inference delay; it is not end-to-end latency.
- GPU load/memory and observed input responsiveness. Capture-to-submit timing is
  useful instrumentation but is not a direct input-to-photon measurement.

Extend shader tests to the real filtered path: different colour/depth sizes,
BGRA capture input, aspect ratios, border cases, and both fill modes. Add
synchronization and fault-injection checks independent of a live game. Separate
shader/CPU tests from OpenXR and model startup so they can run without a headset;
currently `--selftest` still goes through those initialization stages.

Before calling the first release usable, require normal launch and play without
command-line edits or controllers, no interference with game input, passing
lifecycle/stress checks, and documented in-headset frame pacing and visual results
for the reference titles. Set performance preset thresholds from those measured
baselines; the existing evidence does not justify a universal fps guarantee.

## Delivery order and later investigations

**Milestone 1 — reliable prototype:** fix source ownership, tracking, resize,
failure propagation, and setup. Extend tests and collect one real gameplay
baseline with the headset awake. Exit when the lifecycle and ring-stress checks
pass and frame/source identity is traceable throughout the pipeline.

**Milestone 2 — simple playable app:** launcher, keyboard shortcuts, focus
handling, spatial screen/recenter, flat fallback, basic strength control, and
persistent settings. Exit when a seated player can launch, play, adjust, and stop
using only the keyboard and mouse across the initial reference titles.

**Milestone 3 — compatibility and tuning:** expand the matrix, package the app,
add measured presets and diagnostics, and improve fast-motion artifacts without
adding setup burden. Publish tested configurations and known limitations.

After those milestones, investigate motion-aware depth stabilization, better
edge-aware depth upsampling, and two-dimensional hole filling. Each needs a
measured quality benefit against its GPU cost; mirror fill cannot reconstruct
unseen backgrounds and can create repeating patterns.

Fused model exports, GPU-only depth output/normalization, and sliced inference
are optimization experiments. Keep a live-input correctness check and
GPU-completion timing for every experiment: M1 already found fast but stale or
unfinished results. CUDA/TensorRT backends are later options if measured gains
justify maintaining additional paths; they are not prerequisites for the first
release. Real game-depth integrations, injection-based capture, virtual display
drivers, and frame generation are deferred because they add compatibility or
setup complexity. Touch/motion-controller support remains outside initial scope.

## Evidence and code map

- [M1 native inference](research/m1-native-dml-findings.md): verified timings,
  GPU input, unsuccessful output-binding experiments, and model graph analysis.
- [M2 OpenXR bootstrap](research/m2-openxr-findings.md): the tested runtime,
  graphics binding, and initial presentation.
- [M3 depth integration](research/m3-depth-integration-findings.md): stereo
  correctness, worker/render separation, headset-awake results, and depth-chain A/B.
- [M4 capture](research/m4-capture-findings.md): live input, normalization,
  mirror filling, dilation, and remaining image-quality compromises.
- [OpenXR API research](research/openxr-depth-apis.md) and
  [reference-app teardown](research/reference-app-teardown.md): historical reference material;
  later measurements and this status document supersede early assumptions.
- [xrapp5.cpp](bench/native/openxr/xrapp5.cpp): current application;
  [xr_common.h](bench/native/openxr/xr_common.h): CPU reference and shared helpers.
- [build.bat](bench/native/openxr/build.bat): current machine-specific native build.
- [bench_depth.py](bench/bench_depth.py), [native probes](bench/native/dmlgpu/dmlgpu.cpp),
  and [fixed-shape exporter](bench/make_fixed_shape.py): investigation tools.
  Models and build outputs are excluded from version control.

This document defines the current status and proposed direction. Historical
research notes contain superseded next steps; they should not be treated as the
current implementation backlog.
