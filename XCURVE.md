# Curved screen and ambilight (xcc branch)

Two screen options, both per game and both live: wrap the screen around you like a
curved television, and let the picture's edge colours spill into the darkness around
it.

## Why the compositor cannot do the curve

OpenXR has a cylinder layer for exactly this (`XR_KHR_composition_layer_cylinder`),
which would hand the whole job to the runtime. SteamVR does not offer it: its OpenXR
runtime (2.17.10) advertises 43 extensions and that is not one of them, and it exposes
no equirect layer either (`bench/native/openxr/out/xrprobe.exe` prints the list). VRX
therefore gets one **flat** quad per eye, and the curve has to be drawn into it.

## The geometry

The screen is a cylinder of radius `R`, standing vertically, concave towards the
viewer, with the arc's length equal to the width the player chose. Content at `u`
metres along the surface sits at angle `phi = u / R`, so in front of the viewer it is
at

    X(u) = R sin phi                  sideways, metres
    Z(u) = D - R (1 - cos phi)        away, metres (the edges come forward)

and perspective puts it on the flat quad at `P(u) = X(u) * D / Z(u)` horizontally and
`v * D / Z(u)` vertically. One eye at lateral offset `E` sees it a further
`E (1 - D/Z)` sideways, which is the warp's existing disparity term with
`invZ = 1/Z - 1/D` - so the curve costs the shader one extra add per pixel
(`screen_curve.h`, `kWarpHlsl`).

A wrapped screen takes more of your view than a flat one of the same width, so `P` at
the edge is wider than half the screen. Everything is scaled by
`k = (width/2) / P(width/2)`: the quad keeps the size the player asked for, the picture
reaches both edges exactly, and nothing is clipped. Vertically each column then ends up
scaled by `vertMag = (D/Z) / (D/Z at the edge) <= 1`, with the quad made
`k * D/Z(edge)` taller, which leaves the middle of the picture a little short of the
top and bottom of the quad. Those pixels are written transparent, and that is what
bows the outline outwards the way a real curved screen looks from the middle seat.

For the default screen (5.7 m wide at 3 m, 1920 px, 63 mm IPD, focal 1011 px):

| Curve | Wrap | Edges come forward | Edge disparity | Middle scale | Edge stretch | Quad taller |
|---:|---:|---:|---:|---:|---:|---:|
| 25% | 17.5° | 0.22 m | 0.8 px | 0.93x | 1.15x | 0.4% |
| 50% | 35.0° | 0.43 m | 1.8 px | 0.87x | 1.30x | 1.6% |
| 75% | 52.5° | 0.64 m | 2.9 px | 0.81x | 1.46x | 3.6% |
| 100% | 70.0° | 0.84 m | 4.2 px | 0.77x | 1.63x | 6.5% |

100% is a wrap no real screen has; a curved monitor is about 40° (1000R at 700 mm
wide). The disparity from the curve is deliberately small next to the picture's own
depth (24 px across 1.2-12 m) because that is what the shape really amounts to: the
wrap is felt mostly as the edges taking more of your view, which is the "edge stretch"
column above. The curve's disparity is **not** scaled by the 3D strength slider - the
shape of the screen is not the depth of the picture.

A wide screen very close by would otherwise wrap past the viewer's head, so the wrap is
reduced until the edges stay at least 55% of the screen distance away (10 m at 1 m ends
up at 30° rather than 70°).

Both the shader and the CPU reference warp read the same per-column table, built once
per change on the CPU, so `SelfTestWarp` can still compare them pixel for pixel; the
curved cases also use a 3D strength of 1.4 so the curve's own disparity cannot hide
inside the picture's.

## Ambilight

One extra quad layer, 22% of the screen's width larger on every side and submitted
behind the screen, holding a 256 px-wide texture (`ambilight.h`, `kAmbiHlsl`):

- each glow pixel takes the nearest point on the screen's border and averages a 5x5
  patch of the picture just inside it, so every side glows with its own colour;
- brightness falls off with the square of the distance outside the screen, measured in
  **metres** so the corners are not stretched, and reaches zero at the edge of the
  layer;
- the colour is stored already multiplied by that alpha. That is what a compositor
  wants for a blended layer, and it is also exactly right if the runtime ignores the
  alpha, because the world behind the screen is black;
- each frame is blended 12% into the previous glow, so the surround drifts with the
  scene instead of flickering with it.

The taps are whole source pixels rather than filtered samples, so `SelfTestAmbilight`
compares the shader against the CPU reference to the last bit.

## Limits

- Both need the fixed screen. With **Screen follows my head** the picture is the whole
  view, so there is no screen plane to curve and no surround to glow into; the renderer
  logs that it is ignoring them.
- The curve is visible in 3D only with stereo on. With stereo off the geometric stretch
  is still applied, and that alone is a weak cue.
- Compressing the middle of the picture resamples it slightly; the sub-pixel warp is
  what keeps that smooth, so leave it on.
- The glow is built from the captured frame, not from the warped picture, so it ignores
  the stereo shift at the edges (a pixel or two).

## Files

- `bench/native/openxr/screen_curve.h` - the per-column geometry, shared by shader and reference
- `bench/native/openxr/ambilight.h` - the glow's constants and CPU reference
- `bench/native/openxr/xrapp5.cpp` - `kWarpHlsl`, `kAmbiHlsl`, the glow layer, `SelfTestAmbilight`
- `bench/native/openxr/xr_common.h` - `WarpEyeFill` takes the curve table
- `bench/native/openxr/playback_test.cpp` - `TestScreenCurve`, the v8 snapshot
- `desktop/VRX.Desktop` - the **Screen curve** slider (with the arc in the top view) and the **Ambilight** checkbox
