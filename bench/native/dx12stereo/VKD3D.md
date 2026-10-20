# D3D12 through vkd3d-proton: Windows experiment

This is a separate translation experiment, not the native D3D12 mirroring
backend and not an enabled production chooser route. The game calls D3D12;
vkd3d-proton translates those calls and shaders to Vulkan. VRX can then attempt
its separately supplied experimental Vulkan stereo layer.

## Build used

On 20 October 2026, cloned the official repository recursively to
`C:/Users/paulj/dev/vkd3d-proton`, at
`ef9a02e873715c9e2da0d6db8a236aca8380b303` (project version 3.1.0).
All nested submodules were checked out at their pinned revisions. No translator
source changes were needed. The build uses the existing MSYS2 MinGW64 GCC
15.2 toolchain, with Meson, Ninja, glslang and widl installed through MSYS2.

From PowerShell, in the translator checkout:

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
meson setup build-win-local --buildtype release -Denable_tests=true -Denable_extras=false
ninja -C build-win-local -j 6
```

Outputs are `build-win-local/libs/d3d12/d3d12.dll` and
`build-win-local/libs/d3d12core/d3d12core.dll`. The build completed. Upstream
emits Meson compatibility warnings and a test-source printf format warning;
this was not a warning-free build.

## Temporary launcher

`launch-vkd3d.ps1` takes explicit translator DLL paths and DXVK's x64 `dxgi.dll`.
It also installs the matching `d3d11.dll` from the same DXVK directory, unless
an explicit `-D3d11Dll` path is supplied. System Shock creates an auxiliary
D3D11 device even with `-dx12`: using native D3D11 with replacement DXGI caused
a startup access violation. Supplying matching DXVK D3D11 resolved that failure.

The launcher refuses existing destination DLLs, uses exclusive file creation,
records installed SHA256 hashes, waits for the child, and removes only unchanged
installed copies. Keep the launcher alive until the game exits. Logs, tuning
and shader caches are scoped to a unique ignored `out/vkd3d-*` directory.
No global Vulkan layer registration or driver setting changes are made.

```powershell
./launch-vkd3d.ps1 `
  -Executable 'E:/SteamLibrary/steamapps/common/System Shock Remake/SystemShock/Binaries/Win64/SystemReShock-Win64-Shipping.exe' `
  -D3d12Dll 'C:/Users/paulj/dev/vkd3d-proton/build-win-local/libs/d3d12/d3d12.dll' `
  -D3d12CoreDll 'C:/Users/paulj/dev/vkd3d-proton/build-win-local/libs/d3d12core/d3d12core.dll' `
  -DxgiDll '../xgstereo/out/dxvk-2.7.1/x64/dxgi.dll' `
  -SteamAppId 482400 -Stereo none
```

The default arguments include `-dx12 -norhithread -windowed -ResX=1280
-ResY=720`. `-norhithread` preserves the previously established System Shock
DX12 baseline. `-Preview` performs preflight without installation or launch.

`-Stereo mirror -Shift 0.008 -Session UNIQUE_NAME` additionally enables the
explicit VRX Vulkan layer, GPU sharing and initial stereo tuning. This mirror
backend reads its tuning file at startup; changing it requires a restart.
It disables opaque descriptor buffers/descriptor heaps, NVIDIA low-latency
support, shader identifiers, pipeline binaries and graphics pipeline libraries
in the translator, and requests `single_queue`, to meet the layer's existing
requirements. Translation-only mode does not apply those vkd3d restrictions.

## Results on System Shock / RTX 3090

- Upstream device-creation, render-target-clear and indexed-draw smoke tests:
  **327 checks, zero failures, zero skips**, using the built translator DLLs and
  DXVK 2.7.1 DXGI. These are selected tests, not the full upstream suite.
- The new launcher also ran the upstream indexed-draw test: **22 checks, zero
  failures**, then removed all four DLLs. A separate sentinel-file check proved
  that it refuses to replace an existing DLL and preserves its contents.
- System Shock translation-only initially exited with code 3. Its crash context
  for PID 28716 pointed into native `d3d11.dll`. With matching DXVK D3D11 added,
  PID 62016 reached saved gameplay; the user confirmed a full responsive scene.
  Loaded-module paths and vkd3d logs confirmed actual D3D12-to-Vulkan translation.
- With Vulkan stereo enabled, PID 16376 still showed full responsive desktop
  gameplay (user confirmed), but no shared GPU stereo producer was available.
- A diagnostic rerun, PID 56276, confirmed that the shader rejection and the
  1280x720 presentation swapchain belonged to the **same Vulkan device**. The
  layer reported `unsupported physical-pointer shader` and `live=0` at swapchain
  creation. No eye pairs were published. No headset stereo validation is claimed.
- Rejected shader sizes were 5460 and 4300 bytes, with two and three
  `OpConvertUToPtr` instructions respectively. Those sizes match the locally
  compiled `cs_emit_nv_memory_decompression_regions` and
  `cs_emit_nv_memory_decompression_workgroups` helpers. The translator creates
  them during device initialization, independently of game shader execution.
  The current layer rejects integer-derived physical pointers; it supports only
  the reflected typed push-constant pointer path. The rejection was not bypassed.

After testing, the game was restarted in translation-only mode, with normal
VRX depth estimation restored. The native D3D12 geometry chooser gate remains
unchanged. This experiment establishes a working translation baseline; it does
not establish stereo compatibility or a performance advantage.

Next backend work is to handle the translator's shader/address model correctly,
including distinguishing unused helper pipelines from executed workloads,
before assessing main-game stereo and performance. Removing the unsupported
pointer guard would allow second-eye commands to access first-eye resources.

## Pointer and transport fixes: subsequent trial

The independently authored `common/spirv_push_addresses.h` now traces
integer-to-pointer conversions and pointer bitcasts back to specific eight-byte
push-constant fields. It accepts a proven address plus a numeric offset,
bit-preserving conversions and single-store function locals. It also checks
typed physical-pointer loads. Unknown GPU-loaded addresses, truncated addresses,
sums of addresses, unsupported control flow and malformed instructions remain
unsupported. Arbitrary integer fields are not scanned for address-like values.
The existing typed push-field reflection and new integer origins are combined
before recording private second-eye push constants. Caller memory stays intact.

Both actual decompression helper shaders are accepted with relocated push
fields at offsets **0, 8 and 24**. A GPU test writes through typed pointers and
through integer-derived pointers to an interior buffer address. For each mode,
three second-eye submissions complete while the native counter remains 1, 2, 3
(it would be 2, 4, 6 if the second eye wrote the native buffer). The test wrapper
requires successful replay in the layer log, so simply disabling mirroring does
not pass the test.

Further compatibility fixes in the locally supplied layer:

- Accept supported normalized 10-bit swapchain formats and check Vulkan blit
  capabilities before using the existing GPU conversion into shared RGBA8.
  System Shock presents `VK_FORMAT_A2B10G10R10_UNORM_PACK32` (64).
- Create the transport's D3D12 textures/fences with explicit **System32** D3D12
  and DXGI modules. Resolving the game's translated DLLs produced incompatible
  resource handles. The native-module helper is independently authored and has
  also been tested with app-local translator DLLs loaded.
- Keep presentation-timing configuration on the real swapchain. There is no
  second presentation swapchain to configure.
- Resolve translated scalar handles before checking whether replay is still
  valid, so a failed lookup cannot subsequently pass a null handle to the driver.
- Require shader bytecode for pipeline reflection. Identifier-only cache paths
  are disabled in the trial launcher; the pointer guard remains enforced.

With these changes, PID 29376 entered saved gameplay through vkd3d-proton and
published full same-frame eye pairs. The user confirmed **gameplay visible in
both eyes**. A diagnostic GPU readback of frame **4944/4944** contained the full
room, weapon and HUD in both 1280x720 eyes (917515/917532 nonblack pixels).
Its mean RGB eye difference was 0.400668/255 at strength 0.008. The user reported
that depth was too weak, so this is not a comfort or calibrated-depth claim.
The following run (PID 40428) uses strength **0.5**, convergence **0**. The user
confirmed **clear depth and comfortable** in gameplay. Its diagnostic readback
at frame **5096/5096** again contained the complete scene in both eyes
(917967/917893 nonblack pixels; mean RGB eye difference 10.145988/255). The game
and geometry headset viewer were left running at this setting. This validates
one saved gameplay scene, not every effect or a sustained performance target.

Targeted validation: **8/8 Release CTests**, **5/5 Debug fixture CTests**, plus
typed/integer GPU isolation tests against the actual Debug layer. The 10-bit
presentation regression checks ten same-frame pairs, moving geometry, color
conversion and depth-dependent disparity. These are targeted checks, not a full
game or performance certification. The existing source-only native D3D12 suite
was not rerun for these Vulkan-specific changes.

The adapted generator and mirroring backend remain a local external prototype:
their redistribution permission is still unresolved (see
`../xgstereo/MIRROR_ORIGIN.md`). Independently authored helpers, integration
fixtures and launcher changes can be committed without publishing those adapted
files. The native D3D12 chooser gate remains unchanged.
