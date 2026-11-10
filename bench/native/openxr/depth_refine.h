#pragma once
// Depth sharpened by the picture (XPOLISH.md, "Depth edges on the picture's edges").
//
// The depth map is much coarser than the picture (686x392 for a 1920x1080 picture) and
// the model's edges are soft: far to near over about 3 depth pixels. Scaled up to the
// picture by plain blending, an object's edge in the depth is a ramp some 8 picture
// pixels wide that seldom sits exactly on the object's edge in the picture. Everything
// on that ramp is moved by an in-between amount: slanted edges, halos, pieces of
// background that travel with an object.
//
// Here each picture pixel takes its depth from the depth samples around it, weighted by
//  - how close the sample is,
//  - how alike the picture is there: the picture on the depth grid (each depth sample's
//    own patch of picture, averaged) compared with this pixel's colour. A sample whose
//    patch straddles an edge is a mix of both sides and is like neither,
//  - how flat the depth is around the sample: a sample on a ramp is the model's
//    in-between value, and a pixel is better served by a sample of its own colour a
//    little further off, where the depth has settled.
// A pixel with nothing alike around it (busy texture) keeps the plain blend. The depth
// edge then falls on the picture's edge, at the picture's resolution, in every
// direction. Depth is never invented: a part the model did not see at all stays unseen.
//
// The guide: one uint32 per depth sample, the picture's colour there (R in the low
// byte, as kGridHlsl writes it) with the flatness in the top byte (MarkFlat).
//
// RefineDepth is the CPU reference; the shader (kRefineHlsl) does the same sums.
// Tested by playback_test.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <execution>
#include <numeric>
#include <vector>

#include "thin_parts.h"     // thin::NearAt: the plain blend, as the warp reads the depth

namespace refine
{
static const int REACH = 3;                 // depth pixels each way that a picture pixel looks (7 x 7 samples)
static const float SPREAD = 1.5f;           // depth pixels: how fast a sample's weight falls with distance
static const float ALIKE = 20.0f;           // of 255: the colour difference at which a sample's weight has fallen to 0.6
static const float FLAT = 0.04f;            // nearness: the depth range around a sample (3 x 3) at which it counts as on a ramp
static const float FLAT_FLOOR = 0.03f;      // the least weight a sample on a ramp keeps (a thin object is all ramp)
static const int WIDEN = 2;                 // picture pixels: how far the near side is widened afterwards (WidenFine)
static const float PLAIN = 0.02f;          // the weight of the plain blend: what a pixel with nothing alike around it gets

struct Params
{
    int reach = REACH;
    float spread = SPREAD, alike = ALIKE, flat = FLAT, flatFloor = FLAT_FLOOR, plain = PLAIN;
};

// The picture (cw x ch, tight RGBA rows) averaged onto the dw x dh depth grid: each
// sample's own patch. What kGridHlsl makes on the GPU; here for --dump and the tests.
inline std::vector<uint32_t> BoxGuide(const unsigned char* rgba, int cw, int ch, int dw, int dh)
{
    std::vector<uint32_t> guide;
    if (!rgba || cw <= 0 || ch <= 0 || dw <= 0 || dh <= 0) return guide;
    guide.resize((size_t)dw * dh);
    for (int y = 0; y < dh; y++)
    {
        const int y0 = (int)((int64_t)y * ch / dh), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * ch / dh));
        for (int x = 0; x < dw; x++)
        {
            const int x0 = (int)((int64_t)x * cw / dw), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * cw / dw));
            uint32_t sum[3] = { 0, 0, 0 };
            for (int py = y0; py < y1 && py < ch; py++)
                for (int px = x0; px < x1 && px < cw; px++)
                {
                    const unsigned char* p = rgba + ((size_t)py * cw + px) * 4;
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                }
            const uint32_t n = (uint32_t)std::max(1, (std::min(y1, ch) - y0) * (std::min(x1, cw) - x0));
            guide[(size_t)y * dw + x] = ((sum[0] + n / 2) / n) | (((sum[1] + n / 2) / n) << 8) | (((sum[2] + n / 2) / n) << 16);
        }
    }
    return guide;
}

// Writes each sample's flatness into the guide's top byte: 255 where the depth around
// it (3 x 3) is level, falling to FLAT_FLOOR on a ramp.
inline void MarkFlat(std::vector<uint32_t>& guide, const float* near01, int dw, int dh, const Params& params = Params())
{
    if (!near01 || dw <= 0 || dh <= 0 || guide.size() != (size_t)dw * dh) return;
    std::vector<int> rows((size_t)dh);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y)
    {
        const int ya = std::max(0, y - 1), yb = std::min(dh - 1, y + 1);
        for (int x = 0; x < dw; x++)
        {
            const int xa = std::max(0, x - 1), xb = std::min(dw - 1, x + 1);
            float lo = 1e9f, hi = -1e9f;
            for (int sy = ya; sy <= yb; sy++)
                for (int sx = xa; sx <= xb; sx++)
                {
                    const float v = near01[(size_t)sy * dw + sx];
                    lo = std::min(lo, v); hi = std::max(hi, v);
                }
            const float r = (hi - lo) / params.flat;
            const float flat = std::max(params.flatFloor, std::exp(-r * r));
            uint32_t& g = guide[(size_t)y * dw + x];
            g = (g & 0x00FFFFFFu) | ((uint32_t)std::lround(std::min(flat, 1.0f) * 255.0f) << 24);
        }
    });
}

// The nearness of picture pixel (x, y). near01, guide: dw x dh. rgb: this pixel's colour, 0..255.
inline float RefineAt(const float* near01, const uint32_t* guide, int dw, int dh, int x, int y, int cw, int ch,
    float r, float g, float b, const Params& params)
{
    const float fx = ((float)x + 0.5f) * (float)dw / (float)cw - 0.5f, fy = ((float)y + 0.5f) * (float)dh / (float)ch - 0.5f;
    const int cx = (int)std::floor(fx + 0.5f), cy = (int)std::floor(fy + 0.5f);
    const float kSpread = 0.5f / (params.spread * params.spread), kAlike = 0.5f / (params.alike * params.alike);
    float sum = 0, weights = 0;
    for (int oy = -params.reach; oy <= params.reach; oy++)
    {
        const int sy = std::clamp(cy + oy, 0, dh - 1);
        const float dy = (float)(cy + oy) - fy;
        for (int ox = -params.reach; ox <= params.reach; ox++)
        {
            const int sx = std::clamp(cx + ox, 0, dw - 1);
            const float dx = (float)(cx + ox) - fx;
            const uint32_t gv = guide[(size_t)sy * dw + sx];
            const float dr = r - (float)(gv & 255), dg = g - (float)((gv >> 8) & 255), db = b - (float)((gv >> 16) & 255);
            const float w = std::exp(-(dx * dx + dy * dy) * kSpread - (dr * dr + dg * dg + db * db) * kAlike) * ((float)(gv >> 24) * (1.0f / 255.0f));
            sum += w * near01[(size_t)sy * dw + sx];
            weights += w;
        }
    }
    const float plain = thin::NearAt(near01, dw, dh, (float)x, (float)y, cw, ch);
    return (sum + params.plain * plain) / (weights + params.plain);
}

// The whole map: cw x ch nearness for a cw x ch picture (tight RGBA rows). The guide
// must carry the flatness (MarkFlat). Rows in parallel.
inline std::vector<float> RefineDepth(const float* near01, const uint32_t* guide, int dw, int dh,
    const unsigned char* rgba, int cw, int ch, const Params& params = Params())
{
    std::vector<float> out;
    if (!near01 || !guide || !rgba || dw <= 0 || dh <= 0 || cw <= 0 || ch <= 0) return out;
    out.resize((size_t)cw * ch);
    std::vector<int> rows((size_t)ch);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y)
    {
        const unsigned char* p = rgba + (size_t)y * cw * 4;
        float* o = out.data() + (size_t)y * cw;
        for (int x = 0; x < cw; x++, p += 4)
            o[x] = RefineAt(near01, guide, dw, dh, x, y, cw, ch, (float)p[0], (float)p[1], (float)p[2], params);
    });
    return out;
}

// The picture's own edges are soft: an object's outline is 2 or 3 pixels of in-between
// colour, like neither side, and each of those pixels would fall to one side or the
// other on its own - a dotted fringe. The near side is widened by `radius` picture
// pixels in every direction (a running maximum), so the fringe travels with its object
// and what is uncovered beside it is clean background.
inline void WidenFine(std::vector<float>& fine, int cw, int ch, int radius)
{
    if (radius <= 0 || cw <= 0 || ch <= 0 || fine.size() != (size_t)cw * ch) return;
    std::vector<float> across(fine.size());
    std::vector<int> rows((size_t)ch);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y)
    {
        const float* in = fine.data() + (size_t)y * cw;
        float* o = across.data() + (size_t)y * cw;
        for (int x = 0; x < cw; x++)
        {
            float m = in[x];
            for (int i = std::max(0, x - radius); i <= std::min(cw - 1, x + radius); i++) m = std::max(m, in[i]);
            o[x] = m;
        }
    });
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y)
    {
        float* o = fine.data() + (size_t)y * cw;
        for (int x = 0; x < cw; x++)
        {
            float m = across[(size_t)y * cw + x];
            for (int i = std::max(0, y - radius); i <= std::min(ch - 1, y + radius); i++) m = std::max(m, across[(size_t)i * cw + x]);
            o[x] = m;
        }
    });
}
}
