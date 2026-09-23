#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace vrx {
// Recent captured colour frames on the GPU, for Delayed and Matched frame
// timing (frame_timing.h): each new frame is copied from the warp's colour
// buffer into a slot, and an older one is copied back when it is to be shown.
// Copies are recorded in the frame's command buffer; GPU frames run in order,
// so a slot is never overwritten while an earlier frame still reads it.
class VulkanFrameHistory {
public:
    static constexpr uint32_t kSlots = 8;
    VulkanFrameHistory(VkPhysicalDevice gpu, VkDevice device, VkBuffer color, VkDeviceSize bytes);
    ~VulkanFrameHistory();
    VulkanFrameHistory(const VulkanFrameHistory&) = delete;
    VulkanFrameHistory& operator=(const VulkanFrameHistory&) = delete;
    // Keep the colour buffer's current contents in `slot`.
    void RecordStore(VkCommandBuffer command, uint32_t slot) const;
    // Put `slot` back into the colour buffer for the warp and room to read.
    void RecordLoad(VkCommandBuffer command, uint32_t slot) const;

private:
    void Copy(VkCommandBuffer command, VkBuffer from, VkBuffer to) const;
    VkDevice device_;
    VkBuffer color_;
    VkDeviceSize bytes_;
    VkBuffer buffers_[kSlots]{};
    VkDeviceMemory memory_[kSlots]{};
};
} // namespace vrx
