#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace vrx {
class VulkanWarp {
public:
    VulkanWarp(VkPhysicalDevice gpu, VkDevice device, VkFormat format);
    ~VulkanWarp();
    VulkanWarp(const VulkanWarp&) = delete;
    VulkanWarp& operator=(const VulkanWarp&) = delete;
    void Upload(const std::vector<unsigned char>& rgb, const std::vector<float>& nearness);
    void Record(VkCommandBuffer command);
    void SetStrength(float strength) { strength_ = strength; }
    void SetStereoGeometry(float screenDistance, float ipd, bool depthAvailable);
    VkBuffer SceneBuffer() const { return scene_.buffer; }
    VkBuffer ColorBuffer() const { return color_.buffer; }
    bool CompareReference(const std::vector<unsigned char>& rgb,
                          const std::vector<float>& nearness) const;

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    void CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage);
    void DestroyBuffer(Buffer& buffer);
    VkPhysicalDevice gpu_;
    VkDevice device_;
    VkFormat format_;
    float strength_ = 1.0f;
    float screenDistance_ = 2.0f;
    float ipd_ = 0.064f;
    bool depthAvailable_ = true;
    Buffer scene_, nearness_, color_, depth_;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
} // namespace vrx
