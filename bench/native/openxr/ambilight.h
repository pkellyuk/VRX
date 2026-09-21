#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

// Ambilight: the colours at the edge of the picture spread out around the screen,
// like the bias lighting behind a television. A small glow texture covers the
// screen plus a margin on every side; the compositor stretches it (flat screen) or
// the curved-screen pass wraps it round a cylinder just behind the screen.
//
// It is built in two passes (kAmbiHlsl, compiled once with AMBI_RING for step 1):
//
//   1. The ring: kAmbiRing points evenly spaced round the screen's border, each the
//      average of a patch of the picture just inside it (kAmbiTaps^2 taps).
//   2. The glow: every glow pixel blends the WHOLE ring, each point weighted by
//      1 / (r^2 + soft^2)^1.5 for its distance r - how light from a strip along the
//      edge spreads across a wall. Right next to the edge the nearby points win, so
//      the glow follows the local colour; further out a wider stretch of the border
//      counts, so it softens with distance. The first version took the colour of the
//      single nearest border point, which pushed every feature at the edge straight
//      outwards as a line.
//
// Brightness is separate from colour: it falls off with the square of the distance
// outside the screen (measured in metres, so the corners are round, not stretched),
// reaches zero at the edge of the margin, and rises from dark over a thin bezel next
// to the screen - at full brightness against the screen the screen's edge pixels
// flickered in play. Strength scales it.
//
// The colour is stored multiplied by its alpha: what a compositor wants for a
// blended layer, and exactly right if a runtime ignores the alpha, because the world
// behind the screen is black. Each frame is blended into the previous glow, so it
// drifts with the scene instead of flickering.
//
// The taps are whole source pixels and the sums run in the same order as the
// shader's, so this CPU reference and the GPU agree to within a bit
// (SelfTestAmbilight).

static const float kAmbiMargin = 0.22f;         // glow width, as a fraction of the screen's width
static const int kAmbiRing = 256;               // points round the border
static const uint32_t kAmbiRingMax = 256;       // the shader's group-shared copy of the ring
static const int kAmbiTaps = 4;                 // per ring point: kAmbiTaps x kAmbiTaps
static const float kAmbiDepth = 0.06f;          // how far inside the edge a ring point looks, fraction of the screen's smaller side
static const float kAmbiSoft = 0.03f;           // softening distance, fraction of the screen's width
static const float kAmbiBlend = 0.12f;          // per frame, towards the new glow
static const float kAmbiBezel = 0.08f;          // dark rise next to the screen, fraction of the margin
static const float kAmbiBehind = 0.02f;         // metres the glow sits behind the screen
static const int kAmbiDefaultStrength = 85;     // percent

struct AmbiConstants                            // must match cbuffer C in kAmbiHlsl
{
    uint32_t gw = 0, gh = 0;                    // glow texture size
    uint32_t srcW = 0, srcH = 0;                // source picture size
    float rectW = 0, rectH = 0;                 // the glow's rectangle in metres (screen + margin each side)
    float screenW = 0, screenH = 0;             // the screen in metres
    float marginM = 0;                          // metres of glow beyond the screen
    float intensity = 0;                        // brightness next to the screen (the strength)
    float blend = 1;                            // 1 = no temporal smoothing
    uint32_t reset = 1;                         // 1: ignore what the glow texture already holds
    float soft = 0;                             // metres
    float bezel = 0;                            // fraction of the margin
    uint32_t ringN = kAmbiRing;                 // at most kAmbiRingMax: the shader keeps the ring in group-shared memory
    uint32_t pad = 0;
};

struct AmbiRingPoint
{
    float rgb[3] = { 0, 0, 0 };
    float x = 0, y = 0;                         // metres from the middle of the screen, y up
};

inline bool AmbiConstantsUsable(const AmbiConstants& c)
{
    if (c.gw == 0 || c.gh == 0 || c.srcW == 0 || c.srcH == 0) return false;
    if (c.ringN == 0 || c.ringN > kAmbiRingMax) return false;
    if (!(c.screenW > 0) || !(c.screenH > 0) || !(c.marginM > 0)) return false;
    if (!(c.rectW > c.screenW) || !(c.rectH > c.screenH)) return false;
    return true;
}

// Where ring point i sits (clockwise from the top-left corner), and the directions
// along the edge and into the picture there.
inline void AmbiRingPlace(const AmbiConstants& c, uint32_t i, float* x, float* y, float* tx, float* ty, float* nx, float* ny)
{
    if (!x || !y || !tx || !ty || !nx || !ny) return;
    const float W = c.screenW, H = c.screenH;
    float s = ((float)i + 0.5f) / (float)c.ringN * (2.0f * (W + H));
    if (s < W) { *x = -0.5f * W + s; *y = 0.5f * H; *tx = 1; *ty = 0; *nx = 0; *ny = -1; return; }
    s -= W;
    if (s < H) { *x = 0.5f * W; *y = 0.5f * H - s; *tx = 0; *ty = -1; *nx = -1; *ny = 0; return; }
    s -= H;
    if (s < W) { *x = 0.5f * W - s; *y = -0.5f * H; *tx = -1; *ty = 0; *nx = 0; *ny = 1; return; }
    s -= W;
    *x = -0.5f * W; *y = -0.5f * H + s; *tx = 0; *ty = 1; *nx = 1; *ny = 0;
}

// Step 1 for one point: the average of kAmbiTaps x kAmbiTaps source pixels in a
// patch one ring spacing wide along the edge and kAmbiDepth deep into the picture.
inline bool BuildRingPoint(const AmbiConstants& c, const unsigned char* src, int srcPitch, uint32_t i, AmbiRingPoint& out)
{
    if (!src) return false;
    if (!AmbiConstantsUsable(c) || i >= c.ringN) return false;
    if (srcPitch < (int)c.srcW * 4) return false;

    float x, y, tx, ty, nx, ny;
    AmbiRingPlace(c, i, &x, &y, &tx, &ty, &nx, &ny);
    const float spacing = 2.0f * (c.screenW + c.screenH) / (float)c.ringN;
    const float depth = kAmbiDepth * (c.screenW < c.screenH ? c.screenW : c.screenH);
    float sum[3] = { 0, 0, 0 };
    for (int j = 0; j < kAmbiTaps; j++)
        for (int k = 0; k < kAmbiTaps; k++)
        {
            const float along = (((float)k + 0.5f) / (float)kAmbiTaps - 0.5f) * spacing;
            const float in = ((float)j + 0.5f) / (float)kAmbiTaps * depth;
            const float qx = x + tx * along + nx * in, qy = y + ty * along + ny * in;
            int px = (int)std::floor((qx / c.screenW + 0.5f) * (float)c.srcW - 0.5f + 0.5f);
            int py = (int)std::floor((0.5f - qy / c.screenH) * (float)c.srcH - 0.5f + 0.5f);
            px = px < 0 ? 0 : (px > (int)c.srcW - 1 ? (int)c.srcW - 1 : px);
            py = py < 0 ? 0 : (py > (int)c.srcH - 1 ? (int)c.srcH - 1 : py);
            const unsigned char* p = src + (size_t)py * srcPitch + (size_t)px * 4;
            for (int ch = 0; ch < 3; ch++) sum[ch] += p[ch] / 255.0f;
        }
    const float taps = (float)(kAmbiTaps * kAmbiTaps);
    for (int ch = 0; ch < 3; ch++) out.rgb[ch] = sum[ch] / taps;
    out.x = x; out.y = y;
    return true;
}

// Step 2 for one glow pixel, premultiplied RGBA 0..1. `ring` holds c.ringN points.
inline bool AmbilightPixel(const AmbiConstants& c, const AmbiRingPoint* ring, int gx, int gy, float rgba[4])
{
    if (!ring || !rgba) return false;
    if (!AmbiConstantsUsable(c)) return false;
    if (gx < 0 || gy < 0 || gx >= (int)c.gw || gy >= (int)c.gh) return false;

    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    const float px = (((float)gx + 0.5f) / (float)c.gw - 0.5f) * c.rectW;
    const float py = (0.5f - ((float)gy + 0.5f) / (float)c.gh) * c.rectH;
    const float ox = std::fabs(px) - 0.5f * c.screenW, oy = std::fabs(py) - 0.5f * c.screenH;
    const float dx = ox > 0 ? ox : 0.0f, dy = oy > 0 ? oy : 0.0f;
    const float dist = std::sqrt(dx * dx + dy * dy) / c.marginM;
    if (dist <= 0.0f || dist >= 1.0f) return true;          // behind the screen, or past the glow

    float a = (1.0f - dist) * (1.0f - dist) * c.intensity;
    if (c.bezel > 0.0f)
    {
        const float tb = dist / c.bezel < 1.0f ? dist / c.bezel : 1.0f;
        a *= tb * tb * (3.0f - 2.0f * tb);
    }
    const float soft2 = c.soft * c.soft;
    float sumW = 0, sum[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < c.ringN; i++)
    {
        const float rx = px - ring[i].x, ry = py - ring[i].y;
        const float r2 = rx * rx + ry * ry + soft2;
        const float w = 1.0f / (r2 * std::sqrt(r2));
        sumW += w;
        for (int ch = 0; ch < 3; ch++) sum[ch] += w * ring[i].rgb[ch];
    }
    if (!(sumW > 0)) return true;
    for (int ch = 0; ch < 3; ch++) rgba[ch] = (sum[ch] / sumW) * a;
    rgba[3] = a;
    return true;
}

// The whole glow texture as the shader writes it with reset = 1 (no history).
inline bool AmbilightReference(const AmbiConstants& c, const unsigned char* src, int srcPitch,
                              unsigned char* out, int outPitch)
{
    if (!src || !out) return false;
    if (!AmbiConstantsUsable(c)) return false;
    if (outPitch < (int)c.gw * 4) return false;

    AmbiRingPoint ring[kAmbiRingMax];
    for (uint32_t i = 0; i < c.ringN; i++)
        if (!BuildRingPoint(c, src, srcPitch, i, ring[i])) return false;
    for (int gy = 0; gy < (int)c.gh; gy++)
        for (int gx = 0; gx < (int)c.gw; gx++)
        {
            float rgba[4] = { 0, 0, 0, 0 };
            if (!AmbilightPixel(c, ring, gx, gy, rgba)) return false;
            unsigned char* o = out + (size_t)gy * outPitch + (size_t)gx * 4;
            for (int ch = 0; ch < 4; ch++)
            {
                const float value = rgba[ch] < 0 ? 0.0f : (rgba[ch] > 1 ? 1.0f : rgba[ch]);
                o[ch] = (unsigned char)lroundf(value * 255.0f);
            }
        }
    return true;
}
