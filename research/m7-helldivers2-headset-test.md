# M7 — first headset-on Helldivers 2 test

Date: 2026-09-17

## Setup and scope

The user confirmed readiness with the headset on. `helldivers2` was running
(PID 33032). The test requested window-title capture using `--window=HELLDIVERS`
on the existing RTX 3090 / PS VR2 / SteamVR setup:

```powershell
bench/native/openxr/out/xrapp5.exe 60 --window=HELLDIVERS
```

Default strength 1.0, mirror fill, dilation 2, range smoothing enabled, latest
colour plus latest depth, compositor depth submission disabled. Captured size
was 3844×2207; stereo output was 1920×1102, with depth at 686×392. Exact game
graphics settings and display mode were not independently recorded.

The runtime reached VISIBLE, then FOCUSED. Unlike the earlier smoke tests,
every frame was drawn throughout the 60-second frame loop. The process exited
normally with code 0. The raw log is in the ignored build-output directory at
`bench/native/openxr/out/helldivers2-first-headset-test.log`; no images were saved.

The selected window title did not print correctly in the log, and capture does
not yet log the owning PID. The user subsequently reported that the 3D picture
"looked great" during this Helldivers 2 test. Executable identity logging still
needs improvement, and this short run is not a broad compatibility pass.

## Recorded results

| Metric | Result |
|---|---|
| Rendered/submitted frames | 7,107; 118.4 fps overall; all counted frames drawn |
| Captured frames | 2,343; approximately 39 per second |
| Depth updates consumed by rendering | 621; 10.3 updates/s |
| Source-ring drops | 0 |
| Frames suppressed for invalid tracking | 0 |
| Flat fallback due to unavailable/stale depth | Two brief episodes, approximately 50 ms and 99 ms |
| Exit | Normal shutdown, code 0 |

Across 29 approximately two-second reporting intervals:

- Capture rate ranged from 32.9 to 45.8 fps (mean reported rate 39.06).
- Depth update rate ranged from 8.5 to 12.0 per second.
- Reported model time ranged from 71.3 to 119.4 ms (mean of reported samples
  95.31 ms). These are sampled latest-run times, not a per-inference mean or
  latency percentile distribution.
- Mean reported age since depth completion was 50.81 ms. This excludes capture
  and inference time and must not be labelled end-to-end latency.

The compositor submission cadence stayed close to 120 Hz, but this is not 120
new game pictures or depth estimates per second. Captured-frame rate is also
not a direct measurement of the game's native frame rate or input-to-photon
latency. There was no matched baseline with VRX stopped.

## Interpretation and next action

The capture/inference/render pipeline ran for a full minute with visible headset
rendering and clean application shutdown. Under this workload, depth estimation
was substantially slower than the earlier desktop/static-image cases. GPU
contention is a plausible cause, but this run does not isolate its contribution.
The two fallback episodes show that the provisional 250 ms source-depth lag
guard was reached; abrupt flat/stereo switching needs a subjective comfort check.

User feedback was positive overall, but the picture followed head movement. The
requested default is a screen that stays in place, with `=` to reset forward and
an optional head-following mode. The SteamVR dashboard was also open at startup;
having to pick up a controller to dismiss it was an explicit usability failure.
These are the immediate priorities. Separate ratings for HUD readability, mouse
responsiveness, edge artifacts, and sustained comfort were not collected.

After the screen/startup fixes, prioritize the relevant comparison: flat versus stereo for visual
issues, and reduced game/depth GPU workload for latency. Note that `--no-warp`
currently still runs inference: it is a visual comparison, not a no-AI performance
baseline. Add a true no-inference baseline before attributing game slowdown to AI.
Also log selected HWND/PID and make executable-based source selection available.
