#pragma once

#include "playback_policy.h"
#include "synthetic_scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>
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

// Where each output pixel samples its source. Built once per source size and
// shared by the CPU resampler and capture_scale.comp, so both read the same
// source pixels with the same weights.
//   taps > 1 : box filter; x[i][0..taps) and y[j][0..taps) are source columns
//              and rows, averaged with round-to-nearest integer division.
//   taps == 1: bilinear; x[i][0..1] and y[j][0..1] are the two neighbours and
//              fx/fy the weight of the second.
// Output pixels outside `fit` (letterbox) have x or y [0] = -1.
struct ResampleTables {
    int sourceWidth = 0, sourceHeight = 0, outW = 0, outH = 0, taps = 1;
    int xBegin = 0, xEnd = 0, yBegin = 0, yEnd = 0;
    std::vector<std::array<int, 4>> x, y;
    std::vector<float> fx, fy;
};

inline ResampleTables BuildResampleTables(int sourceWidth, int sourceHeight, int outW, int outH,
                                          const FitRect& fit) {
    ResampleTables t;
    t.sourceWidth = sourceWidth; t.sourceHeight = sourceHeight; t.outW = outW; t.outH = outH;
    t.x.assign(size_t(std::max(outW, 0)), {-1, -1, -1, -1});
    t.y.assign(size_t(std::max(outH, 0)), {-1, -1, -1, -1});
    t.fx.assign(t.x.size(), 0.0f);
    t.fy.assign(t.y.size(), 0.0f);
    if (sourceWidth <= 0 || sourceHeight <= 0 || outW <= 0 || outH <= 0 ||
        fit.width <= 0 || fit.height <= 0) return t;
    t.taps = std::clamp(int(std::ceil(std::max(sourceWidth / fit.width, sourceHeight / fit.height))), 1, 4);
    // Output pixels inside `fit` form one contiguous run in each axis.
    auto inside = [](int i, float begin, float size) { return i + 0.5f >= begin && i + 0.5f < begin + size; };
    t.xEnd = outW; t.yEnd = outH;
    while (t.xBegin < outW && !inside(t.xBegin, fit.x, fit.width)) ++t.xBegin;
    while (t.xEnd > t.xBegin && !inside(t.xEnd - 1, fit.x, fit.width)) --t.xEnd;
    while (t.yBegin < outH && !inside(t.yBegin, fit.y, fit.height)) ++t.yBegin;
    while (t.yEnd > t.yBegin && !inside(t.yEnd - 1, fit.y, fit.height)) --t.yEnd;
    for (int x = t.xBegin; x < t.xEnd; ++x) {
        if (t.taps == 1) {
            const float sx = std::clamp((x + 0.5f - fit.x) * sourceWidth / fit.width - 0.5f,
                                        0.0f, float(sourceWidth - 1));
            t.x[x][0] = int(sx);
            t.x[x][1] = std::min(t.x[x][0] + 1, sourceWidth - 1);
            t.fx[x] = sx - t.x[x][0];
        } else for (int i = 0; i < t.taps; ++i)
            t.x[x][i] = std::clamp(int((x + (i + 0.5f) / t.taps - fit.x) *
                                       sourceWidth / fit.width), 0, sourceWidth - 1);
    }
    for (int y = t.yBegin; y < t.yEnd; ++y) {
        if (t.taps == 1) {
            const float sy = std::clamp((y + 0.5f - fit.y) * sourceHeight / fit.height - 0.5f,
                                        0.0f, float(sourceHeight - 1));
            t.y[y][0] = int(sy);
            t.y[y][1] = std::min(t.y[y][0] + 1, sourceHeight - 1);
            t.fy[y] = sy - t.y[y][0];
        } else for (int j = 0; j < t.taps; ++j)
            t.y[y][j] = std::clamp(int((y + (j + 0.5f) / t.taps - fit.y) *
                                       sourceHeight / fit.height), 0, sourceHeight - 1);
    }
    return t;
}

// Resample a 4-byte-per-pixel image with `tables`. `write` receives the output
// pixel index and its RGB for every pixel inside the fit; with more than one
// thread, rows are split between threads and `write` is called concurrently for
// different pixels. The result does not depend on `threads`.
template <typename Write>
inline void ResampleCapture(const unsigned char* pixels, size_t stride, bool rgba,
                            const ResampleTables& t, Write write, int threads = 1) {
    if (!pixels || t.xBegin >= t.xEnd || t.yBegin >= t.yEnd) return;
    const int taps = t.taps, count = taps * taps;
    std::vector<unsigned char> average(size_t(count) * 255 + 1);
    for (size_t sum = 0; sum < average.size(); ++sum)
        average[sum] = static_cast<unsigned char>((int(sum) + count / 2) / count);
    auto boxRow = [&](int y, auto tapCount) {
        constexpr int n = decltype(tapCount)::value;
        const unsigned char* row[n];
        for (int j = 0; j < n; ++j) row[j] = pixels + size_t(t.y[y][j]) * stride;
        for (int x = t.xBegin; x < t.xEnd; ++x) {
            const auto& columns = t.x[x];
            unsigned sums[3]{};
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    const auto* p = row[j] + size_t(columns[i]) * 4;
                    sums[0] += p[0]; sums[1] += p[1]; sums[2] += p[2];
                }
            if (!rgba) std::swap(sums[0], sums[2]);
            write(size_t(y) * t.outW + x, average[sums[0]], average[sums[1]], average[sums[2]]);
        }
    };
    auto bilinearRow = [&](int y) {
        const unsigned char* top = pixels + size_t(t.y[y][0]) * stride;
        const unsigned char* bottom = pixels + size_t(t.y[y][1]) * stride;
        const float fy = t.fy[y];
        for (int x = t.xBegin; x < t.xEnd; ++x) {
            const size_t x0 = size_t(t.x[x][0]) * 4, x1 = size_t(t.x[x][1]) * 4;
            const float fx = t.fx[x];
            unsigned char rgb[3];
            for (int c = 0; c < 3; ++c) {
                const int sc = rgba ? c : 2 - c;
                const float upper = top[x0 + sc] + (top[x1 + sc] - top[x0 + sc]) * fx;
                const float lower = bottom[x0 + sc] + (bottom[x1 + sc] - bottom[x0 + sc]) * fx;
                rgb[c] = static_cast<unsigned char>(std::lround(upper + (lower - upper) * fy));
            }
            write(size_t(y) * t.outW + x, rgb[0], rgb[1], rgb[2]);
        }
    };
    auto rows = [&](int firstRow, int endRow) {
        for (int y = std::max(firstRow, t.yBegin); y < std::min(endRow, t.yEnd); ++y) {
            if (taps == 1) bilinearRow(y);
            else if (taps == 2) boxRow(y, std::integral_constant<int, 2>{});
            else if (taps == 3) boxRow(y, std::integral_constant<int, 3>{});
            else boxRow(y, std::integral_constant<int, 4>{});
        }
    };
    threads = std::clamp(threads, 1, std::max(1, t.outH / 64));
    if (threads == 1) { rows(0, t.outH); return; }
    std::vector<std::thread> workers;
    workers.reserve(threads - 1);
    for (int i = 1; i < threads; ++i)
        workers.emplace_back(rows, t.outH * i / threads, t.outH * (i + 1) / threads);
    rows(0, t.outH / threads);
    for (auto& worker : workers) worker.join();
}

// Fit a captured BGRA/BGRx/RGBA frame into the fixed depth-grid RGB image.
inline void ScaleCapture(const unsigned char* pixels, size_t stride, int sourceWidth,
                         int sourceHeight, bool rgba, std::vector<unsigned char>& output) {
    output.assign(size_t(kSyntheticWidth) * kSyntheticHeight * 3, 0);
    const auto tables = BuildResampleTables(sourceWidth, sourceHeight, kSyntheticWidth, kSyntheticHeight,
        FitCapture(sourceWidth, sourceHeight, kSyntheticWidth, kSyntheticHeight));
    ResampleCapture(pixels, stride, rgba, tables, [&](size_t i, unsigned char r, unsigned char g, unsigned char b) {
                        output[i * 3] = r; output[i * 3 + 1] = g; output[i * 3 + 2] = b;
                    });
}

// Scale a captured frame into the colour texture described by `tables` as packed
// RGBA (red in the low byte), letterboxed in opaque black.
inline void ScaleCaptureColor(const unsigned char* pixels, size_t stride, bool rgba,
                              const ResampleTables& tables, std::vector<uint32_t>& output,
                              int threads = 1) {
    output.assign(size_t(tables.outW) * tables.outH, 0xff000000u);
    ResampleCapture(pixels, stride, rgba, tables,
                    [&](size_t i, unsigned char r, unsigned char g, unsigned char b) {
                        output[i] = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | 0xff000000u;
                    }, threads);
}

// Fit a captured frame into the width x height colour texture as packed RGBA
// (red in the low byte), letterboxed in opaque black like xrapp5's CaptureScaler.
inline void ScaleCaptureColor(const unsigned char* pixels, size_t stride, int sourceWidth,
                              int sourceHeight, bool rgba, int width, int height,
                              std::vector<uint32_t>& output, int threads = 1) {
    ScaleCaptureColor(pixels, stride, rgba, BuildResampleTables(sourceWidth, sourceHeight, width, height,
        FitCapture(sourceWidth, sourceHeight, width, height)), output, threads);
}

// The depth model sees the whole colour texture stretched onto its grid, so a
// depth texel maps proportionally onto colour (the warp's bilinear lookup).
inline void DepthGridFromColor(const std::vector<uint32_t>& color, int width, int height,
                               std::vector<unsigned char>& rgb, int threads = 1) {
    rgb.assign(size_t(kSyntheticWidth) * kSyntheticHeight * 3, 0);
    if (color.size() != size_t(width) * height) return;
    const FitRect whole{0.0f, 0.0f, float(kSyntheticWidth), float(kSyntheticHeight)};
    ResampleCapture(reinterpret_cast<const unsigned char*>(color.data()), size_t(width) * 4, true,
                    BuildResampleTables(width, height, kSyntheticWidth, kSyntheticHeight, whole),
                    [&](size_t i, unsigned char r, unsigned char g, unsigned char b) {
                        rgb[i * 3] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
                    }, threads);
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
