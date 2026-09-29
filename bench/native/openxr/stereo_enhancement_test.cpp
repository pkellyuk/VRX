#include "stereo_enhancement.h"
#include <cstdio>
#include <limits>

int main()
{
    using namespace stereo_enhancement;
    int failed = 0;
    auto check = [&](bool value, const char* name) { if (!value) { ++failed; std::printf("FAIL %s\n", name); } };
    for (int a = 0; a <= 100; ++a)
    {
        float previous = -1;
        for (int i = 0; i <= 10000; ++i)
        {
            const float n = i / 10000.0f, mapped = DepthCurve(n, a / 100.0f);
            check(mapped >= 0 && mapped <= 1 && mapped > previous, "curve is bounded and strictly increasing");
            if (a == 0) check(mapped == n, "neutral curve preserves exact depth");
            previous = mapped;
        }
        check(DepthCurve(0, a / 100.0f) == 0 && DepthCurve(1, a / 100.0f) == 1, "curve keeps endpoints");
    }
    check(DepthCurve(.25f, 1) - DepthCurve(.15f, 1) > .1f, "curve expands low-mid separation");
    check(ConvergenceOffset(0) == 0 && ConvergenceOffset(10) == .25f && ConvergenceOffset(-10) == -.25f,
        "convergence is neutral and bounded");
    check(HeadOffset(0) == 0 && HeadOffset(.2f) == .025f && HeadOffset(-.2f) == -.025f &&
        HeadOffset(std::numeric_limits<float>::quiet_NaN()) == 0, "parallax bounds invalid or excessive tracking");
    HistoryFrame old{1, 7, 1, 1.0, 1.0}, now{2, 7, 2, 1.04, 1.04};
    check(UsableHistory(now, old), "aligned recent history accepted");
    check(!UsableHistory(old, now) && !UsableHistory(now, now), "reverse and unchanged frames rejected");
    auto invalid = now; invalid.layout++;
    check(!UsableHistory(invalid, old), "layout switch invalidates history");
    invalid = now; invalid.sourceTime = invalid.depthTime = 1.2;
    check(!UsableHistory(invalid, old), "old history rejected");
    invalid = now; invalid.depthTime = .9;
    check(!UsableHistory(invalid, old), "misaligned colour-depth rejected");
    const int w = 32, h = 16, dw = 8, dh = 4;
    std::vector<unsigned char> rgb(w * h * 3);
    std::vector<float> flat(dw * dh, .4f), depth(dw * dh);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) for (int c = 0; c < 3; ++c)
        rgb[(y * w + x) * 3 + c] = (unsigned char)(x < w / 2 ? 30 : 220);
    for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) depth[y * dw + x] = x < dw / 2 ? .15f : .85f;
    check(std::abs(GuidedNear(rgb, w, h, flat, dw, dh, 15, 8) - .4f) < 1e-6f, "colour edges cannot invent flat-field geometry");
    check(GuidedNear(rgb, w, h, depth, dw, dh, 15, 8) < .3f && GuidedNear(rgb, w, h, depth, dw, dh, 16, 8) > .7f,
        "guided upscale follows a matching RGB silhouette");
    check(Sharpen(rgb, w, h, 0) == rgb, "neutral sharpening is byte exact");
    const auto sharp = Sharpen(rgb, w, h, 1);
    check(sharp == rgb, "sharpening does not overshoot flat regions or hard edges");
    std::printf("Stereo enhancement policies: %s (%d failures)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
