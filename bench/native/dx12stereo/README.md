# Native DirectX 12 stereo backend

This directory is the independently authored native D3D12 foundation. It does
not load Vulkan or DXVK. `Publisher` accepts two eye resources from the same
simulation frame and submits GPU copies on their rendering queue. The shared
transport is also used by the local experimental Vulkan backend.

**Status:** producer, transport and native command interception foundation
implemented, together with a GPU-tested shader-model-5 vertex rewrite library.
Command/resource mirroring and DXIL rewriting are not implemented here yet.
Interception and rewriting are tested with fixtures, not arbitrary real games.
This does not enable the desktop DX12 button. The fixture supplies both views
explicitly, so passing these tests does not prove stereo in an unmodified game.

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
must belong to that queue's device. Only typed RGBA8, single-sample, one-mip 2D
textures or array slices are currently accepted. Unsupported formats need
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

### Swapchain publication

Native DXGI factory methods are hooked after device creation, including factories
that already exist. The bridge observes D3D12 queue-based swapchains, clones their
backbuffers, and publishes the native left/replayed right view before ordinary
`Present` or `Present1` through the same GPU texture stream consumed by VRX.
Set `VRX_XG_SESSION` to an isolated valid session name to enable publication;
offscreen tests need no session. Internal publisher/consumer device creation is
excluded from app interception. The diagnostic counts ABI is version 3.

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

This remains a limited graphics/compute/presentation subset, not a game-ready DX12 route.
Predication and raytracing acceleration-structure views currently stop replay.
Many commands and newer resource/pipeline creation APIs still need interception
and translation; unobserved commands are not yet comprehensively rejected.
Upload buffers are copied in full at submission and need a scalable update
strategy. Presentation currently requires RGBA8, a single direct queue/node and
blocking Present semantics; `DO_NOT_WAIT` stops the stream rather than reporting
an unaccepted Present as a successful publication. BGRA/HDR conversion,
managed launch/cleanup and real-game
validation remain necessary. Multi-node devices and non-default command-list
flags are rejected. Existing games are not launched with this backend by the chooser.

### Vertex rewrite implementation notes

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
