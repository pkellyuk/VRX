#include "model_prep_cpu.h"
#include "synthetic_scene.h"
#include <algorithm>
#include <stdexcept>

namespace vrx {
namespace {
constexpr int modelWidth = 672, modelHeight = 384;
float SampleChannel(const uint32_t* rgba, int width, int height, int channel, float u, float v) {
    const float sx = std::clamp(u * width - 0.5f, 0.0f, float(width - 1));
    const float sy = std::clamp(v * height - 0.5f, 0.0f, float(height - 1));
    const int x0 = int(sx), y0 = int(sy);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float a = sx - x0, b = sy - y0;
    auto pixel = [&](int x, int y) {
        return float((rgba[size_t(y) * width + x] >> (8 * channel)) & 255u) / 255.0f;
    };
    return (pixel(x0, y0) * (1 - a) + pixel(x1, y0) * a) * (1 - b) +
           (pixel(x0, y1) * (1 - a) + pixel(x1, y1) * a) * b;
}
}

int ModelPrepTaps(int sourceWidth) {
    return std::clamp((sourceWidth + modelWidth - 1) / modelWidth, 1, 4);
}

std::vector<float> PrepareModelInput(const uint32_t* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) throw std::runtime_error("Empty frame for ZipDepth");
    const size_t plane = size_t(modelWidth) * modelHeight;
    std::vector<float> output(plane * 3);
    const bool exact = width == modelWidth && height == modelHeight;
    const int taps = ModelPrepTaps(width);
    for (int channel = 0; channel < 3; ++channel)
        for (int y = 0; y < modelHeight; ++y)
            for (int x = 0; x < modelWidth; ++x) {
                float value = 0.0f;
                if (exact) {
                    value = float((rgba[size_t(y) * width + x] >> (8 * channel)) & 255u) / 255.0f;
                } else {
                    for (int j = 0; j < taps; ++j)
                        for (int i = 0; i < taps; ++i)
                            value += SampleChannel(rgba, width, height, channel,
                                (x + (i + 0.5f) / taps) / modelWidth,
                                (y + (j + 0.5f) / taps) / modelHeight);
                    value /= float(taps * taps);
                }
                output[size_t(channel) * plane + size_t(y) * modelWidth + x] = value;
            }
    return output;
}

std::vector<float> PrepareModelInput(const std::vector<unsigned char>& rgb) {
    if (rgb.size() != size_t(kSyntheticWidth) * kSyntheticHeight * 3)
        throw std::runtime_error("Live RGB frame has wrong size for ZipDepth");
    std::vector<uint32_t> rgba(rgb.size() / 3);
    for (size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = uint32_t(rgb[i * 3]) | (uint32_t(rgb[i * 3 + 1]) << 8) | (uint32_t(rgb[i * 3 + 2]) << 16);
    return PrepareModelInput(rgba.data(), kSyntheticWidth, kSyntheticHeight);
}
} // namespace vrx
