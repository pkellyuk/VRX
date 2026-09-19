#pragma once
// Game-frame timing (see XSYNC.md). Which captured frame the render loop shows with
// the latest completed depth:
//   Latest   the newest frame (default): smooth and immediate, but depth lags it.
//   Delayed  the game stream held back by the measured depth delay, so the newest
//            frame shown is about as old as the newest depth: each depth result
//            lines up with one frame, the frames between reuse it, and the game still
//            updates at the capture rate. Adds roughly the depth delay.
//   Matched  the frame the depth came from (frame matching): exact, but the game only
//            updates at the depth rate.
// Pure policy, no graphics: unit-tested in playback_test.
#include <algorithm>
#include <cstdint>
#include <vector>

enum class FrameTiming { Latest = 0, Delayed = 1, Matched = 2 };

inline const char* FrameTimingName(FrameTiming t)
{
    return t == FrameTiming::Delayed ? "delayed to depth" : t == FrameTiming::Matched ? "matched to depth" : "latest frame";
}

// Smoothed delay between a frame being captured and its depth being published.
struct DepthDelayEstimate
{
    double value = 0;
    bool have = false;
    void Update(double sampleSeconds)
    {
        if (!(sampleSeconds >= 0) || sampleSeconds > 1.0) return;     // ignore stalls and clock oddities
        value = have ? value + 0.2 * (sampleSeconds - value) : sampleSeconds;
        have = true;
    }
};

struct TimedFrame
{
    uint64_t seq = 0;
    double time = 0;
};

// history: recent frames, oldest first. Returns the index to show in Delayed mode:
// the newest frame captured at or before now - delay, or the oldest kept frame when
// none is that old yet. -1 when history is empty.
inline int PickDelayedFrame(const std::vector<TimedFrame>& history, double now, double delay)
{
    if (history.empty()) return -1;
    const double target = now - std::max(0.0, delay);
    int pick = 0;
    for (int i = 0; i < (int)history.size(); i++)
        if (history[i].time <= target) pick = i;
    return pick;
}

// How many frames to keep for Delayed mode: the frames newer than now - delay, plus
// one frame at or before it (the one PickDelayedFrame shows). Returns how many of
// the oldest to drop, never all of them, and at most maxKeep kept.
inline size_t DelayedHistoryExcess(const std::vector<TimedFrame>& history, double now, double delay, size_t maxKeep)
{
    if (history.size() <= 1) return 0;
    const double target = now - std::max(0.0, delay);
    size_t drop = 0;
    // Oldest frames not needed: a newer frame is already at or before the target.
    while (drop + 1 < history.size() && history[drop + 1].time <= target) drop++;
    if (history.size() - drop > maxKeep) drop = history.size() - maxKeep;
    return drop;
}
