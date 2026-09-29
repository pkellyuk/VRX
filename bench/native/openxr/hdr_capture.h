#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <vector>

// Windows scRGB uses 1.0 for 80 nits; SDR white on an HDR display is configurable.
// Capture in float until this one conversion, then share the same SDR image with
// the depth models and both eyes. No independent per-eye exposure adaptation.
inline float CaptureSdrWhiteScale(HMONITOR monitor)
{
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 1.0f;
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return 1.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) != ERROR_SUCCESS) return 1.0f;
    for (UINT32 i = 0; i < paths; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), p[i].sourceInfo.adapterId, p[i].sourceInfo.id };
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || _wcsicmp(source.viewGdiDeviceName, info.szDevice)) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white), p[i].targetInfo.adapterId, p[i].targetInfo.id };
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS)
            return std::clamp(white.SDRWhiteLevel / 1000.0f, 0.5f, 12.5f);
    }
    return 1.0f;
}

inline float HdrShoulder(float value)
{
    value = std::max(0.0f, value);
    return value <= 0.75f ? value : 0.75f + 0.25f * (1.0f - std::exp(-4.0f * (value - 0.75f)));
}

inline float HdrSrgbEncode(float linear)
{
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

inline void HdrToneMap(float& r, float& g, float& b, float whiteScale = 1.0f)
{
    const float white = std::isfinite(whiteScale) ? std::clamp(whiteScale, 0.5f, 12.5f) : 1.0f;
    r = std::isfinite(r) ? std::max(0.0f, r / white) : 0.0f;
    g = std::isfinite(g) ? std::max(0.0f, g / white) : 0.0f;
    b = std::isfinite(b) ? std::max(0.0f, b / white) : 0.0f;
    const float peak = std::max(r, std::max(g, b));
    const float scale = peak > 0.0f ? HdrShoulder(peak) / peak : 1.0f;
    r = HdrSrgbEncode(r * scale); g = HdrSrgbEncode(g * scale); b = HdrSrgbEncode(b * scale);
}
