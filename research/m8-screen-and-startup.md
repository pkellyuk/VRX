# M8 — stationary screen and controller-free startup

Date: 2026-09-17

The first Helldivers 2 test received positive visual feedback, but the user wanted
the picture to stay in place and disliked needing a controller to dismiss SteamVR.

## Behaviour

- Default presentation is two eye-specific OpenXR quad layers at the same fixed
  pose in LOCAL space. Initial placement is 3 metres ahead of the eye midpoint,
  aligned to the current head orientation, with the previous angular size/aspect.
- Head rotation and translation leave that pose and size unchanged. `=` places
  the screen ahead again. A held key only triggers once; a request during invalid
  tracking waits for valid views. Keyboard polling neither steals focus nor
  consumes the key, so the game also receives it. The current binding uses the
  keyboard key containing `=`; modified presses of that key also trigger it.
- `--head-locked` restores the previous projection-layer view. Existing
  `--submit-depth`, `--freeze-pose`, and depth A/B diagnostics explicitly select
  that projection path; they are not stationary-screen controls.
- The stereo warp subtracts the inverse screen distance from its inverse-depth
  endpoints. At default strength, warp disparity plus the compositor's screen
  disparity reproduces the original central-view convergence. This remains a
  stereo picture on a screen, not recovered geometry with true novel viewpoints.
- On SteamVR only, after one second of visible playback, a separate worker sends
  one close-only dashboard request. F8 explicitly requests another dismissal
  while VRX runs, without changing game focus. Held keys trigger once; a request
  during a pending network operation is coalesced for processing afterwards.
  It never toggles or repeatedly suppresses the menu. `--keep-dashboard` disables
  automatic dismissal but leaves F8 available. Like `=`, F8 also reaches the game.
  Failure is logged and does not stop playback. No global SteamVR settings change.

## SteamVR integration and limits

The public OpenVR header exposes `ShowDashboard`, but no matching `HideDashboard`:
[Valve header](https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h).
The installed SteamVR dashboard code instead registers `hide_dashboard_requested`
on the `vrwebui_dashboard` mailbox and calls its own `hideDashboard` handler.
Its mailbox client uses `mailbox_send <mailbox> <JSON>` over local WebSocket port
27062. The local-origin handshake was verified against this installation.

`steamvr_dashboard.h` isolates this internal, version-dependent protocol. WinHTTP
uses no proxy and connects only to loopback, with short connection/request timeouts.
It completes a WebSocket close handshake with a one-second close timeout, instead
of abruptly destroying the connection with a close request potentially queued.
No credentials, browser sessions, OpenVR process initialization, or controller
simulation are involved. Sending successfully does not establish that the UI
closed; headset feedback and SteamVR logs are separate evidence. Future SteamVR
changes may require updating or disabling this adapter. This addresses SteamVR's
VR dashboard, not the game's ordinary Steam overlay.

## Validation

- Standalone playback suite passes: persistent pose and size under head movement,
  rotated/translated recenter, held-key debounce, deferred recenter, and disparity
  compensation, together with existing tracking/recovery/resize checks.
- All 12 GPU warp cases and the model-input preparation comparison pass. Two new
  anchored-depth cases cover both hole-fill modes with positive and negative
  displacement relative to the screen plane.
- Full native build passes. A 60-second `--window=HELLDIVERS` run accepted the
  quad submission and sent the dashboard-close request successfully, then exited
  with code 0. SteamVR stayed SYNCHRONIZED: only one frame was drawn, so this was
  not a headset-visible validation. The run captured 2,571 frames, published
  1,581 depth results, and reported zero source drops or invalid tracking frames.
  Raw log: ignored `bench/native/openxr/out/helldivers2-anchored-headset-test.log`.
- The subsequent 90-second Helldivers 2 run reached VISIBLE and FOCUSED. All
  10,672 counted frames were drawn (118.6 fps), with 3,650 captured frames and
  1,053 consumed depth updates (11.7/s). No stale-depth fallback, invalid tracking,
  source drops, or application errors were logged; shutdown returned code 0.
  The dashboard-close request was sent once. FOCUSED preceded the request, so
  that state transition does not prove dismissal. Only initial screen placement
  was logged; no recenter key press was detected. User confirmation of the
  stationary screen, recenter, and menu behaviour remains pending.
  Raw log: ignored `bench/native/openxr/out/helldivers2-anchored-headset-test-2.log`.
- After restarting SteamVR and Helldivers 2, the user reported the menu did not
  automatically close, later disappeared, then returned. The 90-second test
  exited normally (10,727 loop frames, 6,290 drawn; visibility was interrupted).
  Three short stale-depth fallback episodes occurred. No recenter press was
  logged. Raw log: ignored `out/helldivers2-restarted-steamvr-test.log` beside the
  other logs. The earlier send-success log was not proof of dismissal.
- Investigation found no VRX request in SteamVR's dashboard log for that run.
  Holding the connection open delivered a close request and SteamVR logged
  `Hiding`; a later close to the normal mailbox name logged `already hidden`.
  The native adapter now closes gracefully. A standalone native probe returned
  success and SteamVR independently logged the request with `already hidden`.
  This confirms delivery with the corrected adapter, not automatic-startup
  success across all timings. Dismissal now waits one second after visibility
  because this run lost focus shortly after the original early startup request.
  F8 was added as an explicit keyboard fallback; in-headset F8 verification is next.

Next: expose screen mode and key binding in a simple UI, verify keyboard conflicts,
and test tracking-origin changes and other runtimes. Screen size/distance controls
and persistent per-game profiles remain pending.
