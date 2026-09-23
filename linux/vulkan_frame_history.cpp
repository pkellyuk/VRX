#include "vulkan_frame_history.h"
#include <stdexcept>
#include <string>

namespace vrx {
namespace {
void Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}
}

VulkanFrameHistory::VulkanFrameHistory(VkPhysicalDevice gpu, VkDevice device, VkBuffer color, VkDeviceSize bytes)
    : device_(device), color_(color), bytes_(bytes) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateBuffer(device_, &info, nullptr, &buffers_[slot]), "vkCreateBuffer (frame history)");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffers_[slot], &requirements);
        uint32_t memoryType = UINT32_MAX;
        for (uint32_t i = 0; i < properties.memoryTypeCount && memoryType == UINT32_MAX; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) memoryType = i;
        if (memoryType == UINT32_MAX) throw std::runtime_error("No device-local memory for the frame history");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        Check(vkAllocateMemory(device_, &allocation, nullptr, &memory_[slot]), "vkAllocateMemory (frame history)");
        Check(vkBindBufferMemory(device_, buffers_[slot], memory_[slot], 0), "vkBindBufferMemory (frame history)");
    }
}

VulkanFrameHistory::~VulkanFrameHistory() {
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
        if (buffers_[slot]) vkDestroyBuffer(device_, buffers_[slot], nullptr);
        if (memory_[slot]) vkFreeMemory(device_, memory_[slot], nullptr);
    }
}

// The colour buffer is written and read by compute passes and transfers; each
// copy waits for those and makes its result visible to both.
void VulkanFrameHistory::Copy(VkCommandBuffer command, VkBuffer from, VkBuffer to) const {
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    const VkBufferCopy region{0, 0, bytes_};
    vkCmdCopyBuffer(command, from, to, 1, &region);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, stages, 0, 1, &after, 0, nullptr, 0, nullptr);
}

void VulkanFrameHistory::RecordStore(VkCommandBuffer command, uint32_t slot) const {
    Copy(command, color_, buffers_[slot % kSlots]);
}

void VulkanFrameHistory::RecordLoad(VkCommandBuffer command, uint32_t slot) const {
    Copy(command, buffers_[slot % kSlots], color_);
}
} // namespace vrx
