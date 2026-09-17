#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

struct FitRect { float x = 0, y = 0, width = 0, height = 0; };

inline FitRect FitCapture(int sourceW, int sourceH, int targetW, int targetH)
{
    if (sourceW <= 0 || sourceH <= 0 || targetW <= 0 || targetH <= 0) return {};
    const float scale = std::min(float(targetW) / sourceW, float(targetH) / sourceH);
    const float w = sourceW * scale, h = sourceH * scale;
    return { (targetW - w) * 0.5f, (targetH - h) * 0.5f, w, h };
}

// Compare source timestamps, not time since inference finished. A static frame
// remains valid indefinitely. Newer colour may use depth up to 250 ms behind it.
inline bool UsableDepth(bool healthy, uint64_t colourSeq, double colourTime,
    uint64_t depthSeq, double depthTime, double maxLag = 0.25)
{
    if (!healthy || !colourSeq || !depthSeq) return false;
    if (colourSeq == depthSeq) return true;
    return std::isfinite(colourTime) && std::isfinite(depthTime) &&
        std::abs(colourTime - depthTime) <= maxLag;
}
