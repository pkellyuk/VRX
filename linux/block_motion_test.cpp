// Block motion: a textured picture moved by known amounts is found again over the
// depth grid (686x392): most blocks to the quarter pel, nearly all within half a
// pixel (the moved picture is itself a bilinear resample, which blurs the finest
// steps), and a still picture keeps still.
#include "block_motion.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace {
// Smooth value noise (random levels every 4 pixels, interpolated): textured like a
// game picture and, unlike a pattern, never matching a shifted copy of itself.
fusion::Image Texture(int w, int h) {
    const int gw = w / 4 + 2, gh = h / 4 + 2;
    fusion::Image grid(gw, gh);
    uint32_t state = 12345;
    for (float& value : grid.v) {
        state = state * 1664525u + 1013904223u;
        value = float(state >> 24);
    }
    fusion::Image img(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) img.at(x, y) = fusion::Sample(grid, x / 4.0f, y / 4.0f);
    return img;
}
// ref(p) = cur(p - v): the content at cur(x) was at ref(x + v).
fusion::Image Moved(const fusion::Image& cur, float vx, float vy) {
    fusion::Image ref(cur.w, cur.h);
    for (int y = 0; y < cur.h; y++)
        for (int x = 0; x < cur.w; x++) ref.at(x, y) = fusion::Sample(cur, x - vx, y - vy);
    return ref;
}
// The share of interior blocks whose vector is within `tolerance` quarter pels.
double Found(const std::vector<int16_t>& v, int bw, int bh, float vx, float vy, int tolerance) {
    int good = 0, total = 0;
    for (int by = 2; by < bh - 2; by++)
        for (int bx = 2; bx < bw - 2; bx++, total++) {
            const size_t i = (size_t(by) * bw + bx) * 2;
            if (std::abs(v[i] - std::lround(vx * 4)) <= tolerance && std::abs(v[i + 1] - std::lround(vy * 4)) <= tolerance) good++;
        }
    return double(good) / total;
}
}

int main() {
    const int w = 686, h = 392, bw = (w + 7) / 8, bh = (h + 7) / 8;
    const fusion::Image cur = Texture(w, h);
    const struct { float x, y; } moves[] = {{0, 0}, {5.25f, -3.5f}, {-20.0f, 12.75f}, {0.5f, 0.25f}};
    for (const auto& m : moves) {
        const auto start = std::chrono::steady_clock::now();
        const auto vectors = vrx::EstimateBlockMotion(cur, Moved(cur, m.x, m.y));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        assert(vectors.size() == size_t(bw) * bh * 2);
        const double exact = Found(vectors, bw, bh, m.x, m.y, 0), close = Found(vectors, bw, bh, m.x, m.y, 2);
        std::printf("move %.2f, %.2f: %.0f %% exact, %.0f %% within half a pixel (%.1f ms)\n",
                    m.x, m.y, exact * 100, close * 100, ms);
        assert(exact > 0.65 && close > 0.95);
    }
    std::puts("Block motion checks passed");
}
