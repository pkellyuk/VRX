#include "capture_scale.h"
#include <cassert>
#include <cstddef>
#include <vector>

int main() {
    // A portrait capture has black side bars and unchanged central colour.
    std::vector<unsigned char> portrait(size_t(200) * 400 * 4);
    for (size_t i = 0; i < portrait.size(); i += 4) {
        portrait[i] = 10; portrait[i + 1] = 20; portrait[i + 2] = 30;
    }
    std::vector<unsigned char> output;
    vrx::ScaleCapture(portrait.data(), 200 * 4, 200, 400, true, output);
    assert(output[0] == 0 && output[1] == 0 && output[2] == 0);
    const size_t center = (size_t(vrx::kSyntheticHeight / 2) * vrx::kSyntheticWidth +
                           vrx::kSyntheticWidth / 2) * 3;
    assert(output[center] == 10 && output[center + 1] == 20 && output[center + 2] == 30);

    // A 4x checkerboard must average to grey instead of aliasing to black or white.
    constexpr int width = vrx::kSyntheticWidth * 4, height = vrx::kSyntheticHeight * 4;
    std::vector<unsigned char> checker(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t i = (size_t(y) * width + x) * 4;
            checker[i] = checker[i + 1] = checker[i + 2] = (x + y) % 2 ? 255 : 0;
        }
    vrx::ScaleCapture(checker.data(), size_t(width) * 4, width, height, false, output);
    assert(output[center] >= 127 && output[center] <= 128);
    assert(output[center + 1] == output[center] && output[center + 2] == output[center]);

    // Colour keeps the source's shape, shrunk to at most 1920 wide.
    int colorWidth = 0, colorHeight = 0;
    vrx::ColorSizeFor(3840, 2160, colorWidth, colorHeight);
    assert(colorWidth == 1920 && colorHeight == 1080);
    vrx::ColorSizeFor(1280, 1025, colorWidth, colorHeight);
    assert(colorWidth == 1280 && colorHeight == 1024);

    // An exact 2x shrink averages each 2x2 block; the checkerboard becomes grey.
    std::vector<uint32_t> color;
    vrx::ScaleCaptureColor(checker.data(), size_t(width) * 4, width, height, false,
                           width / 2, height / 2, color);
    const uint32_t middle = color[size_t(height / 4) * (width / 2) + width / 4];
    assert((middle & 255u) >= 127 && (middle & 255u) <= 128 && (middle >> 24) == 255u);

    // A source smaller than the colour texture is letterboxed in opaque black,
    // and a BGRA source is swizzled to RGBA.
    std::vector<unsigned char> bgra(size_t(100) * 100 * 4);
    for (size_t i = 0; i < bgra.size(); i += 4) { bgra[i] = 30; bgra[i + 1] = 20; bgra[i + 2] = 10; }
    vrx::ScaleCaptureColor(bgra.data(), 100 * 4, 100, 100, false, 200, 100, color);
    assert(color[0] == 0xff000000u);
    assert(color[size_t(50) * 200 + 100] == (10u | (20u << 8) | (30u << 16) | 0xff000000u));

    // The depth grid is the whole colour texture, stretched.
    vrx::DepthGridFromColor(color, 200, 100, output);
    assert(output.size() == size_t(vrx::kSyntheticWidth) * vrx::kSyntheticHeight * 3);
    assert(output[0] == 0 && output[center] == 10 && output[center + 1] == 20 && output[center + 2] == 30);

    // Splitting rows between threads does not change the result.
    std::vector<uint32_t> single, split;
    vrx::ScaleCaptureColor(checker.data(), size_t(width) * 4, width, height, false,
                           1000, 571, single, 1);
    vrx::ScaleCaptureColor(checker.data(), size_t(width) * 4, width, height, false,
                           1000, 571, split, 4);
    assert(single == split);
}
