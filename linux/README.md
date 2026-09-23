# Linux development

The port design and release gates are in [LINUX.md](../LINUX.md). This directory
contains a Linux OpenXR/Vulkan synthetic renderer, a PNG still-image path with
ZipDepth, and prerequisite probes. Live capture and the desktop controller are
still under development.

## Build

Install a C++17 compiler, CMake, Vulkan development headers, and
glslangValidator. The model probe is built when ONNX Runtime development headers
and library are found. The PNG and model headset mode also requires libpng.

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
