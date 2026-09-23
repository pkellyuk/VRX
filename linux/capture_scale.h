#pragma once

#include "playback_policy.h"
#include "synthetic_scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vrx {
// Fit a captured BGRA/BGRx/RGBA frame into the fixed stereo texture. Sample
// several source positions per output pixel when shrinking to suppress aliasing.
inline void ScaleCapture(const unsigned char* pixels, size_t stride, int sourceWidth,
                         int sourceHeight, bool rgba, std::vector<unsigned char>& output) {
    output.assign(size_t(kSyntheticWidth) * kSyntheticHeight * 3, 0);
    if (!pixels || sourceWidth <= 0 || sourceHeight <= 0) return;
    const auto fit = FitCapture(sourceWidth, sourceHeight, kSyntheticWidth, kSyntheticHeight);
    if (fit.width <= 0 || fit.height <= 0) return;
    const int taps = std::clamp(int(std::ceil(std::max(sourceWidth / fit.width,
                                                     sourceHeight / fit.height))), 1, 4);
    std::vector<std::array<int, 4>> xSamples(kSyntheticWidth), ySamples(kSyntheticHeight);
    std::vector<bool> validX(kSyntheticWidth), validY(kSyntheticHeight);
    for (int x = 0; x < kSyntheticWidth; ++x) {
        validX[x] = x + 0.5f >= fit.x && x + 0.5f < fit.x + fit.width;
        for (int i = 0; i < taps; ++i)
            xSamples[x][i] = std::clamp(int((x + (i + 0.5f) / taps - fit.x) *
                                                sourceWidth / fit.width), 0, sourceWidth - 1);
    }
    for (int y = 0; y < kSyntheticHeight; ++y) {
        validY[y] = y + 0.5f >= fit.y && y + 0.5f < fit.y + fit.height;
        for (int j = 0; j < taps; ++j)
            ySamples[y][j] = std::clamp(int((y + (j + 0.5f) / taps - fit.y) *
                                                sourceHeight / fit.height), 0, sourceHeight - 1);
    }
    const int count = taps * taps;
    for (int y = 0; y < kSyntheticHeight; ++y) {
        if (!validY[y]) continue;
        for (int x = 0; x < kSyntheticWidth; ++x) {
            if (!validX[x]) continue;
            const size_t index = (size_t(y) * kSyntheticWidth + x) * 3;
            if (taps == 1) {
                const float sx = std::clamp((x + 0.5f - fit.x) * sourceWidth / fit.width - 0.5f,
                                            0.0f, float(sourceWidth - 1));
                const float sy = std::clamp((y + 0.5f - fit.y) * sourceHeight / fit.height - 0.5f,
                                            0.0f, float(sourceHeight - 1));
                const int x0 = int(sx), x1 = std::min(x0 + 1, sourceWidth - 1);
                const int y0 = int(sy), y1 = std::min(y0 + 1, sourceHeight - 1);
                const float fx = sx - x0, fy = sy - y0;
                const auto* a = pixels + size_t(y0) * stride + size_t(x0) * 4;
                const auto* b = pixels + size_t(y0) * stride + size_t(x1) * 4;
                const auto* d = pixels + size_t(y1) * stride + size_t(x0) * 4;
                const auto* e = pixels + size_t(y1) * stride + size_t(x1) * 4;
                for (int c = 0; c < 3; ++c) {
                    const int sc = rgba ? c : 2 - c;
                    const float top = a[sc] + (b[sc] - a[sc]) * fx;
                    const float bottom = d[sc] + (e[sc] - d[sc]) * fx;
                    output[index + c] = static_cast<unsigned char>(std::lround(top + (bottom - top) * fy));
                }
                continue;
            }
            int sums[3]{};
            for (int j = 0; j < taps; ++j) {
                const auto* row = pixels + size_t(ySamples[y][j]) * stride;
                for (int i = 0; i < taps; ++i) {
                    const auto* p = row + size_t(xSamples[x][i]) * 4;
                    if (rgba) { sums[0] += p[0]; sums[1] += p[1]; sums[2] += p[2]; }
                    else { sums[0] += p[2]; sums[1] += p[1]; sums[2] += p[0]; }
                }
            }
            for (int c = 0; c < 3; ++c) output[index + c] = (sums[c] + count / 2) / count;
        }
    }
}
} // namespace vrx
