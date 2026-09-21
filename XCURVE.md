# Curved screen and ambilight (xcc branch)

Two screen options, both per game and both live: wrap the screen around you like a
curved television, and let the picture's edge colours spill into the darkness around
it.

## The curve, second attempt

**First attempt (rejected in the headset).** The compositor draws a quad layer as a
flat rectangle, so the first version worked inside it: each column of the picture moved
to where a cylinder would project it, carried the cylinder's own disparity, and was
scaled vertically. In play it looked like the game squashed into the middle of a flat
display, and that is what it was. The quad's outline stays a flat rectangle at the
screen distance in both eyes, and that outline beats a few pixels of curve disparity
(8 px between the eyes at the most). It could not have worked.

**Why VRX draws it itself.** OpenXR has a cylinder layer
(`XR_KHR_composition_layer_cylinder`) that would hand the job to the runtime, but
SteamVR's OpenXR runtime (2.17.10) does not offer it: it advertises 43 extensions, and
neither the cylinder nor the equirect layer is among them
(`bench/native/openxr/out/xrprobe.exe` prints the list).

**What it does now.** A curved screen is ray-cast for each eye, from that eye's tracked
position, into eye buffers at the runtime's recommended size, and submitted as a
projection layer with the true eye poses (`kCurveHlsl`, `screen_curve.h`). The outline,
perspective, both eyes' views and the parallax when you move your head all come out of
the geometry. The compositor then only has to reproject, as it does for any VR game.

The screen is a vertical cylinder, concave towards you, whose arc length is the width
you chose. In the screen's own frame (origin at its middle, z towards you):

    P(phi, y) = (R sin phi, y, R (1 - cos phi)),   |phi| <= wrap / 2,   R = width / wrap

Each eye's picture is still that eye's depth-warped image, laid on the surface, so the
picture's own depth rides on top of the curve. Four rays per pixel on a rotated grid
smooth the outline; where all four agree (almost every pixel) the picture is sampled
once. A flat screen (0%) still uses the compositor's quads, which need no resampling.

For the default screen (5.7 m wide at 3 m):

| Curve | Wrap | Radius | Edges come nearer | Edge seen at | (flat screen) |
|---:|---:|---:|---:|---:|---:|
| 25% | 17.5° | 18.7 m | 0.22 m | 45.6° | 43.5° |
| 50% | 35.0° | 9.3 m | 0.43 m | 47.5° | 43.5° |
| 75% | 52.5° | 6.2 m | 0.64 m | 49.4° | 43.5° |
| 100% | 70.0° | 4.7 m | 0.84 m | 51.1° | 43.5° |

100% wraps further than any real screen; a curved monitor is about 40° (1000R at 700 mm
wide). A wide screen very close by would otherwise wrap past your head, so the wrap is
reduced until the edges stay at least 55% of the screen distance away (a 10 m screen at
1 m ends up at about 30°).

Cost: one extra pass over both eye buffers at the recommended size (four ray casts and
usually one texture sample per pixel), plus copying them into the swapchain. The eye
swapchain and buffers are made the first time a curve is asked for, so a flat screen
costs nothing.

## Ambilight

A small glow texture (256 px wide) around the screen, 22% of the screen's width on
every side (`ambilight.h`, `kAmbiHlsl`):

- each glow pixel takes the nearest point on the screen's border and averages a 5x5
  patch of the picture just inside it, so every side glows with its own colour;
- brightness falls off with the square of the distance outside the screen, measured in
  **metres** so the corners are not stretched, and reaches zero at the edge of the
  glow;
- right against the screen it rises from dark over a thin bezel (8% of the margin,
  about 10 cm on the default screen). Without the bezel, the first build made a few
  pixels at the screen's edge flicker in play. The glow was at full brightness right
  against the screen, so the compositor's filtering of the screen's border mixed game
  and glow, and the warped edge columns change from frame to frame. Against a dark
  bezel neither shows;
- the colour is stored already multiplied by its alpha: what a compositor wants for a
  blended layer, and also exactly right if a runtime ignores the alpha, because the
  world behind the screen is black;
- each frame is blended 12% into the previous glow, so the surround drifts with the
  scene instead of flickering with it.

With a flat screen the glow is its own quad layer, submitted first and 2 cm behind the
screen, for any compositor that sorts layers by distance. With a curved screen it is a
flat rectangle just behind the middle of the screen in the same ray-cast pass. The
curved screen's wrapped edges come in front of its inner part, as a real one would.

## Tests

- `SelfTestCurve` (GPU vs `CurvedPixel` on the CPU): the last warped self-test pair on
  a fully curved screen with the glow behind it, seen by two eyes, one turned by 3°.
  0 of 98,304 pixels differ by more than 2 bits (worst 1). `--selftest --dump` writes
  both eyes to `curve-eyes.ppm`.
- `SelfTestAmbilight` (GPU vs `AmbilightReference`): the glow agrees to the last bit.
- `TestScreenCurve` (`playback_test.cpp`): the cylinder, rays to the middle, the edge
  and just past it, the top, a gentle curve's precision (a 1% curve has a 467 m
  radius), the clamp, the glow, the smoothed outline and the bezel.

## Limits

- Both need the fixed screen. With **Screen follows my head** the picture is your whole
  view, so there is nothing to curve and nowhere for a glow; the renderer logs that it
  ignores them.
- The curved screen is resampled once more than the flat one (from the warped picture
  into the eye buffer). At the default size the two are about the same resolution.
- The glow is built from the captured frame, not from the warped picture, so it ignores
  the stereo shift at the edges (a pixel or two).

## Files

- `bench/native/openxr/screen_curve.h` - the cylinder, the ray casts and the CPU reference
- `bench/native/openxr/ambilight.h` - the glow's constants and CPU reference
- `bench/native/openxr/xrapp5.cpp` - `kCurveHlsl`, `kAmbiHlsl`, the eye buffers, the layers, the self-tests
- `bench/native/openxr/playback_test.cpp` - `TestScreenCurve`, the v8 snapshot
- `desktop/VRX.Desktop` - the **Screen curve** slider (with the arc in the top view) and the **Ambilight** checkbox
