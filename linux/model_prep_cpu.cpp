#include "model_prep_cpu.h"
#include "synthetic_scene.h"
#include <algorithm>
#include <stdexcept>

namespace vrx {
namespace {
constexpr int modelWidth = 672, modelHeight = 384;
float SampleChannel(const std::vector<unsigned char>& rgb, int channel, float u, float v) {
    const float sx = std::clamp(u * kSyntheticWidth - 0.5f, 0.0f, float(kSyntheticWidth - 1));
    const float sy = std::clamp(v * kSyntheticHeight - 0.5f, 0.0f, float(kSyntheticHeight - 1));
    const int x0 = int(sx), y0 = int(sy);
    const int x1 = std::min(x0 + 1, kSyntheticWidth - 1);
    const int y1 = std::min(y0 + 1, kSyntheticHeight - 1);
    const float a = sx - x0, b = sy - y0;
    auto pixel = [&](int x, int y) {
        return float(rgb[(size_t(y) * kSyntheticWidth + x) * 3 + channel]) / 255.0f;
    };
    return (pixel(x0, y0) * (1 - a) + pixel(x1, y0) * a) * (1 - b) +
           (pixel(x0, y1) * (1 - a) + pixel(x1, y1) * a) * b;
}
}
std::vector<float> PrepareModelInput(const std::vector<unsigned char>& rgb) {
    if (rgb.size() != size_t(kSyntheticWidth) * kSyntheticHeight * 3)
        throw std::runtime_error("Live RGB frame has wrong size for ZipDepth");
    const size_t plane = size_t(modelWidth) * modelHeight;
    std::vector<float> output(plane * 3);
    constexpr int taps = 2;
    for (int channel = 0; channel < 3; ++channel)
        for (int y = 0; y < modelHeight; ++y)
            for (int x = 0; x < modelWidth; ++x) {
                float value = 0.0f;
                for (int j = 0; j < taps; ++j)
                    for (int i = 0; i < taps; ++i)
                        value += SampleChannel(rgb, channel,
                            (x + (i + 0.5f) / taps) / modelWidth,
                            (y + (j + 0.5f) / taps) / modelHeight);
                output[size_t(channel) * plane + size_t(y) * modelWidth + x] = value / 4.0f;
            }
    return output;
}
} // namespace vrx
