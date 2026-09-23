#include "depth_range.h"
#include <cassert>
#include <cmath>
#include <vector>

int main() {
    std::vector<float> raw(1000);
    for (int i = 0; i < 1000; ++i) raw[i] = float(i) / 999.0f;
    vrx::RangeSmoother smoother;
    float low = 0.0f, high = 0.0f;
    vrx::DepthRange(raw, &smoother, 1.0, low, high);
    const float initialLow = low, initialHigh = high;
    for (auto& value : raw) value += 0.1f;
    vrx::DepthRange(raw, &smoother, 1.04, low, high);
    assert(low > initialLow && low < initialLow + 0.05f);
    assert(high > initialHigh && high < initialHigh + 0.05f);
    for (auto& value : raw) value += 2.0f;
    vrx::DepthRange(raw, &smoother, 1.08, low, high);
    assert(low > 2.0f && high > 3.0f);
    assert(std::fabs(low - smoother.low) < 1e-6f);
}
