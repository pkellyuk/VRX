// Offline GPU checks for the live capture path: capture_scale.comp against the
// CPU ScaleCaptureColor (exact) and model_prep.comp at colour resolution
// against the CPU model input reference. Uses an ordinary Vulkan image in place
// of an imported DMA-BUF; the shader path is the same.
#include "capture_scale.h"
#include "vulkan_capture_scale.h"
#include "vulkan_prep.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}

struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    ~Gpu() {
        if (device) vkDeviceWaitIdle(device);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    uint32_t Memory(uint32_t bits, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("No suitable memory type");
    }
    void Begin() const {
        Check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    }
    void Submit() const {
        Check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        Check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        Check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        Check(vkResetFences(device, 1, &fence), "vkResetFences");
    }
};

void CreateGpu(Gpu& gpu) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    Check(vkCreateInstance(&instanceInfo, nullptr, &gpu.instance), "vkCreateInstance");
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(gpu.instance, &count, devices.data());
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gpu.physical = device; break; }
    }
    if (!gpu.physical && !devices.empty()) gpu.physical = devices.front();
    if (!gpu.physical) throw std::runtime_error("No Vulkan device");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu.physical, &properties);
    std::printf("Vulkan GPU: %s\n", properties.deviceName);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &familyCount, families.data());
    gpu.family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { gpu.family = i; break; }
    if (gpu.family == UINT32_MAX) throw std::runtime_error("No compute queue");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = gpu.family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    Check(vkCreateDevice(gpu.physical, &deviceInfo, nullptr, &gpu.device), "vkCreateDevice");
    vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = gpu.family;
    Check(vkCreateCommandPool(gpu.device, &poolInfo, nullptr, &gpu.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = gpu.pool;
    commandInfo.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(gpu.device, &commandInfo, &gpu.command), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(vkCreateFence(gpu.device, &fenceInfo, nullptr, &gpu.fence), "vkCreateFence");
}

struct Buffer {
    const Gpu& gpu;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    Buffer(const Gpu& owner, VkDeviceSize size, VkBufferUsageFlags usage, bool host) : gpu(owner) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        Check(vkCreateBuffer(gpu.device, &info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(gpu.device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = gpu.Memory(requirements.memoryTypeBits, host ?
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT :
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(gpu.device, &allocation, nullptr, &memory), "vkAllocateMemory");
        Check(vkBindBufferMemory(gpu.device, buffer, memory, 0), "vkBindBufferMemory");
        if (host) Check(vkMapMemory(gpu.device, memory, 0, size, 0, &mapped), "vkMapMemory");
    }
    ~Buffer() {
        if (mapped) vkUnmapMemory(gpu.device, memory);
        vkDestroyBuffer(gpu.device, buffer, nullptr);
        vkFreeMemory(gpu.device, memory, nullptr);
    }
};

// A B8G8R8A8 source image in VK_IMAGE_LAYOUT_GENERAL holding `bgra`.
struct SourceImage {
    const Gpu& gpu;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    SourceImage(const Gpu& owner, uint32_t width, uint32_t height, const std::vector<unsigned char>& bgra)
        : gpu(owner) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_B8G8R8A8_UNORM;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Check(vkCreateImage(gpu.device, &info, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(gpu.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = gpu.Memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(gpu.device, &allocation, nullptr, &memory), "vkAllocateMemory");
        Check(vkBindImageMemory(gpu.device, image, memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = info.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(gpu.device, &viewInfo, nullptr, &view), "vkCreateImageView");
        Buffer staging(gpu, bgra.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, bgra.data(), bgra.size());
        gpu.Begin();
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(gpu.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(gpu.command, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(gpu.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        gpu.Submit();
    }
    ~SourceImage() {
        vkDestroyImageView(gpu.device, view, nullptr);
        vkDestroyImage(gpu.device, image, nullptr);
        vkFreeMemory(gpu.device, memory, nullptr);
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        std::puts("vrx-capture-gpu-test: compare the GPU capture scale and colour-size model prep with their CPU references");
        return 0;
    }
    try {
        Gpu gpu;
        CreateGpu(gpu);
        struct Case { uint32_t sourceWidth, sourceHeight, colorWidth, colorHeight; };
        // Shrink 2x, shrink with 3x3 and 4x4 taps, equal size, enlarge (bilinear),
        // pillarboxed portrait and letterboxed ultrawide sources.
        const Case cases[] = {{3840, 2160, 1920, 1080}, {5120, 2880, 1920, 1080}, {7680, 4320, 1920, 1080},
                              {1920, 1080, 1920, 1080}, {1280, 720, 1920, 1080}, {1000, 3000, 1920, 1080},
                              {5120, 1440, 1920, 540}, {1366, 768, 1366, 768}};
        std::mt19937 random(7);
        bool passed = true;
        for (const Case& c : cases) {
            std::vector<unsigned char> bgra(size_t(c.sourceWidth) * c.sourceHeight * 4);
            // Smooth gradients plus noise, so both filtering and exact texel choice matter.
            for (uint32_t y = 0; y < c.sourceHeight; ++y)
                for (uint32_t x = 0; x < c.sourceWidth; ++x) {
                    unsigned char* p = bgra.data() + (size_t(y) * c.sourceWidth + x) * 4;
                    p[0] = uint8_t((x * 255) / c.sourceWidth ^ (random() & 15));
                    p[1] = uint8_t((y * 255) / c.sourceHeight ^ (random() & 15));
                    p[2] = uint8_t(random());
                    p[3] = 255;
                }
            SourceImage source(gpu, c.sourceWidth, c.sourceHeight, bgra);
            Buffer color(gpu, VkDeviceSize(c.colorWidth) * c.colorHeight * 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
            vrx::VulkanCaptureScale scale(gpu.physical, gpu.device, gpu.family, color.buffer,
                                          c.colorWidth, c.colorHeight);
            vrx::VulkanPrep prep(gpu.physical, gpu.device, color.buffer, c.colorWidth, c.colorHeight);
            gpu.Begin();
            scale.Record(gpu.command, source.image, source.view, c.sourceWidth, c.sourceHeight, false, true);
            prep.Record(gpu.command);
            gpu.Submit();
            std::printf("%ux%u -> %ux%u: ", c.sourceWidth, c.sourceHeight, c.colorWidth, c.colorHeight);
            const bool scaled = scale.CompareReference();
            if (!scaled) {
                // Which side is wrong: the GPU result, or the check's copy of the source?
                std::vector<uint32_t> expected;
                vrx::ScaleCaptureColor(bgra.data(), size_t(c.sourceWidth) * 4, int(c.sourceWidth),
                    int(c.sourceHeight), false, int(c.colorWidth), int(c.colorHeight), expected);
                const auto actual = scale.CheckedColor();
                size_t bad = 0, firstRow = SIZE_MAX, lastRow = 0;
                for (size_t i = 0; i < expected.size(); ++i)
                    if (expected[i] != actual[i]) {
                        ++bad;
                        firstRow = std::min(firstRow, i / c.colorWidth);
                        lastRow = std::max(lastRow, i / c.colorWidth);
                    }
                std::printf("  against the uploaded pixels: %zu differ, rows %zu..%zu\n", bad, firstRow, lastRow);
                // Bad columns, and for a 1:1 copy, which source column each bad pixel holds.
                std::vector<int> badColumns;
                for (uint32_t x = 0; x < c.colorWidth; ++x)
                    for (uint32_t y = 0; y < c.colorHeight; ++y)
                        if (expected[size_t(y) * c.colorWidth + x] != actual[size_t(y) * c.colorWidth + x]) {
                            badColumns.push_back(int(x)); break;
                        }
                std::printf("  %zu bad columns:", badColumns.size());
                for (size_t i = 0; i < badColumns.size() && i < 12; ++i) std::printf(" %d", badColumns[i]);
                std::printf("%s\n", badColumns.size() > 12 ? " ..." : "");
                if (!badColumns.empty()) {
                    const uint32_t x = uint32_t(badColumns.front()), y = c.colorHeight / 2;
                    const uint32_t got = actual[size_t(y) * c.colorWidth + x];
                    std::printf("  pixel (%u,%u): expected %08x got %08x;", x, y,
                                expected[size_t(y) * c.colorWidth + x], got);
                    int foundX = -1, foundY = -1;
                    for (uint32_t yy = 0; yy < c.colorHeight && foundX < 0; ++yy)
                        for (uint32_t xx = 0; xx < c.colorWidth; ++xx)
                            if (expected[size_t(yy) * c.colorWidth + xx] == got) { foundX = int(xx); foundY = int(yy); break; }
                    std::printf(" got matches expected (%d,%d)\n", foundX, foundY);
                }
            }
            const bool prepared = prep.CompareReference(scale.CheckedColor());
            passed = passed && scaled && prepared;
        }
        std::puts(passed ? "Capture GPU checks passed" : "Capture GPU checks FAILED");
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "vrx-capture-gpu-test: %s\n", error.what());
        return 1;
    }
}
