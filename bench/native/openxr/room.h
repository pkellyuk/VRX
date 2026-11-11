#pragma once
#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "screen_curve.h"
#include "screen_anchor.h"
#include "ambilight.h"
#include "srgb.h"

// The room: a box round the viewer whose walls, floor and ceiling are lit by the
// picture and the ambilight, like a television or cinema screen in a dark room.
// This header is the CPU reference for everything the GPU does (kRoomHlsl, and
// kCurveHlsl compiled with CURVE_ROOM); the self-tests compare the two. XROOM.md
// explains the design, which came from a three-way design panel.
//
// FRAME. Screen-local, levelled (the screen keeps only its heading while the room
// is on - a tilted floor would be obvious): origin at the middle of the screen,
// x right, y up, z towards the viewer. The recentred head sits at about
// (-horizontal, -height, D).
//
// GEOMETRY. The room is convex, so a ray from inside leaves through exactly one
// face and no surface shadows another:
//   * side walls at x = +-X, far enough for the whole glow and a metre each side of
//     the viewer;
//   * the floor at the real floor (SteamVR's STAGE space, else a seated guess), but
//     always below the screen; the ceiling above the glow;
//   * the back wall behind the viewer;
//   * the FRONT wall is the glow's own surface: the plane where the v1.6 glow quad
//     sits (kAmbiBehind behind a flat screen), or, with a curved screen, the glow's
//     cylinder (radius R + kAmbiBehind round the screen's axis at z = R), continued
//     out to the side walls by the two planes tangent to it ("wings"). The glow
//     therefore lies exactly where it did, now as light on a real wall. The arc is
//     shortened only if a strongly curved small screen would otherwise wrap round
//     the viewer.
//
// LIGHT. Everything is in linear display radiance (1.0 = the headset's white), the
// space the compositor blends in. The picture and the glow are Lambertian area
// emitters: the picture as a grid of patches (each the exact mean of its pixels),
// the glow as blocks of kRoomGlowBlock^2 texels. A surface point p with normal n
// receives E(p) = sum L_j G_j(p), G = pi x the point-to-patch form factor: the disk
// formula far away, Lambert's exact polygon formula close up (a softened point
// light ripples by 10-30% near the screen's base). One uniform term stands for all
// the light bouncing round the room (the integrating-sphere estimate). A surface
// shows L = (albedo/pi)(E + bounce) + shade x the world colour, which becomes the
// room's house lights. The glow is added on top where it lies on the front wall.
// The room light (v11) is one more emitter, the last: a panel flush in the ceiling just
// above and behind the viewer, whose radiance comes from the constants (0 while off).
// The ceiling shows the panel's own light over it (RoomCeilingPanel).
// GLASS (v11, the finish: Glass above 0). The side walls, the back wall and the ceiling
// become glass panes in slim frames, and the floor gets 1 m tiles; beyond the glass lies a
// sky and a ground at the floor's level, with a 1 m grid fading into fog, tinted by the
// world colour (RoomEnvironment). The patterns are box-filtered over each pixel's footprint
// (RoomPulse, RoomBar) from the rays' exact differentials. RoomSurface shades a sample;
// the bounce weighs each face's finished albedo (RoomFinishAlbedo).
// REFLECTIONS (v11, Reflections above 0; they turn the finish on too). The glass and the
// floor carry a coat (Schlick, F0 0.6 at 100%) that mirrors the room once: a sample's ray is
// reflected where it meets its face, from each eye's own position, and sees the screen as a
// small mirror picture of the source (RoomMirrorPicture, 128 texels wide) with an edge
// softened over the pixel's footprint, or else the room's other faces as the lightmap has
// them, without their frames, tiles or reflections (RoomReflection). The bounce counts
// the coat's mean reflectance.
//
// All of it is computed once per frame into a small lightmap (kRoomLightmap^2 per
// face) that both eyes sample: diffuse light does not depend on where you look from.
//
// EYE PASS. Four rays per pixel, as the curved screen's own pass. A ray is tested
// against the screen's box before the screen (RoomScreenHit); one that misses it leaves
// the room through a face found planes first, the curved front only when the box's exit
// lies in front of it (RoomExitFast). The room is convex, so a pixel's first such ray
// takes that full exit and its others only check its face (RoomExitOnFace).

static const int kRoomFaces = 6;
enum RoomFace { kFaceFront = 0, kFaceLeft = 1, kFaceRight = 2, kFaceFloor = 3, kFaceCeiling = 4, kFaceBack = 5 };
static const char* const kRoomFaceNames[kRoomFaces] = { "front", "left", "right", "floor", "ceiling", "back" };
static const int kRoomLightmap = 64;            // texels per side of each face
static const int kRoomGlowBlock = 8;            // glow texels per side of one glow emitter, at least
static const int kRoomMaxGlowBlock = 64;        // the EMIT group loops over as many texels as a block has
static const int kRoomMaxEmitters = 1024;
static const int kRoomEmitterFloat4s = 6;       // per emitter in the GPU buffer
static const int kRoomGroupThreads = 256;       // EMIT: threads per emitter's group, and RoomGroupSum's partial sums
static_assert((kRoomGroupThreads & (kRoomGroupThreads - 1)) == 0, "RoomGroupSum's halving tree needs a power of two");
static const float kRoomSideClearance = 1.0f;   // metres from the viewer to a side wall, at least
static const float kRoomMinHalfWidth = 2.0f;    // a room at least 4 m wide
static const float kRoomMinHeight = 2.5f;
static const float kRoomFloorBelowScreen = 0.2f;
static const float kRoomFrontClearance = 0.5f;  // a curved front stays this far in front of the viewer
static const float kRoomMaxAlbedo = 0.6f;       // walls at Room 100%
static const float kRoomFloorAlbedo = 0.6f;     // the floor, relative to the walls
static const float kRoomShadeFloor = 0.75f;     // house light (world colour) per face
static const float kRoomShadeCeiling = 0.6f;
static const float kRoomLightTau = 0.04f;       // seconds: the screen's light is smoothed this much
static const float kRoomPi = 3.14159265358979f;
// The room light (v11): one soft panel flush in the ceiling, just above and behind the
// viewer. It is always an emitter, the last in the buffer (radiance 0 when it is off).
static const int kRoomLights = 1;               // lamp emitters
static const float kRoomLightW = 2.4f;          // the panel across the room (x), metres ...
static const float kRoomLightD = 1.6f;          // ... and along it (z)
static const float kRoomLightBehind = 0.3f;     // its centre this far behind the head
static const float kRoomLightInset = 0.3f;      // at least this far from the side and back walls and the front
static const float kRoomLightMinSide = 0.5f;    // a room with no space for this much either way has no panel
static const float kRoomLightMax = 20.0f;       // emitted radiance at 100%
static const uint32_t kRoomLightDefault = 0xFFB46B;     // 3000 K, "Soft white"
static const float kRoomFootMin = 1e-4f;        // a pixel's footprint on a surface, metres, at least ...
static const float kRoomFootMax = 10.0f;        // ... and at most
// The finish (v11), on while Glass or Reflections is above 0: the side walls, the back
// wall and the ceiling become glass panes in slim frames, the floor gets 1 m tiles, and
// beyond the glass lies a sky and a ground at the floor's level (RoomEnvironment).
static const float kRoomGlassF0 = 0.6f;         // the coat's reflectance face on, at Reflections 100%
static const float kRoomFloorGloss = 0.5f;      // the floor reflects this much of what the glass does
static const float kRoomFramePitch = 1.5f;      // frames: the bays' target width, metres ...
static const float kRoomFrameW = 0.08f;         // ... and the bars' width
static const float kRoomTransom = 2.4f;         // the crossbar this far above the floor ...
static const float kRoomTransomClear = 0.3f;    // ... when that is at least this far below the ceiling
static const float kRoomGroutW = 0.012f;        // the floor's tiles: 1 m, with grout lines this wide ...
static const float kRoomGrout = 0.35f;          // ... this much darker,
static const float kRoomTileVar = 0.06f;        // ... and the tiles' tone varying this much
static const float kRoomEnvZenith = 0.5f;       // beyond the glass, x the world colour: the sky overhead,
static const float kRoomEnvHorizon = 1.6f;      // the horizon,
static const float kRoomEnvGround = 0.45f;      // the ground
static const float kRoomEnvLine = 0.5f;         // the ground's 1 m grid: its lines this much brighter ...
static const float kRoomEnvLineW = 0.03f;       // ... and this wide
static const float kRoomEnvLineMean = 0.0591f;  // the grid's mean cover, 1 - (1 - 0.03)^2 (for reflected rays)
static const float kRoomEnvFog = 50.0f;         // the ground fades into the horizon's colour over this distance
static const float kRoomEnvHorizonDy = 1e-4f;   // a ray this close to level shows the horizon's colour
// Reflections (v11): the mirror picture of the screen, and the reflected ray.
static const int kRoomMirrorW = 128;            // the mirror picture's width, texels ...
static const int kRoomMirrorMinH = 8;           // ... and its height (the source's aspect), at least
static const int kRoomMirrorMaxH = 256;         // ... and at most (the texture's height)
static const float kRoomNudge = 1e-3f;          // a reflected ray starts this far off its face, metres
static const float kRoomReflFootMax = 0.1f;     // the reflected screen's soft edge, metres, at most

// ENVIRONMENTS (xenvironments, XENV.md). The same convex room dressed four ways, chosen on
// the desktop: the room (everything above, unchanged), a cinema, the open air at night and
// a lounge. An environment colours each face (kRoomEnvTint, relative to the Room slider's
// plain albedo: the lightmap and its bounce take it per channel) and draws its own details
// in the eye pass (RoomEnvSurface), all box-filtered over each pixel's footprint like the
// finish. The room's Glass and Reflections belong to the room alone; the open air has no
// room light either.
static const int kRoomEnvRoom = 0, kRoomEnvCinema = 1, kRoomEnvOpenAir = 2, kRoomEnvLounge = 3, kRoomEnvTheatre = 4, kRoomEnvCount = 5;
static const char* const kRoomEnvNames[kRoomEnvCount] = { "room", "cinema", "open air", "lounge", "theatre" };
// Each face's colour by environment (front, left, right, floor, ceiling, back), linear.
static const float kRoomEnvTint[kRoomEnvCount][6][3] = {
    { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } },
    // cinema: burgundy velvet round the screen and on the walls, dark carpet, a black ceiling
    { { 0.85f, 0.2f, 0.22f }, { 0.85f, 0.2f, 0.22f }, { 0.85f, 0.2f, 0.22f }, { 0.42f, 0.36f, 0.44f }, { 0.16f, 0.16f, 0.18f }, { 0.85f, 0.2f, 0.22f } },
    // open air: nothing round you reflects; the ground is a dark field
    { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0.4f, 0.5f, 0.3f }, { 0, 0, 0 }, { 0, 0, 0 } },
    // lounge: a dark media wall behind the screen, warm greige walls, an oak floor, a warm white ceiling
    { { 0.38f, 0.36f, 0.34f }, { 0.95f, 0.86f, 0.72f }, { 0.95f, 0.86f, 0.72f }, { 1.0f, 0.64f, 0.36f }, { 1.0f, 0.97f, 0.92f }, { 0.95f, 0.86f, 0.72f } },
    // theatre: the cinema's velvet, its patterned carpet (the floor's tint is the carpet's mean,
    // RoomCarpet; tested) and black ceiling
    { { 0.85f, 0.2f, 0.22f }, { 0.85f, 0.2f, 0.22f }, { 0.85f, 0.2f, 0.22f }, { 0.437f, 0.201f, 0.203f }, { 0.16f, 0.16f, 0.18f }, { 0.85f, 0.2f, 0.22f } },
};
// THE THEATRE (xenvironments-cinema, XENV.md "Cinema with seats"): the cinema's walls round
// tiered rows of seats, and you in one of them. Your seat - front, middle or rear - sets the
// screen's distance (RoomTheatreLayout): the first row kRoomFrontRow screen widths from it,
// the last kRoomRearRow, and the middle seat the row nearest kRoomMidRow. The rows are a
// metre apart, each kRoomRiser above the one in front; you sit kRoomSeatedEye above yours.
// The hall is fixed (RoomTheatreSeat): its rows are laid out for a kRoomHallWidth screen
// whatever the screen's width, so a wider screen looks bigger from the same seat - it hangs
// in the same hall rather than the hall growing round it. (Laid out for the screen's own
// width, the Width slider moved you back as it widened the screen, and the screen looked
// the same size.) The hall widens only for a screen wider than kRoomHallWidth.
static const int kRoomSeatFront = 0, kRoomSeatMiddle = 1, kRoomSeatRear = 2, kRoomSeatCount = 3;
static const float kRoomFrontRow = 0.8f, kRoomMidRow = 1.3f, kRoomRearRow = 2.0f;   // screen widths
static const float kRoomFrontRowMin = 2.0f;     // metres: the first row at least this far from a small screen
static const float kRoomHallWidth = 5.7f;       // metres: the screen the hall's rows are laid out for (the default width)
static const float kRoomRowPitch = 1.0f, kRoomRowPitchMin = 0.95f;
static const int kRoomRowsMin = 4;
static const float kRoomRiser = 0.36f, kRoomSeatedEye = 1.15f;
static const float kRoomStageDrop = 0.15f;      // the floor in front of the first row: at least this far below its tread
static const float kRoomHeadroom = 2.6f;        // the ceiling: at least this far above the last row's tread
static const float kRoomRearWalk = 0.8f;        // a walkway behind the last row
static const float kRoomSeatPitch = 0.6f;       // seats along a row, centre to centre
static const float kRoomAisle = 1.1f;           // an aisle down each side
static const float kRoomStandsWidth = 0.6f;     // the seats span at least this many screen widths each side of the middle
// A seat, from its row's centre line (z, where a seated viewer's eyes are), the middle of the
// seat (x) and the tread (y): {x half-width, y0, y1, z0, z1} for the cushion and the back, and
// the armrests (side standards) at the seat's edges, kRoomArmHalfW either side of x = +-0.3.
static const float kRoomCushion[5] = { 0.24f, 0.4f, 0.5f, -0.2f, 0.3f };
static const float kRoomBack[5] = { 0.25f, 0.42f, 1.0f, 0.3f, 0.42f };
static const float kRoomArm[5] = { 0.035f, 0.0f, 0.66f, -0.18f, 0.38f };
static const float kRoomChairTop = 1.0f;
static const float kRoomUpholstery[3] = { 0.8f, 0.1f, 0.12f }, kRoomArmColour[3] = { 0.1f, 0.1f, 0.11f };   // relative albedo
// The tops of the seat backs and the armrests are round (RoomSeatCap): a half cylinder as wide
// as the back is thick or the armrest is wide, along the back (x) or the armrest (z), flat at
// its ends. The id buffer draws each as flat strips inside it (BuildRoomStandsMesh): at most
// 0.5 mm off the curve for the back's 12, 0.7 mm for an armrest's 8.
static const float kRoomBackR = 0.5f * (kRoomBack[4] - kRoomBack[3]), kRoomArmR = kRoomArm[0];
static const int kRoomBackStrips = 12, kRoomArmStrips = 8;
// The seats' look (RoomSeatShade): their other edges shaded round (the normal tilting over
// this far from each edge), creases darker where the cushion meets the back and the arms,
// the backs sewn in vertical channels, velvet pile (fading out when a pixel covers its
// cells), each seat's upholstery a little lighter or darker than the next,
// and the velvet's sheen - brighter where it turns away from the eye. (A second, 6 mm pile
// measured 0.1-0.2 ms more at the headset's size and hardly shows there: a pixel covers it
// beyond a few metres.)
static const float kRoomCushionBevel = 0.04f, kRoomBackBevel = 0.02f, kRoomArmBevel = 0.012f;
static const float kRoomCrease = 0.35f, kRoomCreaseW = 0.07f;
static const float kRoomChannel = 0.125f, kRoomStitchW = 0.006f, kRoomStitch = 0.45f, kRoomChannelBulge = 0.35f;
static const float kRoomPileCell = 0.035f, kRoomPile = 0.08f;
static const float kRoomSeatVary = 0.07f, kRoomArmVary = 0.04f;
static const float kRoomSheen = 0.6f, kRoomArmSheen = 0.35f, kRoomArmPolish = 0.25f;
// The theatre's carpet (RoomCarpet), on its floor, treads and risers: a gold lattice of
// diagonal lines kRoomCarpetTile apart (along x or z), kRoomCarpetLineW wide, with a teal
// diamond (kRoomCarpetDiamond across, on the diagonals) in each cell and a gold dot
// (kRoomCarpetDot) at its heart, on burgundy. Relative albedos, the floor tint's scale.
static const float kRoomCarpetTile = 0.3f, kRoomCarpetLineW = 0.012f, kRoomCarpetDiamond = 0.07f, kRoomCarpetDot = 0.02f;
static const float kRoomCarpetBase[3] = { 0.40f, 0.10f, 0.16f }, kRoomCarpetGold[3] = { 0.95f, 0.70f, 0.30f }, kRoomCarpetTeal[3] = { 0.12f, 0.40f, 0.42f };
static const float kRoomUnderSeat = 0.6f;       // the floor under a seat gets this much less light
static const float kRoomNosingW = 0.04f;        // the aisles' step lights: a strip this deep on each tread's front edge
// The screen as one large area light for the stands (RoomStandsColour): a "coarse" patch
// after the room's emitters, its picture's mean, worked out by EMIT like the screen's
// patches. (A 4 x 3 grid of them measured 0.6 ms more at the headset's size, 2 x 2 0.2 ms,
// for light that hardly differs across a seat 4.5 m or more from the screen.)
static const int kRoomCoarseX = 1, kRoomCoarseY = 1, kRoomCoarse = kRoomCoarseX * kRoomCoarseY;
static const int kRoomSeatWords = 16, kRoomSeatBits = 32 * kRoomSeatWords;    // the GPU's per-tile seat mask (StandsCullH)
static const float kRoomBlockBelow = 1.0f;      // the rows' blocks reach this far below the floor (never seen)

// The stands for a screen W wide and a seat: the rows' count and pitch, the first row's
// distance, the viewer's row and the screen's distance from the viewer.
struct RoomTheatre
{
    int rows = 0, viewerRow = 0;
    float pitch = 0, front = 0, distance = 0;
};

inline bool RoomTheatreLayout(float W, int seat, RoomTheatre& t)
{
    t = RoomTheatre();
    if (!(W > 0) || !std::isfinite(W)) return false;
    const int s = seat < 0 || seat >= kRoomSeatCount ? kRoomSeatMiddle : seat;
    const float front = std::fmax(kRoomFrontRow * W, kRoomFrontRowMin);
    const float rear = std::fmax(kRoomRearRow * W, front + (float)(kRoomRowsMin - 1) * kRoomRowPitchMin);
    int rows = std::max(kRoomRowsMin, (int)std::lround((rear - front) / kRoomRowPitch) + 1);
    if ((rear - front) / (float)(rows - 1) < kRoomRowPitchMin) rows = (int)std::floor((rear - front) / kRoomRowPitchMin + 1e-4f) + 1;
    t.rows = rows;
    t.pitch = (rear - front) / (float)(rows - 1);
    t.front = front;
    if (s == kRoomSeatFront) t.viewerRow = 0;
    else if (s == kRoomSeatRear) t.viewerRow = rows - 1;
    else t.viewerRow = std::min(std::max((int)std::lround((kRoomMidRow * W - front) / t.pitch), 1), rows - 2);
    t.distance = front + (float)t.viewerRow * t.pitch;
    return true;
}

// The hall's rows and your seat in them: the layout for a kRoomHallWidth screen, whatever
// the screen's own width.
inline bool RoomTheatreSeat(int seat, RoomTheatre& t)
{
    return RoomTheatreLayout(kRoomHallWidth, seat, t);
}
// Cinema: the walls' velvet hangs in folds, the front's curtains in tighter ones (a cosine of
// the brightness, box-filtered: RoomFolds); black masking round the screen; amber step lights
// along both aisles, each with a small pool of light on the carpet; sconces on the side walls.
static const float kRoomFoldPitch = 0.3f, kRoomFoldAmp = 0.4f;          // walls
static const float kRoomCurtainPitch = 0.22f, kRoomCurtainAmp = 0.55f;  // the front
static const float kRoomMaskW = 0.3f;           // the masking's width round the screen, metres ...
static const float kRoomMaskLevel = 0.04f;      // ... and how much of the velvet's light it shows
static const float kRoomStepInset = 0.75f;      // the step lights: this far from each side wall,
static const float kRoomStepPitch = 1.0f;       // one every metre from the front,
static const float kRoomStepW = 0.12f, kRoomStepD = 0.05f;              // this size,
static const float kRoomStepL[3] = { 0.6f, 0.25f, 0.06f };              // this bright (amber, linear),
static const float kRoomStepPool = 0.15f, kRoomStepPoolGain = 0.12f;    // with a pool this wide and bright
// Sconces (cinema) and wall lamps (lounge): bodies on the side walls lit by the room light,
// each throwing a fan of its light on the wall (RoomSconce), as wide as kRoomHaloW0 + kRoomHaloW1
// x the height from it.
static const float kRoomHaloW0 = 0.08f, kRoomHaloW1 = 0.4f;
static const float kRoomSconceOff = 0.6f;       // an unlit body: this much of the wall's light
// Open air: a night sky (horizon to zenith, linear) with stars and a line of low hills, over
// a field lit by the screen; the field's house light is the world colour plus kRoomNight.
static const float kRoomNight[3] = { 0.01f, 0.013f, 0.024f };
static const float kRoomSkyHorizon[3] = { 0.016f, 0.021f, 0.042f }, kRoomSkyZenith[3] = { 0.0016f, 0.0022f, 0.0065f };
static const float kRoomHills[3] = { 0.003f, 0.0038f, 0.006f };         // the hills' own colour (half the field's added)
static const float kRoomHillFog = 80.0f;        // the field fades into the hills' colour over this distance
static const float kRoomFieldFall = 3.0f;       // past the lightmap's floor, the screen's light on the field falls off over this
static const float kRoomOpenHalfWidth = 8.0f;   // the open air's "room" (its floor is the lit field): this wide each side, at least,
static const float kRoomOpenBehind = 6.0f;      // reaching this far behind the viewer,
static const float kRoomOpenHeight = 8.0f;      // and this high
static const int kRoomStarGrid = 40;            // stars: at most one per cell of a 40 x 40 grid on each cube face,
static const float kRoomStarDensity = 0.5f;     // in half the cells,
static const float kRoomStarCore = 0.0012f;     // radians: a star's size (its light is kept as a pixel grows)
static const float kRoomStarRise = 0.1f;        // radians: stars fade in over this much above the horizon
// Lounge: oak boards along the room, a rug in front of the viewer, wainscot panelling with a
// dado rail and a skirting board, timber slats on the media wall, downlights in the ceiling.
static const float kRoomBoardW = 0.18f, kRoomBoardL = 1.8f, kRoomSeamW = 0.004f, kRoomSeam = 0.45f;
static const float kRoomBoardVar = 0.16f, kRoomGrainPitch = 0.019f, kRoomGrainAmp = 0.05f;
static const float kRoomRugW = 3.0f, kRoomRugFront = 2.6f, kRoomRugBack = 0.6f, kRoomRugClear = 0.3f, kRoomRugBorder = 0.12f;
static const float kRoomRugField[3] = { 0.55f, 0.22f, 0.18f }, kRoomRugEdge[3] = { 0.8f, 0.7f, 0.52f };   // relative albedo
static const float kRoomSkirtH = 0.12f, kRoomDadoY = 0.9f, kRoomDadoW = 0.03f, kRoomDado = 1.12f;
static const float kRoomGroovePitch = 0.5f, kRoomGrooveW = 0.008f, kRoomGroove = 0.35f;
static const float kRoomSkirt[3] = { 1.0f, 0.98f, 0.95f };              // relative albedo
static const float kRoomSlatPitch = 0.06f, kRoomSlatAmp = 0.35f;
static const float kRoomDownR = 0.055f, kRoomDownTrim = 1.3f;          // downlights: radius, an unlit one's trim

// ------------------------------------------------------------------- geometry
struct RoomInputs
{
    float W = 0, H = 0;                 // the screen, metres
    float eye[3] = { 0, 0, 0 };         // the recentred head, screen-local
    float floorY = NAN;                 // the real floor's y, screen-local (NaN: not tracked)
    Cylinder cyl;                       // curved screen, or not (cyl.curved false)
    int env = kRoomEnvRoom;             // the environment (the open air's room is larger)
    int seat = kRoomSeatMiddle;         // the theatre: the viewer's seat (kRoomSeat*)
};

struct Room
{
    bool valid = false;
    bool curved = false;
    float X = 0;                        // side walls at x = -X and +X
    float yF = 0, yC = 0;               // floor and ceiling
    float zB = 0;                       // back wall
    float g = kAmbiBehind;              // the front wall's distance behind the screen's middle
    float R = 0, Rg = 0;                // curved: the screen's radius and the front wall's (axis at z = R)
    float phiA = 0;                     // the front's arc runs to +-phiA, wings beyond
    float sinA = 0, cosA = 1;
    float xa = 0, za = 0;               // the right end of the arc (the wing starts there)
    float sMax = 0;                     // the front's chart runs s in [-sMax, sMax]
    float zSide = 0;                    // where the front meets the side walls
    float margin = 0;                   // the glow margin, metres
    bool floorTracked = false;          // the floor came from STAGE
    float floorWanted = 0;              // that floor (or the seated guess); yF is lower when the screen reaches below it
    bool phiReduced = false;            // the arc was shortened to keep the viewer inside
    float frontFloorArea = 0;           // floor (and ceiling) area in front of a curved front: outside the room
    // The eye pass's reciprocals of its uniform denominators, and the wings' rates
    // (RoomConstants rows 17-19 carry them, so the GPU divides by none of them per ray).
    float tanA = 0;                     // sinA / cosA: the wings' slope, and the arc's angle limit as a tangent
    float wingS = 0;                    // R / (Rg cosA): chart metres per metre of x along a wing
    float invH = 0;                     // 1 / (yC - yF)
    float inv2X = 0;                    // 1 / (2 X)
    float invSide = 0;                  // 1 / (zB - zSide)
    float invFloorZ = 0;                // 1 / (zB + g)
    float inv2sMax = 0;                 // 1 / (2 sMax)
    // The room light's panel, flush in the ceiling: x in [lightX0, lightX1], z in [lightZ0,
    // lightZ1]. All zero, and lightValid false, when the room has no space for it.
    float lightX0 = 0, lightX1 = 0, lightZ0 = 0, lightZ1 = 0;
    bool lightValid = false;
    // The finish's frames: the side walls' bays (uprights at zSide + k pitchSide, one in
    // each corner), the back wall's (at -X + k pitchBack), and the crossbar's height
    // (-1e9 when the room is too low for it, so that its bar covers nothing).
    float pitchSide = 0, pitchBack = 0, transomY = -1e9f;
    // The environment it was built for (the open air's is larger: RoomInputs::env), and the
    // lounge's rug on the floor: x in [rugX0, rugX1], z in [rugZ0, rugZ1] (all zero when the
    // floor has no space for one).
    int env = kRoomEnvRoom;
    float rugX0 = 0, rugX1 = 0, rugZ0 = 0, rugZ1 = 0;
    // The theatre's stands (zero rows otherwise): row k's centre line at z = standsZ0 + k
    // standsPitch (the viewer's row through the head), its tread at y = standsT0 + k
    // standsRiser, its cell from half a pitch in front of the line to half a pitch behind (the
    // last row's to the back wall); seats every kRoomSeatPitch from x = seatX0 (the viewer's),
    // their centres within seatBlock of the middle, the viewer's own among them (it fits round
    // a seated head: the cushion 0.65 m below the eyes, the armrests 0.3 m to the sides, the
    // back 0.3 m behind and 0.15 m below).
    int standsRows = 0, viewerRow = 0;
    float standsZ0 = 0, standsPitch = 0, standsT0 = 0, standsRiser = 0;
    float seatX0 = 0, seatBlock = 0;
    int seatJMin = 0, seatCols = 0;     // the seats' columns: j = seatJMin .. seatJMin + seatCols - 1 (x = seatX0 + j kRoomSeatPitch)
};

// Depth of the front wall at x (the room is z >= FrontDepth). tanA is sinA / cosA, the
// same float, so this is what it was when it divided.
inline float FrontDepth(const Room& r, float x)
{
    if (!r.curved) return -r.g;
    const float ax = std::fabs(x);
    if (ax <= r.xa) return r.R - std::sqrt(std::fmax(r.Rg * r.Rg - ax * ax, 0.0f));
    return r.za + (ax - r.xa) * r.tanA;
}

// The front's chart coordinate s at x: the glow's own sideways coordinate (arc metres
// at the screen's radius on the arc; along the wings it carries on at the same rate).
inline float FrontS(const Room& r, float x)
{
    if (!r.curved) return x;
    const float ax = std::fabs(x), sign = x < 0 ? -1.0f : 1.0f;
    if (ax <= r.xa) return sign * r.R * std::asin(std::fmin(ax / r.Rg, 1.0f));
    return sign * (r.R * r.phiA + (ax - r.xa) / r.cosA * (r.R / r.Rg));
}

// The front wall at chart coordinate s: its point (x, z) and normal into the room.
inline void FrontPoint(const Room& r, float s, float* x, float* z, float* nx, float* nz)
{
    if (!x || !z || !nx || !nz) return;
    if (!r.curved) { *x = s; *z = -r.g; *nx = 0; *nz = 1; return; }
    const float as = std::fabs(s), sign = s < 0 ? -1.0f : 1.0f;
    if (as <= r.R * r.phiA)
    {
        const float phi = as / r.R;
        *x = sign * r.Rg * std::sin(phi); *z = r.R - r.Rg * std::cos(phi);
        *nx = -sign * std::sin(phi); *nz = std::cos(phi);
        return;
    }
    const float along = (as - r.R * r.phiA) * (r.Rg / r.R);
    *x = sign * (r.xa + along * r.cosA); *z = r.za + along * r.sinA;
    *nx = -sign * r.sinA; *nz = r.cosA;
}

// The front's arc runs to +-phiA (R and Rg set first); the wings carry on from its ends.
inline void RoomSetArc(Room& r, float phiA)
{
    r.phiA = phiA;
    r.sinA = std::sin(phiA); r.cosA = std::cos(phiA);
    r.xa = r.Rg * r.sinA; r.za = r.R - r.Rg * r.cosA;
    r.tanA = r.sinA / r.cosA;
    r.wingS = r.Rg * r.cosA > 0 ? r.R / (r.Rg * r.cosA) : 0.0f;
}

inline bool RoomInside(const Room& r, const float p[3])
{
    if (!p || !r.valid) return false;
    return std::fabs(p[0]) < r.X && p[1] > r.yF && p[1] < r.yC && p[2] < r.zB && p[2] > FrontDepth(r, p[0]);
}

// The room round a screen of size W x H seen from `eye`. Returns false only on
// unusable inputs; the result always contains the viewer.
inline bool BuildRoom(const RoomInputs& in, Room& out)
{
    if (!(in.W > 0) || !(in.H > 0) || !std::isfinite(in.W) || !std::isfinite(in.H)) return false;
    for (int i = 0; i < 3; i++) if (!std::isfinite(in.eye[i])) return false;
    if (!(in.eye[2] > 0)) return false;                 // the viewer must be in front of the screen

    Room r;
    const float W = in.W, H = in.H;
    const float ex = in.eye[0], ey = in.eye[1], ez = in.eye[2];
    r.margin = kAmbiMargin * W;
    r.X = std::fmax(std::fmax(0.5f * W + r.margin + 0.1f * W, std::fabs(ex) + kRoomSideClearance), kRoomMinHalfWidth);
    float floorRef = ey - 1.2f;                         // seated guess
    if (std::isfinite(in.floorY) && ey - in.floorY >= 0.5f && ey - in.floorY <= 2.3f) { floorRef = in.floorY; r.floorTracked = true; }
    r.floorWanted = floorRef;
    r.yF = std::fmin(std::fmin(floorRef, -0.5f * H - kRoomFloorBelowScreen), ey - 0.8f);
    r.yC = std::fmax(std::fmax(r.yF + kRoomMinHeight, 0.5f * H + r.margin), ey + 0.8f);
    r.zB = ez + std::fmax(1.5f, 0.5f * ez);
    r.env = in.env >= 0 && in.env < kRoomEnvCount ? in.env : kRoomEnvRoom;
    RoomTheatre theatre;
    if (r.env == kRoomEnvTheatre)
    {
        // The stands round the viewer's seat: the hall's rows (RoomTheatreSeat), placed so that the
        // viewer's row runs through the head (the screen's distance comes from the same
        // layout, but the rows follow the head wherever it is); the floor in front of the
        // first row, the ceiling, the back wall and the side walls make room for them.
        if (!RoomTheatreSeat(in.seat, theatre)) return false;
        r.standsRows = theatre.rows;
        r.viewerRow = theatre.viewerRow;
        r.standsPitch = theatre.pitch;
        r.standsZ0 = ez - (float)theatre.viewerRow * theatre.pitch;
        r.standsRiser = kRoomRiser;
        r.standsT0 = ey - kRoomSeatedEye - (float)theatre.viewerRow * kRoomRiser;
        r.X = std::fmax(r.X, kRoomStandsWidth * std::fmax(W, kRoomHallWidth) + kRoomAisle);
        r.yF = std::fmin(r.yF, r.standsT0 - kRoomStageDrop);
        const float rearTread = r.standsT0 + (float)(theatre.rows - 1) * kRoomRiser;
        r.yC = std::fmax(r.yC, rearTread + kRoomHeadroom);
        r.zB = std::fmax(r.zB, r.standsZ0 + ((float)theatre.rows - 0.5f) * theatre.pitch + kRoomRearWalk);
        r.seatX0 = ex;
        r.seatBlock = r.X - kRoomAisle - 0.5f * kRoomSeatPitch;
        r.seatJMin = (int)std::ceil((-r.seatBlock - r.seatX0) / kRoomSeatPitch);
        r.seatCols = std::max((int)std::floor((r.seatBlock - r.seatX0) / kRoomSeatPitch) - r.seatJMin + 1, 0);
        // The GPU marks a tile's seats in kRoomSeatBits bits: a wider block keeps its middle.
        while (r.seatCols * theatre.rows > kRoomSeatBits && r.seatCols > 1) { r.seatCols -= 2; r.seatJMin += 1; }
    }
    if (r.env == kRoomEnvOpenAir)
    {
        // No walls to see: the room's floor is the field the screen lights (RoomNightSky
        // carries its edge on beyond it), made larger so that little light is left there.
        r.X = std::fmax(r.X, kRoomOpenHalfWidth);
        r.zB = std::fmax(r.zB, ez + kRoomOpenBehind);
        r.yC = std::fmax(r.yC, r.yF + kRoomOpenHeight);
    }

    r.curved = in.cyl.curved && in.cyl.radius > 0;
    if (r.curved)
    {
        r.R = in.cyl.radius;
        r.Rg = r.R + r.g;
        // The glow reaches (W/2 + margin) along the arc; never past a quarter turn.
        const float phiGlow = std::fmin((0.5f * W + r.margin) / r.R, 1.45f);
        auto fits = [&](float phi)
        {
            Room t = r;
            RoomSetArc(t, phi);
            const float front = std::fmax(FrontDepth(t, ex - 0.5f), FrontDepth(t, ex + 0.5f));
            return front <= ez - kRoomFrontClearance && FrontDepth(t, r.X) <= r.zB - kRoomFrontClearance && t.xa <= r.X;
        };
        float phi = phiGlow;
        if (!fits(phi))
        {
            float lo = 0, hi = phiGlow;                 // a flat front (0) always fits
            for (int i = 0; i < 40; i++) { const float mid = 0.5f * (lo + hi); if (fits(mid)) lo = mid; else hi = mid; }
            phi = lo;
            r.phiReduced = true;
        }
        RoomSetArc(r, phi);
    }
    r.sMax = FrontS(r, r.X);
    r.zSide = FrontDepth(r, r.X);
    if (r.curved)
    {
        // The strip of floor (and ceiling) between z = -g and the curved front is not in
        // the room: integrate (FrontDepth + g) across the width (midpoint rule).
        const int steps = 512;
        double area = 0;
        for (int i = 0; i < steps; i++)
        {
            const float x = -r.X + ((float)i + 0.5f) * 2.0f * r.X / (float)steps;
            area += (double)(FrontDepth(r, x) + r.g) * (2.0 * r.X / steps);
        }
        r.frontFloorArea = (float)area;
    }
    // The eye pass's reciprocals (the denominators are positive: the room has size).
    r.invH = 1.0f / (r.yC - r.yF);
    r.inv2X = 1.0f / (2.0f * r.X);
    r.invSide = 1.0f / (r.zB - r.zSide);
    r.invFloorZ = 1.0f / (r.zB + r.g);
    r.inv2sMax = 1.0f / (2.0f * r.sMax);
    // The room light's panel: kRoomLightW x kRoomLightD, over the viewer's x and
    // kRoomLightBehind behind the head, but kRoomLightInset clear of the side walls, the
    // back wall and the front (zSide is the front's deepest point, so a curved front's
    // too). A room with no space for kRoomLightMinSide either way has none.
    {
        const float w = std::fmin(kRoomLightW, 2.0f * (r.X - kRoomLightInset));
        const float cx = std::fmin(std::fmax(ex, -r.X + kRoomLightInset + 0.5f * w), r.X - kRoomLightInset - 0.5f * w);
        const float d = std::fmin(kRoomLightD, (r.zB - kRoomLightInset) - (r.zSide + kRoomLightInset));
        const float cz = std::fmin(std::fmax(ez + kRoomLightBehind, r.zSide + kRoomLightInset + 0.5f * d), r.zB - kRoomLightInset - 0.5f * d);
        r.lightValid = w >= kRoomLightMinSide && d >= kRoomLightMinSide;
        if (r.lightValid)
        {
            r.lightX0 = cx - 0.5f * w; r.lightX1 = cx + 0.5f * w;
            r.lightZ0 = cz - 0.5f * d; r.lightZ1 = cz + 0.5f * d;
        }
    }
    // The finish's frames: whole bays of about kRoomFramePitch along each wall, and the
    // crossbar at door height when the room is tall enough for it.
    {
        const float side = r.zB - r.zSide, across = 2.0f * r.X;
        r.pitchSide = side / std::fmax(1.0f, std::round(side / kRoomFramePitch));
        r.pitchBack = across / std::fmax(1.0f, std::round(across / kRoomFramePitch));
        r.transomY = r.yF + kRoomTransom <= r.yC - kRoomTransomClear ? r.yF + kRoomTransom : -1e9f;
    }
    // The lounge's rug: kRoomRugW wide under the viewer's x, from kRoomRugFront in front of
    // the head to kRoomRugBack behind it, kept kRoomRugClear off the walls and off the front
    // behind it (a curved front is deepest at its ends, the rug's corners).
    {
        const float w = std::fmin(kRoomRugW, 2.0f * (r.X - kRoomRugClear));
        const float cx = std::fmin(std::fmax(ex, -r.X + kRoomRugClear + 0.5f * w), r.X - kRoomRugClear - 0.5f * w);
        const float front = std::fmax(FrontDepth(r, cx - 0.5f * w), FrontDepth(r, cx + 0.5f * w));
        const float z0 = std::fmax(ez - kRoomRugFront, front + kRoomRugClear), z1 = std::fmin(ez + kRoomRugBack, r.zB - kRoomRugClear);
        if (w > 2.0f * kRoomRugBorder && z1 - z0 > 2.0f * kRoomRugBorder)
        {
            r.rugX0 = cx - 0.5f * w; r.rugX1 = cx + 0.5f * w;
            r.rugZ0 = z0; r.rugZ1 = z1;
        }
    }
    r.valid = true;
    if (!RoomInside(r, in.eye)) return false;
    out = r;
    return true;
}

// A point on a face at chart coordinates (u, v) in [0, 1], and its normal into the room.
inline bool RoomFacePoint(const Room& r, int face, float u, float v, float p[3], float n[3])
{
    if (!p || !n || !r.valid) return false;
    const float h = r.yC - r.yF;
    n[0] = n[1] = n[2] = 0;
    switch (face)
    {
    case kFaceFront:
    {
        float x, z, nx, nz;
        FrontPoint(r, -r.sMax + u * 2.0f * r.sMax, &x, &z, &nx, &nz);
        p[0] = x; p[1] = r.yC - v * h; p[2] = z; n[0] = nx; n[2] = nz;
        return true;
    }
    case kFaceLeft:  p[0] = -r.X; p[1] = r.yC - v * h; p[2] = r.zSide + u * (r.zB - r.zSide); n[0] = 1; return true;
    case kFaceRight: p[0] = r.X;  p[1] = r.yC - v * h; p[2] = r.zSide + u * (r.zB - r.zSide); n[0] = -1; return true;
    case kFaceFloor:   p[0] = -r.X + u * 2.0f * r.X; p[1] = r.yF; p[2] = -r.g + v * (r.zB + r.g); n[1] = 1; return true;
    case kFaceCeiling: p[0] = -r.X + u * 2.0f * r.X; p[1] = r.yC; p[2] = -r.g + v * (r.zB + r.g); n[1] = -1; return true;
    case kFaceBack:  p[0] = -r.X + u * 2.0f * r.X; p[1] = r.yC - v * h; p[2] = r.zB; n[2] = -1; return true;
    default: return false;
    }
}

// Where a ray from inside the room leaves it: the face, its chart coordinates and,
// for the front wall, the glow's coordinates (s, y). `part` is the surface it leaves
// through: the face, except that a curved front's wings are kRoomPartWingL and
// kRoomPartWingR (its arc is kFaceFront), so that a pixel's other rays can try that
// one surface alone (RoomExitOnFace).
struct RoomHit { int face = -1; int part = -1; float t = 0, u = 0, v = 0, s = 0, y = 0; };
static const int kRoomPartWingL = 6, kRoomPartWingR = 7;

// 1/d per component, once per ray: the plane tests multiply by it. It is +-inf where d
// is 0; every use of such a component is guarded by the sign of d, or is a slab bound.
inline bool RoomRayRcp(const float d[3], float rcp[3])
{
    if (!d || !rcp) return false;
    for (int k = 0; k < 3; k++) rcp[k] = 1.0f / d[k];
    return true;
}

// A point along the ray, in one expression, so that every path reaching the same t
// reaches the same point.
inline void RoomAt(const float o[3], const float d[3], float t, float p[3])
{
    if (!o || !d || !p) return;
    p[0] = o[0] + t * d[0]; p[1] = o[1] + t * d[1]; p[2] = o[2] + t * d[2];
}

// ArcHit's quadratic (screen_curve.h), returning the root itself: the first t > 0 on the
// far side of the vertical cylinder of radius rho round the axis at z = axisZ, with
// |hy| <= maxY and |hx| <= tanMax * toward. That is the angle limit as a tangent: no
// atan2, and exact for limits under 90 degrees (the screen's halfWrap is at most 35,
// the front's phiA at most 1.45 rad).
inline bool RoomArcRoot(float axisZ, float rho, float tanMax, float maxY, const float o[3], const float d[3],
                        float* t, float* hx, float* hy, float* hz)
{
    if (!o || !d || !t || !hx || !hy || !hz) return false;
    if (!(rho > 0) || !(axisZ > 0)) return false;
    const float a = d[0] * d[0] + d[2] * d[2];
    if (!(a > 1e-12f)) return false;
    const float b = 2.0f * (o[0] * d[0] + (o[2] - axisZ) * d[2]);
    const float c = o[0] * o[0] + o[2] * o[2] - 2.0f * o[2] * axisZ + (axisZ - rho) * (axisZ + rho);
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0) return false;
    const float sq = std::sqrt(disc);
    const float q = -0.5f * (b + (b < 0 ? -sq : sq));
    float t0 = q / a, t1 = (q != 0) ? c / q : t0;
    if (t0 > t1) { const float swap = t0; t0 = t1; t1 = swap; }
    for (int i = 0; i < 2; i++)
    {
        const float tr = i == 0 ? t0 : t1;
        if (!(tr > 0)) continue;
        const float x = o[0] + tr * d[0], y = o[1] + tr * d[1], z = o[2] + tr * d[2];
        const float toward = axisZ - z;          // > 0 on the screen's side of the axis
        if (!(toward > 0) || std::fabs(y) > maxY || std::fabs(x) > tanMax * toward) continue;
        *t = tr; *hx = x; *hy = y; *hz = z;
        return true;
    }
    return false;
}

// A curved front's arc: where the ray leaves through it, t straight from the root.
inline bool RoomArcT(const Room& r, const float o[3], const float d[3], float* t)
{
    if (!o || !d || !t || !r.curved) return false;
    float hx, hy, hz;
    return RoomArcRoot(r.R, r.Rg, r.tanA, 3.0e38f, o, d, t, &hx, &hy, &hz);
}

// A curved front's wing on `side` (-1 left, +1 right): the plane tangent to the arc at
// its end, beyond that end. The ray's t there, or 0 if it does not leave through it.
inline float RoomWingT(const Room& r, float side, const float o[3], const float d[3])
{
    if (!o || !d || !r.curved) return 0;
    const float nx = -side * r.sinA, nz = r.cosA;           // the wing's normal, into the room
    const float nd = nx * d[0] + nz * d[2];
    if (!(nd < 0)) return 0;
    const float t = (nx * (side * r.xa - o[0]) + nz * (r.za - o[2])) / nd;
    if (side * (o[0] + t * d[0]) < r.xa) return 0;          // on the plane's extension, not the wing
    return t > 0 ? t : 0.0f;
}

// A hit's face and chart coordinates from its point p on surface `part`. Every path goes
// through here, so a ray gets the same (u, v, s, y) whichever path found its exit; the
// uniform denominators are the room's reciprocals, and atan2 runs only for the arc.
inline bool RoomFillHit(const Room& r, int part, float t, const float p[3], RoomHit& hit)
{
    if (!p || part < 0 || part > kRoomPartWingR) return false;
    const int face = part >= kRoomPartWingL ? (int)kFaceFront : part;
    hit.face = face; hit.part = part; hit.t = t; hit.y = p[1]; hit.s = 0;
    switch (face)
    {
    case kFaceFront:
        if (!r.curved) hit.s = p[0];
        else if (part == kFaceFront) hit.s = r.R * std::atan2(p[0], r.R - p[2]);
        else hit.s = (part == kRoomPartWingL ? -1.0f : 1.0f) * (r.R * r.phiA + (std::fabs(p[0]) - r.xa) * r.wingS);
        hit.u = (hit.s + r.sMax) * r.inv2sMax; hit.v = (r.yC - p[1]) * r.invH; break;
    case kFaceLeft: case kFaceRight:
        hit.u = (p[2] - r.zSide) * r.invSide; hit.v = (r.yC - p[1]) * r.invH; break;
    case kFaceFloor: case kFaceCeiling:
        hit.u = (p[0] + r.X) * r.inv2X; hit.v = (p[2] + r.g) * r.invFloorZ; break;
    default:
        hit.u = (p[0] + r.X) * r.inv2X; hit.v = (r.yC - p[1]) * r.invH; break;
    }
    return true;
}

// Where a ray leaves the room, planes first. There is no inside check: the caller knows
// the origin is in the room. The room is the box |x| <= X, yF <= y <= yC, z <= zB (and
// z >= -g for a flat front) cut by z >= FrontDepth(x), which is convex (a circle's arc
// continued by its tangents). So the box's exit is the room's unless it lies in front of
// the front wall, and only then (or with no box exit) is a curved front tested.
inline bool RoomExitFast(const Room& r, const float o[3], const float d[3], const float rcp[3], RoomHit& hit)
{
    if (!o || !d || !rcp) return false;
    float best = 3.0e38f;
    int part = -1;
    auto consider = [&](float t, int p) { if (t > 0 && t < best) { best = t; part = p; } };
    if (d[0] > 0) consider((r.X - o[0]) * rcp[0], kFaceRight);
    if (d[0] < 0) consider((-r.X - o[0]) * rcp[0], kFaceLeft);
    if (d[1] < 0) consider((r.yF - o[1]) * rcp[1], kFaceFloor);
    if (d[1] > 0) consider((r.yC - o[1]) * rcp[1], kFaceCeiling);
    if (d[2] > 0) consider((r.zB - o[2]) * rcp[2], kFaceBack);
    if (!r.curved)
    {
        if (d[2] < 0) consider((-r.g - o[2]) * rcp[2], kFaceFront);
    }
    else
    {
        bool front = part < 0;
        if (!front)
        {
            float p[3];
            RoomAt(o, d, best, p);
            front = p[2] < FrontDepth(r, p[0]);
        }
        if (front)
        {
            float t = 0;
            if (RoomArcT(r, o, d, &t)) consider(t, kFaceFront);
            consider(RoomWingT(r, -1.0f, o, d), kRoomPartWingL);
            consider(RoomWingT(r, 1.0f, o, d), kRoomPartWingR);
        }
    }
    if (part < 0) return false;
    float p[3];
    RoomAt(o, d, best, p);
    return RoomFillHit(r, part, best, p, hit);
}

// The exit if it is on surface `part` (a RoomHit::part): that surface's t alone (one
// multiply for a plane) and a check that the point lies in the room's closure there. The
// room is convex, so a point on one of its bounding surfaces inside its closure is where
// the ray leaves. False when it is not there; the caller then takes RoomExitFast.
inline bool RoomExitOnFace(const Room& r, int part, const float o[3], const float d[3], const float rcp[3], RoomHit& hit)
{
    if (!o || !d || !rcp) return false;
    float t = -1;
    switch (part)
    {
    case kFaceRight:     if (d[0] > 0) t = (r.X - o[0]) * rcp[0]; break;
    case kFaceLeft:      if (d[0] < 0) t = (-r.X - o[0]) * rcp[0]; break;
    case kFaceFloor:     if (d[1] < 0) t = (r.yF - o[1]) * rcp[1]; break;
    case kFaceCeiling:   if (d[1] > 0) t = (r.yC - o[1]) * rcp[1]; break;
    case kFaceBack:      if (d[2] > 0) t = (r.zB - o[2]) * rcp[2]; break;
    case kFaceFront:
        if (!r.curved) { if (d[2] < 0) t = (-r.g - o[2]) * rcp[2]; }
        else if (!RoomArcT(r, o, d, &t)) t = -1;
        break;
    case kRoomPartWingL: t = RoomWingT(r, -1.0f, o, d); break;
    case kRoomPartWingR: t = RoomWingT(r, 1.0f, o, d); break;
    default: return false;
    }
    if (!(t > 0 && t < 3.0e38f)) return false;
    float p[3];
    RoomAt(o, d, t, p);
    bool closure = false;
    switch (part)
    {
    case kFaceLeft: case kFaceRight:
        closure = p[1] >= r.yF && p[1] <= r.yC && p[2] >= r.zSide && p[2] <= r.zB; break;
    case kFaceFloor: case kFaceCeiling:
        closure = std::fabs(p[0]) <= r.X && p[2] >= FrontDepth(r, p[0]) && p[2] <= r.zB; break;
    default:                                                // the back, and the front's plane, arc or wings
        closure = std::fabs(p[0]) <= r.X && p[1] >= r.yF && p[1] <= r.yC; break;
    }
    if (!closure) return false;
    return RoomFillHit(r, part, t, p, hit);
}

// Where a ray from inside the room leaves it (RoomExitFast). Returns false if the ray
// starts outside the room (it then sees the world colour).
inline bool RoomExit(const Room& r, const float o[3], const float d[3], RoomHit& hit)
{
    if (!o || !d) return false;
    if (!RoomInside(r, o)) return false;
    float rcp[3];
    RoomRayRcp(d, rcp);
    return RoomExitFast(r, o, d, rcp, hit);
}

// The curved screen's bounds, for the room's eye pass: the tangent of its half wrap, and
// the box round it padded by kRoomScreenPad (|x| <= boxX, |y| <= boxY, -pad <= z <= boxZ).
// MakeRoomConstants carries them to the GPU (rows 19-20) and the CPU reference computes
// them the same way. All zero for a flat screen: the room's flat layer never tests it.
static const float kRoomScreenPad = 1e-3f;
struct RoomScreenBox { float tanWrap = 0, boxX = 0, boxY = 0, boxZ = 0; };

inline RoomScreenBox RoomScreenBounds(const Cylinder& cyl)
{
    RoomScreenBox b;
    if (!cyl.curved || !(cyl.radius > 0) || !(cyl.halfWrap > 0) || !(cyl.halfHeight > 0)) return b;
    const double R = cyl.radius, w = cyl.halfWrap, half = std::sin(0.5 * w);
    b.tanWrap = (float)std::tan(w);
    b.boxX = (float)(R * std::sin(w) + kRoomScreenPad);
    b.boxY = cyl.halfHeight + kRoomScreenPad;
    b.boxZ = (float)(2.0 * R * half * half + kRoomScreenPad);    // R (1 - cos w), without cancelling at a gentle curve's huge R
    return b;
}

// Where the ray meets the screen, for the room's eye pass: CylinderHit's answer, found
// cheaper. A slab test against the screen's box first (most room rays miss it), then
// ArcHit's quadratic with the tan test for the angle, and atan2 only on a hit, for u.
// CylinderHit itself stays as it is for the plain curve pass.
inline bool RoomScreenHit(const Cylinder& cyl, const RoomScreenBox& box, const float o[3], const float d[3], const float rcp[3],
                          float* tu, float* tv)
{
    if (!o || !d || !rcp || !tu || !tv) return false;
    if (!cyl.curved || !(cyl.radius > 0)) return false;
    const float lo[3] = { -box.boxX, -box.boxY, -kRoomScreenPad }, hi[3] = { box.boxX, box.boxY, box.boxZ };
    float tn[3], tf[3];
    for (int k = 0; k < 3; k++)
    {
        const float t1 = (lo[k] - o[k]) * rcp[k], t2 = (hi[k] - o[k]) * rcp[k];
        tn[k] = std::fmin(t1, t2); tf[k] = std::fmax(t1, t2);
    }
    const float enter = std::fmax(std::fmax(tn[0], tn[1]), tn[2]), leave = std::fmin(std::fmin(tf[0], tf[1]), tf[2]);
    if (!(leave >= std::fmax(enter, 0.0f))) return false;
    float t, hx, hy, hz;
    if (!RoomArcRoot(cyl.radius, cyl.radius, box.tanWrap, cyl.halfHeight, o, d, &t, &hx, &hy, &hz)) return false;
    const float phi = std::atan2(hx, cyl.radius - hz);
    *tu = (cyl.radius * phi) / (2.0f * cyl.halfWidth) + 0.5f;
    *tv = 0.5f - hy / (2.0f * cyl.halfHeight);
    return true;
}

// Surface areas, for the uniform bounce: the true front length, sides, floor, ceiling, back.
inline float RoomArea(const Room& r, int face)
{
    const float h = r.yC - r.yF;
    switch (face)
    {
    case kFaceFront: return 2.0f * (r.curved ? (r.Rg * r.phiA + (r.X - r.xa) / r.cosA) : r.X) * h;
    case kFaceLeft: case kFaceRight: return (r.zB - r.zSide) * h;
    case kFaceFloor: case kFaceCeiling: return 2.0f * r.X * (r.zB + r.g) - r.frontFloorArea;
    case kFaceBack: return 2.0f * r.X * h;
    default: return 0;
    }
}

// ------------------------------------------------------------------- emitters
// One emitter = 6 float4 in the GPU buffer: four corners (c0 carries the area, c1
// the squared diagonal), the normal into the room (w: 1 active, 0 skipped), and the
// radiance (rgb, linear). Corners go round the quad.
struct RoomEmitter
{
    float c[4][4] = {};
    float n[4] = {};
    float L[4] = {};
};
static_assert(sizeof(RoomEmitter) == kRoomEmitterFloat4s * 16, "RoomEmitter is 6 float4");

struct RoomEmitterLayout
{
    int gridX = 0, gridY = 0;           // screen patches
    int glowW = 0, glowH = 0;           // glow texture size
    int block = kRoomGlowBlock;         // glow texels per side of one glow emitter
    int blocksX = 0, blocksY = 0;       // glow blocks
    int lights = 0;                     // the room light (kRoomLights from RoomLayout), after the glow's blocks
    // The theatre's coarse patches (kRoomCoarse, or 0), after the room light. They are not
    // part of count(): the LIGHT pass gathers count() emitters, the coarse ones light only
    // the stands (RoomStandsColour); EMIT works out total() of them.
    int coarse = 0;
    int count() const { return gridX * gridY + blocksX * blocksY + lights; }
    int total() const { return count() + coarse; }
    // The room light's index: every loop over the glow's blocks stops here.
    int lampIndex() const { return gridX * gridY + blocksX * blocksY; }
};

// The emitters for a screen of this shape. The glow's blocks start at kRoomGlowBlock
// texels and grow until everything fits in kRoomMaxEmitters: a 4:3, square or portrait
// picture has a taller glow texture, and at 8 texels its blocks alone passed the
// budget. `minBlock` lets the self-test exercise the larger blocks. The budget counts
// the room light too.
inline RoomEmitterLayout RoomLayout(float W, float H, int glowW, int glowH, int minBlock = kRoomGlowBlock)
{
    RoomEmitterLayout l;
    if (!(W > 0) || !(H > 0) || glowW <= 0 || glowH <= 0) return l;
    l.gridX = 16;
    l.gridY = (int)std::lround(16.0f * H / W);
    l.gridY = l.gridY < 4 ? 4 : (l.gridY > 16 ? 16 : l.gridY);
    l.glowW = glowW; l.glowH = glowH;
    l.lights = kRoomLights;
    for (l.block = std::max(minBlock, kRoomGlowBlock); ; l.block *= 2)
    {
        l.blocksX = (glowW + l.block - 1) / l.block;
        l.blocksY = (glowH + l.block - 1) / l.block;
        if (l.count() <= kRoomMaxEmitters || l.block >= kRoomMaxGlowBlock) break;
    }
    if (l.count() > kRoomMaxEmitters || l.block > kRoomMaxGlowBlock) return RoomEmitterLayout();
    return l;
}

inline void RoomSetQuad(RoomEmitter& e, const float c[4][3], const float n[3])
{
    for (int k = 0; k < 4; k++) { e.c[k][0] = c[k][0]; e.c[k][1] = c[k][1]; e.c[k][2] = c[k][2]; e.c[k][3] = 0; }
    const float a[3] = { c[1][0] - c[0][0], c[1][1] - c[0][1], c[1][2] - c[0][2] };
    const float b[3] = { c[3][0] - c[0][0], c[3][1] - c[0][1], c[3][2] - c[0][2] };
    const float cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
    const float dg[3] = { c[2][0] - c[0][0], c[2][1] - c[0][1], c[2][2] - c[0][2] };
    e.c[0][3] = std::sqrt(cx * cx + cy * cy + cz * cz);
    e.c[1][3] = dg[0] * dg[0] + dg[1] * dg[1] + dg[2] * dg[2];
    e.n[0] = n[0]; e.n[1] = n[1]; e.n[2] = n[2]; e.n[3] = e.c[0][3] > 0 ? 1.0f : 0.0f;
}

// Emitter geometry (radiance left at zero). Screen patches first, row by row from the
// top-left; then the glow's blocks, row by row; then the room light; then, for the theatre,
// the coarse patches (the screen cut kRoomCoarseX x kRoomCoarseY, as its patches are).
// Glow blocks wholly inside the screen (the glow is zero there) or outside the room's height
// are marked inactive, as is the room light when the room has no space for its panel.
inline bool BuildRoomEmitters(const Room& r, const Cylinder& cyl, float W, float H, float glowHalfW, float glowHalfH,
                              const RoomEmitterLayout& l, std::vector<RoomEmitter>& out)
{
    if (!r.valid || l.gridX <= 0 || l.count() > kRoomMaxEmitters) return false;
    if (!(W > 0) || !(H > 0) || !(glowHalfW > 0) || !(glowHalfH > 0)) return false;
    if (l.coarse != (r.env == kRoomEnvTheatre ? kRoomCoarse : 0)) return false;

    out.assign((size_t)l.total(), RoomEmitter());
    // Patch (i, j) of an nx x ny grid over the screen, flat or on the curve.
    auto patch = [&](int i, int j, int nx, int ny, RoomEmitter& e)
    {
        const float y0 = 0.5f * H - (float)j * H / (float)ny, y1 = 0.5f * H - (float)(j + 1) * H / (float)ny;
        float c[4][3], n[3];
        if (!r.curved || !cyl.curved)
        {
            const float x0 = -0.5f * W + (float)i * W / (float)nx, x1 = -0.5f * W + (float)(i + 1) * W / (float)nx;
            const float q[4][3] = { { x0, y0, 0 }, { x1, y0, 0 }, { x1, y1, 0 }, { x0, y1, 0 } };
            memcpy(c, q, sizeof(c));
            n[0] = 0; n[1] = 0; n[2] = 1;
        }
        else
        {
            const float R = cyl.radius;
            const float p0 = -cyl.halfWrap + (float)i * 2.0f * cyl.halfWrap / (float)nx;
            const float p1 = -cyl.halfWrap + (float)(i + 1) * 2.0f * cyl.halfWrap / (float)nx;
            const float x0 = R * std::sin(p0), z0 = R - R * std::cos(p0), x1 = R * std::sin(p1), z1 = R - R * std::cos(p1);
            const float q[4][3] = { { x0, y0, z0 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z0 } };
            memcpy(c, q, sizeof(c));
            const float pc = 0.5f * (p0 + p1);
            n[0] = -std::sin(pc); n[1] = 0; n[2] = std::cos(pc);
        }
        RoomSetQuad(e, c, n);
    };
    for (int j = 0; j < l.gridY; j++)
        for (int i = 0; i < l.gridX; i++) patch(i, j, l.gridX, l.gridY, out[(size_t)j * l.gridX + i]);
    for (int k = 0; k < l.coarse; k++) patch(k % kRoomCoarseX, k / kRoomCoarseX, kRoomCoarseX, kRoomCoarseY, out[(size_t)l.count() + k]);
    for (int by = 0; by < l.blocksY; by++)
        for (int bx = 0; bx < l.blocksX; bx++)
        {
            RoomEmitter& e = out[(size_t)l.gridX * l.gridY + (size_t)by * l.blocksX + bx];
            const float u0 = (float)(bx * l.block) / (float)l.glowW;
            const float u1 = (float)std::min((bx + 1) * l.block, l.glowW) / (float)l.glowW;
            const float v0 = (float)(by * l.block) / (float)l.glowH;
            const float v1 = (float)std::min((by + 1) * l.block, l.glowH) / (float)l.glowH;
            const float s0 = (u0 - 0.5f) * 2.0f * glowHalfW, s1 = (u1 - 0.5f) * 2.0f * glowHalfW;
            float y0 = (0.5f - v0) * 2.0f * glowHalfH, y1 = (0.5f - v1) * 2.0f * glowHalfH;
            const bool insideScreen = std::fmax(std::fabs(s0), std::fabs(s1)) <= 0.5f * W && std::fmax(std::fabs(y0), std::fabs(y1)) <= 0.5f * H;
            y0 = std::fmin(y0, r.yC); y1 = std::fmax(y1, r.yF);
            if (insideScreen || !(y0 > y1) || std::fmax(std::fabs(s0), std::fabs(s1)) > r.sMax) continue;   // inactive
            float xa, za, xb, zb, nxa, nza, nxb, nzb;
            FrontPoint(r, s0, &xa, &za, &nxa, &nza);
            FrontPoint(r, s1, &xb, &zb, &nxb, &nzb);
            const float c[4][3] = { { xa, y0, za }, { xb, y0, zb }, { xb, y1, zb }, { xa, y1, za } };
            float nx, nz, px, pz;
            FrontPoint(r, 0.5f * (s0 + s1), &px, &pz, &nx, &nz);
            const float n[3] = { nx, 0, nz };
            RoomSetQuad(e, c, n);
        }
    // The room light: a quad flush in the ceiling, facing down. It is lit whatever the
    // light's level (0 radiance when off), so turning the light up needs no rebuild.
    if (l.lights > 0)
    {
        RoomEmitter& e = out[(size_t)l.lampIndex()];
        const float c[4][3] = { { r.lightX0, r.yC, r.lightZ0 }, { r.lightX1, r.yC, r.lightZ0 }, { r.lightX1, r.yC, r.lightZ1 }, { r.lightX0, r.yC, r.lightZ1 } };
        const float n[3] = { 0, -1, 0 };
        RoomSetQuad(e, c, n);
        if (!r.lightValid) e.n[3] = 0;
    }
    return true;
}

// The sRGB decode as a 256-entry table: the CPU and GPU decode bytes identically.
inline void RoomDecodeTable(float table[256])
{
    if (!table) return;
    for (int i = 0; i < 256; i++) table[i] = SrgbToLinear((float)i / 255.0f);
}

// Pixel boxes: which source pixels a screen patch averages (edges on whole pixels,
// every pixel counted once), sampled every `stride` pixels.
inline int RoomStride(int srcW) { const int s = srcW / 1920; return s < 1 ? 1 : s; }

inline void RoomPatchBox(int i, int j, const RoomEmitterLayout& l, int srcW, int srcH, int* x0, int* x1, int* y0, int* y1)
{
    if (!x0 || !x1 || !y0 || !y1) return;
    *x0 = (int)((long long)i * srcW / l.gridX); *x1 = (int)((long long)(i + 1) * srcW / l.gridX);
    *y0 = (int)((long long)j * srcH / l.gridY); *y1 = (int)((long long)(j + 1) * srcH / l.gridY);
}

// The shader's reduction, exactly: 256 threads, thread t sums items t, t+256, ...;
// then a halving tree. Same order, same float result.
template <typename Item>
inline void RoomGroupSum(int count, Item item, float out[3])
{
    if (!out) return;
    float part[kRoomGroupThreads][3];
    for (int t = 0; t < kRoomGroupThreads; t++)
    {
        part[t][0] = part[t][1] = part[t][2] = 0;
        for (int k = t; k < count; k += kRoomGroupThreads)
        {
            float v[3];
            item(k, v);
            part[t][0] += v[0]; part[t][1] += v[1]; part[t][2] += v[2];
        }
    }
    for (int stride = kRoomGroupThreads / 2; stride > 0; stride >>= 1)
        for (int t = 0; t < stride; t++)
            for (int ch = 0; ch < 3; ch++) part[t][ch] += part[t + stride][ch];
    out[0] = part[0][0]; out[1] = part[0][1]; out[2] = part[0][2];
}

// Each emitter's radiance this frame, as the EMIT pass computes it. `src` is the
// captured picture (RGBA8, pitch in bytes), `glow` the 8-bit glow texture. Screen
// patches blend towards their new mean by `alpha` (1: take it); glow blocks take
// theirs (the glow is already smoothed). The room light takes `lightL` (rgb, the
// constants' row 14), or 0 when it is null - the block loops never reach its index. The
// theatre's coarse patches blend towards their means like the screen's patches.
inline void RoomGridBox(int i, int j, int w, int h, int srcW, int srcH, int* x0, int* x1, int* y0, int* y1);
inline bool RoomEmitRadiance(const RoomEmitterLayout& l, const unsigned char* src, int srcW, int srcH, int srcPitch,
                             const unsigned char* glow, int glowPitch, bool glowOn, float alpha,
                             const float table[256], std::vector<RoomEmitter>& emitters, const float* lightL = nullptr)
{
    if (!src || !table || srcW <= 0 || srcH <= 0 || srcPitch < srcW * 4) return false;
    if ((int)emitters.size() != l.total()) return false;
    if (glowOn && (!glow || glowPitch < l.glowW * 4)) return false;

    const int stride = RoomStride(srcW);
    for (int j = 0; j < l.gridY; j++)
        for (int i = 0; i < l.gridX; i++)
        {
            int x0, x1, y0, y1;
            RoomPatchBox(i, j, l, srcW, srcH, &x0, &x1, &y0, &y1);
            const int nx = (x1 - x0 + stride - 1) / stride, ny = (y1 - y0 + stride - 1) / stride;
            float sum[3];
            RoomGroupSum(nx * ny, [&](int k, float v[3])
            {
                const unsigned char* p = src + (size_t)(y0 + (k / nx) * stride) * srcPitch + (size_t)(x0 + (k % nx) * stride) * 4;
                v[0] = table[p[0]]; v[1] = table[p[1]]; v[2] = table[p[2]];
            }, sum);
            RoomEmitter& e = emitters[(size_t)j * l.gridX + i];
            const float count = (float)(nx * ny);
            for (int ch = 0; ch < 3; ch++)
            {
                const float mean = count > 0 ? sum[ch] / count : 0.0f;
                e.L[ch] = e.L[ch] + (mean - e.L[ch]) * alpha;
            }
        }
    for (int by = 0; by < l.blocksY; by++)
        for (int bx = 0; bx < l.blocksX; bx++)
        {
            RoomEmitter& e = emitters[(size_t)l.gridX * l.gridY + (size_t)by * l.blocksX + bx];
            e.L[0] = e.L[1] = e.L[2] = 0;
            if (!glowOn || e.n[3] == 0) continue;
            const int gx0 = bx * l.block, gy0 = by * l.block;
            const int bw = std::min(l.block, l.glowW - gx0), bh = std::min(l.block, l.glowH - gy0);
            float sum[3];
            RoomGroupSum(bw * bh, [&](int k, float v[3])
            {
                const unsigned char* p = glow + (size_t)(gy0 + k / bw) * glowPitch + (size_t)(gx0 + k % bw) * 4;
                v[0] = table[p[0]]; v[1] = table[p[1]]; v[2] = table[p[2]];
            }, sum);
            const float count = (float)(bw * bh);
            for (int ch = 0; ch < 3; ch++) e.L[ch] = sum[ch] / count;
        }
    if (l.lights > 0)
    {
        RoomEmitter& e = emitters[(size_t)l.lampIndex()];
        for (int ch = 0; ch < 3; ch++) e.L[ch] = lightL ? lightL[ch] : 0.0f;
    }
    for (int k = 0; k < l.coarse; k++)
    {
        int x0, x1, y0, y1;
        RoomGridBox(k % kRoomCoarseX, k / kRoomCoarseX, kRoomCoarseX, kRoomCoarseY, srcW, srcH, &x0, &x1, &y0, &y1);
        const int nx = (x1 - x0 + stride - 1) / stride, ny = (y1 - y0 + stride - 1) / stride;
        float sum[3];
        RoomGroupSum(nx * ny, [&](int item, float v[3])
        {
            const unsigned char* p = src + (size_t)(y0 + (item / nx) * stride) * srcPitch + (size_t)(x0 + (item % nx) * stride) * 4;
            v[0] = table[p[0]]; v[1] = table[p[1]]; v[2] = table[p[2]];
        }, sum);
        RoomEmitter& e = emitters[(size_t)l.count() + k];
        const float count = (float)(nx * ny);
        for (int ch = 0; ch < 3; ch++)
        {
            const float mean = count > 0 ? sum[ch] / count : 0.0f;
            e.L[ch] = e.L[ch] + (mean - e.L[ch]) * alpha;
        }
    }
    return true;
}

// ------------------------------------------------------------ the mirror picture
// Reflections (v11) see the screen as a small picture of the source: kRoomMirrorW texels
// wide and as tall as the source's aspect makes it (72 for 16:9, 228 for 9:16), each the
// exact mean of its box of source pixels. The GPU's MIRROR pass (kRoomHlsl, ROOM_MIRROR)
// computes it each frame into an RGBA16F texture of kRoomMirrorW x kRoomMirrorMaxH, of
// which only the first h rows are used.

// Pixel box (i, j) of a w x h grid over a srcW x srcH picture: edges on whole pixels,
// every pixel in exactly one box (RoomPatchBox's rule for any grid).
inline void RoomGridBox(int i, int j, int w, int h, int srcW, int srcH, int* x0, int* x1, int* y0, int* y1)
{
    if (!x0 || !x1 || !y0 || !y1) return;
    if (w <= 0 || h <= 0) { *x0 = *x1 = *y0 = *y1 = 0; return; }
    *x0 = (int)((long long)i * srcW / w); *x1 = (int)((long long)(i + 1) * srcW / w);
    *y0 = (int)((long long)j * srcH / h); *y1 = (int)((long long)(j + 1) * srcH / h);
}

// The mirror picture's size for a srcW x srcH source: kRoomMirrorW wide, and
// round(kRoomMirrorW srcH / srcW) tall within [kRoomMirrorMinH, kRoomMirrorMaxH].
inline bool RoomMirrorSize(int srcW, int srcH, int* w, int* h)
{
    if (!w || !h) return false;
    *w = *h = 0;
    if (srcW <= 0 || srcH <= 0) return false;
    const long long rows = (2LL * kRoomMirrorW * srcH + srcW) / (2LL * srcW);
    *w = kRoomMirrorW;
    *h = (int)(rows < kRoomMirrorMinH ? kRoomMirrorMinH : (rows > kRoomMirrorMaxH ? kRoomMirrorMaxH : rows));
    return true;
}

// A mirror picture as floats, [row][column][rgba], linear, for the CPU reference; Sample
// is the eye pass's MirrorH: bilinear by hand, edges clamped to the w x h in use.
struct RoomMirror
{
    int w = 0, h = 0;
    std::vector<float> texels;          // w * h * 4
    bool Sample(float u, float v, float out[3]) const
    {
        if (!out || w <= 0 || h <= 0 || texels.size() != (size_t)w * h * 4) return false;
        const float x = u * (float)w - 0.5f, y = v * (float)h - 0.5f;
        const float fx0 = std::floor(x), fy0 = std::floor(y), fx = x - fx0, fy = y - fy0;
        auto clampi = [](int i, int n) { return i < 0 ? 0 : (i > n - 1 ? n - 1 : i); };
        const int x0 = clampi((int)fx0, w), x1 = clampi((int)fx0 + 1, w), y0 = clampi((int)fy0, h), y1 = clampi((int)fy0 + 1, h);
        auto at = [&](int xi, int yi, int ch) { return texels[((size_t)yi * w + xi) * 4 + ch]; };
        for (int ch = 0; ch < 3; ch++)
        {
            const float top = at(x0, y0, ch) + fx * (at(x1, y0, ch) - at(x0, y0, ch));
            const float bot = at(x0, y1, ch) + fx * (at(x1, y1, ch) - at(x0, y1, ch));
            out[ch] = top + fy * (bot - top);
        }
        return true;
    }
};

// The mirror picture of `src` (RGBA8, pitch in bytes), as the MIRROR pass computes it:
// each texel the mean of its RoomGridBox, every RoomStride-th pixel through the decode
// table, summed row by row in the shader's order.
inline bool RoomMirrorPicture(const unsigned char* src, int srcW, int srcH, int srcPitch, const float table[256], RoomMirror& out)
{
    if (!src || !table || srcW <= 0 || srcH <= 0 || srcPitch < srcW * 4) return false;
    int w = 0, h = 0;
    if (!RoomMirrorSize(srcW, srcH, &w, &h)) return false;
    out.w = w; out.h = h;
    out.texels.assign((size_t)w * h * 4, 0.0f);
    const int stride = RoomStride(srcW);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
        {
            int x0, x1, y0, y1;
            RoomGridBox(i, j, w, h, srcW, srcH, &x0, &x1, &y0, &y1);
            float sum[3] = { 0, 0, 0 };
            int count = 0;
            for (int y = y0; y < y1; y += stride)
                for (int x = x0; x < x1; x += stride)
                {
                    const unsigned char* px = src + (size_t)y * srcPitch + (size_t)x * 4;
                    sum[0] += table[px[0]]; sum[1] += table[px[1]]; sum[2] += table[px[2]];
                    count++;
                }
            float* t = &out.texels[((size_t)j * w + i) * 4];
            for (int ch = 0; ch < 3; ch++) t[ch] = count > 0 ? sum[ch] / (float)count : 0.0f;
            t[3] = 1.0f;
        }
    return true;
}

// -------------------------------------------------------------------- lighting
// G = pi x the form factor from a point (p, n) to a Lambertian quad: exact (Lambert's
// polygon formula) close up, the disk formula further than twice the quad's diagonal.
// Zero if the quad is behind the point's horizon or the point behind the quad.
inline float RoomLambertQuad(const float p[3], const float n[3], const float c[4][4])
{
    float u[4][3];
    for (int k = 0; k < 4; k++)
    {
        const float v[3] = { c[k][0] - p[0], c[k][1] - p[1], c[k][2] - p[2] };
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(len > 1e-9f)) return 0;
        u[k][0] = v[0] / len; u[k][1] = v[1] / len; u[k][2] = v[2] / len;
    }
    float sum = 0;
    for (int k = 0; k < 4; k++)
    {
        const float* a = u[k];
        const float* b = u[(k + 1) & 3];
        const float cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
        const float cl = std::sqrt(cx * cx + cy * cy + cz * cz);
        if (!(cl > 1e-12f)) continue;
        const float theta = std::atan2(cl, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
        sum += theta * (n[0] * cx + n[1] * cy + n[2] * cz) / cl;
    }
    return 0.5f * std::fabs(sum);
}

inline float RoomFormFactor(const float p[3], const float n[3], const RoomEmitter& e)
{
    if (e.n[3] == 0) return 0;
    const float cx = 0.5f * (e.c[0][0] + e.c[2][0]), cy = 0.5f * (e.c[0][1] + e.c[2][1]), cz = 0.5f * (e.c[0][2] + e.c[2][2]);
    const float v[3] = { cx - p[0], cy - p[1], cz - p[2] };
    const float np = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
    const float nq = -(e.n[0] * v[0] + e.n[1] * v[1] + e.n[2] * v[2]);
    if (!(np > 0) || !(nq > 0)) return 0;
    const float r2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    const float area = e.c[0][3], diag2 = e.c[1][3];
    if (r2 >= 4.0f * diag2) return area * (np * nq / r2) / (r2 + area / kRoomPi);
    return RoomLambertQuad(p, n, e.c);
}

// The room's controls beyond the Room slider (v11): Glass walls, 0 (solid) .. 100 (clear),
// Reflections, 0 (none) .. 100 (the coat at its strongest), and the room light, its level
// 0 (off) .. 100 and its colour.
struct RoomLook
{
    int glass = 0;
    int reflect = 0;
    int light = 0;
    uint32_t lightRgb = kRoomLightDefault;      // 0xRRGGBB
    int env = kRoomEnvRoom;                     // the environment (kRoomEnv*): Glass and Reflections are the room's alone
};

// The coat's Fresnel term (Schlick, F0 = kRoomGlassF0 at Reflections r = 1) at c = |cos|
// of the angle to the surface's normal, and its cosine-weighted mean over the hemisphere:
// 2 x the integral of (1 - mu)^5 mu over [0, 1] is 2 B(2, 6) = 1/21.
inline float RoomFresnel(float r, float c)
{
    const float q = 1.0f - c, q2 = q * q, q5 = q2 * q2 * q;
    return r * (kRoomGlassF0 + (1.0f - kRoomGlassF0) * q5);
}

inline float RoomFresnelMean(float r)
{
    return r * (kRoomGlassF0 + (1.0f - kRoomGlassF0) / 21.0f);
}

// The share of a face its frames cover, on average (the finish's bounce): uprights every
// bay and bars along the floor, the crossbar and the ceiling on the walls (half of the
// floor's and the ceiling's bars lie on the wall); beams both ways on the ceiling.
inline float RoomFrameFraction(const Room& r, int face)
{
    if (!r.valid || !(r.pitchSide > 0) || !(r.pitchBack > 0)) return 0;
    const float h = r.yC - r.yF;
    const float bars = kRoomFrameW + (r.transomY >= r.yF ? kRoomFrameW : 0.0f);
    switch (face)
    {
    case kFaceLeft: case kFaceRight: return 1.0f - (1.0f - kRoomFrameW / r.pitchSide) * (1.0f - bars / h);
    case kFaceBack: return 1.0f - (1.0f - kRoomFrameW / r.pitchBack) * (1.0f - bars / h);
    case kFaceCeiling: return 1.0f - (1.0f - kRoomFrameW / r.pitchBack) * (1.0f - kRoomFrameW / r.pitchSide);
    default: return 0;
    }
}

struct RoomShading
{
    float rhoWall = 0, rhoFloor = 0, rhoCeiling = 0;
    float rhoBar = 0, invArea = 0;      // mean albedo and 1 / total area, for the bounce
    float world[3] = { 0, 0, 0 };       // Dec(world colour): the house lights
    // The room light. Level s = (Light/100)^2 and colour c = Dec(rgb) / its largest
    // channel: the panel emits Le = kRoomLightMax s c into the lightmap and is seen as
    // Ls = c min(1, kRoomLightMax s), which keeps its hue and is full from 23% up
    // (kRoomLightMax s reaches 1 at 22.4%).
    bool lightOn = false;               // the panel fits, its level is above 0 and its colour is not black (kRoomFlagLight)
    float lightL[3] = { 0, 0, 0 };      // Le, linear (0 while off)
    float lightSeen[3] = { 0, 0, 0 };   // Ls, linear (0 while off)
    float kappaBar = 0;                 // the panel's share of the ceiling's area (0 with no panel), for the finish's bounce
    // The finish (kRoomFlagFinish): on while Glass or Reflections is above 0.
    bool finish = false;
    float glass = 0;                    // T = Glass / 100: how clear the panes are
    float reflect = 0;                  // r = Reflections / 100: the coat's strength
    float fbar = 0;                     // RoomFresnelMean(r): the coat's mean reflectance
    // The environment (kRoomEnv*): each face's colour (kRoomEnvTint), the bounce per channel
    // (each face's albedo times its colour; none in the open air, whose light escapes to the
    // sky), and a house light added to the world colour's (the open air's night). For the
    // room the colours are 1, the bounce rhoBar's on every channel and the addition 0, so
    // its lightmap is what it was to the bit.
    int env = kRoomEnvRoom;
    float tint[kRoomFaces][3] = { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } };
    float rhoBarRgb[3] = { 0, 0, 0 };
    float bounceRgb[3] = { 0, 0, 0 };
    float ambient[3] = { 0, 0, 0 };
};

// The finish's albedo of face f for the bounce (spec 2.9), given its plain albedo rho: the
// frames are opaque and diffuse; a pane reflects fbar and passes diffuse light through its
// coat twice, (1 - fbar)^2, of which the clear part (T) leaves the room; the ceiling's
// panel is an opaque fitting; the floor's grout is darker and does not shine; the front
// wall is unchanged.
inline float RoomFinishAlbedo(const Room& r, const RoomShading& s, int face, float rho)
{
    const float glassOf = (1.0f - s.fbar) * (1.0f - s.fbar) * (1.0f - s.glass);
    switch (face)
    {
    case kFaceLeft: case kFaceRight: case kFaceBack: case kFaceCeiling:
    {
        const float phi = RoomFrameFraction(r, face);
        const float pane = phi * rho + (1.0f - phi) * (s.fbar + glassOf * rho);
        return face == kFaceCeiling ? s.kappaBar * rho + (1.0f - s.kappaBar) * pane : pane;
    }
    case kFaceFloor:
    {
        const float gBar = 1.0f - (1.0f - kRoomGroutW) * (1.0f - kRoomGroutW);
        const float fl = kRoomFloorGloss * s.fbar * (1.0f - gBar);
        return fl + (1.0f - fl) * (1.0f - fl) * (1.0f - kRoomGrout * gBar) * rho;
    }
    default: return rho;
    }
}

// The lightmap's albedos and bounce, the house lights, the room light and the finish. The
// bounce's mean albedo rhoBar weighs each face's albedo a_f by its area; the screen
// absorbs. With the finish off a_f is each face's plain albedo, so rhoBar is v10's to the
// bit (the room light's panel is part of the ceiling); with it on, RoomFinishAlbedo. The
// room light's own flux joins the bounce as every emitter's does, through RoomTexel's flux sum.
inline RoomShading MakeRoomShading(const Room& r, int roomPercent, uint32_t worldRgb, float screenArea, const RoomLook& look = RoomLook())
{
    RoomShading s;
    const float pct = (float)(roomPercent < 0 ? 0 : (roomPercent > 100 ? 100 : roomPercent)) / 100.0f;
    s.rhoWall = kRoomMaxAlbedo * pct;
    s.rhoCeiling = s.rhoWall;
    s.rhoFloor = kRoomFloorAlbedo * s.rhoWall;
    const float ceiling = RoomArea(r, kFaceCeiling);
    s.kappaBar = r.lightValid && ceiling > 0 ? (r.lightX1 - r.lightX0) * (r.lightZ1 - r.lightZ0) / ceiling : 0.0f;
    s.env = look.env >= 0 && look.env < kRoomEnvCount ? look.env : kRoomEnvRoom;
    const bool roomEnv = s.env == kRoomEnvRoom;
    for (int f = 0; f < kRoomFaces; f++)
        for (int ch = 0; ch < 3; ch++) s.tint[f][ch] = kRoomEnvTint[s.env][f][ch];
    // Glass and Reflections are the room's own: another environment has neither.
    s.glass = roomEnv ? (float)(look.glass < 0 ? 0 : (look.glass > 100 ? 100 : look.glass)) / 100.0f : 0.0f;
    s.reflect = roomEnv ? (float)(look.reflect < 0 ? 0 : (look.reflect > 100 ? 100 : look.reflect)) / 100.0f : 0.0f;
    s.fbar = RoomFresnelMean(s.reflect);
    s.finish = r.valid && (s.glass > 0 || s.reflect > 0);
    float total = 0, weighted = 0, weightedRgb[3] = { 0, 0, 0 };
    for (int f = 0; f < kRoomFaces; f++)
    {
        const float a = RoomArea(r, f);
        const float plain = f == kFaceFloor ? s.rhoFloor : (f == kFaceCeiling ? s.rhoCeiling : s.rhoWall);
        const float rho = s.finish ? RoomFinishAlbedo(r, s, f, plain) : plain;
        const float seen = f == kFaceFront ? std::fmax(a - screenArea, 0.0f) : a;   // the screen absorbs
        total += a;
        weighted += rho * seen;
        for (int ch = 0; ch < 3; ch++) weightedRgb[ch] += rho * s.tint[f][ch] * seen;
    }
    s.rhoBar = total > 0 ? weighted / total : 0;
    s.invArea = total > 0 ? 1.0f / total : 0;
    for (int ch = 0; ch < 3; ch++)
    {
        s.rhoBarRgb[ch] = total > 0 ? weightedRgb[ch] / total : 0;
        s.bounceRgb[ch] = s.env == kRoomEnvOpenAir || !(s.rhoBarRgb[ch] < 0.999f) ? 0.0f : s.rhoBarRgb[ch] * s.invArea / (1.0f - s.rhoBarRgb[ch]);
        s.ambient[ch] = s.env == kRoomEnvOpenAir ? kRoomNight[ch] : 0.0f;
    }
    s.world[0] = SrgbToLinear((float)((worldRgb >> 16) & 255) / 255.0f);
    s.world[1] = SrgbToLinear((float)((worldRgb >> 8) & 255) / 255.0f);
    s.world[2] = SrgbToLinear((float)(worldRgb & 255) / 255.0f);

    const float level = (float)(look.light < 0 ? 0 : (look.light > 100 ? 100 : look.light)) / 100.0f;
    const float strength = level * level;
    const float c[3] = { SrgbToLinear((float)((look.lightRgb >> 16) & 255) / 255.0f), SrgbToLinear((float)((look.lightRgb >> 8) & 255) / 255.0f),
                         SrgbToLinear((float)(look.lightRgb & 255) / 255.0f) };
    const float peak = std::fmax(std::fmax(c[0], c[1]), c[2]);
    s.lightOn = r.valid && r.lightValid && strength > 0 && peak > 0 && s.env != kRoomEnvOpenAir;
    if (s.lightOn)
        for (int ch = 0; ch < 3; ch++)
        {
            const float cn = c[ch] / peak;
            s.lightL[ch] = kRoomLightMax * strength * cn;
            s.lightSeen[ch] = cn * std::fmin(1.0f, kRoomLightMax * strength);
        }
    return s;
}

inline float RoomBounceScale(const RoomShading& sh);

// The bounce on channel ch: the room's from rhoBar (RoomBounceScale, as it always was), an
// environment's from its coloured albedos (bounceRgb).
inline float RoomBounceOf(const RoomShading& sh, int ch)
{
    return sh.env == kRoomEnvRoom ? RoomBounceScale(sh) : sh.bounceRgb[ch];
}

// Where a lightmap texel is lit from. A floor or ceiling texel whose centre lies behind
// a curved front (outside the room) is lit from just inside the wall instead: texels
// out there are never seen, but bilinear sampling at the wall's base blends them in,
// and lit where they are they drew a dark sawtooth seam along it.
static const float kRoomInsideFront = 0.01f;

inline bool RoomLightPoint(const Room& r, int face, int i, int j, float p[3], float n[3])
{
    if (!p || !n) return false;
    if (!RoomFacePoint(r, face, ((float)i + 0.5f) / kRoomLightmap, ((float)j + 0.5f) / kRoomLightmap, p, n)) return false;
    if (r.curved && (face == kFaceFloor || face == kFaceCeiling))
    {
        const float front = FrontDepth(r, p[0]) + kRoomInsideFront;
        if (p[2] < front) p[2] = front;
    }
    return true;
}

// One lightmap texel's radiance (linear rgb), as the LIGHT pass computes it.
inline bool RoomTexel(const Room& r, const RoomShading& sh, const std::vector<RoomEmitter>& emitters, int face, int i, int j, float out[3])
{
    if (!out || !r.valid || face < 0 || face >= kRoomFaces || i < 0 || j < 0 || i >= kRoomLightmap || j >= kRoomLightmap) return false;
    float p[3], n[3];
    RoomLightPoint(r, face, i, j, p, n);
    float E[3] = { 0, 0, 0 }, flux[3] = { 0, 0, 0 };
    // The theatre's coarse patches, after the room's emitters, light only its stands.
    const size_t coarse = r.env == kRoomEnvTheatre ? (size_t)kRoomCoarse : 0;
    if (emitters.size() < coarse) return false;
    for (size_t m = 0; m < emitters.size() - coarse; m++)
    {
        const RoomEmitter& e = emitters[m];
        if (e.n[3] == 0) continue;
        const float G = RoomFormFactor(p, n, e);
        const float a = kRoomPi * e.c[0][3];
        for (int ch = 0; ch < 3; ch++) { E[ch] += e.L[ch] * G; flux[ch] += e.L[ch] * a; }
    }
    const float rho = face == kFaceFloor ? sh.rhoFloor : (face == kFaceCeiling ? sh.rhoCeiling : sh.rhoWall);
    const float shade = face == kFaceFloor ? kRoomShadeFloor : (face == kFaceCeiling ? kRoomShadeCeiling : 1.0f);
    // The face's colour times what it was (the room: 1 x, its bounce from rhoBar, the night 0).
    for (int ch = 0; ch < 3; ch++)
        out[ch] = sh.tint[face][ch] * ((rho / kRoomPi) * (E[ch] + flux[ch] * RoomBounceOf(sh, ch)) + shade * (sh.world[ch] + sh.ambient[ch]));
    return true;
}

// The room pass's view of the screen: flat room layer (the screen itself is drawn by
// the compositor's quads, so this layer shows its footprint in black - if the layers
// slip against each other the gap is dark on dark), or the curved screen.
struct RoomView
{
    bool flatLayer = false;
    float W = 0, H = 0;                 // the screen, for the flat footprint
    bool glowOn = false;
    float glowHalfW = 0, glowHalfH = 0;
    bool dither = true;
    Cylinder cyl;                       // the curved screen, whose bounds the constants carry (not curved for the flat layer)
};

// 1 / x, or 0 where x is not positive (a glow that is off may have no size).
inline float RoomRecip(float x)
{
    return x > 0 ? 1.0f / x : 0.0f;
}

// ------------------------------------------------------------------ constants
// What both room shaders read (cbuffer RoomC: a root CBV, 512 bytes) - the lightmap
// passes (kRoomHlsl) and the eye pass (the curve shader's code, then kCurveRoomHlsl).
static const uint32_t kRoomFlagCurved = 1;      // the front is the glow's cylinder
static const uint32_t kRoomFlagFlatLayer = 2;   // flat screen: the compositor draws it; show its footprint
static const uint32_t kRoomFlagGlow = 4;        // the ambilight is on
static const uint32_t kRoomFlagDither = 8;
static const uint32_t kRoomFlagFinish = 16;     // Glass or Reflections above 0: frames, tiles, the coat, the glass
static const uint32_t kRoomFlagReflect = 32;    // Reflections above 0: the reflected ray and the MIRROR pass
static const uint32_t kRoomFlagLight = 64;      // the room light is on: its panel fits, its level is above 0, its colour is not black

// Rows 0-10 are v10's. Rows 11-20 are v11's: the finish (row 11: glass, reflections and
// the coat's mean reflectance; row 12: the frames), the room light (rows 13-15), the
// mirror picture's size (row 16, zero while Reflections is 0), and the eye pass's
// reciprocals and screen bounds (rows 17-20). The environments (XENV.md) take row 16's third
// value and rows 21-31: the faces' colours, the bounce per channel, the side walls' lights,
// the lounge's rug and the open air's night.
// kRoomCbufferHlsl declares all 32 rows.
struct RoomConstants                            // must match kRoomCbufferHlsl
{
    float X = 0, yF = 0, yC = 0, zB = 0;                            // row 0
    float g = 0, R = 0, Rg = 0, phiA = 0;                           // 1
    float sinA = 0, cosA = 1, xa = 0, za = 0;                       // 2
    float sMax = 0, zSide = 0, glowHalfW = 0, glowHalfH = 0;        // 3
    float rhoWall = 0, rhoFloor = 0, rhoCeiling = 0, bounceScale = 0;   // 4
    float world[4] = { 0, 0, 0, 0 };            // 5, linear: the house lights
    float shadeFloor = kRoomShadeFloor, shadeCeiling = kRoomShadeCeiling, alpha = 1, screenW = 0;   // 6
    float screenH = 0, pad0 = 0, pad1 = 0, pad2 = 0;                // 7
    uint32_t flags = 0, gridX = 0, gridY = 0, emitters = 0;         // 8
    uint32_t srcW = 0, srcH = 0, stride = 1, glowW = 0;             // 9
    uint32_t glowH = 0, blocksX = 0, blocksY = 0, glowBlock = kRoomGlowBlock;   // 10
    float glass = 0, reflect = 0, fbar = 0, rpad3 = 0;              // 11 (v11)
    float pitchSide = 0, pitchBack = 0, transomY = 0, rpad4 = 0;    // 12
    float lightX0 = 0, lightX1 = 0, lightZ0 = 0, lightZ1 = 0;       // 13
    float lightL[4] = { 0, 0, 0, 0 };           // 14: emitted rgb, w = the panel is valid
    float lightSeen[4] = { 0, 0, 0, 0 };        // 15: seen rgb
    uint32_t mirrorW = 0, mirrorH = 0, env = 0, upad1 = 0;          // 16: the mirror picture in use (RoomMirrorSize); the environment
    float invH = 0, inv2X = 0, invSide = 0, invFloorZ = 0;          // 17
    float inv2sMax = 0, invGlowW = 0, invGlowH = 0, tanA = 0;       // 18
    float wingS = 0, scrTanWrap = 0, scrBoxX = 0, scrBoxY = 0;      // 19
    float scrBoxZ = 0, rpad6 = 0, rpad7 = 0, rpad8 = 0;             // 20
    float tint[kRoomFaces][4] = {};             // 21-26: each face's colour (RoomShading::tint), w 0
    float bounceRgb[4] = { 0, 0, 0, 0 };        // 27: the bounce per channel (RoomShading::bounceRgb)
    float sconcePitch = 0, sconceZ0 = 0, sconceY = 0, sconceHalfW = 0;  // 28: the side walls' lights (RoomSconce): every pitch from z0, at y
    float sconceHalfH = 0, haloGain = 0, haloUp = 0, haloDown = 0;      // 29: their size and the fan of light (gain 0: none)
    float rugX0 = 0, rugX1 = 0, rugZ0 = 0, rugZ1 = 0;                   // 30: the lounge's rug (Room::rug*)
    float ambient[4] = { 0, 0, 0, 0 };          // 31: added to the world colour's house light (RoomShading::ambient)
    float standsZ0 = 0, standsPitch = 0, standsT0 = 0, standsRiser = 0; // 32: the theatre's stands (Room::stands*)
    float seatX0 = 0, seatBlock = 0, standsZEnd = 0, rpad9 = 0;         // 33: its seats, and where the last row ends (the back wall)
    uint32_t standsRows = 0, viewerRow = 0, coarseFirst = 0, coarseCount = 0;   // 34: rows (0: no stands), the viewer's, the coarse patches
    int32_t seatJMin = 0; uint32_t seatCols = 0; float rpad10 = 0, rpad11 = 0;  // 35: the seats' columns (Room::seatJMin, seatCols)
    float pad6[48] = {};                                                // 36-47: a 768-byte buffer keeps the ring's CBVs 256-aligned
};
static_assert(sizeof(RoomConstants) == 768, "RoomConstants is one 768-byte constant buffer (three 256-byte steps)");
static_assert(offsetof(RoomConstants, standsZ0) == 512 && offsetof(RoomConstants, seatX0) == 528 && offsetof(RoomConstants, standsRows) == 544 &&
              offsetof(RoomConstants, seatJMin) == 560, "rows 32-35: the theatre (XENV.md)");
static_assert(offsetof(RoomConstants, glowH) == 160 && offsetof(RoomConstants, glass) == 176, "rows 0-10 are v10's; row 11 starts at 176");
static_assert(offsetof(RoomConstants, pitchSide) == 192 && offsetof(RoomConstants, lightX0) == 208 && offsetof(RoomConstants, lightL) == 224 &&
              offsetof(RoomConstants, lightSeen) == 240 && offsetof(RoomConstants, mirrorW) == 256 && offsetof(RoomConstants, invH) == 272 &&
              offsetof(RoomConstants, inv2sMax) == 288 && offsetof(RoomConstants, wingS) == 304 && offsetof(RoomConstants, scrBoxZ) == 320 &&
              offsetof(RoomConstants, tint) == 336, "rows 11-20 as in the v11 spec (section 3.5)");
static_assert(offsetof(RoomConstants, bounceRgb) == 432 && offsetof(RoomConstants, sconcePitch) == 448 && offsetof(RoomConstants, sconceHalfH) == 464 &&
              offsetof(RoomConstants, rugX0) == 480 && offsetof(RoomConstants, ambient) == 496, "rows 21-31: the environment (XENV.md)");

// Whether any of the v11 controls is on in these constants, so that the eye pass needs
// their code (xrapp5.cpp RoomEyePso picks its look variant): the room light, the finish or
// the reflections - or an environment other than the room.
inline bool RoomLookOn(const RoomConstants& rc)
{
    return (rc.flags & (kRoomFlagLight | kRoomFlagFinish | kRoomFlagReflect)) != 0 || rc.env != (uint32_t)kRoomEnvRoom;
}

inline float RoomBounceScale(const RoomShading& sh)
{
    return sh.rhoBar < 0.999f ? sh.rhoBar * sh.invArea / (1.0f - sh.rhoBar) : 0.0f;
}

// alpha: how far the screen's light moves towards this frame's picture (1: all the way).
inline RoomConstants MakeRoomConstants(const Room& r, const RoomShading& sh, const RoomEmitterLayout& l, const RoomView& view,
                                       int srcW, int srcH, float alpha)
{
    RoomConstants c;
    if (!r.valid) return c;
    c.X = r.X; c.yF = r.yF; c.yC = r.yC; c.zB = r.zB;
    c.g = r.g; c.R = r.R; c.Rg = r.Rg; c.phiA = r.phiA;
    c.sinA = r.sinA; c.cosA = r.cosA; c.xa = r.xa; c.za = r.za;
    c.sMax = r.sMax; c.zSide = r.zSide; c.glowHalfW = view.glowHalfW; c.glowHalfH = view.glowHalfH;
    c.rhoWall = sh.rhoWall; c.rhoFloor = sh.rhoFloor; c.rhoCeiling = sh.rhoCeiling; c.bounceScale = RoomBounceScale(sh);
    c.world[0] = sh.world[0]; c.world[1] = sh.world[1]; c.world[2] = sh.world[2]; c.world[3] = 1;
    c.alpha = alpha < 0 ? 0.0f : (alpha > 1 ? 1.0f : alpha);
    c.screenW = view.W; c.screenH = view.H;
    c.flags = (r.curved ? kRoomFlagCurved : 0) | (view.flatLayer ? kRoomFlagFlatLayer : 0) | (view.glowOn ? kRoomFlagGlow : 0) |
              (view.dither ? kRoomFlagDither : 0) | (sh.lightOn ? kRoomFlagLight : 0) | (sh.finish ? kRoomFlagFinish : 0);
    c.gridX = (uint32_t)l.gridX; c.gridY = (uint32_t)l.gridY; c.emitters = (uint32_t)l.count();
    c.srcW = (uint32_t)(srcW > 0 ? srcW : 0); c.srcH = (uint32_t)(srcH > 0 ? srcH : 0); c.stride = (uint32_t)RoomStride(srcW);
    c.glowW = (uint32_t)l.glowW; c.glowH = (uint32_t)l.glowH; c.blocksX = (uint32_t)l.blocksX; c.blocksY = (uint32_t)l.blocksY;
    c.glowBlock = (uint32_t)l.block;
    // Rows 11-12: the finish - how clear the glass is, its reflections and their mean (0
    // while the finish is off), and the frames' bays and crossbar (always, from the room).
    c.glass = sh.finish ? sh.glass : 0.0f; c.reflect = sh.finish ? sh.reflect : 0.0f; c.fbar = sh.finish ? sh.fbar : 0.0f;
    c.pitchSide = r.pitchSide; c.pitchBack = r.pitchBack; c.transomY = r.transomY;
    // Row 16 and flag 32: with Reflections above 0, the mirror picture's size for this
    // source (the MIRROR pass runs only then). No source, no mirror, no reflections.
    int mirrorW = 0, mirrorH = 0;
    if (c.reflect > 0 && RoomMirrorSize(srcW, srcH, &mirrorW, &mirrorH))
    {
        c.flags |= kRoomFlagReflect;
        c.mirrorW = (uint32_t)mirrorW; c.mirrorH = (uint32_t)mirrorH;
    }
    // Rows 13-15: the room light's panel, its emitted radiance (EMIT gives it to the lamp
    // emitter; w is 1 when the panel fits, as the emitter's n.w) and its seen radiance.
    c.lightX0 = r.lightX0; c.lightX1 = r.lightX1; c.lightZ0 = r.lightZ0; c.lightZ1 = r.lightZ1;
    for (int ch = 0; ch < 3; ch++) { c.lightL[ch] = sh.lightL[ch]; c.lightSeen[ch] = sh.lightSeen[ch]; }
    c.lightL[3] = r.lightValid ? 1.0f : 0.0f;
    // Rows 17-20: the eye pass's reciprocals and the screen's bounds, the same floats the
    // CPU reference uses (the room's own, RoomRecip of the glow's size, RoomScreenBounds).
    c.invH = r.invH; c.inv2X = r.inv2X; c.invSide = r.invSide; c.invFloorZ = r.invFloorZ;
    c.inv2sMax = r.inv2sMax; c.invGlowW = RoomRecip(2.0f * view.glowHalfW); c.invGlowH = RoomRecip(2.0f * view.glowHalfH); c.tanA = r.tanA;
    const RoomScreenBox box = RoomScreenBounds(view.cyl);
    c.wingS = r.wingS; c.scrTanWrap = box.tanWrap; c.scrBoxX = box.boxX; c.scrBoxY = box.boxY;
    c.scrBoxZ = box.boxZ;
    // Rows 16 and 21-31: the environment - its faces' colours, the bounce per channel, the
    // side walls' lights (the cinema's sconces in every bay, the lounge's two lamps a wall),
    // the lounge's rug and the open air's night.
    c.env = (uint32_t)sh.env;
    for (int f = 0; f < kRoomFaces; f++)
        for (int ch = 0; ch < 3; ch++) c.tint[f][ch] = sh.tint[f][ch];
    for (int ch = 0; ch < 3; ch++) { c.bounceRgb[ch] = RoomBounceOf(sh, ch); c.ambient[ch] = sh.ambient[ch]; }
    if (sh.env == kRoomEnvCinema || sh.env == kRoomEnvLounge || sh.env == kRoomEnvTheatre)
    {
        const bool cinema = sh.env != kRoomEnvLounge;
        const float side = r.zB - r.zSide;
        c.sconcePitch = cinema ? r.pitchSide : 0.5f * side;
        c.sconceZ0 = r.zSide + 0.5f * c.sconcePitch;
        c.sconceY = std::fmin(r.yF + (cinema ? 2.0f : 1.6f), r.yC - 0.4f);
        c.sconceHalfW = cinema ? 0.07f : 0.11f;
        c.sconceHalfH = cinema ? 0.13f : 0.08f;
        c.haloGain = cinema ? 0.5f : 0.6f;
        c.haloUp = cinema ? 0.9f : 0.6f;
        c.haloDown = cinema ? 0.12f : 0.45f;
    }
    if (sh.env == kRoomEnvLounge) { c.rugX0 = r.rugX0; c.rugX1 = r.rugX1; c.rugZ0 = r.rugZ0; c.rugZ1 = r.rugZ1; }
    // Rows 32-34: the theatre's stands and the coarse patches after the room's emitters.
    if (sh.env == kRoomEnvTheatre && r.standsRows > 0)
    {
        c.standsZ0 = r.standsZ0; c.standsPitch = r.standsPitch; c.standsT0 = r.standsT0; c.standsRiser = r.standsRiser;
        c.seatX0 = r.seatX0; c.seatBlock = r.seatBlock; c.standsZEnd = r.zB;
        c.standsRows = (uint32_t)r.standsRows; c.viewerRow = (uint32_t)r.viewerRow;
        c.coarseFirst = (uint32_t)l.count(); c.coarseCount = (uint32_t)l.coarse;
        c.seatJMin = r.seatJMin; c.seatCols = (uint32_t)r.seatCols;
    }
    return c;
}

// ------------------------------------------------------------------ eye pass
// The 8x8 Bayer matrix: a +-half-step dither on room surfaces only, so dark walls do
// not band; the second eye's pattern is offset by (4, 4).
static const int kRoomBayer[64] = {
     0, 32,  8, 40,  2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37, 63, 31, 55, 23, 61, 29, 53, 21 };

inline float RoomDither(int eye, int px, int py)
{
    const int o = eye ? 4 : 0;
    return (((float)kRoomBayer[((px + o) & 7) + ((py + o) & 7) * 8] + 0.5f) / 64.0f - 0.5f) / 255.0f;
}

// Half-float (the lightmap's format) to float, for reading the GPU's lightmap back.
inline float RoomHalfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 31, man = h & 1023;
    uint32_t bits;
    if (exp == 0)
    {
        if (man == 0) bits = sign;
        else
        {
            int e = -1;
            uint32_t m = man;
            do { e++; m <<= 1; } while (!(m & 1024));
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((m & 1023) << 13);
        }
    }
    else if (exp == 31) bits = sign | 0x7F800000u | (man << 13);
    else bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// A lightmap as floats, [face][row][column][rgba], for the CPU reference.
struct RoomLightmap
{
    std::vector<float> texels;          // kRoomFaces * kRoomLightmap^2 * 4
    bool Sample(int face, float u, float v, float out[3]) const
    {
        if (!out || face < 0 || face >= kRoomFaces || texels.size() != (size_t)kRoomFaces * kRoomLightmap * kRoomLightmap * 4) return false;
        const float x = u * kRoomLightmap - 0.5f, y = v * kRoomLightmap - 0.5f;
        const float fx0 = std::floor(x), fy0 = std::floor(y), fx = x - fx0, fy = y - fy0;
        auto clampi = [](int i) { return i < 0 ? 0 : (i > kRoomLightmap - 1 ? kRoomLightmap - 1 : i); };
        const int x0 = clampi((int)fx0), x1 = clampi((int)fx0 + 1), y0 = clampi((int)fy0), y1 = clampi((int)fy0 + 1);
        auto at = [&](int xi, int yi, int ch) { return texels[(((size_t)face * kRoomLightmap + yi) * kRoomLightmap + xi) * 4 + ch]; };
        for (int ch = 0; ch < 3; ch++)
        {
            const float top = at(x0, y0, ch) + fx * (at(x1, y0, ch) - at(x0, y0, ch));
            const float bot = at(x0, y1, ch) + fx * (at(x1, y1, ch) - at(x0, y1, ch));
            out[ch] = top + fy * (bot - top);
        }
        return true;
    }
};


// Sample kinds for the four rays: 0 picture, 1 + face, 7 footprint, 8 outside.
static const int kRoomKindFootprint = 7, kRoomKindOutside = 8;

// What the eye pass works out once per pixel, not per ray: the screen's bounds, the
// glow's reciprocals, and whether the eye is in the room (all four rays start at the
// eye). `part` is the surface of the pixel's last full exit (-1 before one): a later ray
// that misses the screen tries that surface alone (RoomExitOnFace) and takes the full
// exit only if it is not there - one full exit per pixel, bar edges.
struct RoomPixelState
{
    RoomScreenBox box;
    float invGlowW = 0, invGlowH = 0;
    bool inside = false;
    int part = -1;
};

inline RoomPixelState MakeRoomPixelState(const Cylinder& cyl, const Room& r, const RoomView& view, const float eye[3])
{
    RoomPixelState s;
    s.box = RoomScreenBounds(cyl);
    s.invGlowW = RoomRecip(2.0f * view.glowHalfW);
    s.invGlowH = RoomRecip(2.0f * view.glowHalfH);
    s.inside = eye && RoomInside(r, eye);
    return s;
}

inline bool RoomStandsClassify(const Room& r, const float o[3], const float d[3], const float rcp[3], float* t, int* kind);

// One ray of a pixel, as the eye pass classifies it (kCurveRoomHlsl ClassifyR). In the
// theatre, the stands come before the room's faces (they lie inside it): their kind
// (kRoomKindStands and on) and the ray's t in u.
inline int RoomClassifyRay(const Cylinder& cyl, const Room& r, const RoomView& view, RoomPixelState& state,
                           const float o[3], const float d[3], float* u, float* v, float* gu, float* gv, int standsKind = -1, float standsT = 0)
{
    if (!u || !v || !gu || !gv) return kRoomKindOutside;
    *u = *v = *gu = *gv = 0;
    if (!o || !d) return kRoomKindOutside;
    float rcp[3];
    RoomRayRcp(d, rcp);
    if (!view.flatLayer)
    {
        if (RoomScreenHit(cyl, state.box, o, d, rcp, u, v)) return 0;
    }
    else if (d[2] < 0)
    {
        const float t = -o[2] * rcp[2];
        const float hx = o[0] + t * d[0], hy = o[1] + t * d[1];
        if (t > 0 && std::fabs(hx) <= 0.5f * view.W && std::fabs(hy) <= 0.5f * view.H) return kRoomKindFootprint;
    }
    if (!state.inside) return kRoomKindOutside;
    // The stands: the pixel's sample for this ray (RoomPixel, from the id buffer's neighbourhood
    // as the GPU has it), or, for a ray on its own (standsKind -1), the full trace.
    if (r.standsRows > 0)
    {
        if (standsKind > 0) { *u = standsT; return standsKind; }
        int kind = 0;
        if (standsKind < 0 && RoomStandsClassify(r, o, d, rcp, u, &kind)) return kind;
    }
    RoomHit hit;
    bool found = state.part >= 0 && RoomExitOnFace(r, state.part, o, d, rcp, hit);
    if (!found)
    {
        found = RoomExitFast(r, o, d, rcp, hit);
        if (found) state.part = hit.part;
    }
    if (!found) return kRoomKindOutside;
    *u = hit.u; *v = hit.v;
    if (hit.face == kFaceFront) { *gu = hit.s * state.invGlowW + 0.5f; *gv = 0.5f - hit.y * state.invGlowH; }
    return 1 + hit.face;
}

// One ray on its own (a pixel's first): whether the eye is in the room from `o` itself.
inline int RoomClassify(const CurveConstants& c, const Cylinder& cyl, const Room& r, const RoomView& view,
                        const float o[3], const float d[3], float* u, float* v, float* gu, float* gv)
{
    RoomPixelState state = MakeRoomPixelState(cyl, r, view, o);
    (void)c;
    return RoomClassifyRay(cyl, r, view, state, o, d, u, v, gu, gv);
}

// ------------------------------------------------------- footprints and patterns
// A pixel's rays move linearly with px and py (CurveRay), so these are exact: how the
// direction moves per pixel across (Dx) and down (Dy). A half-size layer's pixels are
// twice as wide, and so are its footprints.
inline bool RoomRayDiff(const CurveConstants& c, int e, float Dx[3], float Dy[3])
{
    if (!Dx || !Dy || e < 0 || e > 1 || c.ew == 0 || c.eh == 0) return false;
    const CurveEye& v = c.eye[e];
    const float sx = (v.tanR - v.tanL) / (float)c.ew, sy = (v.tanD - v.tanU) / (float)c.eh;
    Dx[0] = v.row0[0] * sx; Dx[1] = v.row1[0] * sx; Dx[2] = v.row2[0] * sx;
    Dy[0] = v.row0[1] * sy; Dy[1] = v.row1[1] * sy; Dy[2] = v.row2[1] * sy;
    return true;
}

// Where a ray (direction d) meets a plane with normal n at t: how far the hit moves per
// pixel across (Px) and down (Py), from the ray differentials. False if the ray runs
// along the plane.
inline bool RoomPlaneFootprint(float t, const float d[3], const float n[3], const float Dx[3], const float Dy[3], float Px[3], float Py[3])
{
    if (!d || !n || !Dx || !Dy || !Px || !Py) return false;
    const float nd = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
    if (nd == 0) return false;
    const float kx = (n[0] * Dx[0] + n[1] * Dx[1] + n[2] * Dx[2]) / nd, ky = (n[0] * Dy[0] + n[1] * Dy[1] + n[2] * Dy[2]) / nd;
    for (int k = 0; k < 3; k++)
    {
        Px[k] = t * (Dx[k] - d[k] * kx);
        Py[k] = t * (Dy[k] - d[k] * ky);
    }
    return true;
}

// The footprint along one of the plane's own axes (0 x, 1 y, 2 z): |Px| + |Py| there,
// clamped to [kRoomFootMin, kRoomFootMax].
inline float RoomFootprintAxis(const float Px[3], const float Py[3], int axis)
{
    if (!Px || !Py || axis < 0 || axis > 2) return kRoomFootMin;
    return std::fmin(std::fmax(std::fabs(Px[axis]) + std::fabs(Py[axis]), kRoomFootMin), kRoomFootMax);
}

// A bar of width w centred on c, box-filtered over a footprint f round s: how much of
// [s - f/2, s + f/2] it covers, 0..1.
inline float RoomBar(float s, float c, float w, float f)
{
    if (!(f > 0)) return 0;
    const float v = (std::fmin(s + 0.5f * f, c + 0.5f * w) - std::fmax(s - 0.5f * f, c - 0.5f * w)) / f;
    return std::fmin(std::fmax(v, 0.0f), 1.0f);
}

// How much of a footprint fx across by fz along, round ceiling point (x, z), the room
// light's panel covers (kappa). 0 with no panel: its extents are all zero then.
inline float RoomPanelCoverage(const RoomConstants& rc, float x, float z, float fx, float fz)
{
    return RoomBar(x, 0.5f * (rc.lightX0 + rc.lightX1), rc.lightX1 - rc.lightX0, fx) *
           RoomBar(z, 0.5f * (rc.lightZ0 + rc.lightZ1), rc.lightZ1 - rc.lightZ0, fz);
}

// The panel's coverage of a pixel where its ray (o, d) meets the ceiling: the hit on the
// plane y = yC (one divide) and its footprint from the pixel differentials. 0 for a ray
// that does not rise.
inline float RoomCeilingPanel(const RoomConstants& rc, const float o[3], const float d[3], const float Dx[3], const float Dy[3])
{
    if (!o || !d || !Dx || !Dy) return 0;
    if (!(d[1] > 0)) return 0;
    const float t = (rc.yC - o[1]) / d[1];
    float p[3], Px[3], Py[3];
    RoomAt(o, d, t, p);
    const float n[3] = { 0, -1, 0 };
    if (!RoomPlaneFootprint(t, d, n, Dx, Dy, Px, Py)) return 0;
    return RoomPanelCoverage(rc, p[0], p[2], RoomFootprintAxis(Px, Py, 0), RoomFootprintAxis(Px, Py, 2));
}

// A train of bars of width w centred on the multiples of P, box-filtered over a footprint
// f round s: with u = s + w/2, the bars are [kP, kP + w], C(u) counts the bar length below
// u, and the coverage is (C(u1) - C(u0)) / f. C is continuous, so a float rounding of
// floor() cannot step it; two points in one gap give exactly 0, in one bar exactly 1,
// and the mean over a period is w / P for any f.
inline float RoomPulse(float s, float P, float w, float f)
{
    if (!(f > 0) || !(P > 0)) return 0;
    const float u0 = s + 0.5f * w - 0.5f * f, u1 = s + 0.5f * w + 0.5f * f;
    const float k0 = std::floor(u0 / P), k1 = std::floor(u1 / P);
    const float r0 = u0 - k0 * P, r1 = u1 - k1 * P;
    if (k0 == k1 && r1 <= w) return 1.0f;
    const float v = ((k1 - k0) * w + std::fmin(r1, w) - std::fmin(r0, w)) / f;
    return std::fmin(std::fmax(v, 0.0f), 1.0f);
}

// The theatre's carpet at (u, v) metres along a floor's or step's two axes, its footprint fu,
// fv along them: the colour (relative albedo) box-filtered over the footprint - the lattice's
// lines and the cells' diamonds and dots as bars along the two diagonals (RoomPulse), the
// footprint along a diagonal taken as (fu + fv) / sqrt 2.
inline void RoomCarpet(float u, float v, float fu, float fv, float c[3])
{
    if (!c) return;
    const float r = 0.70710678f, P = kRoomCarpetTile * r;
    const float a = (u + v) * r, b = (u - v) * r, f = (fu + fv) * r;
    const float la = RoomPulse(a, P, kRoomCarpetLineW, f), lb = RoomPulse(b, P, kRoomCarpetLineW, f);
    const float line = la + lb - la * lb;
    const float diamond = RoomPulse(a + 0.5f * P, P, kRoomCarpetDiamond, f) * RoomPulse(b + 0.5f * P, P, kRoomCarpetDiamond, f);
    const float dot = RoomPulse(a + 0.5f * P, P, kRoomCarpetDot, f) * RoomPulse(b + 0.5f * P, P, kRoomCarpetDot, f);
    for (int ch = 0; ch < 3; ch++)
        c[ch] = kRoomCarpetBase[ch] * (1.0f - line - diamond) + kRoomCarpetGold[ch] * (line + dot) + kRoomCarpetTeal[ch] * (diamond - dot);
}

// How much of a pixel's footprint the frames cover (phi) at point p of face `face` (left,
// right, back or ceiling), the footprint fa along the face's first axis (z on the side
// walls, x on the back wall and the ceiling) and fb along its second (y on the walls, z
// on the ceiling): uprights (ceiling: beams across) every bay, one in each corner, and on
// the walls bars along the floor, at the crossbar and along the ceiling.
inline float RoomFrameCoverage(const RoomConstants& rc, int face, const float p[3], float fa, float fb)
{
    if (!p) return 0;
    float cv = 0, ch = 0;
    if (face == kFaceLeft || face == kFaceRight) cv = RoomPulse(p[2] - rc.zSide, rc.pitchSide, kRoomFrameW, fa);
    else if (face == kFaceBack || face == kFaceCeiling) cv = RoomPulse(p[0] + rc.X, rc.pitchBack, kRoomFrameW, fa);
    else return 0;
    if (face == kFaceCeiling) ch = RoomPulse(p[2] - rc.zSide, rc.pitchSide, kRoomFrameW, fb);
    else ch = std::fmin(1.0f, RoomBar(p[1], rc.yF, kRoomFrameW, fb) + RoomBar(p[1], rc.transomY, kRoomFrameW, fb) + RoomBar(p[1], rc.yC, kRoomFrameW, fb));
    return 1.0f - (1.0f - cv) * (1.0f - ch);
}

// The floor tiles' hash: a tone per 1 m tile, 0..65535, the same in C++ and HLSL (uint32
// wrap-around in both).
inline uint32_t RoomHash(int i, int j)
{
    uint32_t k = ((uint32_t)(i + 1024) * 73856093u) ^ ((uint32_t)(j + 1024) * 19349663u);
    k ^= k >> 13;
    k *= 0x5bd1e995u;
    k ^= k >> 15;
    return k & 0xFFFFu;
}

// The floor's tiles at floor point p, footprint fx across and fz along: the grout lines'
// cover g (lines on x = k and z = k, so one runs from the screen's middle towards the
// viewer) and the factor tau = (1 - kRoomGrout g) x the tile's tone, whose variation
// fades out as the footprint nears half a tile.
inline float RoomTileFactor(const float p[3], float fx, float fz, float* g)
{
    if (!p) return 1.0f;
    const float grout = 1.0f - (1.0f - RoomPulse(p[0], 1.0f, kRoomGroutW, fx)) * (1.0f - RoomPulse(p[2], 1.0f, kRoomGroutW, fz));
    const float h = (float)RoomHash((int)std::floor(p[0]), (int)std::floor(p[2]));
    const float fade = std::fmin(std::fmax(1.0f - 2.0f * std::fmax(fx, fz), 0.0f), 1.0f);
    const float tone = 1.0f + kRoomTileVar * (h / 65535.0f - 0.5f) * fade;
    if (g) *g = grout;
    return (1.0f - kRoomGrout * grout) * tone;
}

// What a ray (o, d) sees beyond the glass, linear rgb, tinted by the world colour w: above
// the horizon a sky from 1.6 w at the horizon to 0.5 w overhead; below it a ground at the
// room's floor level, 0.45 w with a faint 1 m grid (filtered over the pixel's footprint
// from Dx and Dy, or its mean when they are null: a reflected ray), fading into the
// horizon's colour with distance; within kRoomEnvHorizonDy of level, the horizon's colour.
// `kind` (may be null): 1 sky, 2 horizon, 3 ground.
inline bool RoomEnvironment(const RoomConstants& rc, const float o[3], const float d[3], const float* Dx, const float* Dy, float out[3], int* kind)
{
    if (!o || !d || !out) return false;
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 0)) return false;
    const float dy = d[1] / len;
    float hc[3];
    for (int ch = 0; ch < 3; ch++) hc[ch] = kRoomEnvHorizon * rc.world[ch];
    if (dy >= 0)
    {
        const float sd = std::sqrt(dy);
        for (int ch = 0; ch < 3; ch++) out[ch] = hc[ch] + (kRoomEnvZenith * rc.world[ch] - hc[ch]) * sd;
        if (kind) *kind = 1;
        return true;
    }
    if (dy > -kRoomEnvHorizonDy)
    {
        for (int ch = 0; ch < 3; ch++) out[ch] = hc[ch];
        if (kind) *kind = 2;
        return true;
    }
    const float tg = (rc.yF - o[1]) / d[1];
    float q[3];
    RoomAt(o, d, tg, q);
    float line = kRoomEnvLineMean;
    if (Dx && Dy)
    {
        float Px[3], Py[3];
        const float n[3] = { 0, 1, 0 };
        RoomPlaneFootprint(tg, d, n, Dx, Dy, Px, Py);
        line = 1.0f - (1.0f - RoomPulse(q[0], 1.0f, kRoomEnvLineW, RoomFootprintAxis(Px, Py, 0))) *
                      (1.0f - RoomPulse(q[2], 1.0f, kRoomEnvLineW, RoomFootprintAxis(Px, Py, 2)));
    }
    const float fog = 1.0f - std::exp(-tg * len / kRoomEnvFog);
    for (int ch = 0; ch < 3; ch++)
    {
        const float G = kRoomEnvGround * rc.world[ch] * (1.0f + kRoomEnvLine * line);
        out[ch] = G + (hc[ch] - G) * fog;
    }
    if (kind) *kind = 3;
    return true;
}

// What the eye pass reads besides its geometry: the room's constants (the room light's
// panel, colour and flag, read from here as the GPU reads them from RoomC), each eye's
// picture (it may have no pixels in the flat layer, which never samples it), the glow
// (null: none), the lightmap, and for reflections the mirror picture and the room, whose
// faces a reflected ray leaves through (RoomPixel fills `room` in when it is null).
struct RoomEyeInputs
{
    const RoomConstants* rc = nullptr;
    const RgbaImage* picture = nullptr;
    const RgbaImage* glow = nullptr;
    const RoomLightmap* light = nullptr;
    const RoomMirror* mirror = nullptr;
    const Room* room = nullptr;
    const std::vector<RoomEmitter>* emitters = nullptr;     // the theatre: its coarse patches and the room light light the stands
};

// What the finish drew at a sample, for the self-test's coverage counts.
struct RoomSurfaceInfo
{
    float phi = 0;                      // the frames' cover (left, right, back, ceiling)
    float kappa = 0;                    // the room light's panel's cover (ceiling)
    float grout = -1;                   // the floor's grout cover (-1: not a tiled floor sample)
    int env = 0;                        // beyond the glass: 0 not drawn, 1 sky, 2 horizon, 3 ground
    float fresnel = 0;                  // the coat's reflectance where a reflection was traced (F, or Ff on the floor), else 0
    float kappaS = 0;                   // how much of that reflection is the screen (its soft edge)
    int envPart = 0;                    // what an environment drew (kRoomPart*), 0 nothing of its own
    float star = 0;                     // the open air: the stars' light at the sample
};

// What an environment drew at a sample (RoomSurfaceInfo::envPart): the open air's sky, hills
// and field beyond the room; the cinema's masking, sconces (lit or not) and step lights; the
// lounge's rug, downlights and floorboards' seams; and the fan of a lit light on a wall.
static const int kRoomPartSky = 1, kRoomPartHills = 2, kRoomPartField = 3, kRoomPartMask = 4, kRoomPartSconce = 5,
                 kRoomPartStep = 6, kRoomPartRug = 7, kRoomPartDownlight = 8, kRoomPartHalo = 9, kRoomPartSeam = 10,
                 kRoomPartUnderSeat = 11;

// A face's axis (0 x, 1 y, 2 z) and the sign of its normal into the room along it.
inline int RoomFaceAxis(int face, float* sgn)
{
    if (sgn) *sgn = (face == kFaceLeft || face == kFaceFloor) ? 1.0f : -1.0f;
    return (face == kFaceLeft || face == kFaceRight) ? 0 : ((face == kFaceFloor || face == kFaceCeiling) ? 1 : 2);
}

// The screen as a reflected ray (o, d, rcp = 1/d) sees it, within `widen` metres of its
// outline: the curved screen (a slab test against the screen's box grown by `widen` - the
// widened arc lies inside it - then the arc's quadratic, and atan2 for the angle: this path
// is secondary only) or the flat layer's rectangle at z = 0. Gives its uv (the picture's),
// the ray's t there and how far inside the outline the hit lies, e (negative outside).
inline bool RoomScreenWideHit(const CurveConstants& c, const RoomConstants& rc, const float o[3], const float d[3], const float rcp[3], float widen,
                              float* u, float* v, float* t, float* e)
{
    if (!o || !d || !rcp || !u || !v || !t || !e) return false;
    *u = *v = *t = *e = 0;
    if ((rc.flags & kRoomFlagFlatLayer) != 0)
    {
        if (!(d[2] < 0) || !(rc.screenW > 0) || !(rc.screenH > 0)) return false;
        const float ts = -o[2] / d[2];
        const float x = o[0] + ts * d[0], y = o[1] + ts * d[1];
        const float hw = 0.5f * rc.screenW, hh = 0.5f * rc.screenH;
        if (!(ts > 0) || std::fabs(x) > hw + widen || std::fabs(y) > hh + widen) return false;
        *t = ts; *u = x / rc.screenW + 0.5f; *v = 0.5f - y / rc.screenH;
        *e = std::fmin(hw - std::fabs(x), hh - std::fabs(y));
        return true;
    }
    const float R = c.radius;
    if (!(R > 0) || !(c.halfWidth > 0) || !(c.halfHeight > 0)) return false;
    const float lo[3] = { -rc.scrBoxX - widen, -rc.scrBoxY - widen, -kRoomScreenPad - widen }, hi[3] = { rc.scrBoxX + widen, rc.scrBoxY + widen, rc.scrBoxZ + widen };
    float tn[3], tf[3];
    for (int k = 0; k < 3; k++)
    {
        const float t1 = (lo[k] - o[k]) * rcp[k], t2 = (hi[k] - o[k]) * rcp[k];
        tn[k] = std::fmin(t1, t2); tf[k] = std::fmax(t1, t2);
    }
    const float enter = std::fmax(std::fmax(tn[0], tn[1]), tn[2]), leave = std::fmin(std::fmin(tf[0], tf[1]), tf[2]);
    if (!(leave >= std::fmax(enter, 0.0f))) return false;
    const float a = d[0] * d[0] + d[2] * d[2];
    if (!(a > 1e-12f)) return false;
    const float b = 2.0f * (o[0] * d[0] + (o[2] - R) * d[2]);
    const float cc = o[0] * o[0] + o[2] * o[2] - 2.0f * o[2] * R;
    const float disc = b * b - 4.0f * a * cc;
    if (disc < 0) return false;
    const float sq = std::sqrt(disc);
    const float q = -0.5f * (b + (b < 0 ? -sq : sq));
    float t0 = q / a, t1 = (q != 0) ? cc / q : t0;
    if (t0 > t1) { const float swap = t0; t0 = t1; t1 = swap; }
    for (int i = 0; i < 2; i++)
    {
        const float tr = i == 0 ? t0 : t1;
        if (!(tr > 0)) continue;
        const float x = o[0] + tr * d[0], y = o[1] + tr * d[1], z = o[2] + tr * d[2];
        const float toward = R - z;
        if (!(toward > 0)) continue;
        const float phi = std::atan2(x, toward);
        if (std::fabs(phi) > c.halfWrap + widen / R || std::fabs(y) > c.halfHeight + widen) continue;
        *t = tr;
        *u = (R * phi) / (2.0f * c.halfWidth) + 0.5f;
        *v = 0.5f - y / (2.0f * c.halfHeight);
        *e = std::fmin(R * (c.halfWrap - std::fabs(phi)), c.halfHeight - std::fabs(y));
        return true;
    }
    return false;
}

// A reflection (spec 2.6): what the coat of a sample on `face` mirrors. The sample's ray
// (direction d, its pixel's differentials Dx and Dy) met the face at p, t; the reflected
// ray keeps the ray's origin eye (so each eye sees its own mirror image) and starts
// kRoomNudge along itself from p (along the normal would shift the mirror image by as much). It sees the screen as the mirror picture, with a soft edge over
// its footprint fw = (|Dx| + |Dy|)(t + t_s) (at most kRoomReflFootMax): kappa_s =
// clamp(e / fw + 0.5, 0, 1). Round or behind the screen it leaves the room through a face
// that shows the lightmap as the finish's secondary surfaces do - no frames, tiles or
// further reflections: the front wall with the glow, the floor coated, the walls and the
// ceiling as glass over the ground's mean grid, the ceiling's panel with an isotropic
// footprint. `own` (the sample's own coated light) stands in if it finds no exit, which
// only a ray from an edge can. kappaS (may be null) gets kappa_s.
inline bool RoomReflection(const CurveConstants& c, const RoomEyeInputs& in, int face, const float p[3], const float d[3], float t,
                           const float Dx[3], const float Dy[3], const float own[3], float out[3], float* kappaS)
{
    if (!p || !d || !Dx || !Dy || !own || !out) return false;
    if (!in.rc || !in.light || !in.mirror || !in.room) return false;
    if (face <= kFaceFront || face >= kRoomFaces) return false;
    const RoomConstants& rc = *in.rc;
    const int axis = RoomFaceAxis(face, nullptr);
    float o2[3], d2[3] = { d[0], d[1], d[2] };
    d2[axis] = -d2[axis];
    RoomAt(p, d2, kRoomNudge / std::sqrt(d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2]), o2);
    const float spread = std::sqrt(Dx[0] * Dx[0] + Dx[1] * Dx[1] + Dx[2] * Dx[2]) + std::sqrt(Dy[0] * Dy[0] + Dy[1] * Dy[1] + Dy[2] * Dy[2]);
    float rcp[3];
    RoomRayRcp(d2, rcp);
    float S[3] = { 0, 0, 0 }, ks = 0, su = 0, sv = 0, ts = 0, es = 0;
    if (RoomScreenWideHit(c, rc, o2, d2, rcp, kRoomReflFootMax, &su, &sv, &ts, &es))
    {
        const float fw = std::fmin(std::fmax(spread * (t + ts), kRoomFootMin), kRoomReflFootMax);
        ks = std::fmin(std::fmax(es / fw + 0.5f, 0.0f), 1.0f);
        if (ks > 0 && !in.mirror->Sample(su, sv, S)) return false;
    }
    if (kappaS) *kappaS = ks;
    if (ks >= 1.0f)
    {
        out[0] = S[0]; out[1] = S[1]; out[2] = S[2];
        return true;
    }
    float B[3] = { own[0], own[1], own[2] };
    RoomHit h;
    if (RoomExitFast(*in.room, o2, d2, rcp, h))
    {
        float Ld2[3];
        if (!in.light->Sample(h.face, h.u, h.v, Ld2)) return false;
        if (h.face == kFaceFront)
        {
            for (int ch = 0; ch < 3; ch++) B[ch] = Ld2[ch];
            const float gu = h.s * rc.invGlowW + 0.5f, gv = 0.5f - h.y * rc.invGlowH;
            if ((rc.flags & kRoomFlagGlow) != 0 && in.glow && gu >= 0 && gu <= 1 && gv >= 0 && gv <= 1)
            {
                float g[4];
                if (!SampleRgba(*in.glow, gu, gv, g)) return false;
                for (int ch = 0; ch < 3; ch++) B[ch] += SrgbToLinear(g[ch]);
            }
        }
        else if (h.face == kFaceFloor)
        {
            for (int ch = 0; ch < 3; ch++) B[ch] = (1.0f - kRoomFloorGloss * rc.fbar) * Ld2[ch];
        }
        else
        {
            float env[3] = { 0, 0, 0 };
            if (rc.glass > 0 && !RoomEnvironment(rc, o2, d2, nullptr, nullptr, env, nullptr)) return false;
            for (int ch = 0; ch < 3; ch++) B[ch] = (1.0f - rc.glass) * (1.0f - rc.fbar) * Ld2[ch] + rc.glass * env[ch];
            if (h.face == kFaceCeiling)
            {
                const float t2 = (rc.yC - o2[1]) * rcp[1];
                float p2[3];
                RoomAt(o2, d2, t2, p2);
                const float f2 = std::fmin(std::fmax(spread * (t + t2), kRoomFootMin), kRoomFootMax);
                const float k2 = RoomPanelCoverage(rc, p2[0], p2[2], f2, f2);
                for (int ch = 0; ch < 3; ch++) B[ch] = k2 * (Ld2[ch] + rc.lightSeen[ch]) + (1.0f - k2) * B[ch];
            }
        }
    }
    for (int ch = 0; ch < 3; ch++) out[ch] = ks * S[ch] + (1.0f - ks) * B[ch];
    return true;
}

// A room surface's radiance at a primary sample (spec 2.5): L comes in as the lightmap's
// Ld and goes out as what the eye sees there. `face` is the sample's face and (o, d) its
// ray, Dx and Dy the ray's differentials: the point is the ray's hit on the face's plane
// (one divide), and its footprint filters the frames, tiles, panel and ground grid.
//   finish off: Ld, plus the room light's panel over the ceiling (kappa Ls);
//   finish on:  floor    Ff Lrefl + (1 - Ff)(1 - fbarF) tau Ld
//               walls    phi Ld + (1 - phi) [F Lrefl + (1 - F)((1 - T)(1 - fbar) Ld + T Env)]   (frames opaque)
//               ceiling  kappa (Ld + Ls) + (1 - kappa) x the walls' formula
// F and Ff are the coat's Fresnel terms at the ray's angle to the face (the floor's without
// its grout), and Lrefl what it mirrors (RoomReflection, which reads `c` and `in`: both
// needed with flag 32); with Reflections 0 they are 0 and no reflection is traced, nor
// where the frames or the panel cover the whole footprint. With Glass and Reflections 0 the
// walls are Ld again, frames and panes alike. The front is never finished.
inline bool RoomSurface(const RoomConstants& rc, int face, const float o[3], const float d[3], const float Dx[3], const float Dy[3], float L[3],
                        RoomSurfaceInfo* info, const CurveConstants* c = nullptr, const RoomEyeInputs* in = nullptr)
{
    if (!o || !d || !Dx || !Dy || !L) return false;
    if (face <= kFaceFront || face >= kRoomFaces) return true;
    const bool finish = (rc.flags & kRoomFlagFinish) != 0;
    if (!finish)
    {
        if (face != kFaceCeiling || (rc.flags & kRoomFlagLight) == 0) return true;
        const float kappa = RoomCeilingPanel(rc, o, d, Dx, Dy);
        for (int ch = 0; ch < 3; ch++) L[ch] += kappa * rc.lightSeen[ch];
        if (info) info->kappa = kappa;
        return true;
    }
    // The face's plane: its axis, where it lies, and its normal into the room.
    const int axis = (face == kFaceLeft || face == kFaceRight) ? 0 : ((face == kFaceFloor || face == kFaceCeiling) ? 1 : 2);
    const float plane = face == kFaceLeft ? -rc.X : (face == kFaceRight ? rc.X : (face == kFaceFloor ? rc.yF : (face == kFaceCeiling ? rc.yC : rc.zB)));
    float n[3] = { 0, 0, 0 };
    n[axis] = (face == kFaceLeft || face == kFaceFloor) ? 1.0f : -1.0f;
    const float t = (plane - o[axis]) / d[axis];
    if (!(t > 0) || !(t < 3.0e38f)) return true;
    float p[3], Px[3], Py[3];
    RoomAt(o, d, t, p);
    if (!RoomPlaneFootprint(t, d, n, Dx, Dy, Px, Py)) return true;
    const bool reflect = (rc.flags & kRoomFlagReflect) != 0;
    if (reflect && (!c || !in)) return false;
    const float cosn = std::fabs(d[axis]) / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float refl[3] = { 0, 0, 0 }, own[3], kappaS = 0;
    if (face == kFaceFloor)
    {
        float g = 0;
        const float tau = RoomTileFactor(p, RoomFootprintAxis(Px, Py, 0), RoomFootprintAxis(Px, Py, 2), &g);
        const float coat = 1.0f - kRoomFloorGloss * rc.fbar * (1.0f - g);
        const float ff = reflect ? kRoomFloorGloss * RoomFresnel(rc.reflect, cosn) * (1.0f - g) : 0.0f;
        if (ff > 0)
        {
            for (int ch = 0; ch < 3; ch++) own[ch] = coat * L[ch];
            if (!RoomReflection(*c, *in, face, p, d, t, Dx, Dy, own, refl, &kappaS)) return false;
        }
        for (int ch = 0; ch < 3; ch++)
        {
            const float base = coat * tau * L[ch];
            L[ch] = ff > 0 ? ff * refl[ch] + (1.0f - ff) * base : base;
        }
        if (info) { info->grout = g; info->fresnel = ff; info->kappaS = kappaS; }
        return true;
    }
    const int a0 = face == kFaceLeft || face == kFaceRight ? 2 : 0, a1 = face == kFaceCeiling ? 2 : 1;
    const float phi = RoomFrameCoverage(rc, face, p, RoomFootprintAxis(Px, Py, a0), RoomFootprintAxis(Px, Py, a1));
    float env[3] = { 0, 0, 0 };
    int envKind = 0;
    if (rc.glass > 0 && !RoomEnvironment(rc, o, d, Dx, Dy, env, &envKind)) return false;
    float kappa = 0;
    if (face == kFaceCeiling) kappa = RoomPanelCoverage(rc, p[0], p[2], RoomFootprintAxis(Px, Py, 0), RoomFootprintAxis(Px, Py, 2));
    // The coat mirrors only where a pane shows: not over a frame or the panel.
    const bool mirrored = reflect && (1.0f - phi) * (1.0f - kappa) > 0;
    const float F = mirrored ? RoomFresnel(rc.reflect, cosn) : 0.0f;
    if (mirrored)
    {
        for (int ch = 0; ch < 3; ch++) own[ch] = (1.0f - rc.fbar) * L[ch];
        if (!RoomReflection(*c, *in, face, p, d, t, Dx, Dy, own, refl, &kappaS)) return false;
    }
    for (int ch = 0; ch < 3; ch++)
    {
        float pane = (1.0f - rc.glass) * (1.0f - rc.fbar) * L[ch] + rc.glass * env[ch];
        if (mirrored) pane = F * refl[ch] + (1.0f - F) * pane;
        const float lg = phi * L[ch] + (1.0f - phi) * pane;
        L[ch] = face == kFaceCeiling ? kappa * (L[ch] + rc.lightSeen[ch]) + (1.0f - kappa) * lg : lg;
    }
    if (info) { info->phi = phi; info->kappa = kappa; info->env = envKind; info->fresnel = F; info->kappaS = kappaS; }
    return true;
}

// ------------------------------------------------------------- environments' details
// A cosine of period P (the cinema's folds, the lounge's slats and the oak's grain): cos(2 pi
// s / P), box-filtered over a footprint f round s, which multiplies it by sinc(pi f / P). From
// a footprint of a whole period on it is let go (0), where the filter's later lobes would
// only show moire. The angle comes from s / P's fraction, so a large s keeps its precision.
inline float RoomFolds(float s, float P, float f)
{
    if (!(P > 0) || !(f >= 0)) return 0;
    const float x = kRoomPi * f / P;
    if (!(x < kRoomPi)) return 0;
    const float q = s / P;
    const float c = std::cos(2.0f * kRoomPi * (q - std::floor(q)));
    return x < 1e-4f ? c : c * (std::sin(x) / x);
}

// The front wall at chart coordinate s from the constants (FrontPoint's): its point (x, z)
// and its normal into the room.
inline void RoomFrontAt(const RoomConstants& rc, float s, float* x, float* z, float* nx, float* nz)
{
    if (!x || !z || !nx || !nz) return;
    if ((rc.flags & kRoomFlagCurved) == 0) { *x = s; *z = -rc.g; *nx = 0; *nz = 1; return; }
    const float as = std::fabs(s), sign = s < 0 ? -1.0f : 1.0f;
    if (as <= rc.R * rc.phiA)
    {
        const float phi = as / rc.R;
        *x = sign * rc.Rg * std::sin(phi); *z = rc.R - rc.Rg * std::cos(phi);
        *nx = -sign * std::sin(phi); *nz = std::cos(phi);
        return;
    }
    const float along = (as - rc.R * rc.phiA) * (rc.Rg / rc.R);
    *x = sign * (rc.xa + along * rc.cosA); *z = rc.za + along * rc.sinA;
    *nx = -sign * rc.sinA; *nz = rc.cosA;
}

// The side walls' lights at wall point (z, y), footprint fz along and fy up: the body's
// cover (box-filtered: one every sconcePitch from sconceZ0, at sconceY) and, through `halo`,
// the fan of light it throws on the wall round it, smooth and so unfiltered: exp(-dz^2 /
// 2 w^2) with w = kRoomHaloW0 + kRoomHaloW1 |dy|, falling off over haloUp above the light
// and haloDown below it, dz along the wall to the nearest light. No lights (pitch 0): 0, 0.
inline float RoomSconce(const RoomConstants& rc, float z, float y, float fz, float fy, float* halo)
{
    if (halo) *halo = 0;
    if (!(rc.sconcePitch > 0) || !(rc.haloUp > 0) || !(rc.haloDown > 0)) return 0;
    const float body = RoomPulse(z - rc.sconceZ0, rc.sconcePitch, 2.0f * rc.sconceHalfW, fz) * RoomBar(y, rc.sconceY, 2.0f * rc.sconceHalfH, fy);
    const float q = (z - rc.sconceZ0) / rc.sconcePitch;
    const float dz = (q - std::floor(q + 0.5f)) * rc.sconcePitch, dy = y - rc.sconceY;
    const float w = kRoomHaloW0 + kRoomHaloW1 * std::fabs(dy);
    const float fall = dy >= 0 ? std::exp(-dy / rc.haloUp) : std::exp(dy / rc.haloDown);
    if (halo) *halo = std::exp(-0.5f * (dz / w) * (dz / w)) * fall;
    return body;
}

// The stars a unit direction n meets, linear rgb (0 where none), for a pixel `pa` radians
// across: at most one star in each cell of a kRoomStarGrid x kRoomStarGrid grid on each cube
// face (in kRoomStarDensity of them), kept 0.35 of a cell inside its cell. A star is a
// Gaussian kRoomStarCore wide, widened to 0.7 of the pixel (so that it never falls between
// pixels) but to a tenth of a cell's angle at most (so that it ends inside its cell), its
// peak lowered as it widens so that its light stays the same. Returns its brightness.
inline float RoomStars(const float n[3], float pa, float rgb[3])
{
    if (!n || !rgb) return 0;
    rgb[0] = rgb[1] = rgb[2] = 0;
    const float an[3] = { std::fabs(n[0]), std::fabs(n[1]), std::fabs(n[2]) };
    const int axis = an[0] >= an[1] && an[0] >= an[2] ? 0 : (an[1] >= an[2] ? 1 : 2);
    const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    if (!(an[axis] > 0)) return 0;
    const float sa = n[a1] / an[axis], sb = n[a2] / an[axis];
    const int face = axis * 2 + (n[axis] < 0 ? 1 : 0);
    const float N = (float)kRoomStarGrid;
    const int i = std::min(std::max((int)std::floor((sa + 1.0f) * 0.5f * N), 0), kRoomStarGrid - 1);
    const int j = std::min(std::max((int)std::floor((sb + 1.0f) * 0.5f * N), 0), kRoomStarGrid - 1);
    const int key = i + 64 * face;
    const uint32_t h1 = RoomHash(key, j);
    const float limit = kRoomStarDensity * 65536.0f;
    if (!((float)h1 < limit)) return 0;
    const float h2 = (float)RoomHash(key, j + 128) / 65535.0f, h3 = (float)RoomHash(key, j + 256) / 65535.0f;
    const float h4 = (float)RoomHash(key, j + 384) / 65535.0f;
    float v[3];
    v[axis] = n[axis] < 0 ? -1.0f : 1.0f;
    v[a1] = -1.0f + 2.0f * ((float)i + 0.35f + 0.3f * h2) / N;
    v[a2] = -1.0f + 2.0f * ((float)j + 0.35f + 0.3f * h3) / N;
    const float vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= vl; v[1] /= vl; v[2] /= vl;
    const float cx = v[1] * n[2] - v[2] * n[1], cy = v[2] * n[0] - v[0] * n[2], cz = v[0] * n[1] - v[1] * n[0];
    const float ang2 = cx * cx + cy * cy + cz * cz;
    const float sigma = std::fmin(std::fmax(0.7f * pa, kRoomStarCore), 0.1f / N);
    const float m2 = h4 * h4, m6 = m2 * m2 * m2;
    const float peak = (0.04f + 1.4f * m6) * (kRoomStarCore / sigma) * (kRoomStarCore / sigma);
    const float I = peak * std::exp(-0.5f * ang2 / (sigma * sigma));
    const float t = (float)h1 / limit;
    const float warm[3] = { 1.0f, 0.86f, 0.72f }, cool[3] = { 0.78f, 0.86f, 1.0f };
    for (int k = 0; k < 3; k++) rgb[k] = I * (warm[k] + (cool[k] - warm[k]) * t);
    return I;
}

// The low hills along the open air's horizon: their height in radians above it at azimuth
// az (0 towards the screen), up to 0.068 (4 degrees); where it dips below 0 the plain
// reaches the horizon between them.
inline float RoomHillHeight(float az)
{
    return 0.03f + 0.022f * std::sin(3.0f * az + 1.1f) + 0.012f * std::sin(7.0f * az + 0.3f) + 0.004f * std::sin(19.0f * az + 2.0f);
}

// The open air as a ray (o, d) sees it, whichever face it left the room by: there are no
// walls. Below the horizon, the field: where the ray meets the floor's plane, the lightmap's
// floor (clamped to its edge, and past the edge its screen light - what it has above the
// field's house light G - falling off over kRoomFieldFall), so the field is lit by the screen
// without a seam wherever the room's faces happen to lie (a curved front's wings cross the
// field beside the viewer); it fades into the hills' colour with distance. Above it, a night
// sky (kRoomSkyHorizon to kRoomSkyZenith, with the world colour's sky on top as beyond the
// room's glass) and its stars, which rise over kRoomStarRise; in front of them the hills,
// their edge filtered over the pixel's angle. `kind` (may be null): kRoomPartSky,
// kRoomPartHills or kRoomPartField; `star` (may be null): the stars' light added.
inline bool RoomNightSky(const RoomConstants& rc, const RoomLightmap& light, const float o[3], const float d[3], const float Dx[3], const float Dy[3],
                         float out[3], int* kind, float* star)
{
    if (!o || !d || !Dx || !Dy || !out) return false;
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 0)) return false;
    const float n[3] = { d[0] / len, d[1] / len, d[2] / len };
    float G[3], Hc[3];
    for (int ch = 0; ch < 3; ch++)
    {
        G[ch] = kRoomShadeFloor * rc.tint[kFaceFloor][ch] * (rc.world[ch] + rc.ambient[ch]);
        Hc[ch] = kRoomHills[ch] + 0.5f * G[ch];
    }
    if (star) *star = 0;
    if (n[1] < -kRoomEnvHorizonDy)
    {
        const float tg = (rc.yF - o[1]) / d[1];
        float q[3], Lf[3];
        RoomAt(o, d, tg, q);
        const float u = (q[0] + rc.X) * rc.inv2X, v = (q[2] + rc.g) * rc.invFloorZ;
        const float uc = std::fmin(std::fmax(u, 0.0f), 1.0f), vc = std::fmin(std::fmax(v, 0.0f), 1.0f);
        if (!light.Sample(kFaceFloor, uc, vc, Lf)) return false;
        const float ex = (u - uc) * 2.0f * rc.X, ez = (v - vc) * (rc.zB + rc.g);
        const float k = 1.0f + std::sqrt(ex * ex + ez * ez) / kRoomFieldFall;
        const float fall = 1.0f / (k * k);
        const float fog = 1.0f - std::exp(-tg * len / kRoomHillFog);
        for (int ch = 0; ch < 3; ch++)
        {
            const float field = G[ch] + (Lf[ch] - G[ch]) * fall;
            out[ch] = field + (Hc[ch] - field) * fog;
        }
        if (kind) *kind = kRoomPartField;
        return true;
    }
    const float el = std::asin(std::fmin(n[1], 1.0f)), az = std::atan2(n[0], -n[2]);
    const float lx = std::sqrt(Dx[0] * Dx[0] + Dx[1] * Dx[1] + Dx[2] * Dx[2]), ly = std::sqrt(Dy[0] * Dy[0] + Dy[1] * Dy[1] + Dy[2] * Dy[2]);
    const float pa = std::fmax(0.5f * (lx + ly) / len, 1e-6f);
    const float cover = std::fmin(std::fmax((RoomHillHeight(az) - el) / pa + 0.5f, 0.0f), 1.0f);
    const float sd = std::sqrt(std::fmax(n[1], 0.0f));
    const float rise = std::fmin(std::fmax(el / kRoomStarRise, 0.0f), 1.0f);
    float srgb[3];
    const float I = RoomStars(n, pa, srgb) * rise;
    for (int ch = 0; ch < 3; ch++)
    {
        const float hw = kRoomEnvHorizon * rc.world[ch];
        const float sky = kRoomSkyHorizon[ch] + (kRoomSkyZenith[ch] - kRoomSkyHorizon[ch]) * sd + hw + (kRoomEnvZenith * rc.world[ch] - hw) * sd;
        out[ch] = (1.0f - cover) * (sky + srgb[ch] * rise) + cover * Hc[ch];
    }
    if (star) *star = (1.0f - cover) * I;
    if (kind) *kind = cover > 0.5f ? kRoomPartHills : kRoomPartSky;
    return true;
}

// An environment's surface (XENV.md): L comes in as the lightmap's light at the sample (on
// `face` at chart u, v; ray o, d with differentials Dx, Dy) and goes out as what the eye sees
// there, before the glow on the front. The room is RoomSurface's. The open air is the field
// and the night sky whichever face the ray left by (RoomNightSky, which reads `light`). The
// cinema and the lounge draw their details, box-filtered over the footprint along the face's
// two axes; the room light lights the side walls' lights and the lounge's downlights
// (lightSeen, while flag 64 is on).
inline bool RoomEnvSurface(const RoomConstants& rc, int face, float u, float v, const float o[3], const float d[3], const float Dx[3], const float Dy[3],
                           float L[3], RoomSurfaceInfo* info, const RoomLightmap* light)
{
    if (!o || !d || !Dx || !Dy || !L) return false;
    if (face < 0 || face >= kRoomFaces) return false;
    const int env = (int)rc.env;
    if (env == kRoomEnvRoom || env >= kRoomEnvCount) return true;
    if (env == kRoomEnvOpenAir)
    {
        if (!light) return false;
        int kind = 0;
        float star = 0;
        if (!RoomNightSky(rc, *light, o, d, Dx, Dy, L, &kind, &star)) return false;
        if (info) { info->envPart = kind; info->star = star; }
        return true;
    }
    // The sample's point on its face and its footprint along the face's axes: fa across (s on
    // the front, z on the side walls, x on the back wall, the floor and the ceiling), fb the
    // other way (y on the walls and the front, z on the floor and the ceiling). The front's
    // point comes from its chart (u, v), its footprint from the plane tangent there.
    float p[3], n[3] = { 0, 0, 0 }, Px[3], Py[3], fa = kRoomFootMin, fb = kRoomFootMin, s = 0;
    if (face == kFaceFront)
    {
        float x, z, nx, nz;
        s = -rc.sMax + u * 2.0f * rc.sMax;
        RoomFrontAt(rc, s, &x, &z, &nx, &nz);
        p[0] = x; p[1] = rc.yC - v * (rc.yC - rc.yF); p[2] = z;
        n[0] = nx; n[2] = nz;
        const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        const float t = ((p[0] - o[0]) * d[0] + (p[1] - o[1]) * d[1] + (p[2] - o[2]) * d[2]) / dd;
        if (!(t > 0) || !RoomPlaneFootprint(t, d, n, Dx, Dy, Px, Py)) return true;
        fa = std::fmin(std::fmax(std::fabs(Px[0] * nz - Px[2] * nx) + std::fabs(Py[0] * nz - Py[2] * nx), kRoomFootMin), kRoomFootMax);
        fb = RoomFootprintAxis(Px, Py, 1);
    }
    else
    {
        float sgn;
        const int axis = RoomFaceAxis(face, &sgn);
        const float plane = face == kFaceLeft ? -rc.X : (face == kFaceRight ? rc.X : (face == kFaceFloor ? rc.yF : (face == kFaceCeiling ? rc.yC : rc.zB)));
        n[axis] = sgn;
        const float t = (plane - o[axis]) / d[axis];
        if (!(t > 0) || !(t < 3.0e38f)) return true;
        RoomAt(o, d, t, p);
        if (!RoomPlaneFootprint(t, d, n, Dx, Dy, Px, Py)) return true;
        fa = RoomFootprintAxis(Px, Py, face == kFaceLeft || face == kFaceRight ? 2 : 0);
        fb = RoomFootprintAxis(Px, Py, face == kFaceFloor || face == kFaceCeiling ? 2 : 1);
    }
    const bool lightOn = (rc.flags & kRoomFlagLight) != 0;
    const float* tint = rc.tint[face];
    int part = 0;
    if (env == kRoomEnvCinema || env == kRoomEnvTheatre)
    {
        if (face == kFaceFront)
        {
            const float mask = RoomBar(s, 0.0f, rc.screenW + 2.0f * kRoomMaskW, fa) * RoomBar(p[1], 0.0f, rc.screenH + 2.0f * kRoomMaskW, fb);
            const float folds = 1.0f + kRoomCurtainAmp * RoomFolds(s, kRoomCurtainPitch, fa);
            for (int ch = 0; ch < 3; ch++) L[ch] *= mask * kRoomMaskLevel + (1.0f - mask) * folds;
            if (mask > 0.5f) part = kRoomPartMask;
        }
        else if (face == kFaceLeft || face == kFaceRight || face == kFaceBack)
        {
            const float folds = 1.0f + kRoomFoldAmp * RoomFolds(face == kFaceBack ? p[0] : p[2], kRoomFoldPitch, fa);
            for (int ch = 0; ch < 3; ch++) L[ch] *= folds;
        }
        else if (face == kFaceFloor && env == kRoomEnvCinema)
        {
            // (The theatre's floor shows only in front of its first row; its step lights are on the stands.)
            const float stepX = rc.X - kRoomStepInset, ax = std::fabs(p[0]);
            const float z0 = p[2] - rc.zSide - 0.5f * kRoomStepPitch;
            const float cover = RoomBar(ax, stepX, kRoomStepW, fa) * RoomPulse(z0, kRoomStepPitch, kRoomStepD, fb);
            const float q = z0 / kRoomStepPitch;
            const float dz = (q - std::floor(q + 0.5f)) * kRoomStepPitch, dx = ax - stepX;
            const float pool = kRoomStepPoolGain * std::exp(-0.5f * (dx * dx + dz * dz) / (kRoomStepPool * kRoomStepPool));
            for (int ch = 0; ch < 3; ch++) L[ch] = (1.0f - cover) * (L[ch] + pool * kRoomStepL[ch] * tint[ch]) + cover * kRoomStepL[ch];
            if (cover > 0.5f) part = kRoomPartStep;
        }
        else if (face == kFaceFloor && env == kRoomEnvTheatre)
        {
            // The theatre's carpet: its colour over the floor's tint (the lightmap's albedo).
            float c[3];
            RoomCarpet(p[0], p[2], fa, fb, c);
            for (int ch = 0; ch < 3; ch++) L[ch] *= tint[ch] > 0 ? c[ch] / tint[ch] : 1.0f;
        }
    }
    else if (env == kRoomEnvLounge)
    {
        if (face == kFaceFront)
        {
            const float slats = 1.0f + kRoomSlatAmp * RoomFolds(s, kRoomSlatPitch, fa);
            for (int ch = 0; ch < 3; ch++) L[ch] *= slats;
        }
        else if (face == kFaceLeft || face == kFaceRight || face == kFaceBack)
        {
            const float across = face == kFaceBack ? p[0] : p[2];
            const float panel = RoomBar(p[1], rc.yF + 0.5f * kRoomDadoY, kRoomDadoY, fb);
            const float groove = RoomPulse(across, kRoomGroovePitch, kRoomGrooveW, fa) * panel;
            const float dado = RoomBar(p[1], rc.yF + kRoomDadoY, kRoomDadoW, fb);
            const float skirt = RoomBar(p[1], rc.yF + 0.5f * kRoomSkirtH, kRoomSkirtH, fb);
            const float wall = (1.0f - kRoomGroove * groove) * (1.0f - dado) + kRoomDado * dado;
            for (int ch = 0; ch < 3; ch++) L[ch] *= wall * (1.0f - skirt) + skirt * (tint[ch] > 0 ? kRoomSkirt[ch] / tint[ch] : 1.0f);
        }
        else if (face == kFaceFloor)
        {
            const float bi = std::floor(p[0] / kRoomBoardW);
            const float off = (float)RoomHash((int)bi, 911) / 65535.0f * kRoomBoardL;
            const float zz = p[2] - off;
            const float bj = std::floor(zz / kRoomBoardL);
            const float fade = std::fmin(std::fmax(1.0f - 2.0f * fa / kRoomBoardW, 0.0f), 1.0f);
            const float seam = std::fmax(RoomPulse(p[0], kRoomBoardW, kRoomSeamW, fa), RoomPulse(zz, kRoomBoardL, kRoomSeamW, fb) * fade);
            const float tone = 1.0f + kRoomBoardVar * ((float)RoomHash((int)bi, (int)bj) / 65535.0f - 0.5f) * fade;
            const float grain = 1.0f + kRoomGrainAmp * RoomFolds(p[0], kRoomGrainPitch, fa);
            const float wood = (1.0f - kRoomSeam * seam) * tone * grain;
            float cover = 0, inner = 0;
            if (rc.rugX1 > rc.rugX0 && rc.rugZ1 > rc.rugZ0)
            {
                const float cx = 0.5f * (rc.rugX0 + rc.rugX1), cz = 0.5f * (rc.rugZ0 + rc.rugZ1), w = rc.rugX1 - rc.rugX0, dl = rc.rugZ1 - rc.rugZ0;
                cover = RoomBar(p[0], cx, w, fa) * RoomBar(p[2], cz, dl, fb);
                inner = RoomBar(p[0], cx, w - 2.0f * kRoomRugBorder, fa) * RoomBar(p[2], cz, dl - 2.0f * kRoomRugBorder, fb);
            }
            const float edge = std::fmax(cover - inner, 0.0f);
            for (int ch = 0; ch < 3; ch++)
            {
                const float rug = tint[ch] > 0 ? (inner * kRoomRugField[ch] + edge * kRoomRugEdge[ch]) / tint[ch] : 0.0f;
                L[ch] *= (1.0f - cover) * wood + rug;
            }
            if (cover > 0.5f) part = kRoomPartRug;
            else if (seam > 0.1f) part = kRoomPartSeam;
        }
        else if (face == kFaceCeiling && rc.lightX1 > rc.lightX0 && rc.lightZ1 > rc.lightZ0)
        {
            // Six downlights in the room light's panel, three across and two along.
            const float cw = (rc.lightX1 - rc.lightX0) / 3.0f, cd = (rc.lightZ1 - rc.lightZ0) / 2.0f;
            const float qx = (p[0] - rc.lightX0) / cw, qz = (p[2] - rc.lightZ0) / cd;
            const float dx = (qx - std::floor(qx) - 0.5f) * cw, dz = (qz - std::floor(qz) - 0.5f) * cd;
            const float within = RoomPanelCoverage(rc, p[0], p[2], fa, fb);
            const float disc = std::fmin(std::fmax((kRoomDownR - std::sqrt(dx * dx + dz * dz)) / (0.5f * (fa + fb)) + 0.5f, 0.0f), 1.0f) * within;
            for (int ch = 0; ch < 3; ch++) L[ch] = lightOn ? L[ch] + disc * rc.lightSeen[ch] : L[ch] * (1.0f + disc * (kRoomDownTrim - 1.0f));
            if (disc > 0.5f) part = kRoomPartDownlight;
        }
    }
    // The side walls' lights (the cinema's sconces, the lounge's lamps): lit by the room
    // light, each with its fan of light on the wall in the wall's own colour; unlit, a dark
    // fitting.
    if (face == kFaceLeft || face == kFaceRight)
    {
        float halo = 0;
        const float body = RoomSconce(rc, p[2], p[1], fa, fb, &halo);
        for (int ch = 0; ch < 3; ch++)
        {
            const float lit = lightOn ? L[ch] + rc.haloGain * tint[ch] * halo * rc.lightSeen[ch] : L[ch];
            L[ch] = (1.0f - body) * lit + body * (lightOn ? rc.lightSeen[ch] : kRoomSconceOff * L[ch]);
        }
        if (body > 0.5f) part = kRoomPartSconce;
        else if (lightOn && rc.haloGain > 0 && halo > 0.3f) part = kRoomPartHalo;
    }
    if (info) info->envPart = part;
    return true;
}

// ------------------------------------------------------------------ the theatre's stands
// Sample kinds after the room's (0-8): kRoomKindStands + part x 6 + the hit face's normal,
// numbered +x, -x, +y, -y, +z, -z (0-5); the part is the tread, the riser, a seat's cushion,
// its back, an armrest, or the round top of the back or an armrest (normal 2: the shading
// works out the curve's own). The sample's u carries the ray's t at the hit.
static const int kRoomKindStands = 9;
static const int kRoomStandTread = 0, kRoomStandRiser = 1, kRoomStandCushion = 2, kRoomStandBack = 3, kRoomStandArm = 4;
static const int kRoomStandBackTop = 5, kRoomStandArmTop = 6, kRoomStandParts = 7;
static const int kRoomKinds = kRoomKindStands + kRoomStandParts * 6;

// The stands' shape, from the room (the CPU's classification) or the constants (its shading
// and the GPU's).
struct RoomStands
{
    int rows = 0, viewerRow = 0, jMin = 0, cols = 0;
    float z0 = 0, pitch = 0, t0 = 0, riser = 0, x0 = 0, block = 0, zEnd = 0, X = 0, floor = 0;
};

inline RoomStands MakeRoomStands(const Room& r)
{
    RoomStands s;
    if (!r.valid || r.env != kRoomEnvTheatre || r.standsRows <= 0) return s;
    s.rows = r.standsRows; s.viewerRow = r.viewerRow;
    s.z0 = r.standsZ0; s.pitch = r.standsPitch; s.t0 = r.standsT0; s.riser = r.standsRiser;
    s.x0 = r.seatX0; s.block = r.seatBlock; s.zEnd = r.zB;
    s.jMin = r.seatJMin; s.cols = r.seatCols; s.X = r.X; s.floor = r.yF - kRoomBlockBelow;
    return s;
}

inline RoomStands MakeRoomStands(const RoomConstants& rc)
{
    RoomStands s;
    s.rows = (int)rc.standsRows; s.viewerRow = (int)rc.viewerRow;
    s.z0 = rc.standsZ0; s.pitch = rc.standsPitch; s.t0 = rc.standsT0; s.riser = rc.standsRiser;
    s.x0 = rc.seatX0; s.block = rc.seatBlock; s.zEnd = rc.standsZEnd;
    s.jMin = rc.seatJMin; s.cols = (int)rc.seatCols; s.X = rc.X; s.floor = rc.yF - kRoomBlockBelow;
    return s;
}

// The slab test for a ray (o, d; rcp = 1 / d, the classification's) from outside a box
// [lo, hi]: where it enters (t in (0, tMax)) and the face it enters by (normal numbered as
// the kinds'). Multiplies by rcp, as the GPU does, so that neither divides per box.
inline bool RoomBoxEnter(const float o[3], const float d[3], const float rcp[3], const float lo[3], const float hi[3], float tMax, float* t, int* normal)
{
    if (!o || !d || !rcp || !lo || !hi || !t || !normal) return false;
    float tn = -3.0e38f, tf = 3.0e38f;
    int axisIn = -1;
    for (int k = 0; k < 3; k++)
    {
        if (d[k] == 0)
        {
            if (o[k] < lo[k] || o[k] > hi[k]) return false;
            continue;
        }
        const float a = (lo[k] - o[k]) * rcp[k], b = (hi[k] - o[k]) * rcp[k];
        const float ta = std::fmin(a, b), tb = std::fmax(a, b);
        if (ta > tn) { tn = ta; axisIn = k; }
        tf = std::fmin(tf, tb);
    }
    if (axisIn < 0 || !(tn <= tf) || !(tn > 0) || !(tn < tMax)) return false;
    *t = tn;
    *normal = axisIn * 2 + (d[axisIn] > 0 ? 1 : 0);
    return true;
}

// The first thing of the stands a ray (o, d, rcp = 1 / d) meets before tMax. The stands are
// boxes: each row a solid block from below the floor to its tread, across the room (its top is
// the tread, its front the riser; a block's other faces lie inside its neighbours or the
// walls), and each seat a cushion, a back and an armrest at each edge (the viewer's own
// too), tested only when the ray meets the seat's bounding box or starts inside it (a head
// leaning into its own seat). Everything is tested in
// one fixed order - the rows from the front, then the seats row by row and along each row -
// and a box replaces the nearest so far only when it is strictly nearer, so the GPU, which
// tests only what its tile of pixels can see (StandsCullH), finds the same box.
struct RoomStandsHit { float t = 0; int part = -1, normal = -1, cell = -1, object = -1, box = 0; };

// Box b of a seat (0 the cushion, 1 the back, 2 and 3 the left and right armrests) whose
// middle is xc, on row tread yk and line zc: its straight part - for the back and the
// armrests the part below their round top (RoomSeatCapOf).
inline void RoomSeatPart(float xc, float yk, float zc, int b, float lo[3], float hi[3])
{
    const float* bx = b == 0 ? kRoomCushion : (b == 1 ? kRoomBack : kRoomArm);
    const float cx = b < 2 ? xc : xc + (b == 2 ? -0.5f : 0.5f) * kRoomSeatPitch;
    const float top = b == 0 ? 0.0f : (b == 1 ? kRoomBackR : kRoomArmR);
    lo[0] = cx - bx[0]; lo[1] = yk + bx[1]; lo[2] = zc + bx[3];
    hi[0] = cx + bx[0]; hi[1] = yk + bx[2] - top; hi[2] = zc + bx[4];
}

// The round top of box b (1 the back, 2 and 3 the armrests): a half cylinder along `axis`
// (0, x, for the back; 2, z, for an armrest) from a0 to a1, its axis at height cy and at cw
// along `across` (z for the back, x for an armrest), radius r - on its straight part's top.
struct RoomSeatCap { int axis = 0, across = 2; float cy = 0, cw = 0, r = 0, a0 = 0, a1 = 0; };

inline bool RoomSeatCapFrom(int b, const float lo[3], const float hi[3], RoomSeatCap& c)
{
    if (b < 1 || b > 3 || !lo || !hi) return false;
    c.axis = b == 1 ? 0 : 2;
    c.across = b == 1 ? 2 : 0;
    c.r = b == 1 ? kRoomBackR : kRoomArmR;
    c.cy = hi[1];
    c.cw = 0.5f * (lo[c.across] + hi[c.across]);
    c.a0 = lo[c.axis];
    c.a1 = hi[c.axis];
    return true;
}

inline bool RoomSeatCapOf(float xc, float yk, float zc, int b, RoomSeatCap& c)
{
    if (b < 1 || b > 3) return false;
    float lo[3], hi[3];
    RoomSeatPart(xc, yk, zc, b, lo, hi);
    return RoomSeatCapFrom(b, lo, hi, c);
}

// Where a ray (rcp = 1 / d) from outside enters a round top, before tMax: on the curve
// (*curved, normal 2) or a flat end (normal as the kinds'). The top is where the cylinder,
// the half-space above its axis and the slab between its ends overlap: the latest of their
// entries, if before the earliest exit. An entry by the flat underside is left to the
// straight part below, which comes first.
inline bool RoomSeatCapEnter(const RoomSeatCap& c, const float o[3], const float d[3], const float rcp[3], float tMax, float* t, int* normal, bool* curved)
{
    if (!o || !d || !rcp || !t || !normal || !curved) return false;
    const float oy = o[1] - c.cy, ow = o[c.across] - c.cw, dy = d[1], dw = d[c.across];
    const float A = dy * dy + dw * dw, B = oy * dy + ow * dw, C = oy * oy + ow * ow - c.r * c.r;
    float tn = -3.0e38f, tf = 3.0e38f;
    int by = 0;                                     // what the entry is: 0 the curve, 1 an end, 2 the underside
    if (A == 0)
    {
        if (C > 0) return false;
    }
    else
    {
        const float disc = B * B - A * C;
        if (!(disc >= 0)) return false;
        const float sq = std::sqrt(disc);
        tn = (-B - sq) / A;
        tf = (-B + sq) / A;
    }
    if (dy == 0)
    {
        if (oy < 0) return false;
    }
    else
    {
        const float ty = (c.cy - o[1]) * rcp[1];
        if (dy > 0)
        {
            if (ty > tn) { tn = ty; by = 2; }
        }
        else tf = std::fmin(tf, ty);
    }
    if (d[c.axis] == 0)
    {
        if (o[c.axis] < c.a0 || o[c.axis] > c.a1) return false;
    }
    else
    {
        const float ea = (c.a0 - o[c.axis]) * rcp[c.axis], eb = (c.a1 - o[c.axis]) * rcp[c.axis];
        const float ta = std::fmin(ea, eb), tb = std::fmax(ea, eb);
        if (ta > tn) { tn = ta; by = 1; }
        tf = std::fmin(tf, tb);
    }
    if (by == 2 || !(tn <= tf) || !(tn > 0) || !(tn < tMax)) return false;
    *t = tn;
    *curved = by == 0;
    *normal = by == 0 ? 2 : c.axis * 2 + (d[c.axis] > 0 ? 1 : 0);
    return true;
}

// Box b of a seat - its straight part and the back's or an armrest's round top - where the
// ray enters it first, before `best`: the part (kRoomStand*) and the normal. One test of the
// whole box, round top and all, first: a ray that misses it misses both; one that enters it
// at or below the round top's axis (not by its top face) enters the straight part there. One
// that enters higher meets the round top or nothing: the straight part's top lies wholly
// under the round top, so the ray would have to pass through the top to reach it.
inline bool RoomSeatPartEnter(float xc, float yk, float zc, int b, const float o[3], const float d[3], const float rcp[3], float best,
                              float* t, int* part, int* normal)
{
    if (!o || !d || !rcp || !t || !part || !normal) return false;
    float lo[3], hi[3], tb;
    int nb;
    const int flat = b == 0 ? kRoomStandCushion : (b == 1 ? kRoomStandBack : kRoomStandArm);
    RoomSeatPart(xc, yk, zc, b, lo, hi);
    RoomSeatCap c;
    const bool capped = RoomSeatCapFrom(b, lo, hi, c);
    const float top = hi[1];
    if (capped) hi[1] = c.cy + c.r;
    if (!RoomBoxEnter(o, d, rcp, lo, hi, best, &tb, &nb)) return false;
    if (!capped || (nb != 2 && o[1] + tb * d[1] <= top))
    {
        *t = tb; *part = flat; *normal = nb;
        return true;
    }
    bool curved = false;
    if (!RoomSeatCapEnter(c, o, d, rcp, best, &tb, &nb, &curved)) return false;
    *t = tb; *part = curved ? (b == 1 ? kRoomStandBackTop : kRoomStandArmTop) : flat; *normal = nb;
    return true;
}

// One seat's boxes (cushion, back, left and right armrests) for row k's tread yk and line zc
// and the seat's middle xc, nearer than `best`.
inline void RoomSeatBoxes(const float o[3], const float d[3], const float rcp[3], float xc, float yk, float zc, int k, int object, float& best, RoomStandsHit& hit)
{
    const float reach = 0.5f * kRoomSeatPitch + kRoomArm[0];
    const float blo[3] = { xc - reach, yk, zc + kRoomCushion[3] }, bhi[3] = { xc + reach, yk + kRoomChairTop, zc + kRoomBack[4] };
    float tb;
    int nb;
    const bool inside = o[0] >= blo[0] && o[0] <= bhi[0] && o[1] >= blo[1] && o[1] <= bhi[1] && o[2] >= blo[2] && o[2] <= bhi[2];
    if (!inside && !RoomBoxEnter(o, d, rcp, blo, bhi, best, &tb, &nb)) return;
    for (int b = 0; b < 4; b++)
    {
        int part = -1;
        if (RoomSeatPartEnter(xc, yk, zc, b, o, d, rcp, best, &tb, &part, &nb))
        {
            best = tb;
            hit.t = tb; hit.normal = nb; hit.cell = k; hit.object = object; hit.box = b; hit.part = part;
        }
    }
}

// Row k's block: x across the room, z its cell (the last row's to the back wall), y one riser
// deep under its tread - the first row's from below the floor. A ray from the eye cannot
// reach a row lower than that without first entering the row in front, so the thin blocks
// draw what solid ones would, and each covers a thin band of the view (the GPU's tiles see
// few of them).
inline void RoomRowBlock(const RoomStands& s, int k, float lo[3], float hi[3])
{
    const float zl = s.z0 + ((float)k - 0.5f) * s.pitch;
    lo[0] = -s.X; lo[1] = k == 0 ? s.floor : s.t0 + (float)(k - 1) * s.riser; lo[2] = zl;
    hi[0] = s.X; hi[1] = s.t0 + (float)k * s.riser; hi[2] = k == s.rows - 1 ? s.zEnd : zl + s.pitch;
}

inline bool RoomStandsTrace(const RoomStands& s, const float o[3], const float d[3], const float rcp[3], float tMax, RoomStandsHit& hit)
{
    hit = RoomStandsHit();
    if (!o || !d || !rcp || s.rows <= 0 || !(s.pitch > 0) || s.cols <= 0) return false;
    float best = tMax;
    for (int k = 0; k < s.rows; k++)
    {
        float lo[3], hi[3], t;
        int n;
        RoomRowBlock(s, k, lo, hi);
        if (RoomBoxEnter(o, d, rcp, lo, hi, best, &t, &n))
        {
            best = t;
            hit.t = t; hit.normal = n; hit.cell = k; hit.object = k; hit.box = 0;
            hit.part = n == 2 ? kRoomStandTread : kRoomStandRiser;
        }
    }
    for (int k = 0; k < s.rows; k++)
    {
        const float yk = s.t0 + (float)k * s.riser, zc = s.z0 + (float)k * s.pitch;
        for (int c = 0; c < s.cols; c++)
        {
            const int j = s.jMin + c;
            RoomSeatBoxes(o, d, rcp, s.x0 + (float)j * kRoomSeatPitch, yk, zc, k, s.rows + k * s.cols + c, best, hit);
        }
    }
    return hit.part >= 0;
}

// ------------------------------------------------------- the stands, rasterized
// The GPU draws the stands' boxes as triangles into an id buffer per eye (one value a pixel,
// at its centre: RecordStandsRaster), and the eye pass reads each pixel's 3 x 3 neighbourhood
// of ids (RoomStandsSamples). A code is the object (a row's block, 0 .. rows - 1, or a seat,
// rows + k cols + c), which of its boxes (a seat's cushion, back, left or right armrest; a
// block's 0) and the face's kind (part x 6 + normal, as the sample kinds): bits 8 up, 6-7
// and 0-5. 0 is nothing.
static const int kRoomCodeBoxShift = 6, kRoomCodeObjectShift = 8;
static const uint32_t kRoomCodeKindMask = 63u;
inline uint32_t RoomStandsCode(int object, int box, int part, int normal)
{
    return ((uint32_t)(object + 1) << kRoomCodeObjectShift) | ((uint32_t)box << kRoomCodeBoxShift) | (uint32_t)(part * 6 + normal);
}

// Seat object `object`'s middle, tread and line; false for a row's block.
inline bool RoomSeatOf(const RoomStands& s, int object, float* xc, float* yk, float* zc)
{
    if (!xc || !yk || !zc || object < s.rows || s.cols <= 0) return false;
    const int seat = object - s.rows;
    if (seat >= s.rows * s.cols) return false;
    const int k = seat / s.cols, j = s.jMin + seat % s.cols;
    *xc = s.x0 + (float)j * kRoomSeatPitch; *yk = s.t0 + (float)k * s.riser; *zc = s.z0 + (float)k * s.pitch;
    return true;
}

// The box `box` of `object` (RoomRowBlock for a row; the straight part of the seat's cushion,
// back or armrests, RoomSeatPart).
inline bool RoomStandsBox(const RoomStands& s, int object, int box, float lo[3], float hi[3])
{
    if (object < 0 || box < 0 || box > 3) return false;
    if (object < s.rows) { RoomRowBlock(s, object, lo, hi); return box == 0; }
    float xc, yk, zc;
    if (!RoomSeatOf(s, object, &xc, &yk, &zc)) return false;
    RoomSeatPart(xc, yk, zc, box, lo, hi);
    return true;
}

// The stands as triangles for the id buffer: every row's block and every seat's four boxes
// (the viewer's own too), each face but the bottom (never seen from above), two
// triangles a face, every vertex carrying its face's code. The back's and the armrests'
// round tops are strips inside the curve (the code of the top) and half-disc fans at their
// ends (the code of the end, a face of the back or armrest); the top of the straight part
// under them is left out. An armrest two seats share is drawn once, with the seat on its
// right (the exact tests still try both: they are the same box).
struct RoomStandsVertex { float p[3]; uint32_t code; };

inline bool BuildRoomStandsMesh(const RoomStands& s, std::vector<RoomStandsVertex>& out)
{
    out.clear();
    if (s.rows <= 0 || s.cols < 0) return false;
    auto push = [&](const float q[3], uint32_t code)
    {
        RoomStandsVertex vtx;
        vtx.p[0] = q[0]; vtx.p[1] = q[1]; vtx.p[2] = q[2];
        vtx.code = code;
        out.push_back(vtx);
    };
    auto addBox = [&](int object, int box, int part, const float lo[3], const float hi[3], bool block)
    {
        // Faces: +x (normal 0), -x (1), +y (2), +z (4), -z (5); a ray enters a face from outside.
        for (int f = 0; f < 5; f++)
        {
            const int normal = f < 3 ? f : f + 1;
            if (normal == 2 && !block && box > 0) continue;     // under the round top
            const int axis = normal / 2;
            const float at = normal % 2 == 0 ? hi[axis] : lo[axis];
            const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
            float q[4][3];
            const float u[4] = { lo[a1], hi[a1], hi[a1], lo[a1] }, v[4] = { lo[a2], lo[a2], hi[a2], hi[a2] };
            for (int c = 0; c < 4; c++) { q[c][axis] = at; q[c][a1] = u[c]; q[c][a2] = v[c]; }
            const int facePart = block ? (normal == 2 ? kRoomStandTread : kRoomStandRiser) : part;
            const uint32_t code = RoomStandsCode(object, box, facePart, normal);
            const int tri[6] = { 0, 1, 2, 0, 2, 3 };
            for (int i = 0; i < 6; i++) push(q[tri[i]], code);
        }
    };
    auto addCap = [&](int object, int box, const RoomSeatCap& c)
    {
        const int strips = box == 1 ? kRoomBackStrips : kRoomArmStrips;
        const int flat = box == 1 ? kRoomStandBack : kRoomStandArm, top = box == 1 ? kRoomStandBackTop : kRoomStandArmTop;
        const uint32_t curve = RoomStandsCode(object, box, top, 2);
        const uint32_t end0 = RoomStandsCode(object, box, flat, c.axis * 2 + 1), end1 = RoomStandsCode(object, box, flat, c.axis * 2);
        auto at = [&](int i, float a, float q[3])
        {
            // Point i of the half circle (0 at +across, strips at -across), at a along the axis.
            const float th = (float)i * (kRoomPi / (float)strips);
            const float cw = i == 0 ? 1.0f : (i == strips ? -1.0f : std::cos(th)), sw = i == 0 || i == strips ? 0.0f : std::sin(th);
            q[c.axis] = a; q[c.across] = c.cw + c.r * cw; q[1] = c.cy + c.r * sw;
        };
        for (int i = 0; i < strips; i++)
        {
            float q0[3], q1[3], q2[3], q3[3];
            at(i, c.a0, q0); at(i + 1, c.a0, q1); at(i + 1, c.a1, q2); at(i, c.a1, q3);
            push(q0, curve); push(q1, curve); push(q2, curve);
            push(q0, curve); push(q2, curve); push(q3, curve);
            float m0[3], m1[3];
            m0[c.axis] = c.a0; m0[c.across] = c.cw; m0[1] = c.cy;
            m1[c.axis] = c.a1; m1[c.across] = c.cw; m1[1] = c.cy;
            push(m0, end0); push(q0, end0); push(q1, end0);
            push(m1, end1); push(q3, end1); push(q2, end1);
        }
    };
    float lo[3], hi[3];
    for (int k = 0; k < s.rows; k++)
    {
        RoomRowBlock(s, k, lo, hi);
        addBox(k, 0, kRoomStandRiser, lo, hi, true);
    }
    const int parts[4] = { kRoomStandCushion, kRoomStandBack, kRoomStandArm, kRoomStandArm };
    for (int seat = 0; seat < s.rows * s.cols; seat++)
    {
        const int j = s.jMin + seat % s.cols;
        float xc, yk, zc;
        if (!RoomSeatOf(s, s.rows + seat, &xc, &yk, &zc)) continue;
        // The left armrest is the right one of the seat to the left, when that seat is there.
        const bool leftShared = j > s.jMin;
        for (int b = 0; b < 4; b++)
        {
            if (b == 2 && leftShared) continue;
            RoomSeatPart(xc, yk, zc, b, lo, hi);
            addBox(s.rows + seat, b, parts[b], lo, hi, false);
            RoomSeatCap c;
            if (RoomSeatCapOf(xc, yk, zc, b, c)) addCap(s.rows + seat, b, c);
        }
    }
    return true;
}

// Object `object`'s first hit by the ray, nearer than `best` (its boxes in their order).
inline void RoomStandsObjectHit(const RoomStands& s, int object, const float o[3], const float d[3], const float rcp[3], float& best, RoomStandsHit& hit)
{
    float lo[3], hi[3], t;
    int n;
    if (object < s.rows)
    {
        RoomRowBlock(s, object, lo, hi);
        if (RoomBoxEnter(o, d, rcp, lo, hi, best, &t, &n))
        {
            best = t;
            hit.t = t; hit.normal = n; hit.cell = object; hit.object = object;
            hit.part = n == 2 ? kRoomStandTread : kRoomStandRiser;
        }
        return;
    }
    const int seat = object - s.rows;
    if (s.cols <= 0) return;
    const int k = seat / s.cols, j = s.jMin + seat % s.cols;
    RoomSeatBoxes(o, d, rcp, s.x0 + (float)j * kRoomSeatPitch, s.t0 + (float)k * s.riser, s.z0 + (float)k * s.pitch, k, object, best, hit);
}

// The four rays of a pixel through the stands, from the 3 x 3 neighbourhood's codes (codes[4]
// the pixel's own). All nine the same: every ray meets that face (or nothing), at its plane.
// Otherwise each ray tests the objects the neighbourhood saw, in increasing order, exactly.
// A hit counts only inside the room (a side wall or a curved front's wings can come first:
// `X`, `front`). kind[s] 0: no stands for ray s; else the sample kind, and t[s] its t.
template <typename FrontDepthFn>
inline void RoomStandsSamples(const RoomStands& s, float X, FrontDepthFn front, const uint32_t codes[9], const float o[3], const float d[4][3],
                              float t[4], int kind[4])
{
    for (int r = 0; r < 4; r++) { t[r] = 0; kind[r] = 0; }
    bool same = true;
    for (int i = 1; i < 9; i++) same = same && codes[i] == codes[0];
    for (int r = 0; r < 4; r++)
    {
        float rcp[3];
        for (int k = 0; k < 3; k++) rcp[k] = 1.0f / d[r][k];
        float th = 0;
        int kh = 0;
        if (same)
        {
            if (codes[0] == 0) continue;
            const int kidx = (int)(codes[0] & kRoomCodeKindMask), box = (int)((codes[0] >> kRoomCodeBoxShift) & 3u);
            const int object = (int)(codes[0] >> kRoomCodeObjectShift) - 1;
            const int part = kidx / 6, normal = kidx % 6, axis = normal / 2;
            if (part == kRoomStandBackTop || part == kRoomStandArmTop)
            {
                // A round top: the curve itself (or, grazing, its end).
                float xc, yk, zc;
                RoomSeatCap c;
                int n;
                bool curved;
                if (!RoomSeatOf(s, object, &xc, &yk, &zc) || !RoomSeatCapOf(xc, yk, zc, box, c) ||
                    !RoomSeatCapEnter(c, o, d[r], rcp, 3.0e38f, &th, &n, &curved)) continue;
                kh = kRoomKindStands + (curved ? part : (box == 1 ? kRoomStandBack : kRoomStandArm)) * 6 + n;
            }
            else
            {
                float lo[3], hi[3];
                if (!RoomStandsBox(s, object, box, lo, hi)) continue;
                th = ((normal % 2 == 0 ? hi[axis] : lo[axis]) - o[axis]) * rcp[axis];
                if (!(th > 0) || !(th < 3.0e38f)) continue;
                kh = kRoomKindStands + kidx;
            }
        }
        else
        {
            int objects[9], count = 0;
            for (int i = 0; i < 9; i++)
            {
                if (codes[i] == 0) continue;
                const int object = (int)(codes[i] >> kRoomCodeObjectShift) - 1;
                bool seen = false;
                for (int m = 0; m < count; m++) seen = seen || objects[m] == object;
                if (!seen) objects[count++] = object;
            }
            std::sort(objects, objects + count);
            float best = 3.0e38f;
            RoomStandsHit hit;
            for (int m = 0; m < count; m++) RoomStandsObjectHit(s, objects[m], o, d[r], rcp, best, hit);
            if (hit.part < 0) continue;
            th = hit.t;
            kh = kRoomKindStands + hit.part * 6 + hit.normal;
        }
        const float px = o[0] + th * d[r][0], pz = o[2] + th * d[r][2];
        if (std::fabs(px) > X || pz < front(px)) continue;
        t[r] = th;
        kind[r] = kh;
    }
}

// G with the disk formula alone (RoomFormFactor's far branch): the stands' light from the
// coarse patches and the room light is smooth enough without the exact one close up.
inline float RoomDiskFactor(const float p[3], const float n[3], const RoomEmitter& e)
{
    if (e.n[3] == 0) return 0;
    const float cx = 0.5f * (e.c[0][0] + e.c[2][0]), cy = 0.5f * (e.c[0][1] + e.c[2][1]), cz = 0.5f * (e.c[0][2] + e.c[2][2]);
    const float v[3] = { cx - p[0], cy - p[1], cz - p[2] };
    const float np = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
    const float nq = -(e.n[0] * v[0] + e.n[1] * v[1] + e.n[2] * v[2]);
    if (!(np > 0) || !(nq > 0)) return 0;
    const float r2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    const float area = e.c[0][3];
    return area * (np * nq / r2) / (r2 + area / kRoomPi);
}

// A 32-bit hash of three whole numbers, and as 0..1 (the seats' look; the GPU's SeatHashH
// does the same in uint).
inline uint32_t RoomSeatHash(int a, int b, int c)
{
    uint32_t h = (uint32_t)a * 0x8da6b343u ^ (uint32_t)b * 0xd8163841u ^ (uint32_t)c * 0xcb1ab31fu;
    h ^= h >> 15; h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h;
}

inline float RoomSeatHash01(int a, int b, int c)
{
    return (float)(RoomSeatHash(a, b, c) >> 8) * (1.0f / 16777216.0f);
}

// Smooth value noise, -0.5..0.5: a value at each whole-number lattice point (`seed` picks the
// set), blended between them.
inline float RoomSeatNoise(float u, float v, int seed)
{
    const float fu = std::floor(u), fv = std::floor(v);
    const int iu = (int)fu, iv = (int)fv;
    float a = u - fu, b = v - fv;
    a = a * a * (3.0f - 2.0f * a);
    b = b * b * (3.0f - 2.0f * b);
    const float h00 = RoomSeatHash01(iu, iv, seed), h10 = RoomSeatHash01(iu + 1, iv, seed);
    const float h01 = RoomSeatHash01(iu, iv + 1, seed), h11 = RoomSeatHash01(iu + 1, iv + 1, seed);
    const float x0 = h00 + (h10 - h00) * a, x1 = h01 + (h11 - h01) * a;
    return x0 + (x1 - x0) * b - 0.5f;
}

// A seat's look at p, where the ray (direction d, at t; differentials Dx, Dy) met `part`
// (a seat's: kRoomStandCushion and on) on the face with normal index `normal`: the shading
// normal ns (a round top's curve; the other edges turning over kRoomCushionBevel /
// kRoomBackBevel / kRoomArmBevel; the back's channels bulging), the albedo's multiplier
// *tone (the creases, the channels' seams and the piping under the back's top, the pile, the
// seat's own tone; an armrest's polished top) and the velvet's *sheen. The seat is found
// from p: its row from z, its middle (an armrest's: the seat to its left) from x.
inline bool RoomSeatShade(const RoomConstants& rc, int part, int normal, const float p[3], const float d[3], float t,
                          const float Dx[3], const float Dy[3], float ns[3], float* tone, float* sheen)
{
    if (!p || !d || !Dx || !Dy || !ns || !tone || !sheen) return false;
    if (part < kRoomStandCushion || part >= kRoomStandParts || normal < 0 || normal > 5 || rc.standsRows == 0 || !(rc.standsPitch > 0)) return false;
    const bool arm = part == kRoomStandArm || part == kRoomStandArmTop;
    const bool top = part == kRoomStandBackTop || part == kRoomStandArmTop;
    const int b = part == kRoomStandCushion ? 0 : (arm ? 3 : 1);
    const int k = std::min(std::max((int)std::floor((p[2] - (rc.standsZ0 - 0.5f * rc.standsPitch)) / rc.standsPitch), 0), (int)rc.standsRows - 1);
    const float yk = rc.standsT0 + (float)k * rc.standsRiser, zc = rc.standsZ0 + (float)k * rc.standsPitch;
    const float q = (p[0] - rc.seatX0) / kRoomSeatPitch;
    const float jn = arm ? std::floor(q) : std::floor(q + 0.5f);
    const float xc = rc.seatX0 + jn * kRoomSeatPitch;
    float lo[3], hi[3];
    RoomSeatPart(xc, yk, zc, b, lo, hi);
    RoomSeatCap c;
    const bool capped = RoomSeatCapOf(xc, yk, zc, b, c);
    const float bevel = b == 0 ? kRoomCushionBevel : (b == 1 ? kRoomBackBevel : kRoomArmBevel);
    // The surface's own normal: the face's, or the round top's from its axis.
    float n0[3] = { 0, 0, 0 };
    const int A = top ? 1 : normal / 2;
    if (top)
    {
        const float vy = std::fmax(p[1] - c.cy, 0.0f), vw = p[c.across] - c.cw;
        const float len = std::sqrt(vy * vy + vw * vw);
        if (len > 0) { n0[1] = vy / len; n0[c.across] = vw / len; }
        else n0[1] = 1.0f;
    }
    else n0[A] = normal % 2 == 0 ? 1.0f : -1.0f;
    // Rounded edges: the normal tilts towards each edge within `bevel` of it (45 degrees at it).
    auto edge = [&](int u)
    {
        return std::fmin(std::fmax((p[u] - (hi[u] - bevel)) / bevel, 0.0f), 1.0f) - std::fmin(std::fmax(((lo[u] + bevel) - p[u]) / bevel, 0.0f), 1.0f);
    };
    float tilt[3] = { 0, 0, 0 };
    if (top) tilt[c.axis] = edge(c.axis);
    else
    {
        for (int u = 0; u < 3; u++)
        {
            if (u == A) continue;
            if (u == 1 && b > 0) continue;              // the round top above; the floor or the cushion below
            if (capped && u == c.across && p[1] > c.cy)
            {
                // An end's half disc: its curved edge.
                const float vy = p[1] - c.cy, vw = p[c.across] - c.cw, len = std::sqrt(vy * vy + vw * vw);
                const float e = std::fmin(std::fmax((len - (c.r - bevel)) / bevel, 0.0f), 1.0f);
                if (len > 0) { tilt[1] += e * vy / len; tilt[c.across] += e * vw / len; }
                continue;
            }
            tilt[u] += edge(u);
        }
    }
    // The footprint along the face's own axes (a round top's seen from above).
    const int ua = A == 0 ? 2 : 0, va = A == 1 ? 2 : 1;
    float fu = kRoomFootMax, fv = kRoomFootMax, Px[3], Py[3];
    if (RoomPlaneFootprint(t, d, n0, Dx, Dy, Px, Py))
    {
        fu = RoomFootprintAxis(Px, Py, ua);
        fv = RoomFootprintAxis(Px, Py, va);
    }
    float tn = 1.0f;
    // Creases: the cushion along the back and the armrests, the back above the cushion.
    if (b == 0 && normal == 2)
    {
        tn *= 1.0f - kRoomCrease * (1.0f - std::fmin(std::fmax((hi[2] - p[2]) / kRoomCreaseW, 0.0f), 1.0f));
        const float side = (0.5f * kRoomSeatPitch - kRoomArm[0]) - std::fabs(p[0] - xc);
        tn *= 1.0f - kRoomCrease * (1.0f - std::fmin(std::fmax(side / kRoomCreaseW, 0.0f), 1.0f));
    }
    if (b == 1 && normal == 5) tn *= 1.0f - kRoomCrease * (1.0f - std::fmin(std::fmax(p[1] - (yk + kRoomCushion[2]), 0.0f) / kRoomCreaseW, 1.0f));
    // The back's channels: each bulging between its seams, and the piping under the round top.
    if (b == 1 && !top && (normal == 4 || normal == 5))
    {
        const float u = (p[0] - xc) / kRoomChannel;
        const float cu = (u - std::floor(u)) * 2.0f - 1.0f;
        tilt[0] += kRoomChannelBulge * cu * std::fmin(std::fmax(1.0f - fu / (0.25f * kRoomChannel), 0.0f), 1.0f);
        float seam = 0;
        for (int i = -1; i <= 1; i++) seam += RoomBar(p[0], xc + (float)i * kRoomChannel, kRoomStitchW, fu);
        seam += RoomBar(p[1], c.cy - 0.5f * kRoomStitchW, kRoomStitchW, fv);
        tn *= 1.0f - kRoomStitch * std::fmin(seam, 1.0f);
    }
    // The pile (fainter on the armrests), where a pixel is smaller than its cells, and the seat's tone.
    const float wp = std::fmin(std::fmax(1.0f - std::fmax(fu, fv) / kRoomPileCell, 0.0f), 1.0f);
    const int seed = arm ? 2 : 1;
    if (wp > 0) tn *= 1.0f + 2.0f * (arm ? 0.5f * kRoomPile : kRoomPile) * wp * RoomSeatNoise(p[ua] / kRoomPileCell, p[va] / kRoomPileCell, seed);
    tn *= 1.0f + 2.0f * (arm ? kRoomArmVary : kRoomSeatVary) * (RoomSeatHash01(k, (int)jn, seed + 4) - 0.5f);
    // The shading normal.
    float m[3] = { n0[0] + tilt[0], n0[1] + tilt[1], n0[2] + tilt[2] };
    const float ml = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    for (int i = 0; i < 3; i++) ns[i] = ml > 0 ? m[i] / ml : n0[i];
    if (part == kRoomStandArmTop)
    {
        const float y = std::fmax(ns[1], 0.0f), y2 = y * y;
        tn *= 1.0f + kRoomArmPolish * y2 * y2;
    }
    // The sheen: brighter where the surface turns away from the eye.
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float cosv = dl > 0 ? std::fabs(ns[0] * d[0] + ns[1] * d[1] + ns[2] * d[2]) / dl : 1.0f;
    const float g = 1.0f - cosv, S = arm ? kRoomArmSheen : kRoomSheen;
    *sheen = 1.0f - 0.2f * S + S * g * g;
    *tone = tn;
    return true;
}

// What the eye sees on the stands at p (the ray o + t d met `part` on the face with normal
// index `normal`): light from the screen's coarse patches and the room light (`lamp`, may be
// null) by the disk formula, the bounce of their flux, and the house light, on the stands'
// carpet (the floor's colour), the seats' upholstery or their dark armrests. A tread is darker
// under a seat (box-filtered over the footprint), and in the aisles each tread's front edge
// has an amber step light. A seat's look is RoomSeatShade's. `coarse` holds rc.coarseCount
// emitters.
inline bool RoomStandsColour(const RoomConstants& rc, const RoomEmitter* coarse, const RoomEmitter* lamp, int part, int normal, const float o[3],
                             const float d[3], float t, const float Dx[3], const float Dy[3], float L[3], RoomSurfaceInfo* info)
{
    if (!o || !d || !Dx || !Dy || !L || (!coarse && rc.coarseCount > 0)) return false;
    if (part < 0 || part >= kRoomStandParts || normal < 0 || normal > 5) return false;
    float p[3], n[3] = { 0, 0, 0 };
    RoomAt(o, d, t, p);
    n[normal / 2] = normal % 2 == 0 ? 1.0f : -1.0f;
    // A seat: its shading normal, tone and sheen (RoomSeatShade), and its ambient light more
    // from above than from below.
    float tone = 1.0f, sheen = 1.0f, hemi = 1.0f;
    if (part >= kRoomStandCushion)
    {
        float ns[3];
        if (!RoomSeatShade(rc, part, normal, p, d, t, Dx, Dy, ns, &tone, &sheen)) return false;
        n[0] = ns[0]; n[1] = ns[1]; n[2] = ns[2];
        hemi = 0.8f + 0.2f * n[1];
    }
    float E[3] = { 0, 0, 0 }, flux[3] = { 0, 0, 0 };
    auto gather = [&](const RoomEmitter& e)
    {
        if (e.n[3] == 0) return;
        const float G = RoomDiskFactor(p, n, e), a = kRoomPi * e.c[0][3];
        for (int ch = 0; ch < 3; ch++) { E[ch] += e.L[ch] * G; flux[ch] += e.L[ch] * a; }
    };
    for (uint32_t c = 0; c < rc.coarseCount; c++) gather(coarse[c]);
    if (lamp) gather(*lamp);
    const bool carpet = part == kRoomStandTread || part == kRoomStandRiser;
    const float rho = carpet ? rc.rhoFloor : rc.rhoWall, shade = carpet ? rc.shadeFloor : 1.0f;
    // The carpet's pattern on a tread (x, z) or a riser (x, y).
    float pattern[3] = { 0, 0, 0 };
    if (carpet)
    {
        float Px[3], Py[3], fu = kRoomFootMax, fv = kRoomFootMax;
        if (RoomPlaneFootprint(t, d, n, Dx, Dy, Px, Py))
        {
            fu = RoomFootprintAxis(Px, Py, 0);
            fv = RoomFootprintAxis(Px, Py, part == kRoomStandTread ? 2 : 1);
        }
        RoomCarpet(p[0], part == kRoomStandTread ? p[2] : p[1], fu, fv, pattern);
    }
    const float* colour = carpet ? pattern : (part == kRoomStandArm || part == kRoomStandArmTop ? kRoomArmColour : kRoomUpholstery);
    float ao = 1.0f, strip = 0.0f;
    if (part == kRoomStandTread)
    {
        float Px[3], Py[3];
        if (RoomPlaneFootprint(t, d, n, Dx, Dy, Px, Py))
        {
            const float fa = RoomFootprintAxis(Px, Py, 0), fb = RoomFootprintAxis(Px, Py, 2);
            const int k = std::min(std::max((int)std::floor((p[2] - (rc.standsZ0 - 0.5f * rc.standsPitch)) / rc.standsPitch), 0), (int)rc.standsRows - 1);
            const float zc = rc.standsZ0 + (float)k * rc.standsPitch;
            const float q = (p[0] - rc.seatX0) / kRoomSeatPitch;
            const float jn = std::floor(q + 0.5f), xc = rc.seatX0 + jn * kRoomSeatPitch;
            const bool seat = std::fabs(xc) <= rc.seatBlock;
            if (seat) ao = 1.0f - kRoomUnderSeat * RoomBar(p[0] - xc, 0.0f, 2.0f * kRoomCushion[0] + 0.02f, fa) * RoomBar(p[2] - zc, 0.05f, 0.6f, fb);
            const float edge = rc.seatBlock + 0.5f * kRoomSeatPitch + kRoomArm[0];
            if (rc.X - edge > 0.4f)
                strip = RoomBar(std::fabs(p[0]), 0.5f * (edge + rc.X), rc.X - edge - 0.3f, fa) *
                        RoomBar(p[2], zc - 0.5f * rc.standsPitch + 0.5f * kRoomNosingW, kRoomNosingW, fb);
        }
    }
    for (int ch = 0; ch < 3; ch++)
    {
        const float lit = colour[ch] * ((rho / kRoomPi) * (E[ch] + flux[ch] * rc.bounceRgb[ch]) + shade * hemi * (rc.world[ch] + rc.ambient[ch])) * ao * tone * sheen;
        L[ch] = (1.0f - strip) * lit + strip * kRoomStepL[ch];
    }
    if (info) info->envPart = strip > 0.5f ? kRoomPartStep : (ao < 0.7f ? kRoomPartUnderSeat : 0);
    return true;
}

// The stands in the CPU's classification: the first hit, if it lies in the room (a curved
// front's wings or a side wall can come first), as its kind, and its t.
inline bool RoomStandsClassify(const Room& r, const float o[3], const float d[3], const float rcp[3], float* t, int* kind)
{
    if (!o || !d || !rcp || !t || !kind) return false;
    RoomStandsHit h;
    if (!RoomStandsTrace(MakeRoomStands(r), o, d, rcp, 3.0e38f, h)) return false;
    float p[3];
    RoomAt(o, d, h.t, p);
    if (std::fabs(p[0]) > r.X || p[2] < FrontDepth(r, p[0])) return false;
    *t = h.t;
    *kind = kRoomKindStands + h.part * 6 + h.normal;
    return true;
}

// A sample's own ray and its pixel's ray differentials, for the v11 look (RoomSurface).
struct RoomSampleRay
{
    float o[3] = { 0, 0, 0 }, d[3] = { 0, 0, 1 };
    float Dx[3] = { 0, 0, 0 }, Dy[3] = { 0, 0, 0 };
};

// One sample's colour, encoded (like the picture): the picture; black; the world
// colour; or a room surface - its lightmap radiance, plus the glow where it lies on
// the front wall (light on the wall adds: it does not hide the wall's own light), and
// with a v11 control on (RoomLookOn; `ray` is then the sample's ray) what RoomSurface
// makes of it: the room light's panel, the frames, the glass, the tiles and the reflections.
inline bool RoomSampleColour(const CurveConstants& c, const RoomView& view, const RoomEyeInputs& in, int kind, float u, float v,
                             float gu, float gv, const RoomSampleRay* ray, float out[3], RoomSurfaceInfo* info = nullptr)
{
    if (!out) return false;
    if (!in.rc || !in.picture || !in.light) return false;
    if (kind == 0)
    {
        float s[4];
        if (!SampleRgba(*in.picture, u, v, s)) return false;
        out[0] = s[0]; out[1] = s[1]; out[2] = s[2];
        return true;
    }
    if (kind == kRoomKindFootprint) { out[0] = out[1] = out[2] = 0; return true; }
    if (kind >= kRoomKindStands && kind < kRoomKinds)
    {
        // The stands: u is the ray's t at the hit (the mean of the four when they agree).
        const RoomConstants& rc = *in.rc;
        if (!ray || !in.emitters || rc.coarseFirst == 0 || (size_t)rc.coarseFirst + rc.coarseCount > in.emitters->size()) return false;
        const RoomEmitter* coarse = in.emitters->data() + rc.coarseFirst;
        const RoomEmitter* lamp = rc.emitters > 0 ? in.emitters->data() + (rc.emitters - 1) : nullptr;
        float L[3];
        const int s = kind - kRoomKindStands;
        if (!RoomStandsColour(rc, coarse, lamp, s / 6, s % 6, ray->o, ray->d, u, ray->Dx, ray->Dy, L, info)) return false;
        for (int ch = 0; ch < 3; ch++) out[ch] = LinearToSrgb(std::fmax(L[ch], 0.0f));
        return true;
    }
    if (kind == kRoomKindOutside) { out[0] = c.world[0]; out[1] = c.world[1]; out[2] = c.world[2]; return true; }
    float L[3];
    if (!in.light->Sample(kind - 1, u, v, L)) return false;
    // An environment's surface comes before the glow: the glow is light on the wall.
    const bool env = in.rc->env != (uint32_t)kRoomEnvRoom;
    if (env && (!ray || !RoomEnvSurface(*in.rc, kind - 1, u, v, ray->o, ray->d, ray->Dx, ray->Dy, L, info, in.light))) return false;
    if (kind - 1 == kFaceFront && view.glowOn && in.glow && gu >= 0 && gu <= 1 && gv >= 0 && gv <= 1)
    {
        float g[4];
        if (!SampleRgba(*in.glow, gu, gv, g)) return false;
        for (int ch = 0; ch < 3; ch++) L[ch] += SrgbToLinear(g[ch]);
    }
    if (!env && ray && RoomLookOn(*in.rc) && !RoomSurface(*in.rc, kind - 1, ray->o, ray->d, ray->Dx, ray->Dy, L, info, &c, &in)) return false;
    for (int ch = 0; ch < 3; ch++) out[ch] = LinearToSrgb(std::fmax(L[ch], 0.0f));
    return true;
}

// One eye-buffer pixel of the room pass: four rays as CurvedPixel, classified with one
// full exit per pixel (RoomPixelState); one sample when they agree; the dither on room
// surfaces. With a v11 control on (RoomLookOn), a room sample goes through RoomSurface:
// when the four agree with the pixel's centre ray (the exact mean of the four
// directions), otherwise each with its own ray. `info` (may be null): what the finish
// drew at the centre sample, or at the first ray's when they do not agree.
inline bool RoomPixel(const CurveConstants& c, const Cylinder& cyl, const Room& r, const RoomView& view, int e, int px, int py,
                      const RoomEyeInputs& inputs, float out[3], RoomSurfaceInfo* info = nullptr)
{
    if (!out) return false;
    if (e < 0 || e > 1 || px < 0 || py < 0 || px >= (int)c.ew || py >= (int)c.eh) return false;
    if (!inputs.rc || !inputs.picture || !inputs.light) return false;
    if (!view.flatLayer && !inputs.picture->data) return false;
    RoomEyeInputs in = inputs;
    if (!in.room) in.room = &r;

    int kind[4];
    float uu[4], vv[4], gu[4], gv[4];
    RoomPixelState state = MakeRoomPixelState(cyl, r, view, c.eye[e].origin);
    // The theatre's stands: the codes of the 3 x 3 neighbourhood of the GPU's half-size id
    // buffer round the pixel's texel (each texel the ray through the corner its 2 x 2 pixels
    // share, edges clamped), then the pixel's four rays through them (RoomStandsSamples).
    float standsT[4] = { 0, 0, 0, 0 };
    int standsKind[4] = { -1, -1, -1, -1 };
    if (r.standsRows > 0 && state.inside)
    {
        const RoomStands st = MakeRoomStands(r);
        uint32_t codes[9];
        for (int i = 0; i < 9; i++)
        {
            const int hw = ((int)c.ew + 1) / 2, hh = ((int)c.eh + 1) / 2;
            const int qx = std::min(std::max(px / 2 + (i % 3) - 1, 0), hw - 1), qy = std::min(std::max(py / 2 + (i / 3) - 1, 0), hh - 1);
            float o[3], d[3], rcp[3];
            CurveRay(c, e, 2.0f * (float)qx + 1.0f, 2.0f * (float)qy + 1.0f, o, d);
            RoomRayRcp(d, rcp);
            RoomStandsHit h;
            codes[i] = RoomStandsTrace(st, o, d, rcp, 3.0e38f, h) ? RoomStandsCode(h.object, h.box, h.part, h.normal) : 0;
        }
        float o[3], d[4][3];
        for (int s = 0; s < 4; s++) CurveRay(c, e, (float)px + 0.5f + kCurveSubsamples[s][0], (float)py + 0.5f + kCurveSubsamples[s][1], o, d[s]);
        int k4[4];
        RoomStandsSamples(st, r.X, [&](float x) { return FrontDepth(r, x); }, codes, o, d, standsT, k4);
        for (int s = 0; s < 4; s++) standsKind[s] = k4[s];
    }
    for (int s = 0; s < 4; s++)
    {
        float o[3], d[3];
        CurveRay(c, e, (float)px + 0.5f + kCurveSubsamples[s][0], (float)py + 0.5f + kCurveSubsamples[s][1], o, d);
        kind[s] = RoomClassifyRay(cyl, r, view, state, o, d, &uu[s], &vv[s], &gu[s], &gv[s], standsKind[s], standsT[s]);
    }
    const bool look = RoomLookOn(*in.rc);
    RoomSampleRay ray;
    if (look && !RoomRayDiff(c, e, ray.Dx, ray.Dy)) return false;
    const bool agree = kind[0] == kind[1] && kind[1] == kind[2] && kind[2] == kind[3];
    if (agree)
    {
        const float mu = (uu[0] + uu[1] + uu[2] + uu[3]) * 0.25f, mv = (vv[0] + vv[1] + vv[2] + vv[3]) * 0.25f;
        const float mgu = (gu[0] + gu[1] + gu[2] + gu[3]) * 0.25f, mgv = (gv[0] + gv[1] + gv[2] + gv[3]) * 0.25f;
        if (look) CurveRay(c, e, (float)px + 0.5f, (float)py + 0.5f, ray.o, ray.d);
        if (!RoomSampleColour(c, view, in, kind[0], mu, mv, mgu, mgv, look ? &ray : nullptr, out, info)) return false;
    }
    else
    {
        out[0] = out[1] = out[2] = 0;
        for (int s = 0; s < 4; s++)
        {
            if (look) CurveRay(c, e, (float)px + 0.5f + kCurveSubsamples[s][0], (float)py + 0.5f + kCurveSubsamples[s][1], ray.o, ray.d);
            float rgb[3];
            if (!RoomSampleColour(c, view, in, kind[s], uu[s], vv[s], gu[s], gv[s], look ? &ray : nullptr, rgb, s == 0 ? info : nullptr)) return false;
            for (int ch = 0; ch < 3; ch++) out[ch] += rgb[ch] * 0.25f;
        }
    }
    const bool room = (kind[0] >= 1 && kind[0] <= kRoomFaces) || kind[0] >= kRoomKindStands;
    if (view.dither && agree && room)
        for (int ch = 0; ch < 3; ch++) out[ch] += RoomDither(e, px, py);
    return true;
}

// ------------------------------------------------------------ shader constants
// A float as an HLSL literal that reads back as exactly this float (%.9g round-trips a
// float; a whole number gets ".0" so that it stays a float literal).
inline std::string RoomHlslFloat(float v)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%.9g", (double)v);
    std::string s(buf);
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

// The constants the room's HLSL shares with this header, as #define lines that
// InitShaders puts in front of every room shader, so that the two cannot drift.
inline std::string RoomHlslDefines()
{
    std::string s;
    char value[32];
    auto def = [&s](const char* name, const std::string& v)
    {
        s += "#define ";
        s += name;
        s += " ";
        s += v;
        s += "\n";
    };
    auto defInt = [&](const char* name, int v) { snprintf(value, sizeof(value), "%d", v); def(name, value); };
    auto defUint = [&](const char* name, uint32_t v) { snprintf(value, sizeof(value), "%uu", v); def(name, value); };
    def("ROOM_PI", RoomHlslFloat(kRoomPi));
    def("ROOM_INSIDE_FRONT", RoomHlslFloat(kRoomInsideFront));
    defInt("ROOM_LIGHTMAP", kRoomLightmap);
    defInt("ROOM_FACES", kRoomFaces);
    defInt("ROOM_FACE_FRONT", kFaceFront);
    defInt("ROOM_FACE_LEFT", kFaceLeft);
    defInt("ROOM_FACE_RIGHT", kFaceRight);
    defInt("ROOM_FACE_FLOOR", kFaceFloor);
    defInt("ROOM_FACE_CEILING", kFaceCeiling);
    defInt("ROOM_FACE_BACK", kFaceBack);
    defInt("ROOM_EMITTER_FLOAT4S", kRoomEmitterFloat4s);
    defInt("ROOM_GROUP_THREADS", kRoomGroupThreads);
    defUint("ROOM_FLAG_CURVED", kRoomFlagCurved);
    defUint("ROOM_FLAG_FLAT_LAYER", kRoomFlagFlatLayer);
    defUint("ROOM_FLAG_GLOW", kRoomFlagGlow);
    defUint("ROOM_FLAG_DITHER", kRoomFlagDither);
    defUint("ROOM_FLAG_LIGHT", kRoomFlagLight);
    defUint("ROOM_FLAG_FINISH", kRoomFlagFinish);
    defUint("ROOM_FLAG_REFLECT", kRoomFlagReflect);
    def("ROOM_GLASS_F0", RoomHlslFloat(kRoomGlassF0));
    def("ROOM_NUDGE", RoomHlslFloat(kRoomNudge));
    def("ROOM_REFL_FOOT_MAX", RoomHlslFloat(kRoomReflFootMax));
    def("ROOM_ENV_LINE_MEAN", RoomHlslFloat(kRoomEnvLineMean));
    def("ROOM_FLOOR_GLOSS", RoomHlslFloat(kRoomFloorGloss));
    def("ROOM_FRAME_W", RoomHlslFloat(kRoomFrameW));
    def("ROOM_GROUT_W", RoomHlslFloat(kRoomGroutW));
    def("ROOM_GROUT", RoomHlslFloat(kRoomGrout));
    def("ROOM_TILE_VAR", RoomHlslFloat(kRoomTileVar));
    def("ROOM_ENV_ZENITH", RoomHlslFloat(kRoomEnvZenith));
    def("ROOM_ENV_HORIZON", RoomHlslFloat(kRoomEnvHorizon));
    def("ROOM_ENV_GROUND", RoomHlslFloat(kRoomEnvGround));
    def("ROOM_ENV_LINE", RoomHlslFloat(kRoomEnvLine));
    def("ROOM_ENV_LINE_W", RoomHlslFloat(kRoomEnvLineW));
    def("ROOM_ENV_FOG", RoomHlslFloat(kRoomEnvFog));
    def("ROOM_ENV_HORIZON_DY", RoomHlslFloat(kRoomEnvHorizonDy));
    def("ROOM_FOOT_MIN", RoomHlslFloat(kRoomFootMin));
    def("ROOM_FOOT_MAX", RoomHlslFloat(kRoomFootMax));
    defInt("ROOM_KIND_FOOTPRINT", kRoomKindFootprint);
    defInt("ROOM_KIND_OUTSIDE", kRoomKindOutside);
    defInt("ROOM_PART_WING_L", kRoomPartWingL);
    defInt("ROOM_PART_WING_R", kRoomPartWingR);
    def("ROOM_SCREEN_PAD", RoomHlslFloat(kRoomScreenPad));
    // The environments (XENV.md).
    auto def3 = [&](const char* name, const float v[3])
    {
        def(name, "float3(" + RoomHlslFloat(v[0]) + ", " + RoomHlslFloat(v[1]) + ", " + RoomHlslFloat(v[2]) + ")");
    };
    defUint("ROOM_ENV_ROOM", (uint32_t)kRoomEnvRoom);
    defUint("ROOM_ENV_CINEMA", (uint32_t)kRoomEnvCinema);
    defUint("ROOM_ENV_OPEN_AIR", (uint32_t)kRoomEnvOpenAir);
    defUint("ROOM_ENV_LOUNGE", (uint32_t)kRoomEnvLounge);
    defUint("ROOM_ENV_COUNT", (uint32_t)kRoomEnvCount);
    def("ROOM_FOLD_PITCH", RoomHlslFloat(kRoomFoldPitch));
    def("ROOM_FOLD_AMP", RoomHlslFloat(kRoomFoldAmp));
    def("ROOM_CURTAIN_PITCH", RoomHlslFloat(kRoomCurtainPitch));
    def("ROOM_CURTAIN_AMP", RoomHlslFloat(kRoomCurtainAmp));
    def("ROOM_MASK_W", RoomHlslFloat(kRoomMaskW));
    def("ROOM_MASK_LEVEL", RoomHlslFloat(kRoomMaskLevel));
    def("ROOM_STEP_INSET", RoomHlslFloat(kRoomStepInset));
    def("ROOM_STEP_PITCH", RoomHlslFloat(kRoomStepPitch));
    def("ROOM_STEP_W", RoomHlslFloat(kRoomStepW));
    def("ROOM_STEP_D", RoomHlslFloat(kRoomStepD));
    def3("ROOM_STEP_L", kRoomStepL);
    def("ROOM_STEP_POOL", RoomHlslFloat(kRoomStepPool));
    def("ROOM_STEP_POOL_GAIN", RoomHlslFloat(kRoomStepPoolGain));
    def("ROOM_HALO_W0", RoomHlslFloat(kRoomHaloW0));
    def("ROOM_HALO_W1", RoomHlslFloat(kRoomHaloW1));
    def("ROOM_SCONCE_OFF", RoomHlslFloat(kRoomSconceOff));
    def3("ROOM_SKY_HORIZON", kRoomSkyHorizon);
    def3("ROOM_SKY_ZENITH", kRoomSkyZenith);
    def3("ROOM_HILLS", kRoomHills);
    def("ROOM_HILL_FOG", RoomHlslFloat(kRoomHillFog));
    def("ROOM_FIELD_FALL", RoomHlslFloat(kRoomFieldFall));
    defInt("ROOM_STAR_GRID", kRoomStarGrid);
    def("ROOM_STAR_DENSITY", RoomHlslFloat(kRoomStarDensity));
    def("ROOM_STAR_CORE", RoomHlslFloat(kRoomStarCore));
    def("ROOM_STAR_RISE", RoomHlslFloat(kRoomStarRise));
    def("ROOM_SHADE_FLOOR", RoomHlslFloat(kRoomShadeFloor));
    def("ROOM_BOARD_W", RoomHlslFloat(kRoomBoardW));
    def("ROOM_BOARD_L", RoomHlslFloat(kRoomBoardL));
    def("ROOM_SEAM_W", RoomHlslFloat(kRoomSeamW));
    def("ROOM_SEAM", RoomHlslFloat(kRoomSeam));
    def("ROOM_BOARD_VAR", RoomHlslFloat(kRoomBoardVar));
    def("ROOM_GRAIN_PITCH", RoomHlslFloat(kRoomGrainPitch));
    def("ROOM_GRAIN_AMP", RoomHlslFloat(kRoomGrainAmp));
    def("ROOM_RUG_BORDER", RoomHlslFloat(kRoomRugBorder));
    def3("ROOM_RUG_FIELD", kRoomRugField);
    def3("ROOM_RUG_EDGE", kRoomRugEdge);
    def("ROOM_SKIRT_H", RoomHlslFloat(kRoomSkirtH));
    def3("ROOM_SKIRT", kRoomSkirt);
    def("ROOM_DADO_Y", RoomHlslFloat(kRoomDadoY));
    def("ROOM_DADO_W", RoomHlslFloat(kRoomDadoW));
    def("ROOM_DADO", RoomHlslFloat(kRoomDado));
    def("ROOM_GROOVE_PITCH", RoomHlslFloat(kRoomGroovePitch));
    def("ROOM_GROOVE_W", RoomHlslFloat(kRoomGrooveW));
    def("ROOM_GROOVE", RoomHlslFloat(kRoomGroove));
    def("ROOM_SLAT_PITCH", RoomHlslFloat(kRoomSlatPitch));
    def("ROOM_SLAT_AMP", RoomHlslFloat(kRoomSlatAmp));
    def("ROOM_DOWN_R", RoomHlslFloat(kRoomDownR));
    def("ROOM_DOWN_TRIM", RoomHlslFloat(kRoomDownTrim));
    // The theatre.
    defUint("ROOM_ENV_THEATRE", (uint32_t)kRoomEnvTheatre);
    defInt("ROOM_KIND_STANDS", kRoomKindStands);
    defInt("ROOM_COARSE_X", kRoomCoarseX);
    defInt("ROOM_COARSE_Y", kRoomCoarseY);
    defInt("ROOM_SEAT_WORDS", kRoomSeatWords);
    def("ROOM_BLOCK_BELOW", RoomHlslFloat(kRoomBlockBelow));
    defInt("ROOM_KINDS", kRoomKinds);
    defInt("ROOM_STAND_TREAD", kRoomStandTread);
    defInt("ROOM_STAND_RISER", kRoomStandRiser);
    defInt("ROOM_STAND_CUSHION", kRoomStandCushion);
    defInt("ROOM_STAND_BACK", kRoomStandBack);
    defInt("ROOM_STAND_ARM", kRoomStandArm);
    defInt("ROOM_STAND_BACK_TOP", kRoomStandBackTop);
    defInt("ROOM_STAND_ARM_TOP", kRoomStandArmTop);
    defInt("ROOM_CODE_BOX_SHIFT", kRoomCodeBoxShift);
    defInt("ROOM_CODE_OBJECT_SHIFT", kRoomCodeObjectShift);
    defUint("ROOM_CODE_KIND_MASK", kRoomCodeKindMask);
    def("ROOM_BACK_R", RoomHlslFloat(kRoomBackR));
    def("ROOM_ARM_R", RoomHlslFloat(kRoomArmR));
    def("ROOM_CUSHION_BEVEL", RoomHlslFloat(kRoomCushionBevel));
    def("ROOM_BACK_BEVEL", RoomHlslFloat(kRoomBackBevel));
    def("ROOM_ARM_BEVEL", RoomHlslFloat(kRoomArmBevel));
    def("ROOM_CREASE", RoomHlslFloat(kRoomCrease));
    def("ROOM_CREASE_W", RoomHlslFloat(kRoomCreaseW));
    def("ROOM_CHANNEL", RoomHlslFloat(kRoomChannel));
    def("ROOM_STITCH_W", RoomHlslFloat(kRoomStitchW));
    def("ROOM_STITCH", RoomHlslFloat(kRoomStitch));
    def("ROOM_CHANNEL_BULGE", RoomHlslFloat(kRoomChannelBulge));
    def("ROOM_PILE_CELL", RoomHlslFloat(kRoomPileCell));
    def("ROOM_PILE", RoomHlslFloat(kRoomPile));
    def("ROOM_SEAT_VARY", RoomHlslFloat(kRoomSeatVary));
    def("ROOM_ARM_VARY", RoomHlslFloat(kRoomArmVary));
    def("ROOM_SHEEN", RoomHlslFloat(kRoomSheen));
    def("ROOM_ARM_SHEEN", RoomHlslFloat(kRoomArmSheen));
    def("ROOM_ARM_POLISH", RoomHlslFloat(kRoomArmPolish));
    def("ROOM_SEAT_PITCH", RoomHlslFloat(kRoomSeatPitch));
    def("ROOM_CHAIR_TOP", RoomHlslFloat(kRoomChairTop));
    auto defBox = [&](const char* name, const float b[5])
    {
        def(name, "{ " + RoomHlslFloat(b[0]) + ", " + RoomHlslFloat(b[1]) + ", " + RoomHlslFloat(b[2]) + ", " + RoomHlslFloat(b[3]) + ", " + RoomHlslFloat(b[4]) + " }");
    };
    defBox("ROOM_CUSHION", kRoomCushion);
    defBox("ROOM_BACK", kRoomBack);
    defBox("ROOM_ARM", kRoomArm);
    def3("ROOM_UPHOLSTERY", kRoomUpholstery);
    def3("ROOM_ARM_COLOUR", kRoomArmColour);
    def("ROOM_UNDER_SEAT", RoomHlslFloat(kRoomUnderSeat));
    def("ROOM_NOSING_W", RoomHlslFloat(kRoomNosingW));
    def("ROOM_CARPET_TILE", RoomHlslFloat(kRoomCarpetTile));
    def("ROOM_CARPET_LINE_W", RoomHlslFloat(kRoomCarpetLineW));
    def("ROOM_CARPET_DIAMOND", RoomHlslFloat(kRoomCarpetDiamond));
    def("ROOM_CARPET_DOT", RoomHlslFloat(kRoomCarpetDot));
    def3("ROOM_CARPET_BASE", kRoomCarpetBase);
    def3("ROOM_CARPET_GOLD", kRoomCarpetGold);
    def3("ROOM_CARPET_TEAL", kRoomCarpetTeal);
    std::string bayer = "{ ";
    for (int i = 0; i < 64; i++)
    {
        snprintf(value, sizeof(value), i ? ", %d" : "%d", kRoomBayer[i]);
        bayer += value;
    }
    bayer += " }";
    def("ROOM_BAYER", bayer);
    return s;
}
