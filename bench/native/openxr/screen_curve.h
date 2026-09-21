#pragma once
#include <cmath>
#include <cstdint>

// Curved screen: a real cylinder, drawn for each eye from that eye's tracked
// position by a ray-cast pass (kCurveHlsl in xrapp5.cpp) into eye buffers that
// are submitted as a projection layer.
//
// Why not the flat quad: the compositor draws a quad layer as a flat rectangle,
// so anything done inside the picture - moving columns, adding the curve's
// disparity - still sits inside a flat frame whose edges say "flat, 3 m away" to
// both eyes. In play that read as the game squashed into the middle of a flat
// display. SteamVR's OpenXR runtime offers no cylinder layer (2.17.10: 43
// extensions, no XR_KHR_composition_layer_cylinder), so VRX renders the
// cylinder itself: outline, perspective, both eyes' views and head-movement
// parallax all come out of the geometry.
//
// Screen-local frame: origin at the middle of the screen, x right, y up, z
// towards the viewer (the quad's normal). The cylinder's axis is vertical and its
// centre of curvature sits `radius` in front of the middle of the screen, so the
// surface is
//
//   P(phi, y) = (R sin phi, y, R (1 - cos phi)),   |phi| <= halfWrap
//
// with the arc length R * phi measuring the picture sideways: the screen keeps
// the width (and height) the player chose, it just bends round them.
//
// Each eye's picture is still that eye's depth-warped image, so the picture's own
// depth rides on top of the curved surface.

static const float kCurveMaxWrap = 70.0f * 3.14159265358979f / 180.0f;    // 100%
// A wide screen close by would otherwise wrap past the viewer's head: the wrap is
// reduced until the edges stay at least this fraction of the distance away.
static const float kCurveMinDepthFraction = 0.55f;

// Sagitta of an arc of `wrap` radians whose ARC LENGTH is `width`: how far the
// edges come towards the viewer.
inline float CurveSag(float width, float wrap)
{
    if (!(wrap > 0) || !(width > 0)) return 0;
    const float radius = width / wrap;
    return radius * (1.0f - std::cos(wrap * 0.5f));
}

// The wrap to use: `fraction` of kCurveMaxWrap, reduced if that would bring the
// edges nearer than kCurveMinDepthFraction * distance.
inline float CurveWrap(float width, float distance, float fraction)
{
    if (!(fraction > 0) || !(width > 0) || !(distance > 0)) return 0;
    if (!std::isfinite(fraction) || !std::isfinite(width) || !std::isfinite(distance)) return 0;

    const float wanted = (fraction > 1 ? 1.0f : fraction) * kCurveMaxWrap;
    const float maxSag = distance * (1.0f - kCurveMinDepthFraction);
    if (CurveSag(width, wanted) <= maxSag) return wanted;

    // The sag rises monotonically with the wrap: bisect for the largest that fits.
    float lo = 0, hi = wanted;
    for (int i = 0; i < 40; i++)
    {
        const float mid = (lo + hi) * 0.5f;
        if (CurveSag(width, mid) <= maxSag) lo = mid; else hi = mid;
    }
    return lo;
}

struct Cylinder
{
    float radius = 0;           // metres
    float halfWrap = 0;         // radians either side of the middle
    float halfWidth = 0;        // metres along the surface
    float halfHeight = 0;       // metres
    bool curved = false;        // false: flat - use the quad layers instead
};

// Returns false only on unusable inputs. fraction 0 gives a flat (not curved) result.
inline bool BuildCylinder(float width, float height, float distance, float fraction, Cylinder& out)
{
    if (!(width > 0) || !std::isfinite(width)) return false;
    if (!(height > 0) || !std::isfinite(height)) return false;
    if (!(distance > 0) || !std::isfinite(distance)) return false;
    if (!std::isfinite(fraction) || fraction < 0) return false;

    out = Cylinder();
    out.halfWidth = width * 0.5f;
    out.halfHeight = height * 0.5f;
    const float wrap = CurveWrap(width, distance, fraction);
    if (!(wrap > 0)) return true;
    out.halfWrap = wrap * 0.5f;
    out.radius = width / wrap;
    out.curved = true;
    return true;
}

// Where the ray o + t d (t > 0, screen-local) meets the screen. tu, tv run 0..1
// across the picture (tv downwards, like texture rows). The quadratic uses the
// stable form, and c is expanded so that a gentle curve's huge radius does not
// cancel away its precision.
inline bool CylinderHit(const Cylinder& cyl, const float o[3], const float d[3], float* tu, float* tv)
{
    if (!o || !d || !tu || !tv) return false;
    if (!cyl.curved) return false;

    const float R = cyl.radius;
    const float a = d[0] * d[0] + d[2] * d[2];
    if (!(a > 1e-12f)) return false;
    const float b = 2.0f * (o[0] * d[0] + (o[2] - R) * d[2]);
    const float c = o[0] * o[0] + o[2] * o[2] - 2.0f * o[2] * R;
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0) return false;
    const float q = -0.5f * (b + (b < 0 ? -std::sqrt(disc) : std::sqrt(disc)));
    float t0 = q / a, t1 = (q != 0) ? c / q : t0;
    if (t0 > t1) { const float swap = t0; t0 = t1; t1 = swap; }

    for (int i = 0; i < 2; i++)
    {
        const float t = i == 0 ? t0 : t1;
        if (!(t > 0)) continue;
        const float hx = o[0] + t * d[0], hy = o[1] + t * d[1], hz = o[2] + t * d[2];
        const float toward = R - hz;             // > 0 on the screen's side of the circle
        if (!(toward > 0)) continue;
        const float phi = std::atan2(hx, toward);
        if (std::fabs(phi) > cyl.halfWrap || std::fabs(hy) > cyl.halfHeight) continue;
        *tu = (R * phi) / (2.0f * cyl.halfWidth) + 0.5f;
        *tv = 0.5f - hy / (2.0f * cyl.halfHeight);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the pass
struct CurveEye                         // 5 x float4 in the constant buffer
{
    float origin[4];                    // the eye, screen-local (w unused)
    float row0[4], row1[4], row2[4];    // screen-local <- view-space rotation, by rows (w unused)
    float tanL, tanR, tanU, tanD;       // the view's field of view
};

struct CurveConstants                   // must match cbuffer C in kCurveHlsl (52 DWORDs)
{
    uint32_t ew = 0, eh = 0;            // eye buffer size
    uint32_t glowOn = 0, pad0 = 0;
    float radius = 0, halfWrap = 0, halfWidth = 0, halfHeight = 0;
    float glowHalfW = 0, glowHalfH = 0, glowZ = 0, pad1 = 0;
    CurveEye eye[2] = {};
};
static_assert(sizeof(CurveConstants) == 52 * 4, "CurveConstants must stay 52 DWORDs (root constants)");

// Four samples per pixel on a rotated grid, for a smooth outline.
static const float kCurveSubsamples[4][2] = { { -0.375f, -0.125f }, { 0.125f, -0.375f }, { 0.375f, 0.125f }, { -0.125f, 0.375f } };

// The ray through (px + sx, py + sy) of eye `e`, screen-local.
inline void CurveRay(const CurveConstants& c, int e, float px, float py, float o[3], float d[3])
{
    const CurveEye& v = c.eye[e];
    const float tx = v.tanL + px / (float)c.ew * (v.tanR - v.tanL);
    const float ty = v.tanU + py / (float)c.eh * (v.tanD - v.tanU);
    o[0] = v.origin[0]; o[1] = v.origin[1]; o[2] = v.origin[2];
    d[0] = v.row0[0] * tx + v.row0[1] * ty - v.row0[2];
    d[1] = v.row1[0] * tx + v.row1[1] * ty - v.row1[2];
    d[2] = v.row2[0] * tx + v.row2[1] * ty - v.row2[2];
}

// Where the ray meets the glow, a flat rectangle just behind the middle of the screen.
inline bool GlowHit(const CurveConstants& c, const float o[3], const float d[3], float* gu, float* gv)
{
    if (!o || !d || !gu || !gv) return false;
    if (c.glowOn == 0 || !(d[2] < 0)) return false;

    const float t = (c.glowZ - o[2]) / d[2];
    if (!(t > 0)) return false;
    const float gx = o[0] + t * d[0], gy = o[1] + t * d[1];
    *gu = gx / (2.0f * c.glowHalfW) + 0.5f;
    *gv = 0.5f - gy / (2.0f * c.glowHalfH);
    return *gu >= 0 && *gu <= 1 && *gv >= 0 && *gv <= 1;
}

// An RGBA8 image and bilinear sampling with clamped edges - what SampleLevel does
// with a linear, clamping sampler, so the CPU reference below matches the shader
// to within the GPU's filter-weight precision.
struct RgbaImage
{
    const unsigned char* data = nullptr;
    int w = 0, h = 0, pitch = 0;
};

inline bool SampleRgba(const RgbaImage& img, float u, float v, float out[3])
{
    if (!img.data || !out || img.w <= 0 || img.h <= 0 || img.pitch < img.w * 4) return false;
    const float x = u * (float)img.w - 0.5f, y = v * (float)img.h - 0.5f;
    const float fx0 = std::floor(x), fy0 = std::floor(y);
    const float fx = x - fx0, fy = y - fy0;
    auto clampi = [](int i, int n) { return i < 0 ? 0 : (i > n - 1 ? n - 1 : i); };
    const int x0 = clampi((int)fx0, img.w), x1 = clampi((int)fx0 + 1, img.w);
    const int y0 = clampi((int)fy0, img.h), y1 = clampi((int)fy0 + 1, img.h);
    for (int ch = 0; ch < 3; ch++)
    {
        auto at = [&](int xi, int yi) { return img.data[(size_t)yi * img.pitch + (size_t)xi * 4 + ch] / 255.0f; };
        const float top = at(x0, y0) + fx * (at(x1, y0) - at(x0, y0));
        const float bot = at(x0, y1) + fx * (at(x1, y1) - at(x0, y1));
        out[ch] = top + fy * (bot - top);
    }
    return true;
}

// One eye-buffer pixel, as kCurveHlsl computes it: four rays; one sample when they
// all agree (inside the picture, inside the glow, or on nothing), four at an edge.
inline bool CurvedPixel(const CurveConstants& c, int e, int px, int py, const Cylinder& cyl,
                        const RgbaImage& picture, const RgbaImage* glow, float out[3])
{
    if (!out) return false;
    if (e < 0 || e > 1 || px < 0 || py < 0 || px >= (int)c.ew || py >= (int)c.eh) return false;
    if (!picture.data) return false;

    int kind[4];
    float uu[4], vv[4];
    for (int s = 0; s < 4; s++)
    {
        float o[3], d[3];
        CurveRay(c, e, (float)px + 0.5f + kCurveSubsamples[s][0], (float)py + 0.5f + kCurveSubsamples[s][1], o, d);
        if (CylinderHit(cyl, o, d, &uu[s], &vv[s])) kind[s] = 0;
        else if (glow && GlowHit(c, o, d, &uu[s], &vv[s])) kind[s] = 1;
        else kind[s] = 2;
    }
    out[0] = out[1] = out[2] = 0;
    if (kind[0] == kind[1] && kind[1] == kind[2] && kind[2] == kind[3])
    {
        if (kind[0] == 2) return true;
        const float mu = (uu[0] + uu[1] + uu[2] + uu[3]) * 0.25f, mv = (vv[0] + vv[1] + vv[2] + vv[3]) * 0.25f;
        return SampleRgba(kind[0] == 0 ? picture : *glow, mu, mv, out);
    }
    for (int s = 0; s < 4; s++)
    {
        if (kind[s] == 2) continue;
        float rgb[3];
        if (!SampleRgba(kind[s] == 0 ? picture : *glow, uu[s], vv[s], rgb)) return false;
        for (int ch = 0; ch < 3; ch++) out[ch] += rgb[ch] * 0.25f;
    }
    return true;
}
