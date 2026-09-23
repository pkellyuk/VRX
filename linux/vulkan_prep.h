#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace vrx {
class VulkanPrep {
public:
    // packedScene: sourceWidth x sourceHeight packed RGBA (red in the low byte).
    VulkanPrep(VkPhysicalDevice gpu, VkDevice device, VkBuffer packedScene,
               uint32_t sourceWidth, uint32_t sourceHeight);
    ~VulkanPrep();
    VulkanPrep(const VulkanPrep&) = delete;
    VulkanPrep& operator=(const VulkanPrep&) = delete;
    static constexpr uint32_t kFrameSlots = 2;   // frames the CPU may record ahead of the GPU
    // Each frame in flight writes its own output; Record, ModelInput and
    // CompareReference use the current slot unless one is given.
    void SetFrameSlot(uint32_t slot) { slot_ = slot % kFrameSlots; }
    void Record(VkCommandBuffer command);
    bool CompareReference(const std::vector<uint32_t>& rgba) const;
    const float* ModelInput() const { return ModelInput(slot_); }
    const float* ModelInput(uint32_t slot) const { return static_cast<const float*>(mapped_[slot % kFrameSlots]); }
    static constexpr int width = 672;
    static constexpr int height = 384;

private:
    VkDevice device_;
    uint32_t sourceWidth_, sourceHeight_;
    uint32_t slot_ = 0;
    VkBuffer output_[kFrameSlots]{};
    VkDeviceMemory memory_[kFrameSlots]{};
    void* mapped_[kFrameSlots]{};
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_[kFrameSlots]{};
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
} // namespace vrx
