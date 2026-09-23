# Linux development

The port design and release gates are in [LINUX.md](../LINUX.md). This directory
contains a Linux OpenXR/Vulkan renderer, static and live ZipDepth paths,
a portal/PipeWire capture adapter, the VRX desktop app (Avalonia), and
prerequisite probes. Packaging is still under development.

## Build

Install a C++17 compiler, CMake, Vulkan development headers, and
glslangValidator. The capture path also needs libportal, PipeWire and GLib
GIO development headers. The model probe is built when ONNX Runtime development
headers and library are found. The PNG and model headset mode also requires
libpng.

```sh
cmake -S linux -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux
ctest --test-dir build/linux --output-on-failure
```

The model and provider must be tested together. For the first Linux host we
used the official ONNX Runtime 1.23.2 Linux GPU bundle with CUDA 12 and cuDNN 9
user libraries. The distribution's ONNX Runtime package exposes CPU, DNNL and
XNNPACK providers but not CUDA. The CUDA test used an isolated bundle and
NVIDIA wheels, without changing the system installation. Set ORT_INCLUDE_DIR
and ORT_LIBRARY at CMake configuration time if those files are not installed
in the normal development locations. Put CUDA/cuDNN libraries and the ONNX
Runtime GPU lib directory in LD_LIBRARY_PATH when running.

## Synthetic headset renderer

The default mode shows a moving scene with known depth in a fixed OpenXR quad.
Vulkan compute generates both eye images and prepares the 672×384 NCHW model
input. The first frame compares both shaders against CPU references. Subpixel
colour may differ by one byte due to GPU rounding; depth must be within 1e-5.
The prepared input must be within 0.0028 in 0..1 pixel units. Prepared depth
is checked but is not yet submitted to an OpenXR depth swapchain.

```sh
VRX_OPENXR_LOADER=/absolute/path/to/libopenxr_loader.so \
  build/linux/vrx-xr-synthetic 10
```

A loader in the system library path needs no override. SteamVR must be the
active OpenXR runtime and the headset must be connected. The process reports
its Vulkan GPU, comparison results and image-bearing frame count. Visibility
in the headset remains a separate check.

`--color=WxH` presents the synthetic scene at another colour size, for example
`--color=1920x1080`, while depth stays on the 686×392 grid. It exercises the
live path's colour-resolution warp and its CPU reference without a capture
source. The scene is upscaled on the CPU each frame, so its frame rate is not
representative.

The CMake build compiles stereo_warp.comp and model_prep.comp to SPIR-V.
VRX_WARP_SPV and VRX_PREP_SPV override the generated shader paths. Without
CMake, compile both shaders and link xr_synthetic.cpp, vulkan_warp.cpp and
vulkan_prep.cpp against Vulkan, dl and pthread.

## ZipDepth still image

Download the checksum-pinned export from the VRX v1.7.6 GitHub release:

```sh
linux/fetch-model.sh
```

The file is stored at bench/models/zipdepth_faithful_fp16_672x384.onnx, which
is ignored by Git. It takes a [PNG release still](../docs/vrx-headset.png) or
another PNG and resizes it to the renderer's 686×392 grid. The Vulkan prep
shader produces the input tensor; ONNX Runtime computes depth once before the
OpenXR frame loop; the Linux path resamples, normalizes and dilates it; and the
Vulkan warp renders stereo eyes. This is a static test path, not live capture.

```sh
build/linux/vrx-model-probe --cuda bench/models/zipdepth_faithful_fp16_672x384.onnx
VRX_OPENXR_LOADER=/absolute/path/to/libopenxr_loader.so \
  build/linux/vrx-xr-synthetic 15 --still=docs/vrx-headset.png --cuda
```

The CUDA option requires the CUDA provider and disables CPU fallback. Omit
it to test CPU inference. On the reference RTX 5060 Ti, warm CUDA inference
took 2.5–3.0 ms; first use took longer due to GPU initialization. The headset
still-image run passed prep and warp comparison. With the headset active,
the 15-second test reached XR_SESSION_STATE_FOCUSED and submitted 1,311
image-bearing frames with no runtime skips. The user confirmed the stereo
still image looked correct in the PICO 4. The model probe also reports the CPU/GPU depth difference.

## Room reference fixture

`vrx-room-reference` runs the portable Windows room CPU reference on Linux with
the synthetic source. It checks flat room geometry, a tracked floor, emitter
layout and radiance, the reduced mirror image, diffuse and lit lightmap
samples, and Fresnel reflection ordering. Its printed sample values are the
starting comparison points for the Vulkan EMIT, MIRROR, LIGHT and eye passes.
The Vulkan `room_mirror.comp`, `room_emit.comp` and `room_light.comp` shaders
build to SPIR-V. `vrx-room-gpu-probe` executes all three and compares MIRROR
against `RoomMirrorPicture`, EMIT against `RoomEmitRadiance`, and every texel of
the six-face LIGHT pass against `RoomTexel`, all at a 1e-5 tolerance. The fixture
exercises picture patches, active and inactive glow blocks, partial temporal
blending, a lit ceiling panel, glass/reflection finish and a nonblack world. The GPU probe is built by
default but only enters CTest when configured with `-DVRX_TEST_VULKAN_GPU=ON`.
The headset renderer does not draw the room yet.

```sh
build/linux-release/vrx-room-reference
build/linux-release/vrx-room-gpu-probe
```

## Live source and the VRX desktop app

`--live` opens the desktop ScreenCast chooser and displays a selected window
or monitor in the headset. It uses flat depth unless `--cuda` is added;
`--live --cuda` runs checked ZipDepth on a separate inference worker and uses
the newest completed depth. Colour keeps the source's size, shrunk to at most
1920 pixels wide as on Windows; the warp samples the 686×392 depth map
bilinearly at that size. `--until-stop` runs until SIGINT or SIGTERM. The
SteamVR loader under the user's standard Steam install is found automatically
when no system OpenXR loader is installed; `VRX_OPENXR_LOADER` overrides it.

```sh
build/linux/vrx-xr-synthetic --until-stop --live --cuda
```

`--source=window`, `--source=screen` or `--source=any` (the default) limits
what the desktop chooser offers.

The VRX desktop app, `linux/VRX.Linux`, is an Avalonia (.NET 10) app laid out
like the Windows WPF app: an Easy | Expert switch, the card with Attach / Play,
Stop VR and Recenter, and in Expert the same collapsible sections. It starts the
engine, shows its log and state, stops it with SIGTERM (forcing it after five
seconds), and keeps named profiles under the XDG config directory
(`linux-profiles.json`, the format the earlier PyQt controller used) plus its
own choices in `linux-app.json`. Settings are saved in the selected profile as
they change, and screen, stereo and room controls apply live through an atomic
`VRXL 4` snapshot, which adds a Recenter counter and the game frame timing;
older snapshots still load. Linux
differences: Wayland does not let an app list other windows, so "Game &
window" chooses between a window, a whole screen or either, and the desktop's
sharing dialog does the choosing; there is no auto-attach; settings the Linux
engine cannot apply yet (curve, follow-head, a separate ambilight,
steadying, timing modes, SteamVR menu options) are shown disabled. The app
starts the room renderer even when Room is 0, so it can be turned on live.

Build it with the .NET 10 SDK (a user-local install from `dotnet-install.sh`
works) and start it with `linux/vrx-linux`, which finds a user-local .NET and
reads optional environment such as the CUDA library path from
`~/.config/vrx/environment`. `linux/install-desktop.sh` adds VRX, with its logo,
to the user's application menu. Set `VRX_LINUX_ENGINE` if the engine is outside
the normal `build/linux-release` or `build/linux` locations.

```sh
dotnet build -c Release linux/VRX.Linux
linux/vrx-linux
linux/install-desktop.sh
```

`vrx-linux --check` tests profiles, the snapshot format and the engine
arguments without a display (it is in CTest when .NET is found).
`VRX_LINUX_SCREENSHOT=<directory>` renders the window in both modes to PNG
files and exits.

## Windows-style room renderer

`--room` adds a tracked per-eye room projection layer behind the stereo screen.
The Linux GPU runs EMIT, MIRROR and LIGHT compute passes; the eye pass is
generated at build time from the Windows HLSL in `xrapp5.cpp` and constants in
`room.h`, then compiled to Vulkan SPIR-V. It draws the same room geometry,
ceiling panel, framed glass, tiles and per-eye reflections. The glow uses the
portable Windows ambilight reference at 64×45 texels, with temporal blending;
this is lower resolution than the Windows 256-texel-wide glow to keep its CPU
step near 1 ms. The room uses the Windows default black world colour.
SteamVR STAGE floor position is used when available, with the seated estimate
as fallback. The PICO 4 test target uses a 1322×1322 room image per eye, half
its reported 2644×2644 recommendation. The stereo screen remains a separate
quad layer; its dark footprint in the room projection prevents bright gaps.

```sh
build/linux/vrx-xr-synthetic 15 --room
build/linux/vrx-xr-synthetic 15 --room-dump=/tmp/vrx-room-eyes.ppm
```

`--room-dump` also compares a grid of eye pixels with `room.h` and saves the
first rendered stereo room image for inspection. It implies `--room`. The
Vulkan EMIT, MIRROR and LIGHT passes can be checked against the CPU reference
with `build/linux/vrx-room-gpu-probe` (or the opt-in
`VRX_TEST_VULKAN_GPU` CTest option). The room was tested on the NVIDIA RTX
5060 Ti with SteamVR; the generated shader needs Vulkan storage-image writes
without format. `--room` reports a clear startup error if the GPU lacks that
feature or the runtime offers no RGBA swapchain format.

`VRXL 2` is a single line: the five `VRXL 1` floats, then Room, Glass,
Reflections and ceiling light percentages (0–100), then the decimal value of
an RGB light colour. `VRXL 3` adds one more field, a counter the desktop app
increments for each Recenter; the engine places the screen in front of the
headset whenever it changes (and on the first tracked frame). The desktop app
validates it. `VRXL 4` then adds the game frame timing: 0 latest frame, 1
delayed to depth, 2 matched to depth (`frame_timing.h`, as on Windows). The
engine keeps the last eight live frames on the GPU for the delayed and
matched modes. The desktop app writes version 4. Room 0 omits the projection layer while
retaining the existing stereo quads.

## Build and run the probe

With CMake, a C++17 compiler, Vulkan development files and glslangValidator:

```sh
cmake -S linux -B build/linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build/linux
ctest --test-dir build/linux --output-on-failure
build/linux/vrx-probe
```

If CMake is not installed, the same source can be compiled directly:

```sh
g++ -std=c++17 -Wall -Wextra -Werror \
  -Ibench/native/openxr/include linux/probe.cpp \
  -o /tmp/vrx-probe -lvulkan -ldl
/tmp/vrx-probe
```

The probe uses the repository's OpenXR headers and loads the OpenXR loader at
runtime. It reports the Vulkan loader/devices, OpenXR Vulkan extension and
headset system, and whether the PipeWire and CUDA driver libraries can be loaded.
Missing prerequisites are reported in the output; they do not make the probe
itself fail. Library presence alone does not prove that portal capture, CUDA
inference, or a VR session will work.

If a loader is installed outside the system library path, pass it explicitly:

```sh
/tmp/vrx-probe --openxr-loader=/absolute/path/to/libopenxr_loader.so.1
```

The loader still needs an active OpenXR runtime configuration. A loader file by
itself does not provide a headset runtime.

For a desktop capture check, query the running user's portal service:

```sh
busctl --user get-property org.freedesktop.portal.Desktop \
  /org/freedesktop/portal/desktop \
  org.freedesktop.portal.ScreenCast version
```

This property confirms that the ScreenCast portal is exposed. A real selection
and PipeWire stream test is required at milestone L3. For inference, the exact
ZipDepth model and the intended ONNX Runtime CUDA provider must be tested
together at L2. Do not infer provider support from `libcuda.so.1` alone.
