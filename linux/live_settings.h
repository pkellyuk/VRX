#pragma once
#include <cmath>
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
        tag != "VRXL" || version != 1 || (input >> extra) ||
        !std::isfinite(candidate.width) || candidate.width < 0.5f || candidate.width > 10.0f ||
        !std::isfinite(candidate.distance) || candidate.distance < 0.5f || candidate.distance > 8.0f ||
        !std::isfinite(candidate.height) || candidate.height < -2.0f || candidate.height > 2.0f ||
        !std::isfinite(candidate.horizontal) || candidate.horizontal < -3.0f || candidate.horizontal > 3.0f ||
        !std::isfinite(candidate.strength) || candidate.strength < 0.0f || candidate.strength > 2.0f)
        return false;
    out = candidate;
    return true;
}
} // namespace vrx
