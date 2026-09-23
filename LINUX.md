# Linux port specification

Status: implementation in progress, 2026-09-23. The Linux Vulkan/OpenXR renderer, GPU stereo warp, static and live ZipDepth paths, portal/PipeWire capture, and the flat-screen room GPU passes are running. The room eye shader shares the Windows HLSL, and the controller has Linux-native room controls. Headset visual acceptance, longer performance measurements, recentering and distribution remain.

## Goal and scope

Run VRX as a native Linux desktop application: select a game window or monitor,
capture its picture, estimate depth, and display a stereo screen in an OpenXR
headset. Retain the Windows build and its current behavior. Linux auto-attach,
foreground-window watching, and the countdown banner are **out of scope**. Linux
users select a source and start a session themselves.

The first playable release (MVP) supports Linux x86-64, one GPU for capture,
inference and rendering, a Vulkan-capable OpenXR runtime, and an NVIDIA GPU with
ONNX Runtime's CUDA execution provider. It includes ZipDepth, a flat stereo
screen, recentering, basic placement and stereo controls, saved profiles, a
room with picture lighting, glass and reflections, and session logs. Other GPUs,
the second model, and optional visual extras are follow-up work.

This is a native port, not an attempt to run the Windows executable through Wine.
The Linux engine must not load Direct3D, DirectML, WinRT or Windows DLLs.

### Reference headset and connection

The first headset validation target is the user's **PICO 4**. Two connection
paths are acceptable: [Steam Link VR on PICO 4](https://steamcommunity.com/games/250820/announcements/detail/514096230384533860)
over Wi-Fi with SteamVR as the Linux OpenXR runtime, or
[ALVR wired mode](https://github.com/alvr-org/ALVR/wiki/ALVR-wired-setup-%28ALVR-over-USB%29)
over USB with SteamVR. **ALVR is not a VRX dependency.** Prefer Steam Link if it
connects reliably on the reference Linux host; retain ALVR as an optional
fallback. The [PICO Steam Link store listing](https://store-global.picoxr.com/en/detail/1/7494362493653958711)
still names Windows in its minimum host requirements, so Linux compatibility
must be established on the actual test machine rather than assumed from the
headset app listing. VRX itself uses OpenXR and does not call either streaming
app's APIs. Before L1 headset testing, verify the chosen headset-to-SteamVR
link, active OpenXR runtime, and a known working OpenXR sample independently
of VRX. The streaming link is a test environment requirement, not a feature VRX
must implement.

## Current code and migration boundaries

| Current component | Location | Linux treatment |
| --- | --- | --- |
| Main engine, D3D12 renderer, shader strings, DirectML inference, OpenXR loop | `bench/native/openxr/xrapp5.cpp` | Extract portable algorithms; implement Linux Vulkan, inference, capture and OpenXR adapters. Do not add a second giant conditional compilation path to this file. |
| Windows capture and source lookup | `capture_window.h` and capture code in `xrapp5.cpp` | Replace with portal/PipeWire source selection and capture. |
| Frame lifetime and timing | `source_ring.h`, `frame_timing.h` | Preserve semantics; remove D3D-specific assumptions from shared contracts. |
| Synthetic reference scene | `synthetic_scene.h` | Shared by Windows and Linux for repeatable render and depth checks. |
| CPU stereo reference, geometry and effects | `xr_common.h`, `screen_anchor.h`, `screen_curve.h`, `room.h`, `depth_fusion.h` | Reuse portable math and reference output; isolate Windows-only includes and helpers. |
| Hardware motion estimation | `motion_estimator.h` | Defer; this is D3D12 video-specific. Disable steadying and fusion until a tested Linux replacement exists. |
| Versioned live settings | `desktop_control.h`, `Profile.Control()` in `Profile.cs` | Use Windows behavior as a reference. The Linux `VRXL` snapshots have their own versioned fields and do not use Windows file I/O or key codes. |
| Desktop app | `desktop/VRX.Desktop` | Use its behavior as a reference, but keep Linux profiles and the PyQt controller native to Linux. WPF/XAML, Win32 window enumeration and Windows profile files are not dependencies. |
| Models and release | `release/build-models.ps1`, `release/build-release.ps1` | Retain model hashes; provide Linux acquisition/build and packaging scripts with Linux dependency notices. |

`xrapp.cpp`, `xrapp3.cpp`, `xrapp4.cpp` and the native probes document earlier
experiments. The production feature baseline is `xrapp5.cpp`; the port must not
mistake an older experiment for the current engine.

## Target architecture

The engine remains a separate native process. The controller starts it, passes a
selected capture source and a settings file path, observes stdout/stderr, and
writes complete atomic control snapshots as it does on Windows. A session has
three threads or equivalent asynchronous stages:

1. **Capture:** receive frames with source identity, dimensions, pixel format,
   timestamp and a completion/synchronization handle; publish them to the
   bounded source ring. Drop an incoming frame if every slot is in use.
2. **Inference:** prepare the fixed-shape ZipDepth input, run ONNX Runtime,
   normalize the depth result, and publish a depth map tagged with the source
   frame sequence and capture timestamp.
3. **Render/OpenXR:** use the most recent completed source frame and depth map;
   warp both eyes, render the screen into OpenXR swapchain images, and submit the
   frame. Never wait for a new depth map on the headset frame path.

Define small interfaces around capture frames, inference input/output, GPU
resources and fences, OpenXR swapchain images, and controller commands. Their
contracts must describe ownership, clock domain, resize generation and error
behavior. Keep CPU geometry/depth algorithms and profile validation independent
of these interfaces. Extract only code needed by the next milestone; keep each
extraction reviewable and verify the Windows result before changing its backend.

## Graphics and OpenXR

Use Vulkan for the Linux engine and the runtime's Vulkan graphics binding. Query
the runtime's required Vulkan device and extensions before device creation; do
not pick a GPU solely from display order. Create color swapchains, render or
compute into their acquired images, and preserve the existing OpenXR session,
pose, recenter and frame sequencing behavior. Respect acquire/wait/release
ordering and cleanly recover from session loss or headset disconnect.

Port the current HLSL compute stages deliberately to Vulkan-compatible shader
source and SPIR-V. Record source, compiler version and options in the build so
shader output can be reproduced. Begin with source preparation, a flat stereo
warp and swapchain copy, then add the room passes before the first release.
Screen curvature and other optional effects may follow later. Match
image origin, color space, alpha, depth convention, row pitch and per-eye pose
against the existing CPU references and Windows outputs. A shader compiling is
not proof of equivalent images.

The production path should keep captured color on the GPU when the compositor
offers an importable DMA-BUF. Provide an explicit copy fallback for formats or
modifiers that cannot be imported. Measure copy cost and latency; never assume
DMA-BUF availability on every desktop or driver. Use Vulkan synchronization for
capture, compute and OpenXR image use; a ring slot cannot be reused until every
consumer has completed its GPU work.

## Capture and source selection

Use the xdg-desktop-portal ScreenCast interface to ask the user to select one
window or monitor, then consume its PipeWire stream. This is the common capture
path for the MVP. The portal may show a chooser and the selected stream is an
opaque source, not a Win32 HWND. Do not require enumerating every game process
or forcing focus back to the game. Display the source label when provided, and
otherwise use a neutral name such as "Selected window".

The capture adapter must handle both DMA-BUF and shared-memory frames, negotiated
formats/modifiers, frame timestamps, size changes, stream pauses, end-of-stream,
and permission withdrawal. On resize, increment a source-layout generation,
retire old GPU resources only after consumers finish, and keep the OpenXR
session running where possible. On source loss, stop capture and show a clear
"select source again" state rather than silently showing a stale frame.

Portal source identity is not guaranteed to map to a stable executable path.
Linux profiles therefore need a user-editable profile name and must store the
last useful source description only as a hint. A restored portal token may be
used only when supported by the selected portal and with a new selection path
always available. Never assume a Windows profile's executable path or HWND can
identify a Linux source.

## Depth inference

Keep the checked ZipDepth ONNX model at `672x384` and its existing preprocessing
and postprocessing rules. Keep the checked Depth Anything V2 model at `686x392`
for later work. A Linux model acquisition script must verify the expected
SHA-256 values recorded in `release/build-models.ps1`; it must not silently
accept a different export.

Use ONNX Runtime's CUDA execution provider for the first accelerated backend.
Prove that the shipped ZipDepth graph loads and runs on the supported driver and
runtime combination before integrating capture. Bind GPU input/output where
supported; a measured copy path is acceptable for MVP if frame latency remains
within the acceptance target. Detect unavailable CUDA or model errors at start
and report them plainly. A CPU inference mode may be provided for diagnostics,
but is not a promise of real-time VR performance. Treat AMD/Intel inference as
separate compatibility work; do not advertise it merely because Vulkan works.

Keep inference off the OpenXR render thread. Preserve depth range smoothing and
the frame identity used by Latest, Delayed and Matched timing modes. MVP requires
Latest; Delayed and Matched can land after the capture timestamps have been
validated. Keep the Windows-specific D3D12 motion estimator, steady depth,
fusion and second-GPU transfer disabled on Linux until equivalent implementations
pass visual and timing tests.

## Controller, settings and input

Keep Linux controller profiles independent of Windows save files and keyed by a
user-editable name. The PyQt controller needs source selection, Start/Stop VR,
Recenter, basic screen/depth controls, a status line, and accessible logs. Add
room controls when the Vulkan room renderer is validated. Hide or clearly
disable settings that Linux cannot apply yet.

Store Linux settings under XDG config/data locations. Use a versioned Linux
profile schema and a separate versioned live-settings snapshot; write both
atomically and validate ranges before applying them. `VRXL 1` snapshots carry width, distance, height, horizontal offset and stereo
strength and load with the room off. `VRXL 2` appends Room, Glass, Reflections,
ceiling light and RGB light colour. The controller writes version 2 and can
load older Linux profiles with defaults for those controls. Numeric serialization
must be culture-independent. Windows JSON profiles and the `VRX 11` desktop
snapshot require no direct compatibility or import path. Never use a Windows
virtual-key number as a Linux key symbol.

For MVP, the Recenter button is sufficient. Add keyboard shortcuts only after
deciding how they work while a game has focus on the selected desktop; shortcut
registration failure must not prevent a session. Do not implement auto-attach,
foreground monitoring, or a countdown overlay on Linux.

## First-release room and reflections

The Linux release must include the room described in `XROOM.md` and the CPU
reference in `bench/native/openxr/room.h`: a screen-lit room with walls, floor
and ceiling; the glow on its front wall; a ceiling light; glass walls; framed
panes and floor tiles; and per-eye reflections of the screen and room. The room
must remain aligned with the fixed screen and the tracked STAGE floor where
available. Linux-native profiles need live Room, Glass, Reflections, Room light
and Light colour controls, with the room off at level 0.

Port the Windows renderer's EMIT, MIRROR, LIGHT and eye passes from the HLSL
source in `xrapp5.cpp` to separately built Vulkan-compatible shaders and SPIR-V.
EMIT reduces source and glow patches to radiance; MIRROR builds the reduced
screen image when reflections are enabled; LIGHT fills the six-face, 64x64
lightmap; the eye pass traces each eye against the screen and room and samples
its lightmap, glass, frames, tiles, ceiling panel and reflected picture. Keep
the room-off path separate so its existing stereo warp is unaffected. Preserve
linear-light colour, image orientation, eye-specific reflected rays, and GPU
synchronization between these passes and OpenXR image submission. Port the
flat-screen room composition first; curved-front geometry may follow after
first release if the room remains correct for the supported flat screen.

The Linux live-settings format uses `VRXL 2` for these controls, preserving
`VRXL 1` parsing without overloading its fields. Use `room.h` and `XROOM.md` as the reference
for geometry, emitter reduction, form factors, bounce, Fresnel coating and
reflection edge filtering. Compare Vulkan lightmap and eye images against CPU
reference outputs across room-off, diffuse, glass, reflections and room-light
cases. Measure room pass GPU time, capture-to-display latency and memory on
the PICO 4 setup, and visually check floor alignment, per-eye reflections,
scene changes and live control updates. Include shader sources, compiled SPIR-V
and versioned Linux profile defaults in the package.

## Build, dependencies and distribution

Add a CMake build for the native Linux engine and a Linux CI job for compilation,
portable unit tests and shader validation. Keep the current Windows batch build
and release workflow working. Pin or constrain OpenXR, Vulkan/shader compiler,
ONNX Runtime and model inputs, and record the exact versions used for validated
release builds. Do not bundle the Windows `release/vendor/*.dll` files on Linux.

Provide a developer build command, a runtime dependency check, and a portable
Linux package before choosing any distribution-specific installer. Package the
engine, controller, shaders, models, licenses and third-party notices together;
show actionable errors for a missing OpenXR runtime, portal backend, PipeWire,
Vulkan capability or CUDA provider. Document the user steps for selecting an
active OpenXR runtime and granting capture access. A Linux package must not
change the Windows package or its signing workflow.

## Validation progress (2026-09-23)

On Ubuntu 26.04/KDE Wayland with an NVIDIA GeForce RTX 5060 Ti, SteamVR
2.17.10 and a PICO 4 connected through Steam Link over Wi-Fi, `vrx-probe`
found the headset and `XR_KHR_vulkan_enable2`. The `vrx-xr-synthetic` program
submitted 897 image-bearing frames in a 20-second run; the user confirmed the
moving screen was visible and correct in the headset. A later five-second run
submitted 75 image-bearing frames using the shared CPU stereo reference and
synthetic ground-truth depth. The extracted CPU warp matched the original code
bit for bit over both eyes, both hole-fill modes, both subpixel modes and
multiple scene times. The CPU path is a correctness baseline, not the intended
real-time renderer. The first Vulkan compute warp now runs on the same
synthetic scene, with forward scatter, near-source selection, mirror hole
filling, subpixel colour sampling and projective depth. A five-second SteamVR
run submitted 208 image-bearing frames and compared the first GPU frame with
the CPU reference: zero differing colour channels and zero depth pixels above
1e-5. This established synthetic parity and successful OpenXR submission.
The CMake build and its two smoke tests passed; its three-second SteamVR run
submitted 118 image-bearing frames and repeated the zero-difference parity
check. The later still-image and ZipDepth validation is recorded below.

The exact ZipDepth FP16 export was extracted from the checksum-verified
[v1.7.6 release](https://github.com/pkellyuk/VRX/releases/tag/v1.7.6) into
Git-ignored bench/models; linux/fetch-model.sh reproduces it. The Linux
Vulkan prep shader matched the CPU reference on a synthetic frame with zero
of 774,144 values outside the Windows 0.0028 tolerance (worst 0.0000124).
ONNX Runtime 1.23.2 validated the model's image [1,3,384,672] float32 input
and depth [1,1,384,672] output. CPU inference returned finite values in
about 60 ms. An isolated official ONNX Runtime CUDA bundle with CUDA 12/cuDNN 9
libraries ran with CPU fallback disabled on the RTX 5060 Ti: warm inference
was 2.5–3.0 ms after initialization. CUDA versus CPU depth differed by
0.000104 mean and 0.000481 max on the test tensor (output range 0.0516).

A PNG still now travels through Vulkan preparation, strict CUDA ZipDepth,
Windows-compatible resampling, percentile normalization and 3-pixel dilation,
then Vulkan stereo warp and OpenXR submission. On docs/vrx-headset.png,
all prep values passed the reference tolerance (worst 0.0000209);
the warp had only two one-byte colour rounding differences and no depth
differences above 1e-5. In the first test, SteamVR moved to
XR_SESSION_STATE_SYNCHRONIZED and requested only one image while the headset
was inactive. With the PICO 4 active through Steam Link, a 15-second
model-backed still
test reached XR_SESSION_STATE_FOCUSED and submitted 1,311 image-bearing
OpenXR frames with zero runtime skips. Vulkan prep and warp comparisons
passed again; the user confirmed the stereo still image looked correct
in the PICO 4. This validates the static ZipDepth-to-headset path on the
reference setup. L3 now has a live colour and asynchronous ZipDepth path.

L3 has begun with `vrx-capture-probe`, a libportal/PipeWire ScreenCast
client. It opens the desktop source chooser, connects to the selected
PipeWire node, negotiates CPU-mappable BGRA/BGRx/RGBA frames, copies them
into a bounded three-slot `SourceRing`, and reports frame sequence,
presentation timestamp, layout generation, copied frames and drops. This is the capture diagnostic used to validate the live adapter. On the first
KDE portal attempt, source selection succeeded but the initial PipeWire
format offer failed with `no more input formats`. Querying the source
revealed BGRA/BGRx at 3840x2160 and a PipeWire object serial. Offering
size and frame-rate ranges and targeting that serial fixed negotiation.
An eight-second KDE monitor test streamed 308 BGRA frames into the ring:
zero unsupported frames, zero ring drops, one layout generation and
33,177,600 bytes in the latest frame. PipeWire did not attach header
sequence/PTS metadata on this host; the ring uses monotonic arrival time
and its own sequence.

The renderer now accepts `--live`. It opens the same portal chooser, runs
PipeWire on a dedicated capture thread, resamples BGRA/BGRx/RGBA to the
686x392 input, and publishes each completed RGB image to a three-slot
`SourceRing`. OpenXR takes the latest completed frame without waiting for
capture. Depth is flat in this first live preview; ZipDepth inference is
not yet connected to live frames. On the PICO 4 via Steam Link, a
12-second run selected the KDE monitor, received 426 frames with zero
ring drops, and submitted 904 image-bearing OpenXR frames with zero
runtime skips. The first live warp matched its CPU reference exactly.
The user confirmed that the live image appeared in the headset. The
SteamVR OpenXR loader for this host lives in SteamVR's
`bin/linux64/libopenxr_loader.so`. The renderer now finds that path under
the user's Steam installation when no system loader is installed;
`VRX_OPENXR_LOADER` remains an override. A three-second synthetic check
using the fallback submitted 228 image-bearing OpenXR frames with zero
runtime skips.

The live path currently uses mapped shared-memory copy and CPU resampling.
With `--live --cuda`, an inference worker takes the newest completed
RGB source frame, prepares the checked ZipDepth NCHW tensor on the CPU,
runs the CUDA-only ONNX Runtime session, applies the existing percentile
normalization and dilation, and publishes a depth map tagged with the
source sequence. OpenXR uses the latest completed depth without waiting
for a new one. The default `--live` mode retains flat depth as a capture
diagnostic. DMA-BUF import and GPU-side live model preparation remain
optimization work. A controlled Qt
window test then alternated source sizes while OpenXR was running. Capture
reported 18 layout generations, 59 captured frames, zero ring drops and
5,999 image-bearing OpenXR frames with zero runtime skips. Closing the
window caused the live path to report `select a source again` and stop;
the renderer now returns a nonzero status for this interrupted session.
A fresh monitor selection after the test-window source closed also
worked: 1,477 captured frames, zero ring drops and 3,531 image-bearing
OpenXR frames in 45 seconds. Explicit portal permission withdrawal
remains to be validated before L3 acceptance.

A 15-second live CUDA ZipDepth run captured 514 frames, completed 445
depth updates, and submitted 860 image-bearing OpenXR frames with zero
capture drops or runtime skips. A 12-second timed run captured 410 frames,
completed 355 depth updates and submitted 672 image-bearing frames, again
with zero drops or skips. The last depth was tagged to capture sequence
409 while capture had reached 410. Across 355 depth samples, median/p95
CPU preparation was 16.81/18.42 ms, model plus depth postprocessing was
9.19/13.23 ms, and frame-arrival-to-completed-depth was 28.11/32.31 ms.
Arrival time is stamped after PipeWire frame copy and CPU downsampling;
these figures exclude upstream capture time and headset display latency.
The user confirmed the live depth effect looked correct in the PICO 4.
The strict CUDA session still disables CPU execution fallback. The live
CPU preparation uses the same reference math as the Vulkan shader check;
a three-second synthetic regression had zero of 774,144 tensor values
outside the established 0.0028 tolerance, with worst error 0.0000124.


L4 has begun with `linux/controller.py`, an early PyQt6 session window.
It starts the engine in `--until-stop --live` mode, offers flat or strict
CUDA ZipDepth, shows process output, stops an active session by SIGTERM,
and saves named launch profiles atomically under the XDG config directory.
The engine's `--until-stop` mode was checked with a synthetic headset run:
SIGTERM ended it cleanly after 1,180 image-bearing OpenXR frames and zero
runtime skips. An offscreen Qt/profile smoke check passed. A UI-driven live CUDA run with the normal desktop environment launched the
portal chooser, reached SteamVR, streamed logs into the controller, and
stopped cleanly from its Stop VR action with exit code 0. It submitted
1,549 image-bearing OpenXR frames with zero runtime skips, captured 905
frames with zero ring drops, and completed 851 depth updates. Median/p95
arrival-to-depth was 27.83/31.40 ms across those updates. The controller
now has live width, distance, height, horizontal and stereo
strength controls stored in named profiles. Its flat/CUDA choice still requires
a new session. The live snapshot parser and controller profile reload have
smoke coverage. A controller-driven live CUDA check changed width from 2.0 to
3.0 m, shifted the screen +0.5 m horizontally and +0.3 m vertically, reduced
stereo strength from 1.0x to 0.4x, then restored the initial values. The engine
logged all four snapshots and stopped cleanly after 2,336 image-bearing OpenXR
frames with zero runtime skips, 1,296 capture frames with zero drops, and 1,234
ZipDepth updates. The user confirmed that the width, position and stereo
changes all looked correct in the PICO 4. The Linux profile format remains
independent of Windows. A Linux `vrx-room-reference` fixture now builds
`room.h` natively and checks room geometry, STAGE floor placement, picture
emitters, the mirror image, diffuse/ceiling-lit lightmap samples and Fresnel
ordering. Its sample values provide a parity baseline for the forthcoming
Vulkan passes. The first Vulkan room shader, `room_mirror.comp`, now compiles to
SPIR-V. Its probe executed on Vulkan and matched `RoomMirrorPicture` in all
37,376 output channels within 1e-5 (worst difference 0.00000006). The Vulkan
`room_emit.comp` shader also matches `RoomEmitRadiance` across all 627 emitter
channels within 1e-5, including glow blocks and a ceiling light. The flat-room
`room_light.comp` pass matches `RoomTexel` across all 73,728 colour channels of
the six-face lightmap within 1e-5, including the glass/reflection finish and
nonblack world colour. The eye pass, room controls, headset image comparisons
and portable packaging remain before L4 acceptance.

## Milestones and acceptance gates

| Gate | Deliverable | Required evidence |
| --- | --- | --- |
| L0: reference environment | Record a specific Linux distribution, desktop session, GPU/driver, PICO 4 connection path (Steam Link over Wi-Fi or ALVR over USB), and active SteamVR OpenXR runtime. Probe Vulkan/OpenXR, portal capture and CUDA/ORT separately. | Versioned diagnostic output and a list of unavailable capabilities. If one link is absent, adjust the test environment before committing to its backend. |
| L1: synthetic headset frame | Linux engine builds and shows the moving synthetic scene and a still image in OpenXR. | Correct left/right orientation, head tracking and recentering; repeated startup/shutdown and session-loss tests; no capture or model required. |
| L2: depth and warp | ZipDepth, preprocessing and flat stereo warp work with synthetic/still input. | Compare GPU preprocessing and eye images against CPU references; document tolerances and representative worst cases; record capture-to-depth and render times. |
| L3: live source | Portal/PipeWire window and monitor capture feed the frame ring. | Sustained play, resize, source close, permission loss and reselect tests; no frame-ring reuse or GPU synchronization validation errors. |
| L4: first playable release | Linux UI launches/stops engine, writes live settings and saves named profiles; the Vulkan room includes picture lighting, glass, frames, tiles, ceiling light and reflections with live controls. | End-to-end source-to-headset session, clean stop, logs, profile reload, clear recovery messages, CPU/reference room image comparisons, PICO 4 visual checks, room GPU timing and a portable package. This is the release gate. |
| L5: further parity | Delayed/Matched timing, curved screen, advanced ambilight, Depth Anything, motion steadying, fusion and multi-GPU support as separate changes. | Each feature has a Windows/reference image comparison, latency budget and hardware coverage before enabling its control. |

For the MVP, measure and report median and 95th-percentile capture-to-display
latency, capture rate, depth update rate, dropped frames, GPU memory and frame
timing on the reference setup. Set numerical release thresholds from L0/L1
measurements rather than copying Windows RTX 3090 figures to an untested Linux
stack. No build-only gate qualifies as a playable release: L4 requires a real
headset and a real captured game or video window.

## Main risks and decisions

- **OpenXR runtime/device compatibility:** the runtime-selected Vulkan GPU may
  not match the capture or CUDA GPU. MVP is single GPU; reject an unsupported
  combination with a diagnostic instead of silently crossing devices.
- **Portal variation:** window capture, restore tokens, stream formats and
  frame rates depend on the desktop backend. Test the reference desktop first,
  then expand the matrix. Manual source selection remains the product behavior.
- **Capture to Vulkan transfer:** DMA-BUF modifiers and synchronization may
  prevent zero-copy import. The copy fallback must be correct before optimizing.
- **Shader parity:** room and curve code contains substantial D3D/HLSL work.
  Preserve CPU reference algorithms and port features incrementally.
- **Inference dependencies:** CUDA and ONNX Runtime versions must be validated
  together with the exact ONNX export. Linux AMD/Intel support is a later,
  separately tested backend decision.
- **Feature scope:** the room, glass, reflections and room light are required
  for the first release. Motion estimation, model fusion, multi-GPU selection,
  curved-screen geometry and auto-attach can follow later.

## Implementation order for the first changes

1. Add Linux build skeleton, dependency probe and a headless OpenXR/Vulkan
   synthetic-scene executable. Retain the Windows build unchanged.
2. Extract portable frame/timing, screen geometry and CPU reference code behind
   narrow headers, with reference tests on both platforms.
3. Implement Vulkan preparation and flat stereo warp; compare images to the
   CPU reference before adding inference.
4. Integrate the verified ZipDepth ONNX model through CUDA, then add portal
   capture and its copy fallback.
5. Build the Linux-native controller and profile store, then port the room's
   CPU-backed geometry and EMIT/MIRROR/LIGHT/eye Vulkan passes. Add live room
   controls and test diffuse lighting, glass, reflections and ceiling light in
   the PICO 4. Package and test the complete L4 path.

## API references to verify during implementation

- [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) for the Linux
  loader and Vulkan graphics binding.
- [xdg-desktop-portal ScreenCast](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
  for selection, stream lifetime and restore-token behavior.
- [PipeWire DMA-BUF sharing](https://docs.pipewire.org/devel/page_dma_buf.html)
  for negotiated GPU buffer import and shared-memory fallback.
- [ONNX Runtime execution providers](https://onnxruntime.ai/docs/execution-providers/)
  and [CUDA provider](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html)
  for the supported inference configuration.
- [Avalonia supported platforms](https://docs.avaloniaui.net/docs/supported-platforms)
  if the Linux controller uses Avalonia.
