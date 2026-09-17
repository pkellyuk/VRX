# Playback recovery — tracking, resize, and inference failure

Date: 2026-09-17

## Implemented behavior

### Tracking and OpenXR lifecycle

`xr_frame_guard.h` provides the tested view-validity check and swapchain helper.
The frame loop checks the locate result, two returned views, and valid position
and orientation bits before using poses. It completes a frame with zero layers
when tracking is invalid, and resumes projection when valid tracking returns.
It logs tracking state changes rather than flooding the log every frame.

An acquired image is not released unless its wait returned `XR_SUCCESS`.
`XR_TIMEOUT_EXPIRED` is positive, but does not make the image ready. Acquire,
wait, release, frame, and session errors stop playback rather than reporting a
normal exit. On a failed swapchain wait the application terminates that session;
it does not attempt to reuse the outstanding image. See the
[OpenXR wait reference](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrWaitSwapchainImage.html).

### Capture resize and closure

The source ring and swapchains keep their startup dimensions, so GPU readers do
not need their resources or descriptors replaced. When content size changes,
capture releases the transition frame, recreates the frame pool, and waits for a
new correctly sized frame. This follows the resize approach described in
[Microsoft's capture guidance](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture).

`CaptureScaler` uses an ordinary copy at the original size. Other sizes are
copied into a shader-readable D3D11 texture, then fitted into the shared output
with a linear-filtered draw. It clears the complete destination to opaque black
before drawing, preserves aspect ratio, and unbinds the output before handing it
to D3D12. This prevents both cropped growth and stale borders after shrinking.

Each resize increments the source layout generation. Stereo is disabled until
the depth result matches the new layout. Rendering remains at the initial output
resolution; increasing a window later does not increase headset resolution.
Letterboxing is the deliberate first implementation, not dynamic screen resizing.

Zero-sized content is skipped without recreating a zero-sized pool. The captured
item's `Closed` event and capture exceptions signal a controlled stop; handlers
and sessions are cleaned up even if startup fails partway through. Source
reopening and display reconnection are still future work. A minimized source may
retain its last frame until capture supplies another one.

### Depth fallback and retries

The renderer checks worker health and compares colour/depth **source** timestamps.
It falls back to unwarped colour if depth is absent, has failed, belongs to a
different capture layout, or is more than 250 ms behind changing content. That
threshold is a provisional guard, not a claimed acceptable gameplay latency.
An unchanged source sequence does not expire simply because time passes.
Paired mode also uses this decision, so a failed worker does not freeze colour on
an old retained frame. Flat frames omit compositor depth submission.

The flat shader path does not sample invalid depth when neither warping nor
depth output is requested. Startup inference failure can now enter flat playback
instead of aborting. The worker permits up to three consecutive failed attempts,
with nominal one-second delays between retries. It retries the same static frame
when necessary. A successful attempt restores health; exhaustion leaves flat
viewing active and produces a non-success exit status. Restart playback to retry
after exhaustion. Capture remains active during a depth failure.

Model output type and element count are checked before copying it, and non-finite
values are rejected before histogram normalization. The histogram index is also
clamped. Prep work is synchronized before retrying after a failed inference.

This does not yet handle failure to initialize the model/runtime in the first
place, rebuild a lost graphics device, or interrupt a driver/model call that
never returns. Those remain explicit limitations.

## Tests and observed results

```powershell
bench/native/openxr/test-source-ring.bat
bench/native/openxr/test-playback.bat
bench/native/openxr/build.bat
bench/native/openxr/out/xrapp5.exe 5 --capture --paired --debug --test-depth-failures=1
bench/native/openxr/out/xrapp5.exe 5 --capture --paired --test-depth-failures=3
bench/native/openxr/out/xrapp5.exe 5 --capture --paired --test-depth-failures=4
```

`--test-depth-failures=N` is a diagnostic injection before the first N depth
attempts, including the synchronous startup attempt. It is off by default.

- Source ownership regression tests: PASS.
- Playback tests: PASS, compiled with warnings treated as errors. Mocked OpenXR
  calls cover failed acquire, wait timeout/error, failed release, and exactly-once
  release of ready images. Tracking tests cover failed locate, missing views,
  invalid orientation/position, and restoration. Policy tests cover unchanged
  sources, stale moving sources, worker failure, missing depth, and recovery.
- Resize pixel tests: PASS using D3D11 WARP (software rendering, no headset).
  They reuse a 64×48 destination across 64×48, 32×24, 96×24, 12×48, 96×72, and
  restored 64×48 input. Checks read back actual pixels to confirm quadrant
  position/colour, aspect fitting, and black bars replacing previous content.
  Clipped transition input is rejected.
- Full native build: PASS.
- GPU prep and all ten warp comparisons: PASS. The additional flat-fallback
  case supplies NaN depth and still produces the expected unwarped colour.
  Existing 4×-disparity depth rounding differences remain within tolerance.
- One injected failure: startup entered flat playback, the worker subsequently
  published 108 results by frame-loop exit, and the process exited 0.
- Three injected failures: startup failed, then the worker failed twice and
  recovered on its third attempt. It published 72 results by frame-loop exit;
  process exit 0.
- Four injected failures: startup and all three worker attempts failed. Capture
  continued, no depth was published, the process stopped after its requested
  duration without crashing, and exit status was 1 as expected.

Runtime tests used the existing RTX 3090 / SteamVR configuration. Only one frame
was drawn per run while the headset was not visible; these runs validate the
recovery control flow and startup flat path, **not continuous visible switching
between flat and stereo**. No captured images were saved. The debug run still
reported the previously seen buffer initial-state warnings and two
`ReflectSharedProperties` errors; it was not a clean debug-layer result.

## Remaining checks before a gameplay claim

- Wear the headset and verify loss/restoration of tracking and visible flat/stereo
  transitions without abrupt or uncomfortable changes.
- Exercise a real captured window through shrink/grow, aspect change, minimize,
  restore, and closure. The pixel tests validate the scaler; they do not reproduce
  Windows.Graphics.Capture's real window event ordering.
- Test capture errors/display removal, runtime restart, device loss, and waits
  that fail to complete. General GPU waits remain a separate reliability item.
- Test Helldivers 2 in borderless mode with keyboard and mouse, including HUD,
  fast aiming, Alt-Tab, and sustained GPU contention. No game compatibility or
  headset-awake frame-rate result has been established by these changes.
