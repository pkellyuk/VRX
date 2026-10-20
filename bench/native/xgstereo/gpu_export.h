// Vulkan -> shared D3D12 array textures. No image mapping or host fence waits.
#pragma once
#include "gpu_transport.h"
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <functional>

namespace xg {
#define XG_GPU_FUNCTIONS(X) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkGetMemoryWin32HandlePropertiesKHR) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkImportSemaphoreWin32HandleKHR) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBlitImage)
class GpuExporter {
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    uint32_t family_ = 0, width_ = 0, height_ = 0;
    PFN_vkSetDeviceLoaderData loader_ = nullptr;
    GpuWriter writer_;
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkSemaphore ready = VK_NULL_HANDLE, released = VK_NULL_HANDLE;
        VkCommandBuffer commands[2]{};
        bool initialized = false;
    };
    std::array<Slot, GpuSlots> slots_;
    int slot_ = -1;
    uint64_t sequence_ = 0, released_ = 0;
    char error_[256] = "GPU geometry stopped: shared resource initialization failed";
    bool Fail(const char* stage, int64_t code = 0)
    { std::snprintf(error_, sizeof(error_), "GPU geometry stopped: %s (%lld)", stage, (long long)code); return false; }
#define XG_MEMBER(fn) PFN_##fn fn##_ = nullptr;
    XG_GPU_FUNCTIONS(XG_MEMBER)
#undef XG_MEMBER
public:
    const char* Error() const { return error_; }
    ~GpuExporter()
    {
        // The owning Vulkan device must be idle before destruction.
        writer_.Close();
        for (auto& s : slots_)
        {
            if (s.ready) vkDestroySemaphore_(device_, s.ready, nullptr);
            if (s.released) vkDestroySemaphore_(device_, s.released, nullptr);
            if (s.image) vkDestroyImage_(device_, s.image, nullptr);
            if (s.memory) vkFreeMemory_(device_, s.memory, nullptr);
        }
        if (pool_) vkDestroyCommandPool_(device_, pool_, nullptr);
    }
    bool Init(VkDevice device, PFN_vkGetDeviceProcAddr next, PFN_vkSetDeviceLoaderData loader,
        uint32_t family, LUID adapter, uint32_t width, uint32_t height, const std::wstring& session, GpuFrameKind kind)
    {
        device_ = device; loader_ = loader; family_ = family; width_ = width; height_ = height;
#define XG_LOAD(fn) fn##_ = reinterpret_cast<PFN_##fn>(next(device, #fn)); if (!fn##_) return false;
        XG_GPU_FUNCTIONS(XG_LOAD)
#undef XG_LOAD
        if (!writer_.Create(session, adapter, width, height, kind)) return Fail("D3D12 shared resource creation");
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex = family;
        if (vkCreateCommandPool_(device, &pool, nullptr, &pool_) != VK_SUCCESS) return false;
        for (uint32_t i = 0; i < GpuSlots; ++i)
        {
            auto& s = slots_[i];
            VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.pNext = &external;
            image.imageType = VK_IMAGE_TYPE_2D; image.format = VK_FORMAT_R8G8B8A8_UNORM; image.extent = {width, height, 1};
            image.mipLevels = 1; image.arrayLayers = 2; image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            const auto created = vkCreateImage_(device, &image, nullptr, &s.image);
            if (created != VK_SUCCESS) return Fail("external Vulkan image creation", created);
            VkMemoryRequirements needs{}; vkGetImageMemoryRequirements_(device, s.image, &needs);
            VkMemoryWin32HandlePropertiesKHR properties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
            const auto propertiesResult = vkGetMemoryWin32HandlePropertiesKHR_(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, writer_.Handle(i, 0), &properties);
            if (propertiesResult != VK_SUCCESS) return Fail("Win32 memory handle properties", propertiesResult);
            const uint32_t mask = properties.memoryTypeBits & needs.memoryTypeBits;
            uint32_t memoryType = 0; while (memoryType < 32 && !(mask & (1u << memoryType))) ++memoryType;
            if (memoryType == 32) return Fail("no compatible external memory type");
            VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.image = s.image;
            VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
            import.pNext = &dedicated; import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT; import.handle = writer_.Handle(i, 0);
            VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memory.pNext = &import;
            // VkMemoryAllocateInfo explicitly ignores allocationSize for
            // D3D12_RESOURCE imports and obtains the actual size from the OS.
            // Vulkan and D3D12's optimal allocation estimates can differ (e.g.
            // at 2560x1440); comparing them would reject a valid shared image.
            memory.allocationSize = needs.size; memory.memoryTypeIndex = memoryType;
            const auto allocated = vkAllocateMemory_(device, &memory, nullptr, &s.memory);
            if (allocated != VK_SUCCESS) return Fail("external Vulkan memory import", allocated);
            const auto bound = vkBindImageMemory_(device, s.image, s.memory, 0);
            if (bound != VK_SUCCESS) return Fail("external Vulkan memory binding", bound);
            for (uint32_t n = 0; n < 2; ++n)
            {
                VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
                VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; semaphore.pNext = &type;
                auto* out = n ? &s.released : &s.ready;
                if (vkCreateSemaphore_(device, &semaphore, nullptr, out) != VK_SUCCESS) return false;
                VkImportSemaphoreWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
                imp.semaphore = *out; imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT; imp.handle = writer_.Handle(i, n + 1);
                if (vkImportSemaphoreWin32HandleKHR_(device, &imp) != VK_SUCCESS) return false;
            }
            VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            command.commandPool = pool_; command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount = 2;
            if (vkAllocateCommandBuffers_(device, &command, s.commands) != VK_SUCCESS) return false;
            if (loader_) for (auto c : s.commands) if (loader_(device, c) != VK_SUCCESS) return false;
        }
        return true;
    }
    bool Reserve() { slot_ = writer_.Reserve(sequence_, released_); return slot_ >= 0; }
    VkSemaphore Ready() const { return slots_[slot_].ready; }
    VkSemaphore Released() const { return slots_[slot_].released; }
    uint64_t Sequence() const { return sequence_; }
    uint64_t ReleaseValue() const { return released_; }
    bool HasPair() const { return slot_ >= 0; }
    void Drop() { slot_ = -1; }
    VkCommandBuffer Record(VkImage source, VkExtent2D extent, uint32_t eye,
        VkImageLayout layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
    {
        if (slot_ < 0 || eye > 1 || extent.width != width_ || extent.height != height_) return VK_NULL_HANDLE;
        auto& s = slots_[slot_]; const auto command = s.commands[eye];
        if (vkResetCommandBuffer_(command, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer_(command, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkImageMemoryBarrier barriers[2]{};
        for (auto& b : barriers) { b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; }
        auto& a = barriers[0]; a.image = source; a.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; a.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        a.oldLayout = layout; a.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; a.srcQueueFamilyIndex = a.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        auto& b = barriers[1]; b.image = s.image; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = s.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.subresourceRange.layerCount = 2; b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; b.dstQueueFamilyIndex = family_;
        vkCmdPipelineBarrier_(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
        VkImageBlit blit{}; blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
        blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(width_), int32_t(height_), 1};
        vkCmdBlitImage_(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        a.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; a.newLayout = layout; a.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; a.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0; b.srcQueueFamilyIndex = family_; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        vkCmdPipelineBarrier_(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
        if (vkEndCommandBuffer_(command) != VK_SUCCESS) return VK_NULL_HANDLE;
        s.initialized = true;
        return command;
    }
    GpuPublish TryPublish(uint64_t left, uint64_t right) { return slot_ < 0 ? GpuPublish::Invalid : writer_.TryPublish(uint32_t(slot_), sequence_, left, right); }
    bool Publish(uint64_t left, uint64_t right) { return slot_ >= 0 && writer_.Publish(uint32_t(slot_), sequence_, left, right); }
    void Close() { writer_.Close(); }
};
} // namespace xg
