#pragma once
// A background layer for what a near object uncovers (XPOLISH.md, "Behind near objects").
//
// When a near object is moved sideways for an eye, a strip of what was behind it comes
// into view on one side, in that eye only. The picture has nothing there, so the warp
// made something up row by row: the background beside the strip, mirrored (FillHole,
// xr_common.h). Each row is filled on its own, so the strip can speckle from row to
// row, and it is sharp picture that one eye sees and the other does not.
//
// Here the picture behind near objects is made once, for both eyes: wherever the depth
// is clearly nearer than the farthest thing a short way along its row, the picture is
// a near object's and is replaced by the background beside it: the nearby samples on its
// rows that are no nearer than the farthest thing there, the closer ones counting more. A strip that an eye uncovers then shows that
// layer at the place the background would really come from, at the background's depth.
// So the strip is the same hidden surface for both eyes, the rows agree, behind a thin
// object it is what is on its two sides, and it is soft - a soft patch in one eye
// gives way to the other eye's sharp picture far more readily than a wrong sharp one.
//
// The layer is on the depth grid (it is made from the picture on the depth grid and the
// depth), one uint32 per sample: R, G, B in the low bytes as kGridHlsl writes them, and
// in the top byte how much of it is the picture's own (255) rather than filled in (0).
// Where it is the picture's own the warp shows the sharp picture itself.
//
// BuildBackLayer is on the CPU, on the worker. Tested by playback_test.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <execution>
#include <numeric>
#include <vector>

namespace back
{
static const int REACH = 14;            // depth pixels along the row: how far the farthest thing is looked for (the widest strip is about 11)
static const float STEP = 0.08f;        // nearness above that farthest thing at which a sample is a near object's (MIRROR_TOL)
static const int ROWS = 2;              // depth rows up and down that the background behind a sample is taken from

// near01: dw x dh, 0 far .. 1 near. picture: dw x dh, R G B in the low bytes (the top byte
// is ignored). Returns dw x dh as described above; empty on bad input.
inline std::vector<uint32_t> BuildBackLayer(const float* near01, const uint32_t* picture, int dw, int dh,
    int reach = REACH, float step = STEP)
{
    std::vector<uint32_t> out;
    if (!near01 || !picture || dw <= 0 || dh <= 0 || reach < 1) return out;
    const size_t n = (size_t)dw * dh;

    // How much of each sample is background: 1 up to half a STEP above the farthest
    // thing within REACH along its row, 0 from a whole STEP.
    std::vector<float> own(n), farthestOf(n);
    std::vector<int> rows((size_t)dh);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y)
    {
        const float* row = near01 + (size_t)y * dw;
        for (int x = 0; x < dw; x++)
        {
            float farthest = row[x];
            for (int i = std::max(0, x - reach); i <= std::min(dw - 1, x + reach); i++) farthest = std::min(farthest, row[i]);
            farthestOf[(size_t)y * dw + x] = farthest;
            own[(size_t)y * dw + x] = std::clamp(2.0f - (row[x] - farthest) / (0.5f * step), 0.0f, 1.0f);
        }
    });
    // The samples next to a near object are a mix of both (the depth's edge is soft and
    // the picture's is averaged over the sample): they are not background either.
    std::vector<float> weight(n);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y)
    {
        for (int x = 0; x < dw; x++)
        {
            float m = 1.0f;
            for (int j = std::max(0, y - 1); j <= std::min(dh - 1, y + 1); j++)
                for (int i = std::max(0, x - 1); i <= std::min(dw - 1, x + 1); i++) m = std::min(m, own[(size_t)j * dw + i]);
            weight[(size_t)y * dw + x] = m;
        }
    });

    // What is behind a near object is taken from the background near it: the samples
    // within REACH along its rows (and ROWS rows up and down, so that the rows agree)
    // that are all background and no nearer than half a STEP above the farthest thing
    // there - not the inside of a wide near object, which is "background" by the test
    // above only because nothing farther is within REACH of it. The nearer ones count
    // for more.
    out.resize(n);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y)
    {
        for (int x = 0; x < dw; x++)
        {
            const size_t at = (size_t)y * dw + x;
            float c[3] = { (float)(picture[at] & 255), (float)((picture[at] >> 8) & 255), (float)((picture[at] >> 16) & 255) };
            if (weight[at] < 1.0f)
            {
                const float limit = farthestOf[at] + 0.5f * step;
                for (int pass = 0; pass < 2; pass++)        // 0: samples that are all background; 1: failing that, any background
                {
                    float sum[3] = { 0, 0, 0 }, k = 0;
                    for (int j = std::max(0, y - ROWS); j <= std::min(dh - 1, y + ROWS); j++)
                        for (int i = std::max(0, x - reach); i <= std::min(dw - 1, x + reach); i++)
                        {
                            const size_t s = (size_t)j * dw + i;
                            if ((pass == 0 ? weight[s] : own[s]) < 1.0f || near01[s] > limit) continue;
                            const float w = 1.0f / (1.0f + (float)((i - x) * (i - x)) + 4.0f * (float)((j - y) * (j - y)));
                            sum[0] += w * (float)(picture[s] & 255); sum[1] += w * (float)((picture[s] >> 8) & 255); sum[2] += w * (float)((picture[s] >> 16) & 255);
                            k += w;
                        }
                    if (k <= 0) continue;
                    for (int ch = 0; ch < 3; ch++) c[ch] = weight[at] * c[ch] + (1.0f - weight[at]) * sum[ch] / k;
                    break;
                }
            }
            uint32_t v = 0;
            for (int ch = 0; ch < 3; ch++) v |= (uint32_t)std::lround(std::clamp(c[ch], 0.0f, 255.0f)) << (8 * ch);
            out[at] = v | ((uint32_t)std::lround(weight[at] * 255.0f) << 24);
        }
    });
    return out;
}
}
