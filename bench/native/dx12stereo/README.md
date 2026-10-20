# Native DirectX 12 stereo backend

This directory is the independently authored native D3D12 foundation. It does
not load Vulkan or DXVK. `Publisher` accepts two eye resources from the same
simulation frame and submits GPU copies on their rendering queue. The shared
transport is also used by the local experimental Vulkan backend.

**Status:** producer and transport implemented; arbitrary-game interception,
command/resource mirroring and shader adjustment are not implemented here yet.
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
