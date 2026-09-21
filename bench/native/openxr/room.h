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

// ------------------------------------------------------------------- geometry
struct RoomInputs
{
    float W = 0, H = 0;                 // the screen, metres
    float eye[3] = { 0, 0, 0 };         // the recentred head, screen-local
    float floorY = NAN;                 // the real floor's y, screen-local (NaN: not tracked)
    Cylinder cyl;                       // curved screen, or not (cyl.curved false)
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
    int count() const { return gridX * gridY + blocksX * blocksY + lights; }
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
// top-left; then the glow's blocks, row by row; then the room light. Glow blocks wholly
// inside the screen (the glow is zero there) or outside the room's height are marked
// inactive, as is the room light when the room has no space for its panel.
inline bool BuildRoomEmitters(const Room& r, const Cylinder& cyl, float W, float H, float glowHalfW, float glowHalfH,
                              const RoomEmitterLayout& l, std::vector<RoomEmitter>& out)
{
    if (!r.valid || l.gridX <= 0 || l.count() > kRoomMaxEmitters) return false;
    if (!(W > 0) || !(H > 0) || !(glowHalfW > 0) || !(glowHalfH > 0)) return false;

    out.assign((size_t)l.count(), RoomEmitter());
    for (int j = 0; j < l.gridY; j++)
        for (int i = 0; i < l.gridX; i++)
        {
            RoomEmitter& e = out[(size_t)j * l.gridX + i];
            const float y0 = 0.5f * H - (float)j * H / (float)l.gridY, y1 = 0.5f * H - (float)(j + 1) * H / (float)l.gridY;
            float c[4][3], n[3];
            if (!r.curved || !cyl.curved)
            {
                const float x0 = -0.5f * W + (float)i * W / (float)l.gridX, x1 = -0.5f * W + (float)(i + 1) * W / (float)l.gridX;
                const float q[4][3] = { { x0, y0, 0 }, { x1, y0, 0 }, { x1, y1, 0 }, { x0, y1, 0 } };
                memcpy(c, q, sizeof(c));
                n[0] = 0; n[1] = 0; n[2] = 1;
            }
            else
            {
                const float R = cyl.radius;
                const float p0 = -cyl.halfWrap + (float)i * 2.0f * cyl.halfWrap / (float)l.gridX;
                const float p1 = -cyl.halfWrap + (float)(i + 1) * 2.0f * cyl.halfWrap / (float)l.gridX;
                const float x0 = R * std::sin(p0), z0 = R - R * std::cos(p0), x1 = R * std::sin(p1), z1 = R - R * std::cos(p1);
                const float q[4][3] = { { x0, y0, z0 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z0 } };
                memcpy(c, q, sizeof(c));
                const float pc = 0.5f * (p0 + p1);
                n[0] = -std::sin(pc); n[1] = 0; n[2] = std::cos(pc);
            }
            RoomSetQuad(e, c, n);
        }
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
// constants' row 14), or 0 when it is null - the block loops never reach its index.
inline bool RoomEmitRadiance(const RoomEmitterLayout& l, const unsigned char* src, int srcW, int srcH, int srcPitch,
                             const unsigned char* glow, int glowPitch, bool glowOn, float alpha,
                             const float table[256], std::vector<RoomEmitter>& emitters, const float* lightL = nullptr)
{
    if (!src || !table || srcW <= 0 || srcH <= 0 || srcPitch < srcW * 4) return false;
    if ((int)emitters.size() != l.count()) return false;
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

// The room's controls beyond the Room slider (v11). So far the room light: its level,
// 0 (off) .. 100, and its colour. Glass and Reflections join it in the next steps.
struct RoomLook
{
    int light = 0;
    uint32_t lightRgb = kRoomLightDefault;      // 0xRRGGBB
};

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
};

// The lightmap's albedos and bounce, the house lights and the room light. The bounce's
// mean albedo rhoBar weighs each face's albedo a_f by its area; the screen absorbs. The
// room light's panel is part of the ceiling, of the ceiling's albedo, so with Glass and
// Reflections at 0 (all there is so far) a_f is each face's plain albedo and rhoBar is
// v10's to the bit. The room light's own flux joins the bounce as every emitter's does,
// through RoomTexel's flux sum; kappaBar is kept for the bounce once the rest of the
// ceiling can be glass.
inline RoomShading MakeRoomShading(const Room& r, int roomPercent, uint32_t worldRgb, float screenArea, const RoomLook& look = RoomLook())
{
    RoomShading s;
    const float pct = (float)(roomPercent < 0 ? 0 : (roomPercent > 100 ? 100 : roomPercent)) / 100.0f;
    s.rhoWall = kRoomMaxAlbedo * pct;
    s.rhoCeiling = s.rhoWall;
    s.rhoFloor = kRoomFloorAlbedo * s.rhoWall;
    float total = 0, weighted = 0;
    for (int f = 0; f < kRoomFaces; f++)
    {
        const float a = RoomArea(r, f);
        const float rho = f == kFaceFloor ? s.rhoFloor : (f == kFaceCeiling ? s.rhoCeiling : s.rhoWall);
        total += a;
        weighted += rho * (f == kFaceFront ? std::fmax(a - screenArea, 0.0f) : a);   // the screen absorbs
    }
    s.rhoBar = total > 0 ? weighted / total : 0;
    s.invArea = total > 0 ? 1.0f / total : 0;
    s.world[0] = SrgbToLinear((float)((worldRgb >> 16) & 255) / 255.0f);
    s.world[1] = SrgbToLinear((float)((worldRgb >> 8) & 255) / 255.0f);
    s.world[2] = SrgbToLinear((float)(worldRgb & 255) / 255.0f);

    const float level = (float)(look.light < 0 ? 0 : (look.light > 100 ? 100 : look.light)) / 100.0f;
    const float strength = level * level;
    const float c[3] = { SrgbToLinear((float)((look.lightRgb >> 16) & 255) / 255.0f), SrgbToLinear((float)((look.lightRgb >> 8) & 255) / 255.0f),
                         SrgbToLinear((float)(look.lightRgb & 255) / 255.0f) };
    const float peak = std::fmax(std::fmax(c[0], c[1]), c[2]);
    s.lightOn = r.valid && r.lightValid && strength > 0 && peak > 0;
    if (s.lightOn)
        for (int ch = 0; ch < 3; ch++)
        {
            const float cn = c[ch] / peak;
            s.lightL[ch] = kRoomLightMax * strength * cn;
            s.lightSeen[ch] = cn * std::fmin(1.0f, kRoomLightMax * strength);
        }
    const float ceiling = RoomArea(r, kFaceCeiling);
    s.kappaBar = r.lightValid && ceiling > 0 ? (r.lightX1 - r.lightX0) * (r.lightZ1 - r.lightZ0) / ceiling : 0.0f;
    return s;
}

inline float RoomBounceScale(const RoomShading& sh);

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
    for (const RoomEmitter& e : emitters)
    {
        if (e.n[3] == 0) continue;
        const float G = RoomFormFactor(p, n, e);
        const float a = kRoomPi * e.c[0][3];
        for (int ch = 0; ch < 3; ch++) { E[ch] += e.L[ch] * G; flux[ch] += e.L[ch] * a; }
    }
    const float rho = face == kFaceFloor ? sh.rhoFloor : (face == kFaceCeiling ? sh.rhoCeiling : sh.rhoWall);
    const float shade = face == kFaceFloor ? kRoomShadeFloor : (face == kFaceCeiling ? kRoomShadeCeiling : 1.0f);
    const float bounceScale = RoomBounceScale(sh);
    for (int ch = 0; ch < 3; ch++)
        out[ch] = (rho / kRoomPi) * (E[ch] + flux[ch] * bounceScale) + shade * sh.world[ch];
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
static const uint32_t kRoomFlagLight = 64;      // the room light is on: its panel fits, its level is above 0, its colour is not black

// Rows 0-10 are v10's. Rows 11-20 are v11's: glass, reflections and the mirror picture
// (rows 11, 12 and 16, zero until the steps that fill them), the room light (rows
// 13-15), and the eye pass's reciprocals and screen bounds (rows 17-20).
// kRoomCbufferHlsl declares rows 0-20.
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
    uint32_t mirrorW = 0, mirrorH = 0, upad0 = 0, upad1 = 0;        // 16
    float invH = 0, inv2X = 0, invSide = 0, invFloorZ = 0;          // 17
    float inv2sMax = 0, invGlowW = 0, invGlowH = 0, tanA = 0;       // 18
    float wingS = 0, scrTanWrap = 0, scrBoxX = 0, scrBoxY = 0;      // 19
    float scrBoxZ = 0, rpad6 = 0, rpad7 = 0, rpad8 = 0;             // 20
    float pad5[44] = {};                                            // 21-31
};
static_assert(sizeof(RoomConstants) == 512, "RoomConstants is one 512-byte constant buffer");
static_assert(offsetof(RoomConstants, glowH) == 160 && offsetof(RoomConstants, glass) == 176, "rows 0-10 are v10's; row 11 starts at 176");
static_assert(offsetof(RoomConstants, pitchSide) == 192 && offsetof(RoomConstants, lightX0) == 208 && offsetof(RoomConstants, lightL) == 224 &&
              offsetof(RoomConstants, lightSeen) == 240 && offsetof(RoomConstants, mirrorW) == 256 && offsetof(RoomConstants, invH) == 272 &&
              offsetof(RoomConstants, inv2sMax) == 288 && offsetof(RoomConstants, wingS) == 304 && offsetof(RoomConstants, scrBoxZ) == 320 &&
              offsetof(RoomConstants, pad5) == 336, "rows 11-20 as in the v11 spec (section 3.5)");

// Whether any of the v11 controls is on in these constants, so that the eye pass needs
// their code (xrapp5.cpp RoomEyePso picks its look variant). So far the room light;
// Glass and Reflections join it in the next steps.
inline bool RoomLookOn(const RoomConstants& rc)
{
    return (rc.flags & kRoomFlagLight) != 0;
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
              (view.dither ? kRoomFlagDither : 0) | (sh.lightOn ? kRoomFlagLight : 0);
    c.gridX = (uint32_t)l.gridX; c.gridY = (uint32_t)l.gridY; c.emitters = (uint32_t)l.count();
    c.srcW = (uint32_t)(srcW > 0 ? srcW : 0); c.srcH = (uint32_t)(srcH > 0 ? srcH : 0); c.stride = (uint32_t)RoomStride(srcW);
    c.glowW = (uint32_t)l.glowW; c.glowH = (uint32_t)l.glowH; c.blocksX = (uint32_t)l.blocksX; c.blocksY = (uint32_t)l.blocksY;
    c.glowBlock = (uint32_t)l.block;
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

// One ray of a pixel, as the eye pass classifies it (kCurveRoomHlsl ClassifyR).
inline int RoomClassifyRay(const Cylinder& cyl, const Room& r, const RoomView& view, RoomPixelState& state,
                           const float o[3], const float d[3], float* u, float* v, float* gu, float* gv)
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

// What the eye pass reads besides its geometry: the room's constants (the room light's
// panel, colour and flag, read from here as the GPU reads them from RoomC), each eye's
// picture (it may have no pixels in the flat layer, which never samples it), the glow
// (null: none) and the lightmap.
struct RoomEyeInputs
{
    const RoomConstants* rc = nullptr;
    const RgbaImage* picture = nullptr;
    const RgbaImage* glow = nullptr;
    const RoomLightmap* light = nullptr;
};

// One sample's colour, encoded (like the picture): the picture; black; the world
// colour; or a room surface - its lightmap radiance, plus the glow where it lies on
// the front wall (light on the wall adds: it does not hide the wall's own light), and
// on the ceiling, with the room light on, the panel's own light where it covers the
// pixel (kappa, RoomCeilingPanel).
inline bool RoomSampleColour(const CurveConstants& c, const RoomView& view, const RoomEyeInputs& in, int kind, float u, float v,
                             float gu, float gv, float kappa, float out[3])
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
    if (kind == kRoomKindOutside) { out[0] = c.world[0]; out[1] = c.world[1]; out[2] = c.world[2]; return true; }
    float L[3];
    if (!in.light->Sample(kind - 1, u, v, L)) return false;
    if (kind - 1 == kFaceFront && view.glowOn && in.glow && gu >= 0 && gu <= 1 && gv >= 0 && gv <= 1)
    {
        float g[4];
        if (!SampleRgba(*in.glow, gu, gv, g)) return false;
        for (int ch = 0; ch < 3; ch++) L[ch] += SrgbToLinear(g[ch]);
    }
    if (kind - 1 == kFaceCeiling && (in.rc->flags & kRoomFlagLight) != 0)
        for (int ch = 0; ch < 3; ch++) L[ch] += kappa * in.rc->lightSeen[ch];
    for (int ch = 0; ch < 3; ch++) out[ch] = LinearToSrgb(std::fmax(L[ch], 0.0f));
    return true;
}

// One eye-buffer pixel of the room pass: four rays as CurvedPixel, classified with one
// full exit per pixel (RoomPixelState); one sample when they agree; the dither on room
// surfaces. With the room light on, a ceiling sample adds the panel's light: when the
// four agree, over the footprint of the pixel's centre ray (the exact mean of the four
// directions); otherwise each of its own ray.
inline bool RoomPixel(const CurveConstants& c, const Cylinder& cyl, const Room& r, const RoomView& view, int e, int px, int py,
                      const RoomEyeInputs& in, float out[3])
{
    if (!out) return false;
    if (e < 0 || e > 1 || px < 0 || py < 0 || px >= (int)c.ew || py >= (int)c.eh) return false;
    if (!in.rc || !in.picture || !in.light) return false;
    if (!view.flatLayer && !in.picture->data) return false;

    int kind[4];
    float uu[4], vv[4], gu[4], gv[4];
    RoomPixelState state = MakeRoomPixelState(cyl, r, view, c.eye[e].origin);
    for (int s = 0; s < 4; s++)
    {
        float o[3], d[3];
        CurveRay(c, e, (float)px + 0.5f + kCurveSubsamples[s][0], (float)py + 0.5f + kCurveSubsamples[s][1], o, d);
        kind[s] = RoomClassifyRay(cyl, r, view, state, o, d, &uu[s], &vv[s], &gu[s], &gv[s]);
    }
    const bool lightOn = (in.rc->flags & kRoomFlagLight) != 0;
    float Dx[3] = { 0, 0, 0 }, Dy[3] = { 0, 0, 0 };
    if (lightOn && !RoomRayDiff(c, e, Dx, Dy)) return false;
    const bool agree = kind[0] == kind[1] && kind[1] == kind[2] && kind[2] == kind[3];
    if (agree)
    {
        const float mu = (uu[0] + uu[1] + uu[2] + uu[3]) * 0.25f, mv = (vv[0] + vv[1] + vv[2] + vv[3]) * 0.25f;
        const float mgu = (gu[0] + gu[1] + gu[2] + gu[3]) * 0.25f, mgv = (gv[0] + gv[1] + gv[2] + gv[3]) * 0.25f;
        float kappa = 0;
        if (lightOn && kind[0] == 1 + kFaceCeiling)
        {
            float o[3], d[3];
            CurveRay(c, e, (float)px + 0.5f, (float)py + 0.5f, o, d);
            kappa = RoomCeilingPanel(*in.rc, o, d, Dx, Dy);
        }
        if (!RoomSampleColour(c, view, in, kind[0], mu, mv, mgu, mgv, kappa, out)) return false;
    }
    else
    {
        out[0] = out[1] = out[2] = 0;
        for (int s = 0; s < 4; s++)
        {
            float kappa = 0;
            if (lightOn && kind[s] == 1 + kFaceCeiling)
            {
                float o[3], d[3];
                CurveRay(c, e, (float)px + 0.5f + kCurveSubsamples[s][0], (float)py + 0.5f + kCurveSubsamples[s][1], o, d);
                kappa = RoomCeilingPanel(*in.rc, o, d, Dx, Dy);
            }
            float rgb[3];
            if (!RoomSampleColour(c, view, in, kind[s], uu[s], vv[s], gu[s], gv[s], kappa, rgb)) return false;
            for (int ch = 0; ch < 3; ch++) out[ch] += rgb[ch] * 0.25f;
        }
    }
    const bool room = kind[0] >= 1 && kind[0] <= kRoomFaces;
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
    def("ROOM_FOOT_MIN", RoomHlslFloat(kRoomFootMin));
    def("ROOM_FOOT_MAX", RoomHlslFloat(kRoomFootMax));
    defInt("ROOM_KIND_FOOTPRINT", kRoomKindFootprint);
    defInt("ROOM_KIND_OUTSIDE", kRoomKindOutside);
    defInt("ROOM_PART_WING_L", kRoomPartWingL);
    defInt("ROOM_PART_WING_R", kRoomPartWingR);
    def("ROOM_SCREEN_PAD", RoomHlslFloat(kRoomScreenPad));
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

// ------------------------------------------------------------ the v10 eye pass
// Today's (v10) classification and eye pass, kept verbatim while the new ones replace
// them: the CPU reference of kCurveRoomV10Hlsl, which the --room-v10-eye A/B diagnostic
// and the room benchmark's case D run. The CPU tests and the GPU self-test check them.
// The calls inside are qualified (room_v10::): argument-dependent lookup would also
// find today's functions of the same signature. Remove them, with kCurveRoomV10Hlsl,
// once the headset numbers are in.
namespace room_v10
{

inline bool RoomExit(const Room& r, const float o[3], const float d[3], RoomHit& hit)
{
    if (!o || !d) return false;
    if (!RoomInside(r, o)) return false;

    float best = 3.0e38f;
    int face = -1;
    auto consider = [&](float t, int f) { if (t > 0 && t < best) { best = t; face = f; } };
    if (d[0] > 0) consider((r.X - o[0]) / d[0], kFaceRight);
    if (d[0] < 0) consider((-r.X - o[0]) / d[0], kFaceLeft);
    if (d[1] < 0) consider((r.yF - o[1]) / d[1], kFaceFloor);
    if (d[1] > 0) consider((r.yC - o[1]) / d[1], kFaceCeiling);
    if (d[2] > 0) consider((r.zB - o[2]) / d[2], kFaceBack);
    float frontPhi = 0;
    bool onArc = false;
    if (!r.curved)
    {
        if (d[2] < 0) consider((-r.g - o[2]) / d[2], kFaceFront);
    }
    else
    {
        float phi = 0, y = 0;
        if (ArcHit(r.R, r.Rg, r.phiA, 3.0e38f, o, d, &phi, &y))
        {
            // ArcHit returns the root; recover its t along the ray from x or z.
            const float hx = r.Rg * std::sin(phi), hz = r.R - r.Rg * std::cos(phi);
            const float t = std::fabs(d[0]) > std::fabs(d[2]) ? (hx - o[0]) / d[0] : (hz - o[2]) / d[2];
            if (t > 0 && t < best) { best = t; face = kFaceFront; frontPhi = phi; onArc = true; }
        }
        for (int side = -1; side <= 1; side += 2)
        {
            const float nx = -side * r.sinA, nz = r.cosA;       // the wing's normal, into the room
            const float px = side * r.xa, pz = r.za;
            const float nd = nx * d[0] + nz * d[2];
            if (!(nd < 0)) continue;
            const float t = (nx * (px - o[0]) + nz * (pz - o[2])) / nd;
            const float hx = o[0] + t * d[0];
            if (side * hx < r.xa) continue;                     // on the plane's extension, not the wing
            if (t > 0 && t < best) { best = t; face = kFaceFront; onArc = false; }
        }
    }
    if (face < 0) return false;

    const float p[3] = { o[0] + best * d[0], o[1] + best * d[1], o[2] + best * d[2] };
    const float h = r.yC - r.yF;
    hit.face = face; hit.t = best; hit.y = p[1];
    switch (face)
    {
    case kFaceFront:
        hit.s = !r.curved ? p[0] : (onArc ? r.R * frontPhi : FrontS(r, p[0]));
        hit.u = (hit.s + r.sMax) / (2.0f * r.sMax); hit.v = (r.yC - p[1]) / h; break;
    case kFaceLeft: case kFaceRight:
        hit.u = (p[2] - r.zSide) / (r.zB - r.zSide); hit.v = (r.yC - p[1]) / h; break;
    case kFaceFloor: case kFaceCeiling:
        hit.u = (p[0] + r.X) / (2.0f * r.X); hit.v = (p[2] + r.g) / (r.zB + r.g); break;
    default:
        hit.u = (p[0] + r.X) / (2.0f * r.X); hit.v = (r.yC - p[1]) / h; break;
    }
    return true;
}

inline int RoomClassify(const CurveConstants& c, const Cylinder& cyl, const Room& r, const RoomView& view,
                        const float o[3], const float d[3], float* u, float* v, float* gu, float* gv)
{
    if (!u || !v || !gu || !gv) return kRoomKindOutside;
    *u = *v = *gu = *gv = 0;
    if (!view.flatLayer)
    {
        if (CylinderHit(cyl, o, d, u, v)) return 0;
    }
    else if (d[2] < 0)
    {
        const float t = -o[2] / d[2];
        const float hx = o[0] + t * d[0], hy = o[1] + t * d[1];
        if (t > 0 && std::fabs(hx) <= 0.5f * view.W && std::fabs(hy) <= 0.5f * view.H) return kRoomKindFootprint;
    }
    RoomHit hit;
    if (!room_v10::RoomExit(r, o, d, hit)) return kRoomKindOutside;
    *u = hit.u; *v = hit.v;
    if (hit.face == kFaceFront) { *gu = hit.s / (2.0f * view.glowHalfW) + 0.5f; *gv = 0.5f - hit.y / (2.0f * view.glowHalfH); }
    (void)c;
    return 1 + hit.face;
}

inline bool RoomSampleColour(const CurveConstants& c, const RoomView& view, int kind, float u, float v, float gu, float gv,
                             const RgbaImage& picture, const RgbaImage* glow, const RoomLightmap& light, float out[3])
{
    if (!out) return false;
    if (kind == 0)
    {
        float s[4];
        if (!SampleRgba(picture, u, v, s)) return false;
        out[0] = s[0]; out[1] = s[1]; out[2] = s[2];
        return true;
    }
    if (kind == kRoomKindFootprint) { out[0] = out[1] = out[2] = 0; return true; }
    if (kind == kRoomKindOutside) { out[0] = c.world[0]; out[1] = c.world[1]; out[2] = c.world[2]; return true; }
    float L[3];
    if (!light.Sample(kind - 1, u, v, L)) return false;
    if (kind - 1 == kFaceFront && view.glowOn && glow && gu >= 0 && gu <= 1 && gv >= 0 && gv <= 1)
    {
        float g[4];
        if (!SampleRgba(*glow, gu, gv, g)) return false;
        for (int ch = 0; ch < 3; ch++) L[ch] += SrgbToLinear(g[ch]);
    }
    for (int ch = 0; ch < 3; ch++) out[ch] = LinearToSrgb(std::fmax(L[ch], 0.0f));
    return true;
}

inline bool RoomPixel(const CurveConstants& c, const Cylinder& cyl, const Room& r, const RoomView& view, int e, int px, int py,
                      const RgbaImage& picture, const RgbaImage* glow, const RoomLightmap& light, float out[3])
{
    if (!out) return false;
    if (e < 0 || e > 1 || px < 0 || py < 0 || px >= (int)c.ew || py >= (int)c.eh) return false;
    if (!view.flatLayer && !picture.data) return false;

    int kind[4];
    float uu[4], vv[4], gu[4], gv[4];
    for (int s = 0; s < 4; s++)
    {
        float o[3], d[3];
        CurveRay(c, e, (float)px + 0.5f + kCurveSubsamples[s][0], (float)py + 0.5f + kCurveSubsamples[s][1], o, d);
        kind[s] = room_v10::RoomClassify(c, cyl, r, view, o, d, &uu[s], &vv[s], &gu[s], &gv[s]);
    }
    const bool agree = kind[0] == kind[1] && kind[1] == kind[2] && kind[2] == kind[3];
    if (agree)
    {
        const float mu = (uu[0] + uu[1] + uu[2] + uu[3]) * 0.25f, mv = (vv[0] + vv[1] + vv[2] + vv[3]) * 0.25f;
        const float mgu = (gu[0] + gu[1] + gu[2] + gu[3]) * 0.25f, mgv = (gv[0] + gv[1] + gv[2] + gv[3]) * 0.25f;
        if (!room_v10::RoomSampleColour(c, view, kind[0], mu, mv, mgu, mgv, picture, glow, light, out)) return false;
    }
    else
    {
        out[0] = out[1] = out[2] = 0;
        for (int s = 0; s < 4; s++)
        {
            float rgb[3];
            if (!room_v10::RoomSampleColour(c, view, kind[s], uu[s], vv[s], gu[s], gv[s], picture, glow, light, rgb)) return false;
            for (int ch = 0; ch < 3; ch++) out[ch] += rgb[ch] * 0.25f;
        }
    }
    const bool room = kind[0] >= 1 && kind[0] <= kRoomFaces;
    if (view.dither && agree && room)
        for (int ch = 0; ch < 3; ch++) out[ch] += RoomDither(e, px, py);
    return true;
}

} // namespace room_v10
