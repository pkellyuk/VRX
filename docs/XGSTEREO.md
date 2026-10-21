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
images, with PRESENT layout translated to GENERAL on the right. The monitor shows
the left eye continuously. Unsupported commands or translations stop the stereo
stream and retain application forwarding.

Synchronisation. The layer submits only from inside the app's own
`vkQueueSubmit*`/`vkQueuePresentKHR` calls, where the app holds the queue's
external synchronisation. Acquisition submits nothing (an earlier version
signalled the right-eye twins of the acquire semaphore/fence with an extra
`vkQueueSubmit` inside `vkAcquireNextImageKHR`, racing the app's own use of that
queue). Right-eye work renders into offscreen twins, so a right-eye wait on an
acquire semaphore is translated into a wait on the per-queue *layer timeline*
value of the export copy that last read that twin (or dropped if the twin was
never exported). Every right-eye submission and every export copy signals its
queue's layer timeline. Swapchain teardown, transport replacement and the
release after a stop wait on these timelines instead of `vkDeviceWaitIdle`
(which would need every queue's external synchronisation).

CPU-written host mappings are synchronized before each submission. Non-cached
upload mappings use Win32 write-watch shadows: only dirty pages need comparison
and copying into both allocations. Cached mappings retain the application's real
left pointer for GPU readbacks. A shadow holds only what the app wrote; when the
app calls `vkInvalidateMappedMemoryRanges` on a shadowed range (its request to
read GPU results) the shadow is refreshed from the left allocation first. Reads
of non-cached coherent memory without an invalidate still see only the app's own
writes; D3D12 upload heaps (vkd3d) cannot be GPU-written and DXVK reads back
through cached memory, so this is documented rather than shadow-free. When the application observes fence or timeline
completion, the corresponding right work must also finish before it can reuse
inputs. The gameplay run logged approximately 0.5 ms of memory synchronization
per submit after write tracking, compared with 17–20 ms for full-allocation scans.
These are diagnostic averages, not an FPS benchmark; rendering twice still costs
GPU time and duplicates resource memory.

DXVK 2.7.1 requires buffer addresses even with descriptor buffers disabled.
The backend records original/twin buffer address ranges in an interval map
(`mirror_addresses.h`: ordered map with binary search under a shared lock, not a
linear scan under a global mutex) and reflects typed physical pointers in
SPIR-V push-constant blocks. Only those declared fields are translated,
preserving buffer offsets and leaving ordinary constants untouched. Push data
persists across pipeline binds, so `vkCmdPushConstants` only records the raw
bytes per command buffer; the right eye receives them at the next draw or
dispatch, translated for the pipeline actually bound then, pushed with the
latest push layout and stage flags split per overlapping range. Unchanged state
costs one lookup per draw. Unknown pointers, partial pointer writes and
integer-computed pointers stop stereo instead of reading the wrong eye's
allocation. Opaque descriptor-buffer replay is unsupported;
the scoped DXVK config sets `dxvk.enableDescriptorBuffer = False` and disables
pipeline libraries and NV low-latency integration for this test path.

Mirror separation and convergence are read at launch **and applied live**: the
layer polls the tuning file (`VRX_XG_TUNING`, at most five times a second) and
writes both eyes' values with one GPU-ordered transfer between frames, so cached
command buffers pick them up and a pair never mixes settings (`tune.ps1` works for
mirror runs; regression `mirror_live_stereo_tuning`). The VRX stereo switch /
`--no-warp` still gives identical eye images immediately.

Swapchain recreation through `oldSwapchain` (resize, mode change) is supported:
the old swapchain is retired (its images present without export) and a new
transport generation starts at the new size. A second, simultaneous swapchain
still stops stereo. Protected resources, shader objects, mesh/task shaders and
arbitrary pointer data-flow are outside this mirror implementation.

Stop and fallback. When stereo stops (any unsupported command, translation,
extension use or failure), the left eye's shaders remain patched, so at the next
submission on the graphics queue the layer writes {0, 0} into both eye offset
buffers with a GPU-ordered transfer: the game renders unshifted (mono), including
cached command buffers (regression `mirror_stop_fallback`). Right-eye work is
then drained on the layer timelines and right-eye memory, swapchain twins and
the shared transport are released; later app destroys still destroy the
remaining right-eye objects (generated destroy wrappers), shadows of unmapped
memory are freed and new maps return the app's own pointer, so VRAM and CPU
cost return to normal.

Device support. At `vkCreateDevice` every enabled extension is checked against
an allowlist: all non-platform, non-provisional device extensions of the pinned
registry (generated by `gen_mirror.py`; their commands are wrapped, left-eye
only, hand-written or stopping stubs) plus reviewed Windows extensions
(`VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32` (imports stop
stereo), `VK_KHR_external_fence_win32`, `VK_KHR_win32_keyed_mutex`,
`VK_NV_win32_keyed_mutex`, `VK_NV_external_memory_win32`,
`VK_EXT_full_screen_exclusive`). Any other extension, or an API version newer
than the registry, keeps ordinary rendering for that device and logs the name.

## Current implementation

An explicit Windows x64 Vulkan layer, `VK_LAYER_VRX_xgstereo`, patches supported
Position outputs with an eye-dependent clip-X offset. Perspective division
makes nearby geometry shift more than distant geometry. Direct BuiltIn Position
outputs and Position members of output blocks work. The rewrite is

```
offset = clamp(shift - convergence * w, -0.25 * |w|, +0.25 * |w|)
x      = |w - 1| <= 1e-4 ? x : x + offset
```

i.e. the per-eye screen (NDC) shift `shift / w - convergence` is clamped to
±0.25 (one eighth of the eye's width; equal to the largest accepted convergence,
so far-field shifts are never clipped), and W within 1e-4 of 1 is treated as a
HUD/full-screen pass (an exact `w == 1` test missed values like 0.99999994).
The same limits apply to the DXBC rewrite (`dx12stereo/dxbc_patch.cpp`).

In mirror mode the shift is applied in the *last pre-rasterisation stage*:
geometry if present, otherwise tessellation evaluation, otherwise vertex
(geometry shaders are rewritten before every vertex emission). Earlier patched
stages of such a pipeline get their original code, so Position is never shifted
twice. If the last stage cannot be patched the vertex shift is kept (correct
only when Position passes through) and logged; mesh/task pipelines are not
shifted and are logged as flat. Fixed/alternate modes shift the vertex stage only
and log pipelines with later stages. The clip rewrite is original VRX code; the mirror backend includes the attributed
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

With GPU transport, presentation schedules a transfer into a shared GPU array
texture and returns without a host fence wait. The shared textures are RGBA8
UNORM holding display-referred sRGB-encoded bytes. In mirror mode UNORM BGRA8/
RGBA8 and 10-bit UNORM (A2B10G10R10/A2R10G10B10) swapchains are blitted (format
and channel conversion only); RGBA8 sRGB is copied byte-for-byte and BGRA8 sRGB
is copied into a BGRA8 UNORM staging image and then blitted, so no path decodes
gamma. Only `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` is exported: HDR/wide-gamut
colour spaces and float formats stop stereo with a logged reason. Alternate mode
with GPU transport accepts only UNORM BGRA8/RGBA8 in that colour space; sRGB is
declined there. The CPU diagnostic
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

The exporter supports one swapchain, one layer, per-eye dimensions of 2..4096
with GPU transport (even 2..2048 for the CPU reference), and transfer-source
support. Unsupported formats, protected swapchains, another present queue,
multiple simultaneous swapchains or presentation failure stop export; both modes
then restore the shader offset to zero. Mirror mode follows swapchain recreation
(new transport generation); alternate mode still requires a game restart after
resizing or recreating its swapchain.

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

Release layer (what packaging ships) — Release configuration, diagnostics off:

```powershell
cmake -S bench/native/xgstereo -B bench/native/xgstereo/out -A x64 -DVRX_XG_VALIDATE_SPIRV=OFF -DVRX_XG_DIAGNOSTICS=OFF
cmake --build bench/native/xgstereo/out --config Release --target vrx_xgstereo
```

Outputs: `bench/native/xgstereo/out/Release/vrx_xgstereo.dll`, its manifest
`VkLayer_vrx_xgstereo.json` (relative `library_path`, keep both together) and
`vrx_xgstereo.pdb` (symbols, not needed at run time).

Diagnostic switches (`VRX_XG_MIRROR_TRACE`, `VRX_XG_MIRROR_FAIL_AT`,
`VRX_XG_SHADOW`, `VRX_XG_TRANSIENT_MAPS`) are compiled only into Debug builds or
with `-DVRX_XG_DIAGNOSTICS=ON`; a release layer ignores those variables. The
DX12 project likewise compiles its replay diagnostics (`VRX_DX12_RECORD_ONLY`,
`CLONE_ONLY`, `SKIP_WORK`, `SKIP_COMMANDS`, `STATE_ONLY`, `RECORD_COMMANDS`,
`REPLAY_PROBES`, pixel traces) only in Debug or with `-DVRX_DX12_DIAGNOSTICS=ON`
(`VRX_DX12_TEST_*` are read by the test fixtures only), and builds the native
proxy (`d3d12.dll` + MinHook) only with `-DVRX_DX12_BUILD_INTERCEPTOR=ON`.

Pipeline caches. Patched shaders embed the eye buffer's device address as a
constant, which changes between runs, so driver and DXVK/vkd3d pipeline caches
miss for patched pre-rasterisation shaders on every launch. A stable mechanism
was evaluated and not adopted: an appended push-constant range or a reserved
descriptor set must be added to every pipeline layout and rebound after every
app bind, which collides with vkd3d's root-constant push ranges (up to the
device limit) and DXVK's descriptor layouts; a fixed device address needs
capture/replay addresses that cannot be chosen freely; a specialization
constant still changes the compiled code. The address constant is therefore
kept. Right-eye pipelines compile synchronously inside the app's creation call;
their cost is measured and logged (calls over 100 ms, then a summary every 500
pipelines, and the total at device close).

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
Set it with the launcher's `-Shift` option; DX11/managed runs can also change it live (`tune.ps1`, desktop). Cinema-screen
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
In **alternate and mirror modes**, adjust separation/convergence without restarting:

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
`Shift / Convergence`, subject to the ±0.25 NDC clamp and the HUD tolerance
described above; with this game's centimetre scale, 0.8/0.002 places it
roughly four metres away. Zero convergence keeps the zero-parallax plane at
infinity. This does not move a real game camera or correct screen-space effects.

In VRX, select the game's actual window and enable Geometry stereo/session
`game`, then Attach / Play. SteamVR must be running for headset presentation.
Startup clears and unsupported shaders leave the transport empty until a
supported vertex shader appears; an initial loading screen no longer permanently
disables export. In mirror mode a resize or fullscreen change recreates the
transport at the new size; in alternate mode it requires stopping VRX and
restarting the game. Close VRX's stream before relaunching the game with the same session.

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
reader. The CPU reference renderer rejects dimensions other than its startup size.
No mismatched eyes are combined silently.

The shared GPU consumer supports replacement generations from the same game PID
and adapter. It retires old queue work and source references, opens the new
textures and resumes in the same VRX process. Resized eyes are fitted into the
stable headset screen on the GPU, with aspect bars and filtering constrained to
each eye. An unchanged size retains pixel-exact copies. This follows the normal
window-capture behaviour; a game resize does not move or resize the headset screen.
Native DX12 fixtures verify two consecutive real swapchain resizes in both
stereo and mono modes. Vulkan/DXVK exporter recreation coverage is still required.

The separate GPU metadata ABI (`GpuHeader`, version 1, 176 bytes) uses
`Local\\VRX.xgstereo.<session>.gpu.frames` and a mutex. Texture/ready/released
handles include the producer PID and generation. Only one consumer may attach;
the adapter LUID, dimensions, format, frame kind, freshness and producer PID are
validated. A reader claims a completed slot under the metadata mutex; the writer
cannot reuse it until the consumer's GPU release fence completes. Rotation keeps
other complete pairs available while the next slot is being written. A full ring
drops the entire incoming pair instead of waiting or overwriting a leased image.

The consumer's PID and process creation time are stored in a 16-byte trailer
after the unchanged 176-byte v1 header (`GpuConsumerLease`; readers of the v1
header, including the desktop, are unaffected; mapping created by an older
producer reads as "start time unknown"). A registered consumer whose process has
exited (or whose PID now belongs to another process) no longer blocks `Open`, and
the producer revokes its leases (probed at most every 250 ms, only while a slot
is blocked) instead of running with fewer slots. When a consumer is *terminated*
Windows completes the shared fences with `UINT64_MAX`; the producer then reports
the generation lost and mirror mode publishes a replacement generation after
waiting for its own copies, so a restarted engine re-attaches without
restarting the game (`WaitForGpuPair` waits for it). Alternate mode stops with a
logged reason in that case. Regression: `gpu_transport_leases` (exited and
killed consumer processes). Texture ownership transfers use external queue-family
barriers and D3D12 shared timeline fences. See the primary API contracts for
[external memory](https://docs.vulkan.org/guide/latest/extensions/external.html),
[D3D12-resource import allocation sizes](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryAllocateInfo.html),
and [D3D12 shared handles](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle).

W≈1 is a heuristic: some geometry has W near 1 and some HUDs do not. Shifting Position
does not update camera uniforms, so shadows, reflections, screen-space effects,
TAA and motion vectors can disagree with the geometry. Alternating eyes adds
one game frame of temporal difference and can smear fast motion or temporal effects.
Unsupported vertex modules pass through unchanged; if no supported vertex
module was observed, the stream stays empty. Shader objects and mesh/task stages
are not shifted (logged); tessellation/geometry stages are shifted in mirror mode
only. DirectX needs a separate translator such as the experimental DXVK bridge
above or vkd3d-proton; the native DirectX 12 proxy (`dx12stereo`) is an
experiment that is not built or shipped by default. OpenGL, 32-bit applications and dual-GPU mirroring are
not supported.

### Desktop chooser and managed launch

Each window preview offers Depth estimate, Vulkan, DXVK and DirectX 12 →
Vulkan through vkd3d-proton (a locally supplied vkd3d-proton build in
`geometry/vkd3d` or `VRX_VKD3D_DIRECTORY`; see
[VKD3D.md](../bench/native/dx12stereo/VKD3D.md)). The native DX12 proxy route is
not offered. Clicking a preview uses
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

Next work: extend mirror pointer/command coverage; validate more games and
performance; calibrate convergence; more rendering paths (mesh shaders, shader
objects); package the optional layer for releases.
