#pragma once
#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "screen_curve.h"
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
//
// All of it is computed once per frame into a small lightmap (kRoomLightmap^2 per
// face) that both eyes sample: diffuse light does not depend on where you look from.

static const int kRoomFaces = 6;
enum RoomFace { kFaceFront = 0, kFaceLeft = 1, kFaceRight = 2, kFaceFloor = 3, kFaceCeiling = 4, kFaceBack = 5 };
static const char* const kRoomFaceNames[kRoomFaces] = { "front", "left", "right", "floor", "ceiling", "back" };
static const int kRoomLightmap = 64;            // texels per side of each face
static const int kRoomGlowBlock = 8;            // glow texels per side of one glow emitter
static const int kRoomMaxEmitters = 1024;
static const int kRoomEmitterFloat4s = 6;       // per emitter in the GPU buffer
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
    bool phiReduced = false;            // the arc was shortened to keep the viewer inside
};

// Depth of the front wall at x (the room is z >= FrontDepth).
inline float FrontDepth(const Room& r, float x)
{
    if (!r.curved) return -r.g;
    const float ax = std::fabs(x);
    if (ax <= r.xa) return r.R - std::sqrt(std::fmax(r.Rg * r.Rg - ax * ax, 0.0f));
    return r.za + (ax - r.xa) * (r.sinA / r.cosA);
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

inline void RoomSetArc(Room& r, float phiA)
{
    r.phiA = phiA;
    r.sinA = std::sin(phiA); r.cosA = std::cos(phiA);
    r.xa = r.Rg * r.sinA; r.za = r.R - r.Rg * r.cosA;
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
// for the front wall, the glow's coordinates (s, y). Returns false if the ray starts
// outside the room (it then sees the world colour).
struct RoomHit { int face = -1; float t = 0, u = 0, v = 0, s = 0, y = 0; };

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

// Surface areas, for the uniform bounce: the true front length, sides, floor, ceiling, back.
inline float RoomArea(const Room& r, int face)
{
    const float h = r.yC - r.yF;
    switch (face)
    {
    case kFaceFront: return 2.0f * (r.curved ? (r.Rg * r.phiA + (r.X - r.xa) / r.cosA) : r.X) * h;
    case kFaceLeft: case kFaceRight: return (r.zB - r.zSide) * h;
    case kFaceFloor: case kFaceCeiling: return 2.0f * r.X * (r.zB + r.g);
    case kFaceBack: return 2.0f * r.X * h;
    default: return 0;
    }
}

// Heading only: the screen's orientation with pitch and roll removed, so the floor is
// level. Looking straight up or down, the heading comes from the head's up vector.
inline XrQuaternionf YawOnly(const XrQuaternionf& q)
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    // forward = q * (0, 0, -1); up = q * (0, 1, 0)
    float fx = -(2 * (x * z + y * w)), fz = -(1 - 2 * (x * x + y * y));
    const float fy = -(2 * (y * z - x * w));
    if (fx * fx + fz * fz < 1e-8f)
    {
        const float ux = 2 * (x * y - z * w), uz = 2 * (y * z + x * w);
        const float sign = fy < 0 ? 1.0f : -1.0f;       // looking down: the top of the head points forward
        fx = sign * ux; fz = sign * uz;
    }
    const float yaw = std::atan2(-fx, -fz);
    return { 0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw) };
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
    int blocksX = 0, blocksY = 0;       // glow blocks
    int count() const { return gridX * gridY + blocksX * blocksY; }
};

inline RoomEmitterLayout RoomLayout(float W, float H, int glowW, int glowH)
{
    RoomEmitterLayout l;
    if (!(W > 0) || !(H > 0) || glowW <= 0 || glowH <= 0) return l;
    l.gridX = 16;
    l.gridY = (int)std::lround(16.0f * H / W);
    l.gridY = l.gridY < 4 ? 4 : (l.gridY > 16 ? 16 : l.gridY);
    l.glowW = glowW; l.glowH = glowH;
    l.blocksX = (glowW + kRoomGlowBlock - 1) / kRoomGlowBlock;
    l.blocksY = (glowH + kRoomGlowBlock - 1) / kRoomGlowBlock;
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
// top-left; then the glow's blocks, row by row. Glow blocks wholly inside the screen
// (the glow is zero there) or outside the room's height are marked inactive.
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
            const float u0 = (float)(bx * kRoomGlowBlock) / (float)l.glowW;
            const float u1 = (float)std::min((bx + 1) * kRoomGlowBlock, l.glowW) / (float)l.glowW;
            const float v0 = (float)(by * kRoomGlowBlock) / (float)l.glowH;
            const float v1 = (float)std::min((by + 1) * kRoomGlowBlock, l.glowH) / (float)l.glowH;
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
    float part[256][3];
    for (int t = 0; t < 256; t++)
    {
        part[t][0] = part[t][1] = part[t][2] = 0;
        for (int k = t; k < count; k += 256)
        {
            float v[3];
            item(k, v);
            part[t][0] += v[0]; part[t][1] += v[1]; part[t][2] += v[2];
        }
    }
    for (int stride = 128; stride > 0; stride >>= 1)
        for (int t = 0; t < stride; t++)
            for (int ch = 0; ch < 3; ch++) part[t][ch] += part[t + stride][ch];
    out[0] = part[0][0]; out[1] = part[0][1]; out[2] = part[0][2];
}

// Each emitter's radiance this frame, as the EMIT pass computes it. `src` is the
// captured picture (RGBA8, pitch in bytes), `glow` the 8-bit glow texture. Screen
// patches blend towards their new mean by `alpha` (1: take it); glow blocks take
// theirs (the glow is already smoothed).
inline bool RoomEmitRadiance(const RoomEmitterLayout& l, const unsigned char* src, int srcW, int srcH, int srcPitch,
                             const unsigned char* glow, int glowPitch, bool glowOn, float alpha,
                             const float table[256], std::vector<RoomEmitter>& emitters)
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
            const int gx0 = bx * kRoomGlowBlock, gy0 = by * kRoomGlowBlock;
            const int bw = std::min(kRoomGlowBlock, l.glowW - gx0), bh = std::min(kRoomGlowBlock, l.glowH - gy0);
            float sum[3];
            RoomGroupSum(bw * bh, [&](int k, float v[3])
            {
                const unsigned char* p = glow + (size_t)(gy0 + k / bw) * glowPitch + (size_t)(gx0 + k % bw) * 4;
                v[0] = table[p[0]]; v[1] = table[p[1]]; v[2] = table[p[2]];
            }, sum);
            const float count = (float)(bw * bh);
            for (int ch = 0; ch < 3; ch++) e.L[ch] = sum[ch] / count;
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

struct RoomShading
{
    float rhoWall = 0, rhoFloor = 0, rhoCeiling = 0;
    float rhoBar = 0, invArea = 0;      // mean albedo and 1 / total area, for the bounce
    float world[3] = { 0, 0, 0 };       // Dec(world colour): the house lights
};

inline RoomShading MakeRoomShading(const Room& r, int roomPercent, uint32_t worldRgb, float screenArea)
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
    return s;
}

// One lightmap texel's radiance (linear rgb), as the LIGHT pass computes it.
inline bool RoomTexel(const Room& r, const RoomShading& sh, const std::vector<RoomEmitter>& emitters, int face, int i, int j, float out[3])
{
    if (!out || !r.valid || face < 0 || face >= kRoomFaces || i < 0 || j < 0 || i >= kRoomLightmap || j >= kRoomLightmap) return false;
    float p[3], n[3];
    RoomFacePoint(r, face, ((float)i + 0.5f) / kRoomLightmap, ((float)j + 0.5f) / kRoomLightmap, p, n);
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
    const float bounceScale = sh.rhoBar < 0.999f ? sh.rhoBar * sh.invArea / (1.0f - sh.rhoBar) : 0.0f;
    for (int ch = 0; ch < 3; ch++)
        out[ch] = (rho / kRoomPi) * (E[ch] + flux[ch] * bounceScale) + shade * sh.world[ch];
    return true;
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
};

// Sample kinds for the four rays: 0 picture, 1 + face, 7 footprint, 8 outside.
static const int kRoomKindFootprint = 7, kRoomKindOutside = 8;

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
    if (!RoomExit(r, o, d, hit)) return kRoomKindOutside;
    *u = hit.u; *v = hit.v;
    if (hit.face == kFaceFront) { *gu = hit.s / (2.0f * view.glowHalfW) + 0.5f; *gv = 0.5f - hit.y / (2.0f * view.glowHalfH); }
    (void)c;
    return 1 + hit.face;
}

// One sample's colour, encoded (like the picture): the picture; black; the world
// colour; or a room surface - its lightmap radiance, plus the glow where it lies on
// the front wall (light on the wall adds: it does not hide the wall's own light).
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

// One eye-buffer pixel of the room pass: four rays as CurvedPixel; one sample when
// they agree; the dither on room surfaces.
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
        kind[s] = RoomClassify(c, cyl, r, view, o, d, &uu[s], &vv[s], &gu[s], &gv[s]);
    }
    const bool agree = kind[0] == kind[1] && kind[1] == kind[2] && kind[2] == kind[3];
    if (agree)
    {
        const float mu = (uu[0] + uu[1] + uu[2] + uu[3]) * 0.25f, mv = (vv[0] + vv[1] + vv[2] + vv[3]) * 0.25f;
        const float mgu = (gu[0] + gu[1] + gu[2] + gu[3]) * 0.25f, mgv = (gv[0] + gv[1] + gv[2] + gv[3]) * 0.25f;
        if (!RoomSampleColour(c, view, kind[0], mu, mv, mgu, mgv, picture, glow, light, out)) return false;
    }
    else
    {
        out[0] = out[1] = out[2] = 0;
        for (int s = 0; s < 4; s++)
        {
            float rgb[3];
            if (!RoomSampleColour(c, view, kind[s], uu[s], vv[s], gu[s], gv[s], picture, glow, light, rgb)) return false;
            for (int ch = 0; ch < 3; ch++) out[ch] += rgb[ch] * 0.25f;
        }
    }
    const bool room = kind[0] >= 1 && kind[0] <= kRoomFaces;
    if (view.dither && agree && room)
        for (int ch = 0; ch < 3; ch++) out[ch] += RoomDither(e, px, py);
    return true;
}
