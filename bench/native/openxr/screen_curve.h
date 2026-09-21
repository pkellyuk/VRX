#pragma once
#include <cmath>
#include <vector>

// Curved screen geometry, shared by the warp shader and its CPU reference.
//
// The compositor gives us one FLAT quad at distance D. A curved screen is a
// cylinder whose axis is vertical and whose concave side faces the viewer, so
// this header works out what to draw on that flat quad to make the picture look
// like it is painted on the cylinder:
//
//   content coordinate u  : metres ALONG the screen surface, 0 in the middle
//   phi = u / R           : the angle it sits at (R = radius of curvature)
//   X(u) = R sin phi      : where it really is, sideways, in metres
//   Z(u) = D - R(1-cos phi): and how far away - the edges come TOWARDS the viewer
//
// Perspective then puts it on the quad plane at P(u) = X(u) * D / Z(u), and one
// eye at lateral offset E sees it a further E(1 - D/Z) sideways from there -
// which is exactly the warp's existing disparity term with invZ = 1/Z - 1/D, so
// the curve costs the shader one extra add per pixel.
//
// P(edge) is wider than half the screen (a wrapped screen fills more of your
// view), so everything is scaled by k = (width/2) / P(width/2): the quad keeps
// the width the player asked for and the picture is compressed towards the
// middle instead of being clipped at the edges. Vertically the same perspective
// divide applies, so a column at Z is magnified by D/Z; after the same k and a
// quad made (k * D/Z at the edge) taller, that leaves each column scaled by
// vertMag = (D/Z) / (D/Z at the edge) <= 1. The middle of the picture therefore
// stops a little short of the top and bottom of the quad - those pixels are
// transparent, which is what makes the outline bow outwards like a real curved
// screen seen from the middle seat.
//
// Everything is computed once per column on the CPU so that the shader and the
// reference warp use bit-identical numbers (SelfTestWarp compares them).

struct CurveColumn
{
    float destBase = 0;     // dest column this source column lands on (fractional)
    float invZ = 0;         // 1/Z(u) - 1/D: the curve's own disparity, per eye metre
    float vertMag = 1;      // vertical magnification of this column (<= 1)
};

struct CurveTable
{
    std::vector<CurveColumn> col;    // one per colour column
    float heightScale = 1;           // multiply the quad's height by this
    float wrapRadians = 0;           // arc actually used (may be clamped, see below)
    bool curved = false;             // false: flat, every column is the identity
};

// 100% wrap. A curved monitor is around 40 degrees (1000R at 700 mm wide), so
// this covers "gentle" through to "wrapped further than any real screen".
static const float kCurveMaxWrap = 70.0f * 3.14159265358979f / 180.0f;

// The sag (how far the edges come forward) may never approach the viewer: a
// 10 m wide screen at 1 m would otherwise wrap past their head. The wrap is
// reduced until the edges stay at least this fraction of the distance away.
static const float kCurveMinDepthFraction = 0.55f;

// Sagitta of an arc of `wrap` radians whose ARC LENGTH is `width`.
inline float CurveSag(float width, float wrap)
{
    if (!(wrap > 0) || !(width > 0)) return 0;
    const float radius = width / wrap;
    return radius * (1.0f - std::cos(wrap * 0.5f));
}

// The wrap to actually use: `fraction` of kCurveMaxWrap, reduced if that would
// bring the edges closer than kCurveMinDepthFraction * distance to the viewer.
inline float CurveWrap(float width, float distance, float fraction)
{
    if (!(fraction > 0) || !(width > 0) || !(distance > 0)) return 0;
    if (!std::isfinite(fraction) || !std::isfinite(width) || !std::isfinite(distance)) return 0;

    const float wanted = (fraction > 1 ? 1.0f : fraction) * kCurveMaxWrap;
    const float maxSag = distance * (1.0f - kCurveMinDepthFraction);
    if (CurveSag(width, wanted) <= maxSag) return wanted;

    // Sag rises monotonically with the wrap, so bisect for the largest wrap that fits.
    float lo = 0, hi = wanted;
    for (int i = 0; i < 40; i++)
    {
        const float mid = (lo + hi) * 0.5f;
        if (CurveSag(width, mid) <= maxSag) lo = mid; else hi = mid;
    }
    return lo;
}

// Per-column geometry for a `cw` pixel wide colour image showing a screen
// `width` metres wide (along its surface) at `distance` metres, curved by
// `fraction` of the maximum wrap. Returns false only on unusable inputs; a
// fraction of 0 gives the flat identity table, which every caller may use.
inline bool BuildCurveTable(int cw, float width, float distance, float fraction, CurveTable& out)
{
    if (cw <= 0) return false;
    if (!(width > 0) || !std::isfinite(width)) return false;
    if (!(distance > 0) || !std::isfinite(distance)) return false;
    if (!std::isfinite(fraction) || fraction < 0) return false;

    out.col.assign((size_t)cw, CurveColumn());
    out.heightScale = 1;
    out.wrapRadians = CurveWrap(width, distance, fraction);
    out.curved = out.wrapRadians > 0;
    if (!out.curved)
    {
        for (int x = 0; x < cw; x++) out.col[(size_t)x] = { (float)x, 0.0f, 1.0f };
        return true;
    }

    const float radius = width / out.wrapRadians;
    const float pxPerMetre = (float)cw / width;
    const float phiEdge = out.wrapRadians * 0.5f;
    const float zEdge = distance - radius * (1.0f - std::cos(phiEdge));
    const float projEdge = radius * std::sin(phiEdge) * distance / zEdge;
    const float k = (width * 0.5f) / projEdge;
    const float magEdge = distance / zEdge;
    out.heightScale = k * magEdge;

    for (int x = 0; x < cw; x++)
    {
        const float u = ((float)x + 0.5f - (float)cw * 0.5f) / pxPerMetre;
        const float phi = u / radius;
        const float z = distance - radius * (1.0f - std::cos(phi));
        const float proj = radius * std::sin(phi) * distance / z;
        CurveColumn c;
        c.destBase = (float)cw * 0.5f + k * proj * pxPerMetre - 0.5f;
        c.invZ = 1.0f / z - 1.0f / distance;
        c.vertMag = (distance / z) / magEdge;
        out.col[(size_t)x] = c;
    }
    return true;
}
