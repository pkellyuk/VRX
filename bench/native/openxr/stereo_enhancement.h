#pragma once
// Optional stereo post-processing policies and CPU references. The live image
// operations run in compute shaders; these functions keep their tests independent
// of the renderer and make the neutral settings an explicit compatibility contract.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace stereo_enhancement
{
inline float DepthCurve(float n, float amount)
{
    if (!(amount > 0)) return n; // preserve the old floating-point path exactly
    const float a = 2.0f * std::clamp(amount, 0.0f, 1.0f);
    n = std::clamp(n, 0.0f, 1.0f);
    return (1.0f + a) * n / (1.0f + a * n);
}

inline float ConvergenceOffset(float position)
{ return 0.25f * std::clamp(position, -1.0f, 1.0f); }

inline float HeadOffset(float metres)
{ return std::isfinite(metres) ? std::clamp(metres, -0.025f, 0.025f) : 0.0f; }

struct HistoryFrame
{
    uint64_t sourceSeq = 0, layout = 0, depthSeq = 0;
    double sourceTime = 0, depthTime = 0;
};

inline bool Aligned(const HistoryFrame& f)
{
    return f.sourceSeq && f.depthSeq && std::isfinite(f.sourceTime) &&
        std::isfinite(f.depthTime) && std::abs(f.sourceTime - f.depthTime) <= 0.050;
}

inline bool UsableHistory(const HistoryFrame& current, const HistoryFrame& previous)
{
    return Aligned(current) && Aligned(previous) && current.layout == previous.layout &&
        current.sourceSeq > previous.sourceSeq && current.sourceTime > previous.sourceTime &&
        current.sourceTime - previous.sourceTime <= 0.120;
}

inline float RgbAt(const std::vector<unsigned char>& rgb, int w, int h, int x, int y, int channel)
{
    x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1);
    return rgb[((size_t)y * w + x) * 3 + channel] / 255.0f;
}

inline float SampleRgb(const std::vector<unsigned char>& rgb, int w, int h, float x, float y, int channel)
{
    x = std::clamp(x, 0.0f, float(w - 1)); y = std::clamp(y, 0.0f, float(h - 1));
    const int x0 = int(x), y0 = int(y);
    const float a = x - x0, b = y - y0;
    const float top = RgbAt(rgb, w, h, x0, y0, channel) * (1 - a) + RgbAt(rgb, w, h, x0 + 1, y0, channel) * a;
    const float bot = RgbAt(rgb, w, h, x0, y0 + 1, channel) * (1 - a) + RgbAt(rgb, w, h, x0 + 1, y0 + 1, channel) * a;
    return top * (1 - b) + bot * b;
}

inline float GuidedNear(const std::vector<unsigned char>& rgb, int w, int h,
    const std::vector<float>& depth, int dw, int dh, int x, int y)
{
    const float fx = (x + 0.5f) * dw / w - 0.5f, fy = (y + 0.5f) * dh / h - 0.5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    auto at = [&](int xx, int yy) { return depth[(size_t)std::clamp(yy, 0, dh - 1) * dw + std::clamp(xx, 0, dw - 1)]; };
    const float ax = fx - x0, ay = fy - y0;
    const float top = at(x0, y0) + ax * (at(x0 + 1, y0) - at(x0, y0));
    const float bot = at(x0, y0 + 1) + ax * (at(x0 + 1, y0 + 1) - at(x0, y0 + 1));
    const float linear = top + ay * (bot - top);
    float sum = 0, weights = 0, minimum = 1, maximum = 0;
    const int cx = int(std::floor(fx + 0.5f)), cy = int(std::floor(fy + 0.5f));
    for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i)
    {
        const int sx = std::clamp(cx + i, 0, dw - 1), sy = std::clamp(cy + j, 0, dh - 1);
        const float n = at(sx, sy), dx = sx - fx, dy = sy - fy;
        float difference = 0;
        for (int ch = 0; ch < 3; ++ch)
        {
            const float d = RgbAt(rgb, w, h, x, y, ch) - SampleRgb(rgb, w, h,
                (sx + 0.5f) * w / dw - 0.5f, (sy + 0.5f) * h / dh - 0.5f, ch);
            difference += d * d;
        }
        const float weight = std::exp2(-0.75f * (dx * dx + dy * dy) - 32.0f * difference);
        sum += weight * n; weights += weight;
        minimum = std::min(minimum, n); maximum = std::max(maximum, n);
    }
    // Texture cannot invent geometry in a locally flat depth field. Retain some
    // model evidence even at an RGB edge to limit texture-copy and thin-object loss.
    if (maximum - minimum < 0.025f || weights < 1e-6f) return linear;
    return linear + 0.85f * (sum / weights - linear);
}

inline std::vector<unsigned char> Sharpen(const std::vector<unsigned char>& rgb, int w, int h, float amount)
{
    if (!(amount > 0)) return rgb;
    std::vector<unsigned char> out(rgb.size());
    const float gain = 0.35f * std::clamp(amount, 0.0f, 1.0f);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) for (int ch = 0; ch < 3; ++ch)
    {
        const float v = RgbAt(rgb, w, h, x, y, ch);
        const float a = RgbAt(rgb, w, h, x - 1, y, ch), b = RgbAt(rgb, w, h, x + 1, y, ch);
        const float c = RgbAt(rgb, w, h, x, y - 1, ch), d = RgbAt(rgb, w, h, x, y + 1, ch);
        const float lo = std::min({v, a, b, c, d}), hi = std::max({v, a, b, c, d});
        const float detail = std::clamp(v - (a + b + c + d) * 0.25f, -0.12f, 0.12f);
        const float sharpened = std::clamp(v + gain * detail, lo, hi);
        out[((size_t)y * w + x) * 3 + ch] = (unsigned char)std::clamp(int(std::lround(sharpened * 255.0f)), 0, 255);
    }
    return out;
}
}
