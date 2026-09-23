#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace vrx {
class VulkanPrep {
public:
    VulkanPrep(VkPhysicalDevice gpu, VkDevice device, VkBuffer packedScene);
    ~VulkanPrep();
    VulkanPrep(const VulkanPrep&) = delete;
    VulkanPrep& operator=(const VulkanPrep&) = delete;
    void Record(VkCommandBuffer command);
    bool CompareReference(const std::vector<unsigned char>& rgb) const;
    const float* ModelInput() const { return static_cast<const float*>(mapped_); }
    static constexpr int width = 672;
    static constexpr int height = 384;

private:
    VkDevice device_;
    VkBuffer output_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
} // namespace vrx
