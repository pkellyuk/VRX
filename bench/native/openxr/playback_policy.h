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

// UsableDepth, plus a grace after a quiet gap. On a still picture frames come only now
// and then (a blinking text cursor), each more than maxLag after the one the depth came
// from, so by source timestamps alone every such frame was shown flat for the one to
// three frames the worker needs to publish its depth: the whole screen twitched. Depth
// that was up to date when a newer frame arrived stays usable for maxLag from then (by
// the caller's clock), while the worker catches up. A worker that never catches up
// still falls back when the grace ends, and a failed one at once.
struct DepthGrace
{
    bool current = false;       // at the last call the depth was of the frame shown
    double until = -1;          // the grace ends at this time
};

inline bool UsableDepthAfterQuiet(DepthGrace& g, double now, bool healthy, uint64_t colourSeq, double colourTime,
    uint64_t depthSeq, double depthTime, double maxLag = 0.25)
{
    if (!healthy || !colourSeq || !depthSeq) { g = {}; return false; }
    if (colourSeq == depthSeq) { g.current = true; g.until = -1; return true; }
    if (g.current) g.until = now + maxLag;      // a newer frame has just arrived
    g.current = false;
    return UsableDepth(healthy, colourSeq, colourTime, depthSeq, depthTime, maxLag) || now <= g.until;
}
