# Source ownership — first reliability milestone

Date: 2026-09-17

## Problem and change

The capture and synthetic producers previously recycled eight source textures
by index. Waiting for a producer fence made a new frame ready to read, but did
not stop the next write from overwriting an outstanding reader. A depth result
also retained only an index, so `--paired` and eye dumps could use different colour
from the frame that produced their depth.

`bench/native/openxr/source_ring.h` now manages frame references and three GPU
completion timelines independently of Direct3D/OpenXR types:

- Reserving a slot creates a new sequence ID. The producer holds the reservation
  through submission and publication.
- Acquiring the latest publication retains its identity under the ring mutex.
  Depth slots retain their associated colour frame, including while the render
  loop is paused or not visible.
- The model registers its prep-dispatch fence; source sampling ends at that
  dispatch, so the source need not wait for subsequent model computation solely
  for GPU access. Its CPU reference remains retained for paired colour.
- Rendering and eye dumps register their graphics fence before dropping the
  frame reference. Both wait on the selected frame's producer value, including
  when that frame is older than the latest publication.
- A producer can reserve a slot only after all CPU references expire and all
  recorded producer/model/graphics work completes. D3D's device-removal sentinel
  is not treated as completed work.
- If no slot is available, the producer drops the incoming frame. The latest
  published frame remains available. A drop count is reported at shutdown.

The depth triple-buffer handoff and stereo shaders are unchanged. This patch
does not resolve tracking validity, resize handling, general worker recovery,
or device-loss handling from the remaining investigation backlog.

## Validation performed

From the repository root on the existing RTX 3090 / SteamVR setup:

```powershell
bench/native/openxr/test-source-ring.bat
bench/native/openxr/build.bat
bench/native/openxr/out/xrapp5.exe --synthetic --selftest --debug
bench/native/openxr/out/xrapp5.exe 8 --capture --paired --debug
```

The standalone test runner discovers Visual Studio C++ tools when needed. It
requires neither a headset, OpenXR runtime, model, nor GPU. It checks:

1. Exclusive reservation and reference retention, including the latest frame.
2. Independent producer/graphics/model completion, monotonic reader fence
   registration, and device-removal sentinel handling.
3. Retaining paired colour unchanged across 10,000 later publications.
4. Dropping 10,000 reservations while all slots await GPU completion, followed
   by resuming when a single slot becomes safe. These GPU timelines are simulated.
5. A real concurrent producer with two delayed CPU readers: 100,000 publications
   and at least 2,000 reads, checking the retained frame's contents before and
   after delay. These checks exercise the production ownership class.

Results:

- Standalone tests: PASS, built with warnings treated as errors.
- Full native build: PASS for xrprobe, xrapp, xrapp3, xrapp4, and xrapp5.
- GPU prep: PASS; 0 of 806,736 values differ beyond tolerance.
- Nine GPU warp comparisons: PASS, 0 colour mismatches. At 4× disparity the
  ramp cases had 2 stretch / 3 mirror depth mismatches, within the existing
  rounding tolerance of 268 out of 537,824 pixels.
- Live capture with paired colour/depth: exit code 0; 464 captures, 417 worker
  publications reported at frame-loop exit, and 0 source drops. The worker
  completed its final in-flight run during shutdown. The session reached
  SYNCHRONIZED but not VISIBLE/FOCUSED; only one frame was drawn. This verifies
  capture, inference, retained pairing during a non-visible session, and shutdown,
  **not sustained headset rendering or gameplay performance**.

Debug output still includes buffer initial-state warnings and, during the live
run, two `ID3D12CompatibilityDevice::ReflectSharedProperties` errors of the type
previously reported in M3. The run is therefore not a clean debug-layer result.
Their origin has not been re-isolated in this change. Shader compilation also
reports existing integer-division performance warnings.

The sandbox initially prevented the runtime reading its local configuration;
the successful runtime checks used approved access to that configuration. No
captured images were saved.

## Next validation

Wear the headset and test sustained rendering with both latest and paired
colour/depth. Exercise GPU contention and delayed inference beyond eight capture
intervals; observe source drops, frame pacing, and successful shutdown. The
standalone tests establish the ownership rules, not actual driver behavior under
those conditions.

Helldivers 2 is the user's chosen first game. Test ordinary capture, keyboard and
mouse focus, rapid aiming, HUD readability, Alt-Tab, and game-frame-time overhead.
No Helldivers 2 compatibility result is claimed by this milestone.
