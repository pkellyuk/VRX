# Environments (xenvironments branch)

From the first players: VRX appeals to people who watch films in VR as much as to gamers,
and one asked for a movie theatre. The room ([XROOM.md](XROOM.md)) stays exactly as it was:
clean, calm and low on distraction. Next to it, the **Environment** list in the desktop's
Room section dresses the same room three more ways:

| Environment | What you see |
|---|---|
| **Room** (default) | The room as before, with its Glass walls, Reflections and Room light. |
| **Cinema** | Burgundy velvet hanging in folds on every wall, tighter curtains either side of the screen with black masking round it, a dark carpet with amber step lights along both aisles (each with a small pool of light), a black ceiling, and a sconce in every bay of the side walls throwing a fan of warm light up the velvet. |
| **Open air at night** | A screen standing in a field under a night sky full of stars, with a line of low hills along the horizon. The screen lights the ground in front of it; the field fades into the hills with distance. |
| **Lounge** | A home theatre: an oak floor of boards with a rug under and in front of you, panelled walls (a dado rail, grooved panels below it, a skirting board) with two lamps on each side wall, six downlights in the ceiling, and timber slats on the dark wall behind the screen. |
| **Dungeon** | Weathered, staggered stone blocks with bevelled joints and damp staining, barred arched alcoves, piers, hanging iron chains, flagstones with drainage grates, ceiling ribs and torch fittings with warm flame and wall halos. |
| **Hell** | Red-black basalt, warped glowing fissures, lava channels in the floor, dark furnace arches with iron bars, massive wall ribs, glowing inscriptions and a hot cracked ceiling. |
| **Cyberpunk neon cinema** | Tiered blue-black seats, graphite acoustic fins, cyan upright lights, magenta rails and ceiling strips, illuminated city displays, control panels and blue aisle lighting. |
| **Starship cinema** | Tiered slate-blue seats inside brushed-alloy bulkheads, illuminated structural ribs, technical panels, ceiling vents, hazard markings and framed space-view ports showing stars, nebula haze and a blue planet. |

Dungeon (5) and Hell (6) use the same convex room bounds as Room, including curved
fronts and floating-window wall constraints. Their architectural relief and fittings
are procedural surface detail, rather than separate collision meshes. Room and Room
light dim decorative emission, including flames, lava, neon, displays and aisle lights.
Room light at zero switches these fittings off; the picture still lights the surfaces.
Materials have no baked-in brightness floor. Sconces and lounge downlights also follow
the unsaturated lamp strength, so they continue to dim across the control's range.
`room_themes.h` shares the material program between C++ and HLSL so the GPU self-test
compares the same materials in both eyes. Details filter over pixel footprints.
The desktop's v19 control snapshot accepts environment values 0–8; older versions
keep their original ranges. `--room-env=dungeon` and `--room-env=hell` also work.

The futuristic cinemas (7 and 8) use the theatre's actual seat mesh, row layout and
front/middle/back seat selection, with their own upholstery, technical flooring and
cyan step lights. `room_future.h` shares their materials between C++ and HLSL.
The starfield is sampled from the view ray, so it stays distant behind the viewports
as the head moves. Use `--room-env=cyberpunk` or `--room-env=starship` from the command line.

Each is lit by the picture and the ambilight exactly as the room is: the same emitters, the
same lightmap, the same eye pass. The Room slider still sets how much the surfaces reflect
(0 is off). The **Room light** is the cinema's sconces and the lounge's downlights and lamps;
the open air has none. **Glass walls** and **Reflections** belong to the room alone and rest
in the others. Everything is per game and applies live; picking an environment while the
room is off turns the room on at 20% so that the choice shows at once. A profile saved before
this keeps the room.

Seats in the cinema were left out on purpose (the room is convex, so a ray leaves it through
exactly one face and nothing shadows anything; seats would end that). See "Left out" below.

## How it is built (`room.h`)

An environment is a value, `kRoomEnvRoom`, `kRoomEnvCinema`, `kRoomEnvOpenAir` or
`kRoomEnvLounge`, in `RoomLook::env`. It changes three things.

1. **The faces' colours.** `kRoomEnvTint` gives each face a colour relative to the Room
   slider's plain albedo (the room: white). The lightmap takes it per channel: a texel is
   `tint x [(rho / pi)(E + flux x bounce) + shade x (world + ambient)]`, and the bounce is
   worked out per channel from each face's albedo times its colour (`RoomShading::bounceRgb`),
   so a red cinema's bounce is red. The open air has no bounce (its light escapes to the sky)
   and adds a night's house light (`kRoomNight`) to the world colour on its field.
   - For the room the colours are 1, the bounce is `rhoBar`'s (`RoomBounceOf`), and the
     night is 0: its lightmap is the formula of before to the bit (`TestRoomEnvironments`
     checks every face).
2. **The geometry, for the open air only.** Its room is at least 16 m wide, reaches 6 m
   behind the viewer and is 8 m high (`kRoomOpen*`): its floor is the field the screen lights.
   The lounge's rug is placed when the room is built (`Room::rug*`): 3 m wide under the
   viewer, from 2.6 m in front of the head to 0.6 m behind it, 0.3 m clear of the walls and
   of the front behind it (a curved front is deepest at its ends, which are the rug's corners).
3. **The eye pass's details** (`RoomEnvSurface`, and `EnvSurfaceH` in `xrapp5.cpp`). Each is
   box-filtered over the pixel's footprint from the rays' exact differentials, as the room's
   frames and tiles are, so none of it shimmers:
   - **Folds, slats and grain** (`RoomFolds`): a cosine of the brightness, box-filtered, which
     multiplies it by `sinc(pi f / P)`; from a footprint of a whole period on it is let go.
   - **The cinema:** walls in folds every 30 cm (+-40%), the front's curtains every 22 cm
     (+-55%), black masking 30 cm round the screen (4% of the velvet's light), amber step
     lights 0.75 m in from each side wall every metre, each with a 15 cm pool of light.
   - **Sconces and lamps** (`RoomSconce`): bodies on the side walls (the cinema's in the
     middle of every bay, 2 m up; the lounge's two a wall, 1.6 m up) lit in the room light's
     colour, each throwing a fan of light on the wall in the wall's own colour, wider the
     further it reaches, rising further than it falls. With the room light off they are dark
     fittings.
   - **The lounge:** boards 18 cm wide and 1.8 m long with 4 mm seams, their ends staggered
     and their tone varying by board (fading out as a pixel nears a board's width), a fine
     grain; the rug in a terracotta field with a cream border; wainscot panels grooved every
     50 cm under a dado rail at 0.9 m and a white skirting board; six downlights in the room
     light's panel; slats every 6 cm on the media wall.
   - **The open air** (`RoomNightSky`): there are no walls, so every ray goes to the field or
     the sky whichever face it leaves by. A ray going down meets the floor's plane and takes
     the floor's lightmap there; past the lightmap's edge, its light above the field's house
     light falls off over 3 m. So the screen lights the field without a seam, even where a
     curved front's wings cross the field beside the viewer. The field fades into the hills'
     colour over 80 m. Above the horizon: a night sky, the world colour's sky on top (as beyond
     the room's glass), stars (below), and low hills (up to 4 degrees, from a sum of three
     sines of the azimuth) whose edge is filtered over the pixel's angle.
   - **Stars** (`RoomStars`): at most one per cell of a 40 x 40 grid on each face of a cube
     round the viewer, in half the cells, kept 0.35 of a cell inside its cell. A star is a
     Gaussian 1.2 mrad wide, widened to 0.7 of the pixel's angle so that it never falls
     between pixels (and so never twinkles as the head moves) but to a tenth of a cell at
     most, so that it ends inside its cell. Its peak drops as it widens, so its light is kept
     (`TestRoomEnvironments` integrates one at two pixel sizes: within 3%). Brightness and
     colour (warm to cool) come from the cell's hash.

## The GPU (`xrapp5.cpp`)

- **Constants:** `RoomConstants` row 16's third value is the environment, and rows 21-31
  (padding before) carry the faces' colours (21-26), the bounce per channel (27), the side
  walls' lights (28-29), the lounge's rug (30) and the open air's night (31). The 512-byte
  buffer is unchanged in size.
- **LIGHT pass:** the texel formula above (one multiply and one add more than before).
- **Eye pass:** an environment other than the room needs the look variant (`RoomLookOn`), so
  the plain room pass is untouched. `LookColour` sends every face, the front included,
  through `EnvSurfaceH` before the glow is added (the glow is light on the wall). The new
  HLSL is a third string literal (the VS 2022 compiler limits one to 16,380 characters).
- **Desktop control v18:** the environment is the snapshot's last value (0-3); a v17
  snapshot leaves the room. `--room-env=room|cinema|open-air|lounge` sets it on the command
  line. Changing it builds the room again (it is in the room's key: the open air's room is
  larger, and the rug and the walls' lights are placed with it).

## Tests

- **`SelfTestRoom`** (`--selftest`) adds six cases, {flat, curved} x {cinema, open air,
  lounge}, each with the room light at 50%, in two dispatches that between them see each
  environment's details. Each compares the emitters, all 24,576 lightmap texels (within
  half-float precision) and every eye pixel (within 2 levels) with `room.h`, and requires the
  details on screen: the cinema's masking, sconces, their light and step lights; the open
  air's sky, hills, field and stars, and only its field lit; the lounge's rug, seams,
  downlights, lamps and their light. First run on an RTX 3090: 0 pixels more than 2 levels
  off in all twelve cases. `--selftest --dump` writes `room-eyes-{flat,curved}-{cinema,
  openair,lounge}-{a,b}.ppm`.
  - With no headset, SteamVR names no GPU; the self-test then uses the high-performance GPU
    (it needs no VR session).
- **`TestRoomEnvironments`** (`playback_test`): the room's texels bit for bit the formula of
  before and its constants unchanged; the cinema's colours, redder bounce, resting glass and
  reflections, sconce placement; the open air's larger room (flat and curved), no walls,
  bounce or room light, its field's night; the lounge's rug (also behind a curved screen) and
  lamps; unknown environments falling back to the room; folds against a brute-force box
  filter, their mean, gain and period; the sconces' bodies and fans; a star's light kept as
  the pixel grows; the horizon meeting the field, the field meeting the floor's lightmap with
  no seam at its edge and falling off past it; the masking, the rug and the night sky as
  surfaces. `TestDesktopControl` checks v18 (the environment parsed, a v17 snapshot in the
  room, a missing, unknown, negative or trailing value and `VRX 19` rejected).
- **The desktop smoke test:** the snapshot's last value for every profile, old profiles in
  the room, validation (0-3), and the list: the room by default; the cinema resting glass and
  reflections (keeping their values); the open air resting the room light; picking one with
  the room off turning the room on at 20%, and the room itself never doing so; resting with
  the room while the screen follows the head; saved and put back with each profile.

## Cinema with seats (xenvironments-cinema branch)

A fifth entry, **Cinema with seats**: the cinema's velvet, masking and sconces round tiered
rows of seats, and you in one of them. It replaces the seatless **Cinema** in the list: the
plain cinema looked unfinished beside it, so its item is hidden (`Visibility="Collapsed"`,
keeping the list's numbering), a profile saved with it opens in the cinema with seats
(`Profile.ShownEnvironment`), and the engine still draws it for a control file or the
command line. A **Seat**
list (Front row, Middle, Back row) appears with it, and the seat sets the screen's distance,
as in a real cinema. While you are seated the Distance slider rests, keeping its value, and
shows the seat's distance; Width still sets the screen's size.

The hall is fixed: its rows are laid out for the default 5.7 m screen whatever the screen's
width (`RoomTheatreSeat`, `kRoomHallWidth`; `Profile.SeatDistance` in the desktop), so the
Width slider makes the screen look bigger or smaller from your seat, as a bigger or smaller
screen in the same cinema would. The hall widens only round a screen wider than 5.7 m.
(First laid out for the screen's own width, the slider moved you back as it widened the
screen, so the screen looked the same size and only the room seemed to grow.)

| Seat | Distance (widths of a 5.7 m screen) | Distance |
|---|---|---|
| Front row | 0.8 | 4.56 m, row 1 of 8 |
| Middle (default) | the row nearest 1.3 | 7.49 m, row 4 of 8 |
| Back row | 2 | 11.4 m, row 8 of 8 |

### The stands (`room.h`)

- **Layout** (`RoomTheatreLayout`, and `Profile.TheatreDistance` in the desktop, the same
  float arithmetic, both tested on the same numbers): for a screen W wide, rows from 0.8 to
  2 W, about a metre apart (0.95 m at least, four rows at least). The middle seat is the row
  nearest 1.3 W, never the first or the last. The hall uses it for W = 5.7 m.
- **Round the viewer** (`BuildRoom`): the viewer's row runs through the head, wherever it
  is. Each row is 0.36 m above the one in front, the viewer's tread 1.15 m below the eyes (a
  seated eye height). The floor in front of the first row (the stage) is at least 0.15 m below
  it and below the screen; the ceiling is 2.6 m above the last row; a 0.8 m walkway runs
  behind it; the side walls leave a 1.1 m aisle each side of a block of seats at least 0.6
  times the screen's width (5.7 m at least) each side of the middle.
- **Seats**, every 0.6 m along a row, lined up on the viewer, your own among them (it fits
  round a seated head: the cushion 0.65 m below the eyes, the armrests 0.3 m to each side,
  the back 0.3 m behind and its top 0.15 m below; a head leaning into its own seat still sees
  it - a ray starting inside a seat's bounding box tests its boxes): a cushion (0.48 x 0.1 x 0.5 m, 0.4 m up), a back (0.5 x 0.58 x
  0.12 m) and an armrest (side standard) at each edge, 0.66 m high. The back's top and each
  armrest's are round: a half cylinder as wide as the back is thick (6 cm radius, along the
  back) or the armrest is wide (3.5 cm, along it), flat at its ends.
- **Boxes:** the stands are nothing but boxes. Each row is a block one riser deep under its
  tread (the first row's reaching below the floor) - its top is the tread, its front the
  riser; a ray from the eye cannot reach anything lower without entering the row in front.
  Each seat is its four boxes behind a bounding box; the back and the armrests are each a
  box below their round top (`RoomSeatPartEnter`: the whole box is tested first, and only a
  ray entering it above the top's axis tests the half cylinder, `RoomSeatCapEnter`).
  `RoomStandsTrace` (the CPU's full trace) tests every box in one fixed order and keeps the
  strictly nearest. The stands come before the room's faces in the classification (they lie
  inside it) and are new sample kinds, 9 to 50: the part (tread, riser, cushion, back,
  armrest, the back's round top, an armrest's) and the face's normal (a round top's is 2;
  the shading works out the curve's), with the ray's t in the sample's u.
- **Drawn, then tested exactly** (`BuildRoomStandsMesh`, `RoomStandsSamples`): the GPU draws
  the boxes as triangles (9,662 for the default room's middle seat; each round top as 12 or
  8 flat strips inside the curve, at most 0.7 mm off it, and half-disc fans at its ends; an
  armrest two seats share drawn once) into an id buffer per
  eye at half size, with a depth test: each texel the code of the face seen along the ray
  through the corner its 2 x 2 pixels share (the object, which of its boxes, the face's
  kind). The eye pass reads each pixel's texel and its eight neighbours. All nine the same:
  every one of the pixel's four rays meets that face (or nothing), at its plane - one
  division - or a round top's curve. Otherwise each ray tests the objects the nine saw, exactly, from the lowest. The
  CPU reference does the same with rays cast through those corners. A feature thinner than
  two pixels between two texels can be missed; everything else is exact, and the anti-
  aliasing is the room's own four rays.
- **Light** (`RoomStandsColour`): the screen as one large area light, its picture's mean (a
  "coarse" patch worked out by EMIT after the room's emitters, blended in like the screen's
  patches, and left out of the lightmap), and the room light, by the disk formula; the bounce
  of their flux; the house light. The eye pass reads the two from group-shared memory. A seat
  back's side towards the screen is lit by the picture, its other side only by the room
  light and the bounce. The stands' carpet is the hall's (below), the seats red upholstery
  with dark armrests. A tread is 60% darker under a seat, and in the aisles each tread's
  front edge has an amber step light; both are box-filtered over the pixel's footprint.
- **The carpet** (`RoomCarpet`): a cinema carpet on the floor in front of the first row, the
  treads and the risers - a gold lattice of diagonal lines 0.3 m apart, a teal diamond with a
  gold dot at its heart in each cell, on burgundy - box-filtered over the pixel's footprint so
  it does not shimmer, and averaging to the floor's tint from afar (the tint is the carpet's
  mean, so the lightmap's bounce is the pattern's; tested). The floor's lightmap is
  multiplied by the pattern over the tint, as the lounge's rug. It added about 0.1 ms to the
  curved eye pass.
- **The seats' look** (`RoomSeatShade`), so they do not look made of boxes: the round tops'
  normals follow the curve, and every other edge is shaded round (the normal tilting over
  4 cm on the cushion, 2 cm on the back, 1.2 cm on an armrest); creases are darker where the
  cushion meets the back and the armrests; the backs are sewn in four vertical channels, each
  bulging between its seams, with piping under the round top; the velvet has a pile (3.5 cm
  value noise, fading out where a pixel covers its cells) and a sheen (brighter where it
  turns away from the eye); each seat is up to 7% lighter or darker than the next; the
  armrests' tops are polished. The seats' ambient light comes more from above than below.
- **Depth:** in this environment the warp's scene depth runs 0.4 to 4 times the screen's
  distance (1.2 to 12 m at the default 3 m, so nothing changes there; elsewhere it stays 1.2
  to 12 m). The picture's depth stays round the screen at every seat, and a farther seat sees
  it more gently, as in a 3D cinema.

### Speed

`--selftest --bench-theatre` times the cinema with and without its seats at the middle seat
(`BenchTheatre`, also the end of `--bench-room`), at the PSVR2's 2804 x 2860 eye buffers, on an
RTX 3090, uncontended. What the seats add to the eye pass, p50 over the four views:

| Version | Curved 60% (full size) | Flat (half-size room layer) |
|---|---|---|
| Every ray walking the rows' cells, testing the seats it crosses | +5.4 to +7.1 ms | +1.1 to +1.4 ms |
| The boxes tile-culled per 8 x 8 pixels, every ray testing its tile's | +8.2 to +9.9 ms | +1.8 to +2.0 ms |
| Drawn into a full-size id buffer, tested from its neighbourhood (12 coarse patches) | +1.1 to +1.9 ms | +0.39 to +0.55 ms |
| ... the id buffer at half size, the light from one patch | **+0.67 to +0.92 ms** | **+0.27 to +0.38 ms** |
| ... round tops and the seats' look (minimum, see below) | **+1.15 to +1.45 ms** | **+0.47 to +0.58 ms** |

The tracers spent their time on the work itself - four rays a pixel, each through several
rows and seats - which no tuning of the walk brought down (experiments: one copy of the
tracer in place of eight inlined, a loop over the rays, fewer registers, the seats' boxes
behind bounding boxes and in the ray's order). Drawing the boxes costs the GPU almost
nothing; of the "half size" line about 0.27 ms is the raster and reading it, 0.1 ms the
rays' tests and the rest the seats' light.

The round tops and the look were timed while the player was running in VR (contended), so
the last line is the minimum over the runs, not the p50; the line before it, timed the same
way, was +0.65 to +0.89 ms curved and +0.27 to +0.38 ms flat. Of the difference about 0.3 ms
is the round tops' tests and 0.25-0.35 ms the look (its pile noise 0.05 ms). Looping over the
four rays in the stands' tests and over the four samples' colours (instead of unrolled
copies) took 0.4-0.6 ms off the curved pass; a second, 6 mm pile, testing the straight part
and the round top separately, and looping over a seat's boxes or the rays' classification
measured slower.

### The GPU

- `RoomConstants` grows to 768 bytes (three 256-byte steps, so the ring's CBVs stay
  aligned): rows 32-35 carry the stands, the viewer's row, the coarse patch and the seats'
  columns.
- The emitter buffer has room for the coarse patches after `kRoomMaxEmitters`; the room's eye
  pass reads it as a root SRV (t4), and the id buffer as t5.
- The stands' raster is the engine's first graphics pipeline (`InitStandsRaster`,
  `RecordStandsRaster`): its vertex shader is the eye pass's own projection
  (`StandsProjection`), so a texel's centre sees what its ray would.
- The stands' code is in its own eye pass (`ROOM_STANDS`, chosen when the constants have
  stands), which leaves out the room's glass and reflections and the other environments'
  details; the other eye passes are unchanged (the stands' code, compiled into the look pass,
  had cost every environment 0.3 ms).
- Desktop control v19 adds the seat (0 front, 1 middle, 2 rear) and allows environment 4; a
  v18 snapshot has no theatre and the middle seat. `--room-seat=front|middle|rear` sets it
  on the command line. The seat is in the room's key, so changing it builds the room again.

### Tests

- `SelfTestRoom` adds flat and curved theatres (the middle seat at its distance) in two
  dispatches: ahead over the rows in front, to the right along the seats and the aisle,
  round to the rows behind, and down to the floor under the seats. The GPU draws the stands
  and the eye pass reads them; both match `room.h` with 4 of 196,608 pixels more than 2
  levels off (983 allowed), the coarse patch and every lightmap texel too; the treads,
  risers, cushions, backs, armrests, the floor under the seats, the step lights and the
  masking must all be on screen.
- `TestRoomTheatre` (`playback_test`): the layouts (5.7 m: 8 rows 0.977 m apart, 4.56 / 7.49
  / 11.4 m; a 1 m screen four rows from 2 m; a 10 m screen 13 rows to 20 m); the stands round
  the viewer; rays straight down onto the viewer's own cushion, from a head leaning into
  its own seat onto its armrest, just in front of the row's seats onto the tread, onto the next seat's
  cushion, between two seats onto an armrest's round top, onto a back's round top on its
  crown and 5 cm off it (lower on the curve) and by its square corner (lower still, the
  corner gone), into the gap between two backs onto a back's flat end, back at seat height
  under the next row's seat to the riser after it, ahead and down grazing a round top and,
  steeper, onto a seat back; the whole screen seen over
  the rows in front from the middle seat; the carpet's mean against the floor's tint, from
  afar too, and its gold, teal and burgundy where they should be; the coarse patch as the picture's exact mean, left
  out of the lightmap; the constants; a seat back lit on its side towards the screen, the
  floor darker under a seat, the aisle's step lights.
- `TestDesktopControl` checks v19; the desktop smoke test checks the seat's default, its
  snapshot value, its distances against the same numbers, and the list: the seat only with
  this environment, the Distance slider resting (and keeping its value) while seated and back
  with the screen following the head or another environment.

## Left out

- **The seats' shape:** boxes with round tops on the back and the armrests; the other edges
  are only shaded round (the outline stays square), the backs do not recline, the seats do
  not tip up, and there are no cup holders, rows' letters or audience.
- **Shadows on the stands:** a seat does not shadow the seat behind it, only the floor under
  itself. The stands' light ignores the glow, and the room's lightmap ignores the stands (the
  walls are lit as if the rows were not there).
- The open air's room light, and glass or reflections in the cinema, the open air and the
  lounge (a polished oak floor would be the next to try).
- Separate settings per environment: one Room slider, one room light and one world colour
  serve them all, saved per game as before.
