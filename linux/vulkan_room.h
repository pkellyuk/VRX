#pragma once
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include "room.h"
#include "live_settings.h"
#include <vector>
#include <chrono>

namespace vrx {
class VulkanRoom {
public:
    // Half of the PICO 4/SteamVR recommended 2644 square eye target.
    static constexpr uint32_t EyeWidth = 1322, EyeHeight = 1322;
    static constexpr uint32_t GlowWidth = 64, GlowHeight = 45;
    VulkanRoom(VkPhysicalDevice gpu, VkDevice device, VkBuffer source, VkBuffer stereo,
               VkFormat screenFormat);
    ~VulkanRoom();
    VulkanRoom(const VulkanRoom&) = delete;
    VulkanRoom& operator=(const VulkanRoom&) = delete;
    void Prepare(const std::vector<unsigned char>& rgb, const LiveSettings& settings,
                 const XrView eyes[2], float floorLocalY);
    void Record(VkCommandBuffer command, VkImage destination);
    void EnableCapture() { capture_ = true; }
    void SaveCapture(const char* path) const;
    void PrintTiming() const;
    bool CompareReference() const;
private:
    struct Buffer { VkBuffer handle = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
                    void* mapped = nullptr; VkDeviceSize size = 0; };
    struct Image { VkImage handle = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
                   VkImageView view = VK_NULL_HANDLE; VkFormat format = VK_FORMAT_UNDEFINED;
                   uint32_t width = 0, height = 0, layers = 0; };
    VkPhysicalDevice gpu_;
    VkDevice device_;
    VkBuffer source_;
    VkBuffer stereo_;
    VkFormat screenFormat_;
    Buffer decode_, emitter_, glowBuffer_, mirrorBuffer_, lightBuffer_, curveBuffer_, roomBuffer_, readback_;
    bool capture_ = false;
    bool glowHistoryValid_ = false;
    float lastWidth_ = 0.0f, lastHeight_ = 0.0f;
    float geometryKey_[6] = {};
    std::chrono::steady_clock::time_point lastPrepare_{};
    Image picture_, glow_, mirror_, light_, eye_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout passLayout_ = VK_NULL_HANDLE, eyeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet passSet_ = VK_NULL_HANDLE, mirrorSet_ = VK_NULL_HANDLE, eyeSet_ = VK_NULL_HANDLE;
    VkPipelineLayout passPipelineLayout_ = VK_NULL_HANDLE, eyePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline emitPipeline_ = VK_NULL_HANDLE, mirrorPipeline_ = VK_NULL_HANDLE;
    VkPipeline lightPipeline_ = VK_NULL_HANDLE, eyePipeline_ = VK_NULL_HANDLE;
    VkQueryPool timingQueries_ = VK_NULL_HANDLE;
    float timestampPeriod_ = 0.0f;
    Room room_;
    RoomShading shading_;
    RoomEmitterLayout layout_;
    RoomConstants roomConstants_;
    CurveConstants curveConstants_;
    uint32_t mirrorW_ = 0, mirrorH_ = 0;
    void CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage);
    void CreateImage(Image& out, uint32_t width, uint32_t height, uint32_t layers,
                     VkFormat format, VkImageUsageFlags usage, VkImageViewType viewType);
    void DestroyBuffer(Buffer& buffer);
    void DestroyImage(Image& image);
    VkPipeline CreatePipeline(const char* path, VkPipelineLayout layout);
    void Transition(VkCommandBuffer cmd, const Image& image, VkImageLayout oldLayout,
                    VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
};
}
