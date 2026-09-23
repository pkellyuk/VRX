#pragma once
#include "synthetic_scene.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace vrx {
inline constexpr int kStereoRowPitch = 2816;

// Forward stereo warp for one eye. Every SOURCE pixel is moved by its OWN
// disparity and z-tested at the destination, so near content really shifts and
// occludes what is behind it. (The earlier backward warp looked up disparity at
// the destination pixel, which merely trims the edges of a flat-coloured object
// instead of moving it - and lets far content overwrite near content.)
// Disocclusion holes are filled from the FARTHER neighbour, i.e. background.
//   eyeOffset : eye's lateral offset in metres (left negative)
//   outRGBA   : kStereoRowPitch bytes per row;  outDepth : kStereoRowPitch/4 floats per row.
//   Depth is written as a real D3D projective depth for the distance the warp
//   used, d = farZ/(farZ-nearZ) * (1 - nearZ/Z), so that the nearZ/farZ declared in
//   XrCompositionLayerDepthInfoKHR decode it back to the same metres. (Writing
//   "1 - nearness" linearly, as the first version did, decodes to 0.1-1 m for
//   almost the whole range - a depth image that contradicts the stereo disparity.)
// Hole-fill modes for disocclusions (the strip of background a near object uncovers
// when it shifts). Both fill from the FARTHER neighbour, i.e. the background side.
//   kFillStretch : repeat that one background pixel across the hole. Cheap, but a
//                  hole up to ~30 px wide becomes a flat horizontal streak, and if
//                  the edge pixel is tinted by the foreground the streak is too.
//   kFillMirror  : reflect the background texture outward about the hole's edge
//                  (dest anchor+k takes source anchorSrc-/+k), so the hole carries
//                  real texture that continues across the seam. A guard stops the
//                  reflection from pulling in anything nearer than the anchor
//                  (another foreground object): it holds the last good pixel.
inline constexpr int kFillStretch = 0;
inline constexpr int kFillMirror = 1;
inline constexpr float kMirrorTol = 0.08f;      // nearness units (0 far .. 1 near)

// Fills dest hole [x, r) of one row. src[] holds scatter results (>= 0) and is
// written with -2 - source for filled pixels. leftSrc/rightSrc are the sources of
// dest x-1 and dest r (-1 if outside the row); holes are maximal runs, so both
// neighbours are scatter hits, never fills.
inline void FillHole(std::vector<int>& src, const float* nrow, int w, int x, int r,
                     int leftSrc, int rightSrc, int fillMode)
{
    if (!nrow) return;
    if (x < 0 || r > w || x >= r) return;

    if (leftSrc < 0 && rightSrc < 0)
    {
        for (int h = x; h < r; h++) src[h] = -2 - x;     // whole row empty: as the original fill
        return;
    }

    const bool useLeft = leftSrc >= 0 && !(rightSrc >= 0 && nrow[rightSrc] < nrow[leftSrc]);
    const int anchor = useLeft ? leftSrc : rightSrc;

    if (fillMode != kFillMirror)
    {
        for (int h = x; h < r; h++) src[h] = -2 - anchor;
        return;
    }

    // walk outward from the anchor so the guard's "last good" propagates correctly
    const float anchorNear = nrow[anchor];
    int good = anchor;
    const int n = r - x;
    for (int k = 1; k <= n; k++)
    {
        const int h = useLeft ? (x - 1 + k) : (r - k);
        int cand = useLeft ? anchor - k : anchor + k;
        cand = cand < 0 ? 0 : (cand >= w ? w - 1 : cand);
        if (nrow[cand] <= anchorNear + kMirrorTol) good = cand;
        src[h] = -2 - good;
    }
}

// subpixel: keep the fractional part of each shift. Whole-pixel shifts quantise a
// smoothly receding surface into ~25 flat bands with a 1 px step between them, which
// reads as ridges ("ploughed field") on uniform texture; sampling the colour at the
// fractional source position that lands on this destination removes them.
inline void WarpEyeFill(const std::vector<unsigned char>& scene, const std::vector<float>& near01,
                        float eyeOffset, float focalPx, float scale, float invZNear, float invZFar,
                        float nearZ, float farZ, bool doWarp, int fillMode,
                        unsigned char* outRGBA, float* outDepth, bool subpixel = false)
{
    if (!outRGBA || !outDepth) return;
    if (scene.size() != (std::size_t)kSyntheticWidth * kSyntheticHeight * 3 || near01.size() != (std::size_t)kSyntheticWidth * kSyntheticHeight) return;

    std::vector<int> src(kSyntheticWidth);
    std::vector<float> srcDest(kSyntheticWidth);            // where the chosen source pixel really lands
    for (int y = 0; y < kSyntheticHeight; y++)
    {
        const float* nrow = near01.data() + (std::size_t)y * kSyntheticWidth;
        std::fill(src.begin(), src.end(), -1);

        for (int x = 0; x < kSyntheticWidth; x++)
        {
            int dx = x;
            float destF = (float)x;
            if (doWarp)
            {
                // Content at distance Z sits at -focal*E/Z in the eye's image
                // relative to the cyclopean image (left eye: shifted right).
                float invZ = invZFar + nrow[x] * (invZNear - invZFar);
                const float s = scale * focalPx * eyeOffset * invZ;
                destF = (float)x - s;
                dx = x - (int)std::lround(s);
            }
            if (dx < 0 || dx >= kSyntheticWidth) continue;
            if (src[dx] < 0 || nrow[x] > nrow[src[dx]]) { src[dx] = x; srcDest[dx] = destF; }
        }

        // holes: maximal runs of unwritten dest pixels
        int lastValid = -1;
        for (int x = 0; x < kSyntheticWidth; x++)
        {
            if (src[x] >= 0) { lastValid = src[x]; continue; }
            int r = x + 1;
            while (r < kSyntheticWidth && src[r] < 0) r++;
            int rightValid = r < kSyntheticWidth ? src[r] : -1;
            FillHole(src, nrow, kSyntheticWidth, x, r, lastValid, rightValid, fillMode);
            x = r - 1;
        }

        unsigned char* crow = outRGBA + (std::size_t)y * kStereoRowPitch;
        float* drow = outDepth + (std::size_t)y * (kStereoRowPitch / 4);
        for (int x = 0; x < kSyntheticWidth; x++)
        {
            const bool filled = src[x] < 0;
            int s = filled ? -2 - src[x] : src[x];
            // Filled pixels have no continuous mapping, so they stay whole-pixel.
            // srcDest is indexed by DESTINATION: where the pixel written here landed.
            const float pos = (subpixel && !filled) ?
                std::min(std::max((float)s + ((float)x - srcDest[x]), 0.0f), (float)(kSyntheticWidth - 1)) : (float)s;
            const int i0 = (int)pos, i1 = i0 + 1 < kSyntheticWidth ? i0 + 1 : kSyntheticWidth - 1;
            const float fr = pos - i0;
            const std::size_t a = ((std::size_t)y * kSyntheticWidth + i0) * 3, b = ((std::size_t)y * kSyntheticWidth + i1) * 3;
            // Same arithmetic as the shader: UNORM values, lerp as c0 + f*(c1-c0),
            // then the UNORM store's round-to-nearest.
            for (int ch = 0; ch < 3; ch++)
            {
                const float c0 = scene[a + ch] / 255.0f, c1 = scene[b + ch] / 255.0f;
                crow[x * 4 + ch] = (unsigned char)std::lround((c0 + fr * (c1 - c0)) * 255.0f);
            }
            crow[x * 4 + 3] = 255;
            float invZ = invZFar + nrow[s] * (invZNear - invZFar);
            drow[x] = (farZ / (farZ - nearZ)) * (1.0f - nearZ * invZ);
        }
    }
}

// The original stretch-fill warp (xrapp3 / xrapp4 and their shader).
inline void WarpEye(const std::vector<unsigned char>& scene, const std::vector<float>& near01,
                    float eyeOffset, float focalPx, float scale, float invZNear, float invZFar,
                    float nearZ, float farZ,
                    bool doWarp, unsigned char* outRGBA, float* outDepth)
{
    WarpEyeFill(scene, near01, eyeOffset, focalPx, scale, invZNear, invZFar, nearZ, farZ, doWarp,
                kFillStretch, outRGBA, outDepth);
}

} // namespace vrx
