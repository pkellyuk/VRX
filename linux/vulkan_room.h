#pragma once
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include "room.h"
#include "live_settings.h"
#include "screen_anchor.h"
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
    // screen: the placed screen (pose, size and the recentre point it was
    // placed from); floorLocalY: the STAGE floor in LOCAL space, or NaN;
    // cylinder: the curved screen (screen_curve.h), or not curved.
    // With the room on, the eye layer is the room, and a curved screen is drawn
    // in it; with the room off (or no room round this viewer) a curved screen
    // is drawn alone. Returns false when there is nothing to draw: a flat
    // screen with no room.
    bool Prepare(const uint32_t* glow, uint32_t glowWidth, uint32_t glowHeight, const LiveSettings& settings,
                 const XrView eyes[2], const ScreenAnchor& screen, float floorLocalY, const Cylinder& cylinder);
    // The last Prepare drew the screen itself (curved), so the quads are not shown.
    bool DrawsScreen() const { return cylinder_.curved; }
    // The last Prepare wants the glow as its own layer behind a flat screen
    // (no room): RecordGlow copies it into a GlowWidth x GlowHeight image.
    bool GlowLayer() const { return glowLayer_; }
    void RecordGlow(VkCommandBuffer command, VkImage destination);
    // The last Prepare wants the world colour as its own layer behind a flat
    // screen (no room; a black world needs none). WorldRgb is 0xRRGGBB.
    bool WorldLayer() const { return worldLayer_; }
    uint32_t WorldRgb() const { return worldRgb_; }
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
    bool roomRejected_ = false;       // BuildRoom failed for the current geometry key
    std::vector<float> glowHistory_;
    // Written by Record's command buffer, not directly into GPU-visible memory.
    std::vector<RoomEmitter> pendingEmitters_;
    std::vector<unsigned char> glowBytes_;
    float lastWidth_ = 0.0f, lastHeight_ = 0.0f;
    float geometryKey_[7] = {};
    Cylinder cylinder_;
    bool curveOnly_ = false;          // the curved screen alone: no room passes
    bool glowOn_ = true;              // the ambilight is on
    bool glowLayer_ = false;          // ... and shown as its own layer behind a flat screen
    uint32_t worldRgb_ = 0;           // the world colour round the screen
    bool worldLayer_ = false;         // ... shown as its own layer behind a flat screen
    RoomView view_;
    Buffer pictureReadback_;          // the warped pictures of a captured frame, for CompareReference
    std::chrono::steady_clock::time_point lastPrepare_{};
    Image picture_, glow_, mirror_, light_, eye_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout passLayout_ = VK_NULL_HANDLE, eyeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet passSet_ = VK_NULL_HANDLE, mirrorSet_ = VK_NULL_HANDLE, eyeSet_ = VK_NULL_HANDLE;
    VkPipelineLayout passPipelineLayout_ = VK_NULL_HANDLE, eyePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline emitPipeline_ = VK_NULL_HANDLE, mirrorPipeline_ = VK_NULL_HANDLE;
    VkPipeline lightPipeline_ = VK_NULL_HANDLE, eyePipeline_ = VK_NULL_HANDLE, curvePipeline_ = VK_NULL_HANDLE;
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
    void ComputeGlow(const uint32_t* source, uint32_t width, uint32_t height, float W, float H, int strength);
    void Transition(VkCommandBuffer cmd, const Image& image, VkImageLayout oldLayout,
                    VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
};
}
