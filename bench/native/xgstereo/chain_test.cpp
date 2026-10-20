#include <cstring>
#include "device_chain.h"
#include <cstdio>
#include <cstdlib>

static void Check(bool condition, const char* what)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
int main()
{
    VkPhysicalDeviceVulkan12Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features.timelineSemaphore = VK_TRUE;
    VkDevicePrivateDataCreateInfo tail{VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO}; tail.privateDataSlotRequestCount = 7;
    features.pNext = &tail;
    VkLayerDeviceCreateInfo loader{}; loader.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
    loader.function = VK_LOADER_DATA_CALLBACK; loader.pNext = &features;
    xg::DeviceChain copy;
    Check(copy.Clone(&loader) && copy.EnableAddressFeature(), "copy loader and Vulkan feature nodes");
    const auto* copiedLoader = static_cast<const VkLayerDeviceCreateInfo*>(copy.head);
    const auto* copiedFeatures = static_cast<const VkPhysicalDeviceVulkan12Features*>(copiedLoader->pNext);
    const auto* copiedTail = static_cast<const VkDevicePrivateDataCreateInfo*>(copiedFeatures->pNext);
    Check(copiedFeatures != &features && copiedTail != &tail && copiedFeatures->bufferDeviceAddress &&
        copiedFeatures->timelineSemaphore && copiedTail->privateDataSlotRequestCount == 7 && !copiedTail->pNext,
        "preserve existing flags and full chain order");
    Check(!features.bufferDeviceAddress && features.pNext == &tail && loader.pNext == &features,
        "never mutate application-owned feature flags or links");
    VkBaseInStructure unknown{VK_STRUCTURE_TYPE_MAX_ENUM, nullptr};
    xg::DeviceChain rejected;
    Check(!rejected.Clone(&unknown), "unknown device nodes decline activation");
    VkPhysicalDeviceBufferDeviceAddressFeatures address{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    xg::DeviceChain addressCopy;
    Check(addressCopy.Clone(&address) && addressCopy.EnableAddressFeature() && !address.bufferDeviceAddress,
        "enable existing address feature on the copy only");
    std::puts("PASS: device-chain cloning, feature preservation, loader nodes and unknown-node rejection");
    return 0;
}
