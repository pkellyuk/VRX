# Optional GPU depth processing

The `xgpuoffload` branch starts from `main` and extracts the GPU depth pipeline
from `xoptimal` (`c0a70a0`). It does not include the visual enhancements.

In the desktop app, enable **GPU depth post-processing (experimental)** under
the depth settings, below **Depth GPU**. The checkbox is saved per game and
applies at the next **Attach / Play**. It defaults off for new and old profiles.
The desktop app passes `--gpu-depth` only when enabled; `--cpu-depth` explicitly
selects the default CPU post-processing path for command-line comparisons.
The live desktop control format is v13 (v12 is reserved for the separate
`xoptimal` experiment); older v1-v11 snapshots remain supported.

**Show depth map instead of game** switches live to the processed nearness
used by the stereo warp: white is near, black is far. Both eyes see the same
map without stereo displacement. An unavailable/stale map displays black.
It works with either CPU or GPU processing and adds no depth readback.
Unticking it restores the game view. The command-line equivalent is
`--show-depth-map`.

With GPU processing enabled, DirectML output binding, normalization, resampling,
dilation, motion luma preparation, steadying, fusion and foreground crop fitting
and application retain image-sized data on GPU. Post-processing runs on the
headset GPU; inference runs on the selected Depth GPU. Cross-adapter buffers and
fences transfer data when those differ. The CPU reads the 49 x 28 foreground
tracker grid and scalar diagnostics. Full-map readback is reserved for explicit
synthetic tests and dumps. This reduces CPU work and transfers; it is not a
guarantee of higher game FPS.

Off uses the existing CPU algorithms and avoids the new shader compilation,
depth-slot buffers and GPU motion-vector buffers. Both modes still use DirectML
for the primary model. A recoverable GPU-processing failure retires outstanding
GPU work before using the CPU path. Unsafe retirement stops depth processing.

## Validation

Build the engine with `build.bat --desktop`, and build the desktop app in Release.
Run its `--smoke-test` to check opt-in defaults, per-game persistence, checkbox
mapping, launch arguments, base settings and Apply to all.

From the repository root, representative native checks are:

```powershell
bench/native/openxr/out/xrplayer.exe --selftest-gpu=0 --steady --debug
bench/native/openxr/out/xrplayer.exe --selftest-gpu=0 --gpu-depth --steady --fuse --debug
bench/native/openxr/out/xrplayer.exe --selftest-gpu=1 --gpu-depth --steady --fuse --debug
bench/native/openxr/out/xrplayer.exe --selftest-gpu=0 --depth-gpu=auto --gpu-depth --steady --fuse --debug
cmd /c bench\native\openxr\build-gpu-depth-probe.bat
bench/native/openxr/out/gpu-depth-probe/gpu_depth_probe.exe 0
bench/native/openxr/out/gpu-depth-probe/gpu_depth_probe.exe 1
cmd /c bench\native\openxr\test-playback.bat
bench/native/openxr/out/playback_test.exe desktop/out/control-contract.txt
cmd /c bench\native\openxr\test-source-ring.bat
```

Validated on RTX 3090 and RTX 3060: CPU default path, GPU processing on either
card, second-GPU inference, model switches, exact published-slot render copies,
and injected GPU failure recovery all passed. GPU reference probes passed
normalization, steadying, fusion, crop acceptance/rejection, nonfinite handling
and changing cross-adapter transfers. D3D12 reported zero errors; it emitted
the existing buffer initial-state warning (1328). Desktop smoke, playback/control
contract and source ownership tests passed.

One additional CPU-mode test with fusion on the same RTX 3090 failed its
`fused > 0` assertion: six anchor passes completed, but their roughly 737 ms
median duration exceeded the 300 ms local-fusion age limit for the moving test
source. The CPU default test (fusion off) and all GPU fusion configurations
passed. The CPU algorithm and age limit have not been changed to mask this.
These are synthetic validation results, not a game-FPS benchmark or a test of
recovery from a real driver hang.

Depth-preview validation also passes: per-game persistence, live v13 snapshot
parsing, checkbox on/off mapping, grayscale GPU pixels for both eyes, unavailable
map black output, and return to the normal game renderer.
