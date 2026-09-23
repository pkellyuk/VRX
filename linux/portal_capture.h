#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace vrx {
// xdg-desktop-portal ScreenCast source selection and PipeWire capture. With
// DMA-BUF modifiers the source's buffers are handed to the renderer on the
// GPU; otherwise (or if the source declines them) frames arrive in shared
// memory and are scaled to the colour size on the CPU.
class PortalCapture {
public:
    struct Frame {
        std::vector<uint32_t> color;     // colorWidth x colorHeight packed RGBA
        uint32_t colorWidth = 0, colorHeight = 0;
        uint64_t sequence = 0;
        uint64_t layout = 0;
        double arrival = 0;
    };
    // A source buffer held for the renderer. Its descriptor stays valid while
    // the frame is in use; bufferId identifies the same buffer across frames
    // until bufferGeneration changes (the source renegotiated its buffers).
    struct GpuFrame {
        int fd = -1;
        uint64_t offset = 0;
        uint32_t stride = 0;
        uint64_t modifier = 0;
        uint32_t width = 0, height = 0;
        uint64_t bufferId = 0, bufferGeneration = 0;
        uint64_t sequence = 0, layout = 0;
        double arrival = 0;
    };
    PortalCapture();
    ~PortalCapture();
    PortalCapture(const PortalCapture&) = delete;
    PortalCapture& operator=(const PortalCapture&) = delete;
    // Opens the desktop chooser; starts capture after selection. `modifiers`:
    // DRM format modifiers for B8G8R8A8 the renderer can import; empty for
    // shared memory only.
    bool Open(const std::vector<uint64_t>& modifiers = {});
    // The colour texture's size, once the first format is known.
    bool ColorSize(uint32_t& width, uint32_t& height) const;
    bool DmaBuf() const;             // frames arrive as DMA-BUFs
    // Shared memory: copy of the newest scaled colour frame.
    bool Latest(Frame& output) const;
    // DMA-BUF: take the newest frame newer than the one in use. The frame in
    // use until now goes back to the source, so call this only when the GPU
    // has finished reading it.
    bool AcquireGpuFrame(GpuFrame& output);
    // Give back the frame in use (before shutting the renderer down).
    void ReleaseGpuFrame();
    bool Healthy() const;
    uint64_t Captured() const;
    uint64_t Dropped() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrx
