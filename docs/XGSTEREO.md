# xgstereo: experimental geometry stereo

Branch `xgstereo` adds a second stereo backend. VRX's existing capture → AI depth
→ stereo warp path remains the default, including for old profiles.

The adapted Vulkan mirror sources remain local pending upstream redistribution
permission or an independent replacement. The published native DX12 producer,
shared transport, VRX consumer and desktop coordinator are independently authored.
The desktop's optional Vulkan/DXVK routes require a separately built layer.
See [render path status](RENDER_PATH_STATUS.md) for the remaining completion gates.

## GPU sharing and same-frame status

The launchers now default to `-Transport gpu`. The Vulkan layer writes into a
ring of three shared D3D12 array textures, with one full-resolution slice per
eye. VRX opens those textures on the same adapter and copies their GPU pixels
into its source/eye textures. Pixel readback and CPU upload are absent from this
path. `-Transport cpu` retains the older diagnostic reference transport.

System Shock through DXVK has passed the actual VRX GPU-copy check at
**2560x1440 per eye**, without OpenXR or AI inference. This establishes texture
transport and pixel preservation, not a measured FPS improvement or comfort.

**App-unaware same-frame rendering is now implemented and tested with System
Shock through DXVK.** Select `-Stereo mirror` in `launch-dx11.ps1` (or `-Eye
mirror` for a native Vulkan application). The existing alternate mode remains
available. Mirror mode runs one game simulation and duplicates its Vulkan
commands, allocations, buffers, images, views, descriptors, pipelines and sync
objects on the same device. The right eye has separate render targets and GPU
history. Both views are exported in one complete GPU pair at each presentation;
there is no alternating game-frame pairing or half-height packing.

Actual gameplay on 2026-10-20 passed VRX's offline GPU check at **2560x1440 per
eye, frames 12672/12672**. A separate diagnostic captured distinct, populated
left/right gameplay images at 12600/12600. This confirms rendering and transport;
a subsequent SteamVR headset test reached VISIBLE/FOCUSED, logged roughly
116 drawn frames/s during the initial active interval, and the user reported
**clear stereo and comfortable**. This is one tested gameplay scene, not a
full-game effect or sustained performance guarantee. System Shock was left
running at the user's request. The initial headset test used a five-minute session; the recovery run stays open until stopped.

The mirror generator and manual resource/command translation are adapted from
vrdual; provenance and the outstanding upstream redistribution-license question
are recorded in [MIRROR_ORIGIN.md](../bench/native/xgstereo/MIRROR_ORIGIN.md).
This is a local prototype. VRX's SPIR-V clip rewrite and GPU transport remain
separate VRX implementations. `same_frame.h` also retains the cooperating-renderer
API and moving-snapshot fixture.

To start the tested System Shock path:

```powershell
./bench/native/xgstereo/launch-dx11.ps1 `
  -Executable 'E:\SteamLibrary\steamapps\common\System Shock Remake\SystemShock\Binaries\Win64\SystemReShock-Win64-Shipping.exe' `
  -DxvkDirectory './bench/native/xgstereo/out/dxvk-2.7.1/x64' `
  -SteamAppId 482400 -Stereo mirror -Shift 0.8 -Convergence 0.002
```

Keep the launcher alive for scoped DLL cleanup. `-Stereo alternate` selects the
older alternate-frame backend. Mirror mode requires `-Transport gpu`.

A later black-screen report exposed a publication bug: a transient consumer
metadata-mutex timeout was treated as a fatal exporter error. The publisher now
distinguishes `Busy` from invalid metadata. Busy publication drops only the
reserved pair; its GPU fence still protects the texture until copies finish,
and the stream continues. A regression holds the mutex after a real reservation,
observes dropped publications, releases it, and verifies subsequent complete
stereo pairs. The game and VRX were restarted with that fix. The replacement
headset run has no automatic timeout and logged sustained drawn frames beyond
the earlier failure point.

## Same-frame mirror implementation

`mirror_backend.h` uses generated Vulkan dispatch/translation wrappers from the
pinned `vk.xml`. App handles always refer to the left resources. Deep translations
replace nested handles in right-eye calls. Swapchain twins are ordinary offscreen
images, with PRESENT layout translated to GENERAL on the right. Acquire signals
separate right-eye synchronization; presentation consumes both eyes' completion
semaphores before the shared-texture copies and the monitor present. The monitor
shows the left eye continuously. Unsupported commands or translations stop the
stereo stream and retain application forwarding.

CPU-written host mappings are synchronized before each submission. Non-cached
upload mappings use Win32 write-watch shadows: only dirty pages need comparison
and copying into both allocations. Cached mappings retain the application's real
left pointer for GPU readbacks. When the application observes fence or timeline
completion, the corresponding right work must also finish before it can reuse
inputs. The gameplay run logged approximately 0.5 ms of memory synchronization
per submit after write tracking, compared with 17–20 ms for full-allocation scans.
These are diagnostic averages, not an FPS benchmark; rendering twice still costs
GPU time and duplicates resource memory.

DXVK 2.7.1 requires buffer addresses even with descriptor buffers disabled.
The backend records original/twin buffer address ranges and reflects typed
physical pointers in SPIR-V push-constant blocks. Only those declared fields are
translated, preserving buffer offsets and leaving ordinary constants untouched.
Graphics and compute bindings have independent reflection state. Unknown pointers,
partial pointer writes and integer-computed pointers stop stereo instead of
reading the wrong eye's allocation. Opaque descriptor-buffer replay is unsupported;
the scoped DXVK config sets `dxvk.enableDescriptorBuffer = False` and disables
pipeline libraries and NV low-latency integration for this test path.

Mirror separation and convergence are currently read **at launch**. Use the VRX
stereo switch / `--no-warp` for immediate identical eye images; restart the game
to change mirror separation or convergence. `tune.ps1` rejects a mirror run rather
than claiming a live update happened. Live tuning remains supported in alternate
mode. Swapchain recreation, protected resources, external application resources,
shader objects, mesh/tessellation/geometry stereo and arbitrary pointer data-flow
are outside this first mirror implementation.

## Current implementation

An explicit Windows x64 Vulkan layer, `VK_LAYER_VRX_xgstereo`, patches supported
vertex Position outputs with an eye-dependent clip-X offset. Perspective division
makes nearby geometry shift more than distant geometry. Direct BuiltIn Position
outputs and Position members of output blocks work. W exactly 1 is skipped as a
basic HUD/fullscreen-pass heuristic. The clip rewrite is original VRX code; the mirror backend includes the attributed
vrdual adaptations described above.

Both ordinary shader modules and inline vertex modules passed to
`vkCreateGraphicsPipelines` are patched. Inline stage chains are copied without
changing the application's structures. Shader-module identifiers are computed
from the same patched code, so identifier-based cache lookups stay consistent.
The DLL statically links its C++ runtime: a game's older app-local `msvcp140.dll`
must not supply the mutex implementation used by the layer. Release builds
include a separate PDB for diagnosing native crashes.

`VRX_XG_EYE=alternate` runs one game process on one graphics queue. The shader
loads the current offset through a Vulkan buffer device address. Before each
app submission, a separate control submission orders the offset update on the
GPU; application submit arrays, wait semaphores and pNext chains are forwarded
unchanged. Both legacy submission and core/KHR synchronization2 are intercepted.
The eye flips after each successful presentation, so a cached command buffer
still renders the appropriate eye. Left and right are consecutive game frames,
not two views of precisely the same simulation time.

With GPU transport, presentation schedules a blit into a shared GPU array texture
and returns without a host fence wait. UNORM BGRA8/RGBA8 are supported; sRGB is
currently declined to avoid an unintended gamma conversion. The CPU diagnostic
transport instead copies into a host-visible buffer and exports complete RGBA
pairs through bounded shared memory (including sRGB byte formats).
The copy consumes the app's present semaphores; the original present waits on
a replacement semaphore, allocated per swapchain image. Readback waits are
bounded. BGRA8 and RGBA8, UNORM and sRGB, are supported. The monitor alternates
eyes and can flicker. Each eye updates at half the game's presentation rate,
in either case. GPU sharing removes readback overhead but does not remove the
temporal difference between the alternating game views.

The layer requires Vulkan 1.2+, buffer device addresses and exactly one enabled,
unprotected graphics queue. Additional non-graphics queues pass through. It
shallow-copies the device-create chain before enabling the address feature;
application-owned feature flags and links are not modified. Unsupported device
chains, the older EXT address feature, or multiple graphics queues decline
activation and retain ordinary rendering. No capabilities are hidden. GPU sharing
also requires timeline semaphores, external-memory/semaphore Win32 extensions,
and a valid single-node adapter LUID; only copied feature/extension structures
are changed to enable those capabilities.

The exporter supports one swapchain, one layer, even per-eye dimensions of
2..4096 with GPU transport (2..2048 for the CPU reference), and transfer-source
support. Unsupported
formats, protected swapchains, another present queue, multiple swapchains,
recreation or presentation failure stop export. Subsequent control submissions
restore the shader offset to zero. Stop VR, then restart the game after resizing or otherwise
recreating its swapchain. This is deliberately a narrow first implementation.

VRX accepts the stream with `--geometry-session=NAME`. It bypasses model loading,
AI inference and stereo warping, copies each eye into the existing picture
array, and uses the screen/room/OpenXR presentation path. The left eye supplies
ambilight, room lighting and reflection inputs. `--no-warp` or turning stereo
off duplicates the left eye. An optional `--geometry-pid=PID` binds the stream
to the selected game process; desktop attachment supplies this automatically.

Expert mode now has **Geometry stereo — experimental Vulkan** in **3D & depth**,
with a saved per-game session name. It applies at the next Attach / Play. The
unused AI controls, strength slider and desktop switching are disabled while
selected; their saved values are retained. The game must be launched with the
layer first: attaching VRX does not load a layer into an already running game.
Whole-display sources do not support this mode.

## Build

Requires Windows x64, CMake 3.24+, Visual Studio C++ tools, PowerShell 7 and
Python 3. CMake fetches Khronos Vulkan headers, SPIR-V headers and SPIRV-Tools
at pinned commits. SPIRV-Tools is linked only into the shader test. The generated
device-chain sizes come from the pinned Vulkan registry. A Vulkan SDK is not
required. GPU tests require a Vulkan graphics driver; submission tests use
Vulkan 1.3 and the KHR synchronization2 extension on the test machine.

From the VRX root:

```powershell
cmake -S bench/native/xgstereo -B bench/native/xgstereo/out -A x64
cmake --build bench/native/xgstereo/out --config Release --target xgstereo_checks --parallel 4
cmd /c 'bench\native\openxr\build.bat --desktop'
$env:LIB = ''
dotnet build desktop/VRX.Desktop/VRX.Desktop.csproj -c Release
cmake -S bench/native/xgstereo -B bench/native/xgstereo/out -A x64 "-DVRX_XG_ENGINE=$PWD/bench/native/openxr/out/xrplayer.exe"
ctest --test-dir bench/native/xgstereo/out -C Release --output-on-failure
```

`VRX_XG_ENGINE` enables the cross-process engine handoff tests. If the engine
was built into a separate directory with `VRX_NATIVE_OUT`, point this option at
that directory's `xrplayer.exe`. The desktop normally uses the engine under
`bench/native/openxr/out`, so rebuild that location to try the UI in a development
checkout. The native Vulkan launcher makes no layer registration, driver
installation or game-directory changes. The optional DXVK bridge described below
temporarily adds two DLLs beside the game executable. Packaging these experimental
tools into a release is not implemented.

For a layer-only build without SPIRV-Tools, configure with
`-DVRX_XG_VALIDATE_SPIRV=OFF` and build target `vrx_xgstereo`. Re-enable it for
full shader validation.

## Try a compatible application

Close the game first. From PowerShell, launch a **native Vulkan x64** application
that permits graphics layers:

```powershell
./bench/native/xgstereo/launch.ps1 -Executable 'C:\Games\Example\game.exe' -Eye alternate -Session game
```

In VRX, select that game's window, switch to Expert, enable **Geometry stereo**,
leave the session as `game`, and click **Attach / Play**. Use the same session
name in the launcher and VRX. The selected process must own the stream. Changing
the backend or session while playing requires Stop VR and Attach / Play again.
The ordinary stereo on/off, screen, curve, room and recenter controls remain
available. The game must continue presenting frames while VRX is in front.

`-GameArguments @('argument one', 'argument two')` passes literal arguments.
`-Preview` shows launch settings without starting anything. Only the child and
its inheriting descendants receive layer environment variables; the parent
process environment is unchanged. `-Eye left` and `-Eye right` retain the
fixed-eye diagnostic mode and do not export a stream.

| Setting | Meaning |
|---|---|
| `VRX_XG_EYE` | `left`, `right`, `alternate`, or `mirror`; absent/invalid means unchanged shaders |
| `VRX_XG_SESSION` | Session name for geometry export; letters, digits, `_` and `-`, 1..64 characters |
| `VRX_XG_TRANSPORT` | `gpu` for shared array textures/fences; `cpu` or absent for the reference readback transport; launchers default to `gpu` |
| `VRX_XG_SHIFT` | Clip-space offset magnitude, 0..64; default 0.032, positive for alternate/mirror modes |

The left eye adds the offset, the right subtracts it. This is not a calibrated
IPD: for an ordinary perspective projection the magnitude is
`P00 × IPD × worldUnitsPerMetre / 2`, where `P00 = 1 / tan(horizontalFov / 2)`.
Use the launcher's `-Shift` option and restart the game to change it. Cinema-screen
convergence and game-specific FOV/world scale need further calibration.

The default assumes metre-scale world units. Unreal games commonly use centimetres:
with a roughly 90-degree horizontal FOV, `-Shift 3.2` corresponds to about a
6.4 cm eye separation. However, that setting proved uncomfortable on the virtual
screen in System Shock. Use a smaller initial value and tune for the screen:
world-scale IPD alone does not establish comfortable cinema-screen convergence.

The layer runs inside the game and modifies shaders. Anti-cheat compatibility
has not been established. Use offline applications or titles whose publisher
permits this integration.

## System Shock remake: DirectX 11 through DXVK

The installed Steam build (app 482400, build 22239093) cannot run its native
Vulkan renderer: `-vulkan -stdout -FullStdOutLogOutput` reports a missing
`Engine/GlobalShaderCache-SF_VULKAN_SM5.bin` and exits, including without VRX.
The original access violation was a separate layer bug: System Shock's app-local
MSVC 14.24 runtime crashed on the first mutex lock made by our newer C++ headers.
Statically linking the layer's runtime fixes that startup failure without
replacing the game's runtime DLLs.

The working experimental route is System Shock's cooked D3D11 renderer through
[DXVK](https://github.com/doitsujin/dxvk). Tested here with the official
[DXVK 2.7.1 release](https://github.com/doitsujin/dxvk/releases/tag/v2.7.1), archive
SHA-256 `d85ce7c79f57ecd765aaa1b9e7007cb875e6fde9f6d331df799bce73d513ce87`.
DXVK is downloaded separately and is not bundled with VRX. Upstream describes
[Windows use as unofficial](https://github.com/doitsujin/dxvk/wiki/Windows).

With the game closed, extract the official archive under `bench/native/xgstereo/out`
and run from the VRX root (substitute the installed game path):

```powershell
./bench/native/xgstereo/launch-dx11.ps1 `
  -Executable 'E:\SteamLibrary\steamapps\common\System Shock Remake\SystemShock\Binaries\Win64\SystemReShock-Win64-Shipping.exe' `
  -DxvkDirectory './bench/native/xgstereo/out/dxvk-2.7.1/x64' `
  -SteamAppId 482400 -Session game -Shift 0.8 -Convergence 0.002
```

The launcher now uses `-dx11 -windowed -ResX=2560 -ResY=1440` with GPU sharing.
For the older CPU reference, pass `-Transport cpu` and suitable `-GameArguments`
(for example 1920x1080). It temporarily installs DXVK's `d3d11.dll` and `dxgi.dll`,
refuses to overwrite existing files, and removes its unchanged DLLs after the game
exits. Keep the launcher running. If it is forcibly terminated, its
`out/dx11-*/installed.json` records paths and hashes for recovery; remove only
the matching files after closing the game. Per-run DXVK/layer logs are also
stored there. `-Preview` shows the plan without installing or starting anything.
The per-run config disables graphics pipeline libraries, descriptor buffers and
NV low-latency integration; inline module handling remains necessary.

The DX11 launcher also creates `stereo.txt` and sets `VRX_XG_TUNING` for its child.
In **alternate mode**, adjust separation/convergence without restarting:

```powershell
./bench/native/xgstereo/tune.ps1 -Session game -Shift 0.8 -Convergence 0.002
# If separation is uncomfortable, immediately return to identical eye images:
./bench/native/xgstereo/tune.ps1 -Session game -Mono
```

The helper finds the live run using its PID, process start time and executable
path; it refuses ambiguous or expired sessions. `-RunDirectory` selects a run
explicitly. It atomically replaces the two-number control file. The layer polls
at most five times per second and applies both values at a new left-eye frame,
so an exported pair never mixes different tuning settings. Invalid, partial,
non-finite or out-of-range input retains the previous values. The shader uses
`x += signedShift - signedConvergence * w`, while retaining the W=1 HUD heuristic.
For ordinary perspective projection the zero-parallax clip W is
`Shift / Convergence`; with this game's centimetre scale, 0.8/0.002 places it
roughly four metres away. Zero convergence keeps the zero-parallax plane at
infinity. This does not move a real game camera or correct screen-space effects.

In VRX, select the game's actual window and enable Geometry stereo/session
`game`, then Attach / Play. SteamVR must be running for headset presentation.
Startup clears and unsupported shaders leave the transport empty until a
supported vertex shader appears; an initial loading screen no longer permanently
disables export. Avoid resizing or changing fullscreen mode during this test:
presentation failure or swapchain recreation requires stopping VRX and restarting
the game. Close VRX's stream before relaunching the game with the same session.

Verified on 2026-10-20: game startup through DXVK, supported inline shaders patched,
1920x1080 per-eye pairs delivered into VRX's D3D12 eye textures, a focused SteamVR
OpenXR session, and gameplay snapshots with visible left/right separation.
The earlier default shift 0.032 appeared flat in gameplay; 3.2 produced double
images/discomfort. Live tuning was added and the current trial uses 0.8 with
convergence 0.002, but the user still reported queasiness and reduced apparent
resolution. The later GPU-sharing test passed at 2560x1440 per eye without a
headset session. The subsequent mirror backend passed actual gameplay with equal
frame IDs at the same resolution, as recorded above. The later same-frame headset trial was reported clear and comfortable.
Convergence, sustained motion and full-game rendering quality still need evaluation; neither successful transport nor distinct eye
images alone establishes comfortable stereo.

## Controlled demos and verification

The native suite includes app-unaware mirror checks for cached commands,
persistently mapped moving inputs, inline shaders, independent GPU compute
writes through typed physical pointers, and the real VRX engine handoff.

The native suite covers:

- SPIR-V validation of baked and live-eye patches, direct/block Position forms,
  shader isolation, HUD handling and malformed input.
- Device-chain copying, preservation of existing features and unknown-node rejection.
- Cross-process pair delivery, resize metadata, concurrent access, invalid lengths,
  stale frames, closure and abandoned mutexes.
- D3D12 eye-slice copies, row pitch, edge pixels, mono toggling, source reuse and
  left-eye lighting. D3D12 debug-layer errors fail the check when it is installed.
- Fixed-eye offscreen geometry and real alternating-eye Win32 swapchain rendering.
  The latter uses a single process and device, replays the same recorded draw
  commands and checks legacy/core/KHR submissions. Near/far/HUD shifts are checked
  numerically, including BGRA conversion and consecutive frame identities.
- An unsupported two-graphics-queue configuration keeps ordinary mono rendering
  and presentation, without creating a geometry stream.
- Startup/unsupported vertex modules leave the stream empty rather than exporting mono as stereo.
- Inline vertex modules preserve application-owned stage/code structures, produce
  alternating depth shifts, and generate the same shader identifier as a patched
  ordinary module.
- Live separation/convergence changes alter cached draws at pair boundaries;
  malformed updates retain the previous values and zero/zero produces mono.
- Runtime import isolation and Vulkan instance startup, optionally with a
  preloaded legacy app-local MSVC runtime (`-DVRX_XG_LEGACY_RUNTIME=.../msvcp140.dll`).
- DX11 launcher preview, cleanup on child exit and preservation of existing game DLLs.
- GPU lease/fence completion, nonblocking full-ring handling, duplicate producer,
  stale/resize/adapter rejection and producer closure.
- Vulkan swapchain -> shared GPU array -> D3D12 consumer, including BGRA channel
  conversion and actual left/right parallax.
- Cooperative same-frame rendering of a moving snapshot, equal frame IDs and
  matching simulation positions in the actual eye pixels.
- Child-only launcher settings, session selection, literal argument handling and exit codes.
- Two Vulkan → shared memory → actual VRX source-ring upload → D3D12 handoff tests:
  the static reference producer and the alternating swapchain exporter, each
  checked pixel-for-pixel in stereo and mono, with producer PID binding and
  rejection of a different producer's stream.

The existing desktop `--smoke-test` also checks the geometry controls, persistence,
old-profile defaults, invalid session names, engine arguments and localized UI.
Its screenshots include `desktop/out/ui-geometry-stereo.png`.

To serve the real alternating presentation fixture for 120 seconds, run in one
PowerShell window:

```powershell
$env:VK_LAYER_PATH = "$PWD\bench\native\xgstereo\out\Release"
./bench/native/xgstereo/out/Release/present_test.exe --serve=demo
```

In another window:

```powershell
# Verify upload/copy without starting OpenXR or a model:
./bench/native/openxr/out/xrplayer.exe --geometry-session=demo --check-geometry
# Or request headset presentation:
./bench/native/openxr/out/xrplayer.exe 60 --geometry-session=demo --room=30 --curve=25 --ambilight
```

`render_test.exe --serve=demo` is the older static reference producer. Both demos
use a deterministic scene, not a real game. Tests do not start headset sessions.

For the GPU alternating fixture, use `present_test.exe --serve-gpu=demo` instead
of `--serve=demo`. For the cooperative same-frame fixture:

```powershell
$env:VK_LAYER_PATH = "$PWD\bench\native\xgstereo\out\Release"
./bench/native/xgstereo/out/Release/same_frame_test.exe --serve-gpu=demo
```

The same `xrplayer --geometry-session=demo --check-geometry` command detects the
GPU transport and logs `same-frame eyes` or `alternating-frame eyes`. The fixture
uses CPU readback/waits to check expected pixels and manage its test resources;
it is not a performance benchmark. The production shared-texture producer API
does not perform host image readback or wait for the copy to finish.
Windows/NVIDIA RTX 3090 execution, SPIR-V validation and the offline engine
handoffs are verified. System Shock testing is described above. A Khronos Vulkan
validation-layer run and broad game/headset quality evaluation remain outstanding.

## Transport and remaining limitations

The CPU reference transport version 1 uses a local-session named mapping and mutex, one producer
per name. Every publication contains a complete RGBA SBS pair, sequence,
per-eye frame IDs and monotonic timestamp. Pixel storage is bounded at 32 MiB
plus a fixed header. A busy consumer causes a whole-pair drop. VRX hides invalid,
closed or more-than-one-second-old streams. An abandoned mutex invalidates its
reader. The transport accepts dimension changes; the renderer rejects dimensions
other than its startup size, and the current swapchain exporter stops on recreation.
No mismatched eyes are combined silently.

The separate GPU metadata ABI (`GpuHeader`, version 1, 176 bytes) uses
`Local\\VRX.xgstereo.<session>.gpu.frames` and a mutex. Texture/ready/released
handles include the producer PID and generation. Only one consumer may attach;
the adapter LUID, dimensions, format, frame kind, freshness and producer PID are
validated. A reader claims a completed slot under the metadata mutex; the writer
cannot reuse it until the consumer's GPU release fence completes. Rotation keeps
other complete pairs available while the next slot is being written. A full ring
drops the entire incoming pair instead of waiting or overwriting a leased image.
Consumer death cannot release an unfinished GPU lease; restart the session after
an interrupted consumer. Texture ownership transfers use external queue-family
barriers and D3D12 shared timeline fences. See the primary API contracts for
[external memory](https://docs.vulkan.org/guide/latest/extensions/external.html),
[D3D12-resource import allocation sizes](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryAllocateInfo.html),
and [D3D12 shared handles](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle).

W=1 is a heuristic: some geometry has W=1 and some HUDs do not. Shifting Position
does not update camera uniforms, so shadows, reflections, screen-space effects,
TAA and motion vectors can disagree with the geometry. Alternating eyes adds
one game frame of temporal difference and can smear fast motion or temporal effects.
Unsupported vertex modules pass through unchanged; if no supported vertex
module was observed, the stream stays empty. Shader objects and
geometry/tessellation/mesh stages need additional handling. DirectX needs a
separate translator such as the experimental DXVK bridge above; there is no
native DirectX backend. OpenGL, 32-bit applications and dual-GPU mirroring are
not supported.

### Desktop chooser and managed launch

Each window preview offers Depth estimate, Vulkan and DXVK. DX12 is disabled
because there is no native DirectX 12 stereo backend. Clicking a preview uses
that executable's saved method; displays continue to use depth estimation.
Expert also has a stereo method selector. Applying general settings to all games
preserves their individual launch methods and calibration.

Geometry selection reuses a fresh same-frame GPU stream owned by the selected
process when available. Otherwise a restart dialog shows the executable,
preserved arguments, working folder, detected Steam app ID, separation and
convergence. VRX closes the game normally after the explicit restart action.
Unreal Shipping executables receive the appropriate -vulkan or -dx11 switch.
Other engines retain their arguments for review. Native Vulkan requires the
game's Vulkan assets; System Shock requires DXVK instead.

The coordinator requires a built x64 layer (repository Release output or
geometry/layer beside the desktop executable). DXVK uses the local 2.7.1 cache
or downloads the pinned official archive and verifies its SHA-256. Existing
game DLLs are preserved unless recorded as owned by the matching VRX run.
Only unchanged owned files are removed on game exit; recovery runs on the next
UI start. Run manifests, tuning and logs live under the profile root's
geometry/runs. The renderer starts after the new process publishes a valid
same-frame stream. Auto-attach can reuse a ready stream but never restarts games.

Desktop build and smoke checks cover path persistence, chooser selection,
launch preflight, literal arguments, Steam detection, stream ownership and
scoped file cleanup. The opt-in `--smoke-test --geometry-launch-integration-test`
extension passed two consecutive managed DXVK restarts of an ordinary D3D11
fixture. It checked distinct same-frame eyes at 320x241 per eye, ready-stream
reuse, actual VRX GPU consumption, and cleanup after normal exit. An isolated
renderer build can be selected with `VRX_GEOMETRY_TEST_ENGINE`.
Earlier System Shock headset validation used the scoped PowerShell launcher;
the managed launcher still needs a System Shock gameplay run.

GPU transport now accepts odd per-eye extents; a window's client area need not
be even. Native D3D12 tests cover 97x65 as well as 2560x1440. The reference CPU
transport retains its original even-size restriction. Managed file cleanup is
serialized per executable and retires old manifests, including installation
rollback, so a delayed observer cannot delete a subsequent run's identical DLLs.

Next work: extend mirror pointer/command coverage and ordered live tuning;
validate more games and performance; calibrate convergence; support swapchain
recreation and more rendering paths; package the optional layer for releases.
