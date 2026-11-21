# Room life and music

On `xcreatures`, **Expert → Room → Room life & music** adds six independent switches.
The controller menu's **Room** page exposes the same controls and selectors. Turn on
the room and use a fixed screen first. Changes apply immediately and are saved per game;
base settings and Apply to all carry them too. Older profiles keep their previous settings. Newly introduced effects remain opt-in.

## Controls

* **Listen to playback audio** opens WASAPI playback loopback on Windows' default
  playback output. It hears the output mix, including other applications on that output.
  It never opens a microphone, records a file or sends audio anywhere. Output-device
  changes and capture errors trigger reconnection. The status below the switch shows
  connecting, listening or unavailable. Silence fades the response; animations can still
  run quietly with listening off. Listening stops when the room or VR session stops.
* **Wall & ceiling music animations** projects Spectrum, Oscilloscope, Plasma, Ripples,
  Kaleidoscope, Neon grid, Aurora, Vortex, Lava, Stars, Ribbons or Lissajous onto room
  surfaces. Spectrum uses sixteen logarithmic bands; Oscilloscope uses captured samples.
  Other presets respond to bass, midrange, treble and level. A twelve-checkbox grid enables any combination at once; the result is normalised to keep overlapping layers controlled. Music reactivity ranges from 0% to 300% and changes the audio response independently of effects brightness. Old single-preset profiles migrate to the same selected layer. The floor stays available
  for disco illumination.
* **Animated creatures** enables up to 24 inhabitants. Separate species switches choose
  holographic sharks, small fish, walking robots, jellyfish, manta rays, fireflies,
  turtles and survey drones. Robots and turtles stay on the floor or theatre treads.
  Fish swim, jellyfish pulse, mantas flex, drones hover, and fireflies flutter. Audio
  changes movement speed, animation, colour and glow. A crowded room may admit fewer
  creatures than requested: placement is rejected rather than overlapping geometry.
* **Colourful 3D dust** adds up to 100 small world-space motes, with stereo parallax,
  drifting positions and treble-sensitive colours. These are actual ray/sphere hits,
  occluded by room surfaces, seating, the screen and creatures.
* **Moving disco spotlights** adds three slowly sweeping coloured spotlights. Bass
  changes their brightness. These illuminate room surfaces, seating and creatures.
* **Ray-traced shadows** enables visibility rays for those spotlights. Creature shadows
  trace the same animated procedural shapes as the camera. Seating uses conservative
  boxes including its rounded caps; row platforms cast shadows too. This is software
  ray tracing in the existing D3D12 compute renderer, not a hardware DXR requirement.

**Effects brightness** controls the added visuals, creatures, particles and spotlights.
Population and particle density are separate controls. Shadows have no visible effect
without disco lights. Glass and the room's existing reflections keep their previous
controls; creature reflections are not included in that existing mirror implementation.

Flat screens with creatures or particles use a full-resolution projection containing
both picture and room. This lets an inhabitant swim in front of the picture and be
hidden when behind it. Turning both off restores the original flat quad composition.
Curved screens already share the projection. Floating controller panels retain their
existing compositor priority.

## The directed interaction table

The authoritative lookup is `kLifeRelations` in
`bench/native/openxr/room_life.h`. Like Eloria's `RELATIONS`, each row is the observer
and each column is the observed species. These are deliberate asymmetric relationships;
the table drives generic steering, rather than species-specific movement branches.

| Observer | Responds to |
|---|---|
| Shark | Playfully chases fish; schools with sharks; escorts mantas; investigates jellyfish and drones |
| Fish | Flees sharks, mantas and drones; schools with fish, jellyfish and turtles; follows fireflies |
| Robot | Guards fish; groups with robots; follows fireflies; escorts turtles and drones; investigates sharks and jellyfish |
| Jellyfish | Groups with fish, jellyfish and fireflies; escorts mantas; guards turtles; investigates robots and drones |
| Manta | Escorts sharks and turtles; guards fish; follows jellyfish; groups with mantas; investigates fireflies and drones |
| Firefly | Flees sharks and mantas; groups with fish, jellyfish and fireflies; teases robots and drones; follows turtles |
| Turtle | Flees sharks; follows fish; escorts robots; investigates jellyfish and fireflies; groups with turtles; guards drones |
| Drone | Investigates sharks and mantas; playfully chases fish; escorts robots; follows jellyfish and fireflies; guards turtles; groups with drones |

`Ignore` leaves wandering and collision avoidance active. `School` adds alignment,
cohesion and separation. `Follow` approaches while keeping clearance. `Flee` moves away.
`Tease` steers around the other creature. None of these allow creatures to overlap.
`Escort` aligns with a companion and keeps nearby clearance. `Guard` moves toward a
safe point between a nearby shark and its companion. `Investigate` approaches and
backs away in a curiosity cycle. `Chase` pursues playfully without damage or contact
penetration. Contacts exchange mass-weighted impulses; the collision constraints apply
regardless of the relationship. The original five attitudes remain available.

The table is compiled into the engine; changing it requires a rebuild.

## Implementation and limits

`room_audio.h` owns the playback worker, COM/device lifecycle, 1024-sample Hann-windowed
FFT, sixteen bands, RMS, waveform and bass onset envelope. No audio capture runs on
the XR rendering thread. `room_life.h` smooths signals and simulates bounded creatures
at 120 Hz, capping catch-up after stalls. Motion uses conservative enclosing spheres,
pairwise separation and mass-weighted contact impulses, inflated seating geometry, row blocks, curved room boundaries and
screen exclusion. These are VRX's virtual room and chairs; physical furniture is not
scanned. A headset cannot supply arbitrary real-room collision geometry through this
implementation.

`room_life_shader.h` contains bounded procedural shapes, animated details, normals,
lighting, spotlight visibility, patterns and dust. Bounding spheres reject rays before
shape marching. Effects compose once per eye pixel after the room's existing material
samples. Objects and audio occupy a shared 2560-byte, 256-aligned constant buffer;
the original material offsets stay unchanged. Control snapshot v20 appended flags,
preset, species mask, population, brightness and particle density. V21 adds the visualizer layer mask, reactivity, beam-reaction switch and Vrex switch, with matching native and desktop validation. V1–v20 snapshots remain supported.

Performance depends on eye resolution, creature coverage and GPU headroom alongside
depth inference. Begin with 12 creatures and 40 motes; reduce population/density or
disable traced shadows first if needed. There is no hardware VR frame-rate claim until
tested on the target headset. First-run shader compilation can take several minutes for the detailed model variants;
release shader prewarming includes the updated variants.

## Verification

* `dotnet build desktop/VRX.Desktop/VRX.Desktop.csproj`
* `VRX.Desktop.exe --smoke-test`: settings persistence, v20 snapshot, validation,
  desktop controls and controller bindings, alongside the existing application checks.
* `bench/native/openxr/test-room-life.bat`: silence/tone FFT checks, 30-second collision
  soaks in all nine environments with flat and curved screens, species filtering,
  toggles, contact bounce, directed relationships, GPU creature/preset galleries, finite pixel checks
  and front/behind-screen occlusion. Gallery PPMs go to the ignored native `out` folder.
* `bench/native/openxr/out/room_life_test.exe --audio`: also opens the actual default
  playback endpoint and checks immediate signal clearing when disabled.
* `bench/native/openxr/test-playback.bat`: existing capture, depth, screen and room checks.
* `xrplayer.exe --warm-shader-cache=PATH`: compiles every production shader variant.

Before release, check live playback and output-device switching, all six switches,
species and preset selectors from both desktop and controllers, flat/curved stereo
occlusion, walking near theatre seating, floor contact, profile switching and frame
timings at the target headset resolution. Include silence, no playback device,
head tracking loss, room resizing, recentering and turning the room off during capture.


## Controller reactions and Vrex

Enable **Creatures react to controller beams** to make the nearest visible inhabitant
notice a tracked controller. Pointing attracts curious robots and drones; fireflies
circle the beam. Triggering startles fish, sharks and mantas into moving away; turtles
pause. Responses decay when the beam leaves, tracking is lost, controllers are disabled
or a panel, screen, wall or chair blocks the ray. The regular menu and screen controls
retain their existing actions. No artificial controller poses are generated.

**Vrex — jetpack robot companion** is separate from the ordinary population and species
mask. He follows the closest tracked controller beam with a damped spring, momentum,
and hand-selection hysteresis. His target stays above the beam and before blocking
surfaces; collision-safe sliding keeps him out of room furniture and walls. When no
controller is available he floats near the viewer. Trigger-point for a reaction bubble.

Vrex recreates the approved teal PersonaBot mascot as procedural 3D geometry: rounded
head and body, navy visor, happy eyes, pink cheeks, centered antenna, headphone ears,
soft limbs and twin jetpack exhaust. The design reference is the PNG artwork at
`PersonaBot/src/PersonaBot.UI/Assets/personabot-logo.png`. His chest carries a
sampled version of `docs/vrx-logo.png`. Short preset bubbles cover greetings, beam
following, music, controller reactions and readiness. These are preset companion
behaviours, without conversational AI. Badge and bubble orientation is verified with
the gallery camera facing the same direction as the headset.
The model source is `room_creature_models.h`; the editable badge/speech generator is
`desktop/assets/make_vrex_branding.py`.

When Vrex is following one controller, hover the other beam near him briefly to open
two choices: **Hello** and **What are you doing?** Trigger-press and release on a choice
to get a preset reply. Hello brings a wave and wink; the second reply describes riding
the beam and adds a shrug. Hover and press states are visible. The menu stays available
while targeted and closes after the pointer leaves, tracking is lost, or Vrex is off.
Foreground panels, the screen and room furniture block picking. The free beam's helper
target prevents clicks reaching a browser, settings panel or game behind him.

Vrex also bobs gently, moves his limbs, turns and points along travel, and occasionally
does an eased idle spin. The existing Vrex switch controls these companion features.
The menu uses two English bitmap labels in `desktop/assets/vrex-menu.en.json`; the
branding generator builds them alongside the speech atlas. Screen recognition, GPU
vision models, conversational replies and automatic web help are later stages and are
not implemented in this build.

All species now use simulation-driven gait clocks and smoothed heading changes. Their
models add fins and mouth/gill detail, layered jellyfish bells and tendrils, manta wing
motion and spots, firefly legs/antennae, turtle shell scutes, and drone arms/rotors/lenses.
Ground creatures roll their enclosing collision support smoothly over theatre step
edges rather than jumping their full body height at the tread boundary.

## Menus and appearance

Room settings are organized into six categories: Environment & seating, Surfaces &
lighting, Music visualizations, Creatures, Vrex, and Atmosphere effects. The desktop
uses collapsible groups; the floating Room menu uses category cards opening separate
pages with breadcrumbs, Back navigation and independent remembered scroll offsets.
Selectors and colour editors return to their owning category rather than the root.

The floating menu remembers a separate scroll offset for every settings/choice page,
including returning from a colour selector. The thumbstick scroll rate is five times
its previous value, including other floating panels. Settings scrolling accumulates
every input packet, smooths motion at up to 90 Hz and supports trigger-dragging the
scrollable contents with inertial release. Sliders retain their direct manipulation.
Home uses icon cards and controls use rounded surfaces and switch tracks. A persistent
3D toggle is available at the top of desktop and floating settings. Floating controls,
keyboard keys and browser chrome dim on press and retain a 150 ms release pulse;
browser page clicks also show a brief pointer ring.

Every panel has a Close icon at the top right. Settings has an icon-only Back control
at the top left (disabled at the root), a breadcrumb in place of the instruction line,
and a uniform three-column action grid. The native title-drag hit area excludes both
title-bar controls. Slow software blur effects were replaced with layered border
strokes; settings uses panel-resolution rasterization during scrolling and restores
supersampled text when settled. Bulk image copies and releasing the IPC mutex during
OpenXR action synchronization reduce input contention. The smoke test records scroll
render timings in `desktop/out/floating-panel-performance.txt`; these timings exclude
headset contention and are not headset frame-rate guarantees.

## Floating keyboard and Windows dictation

Tab and PC keys sit on the left; Enter sits on the right at four times its former
width. Voice input uses the installed Windows desktop speech recognizer and selected
microphone. The inline microphone picker refreshes available inputs and restarts
recognition when switching while listening. Capture stops when the keyboard closes,
controllers turn off, or the input target changes. Audio uses a bounded in-memory PCM
buffer and is never saved as a recording. Missing recognizers or unavailable devices
produce an inline status message. The smoke test feeds synthesized speech into the
real Windows engine without opening a microphone or playing sound.
The PCM reader collects complete requested blocks across capture packets, avoiding
short reads that Windows speech can interpret as EOF. Its test includes split packets
and timed, microphone-sized synthesized input, rather than only preloaded clips.

Under **Themes**, choose Default, Robots, Dinosaurs, Fish tank,
Hell, Heaven, Dark, Light or Grape. Floating panel background opacity ranges from 0% to
100%. Themes decorate the settings, keyboard, snapshot and browser chrome; content and
controls stay readable. Browser pages and captured images retain their own colours.
A separate desktop theme applies a restrained palette without the floating decorations.
Themes is its own section in both desktop settings and the floating settings menu.
The quick-control grid can be rolled up using the chevron beside the breadcrumb,
leaving navigation available and increasing the content viewport. Scrollable settings
show a slim position indicator with up/down cues when more content is off-screen.
The browser panel's address bar has its own Clear button. It clears and focuses the
panel URL for keyboard/dictation input without changing the currently loaded page.

Website microphone, camera and general permission requests appear inside the browser
panel with Allow and Deny controls. Choices persist in the browser profile's
`site-permissions.json`, separately for each capability, requesting origin and embedding
page origin. Unanswered requests are cancelled on navigation or panel closure. **Site
access** lists saved choices; **Reset and reload** forgets the site's VRX decisions,
resets corresponding Chromium content settings where available and reloads the page.
Opaque origins and unknown permission flags are not granted. Windows privacy settings
still apply. Browser smoke tests use Chromium's synthetic media devices and cover real
media and notification callbacks without opening a real microphone or camera.

## Browser-based game help prototype

The floating settings action grid includes **Help with this game**. On first use,
choose a provider and allow screenshot sharing in **Game help settings** (also
accessible from the main floating settings page). Google AI Mode is the default;
Google Lens, Microsoft Copilot and Google Gemini are the other choices. These are
fixed, major-provider destinations rather than user-supplied upload URLs.

Once allowed, pressing Help requests one fresh image from the existing native screen
capture pipeline. Capture serials in snapshot metadata correlate that image with the
request, avoiding confusion with ordinary snapshots. VRX encodes a JPEG at up to 1920
pixels on its longest edge, automatically attaches it through the provider's regular
browser upload control, fills a question asking for scene-specific help and useful
guide links, then submits. There is no file picker, new local model or paid API key.
The source is the current VRX screen image, which can include desktop content when
that is the selected VRX source. It excludes VRX's compositor panels.

Screenshot sharing is opt-in, remembered independently per provider in
`%LOCALAPPDATA%/VRX/game-help.json`, and can be revoked in Game help settings. It is
separate from website microphone/camera grants. Attachment injection checks the
approved HTTPS destination in both the application and the receiving document; login
pages, unrelated redirects and lookalike domains do not receive the image. Image
buffers are cleared after submission or closure. No continuous screen feed is sent.

First-use site consent, sign-in, service limits or CAPTCHA remain the site's own UI.
VRX waits for setup and offers **Retry help** if the page is not ready. Existing chat
drafts are preserved. Free consumer services can change their pages, access rules and
limits; this is a browser integration prototype, not a guaranteed provider API.

Live tests with a synthetic Sokoban image verified automatic image-plus-question
submission through Google AI Mode and Google Lens. Copilot and Gemini are available
as provider choices, but account-authenticated handoffs have not been verified.
The ordinary smoke suite uses a loopback fixture to test consent, image attachment,
question submission, one-upload behavior and destination checks. A separate
`--smoke-test --probe-game-help` run uses an isolated browser profile and synthetic
images to probe the public provider pages; it never captures the user's live screen.
These choices are app-wide, persist in `app-settings.json`, and apply live.

See `docs/INTERNATIONALIZATION.md` for the proposed language rollout, translation review,
keyboard/font work, RTL requirements and the remaining English-only Vrex atlas work.
