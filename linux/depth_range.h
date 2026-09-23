#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vrx {
struct RangeSmoother {
    bool have = false;
    float low = 0.0f, high = 1.0f;
    double lastTime = 0.0;
};

// Windows SmoothRange's percentile range and 0.4-second temporal smoothing.
// Large range jumps are treated as scene cuts and applied immediately.
inline void DepthRange(const std::vector<float>& raw, RangeSmoother* smoother,
                       double now, float& low, float& high) {
    if (raw.empty()) { low = 0.0f; high = 1.0f; return; }
    const auto [minIt, maxIt] = std::minmax_element(raw.begin(), raw.end());
    const float minimum = *minIt, maximum = *maxIt;
    if (maximum - minimum < 1e-6f) {
        low = minimum; high = maximum;
        if (smoother) smoother->have = false;
        return;
    }
    constexpr int bins = 1024;
    uint32_t histogram[bins]{};
    const float k = float(bins - 1) / (maximum - minimum);
    for (float value : raw) ++histogram[std::clamp(int((value - minimum) * k), 0, bins - 1)];
    const size_t lowCount = size_t(raw.size() * 0.005);
    const size_t highCount = size_t(raw.size() * 0.995);
    size_t accumulated = 0;
    int lowBin = 0, highBin = bins - 1;
    bool foundLow = false;
    for (int i = 0; i < bins; ++i) {
        accumulated += histogram[i];
        if (!foundLow && accumulated > lowCount) { lowBin = i; foundLow = true; }
        if (accumulated >= highCount) { highBin = i; break; }
    }
    float newLow = minimum + lowBin / k;
    float newHigh = minimum + (highBin + 1) / k;
    if (newHigh - newLow < 1e-6f) { newLow = minimum; newHigh = maximum; }
    if (smoother) {
        const float range = smoother->high - smoother->low;
        const bool cut = !smoother->have ||
            (std::fabs(newLow - smoother->low) + std::fabs(newHigh - smoother->high)) > 0.6f * range;
        if (cut) {
            smoother->low = newLow;
            smoother->high = newHigh;
            smoother->have = true;
        } else {
            const float a = std::clamp(1.0f - std::exp(float(-(now - smoother->lastTime) / 0.4)),
                                       0.0f, 1.0f);
            smoother->low += a * (newLow - smoother->low);
            smoother->high += a * (newHigh - smoother->high);
        }
        smoother->lastTime = now;
        low = smoother->low;
        high = smoother->high;
    } else {
        low = newLow;
        high = newHigh;
    }
}
} // namespace vrx
