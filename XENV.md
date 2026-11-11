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

## Left out

- **Seats.** Rows of seat backs would break the room's convexity (a ray could hit a seat and
  then the floor behind it), need their own lighting and shadows, and cost GPU time. The
  cinema is a screening room without them; seats are the first candidate for a follow-up
  once the cinema has been seen in the headset.
- The open air's room light, and glass or reflections in the cinema, the open air and the
  lounge (a polished oak floor would be the next to try).
- Separate settings per environment: one Room slider, one room light and one world colour
  serve them all, saved per game as before.
