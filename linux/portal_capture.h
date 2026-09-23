#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace vrx {
class PortalCapture {
public:
    struct Frame {
        std::vector<uint32_t> color;     // colorWidth x colorHeight packed RGBA
        std::vector<unsigned char> rgb;  // the colour on the depth model's grid
        uint32_t colorWidth = 0, colorHeight = 0;
        uint64_t sequence = 0;
        uint64_t layout = 0;
        double arrival = 0;
    };
    PortalCapture();
    ~PortalCapture();
    PortalCapture(const PortalCapture&) = delete;
    PortalCapture& operator=(const PortalCapture&) = delete;
    bool Open(); // Opens the desktop chooser; starts capture after selection.
    bool Latest(Frame& output, bool color = true, bool rgb = true) const;
    bool Healthy() const;
    uint64_t Captured() const;
    uint64_t Dropped() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrx
