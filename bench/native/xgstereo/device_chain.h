// Shallow-copy an entire device pNext chain before enabling the live-eye feature.
// Never write feature flags or pNext pointers into application-owned structures.
#pragma once
#include <vulkan/vk_layer.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <cstring>

namespace xg {
inline size_t DeviceNodeSize(VkStructureType type)
{
    if (type == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) return sizeof(VkLayerDeviceCreateInfo);
#include "chain_sizes.inc"
}
class DeviceChain {
    std::vector<std::unique_ptr<uint8_t[]>> nodes_;
public:
    void* head = nullptr;
    bool Clone(const void* source)
    {
        VkBaseOutStructure* previous = nullptr;
        for (auto* node = static_cast<const VkBaseInStructure*>(source); node; node = node->pNext)
        {
            if (nodes_.size() >= 128) return false;
            const size_t bytes = DeviceNodeSize(node->sType);
            if (!bytes) return false;
            auto copy = std::make_unique<uint8_t[]>(bytes);
            std::memcpy(copy.get(), node, bytes);
            auto* current = reinterpret_cast<VkBaseOutStructure*>(copy.get());
            current->pNext = nullptr;
            if (previous) previous->pNext = current; else head = current;
            previous = current;
            nodes_.push_back(std::move(copy));
        }
        return true;
    }
    bool EnableAddressFeature()
    {
        for (auto* node = static_cast<VkBaseOutStructure*>(head); node; node = node->pNext)
        {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            { reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(node)->bufferDeviceAddress = VK_TRUE; return true; }
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)
            { reinterpret_cast<VkPhysicalDeviceBufferDeviceAddressFeatures*>(node)->bufferDeviceAddress = VK_TRUE; return true; }
        }
        return false;
    }
    bool EnableTimelineFeature()
    {
        for (auto* node = static_cast<VkBaseOutStructure*>(head); node; node = node->pNext)
        {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            { reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(node)->timelineSemaphore = VK_TRUE; return true; }
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
            { reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(node)->timelineSemaphore = VK_TRUE; return true; }
        }
        return false;
    }
};
} // namespace xg
