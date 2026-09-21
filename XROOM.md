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
    fits: 16 for 4:3, 5:4, 1:1 and 9:16. A 16:9 picture keeps 8 (881 emitters, the
    room light included).
- **The room light** (v11, `--room-light=N` and `--room-light-colour=RRGGBB` until the
  desktop has its controls): a 2.4 x 1.6 m panel flush in the ceiling, over the viewer
  and centred 0.3 m behind the head, kept 0.3 m clear of the walls and the front (the
  default room's spans x -1.2..1.2 m and z 2.5..4.1 m). It is always the last emitter, so
  turning it up needs no rebuild; its radiance comes from the constants and is 0 while
  it is off, which adds exactly +0 to every texel.
  - Level s = (Light/100)^2 and colour c = the decoded colour over its largest channel
    (#FFB46B, 3000 K, is (1, 0.456, 0.147)). The panel emits 20 s c into the lightmap
    and is seen as c min(1, 20 s): the hue is kept, and it is full from 23% up.
  - The ceiling gets none of its light (the panel lies in it); its flux joins the bounce
    like every emitter's. With Glass and Reflections at 0 the bounce's mean albedo is
    v10's to the bit: the panel is part of the ceiling.
  - The eye pass adds the panel's own light over the ceiling, box-filtered over each
    pixel's footprint there (from the rays' exact pixel differentials), so its edges
    are soft over about a pixel. The EMIT pass gives the room light its radiance before
    the glow's branch: as a glow block its first row would lie past the glow's last.
- **Glass walls** (v11, `--room-glass=N` until the desktop has its controls; 0 solid ..
  100 clear). Above 0 the finish is on (flag 16): the side walls, the back wall and the
  ceiling become glass panes in 8 cm frames, and the floor gets 1 m tiles. The wall with
  the screen and the floor stay solid.
  - Frames: uprights in whole bays of about 1.5 m, one in each corner (the default room's
    side walls have 3 bays of 1.507 m, its back wall 6 of 1.558 m), bars along the floor
    and the ceiling, a crossbar 2.4 m above the floor when the room is at least 2.7 m
    high, and ceiling beams in line with the uprights. The frames are opaque and show the
    wall's own light.
  - A pane shows (1 - T) of the wall's light plus T of what lies beyond it (T = Glass/100):
    a sky from 1.6 x the world colour at the horizon to 0.5 x overhead, and a ground at
    the room's floor level, 0.45 x the world colour, with a 1 m grid 50% brighter
    (lines on x = k and z = k, like the floor's tiles) fading into the horizon's colour
    with distance (fog over 50 m). A black world gives a glass room at night.
  - Tiles: 1.2 cm grout lines 35% darker on x = k and z = k (one runs from the screen's
    middle towards the viewer), and a +-3% tone per tile that fades out where a pixel
    covers half a tile. The panel is an opaque fitting in the ceiling (unlit while the
    room light is off).
  - Every pattern is box-filtered over the pixel's footprint on its plane, from the rays'
    exact pixel differentials, so none of it aliases; the flat screen's half-size layer
    filters over its twice-as-large pixels.
  - The bounce weighs each face's finished albedo: a pane's (1 - T) rho (clear glass
    lets light out, so the room gets a little darker), the frames' share (about 8.5% of a
    wall, 10% of the ceiling) at rho, the panel's area at rho, the floor's grout. With
    Glass 0 the bounce is v10's to the bit.
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
2. **LIGHT**: one thread per lightmap texel gathers all the emitters (881 for a 16:9
   picture) through group-shared memory.
3. **Eye pass:** the curve shader's code up to its main, then `kCurveRoomHlsl`. Each
   ray hits either the curved screen or, for a flat screen, its black footprint;
   otherwise it gets the face it leaves through, with the glow on the front wall and a
   per-eye dither on room surfaces so dark walls do not band. The plain curve pass is
   compiled from its own text, so it is unchanged. Its classification (Stage 1 of v11)
   does the same work more cheaply:
   - whether the eye is in the room is decided once per pixel, not per ray;
   - the screen: a slab test against the screen's box (padded 1 mm) first, then the
     cylinder's quadratic with the angle limit as a tangent, and `atan2` only on a hit;
   - the exit: the planes first, with one reciprocal of the ray per axis; the curved
     front only when the box's exit lies in front of it, since the room is convex. Its
     arc gives t straight from the quadratic's root, again with the tangent test;
   - one full exit per pixel: the first ray that misses the screen takes it, and the
     others only check that surface (its plane, the arc or one wing) and that the
     point lies within the room there. At an edge they fall back to the full exit;
   - the uniform denominators (the room's size, the glow's) are reciprocals in the
     constants, rows 17-20, with the screen's bounds.

   The results differ from v10's only by float rounding and at exact edge ties.

   The eye pass is compiled twice. With `ROOM_LOOK 0` it has no code for the v11
   controls and is Stage 1's pass exactly; it draws a room whose controls are all 0.
   With `ROOM_LOOK 1` it adds the room light's panel and the glass (and, later,
   Reflections); it draws a room with any of them on (`RoomLookOn`). In one shader the
   light's code, even with the light off, cost the plain pass about 4% (curved, 60%:
   0.916 against 0.876 ms eye min), through registers. With the light on, the look pass
   costs 0.03-0.04 ms more than the plain one on the curved screen and 0.006-0.009 ms on
   the flat screen's half-size layer.
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
frame on an RTX 3090; the offline benchmark below measures more. On an RTX 3090 at
boost clocks, the room and a curved screen take 1.08-1.20 ms p50 in total with Stage 1
(the v10 eye pass: 1.37-1.99 ms), against 0.87-1.08 ms for the curved screen alone,
across the four views and both curves. The headset's figures are still to come.

`--selftest --bench-room` measures the same passes offline, with no VR session, at the
PSVR2's 2804 x 2860 eye buffers: the curve alone (A), the curve with the room (B), the
same with the room light at 50% (C), the kept v10 eye pass (D, also `--room-v10-eye` in
playback) and the flat screen's half-size room layer without and with the room light
(E, F), each at four views (yaw 0, 30, 60 and 120 degrees, pitch -15) and the curved
ones at 60% and 100% curve. C and F are the spec's Glass 60 / Reflections 40 / Light 50
cases; until Reflections exist they draw Glass 60 and the room light at 50%, and the log
gives what that costs (C minus B, F minus E).
- **Interleaved.** The cases that draw the same screen (the curved one at each curve,
  or the flat one) are drawn in turn, frame by frame, the order rotating each round, so
  other work on the GPU falls on them alike. Every round also draws a probe, the curve
  alone at 60% and yaw 0: the same work in every group. Each case and view gets 30
  frames to warm up and 300 timed ones.
- **Clocks.** It locks the GPU clocks when Developer Mode allows and SteamVR's
  compositor is not running, because the lock slows the headset's own frames too.
  `--bench-lock` locks them anyway. Otherwise, or with `--bench-boost`, it runs three
  times for the spread.
- **Contention.** A row (a case at one curve, view and run) is marked CONTENDED when
  its eye or total p50 is more than 10% above its min, or when the probe's min in the
  same rounds is more than 5% above its best. Other work shared the GPU then, and even
  the min is not reliable. The summary and the acceptance lines use steady rows only.
  They say "not judged" where a comparison has none, and give "meets" or "misses"
  otherwise. SteamVR's compositor drawing for a headset in use can contend every
  curved row. For a baseline, run it with nothing else drawing on the GPU: no game,
  and SteamVR closed or idle.
- **Check.** One extra frame per case and view is read back and compared with the CPU
  reference (`room.h`, or `screen_curve.h` for A) at every 16th pixel. At most 0.1% of
  those may be more than 2 levels off.
  Away from any edge, where only rounding separates the GPU from `room.h` (3 levels at
  most, measured), at most 0.01% may be more than 8 levels off.
- **Output.** It logs min / p50 / p95 per pass with each view's screen, room and mixed
  pixel counts, and the eye pass's time per million off-screen pixels on the min and
  on the p50. It writes all of it, with the probe's times and the CONTENDED flag and
  reason, to `room-bench.csv`.
- It runs only when the self-test passes.

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
  - Stage 1 against the kept v10 code (`room_v10`):
    - the exit on 20,000 random rays from the eye and 20,000 from points inside, in the
      flat, 60%, 100% and shortened-arc rooms: the same face bar 0.05% ties, t within
      1e-4 relative (the point within 1e-6 m for an exit millimetres away), u and v
      within 1e-4;
    - on those rays the exit's surface alone gives the identical hit and no other
      surface claims it; rays 1 mm and 1 cm either side of every edge of the room and of
      each arc/wing join leave where they are aimed, and the neighbouring surface
      refuses them;
    - the room's screen test against `CylinderHit` on 20,000 rays at curves of 1-100%:
      the same hit or miss bar 0.05% within 1e-5 m of the outline, uv within 1e-5;
    - the eye pass pixel by pixel on the self-test's two views, flat and curved, over a
      lightmap lit on the CPU: every pixel within 1/255, 99.9% the same 8-bit value;
    - the constants' rows 17-20 carry the room's reciprocals and the screen's box;
  - the room light: its placement and clearance (also 3 m to the side and in the small
    close curved room), the last emitter in every picture shape's budget (753, 881,
    929, 417, 433, 513 and 657, with the same glow blocks as before), the floor under
    it against the closed form, all its light landing below the ceiling (3%, flat and
    curved), its level and colour, flag 64, every lightmap texel bit for bit v10's with
    it at 0, the bounce's mean albedo v10's bit for bit, the panel's soft edge, and the
    footprints against a central difference;
  - glass: the frames' bays and crossbar (none in a 2.5 m room), uprights in every
    corner; the filtered bars exactly 1 in a bar and 0 in a gap, w/P on average for any
    footprint and smooth across an edge; the tiles' mean, their tone, their pinned hash
    and their lines on x = k and z = k; Fresnel and its mean (the /21); the bounce
    falling as the glass clears, every face's albedo within 0..1, the frames' mean
    cover what the eye pass draws; beyond the glass a black world giving nothing, no
    step at the horizon, the far ground in the horizon's colour and the ground's grid on
    the tiles' lines; a sample on an upright, a pane, a tile and the panel; Glass 0
    giving the lightmap's light on frames and panes alike;
  - details: the dither, half floats, levelling, the v10 snapshot.
- **`SelfTestRoom`**, flat and 100% curved, each with the v11 controls at 0, with the
  room light at 50% (#FFB46B), and with the look (Glass 60 and that light), compares the
  GPU with `room.h`:
  - the emitters identical: 881 with 8-texel glow blocks (flat), 337 with 16-texel
    blocks (curved), so both block sizes run on the GPU; the room light's radiance is
    the constants' exactly;
  - all 24,576 lightmap texels within half-float precision (worst 9.6e-4 relative);
  - the eye pass within 2 bits, for one eye looking up at the screen and one turned to
    a side wall and the floor; the kept v10 eye pass likewise against `room_v10`, and
    how many pixels the two eye passes draw differently;
  - with the room light on: one eye turned round and looking up at the panel (which
    must cover at least 1% of its pixels), one turned left to the left wall, the floor
    and, curved, the front's left wing; the floor under the panel lit by at least 90% of the panel's
    direct light;
  - with the look, two dispatches: A, the screen and the tiled floor, and the right
    glass wall; B, the panel with the ceiling's beams and the back glass, and the left
    glass with the ground and the horizon. From the CPU reference, frames must cover
    over 0.2% of A's pixels and the tiles over 1%, and in B the panel, the ground and
    the sky over 1% each;
  - `--selftest --dump` writes `room-eyes-flat.ppm`, `room-eyes-curved.ppm`,
    `room-eyes-flat-light.ppm`, `room-eyes-curved-light.ppm` and
    `room-eyes-{flat,curved}-look-{a,b}.ppm`.

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
