# Native DirectX 12 stereo backend

This directory is the independently authored native D3D12 foundation. It does
not load Vulkan or DXVK. `Publisher` accepts two eye resources from the same
simulation frame and submits GPU copies on their rendering queue. The shared
transport is also used by the local experimental Vulkan backend.

**Status:** GPU transport, native command/resource replay, swapchain publication
and a GPU-tested shader-model-5 vertex/geometry rewrite library are implemented for the
subset described below. DXIL rewriting remains unsupported. Ordinary native
fixtures render one view; the bridge creates the second eye from those commands.
These checks do not establish compatibility with arbitrary games and do not
enable the desktop DX12 button.

Build and test on Windows x64 with the Visual Studio C++ tools and Windows SDK:

```powershell
cmake -S bench/native/dx12stereo -B bench/native/dx12stereo/out -A x64
cmake --build bench/native/dx12stereo/out --config Release
ctest --test-dir bench/native/dx12stereo/out -C Release --output-on-failure
```

The tests draw a triangle into independent eye slices, move its projection in
opposite directions, and encode the simulation frame in its color. They verify
both eyes' pixels after sharing to another D3D12 device, at 96x64 and 2560x1440
per eye, plus odd 97x65 extents. Repeated drawing checks restoration of input resource states. A stalled
GPU queue verifies bounded allocation, nonblocking frame drops and recovery.
The D3D12 debug layer is enabled when available; errors fail the tests.

Additional transport tests exercise lease release, consumer GPU completion,
metadata contention, stale/foreign streams, dimensions and atomic CPU pairs.
Only diagnostic tests read image pixels back to the CPU.

`d3d11_launch_fixture.exe` is a separate ordinary D3D11 application for the
managed DXVK launcher integration test. It contains no VRX or Vulkan API calls.
`--self-test` draws eight frames and exits; otherwise it runs until normally
closed, with a two-minute safety limit. The opt-in desktop smoke extension
`--smoke-test --geometry-launch-integration-test` copies it into a unique test
directory, performs two DXVK restarts, checks distinct same-frame eyes and the
VRX GPU consumer, then verifies temporary DLL cleanup. That extension requires
the separately built experimental layer, DXVK cache, GPU snapshot utility and
geometry-capable VRX engine. `VRX_GEOMETRY_TEST_ENGINE` can select an isolated
engine build while another headset session is active.

The producer can serve the local VRX geometry consumer with
`dx12_publisher_test.exe --serve-gpu=SESSION`. With the experimental renderer
built, `xrplayer.exe --geometry-session=SESSION --geometry-pid=PRODUCER_PID
--check-geometry --debug --normal-gpu-priority` checks the cross-process GPU
handoff without starting OpenXR or AI inference.

## Integration contract

Call `Create` with a single-node DIRECT queue and a unique session name. After
submitting both eyes' rendering, serialize `Submit` with other submissions to
that queue. Supply the exact current states of the two subresources. Resources
must belong to that queue's device. The typed SDR formats listed below,
single-sample, one-mip 2D textures or array slices are accepted. Direct publisher
callers must establish the source's SDR interpretation; swapchain interception
tracks colour-space changes. Unsupported formats need
explicit conversion; they must not be reinterpreted as RGBA8.

Input states are restored after the GPU copies. A GPU completion fence protects
each allocator and retained resource reference. Consumer leases prevent reuse
until its GPU finishes. Dropped publications never cause an old eye to be paired
with a new one. The caller owns cross-queue rendering dependencies and must let
the queue drain before destroying the producer; destruction retires pending
copies before releasing their resources.

The shared metadata carries one frame ID for both eye slices. It describes the
producer's contract, not proof that a caller supplied correct simulation data.
The draw and readback checks provide that evidence for the included fixture.

`VRX_VULKAN_LAYER_DIRECTORY` optionally builds the independently authored Vulkan
presentation fixtures against a separately supplied layer. The pinned Vulkan
and SPIR-V headers are fetched only when this option is enabled. These checks
cover GPU presentation, command mirroring and six real metadata-mutex holds
followed by ten verified fresh pairs. The hold count has a deadline rather than
a fixed number of rendered frames, because reservation observation depends on
thread scheduling. The adapted mirror source itself is not included.

## Native interception prototype

The build produces an opt-in app-local `d3d12.dll` under `out/proxy/Release`.
It forwards Windows' D3D12 exports by their names and ordinals, loading the real
runtime by its absolute System32 path. Private and PIX exports use Win64
tail-call forwarders which preserve argument registers and the stack. Nothing
is installed into Windows or an existing game by this build or these tests.

Only a child launched with `VRX_DX12_INTERCEPT=1` installs the hooks. The native
device, graphics command lists and queues stay the original COM objects, so
their identity, QueryInterface behavior and parameters passed to the driver are
preserved. Nested driver/debug-layer calls are forwarded without a second app
event. Hook slots are derived from the installed Microsoft SDK declarations.
The bridge is pinned once hooks are installed; unloading active trampolines is
not supported. It captures legacy and Device4 command-list creation, resources,
graphics pipelines, draws, close/reset and queue submission. The additional
`VRX_DX12_MIRROR=1` switch enables the limited command/resource replay described
below; interception alone preserves native forwarding.

MinHook 1.3.4 is fetched at commit
`c3fcafdc10146beb5919319d0683e44e3c30d537`. Its upstream BSD license, including
the bundled disassembler notices, is copied beside the DLL as
`MinHook-LICENSE.txt` and must accompany binary distributions. See
[MinHook upstream](https://github.com/TsudaKageyu/minhook/tree/c3fcafdc10146beb5919319d0683e44e3c30d537).
Use `VRX_DX12_BUILD_INTERCEPTOR=OFF` to build only the producer tests without
this dependency or MASM. Interceptor builds require MSVC's x64 assembler.

The enabled/disabled interception tests copy only their fixture and the new DLL
into a fresh temporary directory, then remove that directory after child exit.
They check native API interception counts, a null-output capability probe,
legacy/versioned root signatures, original COM identity, pixel preservation,
list reset, indexed draws inside bundles, and concurrent device use. A separate
assembly test checks integer, floating-point and stack arguments and returns
through the opaque export forwarders. Existing games and headset sessions are
not touched. The desktop DX12 route remains disabled pending broader command
and shader coverage, managed launch/cleanup and real-game validation.

### Same-frame command/resource replay

`mirror.cpp` independently mirrors committed resources, command allocators,
descriptor heaps, compatible graphics pipelines and graphics/compute command lists. The app
records its native left view once. A rewritten vertex pipeline records the
right view into separate resources, and both lists are submitted in the same
queue batch before the app's completion fence. Root constants and upload-buffer
snapshots come from that submission. `VRX_DX12_SHIFT` (default 0.25) is the full
native-left/right clip offset; `VRX_DX12_CONVERGENCE` defaults to zero.

GPU and WARP fixtures verify 18 successive current-frame pairs, direct and
indexed draws, a nonzero index-buffer address offset, depth-dependent disparity,
unchanged native-left centering and HUD pixels. A native-only indexed control
checks the fixture geometry before replay. Final Release retirement uses identity
tokens and a deferred queue, avoiding a backend lock inside driver Release
callbacks and avoiding deletion of a newer object at a reused address. Fixtures
check that all tracked identities and shadow objects retire after scene teardown.
An unsupported predication command stops replay while the original view
continues rendering correctly.

Additional GPU and WARP fixtures cover SRV/CBV descriptor tables at nonzero heap
offsets, simple/ranged descriptor copies, root CBV/SRV addresses, and separate
depth targets with a deliberately occluded draw. A compute variant reads each
frame's upload data through a root SRV, writes a separate per-eye UAV through a
descriptor table, then transitions that output for sampling by the graphics
pass. Both eyes must contain the current frame's exact colour and valid geometry
disparity, and all shadows must retire after teardown. Compute pipelines share
immutable native bytecode; bindings, resources and dispatches are replayed.

Device2 pipeline streams are parsed using independently pointer-aligned
enum/value subobjects, as specified by Microsoft's
[pipeline-stream ABI](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_pipeline_state_stream_desc).
Graphics streams retain their original settings while the right-eye final
position-producing shader (vertex or geometry) and cached-PSO entry are replaced.
Compute streams share immutable code.
Bounds, duplicate definitions and unsupported shader stages are checked before
rewriting. Native GPU/WARP fixtures use real cached pipeline streams.

Memory heaps and placed resources retain their original offsets and overlaps
in a separate right-eye heap. The GPU test writes through one placed buffer and
reads the overlapping bytes through another after an aliasing barrier.
CreateHeap1, CreateCommittedResource1/2 and CreatePlacedResource1 are also
observed; ordinary descriptor-1 resources use their equivalent base description.
Returned COM interface aliases are tracked without replacing app objects.
Protected sessions and sampler-feedback regions currently stop replay with an
explicit reason. The private diagnostic counts ABI is version 4 and includes
modern-factory counts and the last stop reason.

### Swapchain publication

Native DXGI factory methods are hooked after device creation, including factories
that already exist. The bridge observes D3D12 queue-based swapchains, clones their
backbuffers, and publishes the native left/replayed right view before ordinary
`Present` or `Present1` through the same GPU texture stream consumed by VRX.
Set `VRX_XG_SESSION` to an isolated valid session name to enable publication;
offscreen tests need no session. Internal publisher/consumer device creation is
excluded from app interception. The diagnostic counts ABI is version 4.

HWND hardware/WARP and legacy factory fixtures verify 18 current-frame pairs,
both Present APIs, test-only Present suppression, two resizes (including explicit
single-node queue arrays), odd extents, native-left centering, HUD pixels and
teardown. Resize releases pending backbuffer references after copy completion;
new GPU stream generations can replace the producer's inactive mapping while
old readers still hold it. Old readers detect that replacement, and their
cleanup cannot disconnect the new reader. A fence/event worker also releases
completed source references without needing another frame, preventing a native
swapchain/backbuffer reference cycle at app exit.
An optional `native_dx12_present_engine_handoff` test copies the ordinary fixture
and bridge into an owned temporary directory, starts its native swapchain loop,
and verifies the actual VRX GPU consumer in stereo and mono modes, plus wrong-PID
rejection. The process and copied DLLs are scoped to that fixture; no game or
headset session is started. Readiness requires initialized metadata owned by the
launched process, rather than only the existence of a named mapping.

VRX now reconnects automatically when that same producer/adapter publishes a new
GPU generation. It waits for its previous queue work and source-ring readers
before replacing textures. The headset presentation size remains stable, as
with ordinary window capture. A resized source is fitted on the GPU with aspect
bars and eye-local bilinear filtering; unchanged dimensions retain the exact
copy path. No filtering crosses the stereo seam and no CPU pixel upload occurs.
Hardware/WARP fitting tests compare pixels against an independent reference.
The resize/mono engine checks use a handshake to make one consumer process
verify the original pair and two real native-swapchain replacement generations.
These exercise the production reconnect and presentation functions without XR.

### Native 8-bit formats

The publisher accepts typed RGBA8, BGRA8 and BGRX8, including their sRGB variants.
It also converts SDR `R10G10B10A2_UNORM` using a compatible typeless staging
resource and a normalized ten-bit view. The GPU explicitly rounds RGB to the
nearest 8-bit value and preserves all four two-bit alpha levels. Pixel tests
exercise every 10-bit RGB code and compare against integer reference rounding;
they also verify the original packed bytes remain unchanged.
RGBA uses a byte-preserving compatible-format copy. BGRA/BGRX first copy the two
game views into a private typeless array, then a compute pass writes canonical
RGBA8 directly into a UAV-capable shared texture. Thus the game backbuffer needs
no shader view. UNORM views of the typeless staging resource preserve encoded
sRGB values; the pass changes channel storage, not the transfer function.
BGRX output is opaque; BGRA and RGBA preserve the source alpha.

The intermediate array and descriptor heap belong to each bounded ring slot;
no descriptor or texture is recycled before the previous producer/consumer GPU
work retires. The original input bytes and resource states are restored. Native
hardware/WARP tests cover all six formats, every 8-bit channel value, odd extents,
nonzero array slices, alpha, untouched original resources, and pressure on all
three in-flight slots. BGRA swapchain tests also change back to RGBA at resize;
VRX handoff and automatic stereo/mono reconnect tests cover that format change.
Swapchain colour spaces are tracked through successful `SetColorSpace1` calls.
Only full-range SDR Rec.709 is published. PQ/HDR, linear and wide-gamut changes
stop publication with a diagnostic instead of treating encoded HDR as SDR;
failed native colour-space changes leave the active stream alone. The native
10-bit swapchain tests verify disparity, same-frame identity, resizes and cleanup
on hardware and WARP. Only compatible format families are copied, following Microsoft's
[CopyTextureRegion requirements](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion).

This remains a limited graphics/compute/presentation subset, not a game-ready DX12 route.
Raytracing acceleration-structure views currently stop replay.
Several commands and newer resource/pipeline creation APIs still need
translation. Base graphics-command-list calls now either replay or explicitly
stop stereo; passive markers/events and COM bookkeeping are excluded. The SDK
slot generator checks this base-interface coverage at configure time. Stream-output
calls currently forward natively and stop replay. Hardware/WARP tests verify
that an unsupported stream-output binding still allows a native timestamp query
to produce its GPU result and
that the original game view continues after replay stops. Newer command-list
interfaces are not yet comprehensively guarded and remain a compatibility gap.
Dynamic blend factors and stencil references are replayed. A dedicated
hardware/WARP test renders through a stencil-equality test with changing
references and blend constants, verifies native and right-eye colours and
disparity, and checks complete retirement.
Query heaps are cloned independently. Begin/end/resolve operations translate
the query heap and output buffer, and GPU predication translates its buffer
while preserving offsets and operations. Hardware/WARP tests cover binary
occlusion queries with visible and empty draws, timestamp ordering, result
offsets, conditional rendering, null-predicate disable and heap reuse over
multiple frames. Both native and right-eye GPU query results and rendered
pixels are checked; no query values are emulated on the CPU.
Hook installation failures also stop replay before queue submission or
publication; native execution continues.
Float/uint UAV clears translate both descriptor handles and the resource;
multisample resolves and discard calls translate their resources and preserve
subresource indices/regions. Hardware/WARP checks compare native and right-eye
GPU bytes for exact uint patterns, float clears through staged descriptors,
partial rectangles in a nonzero array slice, four-sample resolves, and repeated
discard/reinitialization. Clear ordering barriers and resource states are
checked by the D3D12 debug layer when available.
Indirect draw, indexed-draw and dispatch signatures share their immutable native
signature while argument/count resources map to the right-eye copies. Argument
and counter offsets and maximum command counts are preserved. Signatures that
embed GPU addresses or other unsupported arguments stop replay when used; their
creation alone does not stop an otherwise compatible scene. Hardware/WARP tests
generate arguments and count buffers on the GPU, verify visible and suppressed
draws, dispatch results and both eyes' pixels, and exercise safe native fallback
for an address-bearing vertex-buffer signature. Binding-bearing arguments still
need GPU address translation before they can be replayed.
Upload buffers are copied in full at submission and need a scalable update
strategy. Presentation currently requires one of those typed SDR formats, a single direct queue/node and
blocking Present semantics; `DO_NOT_WAIT` stops the stream rather than reporting
an unaccepted Present as a successful publication. HDR colour-space interpretation/conversion,
managed chooser launch/cleanup and real-game
stereo validation remain necessary. Multi-node devices and non-default command-list
flags are rejected. Existing games are not launched with this backend by the chooser.

### Scoped native launch and live diagnostics

`launch-native.ps1 -Executable PATH -Session TOKEN [-SteamAppId ID]` launches an
x64 application with an app-local bridge. The launcher waits for exit, then
removes only its unchanged DLL, provided no other instance is using that game
directory. It preserves existing `d3d12.dll`, `d3d11.dll` and `dxgi.dll` files and
refuses to overwrite them. Arguments are passed literally; the default Unreal
arguments request DX12 in a 1280x720 window. Use `-GameArguments` for other engines.
`-ObserveOnly` forwards native rendering without creating right-eye resources.
Keep the launcher alive: its `out/native-GUID` directory records process/file
ownership for recovery if the launcher is interrupted; startup recovery is not
yet implemented. This CLI does not wire up the desktop chooser.

`dx12_status.exe TOKEN [EXPECTED_PID]` reads bounded, process-owned JSON counters
and the first replay-stop reason without loading a graphics device in the reader.
The snapshot identifies observation versus mirroring mode, its PID and age.
Native updates are throttled and drop rather than wait on a diagnostics reader.
A live process, recent counters, actual publications and a verified GPU stream
are all necessary before advertising a successful stereo connection; a launched
process or a zero stop count alone is insufficient.

The launcher integration test runs an ordinary native application from a path
containing spaces/brackets, verifies live status in another process in both
modes, stops it normally, and checks owned cleanup and existing-DLL preservation.

A local System Shock Remake Steam build 22239093 test used native `-dx12` with
this launcher. Observation recorded pipeline streams, placed resources and
memory heaps with zero hook failures; the game exited normally and its bridge
was removed. With mirroring requested, the game continued rendering but replay
stopped at reserved/tiled resource creation. That attempt later produced an
Unreal GPU-hang report (`DXGI_ERROR_DEVICE_HUNG`), despite zero recorded replays;
the cause has not been established. After implementing sparse-resource mapping,
a later attempt replayed three submissions, then rejected the presentation
format/extent and exited normally when closed. A diagnostic retry identified a
1280x720, single-sample `R10G10B10A2_UNORM` swapchain. Those attempts published no
stereo frames. With SDR ten-bit conversion and base-command guards, a later
attempt published one frame before stopping at `OMSetStencilRef`; after dynamic
output-state replay was added, it stopped at `BeginQuery`. Gameplay stereo
remains unverified.
After geometry-stage support, startup reached a float UAV clear. Longer native
DX12 attempts also produced the same pipeline-creation GPU-hang report after
about 64–70 seconds in observation-only mode and in a separate control run with
no VRX DLL or hooks loaded (Windows System32 DirectX modules verified). The
underlying game/driver failure is unresolved; the control establishes that it
can reproduce outside stereo replay. The tested local adapter was an RTX 3090
with driver 617.42. No system/driver settings were changed for those controls.
A launch-only `-norhithread` control subsequently reached the title screen and
exited normally; a bridge run with the same option continued rendering beyond
the earlier failure window but stopped replay at indirect execution. This is a
promising diagnostic observation, not gameplay/comfort validation or a default
argument. The game controls were left to the user.
This establishes concrete compatibility gaps, not native DX12 game compatibility.

Reserved resources created through Device/Device4 now have independent right-eye
reservations. Queue `UpdateTileMappings` and `CopyTileMappings` translate both
resource and physical-heap identities, preserving native tile indices, optional
arrays and mapping flags. Command-list `CopyTiles` maps both resources. The twin
heaps preserve shared physical pages when multiple resources/ranges alias them.
Hardware and WARP tests verify actual GPU bytes after permuted mappings, copied
mappings, reuse of a physical tile, skipped ranges, null mappings and copies in
both directions. They also check native output, newer returned interfaces and
complete retirement. Mapping changes retain the application's synchronization
requirements; the bridge does not add cross-queue dependency management.

### Vertex rewrite implementation notes

For a pipeline with a geometry shader, the bridge rewrites that final stage
instead of its vertex shader. The position offset is emitted before each
geometry emit/emit-and-cut operation, using a saved unmodified position, so
repeated emissions do not accumulate stereo offsets. Non-position varyings,
early returns and strip cuts are preserved. Only stream zero is supported;
stream-output pipelines, tessellation and mesh stages remain unsupported.
Hardware/WARP tests deliberately scale positions in the geometry stage to
distinguish the final-stage offset from an incorrectly applied vertex offset.
They cover both native/cached-stream factories, depth-dependent disparity,
indexed input, conditional emissions, varyings and the W=1 HUD heuristic.

A native compiler probe confirmed that `D3DAssemble` in the available Microsoft
47 runtime rejects `vs_5_0` assembly produced by `D3DDisassemble`. `dxbc_patch.cpp`
therefore rewrites shader-model-5 tokens directly. It preserves signature and
binding chunks, decodes immediate/relative/extended operands, redirects position
register writes to spare temporaries, and applies the signed clip offset and
convergence at each return. Stale debug, statistics and debug-hash chunks are
removed. The original bytecode is never modified.

Native GPU and WARP tests pass for 5.0 and 5.1 shaders with conditional control flow and
non-position varyings. Observed disparity is 19.25 pixels at W=2 and 9.58 at W=4;
the chosen convergence plane aligns, and W=1 HUD pixels match the original
exactly. The HUD heuristic can be disabled for orthographic geometry. Zero
calibration is pixel-identical to the original. Checks also cover signature
preservation, output-looking immediate literals, missing temporary declarations,
checksums and malformed bounds.
The D3D12 debug layer is used when available. With the mirror switch enabled,
this library supplies the right-eye pipeline while the original graphics calls
continue rendering the native left view.

Microsoft's public DXC source provides the token definitions and retail container
hash implementation under its included University of Illinois/NCSA license.
The inspected revision is `194bb75bea2becb42810d72a16195089c1908e10`:

| Upstream path | SHA-256 |
| --- | --- |
| `lib/DxilHash/DxilHash.cpp` | `d66b631ea28d4c90a8217fac004f9290eaac07f79115451e0c4e954c0b0c94af` |
| `include/dxc/Support/d3d12TokenizedProgramFormat.hpp` | `cf808b363406fce9387d7ec8daec19940175af966589c0d3e084a4c41c3f5060` |
| `LICENSE.TXT` | `27a49e35d1da96eba18fba54bc882667ff0ff8c0254f16f2b6e165d605ba7df8` |

The build fetches those pinned files and verifies their SHA-256 values. Tests
confirm the retail hash matches original Microsoft compiler output and that
rewritten programs create native pipelines and produce the expected GPU pixels.
`DXC-Hash-LICENSE.txt` and `DXC-Hash-NOTICE.txt` accompany the test binary and must
accompany binaries using that implementation. The hash is a container checksum,
not an authentication mechanism. The transformer bounds container and operand
access; the D3D12 runtime remains the shader semantic validator.

DXIL, linkage/subroutines and unsupported position layouts need further work.
Rejected transforms return no replacement bytecode, allowing native forwarding
without advertising a stereo stream. W=1 is a heuristic and cannot identify
every HUD or projection convention automatically.
