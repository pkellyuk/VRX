# VRX image quality and GPU depth pipeline

The `xoptimal` work adds optional image and stereo controls and moves image-sized
depth processing onto the GPU. It continues to use captured game images and
estimated depth. It does not inject into a game or read its depth buffer.

All seven new profile settings start at zero or off, including in old profiles,
new profiles and new-install defaults. Existing stereo strength, timing, model,
room and screen settings retain their previous defaults. Base settings, Reset
this profile and Apply to all include the new controls.

## What changes the 3D effect

The depth model supplies relative depth, not the game's actual geometry. VRX
normalizes that estimate and converts it into horizontal displacement for each
eye. A scene can span the full depth range while most objects occupy a narrow
part of it. Increasing overall strength also increases the gaps revealed around
foreground objects. Incorrect or old depth can make those gaps and silhouettes
more noticeable instead of improving the picture.

The new controls address different parts of that process. Scene depth position
moves the apparent scene relative to the screen; it does not increase separation
between objects. Image sharpness improves clarity rather than stereo separation.
Depth separation changes how the existing depth range is distributed.

## Controls

The controls are in Expert mode's **3D & depth** section. They save per game.

| Control | Default / range | Behaviour |
| --- | --- | --- |
| Depth separation | 0%; 0–100% | A monotonic depth curve emphasizes distant layers and compresses the near end. Zero preserves the original mapping. |
| Scene depth position | 0%; −100–100% | Shifts disparity relative to the screen plane. Zero preserves the original position. |
| Refine depth edges | Off | Uses image colour to guide depth interpolation near depth boundaries. |
| Recover background from earlier frames (experimental) | Off | Repairs newly exposed stereo holes with verified history when suitable history exists. |
| Image sharpness | 0%; 0–100% | Bounded adaptive sharpening of the shared source picture before eye separation. |
| Head movement depth | 0%; 0–100% | Experimental, small horizontal viewpoint changes inside the picture when the head moves sideways. Needs a fixed screen. |
| Capture HDR colour | Off | Captures float scRGB and tone maps it on the GPU to the SDR picture used by the models and headset. |

All except Capture HDR colour apply live. HDR capture takes effect at the next
Attach / Play. Head movement depth rests while Screen follows my head is selected
and keeps its saved amount.

The controls do not guarantee that every game or scene looks better. Depth errors
are still possible, colour edges can be misleading, and hidden surfaces may never
have appeared in the captured frames. Temporal filling and head movement depth
remain experimental. Headset comparisons on representative gameplay are needed
before changing the neutral defaults.

## Stereo and image processing

`stereo_enhancement.h`, the warp in `xrapp5.cpp` and
`stereo_enhancement_shaders.h` share a numerical test contract.

* **Depth curve:** for normalized nearness `n` and amount `a` in 0–1,
  `n' = (1 + 2a) n / (1 + 2a n)`. The endpoints stay fixed and ordering is
  preserved. This is a stable, manually controlled curve; it is not automatic
  histogram equalization or scene-adaptive exposure. Its local effect depends on
  the original depth: distant layers expand while the near end compresses.
* **Scene position:** adds up to ±0.25 inverse metres to the disparity mapping.
  This changes convergence without changing the depth differences between
  objects. The existing physical screen-plane correction remains in place.
* **Guided depth edges:** a parallel preparation pass evaluates a small
  colour-guided depth neighbourhood at colour resolution. A locally flat depth
  field keeps ordinary interpolation; non-flat areas retain some model evidence
  to limit texture copying. The older foreground dilation is bypassed when this
  refinement is selected, so both edge treatments do not accumulate.
* **Sharpening:** a bounded five-tap filter runs on the common colour source.
  Local minima/maxima constrain overshoot. Both eyes therefore start with the
  same sharpened image. This is VRX's own lightweight filter, not a bundled
  FidelityFX CAS implementation or a final-eye sharpening pass.
* **Temporal filling:** history is accepted only with suitable source/depth
  alignment, increasing capture timestamps, a matching layout and short age.
  The current policy requires image/depth timestamps within 50 ms and successive
  history images within 120 ms. A GPU scene-change check and local matching
  reject unreliable candidates. Matching is limited to two background patches
  per row per eye, approximately ±9 pixels horizontally and ±5 vertically.
  Only holes 3–64 pixels wide with foreground-boundary evidence are candidates;
  smaller gaps and exhausted search budgets use spatial filling.
  The existing background-side spatial filling
  remains the fallback. This reuses observed image information; it does not run a
  generative inpainting model or invent game geometry.
* **Head movement depth:** the extra offset follows the screen's horizontal
  axis, is zero after recentering, and is smoothly limited to a 25 mm displacement.
  It operates inside the already placed screen. It does not control the game
  camera, and it does not provide full six-degree-of-freedom scene reconstruction.

The neutral warp has its own shader variant. Optional temporal matching does not
add its register requirements to the neutral shader. The existing subpixel warp,
frame timing modes, motion-steady depth and two-model fusion remain available.

## HDR capture

`--hdr-capture` selects `R16G16B16A16_FLOAT` in the Windows Graphics Capture frame
pool. Initial creation, resizing and switching between a window and its display
retain that choice. `CaptureScaler` keeps the source format until the GPU tone
map, including client-area cropping and fitting a resized source into the stable
capture ring.

Windows scRGB uses a value of 1 for 80 nits. The current display's SDR white
level is queried periodically and used to calibrate the image. The tone map
uses a shared highlight shoulder and sRGB encoding before the SDR capture ring,
so inference and both eye views receive the same picture. Invalid channels are
sanitized; the output and letterbox bars are opaque.

This is HDR-aware capture with SDR output. It does not make the headset output
HDR, preserve every wide-gamut colour, or reproduce the game's own tone mapper.
The white-level query and capture-format choices follow Microsoft's
[SDR white-level definition](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_sdr_white_level)
and [HDR capture guidance](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture).

## GPU residency

The selected depth GPU runs the main depth model and the optional asynchronous
Depth Anything V2 anchor. The headset GPU runs depth image processing near the
captured colour and hardware motion estimator. Selecting another depth GPU
therefore offloads inference, not every shader used for depth or presentation.

The normal GPU path uses persistent DirectML GPU output binding. Normalization,
resampling, histogram work, dilation, motion-based steadying and guided fusion
run in compute shaders. Luma goes directly to the hardware motion estimator,
and resolved vectors stay on GPU for depth reprojection. Cross-adapter bridges
transfer image/depth data with GPU copies and fences. Foreground crop fitting
and application also run on GPU.

Published depth lives in immutable GPU slots and is copied to the renderer's
depth buffer after the relevant fence. The CPU reads a 49×28 tracker summary
(about 5.5 KB) and scalar diagnostics. Tracking decisions, settings, scheduling
and small control data remain on CPU. Moving these small decisions would not
remove an image-sized transfer.

Each slot records whether its depth is undilated for edge refinement. Changing
the edge setting republishes the retained depth even when capture is still,
keeping the original colour/depth pairing. Model switches refresh a still
image too. All depth kernels use the existing shader cache and are compiled
before creating the VR session when a cached version is unavailable.

The CPU reference path remains available with `--cpu-depth` and as an explicit
fallback if GPU processing cannot initialize or fails safely. Model inference
still uses DirectML in this mode; depth output and post-processing move through
CPU memory. Both model sessions reject silent CPU execution-provider fallback.
The desktop reports the processing fallback. GPU residency does not remove inference cost, contention, cross-GPU
copy cost or every CPU synchronization wait. Sustained gameplay timing must be
measured before claiming a frame-rate or latency improvement.
If outstanding GPU work cannot retire safely, depth stops instead of reusing its
buffers on the CPU. A real driver hang/device reset was not simulated; existing
global shutdown waits may still require restarting the engine for a stuck driver.

## Visible health and control compatibility

Both Easy and Expert modes show the actual engine-reported stereo state during
a session: active, flat because depth is unavailable/out of date, disabled, or
waiting. They also show GPU processing versus CPU fallback and a warning when
requested depth improvements cannot run. These reports are separate from the
requested settings and remain visible without opening the session log.

The structured log contract is:

```text
Stereo status: active|flat|disabled|waiting; optional diagnostic detail
Depth pipeline: GPU|CPU fallback; optional diagnostic detail
Depth features: available|unavailable; optional diagnostic detail
Stereo effects: available|unavailable; optional diagnostic detail
```

Each line contains one state, not the literal `|` alternatives. Timestamps/thread
prefixes are allowed. Labels in the desktop are localized; engine diagnostic
details retain their original wording. A new session clears earlier health.
Depth-feature and optional stereo-effect warnings are independent: recovery of
one does not hide a failure in the other.

Desktop snapshots are now version 12. After the v11 room-light colour, they
append these fields in order:

```text
depthContrast convergence edgeRefinement temporalFill sharpness headParallax hdrCapture
```

The native parser retains versions 1–11 with neutral defaults for the new
fields. Partial, malformed, out-of-range, trailing and future-version snapshots
are rejected without replacing the last complete settings.

## Verification

Completed checks for this work:

* Release .NET build: zero warnings and zero errors with the documented clean
  `LIB` environment.
* Full WPF smoke test: old-profile defaults, per-game round trips, base settings,
  Reset, Apply to all, all seven controls, startup-only HDR argument, exact
  health parsing and recovery, both-mode visibility, localization and layout.
  Generated Easy/Expert health and all-open Expert screenshots were inspected.
* Native playback tests, including v12 parsing and the snapshot emitted by C#.
* `test-hdr.bat`: GPU shader versus CPU float reference, crop and two white
  levels, monotonic/bounded mapping, actual GPU NaN/Inf sanitization, linear HDR
  resize with black bars, same-size source transitions between SDR and HDR,
  recentering and bounded/rotated head offsets. The shader tests use D3D11 WARP.
* Focused GPU-depth probes on the RTX 3090 and RTX 3060: normalization maximum difference
  about `6e-8`, translated partial-trust steadying `3e-8`, and guided fusion
  `1.5e-6` versus the CPU reference. Flat/reset/non-finite cases passed.
  Changing data survived a 3060 → 3090 → 3060 bridge exactly.
  Foreground crop alignment/blending at a noncentral origin matched the CPU
  reference within `1.55e-6`; flat, negatively correlated and nonfinite crops
  were rejected with the baseline left exactly unchanged.

* Final native build and six integrated configurations: ZipDepth on one GPU with
  and without steadying/fusion, ZipDepth plus fusion on the second GPU, Depth
  Anything V2 on either GPU with steadying, and the explicit CPU post-processing
  reference. All passed using changing synthetic frames; debug runs reported
  zero Direct3D errors on both devices. A final audit of every distinct warning
  found only D3D12 message 1328 (buffer initial state ignored), with no additional
  synchronization or resource-binding warnings.
* Exact published-depth-to-renderer handoff; switching between both model
  bindings; static-image edge toggles in both directions on GPU and CPU; and an
  injected, safely retired GPU failure recovering to rendered CPU depth.
* Original warp, subpixel, capture preparation, curved screen, ambilight and room
  renderer goldens, plus combined enhancement goldens. Temporal tests verify
  nonzero `(4,2)` source motion, recovered historical RGB, repeated-frame
  stability and rejection after cuts, layout/curve changes and stale history.
* Source ownership stress: 100,000 publications, 2,000 reads and zero drops.
* Shader-cache prewarm: all 42 compute variants compiled successfully into an
  initially empty cache without an OpenXR instance or GPU workload.

The automated checks do not establish perceived improvement in every game.
Representative headset/gameplay A/B testing is still needed before changing the
neutral defaults.

### Optional-effects cost

The final 1920×1080 synthetic benchmark on the RTX 3090 measured the following
GPU times. It uses normal disparity, alternating source frames and 24 measured
samples after six warmups. The source is upsampled from the depth-grid test
image; these are isolated stereo-pass costs, not full application frame times.

| Configuration | GPU p50 | GPU p95 |
| --- | ---: | ---: |
| Original warp | 2.68 ms | 3.60 ms |
| Depth curve | 3.18 ms | 3.66 ms |
| Guided depth edges | 3.06 ms | 3.71 ms |
| Sharpening at 50% | 2.90 ms | 3.84 ms |
| Temporal filling | 3.63 ms | 4.22 ms |
| Combined curve, edges, sharpening and temporal filling | 6.50 ms | 7.98 ms |

The repeated baseline was 2.66 ms p50. Temporal filling recovered about 2,910
pixels per eye pair in this scene, and the combined case about 6,359. The combined
case costs substantially more GPU time and remains opt-in. A busy game and the
headset compositor will change these timings.

Useful commands from the repository root:

```powershell
$env:LIB = ''
dotnet build desktop/VRX.Desktop/VRX.Desktop.csproj -c Release
desktop/VRX.Desktop/bin/Release/net10.0-windows/VRX.Desktop.exe --smoke-test
cmd /c bench\native\openxr\test-playback.bat
bench/native/openxr/out/playback_test.exe desktop/out/control-contract.txt
cmd /c bench\native\openxr\test-hdr.bat
cmd /c bench\native\openxr\test-stereo-enhancement.bat
cmd /c bench\native\openxr\test-source-ring.bat
cmd /c bench\native\openxr\build-gpu-depth-probe.bat
bench/native/openxr/out/gpu-depth-probe/gpu_depth_probe.exe 0
bench/native/openxr/out/xrplayer.exe --selftest-gpu=0 --steady --fuse --debug
bench/native/openxr/out/xrplayer.exe --selftest-gpu=0 --bench-stereo
# On a machine with a second adapter:
bench/native/openxr/out/xrplayer.exe --selftest-gpu=0 --depth-gpu=1 --steady --fuse --debug
```

`--selftest-gpu=N` selects a DXGI adapter and creates no OpenXR instance or
session. Its final lifecycle test deliberately logs a CPU fallback after an
injected GPU error; the accompanying PASS verifies recovery. This test hook is
not enabled during normal playback. Adapter numbering depends on the machine.

The numerical references and smoke tests do not require attaching to or
modifying a game. Actual Windows HDR capture and perceived stereo comfort still
need validation on the intended display/headset combination.
