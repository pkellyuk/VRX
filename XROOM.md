# The room (xroom branch)

From play: "could a part of this look like the ambilight is reflecting off of walls in
a room?" With the **Room** slider above 0, VRX puts you in a dark room whose walls,
floor and ceiling are lit by the picture and the ambilight, like a television or a
cinema screen in a dark room. The slider sets how pale the walls are (0 is off; 30-60%
looks like a cinema).

## How the design was chosen

Three designers worked independently, each with a different priority: physical
correctness, the lowest cost, and what you would notice in the headset. Two judges
checked each design against the code and scored all three. Both picked the physical
design and grafted on the other two's best ideas. From the lean design: a separate
shader variant so the room-off pass is unchanged, two lightmap passes, and the world
colour for an eye outside the room. From the experience design: one slider, a black
footprint under a flat screen, and the real floor from SteamVR.

## Geometry (`room.h`)

The frame is the screen's own, levelled: while the room is on, the screen keeps only
its heading, because a tilted floor would be obvious.

- **Side walls:** clear of the whole glow and at least a metre from the viewer. The
  room is at least 4 m wide.
- **Floor:** SteamVR's STAGE floor when it gives a plausible height, otherwise a seated
  guess. It is always at least 20 cm below the screen. It is looked up again when the
  screen is recentred and after SteamVR's "reset seated position" (once the change
  takes effect).
- **Ceiling:** above the whole glow, at least 2.5 m above the floor.
- **Back wall:** behind the viewer.
- **Front wall:** the glow's own surface. For a flat screen, that is the plane 2 cm
  behind it where the v1.6 glow quad sits. For a curved screen, it is the glow's
  cylinder (2 cm beyond the screen, round the same axis), continued out to the side
  walls by two tangent planes, the "wings". The glow therefore lies exactly where it
  did, now as light on a real wall.
- **Shortened arc:** only if a small, strongly curved screen would otherwise wrap round
  the viewer.

The room is convex, so a ray from the eye leaves through exactly one face and no
surface shadows another.

For the default screen (5.7 m wide at 3 m) the room is 9.35 x 4.66 x 4.52 m. At 100%
curve the wings meet the side walls level with the viewer's shoulders.

## Light

Everything is in linear display radiance, the space the compositor blends in.

- **Emitters:** the picture is a 16-wide grid of patches (16 x 9 for 16:9), each the
  exact mean of its pixels. The glow is blocks of 8 x 8 texels. Both are treated as
  Lambertian area lights.
  - The LIGHT pass gathers at most 1,024 emitters. A 4:3, square or portrait picture
    has a taller glow texture, and at 8 texels its blocks alone passed that (a 4:3
    picture needed 1,056), so the blocks grow to 16, 32 or 64 texels until everything
    fits: 16 for 4:3, 5:4, 1:1 and 9:16. A 16:9 picture keeps 8 (880 emitters).
- **Direct light:** a surface point p receives E = sum L_j G_j, where G is pi times the
  point-to-patch form factor.
  - Close up it uses Lambert's exact polygon formula. A softened point light would
    ripple by 10-30% on the floor right under the screen.
  - Further than twice a patch's diagonal it uses the disk formula, which is within 3%
    of the exact one where it takes over.
- **Bounce:** one uniform term stands for the light bouncing round the room (the
  integrating-sphere estimate). The screen itself absorbs.
- **Surfaces:**
  - A surface shows L = (albedo / pi) (E + bounce) + shade x the world colour.
  - The world colour becomes the room's house lights: walls 1.0, floor 0.75,
    ceiling 0.6.
  - Albedo: walls reach 60% at Room 100%; the floor is 60% of the walls.
  - On the front wall the glow is added on top. It is light on the wall, so it adds to
    the wall's own light rather than hiding it.
- **Where the floor meets a curved front:** the lightmap's floor and ceiling are
  rectangles, so along a curved front some of their texels lie behind the wall, outside
  the room. They are never seen, but bilinear sampling at the wall's base blends them
  in. Lit where they were, from behind the glow, they were black and drew a dark
  sawtooth along the base of the wall, so they are lit from 1 cm inside the wall
  instead. The strip behind the front is also left out of the floor's and ceiling's
  area in the bounce term.
- **Smoothing:** the screen's light is smoothed with a 40 ms time constant, so the
  walls do not strobe with every cut. The glow is already smoothed.

All of it is worked out once per frame into a lightmap of 6 faces x 64 x 64 (about
7-15 cm per texel on the default room). Both eyes sample it: diffuse light does not
depend on where you look from.

## Passes

1. **EMIT** (`kRoomHlsl` with `ROOM_EMIT`): one 256-thread group per emitter works out
   its radiance. Pixels are decoded through a 256-entry table and summed in the same
   order as the CPU reference, so the two agree exactly.
2. **LIGHT**: one thread per lightmap texel gathers all the emitters (880 for a 16:9
   picture) through group-shared memory.
3. **Eye pass:** the curve shader's code up to its main, then `kCurveRoomHlsl`. Each
   ray hits either the curved screen or, for a flat screen, its black footprint;
   otherwise it gets the face it leaves through, with the glow on the front wall and a
   per-eye dither on room surfaces so dark walls do not band. The plain curve pass is
   compiled from its own text, so it is unchanged.
4. **Layers:**
   - A **curved** screen is one projection layer, as before.
   - A **flat** screen stays the compositor's two quads. The room goes under them as a
     projection layer drawn at half size; the screen's footprint in it is black, so
     any slip between the layers is dark on dark.

GPU time of the room and screen passes is measured with timestamps and logged every two
seconds as "room + screen GPU p50/p95". With the room on, five timestamps split it by
pass, and the line ends with each pass's p50: "emit, light, eye, copy". A curved screen
without the room keeps its two timestamps. The first frame also logs each eye's field
of view, in the form `--bench-fov` takes. The design panel estimated about 0.3 ms per
frame on an RTX 3090. That is an estimate until the log shows it in the headset.

`--selftest --bench-room` measures the same passes offline, with no VR session, at the
PSVR2's 2804 x 2860 eye buffers: the curve alone (A), the curve with the room (B), the
kept v10 eye pass (D, also `--room-v10-eye` in playback) and the flat screen's
half-size room layer (E), each at four views (yaw 0, 30, 60 and 120 degrees, pitch -15)
and the curved ones at 60% and 100% curve. Every case and view draws 30 frames to warm
up and 300 timed ones. It locks the GPU clocks when Developer Mode allows (else, or
with `--bench-boost`, it runs three times for the spread), logs min / p50 / p95 per
pass with each view's screen, room and mixed pixel counts, spot-checks one frame per
view against `room.h`, and writes `room-bench.csv`.

## Tests

- **`TestRoom`** (`playback_test.cpp`):
  - geometry: the default room's size, the curved front's smooth join and chart, a
    close curved screen keeping the viewer inside, a 1% curve being nearly flat;
  - 2,000 random rays each leaving through a face at its chart coordinates;
  - form factors: against brute-force integration (0.5%), against the closed form for
    the back wall's middle (2%), reciprocity, a huge emitter giving pi;
  - energy: all the screen's light lands on the room, within 3%, flat and curved;
  - the emitter budget: 21:9, 16:9, 16:10, 4:3, 5:4, 1:1 and 9:16 pictures all fit, with
    blocks that cover the glow exactly; 16-texel blocks, the partial ones at the edges
    too, average exactly their own texels;
  - the curved front: the strip of floor behind it measured to 1%, every lightmap
    texel lit from inside the room, and the texel behind the wall's base lit like its
    neighbour inside (this and the previous check both fail without the fix);
  - cases: a black picture giving nothing, the house light, mirror symmetry, red on the
    left lighting the left wall, the glow on and off, the smoothing;
  - details: the dither, half floats, levelling, the v10 snapshot.
- **`SelfTestRoom`**, flat and 100% curved, compares the GPU with `room.h`:
  - the emitters identical: 880 with 8-texel glow blocks (flat), 336 with 16-texel
    blocks (curved), so both block sizes run on the GPU;
  - all 24,576 lightmap texels within half-float precision (worst 9.6e-4 relative);
  - the eye pass within 2 bits, for one eye looking up at the screen and one turned to
    a side wall and the floor;
  - `--selftest --dump` writes `room-eyes-flat.ppm` and `room-eyes-curved.ppm`.

## Limits, and what was left out on purpose

- The room needs the fixed screen. It rests while *Screen follows my head* is ticked,
  and `--head-locked` ignores it.
- If a room cannot be built (for example with the viewer behind the screen), the log
  says so once and it is tried again only when the screen, the recentre point, the
  curve or the floor change. The screen stays level while the slider is up either way:
  levelling only when a room was built would move the screen, change the room's
  inputs, and could flip between the two every frame.
- The default screen reaches below a seated player's real floor, so the room's floor is
  below it too. A raised platform (a cinema riser) so that your feet are on the virtual
  floor is the first candidate for a follow-up, once the room has been seen in the
  headset.
- Not in this version: a soft floor reflection of the screen, a gathered first bounce,
  textures, props, per-face colours, and a room outline in the desktop's top view.
