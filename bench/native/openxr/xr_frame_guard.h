#pragma once
#include <openxr/openxr.h>

inline bool ValidStereoViews(XrResult result, uint32_t count, XrViewStateFlags flags)
{
    const auto required = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    return XR_SUCCEEDED(result) && count == 2 && (flags & required) == required;
}

// Acquisition alone does not permit access or release. In particular,
// XR_TIMEOUT_EXPIRED is a positive result but does not mean an image is ready.
struct XrReadyImage
{
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t index = 0;
    bool ready = false;
    XrResult result = XR_SUCCESS;

    bool Acquire(XrSwapchain sc, PFN_xrAcquireSwapchainImage acquire, PFN_xrWaitSwapchainImage wait)
    {
        swapchain = sc;
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        result = acquire(sc, &ai, &index);
        if (result != XR_SUCCESS) return false;
        XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = XR_INFINITE_DURATION;
        result = wait(sc, &wi);
        ready = result == XR_SUCCESS;
        return ready;
    }

    bool Release(PFN_xrReleaseSwapchainImage release)
    {
        if (!ready) return true;
        ready = false;
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        result = release(swapchain, &ri);
        return result == XR_SUCCESS;
    }
};
