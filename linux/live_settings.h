#pragma once
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace vrx {
struct LiveSettings {
    float width = 2.0f;
    float distance = 2.0f;
    float height = 0.0f;
    float horizontal = 0.0f;
    float strength = 1.0f;
    int room = 30;
    int glass = 60;
    int reflect = 25;
    int light = 30;
    uint32_t lightRgb = 0xFFB46B;
    // VRXL 3: incremented by the desktop app for each Recenter request; the
    // engine re-places the screen whenever it changes.
    uint32_t recenter = 0;
    // VRXL 4: which game frame is shown with the depth (frame_timing.h):
    // 0 latest, 1 delayed to depth, 2 matched to depth.
    int timing = 0;
};

inline bool ReadLiveSettings(const std::string& path, LiveSettings& out) {
    std::ifstream file(path);
    if (!file) return false;
    std::string line, tag, extra;
    int version = 0;
    LiveSettings candidate;
    if (!std::getline(file, line)) return false;
    std::istringstream input(line);
    if (!(input >> tag >> version >> candidate.width >> candidate.distance >>
          candidate.height >> candidate.horizontal >> candidate.strength) ||
        tag != "VRXL" || version < 1 || version > 4) return false;
    if (version == 1) {
        candidate.room = 0;
    } else if (!(input >> candidate.room >> candidate.glass >> candidate.reflect >>
                 candidate.light >> candidate.lightRgb)) return false;
    if (version >= 3 && !(input >> candidate.recenter)) return false;
    if (version >= 4 && !(input >> candidate.timing)) return false;
    if ((input >> extra) ||
        !std::isfinite(candidate.width) || candidate.width < 0.5f || candidate.width > 10.0f ||
        !std::isfinite(candidate.distance) || candidate.distance < 0.5f || candidate.distance > 8.0f ||
        !std::isfinite(candidate.height) || candidate.height < -2.0f || candidate.height > 2.0f ||
        !std::isfinite(candidate.horizontal) || candidate.horizontal < -3.0f || candidate.horizontal > 3.0f ||
        !std::isfinite(candidate.strength) || candidate.strength < 0.0f || candidate.strength > 2.0f ||
        candidate.room < 0 || candidate.room > 100 || candidate.glass < 0 || candidate.glass > 100 ||
        candidate.reflect < 0 || candidate.reflect > 100 || candidate.light < 0 || candidate.light > 100 ||
        candidate.lightRgb > 0xFFFFFFu || candidate.timing < 0 || candidate.timing > 2) return false;
    out = candidate;
    return true;
}
} // namespace vrx
