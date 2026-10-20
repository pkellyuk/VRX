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
explicit VRX Vulkan layer, GPU sharing and live tuning. This is currently an
unsuccessful compatibility test, not working geometry stereo. It disables
opaque descriptor buffers/descriptor heaps and NVIDIA low-latency support in
the translator, and requests `single_queue`, to meet the layer's existing
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
