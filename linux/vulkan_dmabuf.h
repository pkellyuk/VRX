#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace vrx {
// Device extensions needed to import compositor DMA-BUFs as Vulkan images.
std::vector<const char*> DmabufDeviceExtensions();
// True when the GPU offers every extension in DmabufDeviceExtensions().
bool DmabufSupported(VkPhysicalDevice gpu);
// Single-plane DRM format modifiers for `format` that can be imported from a
// DMA-BUF as sampled, transfer-source images.
std::vector<uint64_t> DmabufModifiers(VkPhysicalDevice gpu, VkFormat format);

// One compositor buffer imported as a Vulkan image. The fd is duplicated, so
// the caller keeps ownership of its own descriptor. The image's content is
// owned by the foreign (compositor) queue family between uses.
class DmabufImage {
public:
    DmabufImage(VkPhysicalDevice gpu, VkDevice device, int fd, uint32_t width, uint32_t height,
                VkFormat format, uint64_t modifier, uint64_t offset, uint32_t stride);
    ~DmabufImage();
    DmabufImage(const DmabufImage&) = delete;
    DmabufImage& operator=(const DmabufImage&) = delete;
    VkImage Image() const { return image_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

private:
    VkDevice device_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    uint32_t width_, height_;
};
} // namespace vrx
