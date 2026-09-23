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
    static constexpr uint32_t GlowWidth = 64, GlowHeight = 45;
    // source: the colour (colorWidth x colorHeight packed RGBA); stereo: both
    // eyes' warped colour at the same size.
    VulkanRoom(VkPhysicalDevice gpu, VkDevice device, VkBuffer source, VkBuffer stereo,
               VkFormat screenFormat, uint32_t colorWidth, uint32_t colorHeight,
               uint32_t eyeWidth, uint32_t eyeHeight);
    uint32_t EyeWidth() const { return eyeWidth_; }
    uint32_t EyeHeight() const { return eyeHeight_; }
    ~VulkanRoom();
    VulkanRoom(const VulkanRoom&) = delete;
    VulkanRoom& operator=(const VulkanRoom&) = delete;
    // glow: packed RGBA picture the screen's glow is taken from (any size).
    void Prepare(const uint32_t* glow, uint32_t glowWidth, uint32_t glowHeight, const LiveSettings& settings,
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
    uint32_t colorWidth_, colorHeight_;
    uint32_t eyeWidth_, eyeHeight_;   // the room layer's size per eye
    Buffer decode_, emitter_, glowBuffer_, mirrorBuffer_, lightBuffer_, curveBuffer_, roomBuffer_, readback_;
    bool capture_ = false;
    bool glowHistoryValid_ = false;
    std::vector<float> glowHistory_;
    // Written by Record's command buffer, not directly into GPU-visible memory.
    std::vector<RoomEmitter> pendingEmitters_;
    std::vector<unsigned char> glowBytes_;
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
