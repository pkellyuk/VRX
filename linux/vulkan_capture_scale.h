#pragma once
#include "capture_scale.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <map>
#include <vector>

namespace vrx {
// Scales a captured image that is already on the GPU into the stereo warp's
// colour buffer (colorWidth x colorHeight packed RGBA) with capture_scale.comp.
// The result is identical to the CPU ScaleCaptureColor for the same frame.
class VulkanCaptureScale {
public:
    VulkanCaptureScale(VkPhysicalDevice gpu, VkDevice device, uint32_t queueFamily,
                       VkBuffer color, uint32_t colorWidth, uint32_t colorHeight);
    ~VulkanCaptureScale();
    VulkanCaptureScale(const VulkanCaptureScale&) = delete;
    VulkanCaptureScale& operator=(const VulkanCaptureScale&) = delete;
    // Record scaling `image` (in VK_IMAGE_LAYOUT_GENERAL) into the colour buffer,
    // then make the colour visible to compute and transfer reads. `foreign`:
    // the image belongs to another process (an imported DMA-BUF) and is
    // acquired from and released back to the foreign queue family. `check`:
    // also copy the source and result for CompareReference after completion.
    void Record(VkCommandBuffer command, VkImage image, VkImageView view, uint32_t sourceWidth,
                uint32_t sourceHeight, bool foreign, bool check = false);
    // Compares the last checked Record with the CPU scaler for the same pixels.
    bool CompareReference() const;
    // The colour from the last checked Record (empty before one completes).
    std::vector<uint32_t> CheckedColor() const;
    // Drop cached descriptor sets for imported views that are about to be destroyed.
    void ForgetViews();

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };
    void CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    void DestroyBuffer(Buffer& buffer);
    void UpdateTables(VkCommandBuffer command, uint32_t sourceWidth, uint32_t sourceHeight);
    VkDescriptorSet SetFor(VkImageView view);
    VkPhysicalDevice gpu_;
    VkDevice device_;
    uint32_t family_;
    VkBuffer color_;
    uint32_t colorWidth_, colorHeight_;
    ResampleTables tables_;
    Buffer xTaps_, yTaps_, xFraction_, yFraction_, sourceCheck_, colorCheck_;
    uint32_t checkedWidth_ = 0, checkedHeight_ = 0;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::map<VkImageView, VkDescriptorSet> sets_;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
} // namespace vrx
