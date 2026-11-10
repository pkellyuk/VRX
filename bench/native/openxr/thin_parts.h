#pragma once
// Thin parts that lean (XPOLISH.md, "Thin parts leaning"): a diagnostic, used by --dump.
//
// Both eyes are made from one picture and one depth map, so how far any pixel is moved
// in each eye is known: it is its depth. A thin upright part of something - a rocket's
// spike, an antenna, a sword - should move as one piece. It leans when the depth along
// it changes: the depth map is much coarser than the picture and the model sees less
// and less of the part as it narrows, so its depth fades toward what is behind it, the
// tip moves less than the base, and the two eyes disagree about its angle.
//
// The depth cannot say where the thin part is (that is what it gets wrong), so the
// part is found in the picture: on each row, a narrow bar - two opposite edges a few
// pixels apart with much the same picture either side - and bars on successive rows
// that line up are one part. A part must run for MIN_LENGTH of the picture's height,
// which is short (a castle's spires are 15 to 40 rows of a 1080-row picture), so text
// strokes, scrollbars and window borders are parts too. What tells a spire from those
// is the depth: read along each part, its lean is how much its shift changes from one
// end to the other compared with what is beside it, and CarryDepthAlongParts acts only
// on a part that leans toward something nearer that it is attached to.
//
// Pure CPU, at the picture's own resolution: at half of it the thinnest spires (1 to 2
// px) are no longer found. FindThinParts changes no depth. Tested by playback_test.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <execution>
#include <numeric>
#include <vector>

namespace thin
{
static const float MAX_WIDTH = 0.007f;      // of the picture's width: the widest bar that counts as thin (13 px at 1920)
static const float MIN_LENGTH = 0.012f;     // of the picture's height: the shortest run that is a part (13 rows at 1080)
static const float EDGE = 12.0f;            // of 255: the luma change across 2 px that is one side of a bar (dark spires on dark forest)
static const float SIDES = 0.6f;            // the picture either side of a bar may differ by this share of the bar's own contrast
static const int DRIFT = 2;                 // px: how far a bar's centre may move from one row to the next
static const int GAP = 2;                   // rows a part may be missing for (a highlight, a crossing line)
static const float LEAN_PX = 2.0f;          // px per eye: a part whose shift changes by this much along it is leaning

static const float BESIDE = 6.0f;           // depth pixels: how far outside the part the depth beside it is read
                                            // (beyond the reach of DilateNear, which widens the part's own depth)

// What is tried when finding parts; the defaults are the constants above.
struct Params
{
    float maxWidth = MAX_WIDTH, minLength = MIN_LENGTH, edge = EDGE, sides = SIDES;
};

// nearness: the depth on the part. beside: the mean of the depth BESIDE either side of it.
struct Point { int y; float x; float width; float nearness; float beside; };

struct Part
{
    std::vector<Point> points;      // one per row it was seen on, top to bottom
    bool bright = false;            // brighter than what is beside it
    float lean = 0;                 // px per eye: how much its shift changes along it, compared with what is beside it
    float leanFrom = 0, leanTo = 0; // its nearness above what is beside it (0..1), least and most along it
    float spread = 0;               // px per eye: how much its own shift changes along it (with a tilted surface it lies on)
    bool leaning = false;           // lean >= LEAN_PX
    // Set by CarryDepthAlongParts:
    bool carried = false;           // it was given the depth of what it is attached to
    bool attachedBelow = false;     // ... which is at its bottom end (else its top)
    float target = 0;               // that depth
    int top() const { return points.empty() ? 0 : points.front().y; }
    int bottom() const { return points.empty() ? 0 : points.back().y; }
    int length() const { return points.empty() ? 0 : bottom() - top() + 1; }
};

// Nearness at picture pixel (x, y) from the dw x dh depth grid, as the warp reads it
// (NearAt in kWarpHlsl, without the row snap).
inline float NearAt(const float* near01, int dw, int dh, float x, float y, int w, int h)
{
    const float fx = (x + 0.5f) * (float)dw / (float)w - 0.5f, fy = (y + 0.5f) * (float)dh / (float)h - 0.5f;
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    const int x0 = std::clamp((int)x0f, 0, dw - 1), x1 = std::clamp((int)x0f + 1, 0, dw - 1);
    const int y0 = std::clamp((int)y0f, 0, dh - 1), y1 = std::clamp((int)y0f + 1, 0, dh - 1);
    const float a = near01[(size_t)y0 * dw + x0], b = near01[(size_t)y0 * dw + x1];
    const float c = near01[(size_t)y1 * dw + x0], d = near01[(size_t)y1 * dw + x1];
    const float top = a + tx * (b - a), bot = c + tx * (d - c);
    return top + ty * (bot - top);
}

// The parts' shapes, from the picture alone: the costly half (every row of the picture;
// the rows are searched in parallel). luma: w x h in 0..255. The points' nearness and
// beside, and the lean, are left for MeasureThinParts.
inline std::vector<Part> TraceThinParts(const float* luma, int w, int h, const Params& params = Params())
{
    std::vector<Part> done;
    if (!luma || w < 8 || h < 8) return done;
    const int maxWidth = std::max(3, (int)std::lround(params.maxWidth * w));
    const int minLength = std::max(8, (int)std::lround(params.minLength * h));

    struct Bar { float x; float width; bool bright; };
    std::vector<std::vector<Bar>> rowBars((size_t)h);
    std::vector<int> rows((size_t)h);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y)
    {
        const float* row = luma + (size_t)y * w;
        // Edges: where the change across 2 px is at least EDGE and the largest of its own
        // sign locally (the two sides of a 2 px bar are neighbours, of opposite signs).
        static thread_local std::vector<int> edgeX;
        static thread_local std::vector<float> edgeG;
        edgeX.clear(); edgeG.clear();
        for (int x = 1; x < w - 1; x++)
        {
            const float g = row[x + 1] - row[x - 1];
            if (std::fabs(g) < params.edge) continue;
            float gl = x > 1 ? row[x] - row[x - 2] : 0.0f, gr = x < w - 2 ? row[x + 2] - row[x] : 0.0f;
            if ((gl > 0) != (g > 0)) gl = 0;
            if ((gr > 0) != (g > 0)) gr = 0;
            if (std::fabs(g) < std::fabs(gl) || std::fabs(g) <= std::fabs(gr)) continue;
            edgeX.push_back(x); edgeG.push_back(g);
        }
        // Bars: two neighbouring edges of opposite sign, close together, with much the
        // same picture outside them.
        std::vector<Bar>& bars = rowBars[(size_t)y];
        for (size_t i = 0; i + 1 < edgeX.size(); i++)
        {
            const int xa = edgeX[i], xb = edgeX[i + 1];
            if ((edgeG[i] > 0) == (edgeG[i + 1] > 0) || xb - xa > maxWidth) continue;
            const float left = row[std::max(0, xa - 2)], right = row[std::min(w - 1, xb + 2)];
            const float inside = row[(xa + xb) / 2];
            const float contrast = std::min(std::fabs(inside - left), std::fabs(inside - right));
            if (contrast < params.edge || std::fabs(left - right) > params.sides * contrast) continue;
            bars.push_back({ 0.5f * (float)(xa + xb), (float)(xb - xa), edgeG[i] > 0 });
        }
    });

    struct Open { Part part; float x; int lastY; };
    std::vector<Open> open;
    auto close = [&](Open& o)
    {
        if (o.part.length() >= minLength) done.push_back(std::move(o.part));
    };
    for (int y = 0; y < h; y++)
    {
        // Each bar continues the nearest open part that ended just above it, or starts one.
        for (const Bar& b : rowBars[(size_t)y])
        {
            int best = -1;
            float bestD = (float)DRIFT + 0.5f;
            for (size_t k = 0; k < open.size(); k++)
            {
                if (open[k].lastY == y || open[k].part.bright != b.bright) continue;
                const float d = std::fabs(open[k].x - b.x);
                if (d < bestD) { bestD = d; best = (int)k; }
            }
            const Point p{ y, b.x, b.width, 0.0f, 0.0f };
            if (best >= 0) { open[best].part.points.push_back(p); open[best].x = b.x; open[best].lastY = y; }
            else { Open o; o.part.bright = b.bright; o.part.points.push_back(p); o.x = b.x; o.lastY = y; open.push_back(std::move(o)); }
        }
        // Parts not seen for more than GAP rows have ended.
        size_t keep = 0;
        for (size_t k = 0; k < open.size(); k++)
        {
            if (y - open[k].lastY > GAP) { close(open[k]); continue; }
            if (keep != k) open[keep] = std::move(open[k]);
            keep++;
        }
        open.resize(keep);
    }
    for (Open& o : open) close(o);
    return done;
}

// The depth along each part, and its lean: the cheap half, done again whenever the depth
// changes. parts: from TraceThinParts on a w x h picture. near01: dw x dh, 0 far .. 1
// near. pxPerNear: how many pixels the picture moves in one eye for the whole depth
// range (the warp's scale). The parts are left sorted, the most leaning first.
inline void MeasureThinParts(std::vector<Part>& parts, int w, int h, const float* near01, int dw, int dh, float pxPerNear)
{
    if (!near01 || w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return;
    for (Part& part : parts)
    {
        for (Point& p : part.points)
        {
            const float reach = 0.5f * p.width + BESIDE * (float)w / (float)dw;
            p.nearness = NearAt(near01, dw, dh, p.x, (float)p.y, w, h);
            p.beside = 0.5f * (NearAt(near01, dw, dh, std::max(0.0f, p.x - reach), (float)p.y, w, h) +
                               NearAt(near01, dw, dh, std::min((float)(w - 1), p.x + reach), (float)p.y, w, h));
        }
        // The lean: how much the part's shift changes along it compared with what is
        // beside it. A scrollbar on a window that the depth tilts changes with the window
        // and does not lean; a spike whose depth fades into the sky's does. Medians over
        // 5 rows, so that one odd row does not make it.
        const size_t n = part.points.size();
        float lo = 1e9f, hi = -1e9f, ownLo = 1e9f, ownHi = -1e9f;
        for (size_t i = 0; i < n; i++)
        {
            float v[5], own[5]; int count = 0;
            for (size_t k = (i >= 2 ? i - 2 : 0); k <= std::min(n - 1, i + 2); k++)
            {
                v[count] = part.points[k].nearness - part.points[k].beside;
                own[count++] = part.points[k].nearness;
            }
            std::sort(v, v + count); std::sort(own, own + count);
            lo = std::min(lo, v[count / 2]); hi = std::max(hi, v[count / 2]);
            ownLo = std::min(ownLo, own[count / 2]); ownHi = std::max(ownHi, own[count / 2]);
        }
        part.leanFrom = lo; part.leanTo = hi;
        part.lean = (hi - lo) * pxPerNear;
        part.spread = (ownHi - ownLo) * pxPerNear;
        part.leaning = part.lean >= LEAN_PX;
    }
    std::sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.lean > b.lean; });
}

// Both halves at once.
inline std::vector<Part> FindThinParts(const float* luma, int w, int h, const float* near01, int dw, int dh, float pxPerNear,
    const Params& params = Params())
{
    if (!near01 || dw <= 0 || dh <= 0) return {};
    std::vector<Part> parts = TraceThinParts(luma, w, h, params);
    MeasureThinParts(parts, w, h, near01, dw, dh, pxPerNear);
    return parts;
}

// A thin part takes the depth of what it is attached to. The depth model often does not
// see a thin part at all: a spire has the depth of the mountain behind it, and only its
// foot, inside the reach of DilateNear, is dragged along with the roof - so it bends. A
// part that meets something nearer at one end (the roof under a spire, the ground under
// a post) is given that depth along its whole length, so it moves with it as one piece.
// A part with nothing nearer at either end (a text stroke, a scrollbar: the same depth
// all round) is left alone. Depth is only ever brought nearer.
//
// near01: dw x dh, changed in place. parts: from FindThinParts on a w x h picture;
// `carried`, `attachedBelow` and `target` are set. Returns how many parts were carried.
static const float ATTACH = 0.08f;          // nearness: how much nearer than beside the part the thing at its end must be
static const int ATTACH_REACH = 8;          // depth rows beyond the part's end searched for what it is attached to
static const int ATTACH_ACROSS = 1;         // depth columns either side of the part's end searched
static const float GAP_DEPTH = 0.05f;       // nearness: a part this much farther than what is beside it is a gap, not a thing

inline size_t CarryDepthAlongParts(std::vector<float>& near01, int dw, int dh, std::vector<Part>& parts, int w, int h)
{
    if (dw <= 0 || dh <= 0 || w <= 0 || h <= 0 || near01.size() != (size_t)dw * dh) return 0;
    const std::vector<float> before = near01;       // what is attached is read from the map as it was
    auto gridX = [&](float x) { return (x + 0.5f) * (float)dw / (float)w - 0.5f; };
    auto gridY = [&](float y) { return (y + 0.5f) * (float)dh / (float)h - 0.5f; };
    auto beyond = [&](const Point& end, int direction)      // the nearest depth just past an end
    {
        const int gx = (int)std::lround(gridX(end.x)), gy = (int)std::lround(gridY((float)end.y));
        float best = -1.0f;
        for (int k = 1; k <= ATTACH_REACH; k++)
        {
            const int y = gy + direction * k;
            if (y < 0 || y >= dh) break;
            for (int x = std::max(0, gx - ATTACH_ACROSS); x <= std::min(dw - 1, gx + ATTACH_ACROSS); x++)
                best = std::max(best, before[(size_t)y * dw + x]);
        }
        return best;
    };

    size_t carried = 0;
    std::vector<float> beside;
    for (Part& part : parts)
    {
        part.carried = false;
        if (part.points.size() < 2) continue;
        beside.clear();
        for (const Point& p : part.points) beside.push_back(p.beside);
        std::nth_element(beside.begin(), beside.begin() + beside.size() / 2, beside.end());
        const float around = beside[beside.size() / 2];
        const float above = beyond(part.points.front(), -1), below = beyond(part.points.back(), +1);
        part.attachedBelow = below >= above;
        part.target = std::max(above, below);
        if (part.target - around < ATTACH) continue;
        // Only a part that leans, is nowhere farther than what is beside it (that is a
        // gap between two things, not a thing), and is nearest at the attached end:
        // the foot of a spire, dragged along by its roof.
        if (!part.leaning || part.leanFrom < -GAP_DEPTH) continue;
        const size_t n = part.points.size(), third = std::max<size_t>(1, n / 3);
        // (the most it stands out in the third at each end: the drag is often only the
        // last few rows of the foot)
        float nearEnd = -1e9f, farEnd = -1e9f;
        for (size_t i = 0; i < third; i++)
        {
            const Point& a = part.points[i];
            const Point& b = part.points[n - 1 - i];
            float& top = part.attachedBelow ? farEnd : nearEnd;
            float& bottom = part.attachedBelow ? nearEnd : farEnd;
            top = std::max(top, a.nearness - a.beside);
            bottom = std::max(bottom, b.nearness - b.beside);
        }
        if (nearEnd - farEnd < 0.5f * (part.leanTo - part.leanFrom)) continue;

        for (const Point& p : part.points)
        {
            const float half = 0.5f * p.width + 1.0f;
            const int x0 = std::max(0, (int)std::floor(gridX(p.x - half))), x1 = std::min(dw - 1, (int)std::floor(gridX(p.x + half)) + 1);
            const int y0 = std::max(0, (int)std::floor(gridY((float)p.y))), y1 = std::min(dh - 1, y0 + 1);
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                {
                    float& v = near01[(size_t)y * dw + x];
                    v = std::max(v, part.target);
                }
        }
        // ...and the few rows between its end and what it is attached to (the part ends
        // where it widens into the roof), so that there is no step left at its foot.
        {
            const Point& end = part.attachedBelow ? part.points.back() : part.points.front();
            const int gx = (int)std::lround(gridX(end.x)), gy = (int)std::lround(gridY((float)end.y));
            for (int k = 1; k <= ATTACH_REACH; k++)
            {
                const int y = gy + (part.attachedBelow ? k : -k);
                if (y < 0 || y >= dh) break;
                for (int x = std::max(0, gx - ATTACH_ACROSS); x <= std::min(dw - 1, gx + ATTACH_ACROSS); x++)
                {
                    float& v = near01[(size_t)y * dw + x];
                    v = std::max(v, part.target);
                }
            }
        }
        part.carried = true;
        carried++;
    }
    return carried;
}

// The picture in grey with the parts drawn over it, RGBA8, w x h: red where a part
// leans (brighter the more), green where it does not.
inline std::vector<unsigned char> MarkThinParts(const float* luma, int w, int h, const std::vector<Part>& parts)
{
    std::vector<unsigned char> out((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++)
    {
        const unsigned char g = (unsigned char)std::clamp(luma[i] * 0.5f, 0.0f, 255.0f);
        out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = g; out[i * 4 + 3] = 255;
    }
    for (const Part& part : parts)
        for (const Point& p : part.points)
        {
            const int half = std::max(1, (int)std::lround(p.width * 0.5f)) + 1;
            for (int x = (int)p.x - half; x <= (int)p.x + half; x++)
            {
                if (x < 0 || x >= w) continue;
                unsigned char* o = out.data() + ((size_t)p.y * w + x) * 4;
                if (part.leaning) { o[0] = 255; o[1] = (unsigned char)std::clamp(160.0f - part.lean * 16.0f, 0.0f, 160.0f); o[2] = 0; }
                else { o[0] = 0; o[1] = 200; o[2] = 60; }
            }
        }
    return out;
}
}
