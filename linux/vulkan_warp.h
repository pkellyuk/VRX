#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace vrx {
// Forward stereo warp at colour resolution (colorWidth x colorHeight) with the
// depth model's kSyntheticWidth x kSyntheticHeight nearness sampled bilinearly.
// GPU working buffers are device-local; new colour and depth go through host
// staging buffers and are copied by RecordUpload.
class VulkanWarp {
public:
    VulkanWarp(VkPhysicalDevice gpu, VkDevice device, VkFormat format,
               uint32_t colorWidth, uint32_t colorHeight);
    ~VulkanWarp();
    VulkanWarp(const VulkanWarp&) = delete;
    VulkanWarp& operator=(const VulkanWarp&) = delete;
    static constexpr uint32_t kFrameSlots = 2;   // frames the CPU may record ahead of the GPU
    // Staging for Upload*/RecordUpload: each frame in flight has its own.
    void SetFrameSlot(uint32_t slot) { slot_ = slot % kFrameSlots; }
    void UploadColor(const std::vector<uint32_t>& rgba);   // colorWidth x colorHeight packed RGBA
    void UploadNearness(const std::vector<float>& nearness); // depth grid, 0 far .. 1 near
    void RecordUpload(VkCommandBuffer command);
    // readback: also copy both eyes' colour and depth for CompareReference.
    void Record(VkCommandBuffer command, bool readback = false);
    void SetStrength(float strength) { strength_ = strength; }
    // screenWidth sets the focal length in colour pixels (xrapp5: cw * distance / width).
    void SetStereoGeometry(float screenDistance, float screenWidth, float ipd, bool depthAvailable);
    uint32_t ColorWidth() const { return colorWidth_; }
    uint32_t ColorHeight() const { return colorHeight_; }
    VkBuffer SceneBuffer() const { return scene_.buffer; }
    VkBuffer ColorBuffer() const { return color_.buffer; }
    // Compares the last readback with the CPU reference for the same inputs.
    bool CompareReference(const std::vector<uint32_t>& rgba,
                          const std::vector<float>& nearness) const;

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    struct Parameters;
    Parameters MakeParameters(bool writeDepth) const;
    void CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void DestroyBuffer(Buffer& buffer);
    VkPhysicalDevice gpu_;
    VkDevice device_;
    VkFormat format_;
    uint32_t colorWidth_, colorHeight_;
    float strength_ = 1.0f;
    float screenDistance_ = 2.0f;
    float screenWidth_ = 2.0f;
    float ipd_ = 0.064f;
    bool depthAvailable_ = true;
    bool colorPending_ = false, nearnessPending_ = false;
    Buffer scene_, nearness_, color_, depth_, source_, landed_;
    Buffer sceneStaging_[kFrameSlots], nearnessStaging_[kFrameSlots], readback_;
    uint32_t slot_ = 0;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
} // namespace vrx
