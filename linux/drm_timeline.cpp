#include "drm_timeline.h"
#include <drm/drm.h>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace vrx {
namespace {
int Ioctl(int fd, unsigned long request, void* argument) {
    int result;
    do result = ioctl(fd, request, argument);
    while (result == -1 && (errno == EINTR || errno == EAGAIN));
    return result;
}
}

DrmTimelines::DrmTimelines() {
    // Syncobjs are not tied to one GPU; the first render node that supports
    // them serves. A node without syncobj support rejects the probe below.
    for (int minor = 128; minor < 192 && drm_ < 0; ++minor) {
        const std::string path = "/dev/dri/renderD" + std::to_string(minor);
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drm_syncobj_create create{};
        if (Ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) == 0) {
            drm_syncobj_destroy destroy{};
            destroy.handle = create.handle;
            Ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
            drm_ = fd;
            std::printf("Explicit sync: DRM timelines on %s\n", path.c_str());
        } else {
            close(fd);
        }
    }
}

DrmTimelines::~DrmTimelines() {
    if (drm_ >= 0) close(drm_);
}

uint32_t DrmTimelines::Import(int fd) const {
    if (drm_ < 0 || fd < 0) return 0;
    drm_syncobj_handle handle{};
    handle.fd = fd;
    return Ioctl(drm_, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle) == 0 ? handle.handle : 0;
}

void DrmTimelines::Destroy(uint32_t handle) const {
    if (drm_ < 0 || !handle) return;
    drm_syncobj_destroy destroy{};
    destroy.handle = handle;
    Ioctl(drm_, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
}

bool DrmTimelines::Wait(uint32_t handle, uint64_t point, int64_t timeoutNs) const {
    if (drm_ < 0 || !handle) return false;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    drm_syncobj_timeline_wait wait{};
    wait.handles = reinterpret_cast<uintptr_t>(&handle);
    wait.points = reinterpret_cast<uintptr_t>(&point);
    wait.timeout_nsec = int64_t(now.tv_sec) * 1000000000 + now.tv_nsec + timeoutNs;   // absolute
    wait.count_handles = 1;
    // The producer may not have submitted the work for this point yet.
    wait.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
    return Ioctl(drm_, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait) == 0;
}

bool DrmTimelines::Signal(uint32_t handle, uint64_t point) const {
    if (drm_ < 0 || !handle) return false;
    drm_syncobj_timeline_array signal{};
    signal.handles = reinterpret_cast<uintptr_t>(&handle);
    signal.points = reinterpret_cast<uintptr_t>(&point);
    signal.count_handles = 1;
    return Ioctl(drm_, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &signal) == 0;
}
} // namespace vrx
