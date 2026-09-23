#pragma once
// Block motion estimation on the CPU, standing in on Linux for the hardware motion
// estimator xrapp5 uses for steady depth (motion_estimator.h: D3D12 video motion
// estimation, which Vulkan does not offer). Same output: one vector per 8x8 block of
// `cur`, (x, y) int16 in quarter pels, pointing to where the block's content was in
// `ref` - what fusion::MotionFromVectors reads.
//
// Coarse to fine on a 2x box pyramid: a full search at quarter size (up to
// kMotionRange pixels each way), refined at half and full size, then to the quarter
// pel with bilinear samples. The cost of a vector is its sum of absolute
// luma differences plus a small penalty on its length, so flat or ambiguous areas keep
// still rather than wander. Wrong matches are caught later by fusion::MotionTrust,
// which only trusts moved depth where the moved picture matches.
#include "depth_fusion.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace vrx {

static const int kMotionRange = 32;          // full-size pixels each way
static const float kMotionLengthCost = 2.0f;  // per full-size pixel of vector length, in summed luma

// 2x box downscale (odd edges replicate).
inline fusion::Image MotionHalf(const fusion::Image& src)
{
    fusion::Image out((src.w + 1) / 2, (src.h + 1) / 2);
    for (int y = 0; y < out.h; y++)
        for (int x = 0; x < out.w; x++)
        {
            const int x0 = std::min(2 * x, src.w - 1), x1 = std::min(2 * x + 1, src.w - 1);
            const int y0 = std::min(2 * y, src.h - 1), y1 = std::min(2 * y + 1, src.h - 1);
            out.at(x, y) = 0.25f * (src.at(x0, y0) + src.at(x1, y0) + src.at(x0, y1) + src.at(x1, y1));
        }
    return out;
}

// Sum of absolute differences between the size x size window of `cur` at (x, y) and
// `ref` at (x + dx, y + dy), whole pixels, borders replicated.
inline float MotionSad(const fusion::Image& cur, const fusion::Image& ref, int x, int y, int size, int dx, int dy)
{
    float sum = 0;
    if (x >= 0 && y >= 0 && x + size <= cur.w && y + size <= cur.h &&
        x + dx >= 0 && y + dy >= 0 && x + dx + size <= ref.w && y + dy + size <= ref.h)
    {
        for (int j = 0; j < size; j++)
        {
            const float* c = cur.v.data() + (size_t)(y + j) * cur.w + x;
            const float* r = ref.v.data() + (size_t)(y + j + dy) * ref.w + x + dx;
            for (int i = 0; i < size; i++) sum += std::fabs(c[i] - r[i]);
        }
        return sum;
    }
    for (int j = 0; j < size; j++)
    {
        const int cy = std::clamp(y + j, 0, cur.h - 1), ry = std::clamp(y + j + dy, 0, ref.h - 1);
        const float* c = cur.v.data() + (size_t)cy * cur.w;
        const float* r = ref.v.data() + (size_t)ry * ref.w;
        for (int i = 0; i < size; i++)
            sum += std::fabs(c[std::clamp(x + i, 0, cur.w - 1)] - r[std::clamp(x + i + dx, 0, ref.w - 1)]);
    }
    return sum;
}

// The same with a fractional offset, sampled bilinearly as fusion::Sample does (the
// weights are the same for the whole window; borders replicated).
inline float MotionSadFraction(const fusion::Image& cur, const fusion::Image& ref, int x, int y, int size, float dx, float dy)
{
    const int ix = (int)std::floor(dx), iy = (int)std::floor(dy);
    const float fx = dx - ix, fy = dy - iy;
    const bool inside = x + ix >= 0 && y + iy >= 0 && x + ix + size < ref.w && y + iy + size < ref.h &&
                        x >= 0 && y >= 0 && x + size <= cur.w && y + size <= cur.h;
    if (!inside)
    {
        float sum = 0;
        for (int j = 0; j < size; j++)
        {
            const int cy = std::clamp(y + j, 0, cur.h - 1);
            for (int i = 0; i < size; i++)
                sum += std::fabs(cur.at(std::clamp(x + i, 0, cur.w - 1), cy) - fusion::Sample(ref, x + i + dx, y + j + dy));
        }
        return sum;
    }
    const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
    float sum = 0;
    for (int j = 0; j < size; j++)
    {
        const float* c = cur.v.data() + (size_t)(y + j) * cur.w + x;
        const float* r0 = ref.v.data() + (size_t)(y + j + iy) * ref.w + x + ix;
        const float* r1 = r0 + ref.w;
        for (int i = 0; i < size; i++)
            sum += std::fabs(c[i] - (w00 * r0[i] + w10 * r0[i + 1] + w01 * r1[i] + w11 * r1[i + 1]));
    }
    return sum;
}

// The length penalty of a vector (dx, dy) in a level `scale` times smaller than full
// size, for a size x size window: kMotionLengthCost per full-size pixel, scaled to the
// window's share of an 8x8 block's sum.
inline float MotionLengthCost(int dx, int dy, int scale, int size)
{
    return kMotionLengthCost * (float)((std::abs(dx) + std::abs(dy)) * scale) * (float)(size * size) / 64.0f;
}

// Vectors for bw x bh blocks of 8x8 (cur and ref the same size, luma 0..255).
inline std::vector<int16_t> EstimateBlockMotion(const fusion::Image& cur, const fusion::Image& ref)
{
    const int bw = (cur.w + fusion::MV_BLOCK - 1) / fusion::MV_BLOCK;
    const int bh = (cur.h + fusion::MV_BLOCK - 1) / fusion::MV_BLOCK;
    std::vector<int16_t> vectors((size_t)bw * bh * 2, 0);
    if (!cur.valid() || !ref.valid() || cur.w != ref.w || cur.h != ref.h) return vectors;
    const fusion::Image cur1 = MotionHalf(cur), ref1 = MotionHalf(ref);
    const fusion::Image cur2 = MotionHalf(cur1), ref2 = MotionHalf(ref1);

    fusion::ForRows(bh, [&](int by)
    {
        for (int bx = 0; bx < bw; bx++)
        {
            const int x0 = bx * fusion::MV_BLOCK, y0 = by * fusion::MV_BLOCK;
            // Quarter size: the 16x16 area round the block, a 4x4 window, searched fully.
            int vx = 0, vy = 0;
            {
                const int range = kMotionRange / 4;
                float best = std::numeric_limits<float>::max();
                for (int dy = -range; dy <= range; dy++)
                    for (int dx = -range; dx <= range; dx++)
                    {
                        const float cost = MotionSad(cur2, ref2, x0 / 4 - 1, y0 / 4 - 1, 4, dx, dy) +
                                           MotionLengthCost(dx, dy, 4, 4);
                        if (cost < best) { best = cost; vx = dx; vy = dy; }
                    }
            }
            // Half size (the same 16x16 area, 8x8) and full size (the block): +-2 round
            // the coarser vector, and still at full size as a fallback.
            auto refine = [&](const fusion::Image& c, const fusion::Image& r, int x, int y, int size, int scale,
                              int cx, int cy, bool tryStill)
            {
                float best = std::numeric_limits<float>::max();
                int ox = cx, oy = cy;
                auto consider = [&](int dx, int dy)
                {
                    const float cost = MotionSad(c, r, x, y, size, dx, dy) + MotionLengthCost(dx, dy, scale, size);
                    if (cost < best) { best = cost; ox = dx; oy = dy; }
                };
                if (tryStill) consider(0, 0);
                for (int dy = cy - 2; dy <= cy + 2; dy++)
                    for (int dx = cx - 2; dx <= cx + 2; dx++) consider(dx, dy);
                return std::pair<int, int>{ ox, oy };
            };
            auto [hx, hy] = refine(cur1, ref1, x0 / 2 - 2, y0 / 2 - 2, 8, 2, 2 * vx, 2 * vy, false);
            auto [fx, fy] = refine(cur, ref, x0, y0, fusion::MV_BLOCK, 1, 2 * hx, 2 * hy, true);
            // Every quarter pel within 3/4 of a pixel of the best whole-pixel vector.
            float qx = (float)fx, qy = (float)fy;
            float best = MotionSad(cur, ref, x0, y0, fusion::MV_BLOCK, fx, fy);
            for (int j = -3; j <= 3; j++)
                for (int i = -3; i <= 3; i++)
                {
                    if (i == 0 && j == 0) continue;
                    const float dx = fx + i * 0.25f, dy = fy + j * 0.25f;
                    const float cost = MotionSadFraction(cur, ref, x0, y0, fusion::MV_BLOCK, dx, dy);
                    if (cost < best) { best = cost; qx = dx; qy = dy; }
                }
            const size_t index = ((size_t)by * bw + bx) * 2;
            vectors[index] = (int16_t)std::lround(qx * 4.0f);
            vectors[index + 1] = (int16_t)std::lround(qy * 4.0f);
        }
    });
    return vectors;
}

} // namespace vrx
