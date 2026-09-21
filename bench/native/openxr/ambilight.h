#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

// Ambilight: the colours at the edge of the picture spread out around the screen,
// like the bias lighting behind a television. It is one extra quad layer, a little
// larger than the screen and submitted behind it, holding a small texture:
//
//   * every glow pixel finds the nearest point on the screen's border and takes
//     the average of a patch of the picture just inside it, so each side glows
//     with its own colour;
//   * brightness falls off with the distance outside the screen, measured in
//     METRES so the corners are not stretched, and reaches zero at the edge of
//     the layer;
//   * right against the screen it starts dark and rises over a thin "bezel"
//     (kAmbiBezel of the margin). A glow at full brightness touching the screen
//     made its edge pixels flicker in play: the compositor's filtering of the
//     screen's border then mixes game and glow, and the warped edge columns
//     change from frame to frame. Against a dark bezel neither shows;
//   * the colour is stored already multiplied by that alpha, which is what a
//     compositor wants for a blended layer and is also exactly right if the
//     runtime ignores the alpha, because the world behind the screen is black;
//   * each frame is blended into the previous glow, so the surround drifts with
//     the scene instead of flickering with it.
//
// The taps are whole source pixels rather than filtered samples, so the CPU
// reference below and the shader agree to the last bit (SelfTestAmbilight).

static const float kAmbiMargin = 0.22f;     // glow width, as a fraction of the screen's width
static const int kAmbiTaps = 5;             // blur taps per axis (kAmbiTaps^2 in total)
static const float kAmbiIntensity = 0.85f;  // brightness just outside the picture
static const float kAmbiBlend = 0.12f;      // per frame, towards the new glow
static const float kAmbiBlurFraction = 0.05f;   // tap spacing, as a fraction of the source width
static const float kAmbiBezel = 0.08f;      // dark rise next to the screen, as a fraction of the margin
static const float kAmbiBehind = 0.02f;     // metres the glow sits behind the screen

struct AmbiConstants                        // must match cbuffer C in kAmbiHlsl
{
    uint32_t gw = 0, gh = 0;                // glow texture size
    uint32_t srcW = 0, srcH = 0;            // source picture size
    float insetX = 0, insetY = 0;           // where the screen sits inside the glow rect (0 .. 0.5)
    float rectW = 0, rectH = 0;             // the glow rect in metres
    float marginM = 0;                      // metres of glow outside the screen
    float intensity = 0;
    float blend = 1;                        // 1 = no temporal smoothing
    uint32_t reset = 1;                     // 1: ignore what the glow texture already holds
    float blurPx = 0;                       // tap spacing in source pixels
    float bezel = 0;                        // dark rise next to the screen, fraction of the margin
};

// The glow for one pixel, in premultiplied linear-in-storage RGBA (0..1).
// Returns false and leaves `rgba` untouched on unusable constants.
inline bool AmbilightPixel(const AmbiConstants& c, const unsigned char* src, int srcPitch,
                          int gx, int gy, float rgba[4])
{
    if (!src || !rgba) return false;
    if (c.gw == 0 || c.gh == 0 || c.srcW == 0 || c.srcH == 0) return false;
    if (srcPitch < (int)c.srcW * 4) return false;
    if (!(c.marginM > 0) || !(c.insetX > 0) || !(c.insetY > 0)) return false;
    if (gx < 0 || gy < 0 || gx >= (int)c.gw || gy >= (int)c.gh) return false;

    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    const float u = ((float)gx + 0.5f) / (float)c.gw;
    const float v = ((float)gy + 0.5f) / (float)c.gh;
    const float sx = (u - c.insetX) / (1.0f - 2.0f * c.insetX);
    const float sy = (v - c.insetY) / (1.0f - 2.0f * c.insetY);
    const float cx = sx < 0 ? 0.0f : (sx > 1 ? 1.0f : sx);
    const float cy = sy < 0 ? 0.0f : (sy > 1 ? 1.0f : sy);
    const float dxm = (sx - cx) * (c.rectW * (1.0f - 2.0f * c.insetX));
    const float dym = (sy - cy) * (c.rectH * (1.0f - 2.0f * c.insetY));
    const float dist = std::sqrt(dxm * dxm + dym * dym) / c.marginM;
    if (dist <= 0.0f || dist >= 1.0f) return true;          // inside the screen, or past the glow

    float a = (1.0f - dist) * (1.0f - dist) * c.intensity;
    if (c.bezel > 0.0f)
    {
        const float tb = dist / c.bezel < 1.0f ? dist / c.bezel : 1.0f;
        a *= tb * tb * (3.0f - 2.0f * tb);
    }
    const float px = cx * (float)c.srcW - 0.5f;
    const float py = cy * (float)c.srcH - 0.5f;
    const int half = kAmbiTaps / 2;
    float sum[3] = { 0, 0, 0 };
    for (int j = -half; j <= half; j++)
        for (int i = -half; i <= half; i++)
        {
            int xi = (int)std::floor(px + (float)i * c.blurPx + 0.5f);
            int yi = (int)std::floor(py + (float)j * c.blurPx + 0.5f);
            xi = xi < 0 ? 0 : (xi > (int)c.srcW - 1 ? (int)c.srcW - 1 : xi);
            yi = yi < 0 ? 0 : (yi > (int)c.srcH - 1 ? (int)c.srcH - 1 : yi);
            const unsigned char* p = src + (size_t)yi * srcPitch + (size_t)xi * 4;
            for (int ch = 0; ch < 3; ch++) sum[ch] += p[ch] / 255.0f;
        }
    const float taps = (float)(kAmbiTaps * kAmbiTaps);
    for (int ch = 0; ch < 3; ch++) rgba[ch] = (sum[ch] / taps) * a;
    rgba[3] = a;
    return true;
}

// The whole glow texture, as the shader writes it with reset = 1 (no history).
inline bool AmbilightReference(const AmbiConstants& c, const unsigned char* src, int srcPitch,
                              unsigned char* out, int outPitch)
{
    if (!src || !out) return false;
    if (c.gw == 0 || c.gh == 0) return false;
    if (outPitch < (int)c.gw * 4) return false;

    for (int gy = 0; gy < (int)c.gh; gy++)
        for (int gx = 0; gx < (int)c.gw; gx++)
        {
            float rgba[4] = { 0, 0, 0, 0 };
            if (!AmbilightPixel(c, src, srcPitch, gx, gy, rgba)) return false;
            unsigned char* o = out + (size_t)gy * outPitch + (size_t)gx * 4;
            for (int ch = 0; ch < 4; ch++)
            {
                const float value = rgba[ch] < 0 ? 0.0f : (rgba[ch] > 1 ? 1.0f : rgba[ch]);
                o[ch] = (unsigned char)lroundf(value * 255.0f);
            }
        }
    return true;
}
