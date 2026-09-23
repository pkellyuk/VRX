#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

// Shared synthetic source and known depth for Windows and Linux render checks.
namespace vrx {
inline constexpr int kSyntheticWidth = 686;
inline constexpr int kSyntheticHeight = 392;

// ------------------------------------------------------------------- scene
// A synthetic scene with unambiguous depth structure: far gradient backdrop,
// a mid-depth panel, and a near marker that moves so the parallax is obvious.
inline void MakeScene(std::vector<unsigned char>& rgb, double t)
{
    rgb.resize((std::size_t)kSyntheticWidth * kSyntheticHeight * 3);
    int markerX = (int)((0.5 + 0.35 * std::sin(t * 0.8)) * (kSyntheticWidth - 120));

    for (int y = 0; y < kSyntheticHeight; y++)
    {
        for (int x = 0; x < kSyntheticWidth; x++)
        {
            unsigned char r, g, b;
            // backdrop: dim blue-grey gradient (far)
            float k = 0.35f + 0.25f * ((float)x / kSyntheticWidth);
            r = (unsigned char)(60 * k); g = (unsigned char)(70 * k); b = (unsigned char)(110 * k);

            // mid panel
            if (x > kSyntheticWidth * 0.18 && x < kSyntheticWidth * 0.52 && y > kSyntheticHeight * 0.22 && y < kSyntheticHeight * 0.78)
            { r = 190; g = 150; b = 60; }

            // near marker (moves)
            if (x >= markerX && x < markerX + 120 && y > kSyntheticHeight * 0.35 && y < kSyntheticHeight * 0.65)
            { r = 235; g = 235; b = 235; }

            std::size_t i = ((std::size_t)y * kSyntheticWidth + x) * 3;
            rgb[i + 0] = r; rgb[i + 1] = g; rgb[i + 2] = b;
        }
    }
}

// Ground-truth "nearness" (0 = far, 1 = near) for MakeScene. Flat-coloured
// rectangles carry no monocular depth cues, so the model's answer for that scene
// is arbitrary; --truth substitutes this so the warp can be judged on its own.
inline void MakeTruth(std::vector<float>& near01, double t)
{
    near01.assign((std::size_t)kSyntheticWidth * kSyntheticHeight, 0.0f);
    int markerX = (int)((0.5 + 0.35 * std::sin(t * 0.8)) * (kSyntheticWidth - 120));

    for (int y = 0; y < kSyntheticHeight; y++)
    {
        for (int x = 0; x < kSyntheticWidth; x++)
        {
            float n = 0.0f;
            if (x > kSyntheticWidth * 0.18 && x < kSyntheticWidth * 0.52 && y > kSyntheticHeight * 0.22 && y < kSyntheticHeight * 0.78) n = 0.5f;
            if (x >= markerX && x < markerX + 120 && y > kSyntheticHeight * 0.35 && y < kSyntheticHeight * 0.65) n = 1.0f;
            near01[(std::size_t)y * kSyntheticWidth + x] = n;
        }
    }
}

// Mean nearness inside each region of MakeScene - tells us in the log what the
// model actually thinks of the synthetic scene.
inline void RegionMeans(const std::vector<float>& near01, double t, float& back, float& panel, float& marker)
{
    std::vector<float> truth;
    MakeTruth(truth, t);
    double s[3] = { 0, 0, 0 }; std::size_t n[3] = { 0, 0, 0 };
    for (std::size_t i = 0; i < truth.size(); i++)
    {
        int k = truth[i] > 0.75f ? 2 : (truth[i] > 0.25f ? 1 : 0);
        s[k] += near01[i]; n[k]++;
    }
    back = n[0] ? (float)(s[0] / n[0]) : 0;
    panel = n[1] ? (float)(s[1] / n[1]) : 0;
    marker = n[2] ? (float)(s[2] / n[2]) : 0;
}

} // namespace vrx
