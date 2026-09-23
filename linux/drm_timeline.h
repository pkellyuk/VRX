#pragma once
#include <cstdint>

namespace vrx {
// DRM timeline syncobjs for PipeWire explicit sync (SPA_META_SyncTimeline):
// the producer's acquire point says when a buffer's contents are ready, and
// the consumer signals the release point when it no longer reads the buffer.
// Uses the kernel ioctls directly on a DRM render node.
class DrmTimelines {
public:
    DrmTimelines();
    ~DrmTimelines();
    DrmTimelines(const DrmTimelines&) = delete;
    DrmTimelines& operator=(const DrmTimelines&) = delete;
    bool Available() const { return drm_ >= 0; }
    // A handle for a syncobj fd (the fd stays the caller's), or 0 on failure.
    uint32_t Import(int fd) const;
    void Destroy(uint32_t handle) const;
    // Wait until `point` is signalled, up to timeoutNs; false on timeout or error.
    bool Wait(uint32_t handle, uint64_t point, int64_t timeoutNs) const;
    bool Signal(uint32_t handle, uint64_t point) const;

private:
    int drm_ = -1;
};
} // namespace vrx
