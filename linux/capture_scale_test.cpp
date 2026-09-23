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
}
