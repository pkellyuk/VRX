#include "vulkan_dmabuf.h"
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace vrx {
namespace {
void Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}
}

std::vector<const char*> DmabufDeviceExtensions() {
    return {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
}

bool DmabufSupported(VkPhysicalDevice gpu) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, available.data());
    for (const char* wanted : DmabufDeviceExtensions()) {
        bool found = false;
        for (const auto& extension : available)
            if (std::strcmp(extension.extensionName, wanted) == 0) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

std::vector<uint64_t> DmabufModifiers(VkPhysicalDevice gpu, VkFormat format) {
    VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    properties.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(gpu, format, &properties);
    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = modifiers.data();
    vkGetPhysicalDeviceFormatProperties2(gpu, format, &properties);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    std::vector<uint64_t> result;
    for (const auto& modifier : modifiers) {
        if (modifier.drmFormatModifierPlaneCount != 1 ||
            (modifier.drmFormatModifierTilingFeatures & needed) != needed) continue;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        modifierInfo.drmFormatModifier = modifier.drmFormatModifier;
        modifierInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        external.pNext = &modifierInfo;
        VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.pNext = &external;
        info.format = format;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 imageProperties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        imageProperties.pNext = &externalProperties;
        if (vkGetPhysicalDeviceImageFormatProperties2(gpu, &info, &imageProperties) != VK_SUCCESS) continue;
        if (!(externalProperties.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) continue;
        result.push_back(modifier.drmFormatModifier);
    }
    return result;
}

DmabufImage::DmabufImage(VkPhysicalDevice gpu, VkDevice device, int fd, uint32_t width, uint32_t height,
                         VkFormat format, uint64_t modifier, uint64_t offset, uint32_t stride)
    : device_(device), width_(width), height_(height) {
    VkSubresourceLayout plane{};
    plane.offset = offset;
    plane.rowPitch = stride;
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicitModifier{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    explicitModifier.drmFormatModifier = modifier;
    explicitModifier.drmFormatModifierPlaneCount = 1;
    explicitModifier.pPlaneLayouts = &plane;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    external.pNext = &explicitModifier;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &external;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(device_, &imageInfo, nullptr, &image_), "vkCreateImage (DMA-BUF)");
    try {
        auto getFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
        if (!getFdProperties) throw std::runtime_error("vkGetMemoryFdPropertiesKHR unavailable");
        VkMemoryFdPropertiesKHR fdProperties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        Check(getFdProperties(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProperties),
              "vkGetMemoryFdPropertiesKHR");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, image_, &requirements);
        const uint32_t types = requirements.memoryTypeBits & fdProperties.memoryTypeBits;
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(gpu, &memoryProperties);
        uint32_t memoryType = UINT32_MAX;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if (types & (1u << i)) { memoryType = i; break; }
        if (memoryType == UINT32_MAX) throw std::runtime_error("No memory type can import this DMA-BUF");
        const int owned = dup(fd);
        if (owned < 0) throw std::runtime_error("Cannot duplicate DMA-BUF descriptor");
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = image_;
        VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        import.fd = owned;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &import;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        const VkResult result = vkAllocateMemory(device_, &allocation, nullptr, &memory_);
        if (result != VK_SUCCESS) close(owned);  // Vulkan owns it only on success.
        Check(result, "vkAllocateMemory (DMA-BUF import)");
        Check(vkBindImageMemory(device_, image_, memory_, 0), "vkBindImageMemory (DMA-BUF)");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image_;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(device_, &viewInfo, nullptr, &view_), "vkCreateImageView (DMA-BUF)");
    } catch (...) {
        if (memory_) vkFreeMemory(device_, memory_, nullptr);
        vkDestroyImage(device_, image_, nullptr);
        throw;
    }
}

DmabufImage::~DmabufImage() {
    if (view_) vkDestroyImageView(device_, view_, nullptr);
    if (image_) vkDestroyImage(device_, image_, nullptr);
    if (memory_) vkFreeMemory(device_, memory_, nullptr);
}
} // namespace vrx
