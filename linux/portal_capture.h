#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace vrx {
class PortalCapture {
public:
    struct Frame {
        std::vector<unsigned char> rgb;
        uint64_t sequence = 0;
        uint64_t layout = 0;
        double arrival = 0;
    };
    PortalCapture();
    ~PortalCapture();
    PortalCapture(const PortalCapture&) = delete;
    PortalCapture& operator=(const PortalCapture&) = delete;
    bool Open(); // Opens the desktop chooser; starts capture after selection.
    bool Latest(Frame& output) const;
    bool Healthy() const;
    uint64_t Captured() const;
    uint64_t Dropped() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrx
