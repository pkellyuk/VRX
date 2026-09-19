#pragma once
// Depth steadying and two-model fusion: the CPU maths, shared by xrapp5 and its tests.
// A port of bench/xmmodel/xm_fuse.py (see XMMODEL.md), kept numerically close to it:
// the OpenCV operations used there are reproduced here (GaussianBlur with ksize from
// sigma and BORDER_REFLECT, INTER_AREA downscale, pixel-centre INTER_LINEAR upscale,
// bilinear remap with replicated borders).
//
// Images are row-major float, 0..1 near values unless stated. Motion vectors are the
// hardware motion estimator's resolved output: per 8x8 block, (x, y) int16 in quarter
// pels, pointing from the current block to where its content was in the reference.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <execution>
#include <numeric>
#include <vector>

namespace fusion
{

static const int MV_BLOCK = 8;
static const float VERIFY_LO = 0.04f;      // luma mismatch (0..1) still fully trusted
static const float VERIFY_HI = 0.12f;      // luma mismatch treated as a failed match
static const float VERIFY_CUT = 0.5f;      // mean trust below this: trust nothing this frame
static const float TRUST_SIGMA = 4.0f;     // px, smoothing of the luma mismatch
static const float FIT_EPS = 2e-3f;        // guided-filter regulariser
static const int FIT_SCALE = 4;            // the fit runs at 1/4 resolution
static const float FUSE_SIGMA = 16.0f;     // px on the depth grid, per-region fit window
static const float STEADY_ALPHA = 0.5f;    // weight of the moved previous output at full trust

struct Image
{
    int w = 0, h = 0;
    std::vector<float> v;
    Image() = default;
    Image(int width, int height, float fill = 0.0f) : w(width), h(height), v((size_t)width * height, fill) {}
    float& at(int x, int y) { return v[(size_t)y * w + x]; }
    float at(int x, int y) const { return v[(size_t)y * w + x]; }
    bool valid() const { return w > 0 && h > 0 && v.size() == (size_t)w * h; }
};

// Runs fn(row) for every row, in parallel.
template <typename F>
inline void ForRows(int h, F fn)
{
    if (h <= 0) return;
    std::vector<int> rows(h);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), fn);
}

// Runs fn(i) for every element index of a w x h image, in parallel by row.
template <typename F>
inline void ForPixels(int w, int h, F fn)
{
    ForRows(h, [&](int y)
    {
        const size_t begin = (size_t)y * w, end = begin + w;
        for (size_t i = begin; i < end; i++) fn(i);
    });
}

// cv2.getGaussianKernel for float images: ksize = round(sigma * 8 + 1) | 1.
inline std::vector<float> GaussianKernel(float sigma)
{
    if (!(sigma > 0)) return { 1.0f };
    const int k = ((int)std::lround(sigma * 8.0f + 1.0f)) | 1;
    std::vector<double> d(k);
    double sum = 0;
    for (int i = 0; i < k; i++)
    {
        const double x = i - (k - 1) * 0.5;
        d[i] = std::exp(-x * x / (2.0 * sigma * sigma));
        sum += d[i];
    }
    std::vector<float> out(k);
    for (int i = 0; i < k; i++) out[i] = (float)(d[i] / sum);
    return out;
}

// BORDER_REFLECT ("fedcba|abcdefgh|hgfedcb").
inline int Reflect(int i, int n)
{
    if (n == 1) return 0;
    while (i < 0 || i >= n)
    {
        if (i < 0) i = -i - 1;
        if (i >= n) i = 2 * n - i - 1;
    }
    return i;
}

// cv2.GaussianBlur(src, (0, 0), sigma, borderType=BORDER_REFLECT).
inline Image Blur(const Image& src, float sigma)
{
    if (!src.valid()) return {};
    const std::vector<float> k = GaussianKernel(sigma);
    const int w = src.w, h = src.h;
    Image tmp(w, h), out(w, h);
    // Horizontal: reflect-pad each row once, then plain dot products.
    // Locals inside each lambda: captured references would block vectorisation
    // (the compiler cannot prove a write to dst leaves the loop bound unchanged).
    ForRows(h, [&](int y)
    {
        const int w = src.w, r = (int)k.size() / 2, n = (int)k.size();
        std::vector<float> padded((size_t)w + 2 * r);
        const float* row = src.v.data() + (size_t)y * w;
        for (int x = -r; x < w + r; x++) padded[(size_t)x + r] = row[Reflect(x, w)];
        float* dst = tmp.v.data() + (size_t)y * w;
        std::fill(dst, dst + w, 0.0f);
        for (int i = 0; i < n; i++)
        {
            // one tap across the whole row: vectorises without reordering the sums
            const float ki = k[i];
            const float* p = padded.data() + i;
            for (int x = 0; x < w; x++) dst[x] += ki * p[x];
        }
    });
    // Vertical: accumulate whole (reflected) rows, which vectorises across x.
    ForRows(h, [&](int y)
    {
        const int w = src.w, h = src.h, r = (int)k.size() / 2, n = (int)k.size();
        float* dst = out.v.data() + (size_t)y * w;
        std::fill(dst, dst + w, 0.0f);
        for (int i = 0; i < n; i++)
        {
            const float ki = k[i];
            const float* s = tmp.v.data() + (size_t)Reflect(y + i - r, h) * w;
            for (int x = 0; x < w; x++) dst[x] += ki * s[x];
        }
    });
    return out;
}

// cv2.resize INTER_AREA for a downscale: each output pixel is the area-weighted mean
// of the source interval it covers (fractional coverage at both ends).
inline Image AreaDown(const Image& src, int dw, int dh)
{
    if (!src.valid() || dw <= 0 || dh <= 0) return {};
    auto weights = [](int s, int d)
    {
        std::vector<std::vector<std::pair<int, float>>> w(d);
        const double scale = (double)s / d;
        for (int i = 0; i < d; i++)
        {
            const double a = i * scale, b = (i + 1) * scale;
            for (int j = (int)std::floor(a); j < (int)std::ceil(b) && j < s; j++)
            {
                const double cover = std::min(b, (double)j + 1) - std::max(a, (double)j);
                if (cover > 1e-9) w[i].push_back({ j, (float)(cover / scale) });
            }
        }
        return w;
    };
    const auto wx = weights(src.w, dw), wy = weights(src.h, dh);
    Image out(dw, dh);
    ForRows(dh, [&](int y)
    {
        for (int x = 0; x < dw; x++)
        {
            float s = 0;
            for (const auto& [sy, fy] : wy[y])
                for (const auto& [sx, fx] : wx[x]) s += fy * fx * src.at(sx, sy);
            out.at(x, y) = s;
        }
    });
    return out;
}

// cv2.resize INTER_LINEAR: pixel-centre mapping, clamped at the edges.
inline Image LinearResize(const Image& src, int dw, int dh)
{
    if (!src.valid() || dw <= 0 || dh <= 0) return {};
    auto taps = [](int s, int d)
    {
        std::vector<std::pair<int, float>> t(d);
        for (int i = 0; i < d; i++)
        {
            float p = (i + 0.5f) * s / d - 0.5f;
            int i0 = (int)std::floor(p);
            float f = p - i0;
            if (i0 < 0) { i0 = 0; f = 0; }
            if (i0 >= s - 1) { i0 = s - 1; f = 0; }
            t[i] = { i0, f };
        }
        return t;
    };
    const auto tx = taps(src.w, dw), ty = taps(src.h, dh);
    Image out(dw, dh);
    ForRows(dh, [&](int y)
    {
        const int y0 = ty[y].first, y1 = std::min(y0 + 1, src.h - 1);
        const float fy = ty[y].second;
        for (int x = 0; x < dw; x++)
        {
            const int x0 = tx[x].first, x1 = std::min(x0 + 1, src.w - 1);
            const float fx = tx[x].second;
            const float top = src.at(x0, y0) + fx * (src.at(x1, y0) - src.at(x0, y0));
            const float bot = src.at(x0, y1) + fx * (src.at(x1, y1) - src.at(x0, y1));
            out.at(x, y) = top + fy * (bot - top);
        }
    });
    return out;
}

// Bilinear sample with replicated borders.
inline float Sample(const Image& img, float x, float y)
{
    x = std::clamp(x, 0.0f, (float)(img.w - 1));
    y = std::clamp(y, 0.0f, (float)(img.h - 1));
    const int x0 = (int)x, y0 = (int)y;
    const int x1 = std::min(x0 + 1, img.w - 1), y1 = std::min(y0 + 1, img.h - 1);
    const float fx = x - x0, fy = y - y0;
    const float top = img.at(x0, y0) + fx * (img.at(x1, y0) - img.at(x0, y0));
    const float bot = img.at(x0, y1) + fx * (img.at(x1, y1) - img.at(x0, y1));
    return top + fy * (bot - top);
}

// Where each pixel of the current frame was in the reference frame.
struct Motion
{
    Image mapX, mapY;
    bool valid() const { return mapX.valid() && mapY.valid(); }
};

// Hardware vectors (bw x bh blocks, int16 x/y pairs in quarter pels) -> per-pixel
// maps on a w x h grid. Each vector belongs to its block's centre and is spread
// bilinearly, as xm_fuse.load_hw_mv does.
inline Motion MotionFromVectors(const int16_t* vectors, int bw, int bh, int w, int h)
{
    Motion m;
    if (!vectors || bw <= 0 || bh <= 0 || w <= 0 || h <= 0) return m;
    Image vx(bw, bh), vy(bw, bh);
    for (int i = 0; i < bw * bh; i++)
    {
        vx.v[i] = vectors[i * 2 + 0] / 4.0f;
        vy.v[i] = vectors[i * 2 + 1] / 4.0f;
    }
    m.mapX = Image(w, h);
    m.mapY = Image(w, h);
    // Bilinear taps into the block grid (clamped = replicated border), same as Sample.
    struct Tap { int i0, i1; float f; };
    auto taps = [](int n, int blocks)
    {
        std::vector<Tap> t(n);
        for (int i = 0; i < n; i++)
        {
            const float p = std::clamp((i + 0.5f) / MV_BLOCK - 0.5f, 0.0f, (float)(blocks - 1));
            const int i0 = (int)p;
            t[i] = { i0, std::min(i0 + 1, blocks - 1), p - i0 };
        }
        return t;
    };
    const std::vector<Tap> tx = taps(w, bw), ty = taps(h, bh);
    ForRows(h, [&](int y)
    {
        const int width = w, blocksW = bw;
        const Tap row = ty[y];
        const float* vx0 = vx.v.data() + (size_t)row.i0 * blocksW;
        const float* vx1 = vx.v.data() + (size_t)row.i1 * blocksW;
        const float* vy0 = vy.v.data() + (size_t)row.i0 * blocksW;
        const float* vy1 = vy.v.data() + (size_t)row.i1 * blocksW;
        float* mx = m.mapX.v.data() + (size_t)y * width;
        float* my = m.mapY.v.data() + (size_t)y * width;
        for (int x = 0; x < width; x++)
        {
            const Tap c = tx[x];
            const float ax = vx0[c.i0] + c.f * (vx0[c.i1] - vx0[c.i0]), bx = vx1[c.i0] + c.f * (vx1[c.i1] - vx1[c.i0]);
            const float ay = vy0[c.i0] + c.f * (vy0[c.i1] - vy0[c.i0]), by = vy1[c.i0] + c.f * (vy1[c.i1] - vy1[c.i0]);
            mx[x] = x + ax + row.f * (bx - ax);
            my[x] = y + ay + row.f * (by - ay);
        }
    });
    return m;
}

// cv2.remap(src, mapX, mapY, INTER_LINEAR, BORDER_REPLICATE).
inline Image Remap(const Image& src, const Motion& m)
{
    if (!src.valid() || !m.valid() || m.mapX.w != src.w || m.mapX.h != src.h) return {};
    Image out(src.w, src.h);
    ForRows(src.h, [&](int y)
    {
        for (int x = 0; x < src.w; x++) out.at(x, y) = Sample(src, m.mapX.at(x, y), m.mapY.at(x, y));
    });
    return out;
}

// 0..1 per pixel: how well the reference picture, moved by the motion, matches the
// current picture (luma in 0..255). All zero when most of the frame fails.
inline Image MotionTrust(const Image& lumaCur, const Image& lumaRef, const Motion& m, float* meanOut = nullptr)
{
    if (meanOut) *meanOut = 0;
    if (!lumaCur.valid() || !lumaRef.valid() || lumaCur.w != lumaRef.w || lumaCur.h != lumaRef.h) return {};
    Image moved = Remap(lumaRef, m);
    if (!moved.valid()) return {};
    Image diff(lumaCur.w, lumaCur.h);
    ForPixels(diff.w, diff.h, [&](size_t i) { diff.v[i] = std::fabs(lumaCur.v[i] - std::round(moved.v[i])) / 255.0f; });
    Image r = Blur(diff, TRUST_SIGMA);
    std::vector<double> rowSum(r.h);
    ForRows(r.h, [&](int y)
    {
        const int width = r.w;
        float* t = r.v.data() + (size_t)y * width;
        double sum = 0;
        for (int x = 0; x < width; x++)
        {
            t[x] = std::clamp((VERIFY_HI - t[x]) / (VERIFY_HI - VERIFY_LO), 0.0f, 1.0f);
            sum += t[x];
        }
        rowSum[y] = sum;
    });
    const float mean = (float)(std::accumulate(rowSum.begin(), rowSum.end(), 0.0) / r.v.size());
    if (meanOut) *meanOut = mean;
    if (mean < VERIFY_CUT) std::fill(r.v.begin(), r.v.end(), 0.0f);
    return r;
}

// Least-squares d ~ a*z + b over the whole map.
inline void GlobalFit(const Image& z, const Image& d, float& a, float& b)
{
    a = 1; b = 0;
    if (!z.valid() || !d.valid() || z.v.size() != d.v.size()) return;
    // Per-row partial sums in parallel, combined in row order (deterministic).
    std::vector<double> sumZ(z.h), sumD(z.h), sumZZ(z.h), sumZD(z.h);
    ForRows(z.h, [&](int y)
    {
        const int width = z.w;
        const float* zr = z.v.data() + (size_t)y * width;
        const float* dr = d.v.data() + (size_t)y * width;
        double sz = 0, sd = 0, szz = 0, szd = 0;
        for (int x = 0; x < width; x++)
        {
            sz += zr[x]; sd += dr[x];
            szz += (double)zr[x] * zr[x]; szd += (double)zr[x] * dr[x];
        }
        sumZ[y] = sz; sumD[y] = sd; sumZZ[y] = szz; sumZD[y] = szd;
    });
    const double n = (double)z.v.size();
    const double zm = std::accumulate(sumZ.begin(), sumZ.end(), 0.0) / n;
    const double dm = std::accumulate(sumD.begin(), sumD.end(), 0.0) / n;
    const double var = std::accumulate(sumZZ.begin(), sumZZ.end(), 0.0) / n - zm * zm;
    const double cov = std::accumulate(sumZD.begin(), sumZD.end(), 0.0) / n - zm * dm;
    a = (float)(cov / std::max(var, 1e-6));
    b = (float)(dm - a * zm);
}

// Guided filter with z as the guide: a smooth per-region scale/shift so a*z + b ~ d,
// regularised towards the global fit where z is flat. Returns clip(a*z + b, 0, 1).
inline Image GuidedFit(const Image& z, const Image& d, float sigma)
{
    if (!z.valid() || !d.valid() || z.w != d.w || z.h != d.h) return {};
    float ag, bg;
    GlobalFit(z, d, ag, bg);
    const int sw = z.w / FIT_SCALE, sh = z.h / FIT_SCALE;
    const float s = sigma / FIT_SCALE;
    Image zs = AreaDown(z, sw, sh), ds = AreaDown(d, sw, sh);
    Image zz(sw, sh), zd(sw, sh);
    for (size_t i = 0; i < zs.v.size(); i++) { zz.v[i] = zs.v[i] * zs.v[i]; zd.v[i] = zs.v[i] * ds.v[i]; }
    Image mz = Blur(zs, s), md = Blur(ds, s), mzz = Blur(zz, s), mzd = Blur(zd, s);
    Image a(sw, sh), b(sw, sh);
    for (size_t i = 0; i < a.v.size(); i++)
    {
        const float var = mzz.v[i] - mz.v[i] * mz.v[i];
        const float cov = mzd.v[i] - mz.v[i] * md.v[i];
        a.v[i] = (cov + FIT_EPS * ag) / (var + FIT_EPS);
        b.v[i] = md.v[i] - a.v[i] * mz.v[i];
    }
    Image af = LinearResize(Blur(a, s), z.w, z.h), bf = LinearResize(Blur(b, s), z.w, z.h);
    Image out(z.w, z.h);
    ForPixels(z.w, z.h, [&](size_t i) { out.v[i] = std::clamp(af.v[i] * z.v[i] + bf.v[i], 0.0f, 1.0f); });
    return out;
}

// Two-model fusion for the current frame (xm_fuse mc_s16_hwv):
//   z       current fast-model near values
//   anchor  the slower model's near values for an older frame, already moved to the
//           current frame (Remap with the current->anchor motion)
//   trust   MotionTrust for that motion (empty = fully trusted, e.g. the same frame)
//   ga, gb  global fit of the anchor onto the fast model at the anchor's frame
inline Image Fuse(const Image& z, const Image& anchor, const Image& trust, float ga, float gb, float sigma = FUSE_SIGMA)
{
    if (!z.valid() || !anchor.valid() || z.v.size() != anchor.v.size()) return {};
    Image target(z.w, z.h);
    const bool haveTrust = trust.valid() && trust.v.size() == z.v.size();
    ForPixels(z.w, z.h, [&](size_t i)
    {
        const float w = haveTrust ? trust.v[i] : 1.0f;
        const float fallback = std::clamp(ga * z.v[i] + gb, 0.0f, 1.0f);
        target.v[i] = w * anchor.v[i] + (1.0f - w) * fallback;
    });
    return GuidedFit(z, target, sigma);
}

// Steadying (xm_fuse stabilise): the previous output moved to this frame, blended in
// with weight STEADY_ALPHA * trust.
inline Image Steady(const Image& cur, const Image& previousMoved, const Image& trust, float alpha = STEADY_ALPHA)
{
    if (!cur.valid()) return {};
    if (!previousMoved.valid() || !trust.valid() || previousMoved.v.size() != cur.v.size() || trust.v.size() != cur.v.size()) return cur;
    Image out(cur.w, cur.h);
    ForPixels(cur.w, cur.h, [&](size_t i)
    {
        const float w = alpha * trust.v[i];
        out.v[i] = cur.v[i] + w * (previousMoved.v[i] - cur.v[i]);
    });
    return out;
}

// BT.601 luma (cv2 COLOR_RGB2GRAY), rounded, from packed RGBA8 (R in the low byte).
inline Image LumaFromRgba(const uint32_t* rgba, int w, int h)
{
    Image out;
    if (!rgba || w <= 0 || h <= 0) return out;
    out = Image(w, h);
    ForPixels(w, h, [&](size_t i)
    {
        const uint32_t p = rgba[i];
        const float r = (float)(p & 255), g = (float)((p >> 8) & 255), b = (float)((p >> 16) & 255);
        out.v[i] = std::round(0.299f * r + 0.587f * g + 0.114f * b);
    });
    return out;
}

} // namespace fusion
