// fusion_golden: checks the C++ fusion maths (openxr/depth_fusion.h) against the Python
// reference (bench/xmmodel/xm_fuse.py) on one real frame exported by xm_golden.py.
//
// usage: fusion_golden.exe <golden_dir>      exit code 0 = all within tolerance
#include "../openxr/depth_fusion.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

static bool ReadRaw(const std::string& path, void* dst, size_t bytes)
{
    if (!dst) return false;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { printf("[fusion_golden] missing %s\n", path.c_str()); return false; }
    if ((size_t)f.tellg() != bytes) { printf("[fusion_golden] %s is %lld bytes, expected %zu\n", path.c_str(), (long long)f.tellg(), bytes); return false; }
    f.seekg(0);
    f.read((char*)dst, (std::streamsize)bytes);
    return (bool)f;
}

static bool LoadImage(const std::string& dir, const char* name, int w, int h, fusion::Image& out)
{
    out = fusion::Image(w, h);
    return ReadRaw(dir + "\\" + name, out.v.data(), out.v.size() * sizeof(float));
}

// Mean and max absolute difference; passes when both are under the limits.
static bool Compare(const char* what, const fusion::Image& got, const fusion::Image& want, float meanLimit, float maxLimit)
{
    if (!got.valid() || got.v.size() != want.v.size()) { printf("[fusion_golden] %-12s FAIL: no output\n", what); return false; }
    double sum = 0; float worst = 0;
    for (size_t i = 0; i < got.v.size(); i++)
    {
        const float d = std::fabs(got.v[i] - want.v[i]);
        sum += d;
        worst = std::max(worst, d);
    }
    const float mean = (float)(sum / got.v.size());
    const bool ok = mean <= meanLimit && worst <= maxLimit;
    printf("[fusion_golden] %-12s %s  mean |diff| %.6f (limit %.4f)  max %.5f (limit %.3f)\n", what, ok ? "PASS" : "FAIL", mean, meanLimit, worst, maxLimit);
    return ok;
}

int main(int argc, char** argv)
{
    if (argc < 2 || !argv[1]) { printf("usage: fusion_golden.exe <golden_dir>\n"); return 2; }
    const std::string dir = argv[1];
    const int w = 686, h = 392, bw = (w + 7) / 8, bh = (h + 7) / 8;
    printf("[fusion_golden] enter %s\n", dir.c_str());

    fusion::Image z, zPrev, anchor, lumaCur, lumaAnchor, lumaPrev, expTrust, expFused, expTrustPrev, expSteady;
    std::vector<int16_t> vecAnchor((size_t)bw * bh * 2), vecPrev((size_t)bw * bh * 2);
    float ga = 1, gb = 0;
    std::ifstream fit(dir + "\\fit.txt");
    if (!(fit >> ga >> gb)) { printf("[fusion_golden] FAIL fit.txt\n"); return 1; }
    if (!LoadImage(dir, "z.f32", w, h, z) || !LoadImage(dir, "z_prev.f32", w, h, zPrev) || !LoadImage(dir, "anchor.f32", w, h, anchor) ||
        !LoadImage(dir, "luma_cur.f32", w, h, lumaCur) || !LoadImage(dir, "luma_anchor.f32", w, h, lumaAnchor) ||
        !LoadImage(dir, "luma_prev.f32", w, h, lumaPrev) || !LoadImage(dir, "expect_trust.f32", w, h, expTrust) ||
        !LoadImage(dir, "expect_fused.f32", w, h, expFused) || !LoadImage(dir, "expect_trust_prev.f32", w, h, expTrustPrev) ||
        !LoadImage(dir, "expect_steady.f32", w, h, expSteady) ||
        !ReadRaw(dir + "\\vec_anchor.i16", vecAnchor.data(), vecAnchor.size() * 2) ||
        !ReadRaw(dir + "\\vec_prev.i16", vecPrev.data(), vecPrev.size() * 2)) return 1;

    const auto t0 = std::chrono::steady_clock::now();
    fusion::Motion toAnchor = fusion::MotionFromVectors(vecAnchor.data(), bw, bh, w, h);
    float trustMean = 0;
    fusion::Image trust = fusion::MotionTrust(lumaCur, lumaAnchor, toAnchor, &trustMean);
    fusion::Image fused = fusion::Fuse(z, fusion::Remap(anchor, toAnchor), trust, ga, gb);
    const auto t1 = std::chrono::steady_clock::now();
    fusion::Motion toPrev = fusion::MotionFromVectors(vecPrev.data(), bw, bh, w, h);
    fusion::Image trustPrev = fusion::MotionTrust(lumaCur, lumaPrev, toPrev);
    fusion::Image steady = fusion::Steady(z, fusion::Remap(zPrev, toPrev), trustPrev);
    const auto t2 = std::chrono::steady_clock::now();
    printf("[fusion_golden] trust mean %.3f; fuse %.2f ms, steady %.2f ms (CPU)\n", trustMean,
        std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());

    // Per-step cost, to see where the time goes.
    auto timeIt = [](const char* what, auto fn)
    {
        const auto a = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; i++) fn();
        printf("[fusion_golden]   %-22s %.3f ms\n", what, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count() / 20);
    };
    timeIt("MotionFromVectors", [&] { fusion::MotionFromVectors(vecAnchor.data(), bw, bh, w, h); });
    timeIt("Remap", [&] { fusion::Remap(anchor, toAnchor); });
    timeIt("Blur sigma 4 full", [&] { fusion::Blur(z, 4.0f); });
    timeIt("MotionTrust", [&] { fusion::MotionTrust(lumaCur, lumaAnchor, toAnchor); });
    timeIt("AreaDown", [&] { fusion::AreaDown(z, w / 4, h / 4); });
    timeIt("LinearResize", [&] { fusion::LinearResize(fusion::AreaDown(z, w / 4, h / 4), w, h); });
    timeIt("GuidedFit", [&] { fusion::GuidedFit(z, anchor, 16.0f); });

    // OpenCV rounds remap coordinates to 1/32 px and the luma of moved pixels to
    // uint8, so a few edge pixels may differ; the means must be tiny.
    bool ok = Compare("trust", trust, expTrust, 0.005f, 0.30f);
    ok &= Compare("fused", fused, expFused, 0.002f, 0.08f);
    ok &= Compare("trust_prev", trustPrev, expTrustPrev, 0.005f, 0.30f);
    ok &= Compare("steady", steady, expSteady, 0.002f, 0.08f);
    printf("[fusion_golden] exit %s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
