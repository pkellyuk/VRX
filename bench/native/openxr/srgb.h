#pragma once
#include <cmath>

// The sRGB transfer function (IEC 61966-2-1), for blending in linear light the way
// a compositor does with an _SRGB swapchain: it decodes each layer's texels to
// linear, blends, and encodes for the display. kAmbiHlsl and kCurveHlsl use the
// same formulas (Dec / Enc), so the CPU references match the shaders.

inline float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float LinearToSrgb(float l)
{
    return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}
