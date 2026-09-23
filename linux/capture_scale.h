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
// Colour is presented at the source's size, shrunk to at most this width, as in
// xrapp5 (MAX_COLOR_W). Depth stays on the model's kSyntheticWidth grid.
inline constexpr int kMaxColorWidth = 1920;

inline void ColorSizeFor(int sourceWidth, int sourceHeight, int& width, int& height) {
    const double k = sourceWidth > kMaxColorWidth ? double(kMaxColorWidth) / sourceWidth : 1.0;
    width = std::max(2, int(sourceWidth * k + 0.5) & ~1);
    height = std::max(2, int(sourceHeight * k + 0.5) & ~1);
}

// Resample a 4-byte-per-pixel image into the rectangle `fit` of an outW x outH
// target. Shrinking averages up to 4x4 source samples per output pixel to
// suppress aliasing; enlarging (or an equal size) is bilinear. `write` receives
// the output pixel index and its RGB for every pixel inside `fit`.
template <typename Write>
inline void ResampleCapture(const unsigned char* pixels, size_t stride, int sourceWidth,
                            int sourceHeight, bool rgba, int outW, int outH,
                            const FitRect& fit, Write write) {
    if (!pixels || sourceWidth <= 0 || sourceHeight <= 0 || outW <= 0 || outH <= 0) return;
    if (fit.width <= 0 || fit.height <= 0) return;
    const int taps = std::clamp(int(std::ceil(std::max(sourceWidth / fit.width,
                                                     sourceHeight / fit.height))), 1, 4);
    std::vector<std::array<int, 4>> xSamples(outW), ySamples(outH);
    std::vector<bool> validX(outW), validY(outH);
    for (int x = 0; x < outW; ++x) {
        validX[x] = x + 0.5f >= fit.x && x + 0.5f < fit.x + fit.width;
        for (int i = 0; i < taps; ++i)
            xSamples[x][i] = std::clamp(int((x + (i + 0.5f) / taps - fit.x) *
                                                sourceWidth / fit.width), 0, sourceWidth - 1);
    }
    for (int y = 0; y < outH; ++y) {
        validY[y] = y + 0.5f >= fit.y && y + 0.5f < fit.y + fit.height;
        for (int j = 0; j < taps; ++j)
            ySamples[y][j] = std::clamp(int((y + (j + 0.5f) / taps - fit.y) *
                                                sourceHeight / fit.height), 0, sourceHeight - 1);
    }
    const int count = taps * taps;
    for (int y = 0; y < outH; ++y) {
        if (!validY[y]) continue;
        for (int x = 0; x < outW; ++x) {
            if (!validX[x]) continue;
            const size_t index = size_t(y) * outW + x;
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
                unsigned char rgb[3];
                for (int c = 0; c < 3; ++c) {
                    const int sc = rgba ? c : 2 - c;
                    const float top = a[sc] + (b[sc] - a[sc]) * fx;
                    const float bottom = d[sc] + (e[sc] - d[sc]) * fx;
                    rgb[c] = static_cast<unsigned char>(std::lround(top + (bottom - top) * fy));
                }
                write(index, rgb[0], rgb[1], rgb[2]);
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
            write(index, static_cast<unsigned char>((sums[0] + count / 2) / count),
                  static_cast<unsigned char>((sums[1] + count / 2) / count),
                  static_cast<unsigned char>((sums[2] + count / 2) / count));
        }
    }
}

// Fit a captured BGRA/BGRx/RGBA frame into the fixed depth-grid RGB image.
inline void ScaleCapture(const unsigned char* pixels, size_t stride, int sourceWidth,
                         int sourceHeight, bool rgba, std::vector<unsigned char>& output) {
    output.assign(size_t(kSyntheticWidth) * kSyntheticHeight * 3, 0);
    const auto fit = FitCapture(sourceWidth, sourceHeight, kSyntheticWidth, kSyntheticHeight);
    ResampleCapture(pixels, stride, sourceWidth, sourceHeight, rgba, kSyntheticWidth,
                    kSyntheticHeight, fit, [&](size_t i, unsigned char r, unsigned char g, unsigned char b) {
                        output[i * 3] = r; output[i * 3 + 1] = g; output[i * 3 + 2] = b;
                    });
}

// Fit a captured frame into the width x height colour texture as packed RGBA
// (red in the low byte), letterboxed in opaque black like xrapp5's CaptureScaler.
inline void ScaleCaptureColor(const unsigned char* pixels, size_t stride, int sourceWidth,
                              int sourceHeight, bool rgba, int width, int height,
                              std::vector<uint32_t>& output) {
    output.assign(size_t(width) * height, 0xff000000u);
    const auto fit = FitCapture(sourceWidth, sourceHeight, width, height);
    ResampleCapture(pixels, stride, sourceWidth, sourceHeight, rgba, width, height, fit,
                    [&](size_t i, unsigned char r, unsigned char g, unsigned char b) {
                        output[i] = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | 0xff000000u;
                    });
}

// The depth model sees the whole colour texture stretched onto its grid, so a
// depth texel maps proportionally onto colour (the warp's bilinear lookup).
inline void DepthGridFromColor(const std::vector<uint32_t>& color, int width, int height,
                               std::vector<unsigned char>& rgb) {
    rgb.assign(size_t(kSyntheticWidth) * kSyntheticHeight * 3, 0);
    if (color.size() != size_t(width) * height) return;
    const FitRect whole{0.0f, 0.0f, float(kSyntheticWidth), float(kSyntheticHeight)};
    ResampleCapture(reinterpret_cast<const unsigned char*>(color.data()), size_t(width) * 4,
                    width, height, true, kSyntheticWidth, kSyntheticHeight, whole,
                    [&](size_t i, unsigned char r, unsigned char g, unsigned char b) {
                        rgb[i * 3] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
                    });
}

// Pack a depth-grid RGB image (synthetic scene, still) as RGBA colour.
inline std::vector<uint32_t> PackRgb(const std::vector<unsigned char>& rgb) {
    std::vector<uint32_t> packed(rgb.size() / 3);
    for (size_t i = 0; i < packed.size(); ++i)
        packed[i] = uint32_t(rgb[i * 3]) | (uint32_t(rgb[i * 3 + 1]) << 8) |
                    (uint32_t(rgb[i * 3 + 2]) << 16) | 0xff000000u;
    return packed;
}
} // namespace vrx
