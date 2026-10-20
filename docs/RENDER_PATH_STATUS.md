# Render path completion evidence

The goal remains complete, automatic Depth estimate, Vulkan, DXVK and native
DX12 paths through the existing VRX chooser. Foundation tests do not establish
arbitrary-game compatibility. This goal is not complete.

| Path | Current evidence | Required work remaining |
| --- | --- | --- |
| Depth estimate | Desktop smoke; actual GPU self-tests for DAV2 and ZipDepth, including steady/fusion | Continue checking chooser regressions as geometry backends change |
| Vulkan | Local shader rewrite, same-frame mirroring, GPU transport; 26 native regression tests | Resolve adapted mirror source redistribution or replace it independently; package layer; validate managed native-Vulkan launch and more game rendering paths |
| DXVK | Local System Shock headset confirmation; ordinary D3D11 fixture passes two managed restarts, distinct same-frame geometry, VRX GPU consumer and exit cleanup | Resolve mirror source issue; package translator/layer; test managed System Shock gameplay; expand format, resize and rendering coverage |
| Native DX12 | Native GPU publisher and cross-process VRX consumer; opt-in DLL interception; DXBC 5.0/5.1 rewrite; GPU/WARP replay tests cover draws, descriptors, depth, compute dependencies and cleanup; ordinary DX12 swapchain fixtures publish 18 same-frame pairs through Present/Present1, survive two resizes and retire resources correctly | Complete modern API/resource/command coverage; implement DXIL, scalable uploads, format conversion, consumer resize reconnection and managed native launch/cleanup; test real DX12 games before enabling chooser button |

Shared requirements still include robust resource and command coverage, HDR and
format handling, resize/swapchain recreation, independent eye histories,
same-frame simulation data, process ownership, bounded GPU backpressure,
normal restart and cleanup behavior, per-game settings, and deployable builds.
Game and headset performance/comfort need actual runtime evidence; unit tests
alone cannot prove those properties.

## Repeatable checks

```powershell
dotnet build desktop/VRX.Desktop/VRX.Desktop.csproj -c Release
desktop/VRX.Desktop/bin/Release/net10.0-windows/VRX.Desktop.exe --smoke-test
cmake -S bench/native/dx12stereo -B bench/native/dx12stereo/out -A x64
cmake --build bench/native/dx12stereo/out --config Release
ctest --test-dir bench/native/dx12stereo/out -C Release --output-on-failure
```

`VRX_DX12_ENGINE` is an optional CMake cache path to a geometry-capable xrplayer
for the cross-process check. The opt-in managed DXVK integration smoke extension
requires the local experimental Vulkan layer, verified DXVK cache and GPU
snapshot utility; see [the native test notes](../bench/native/dx12stereo/README.md).

The standard smoke test uses isolated profile and fixture files. The managed
integration test closes/restarts only its freshly copied fixture, never an
installed game. Neither starts a new headset session.
