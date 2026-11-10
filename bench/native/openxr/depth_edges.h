#pragma once
// Horizontal depth edges made into clean steps (XPOLISH.md, "Edges across the picture").
//
// The warp moves pixels sideways only, by their depth. Where the depth changes from row
// to row - the top or bottom edge of anything nearer than what is behind it - each row is
// moved a little more than the one before, in opposite directions for the two eyes: the
// picture there is sheared. The model's edge is a ramp a few depth rows tall, and
// DilateNear then carried that ramp outward (3 depth rows, about 8 of the picture's) onto
// whatever is next to the object: desktop icons just above a window had their lower
// halves slanted at about 45 degrees.
//
// SnapRowEdges turns each short ramp down a column into a step between two rows: at the
// picture's own edge when there is one within ROW_SEARCH rows of the ramp's middle, else
// at the middle. The rows above take the ramp's upper value and the rows below its lower
// one, so the picture is cut at the edge instead of sheared across it. It marks each step
// in `barrier`, and DilateNearRows (the vertical half of DilateNear) does not carry depth
// across a marked step. A longer ramp is a real slope (a floor, a road) and is left alone.
//
// Pure CPU, on the depth grid; the caller owns the buffers. Tested by playback_test.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace edges
{
static const float ROW_STEP = 0.03f;    // nearness (0..1): a row-to-row change this large is part of an edge
static const float ROW_EDGE = 0.12f;    // an edge's whole change must reach this to be made a step
static const int ROW_EDGE_MAX = 6;      // rows: a longer ramp is a slope, not an edge
static const int ROW_SEARCH = 2;        // rows either side of the ramp's middle searched for the picture's edge
static const int ROW_LUMA_REACH = 3;    // columns either side averaged when scoring the picture's edge
static const float ROW_LUMA = 6.0f;     // of 255: the mean luma change across rows that counts as the picture's edge
// The warp shader's part (NearAt): depth rows that differ by more than this are not
// blended down the picture's rows between them (which would be a ramp again, about 3
// picture rows tall); each picture row takes the nearer depth row's value.
static const float ROW_SNAP = 0.25f;

// near01: w x h, 0 far .. 1 near, changed in place. luma: w x h in 0..255, or null (the
// steps then sit at the ramps' middles). barrier: resized to w x h; barrier[y * w + x] = 1
// marks a step between rows y and y + 1 of column x. Returns the number of steps made.
inline size_t SnapRowEdges(std::vector<float>& near01, const float* luma, int w, int h, std::vector<uint8_t>& barrier)
{
    barrier.assign((size_t)std::max(w, 0) * std::max(h, 0), 0);
    if (w <= 0 || h <= 1 || near01.size() != (size_t)w * h) return 0;

    // The picture's edge between rows e and e + 1 at column x: the mean change across
    // them over a few columns, so one odd pixel does not decide it.
    auto lumaEdge = [&](int x, int e)
    {
        const int a = std::max(0, x - ROW_LUMA_REACH), b = std::min(w - 1, x + ROW_LUMA_REACH);
        float sum = 0;
        for (int c = a; c <= b; c++) sum += std::fabs(luma[(size_t)(e + 1) * w + c] - luma[(size_t)e * w + c]);
        return sum / (float)(b - a + 1);
    };

    size_t steps = 0;
    for (int x = 0; x < w; x++)
    {
        auto v = [&](int y) -> float& { return near01[(size_t)y * w + x]; };
        int y = 0;
        while (y < h - 1)
        {
            const float d = v(y + 1) - v(y);
            if (!(std::fabs(d) >= ROW_STEP)) { y++; continue; }
            // The ramp: rows y0..y1, each changing the same way by at least ROW_STEP.
            const bool up = d > 0;
            const int y0 = y;
            int y1 = y + 1;
            while (y1 < h - 1)
            {
                const float d2 = v(y1 + 1) - v(y1);
                if (!(std::fabs(d2) >= ROW_STEP) || (d2 > 0) != up) break;
                y1++;
            }
            const float top = v(y0), bottom = v(y1);
            if (std::fabs(bottom - top) < ROW_EDGE || y1 - y0 > ROW_EDGE_MAX) { y = y1; continue; }

            // The step goes between rows e and e + 1: first where the ramp passes halfway...
            const float mid = 0.5f * (top + bottom);
            int e = y1 - 1;
            for (int k = y0 + 1; k <= y1; k++)
                if (up ? v(k) >= mid : v(k) <= mid) { e = k - 1; break; }
            // ...then at the picture's own edge, if one is close (nearer wins a tie).
            if (luma)
            {
                float best = ROW_LUMA;
                int bestE = e;
                for (int k = 0; k <= 2 * ROW_SEARCH; k++)
                {
                    const int c = e + ((k & 1) ? -(k + 1) / 2 : k / 2);     // e, e-1, e+1, e-2, e+2
                    if (c < 0 || c > h - 2) continue;
                    const float score = lumaEdge(x, c);
                    if (score > best) { best = score; bestE = c; }
                }
                e = bestE;
            }

            for (int k = std::min(y0, e + 1); k <= e; k++) v(k) = top;
            for (int k = e + 1; k <= std::max(y1, e); k++) v(k) = bottom;
            barrier[(size_t)e * w + x] = 1;
            steps++;
            y = std::max(y1, e + 1);
        }
    }
    return steps;
}

// A step also stops depth being carried across it in the columns DilateNear's horizontal
// half has just widened the near side into: each mark is spread rx columns either way.
inline void SpreadBarrier(std::vector<uint8_t>& barrier, int w, int h, int rx)
{
    if (rx <= 0 || w <= 0 || h <= 0 || barrier.size() != (size_t)w * h) return;
    std::vector<uint8_t> row(w);
    for (int y = 0; y < h; y++)
    {
        uint8_t* p = barrier.data() + (size_t)y * w;
        std::copy(p, p + w, row.begin());
        for (int x = 0; x < w; x++)
        {
            if (row[x]) continue;
            const int a = std::max(0, x - rx), b = std::min(w - 1, x + rx);
            for (int i = a; i <= b; i++) if (row[i]) { p[x] = 1; break; }
        }
    }
}

// The vertical half of DilateNear (xr_common.h): each pixel takes the nearest value within
// ry rows above and below it, but not from beyond a marked step.
inline void DilateNearRows(std::vector<float>& near01, int w, int h, int ry, const std::vector<uint8_t>& barrier)
{
    if (ry <= 0 || w <= 0 || h <= 0 || near01.size() != (size_t)w * h || barrier.size() != near01.size()) return;
    const std::vector<float> src = near01;
    for (int y = 0; y < h; y++)
    {
        float* p = near01.data() + (size_t)y * w;
        for (int x = 0; x < w; x++)
        {
            float m = src[(size_t)y * w + x];
            for (int i = y - 1; i >= y - ry && i >= 0; i--)
            {
                if (barrier[(size_t)i * w + x]) break;              // a step between rows i and i + 1
                m = std::max(m, src[(size_t)i * w + x]);
            }
            for (int i = y + 1; i <= y + ry && i < h; i++)
            {
                if (barrier[(size_t)(i - 1) * w + x]) break;        // a step between rows i - 1 and i
                m = std::max(m, src[(size_t)i * w + x]);
            }
            p[x] = m;
        }
    }
}
}
