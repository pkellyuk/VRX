// Execute the first Vulkan room pass and compare every texel with room.h.
#include "room.h"
#include "synthetic_scene.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}
struct Buffer { VkBuffer handle = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void* mapped = nullptr; };
Buffer MakeBuffer(VkPhysicalDevice gpu, VkDevice device, VkDeviceSize size) {
    Buffer b;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(device, &bi, nullptr, &b.handle), "vkCreateBuffer");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, b.handle, &req);
    VkPhysicalDeviceMemoryProperties props{}; vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { memoryType = i; break; }
    if (memoryType == UINT32_MAX) throw std::runtime_error("No coherent host-visible memory");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = memoryType;
    Check(vkAllocateMemory(device, &ai, nullptr, &b.memory), "vkAllocateMemory");
    Check(vkBindBufferMemory(device, b.handle, b.memory, 0), "vkBindBufferMemory");
    Check(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped), "vkMapMemory");
    return b;
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        std::puts("vrx-room-mirror-probe: compare Vulkan room mirror against the CPU reference"); return 0;
    }
    if (argc != 1) return 2;
    try {
        std::vector<unsigned char> rgb;
        vrx::MakeScene(rgb, 0.0);
        std::vector<unsigned char> rgba(size_t(vrx::kSyntheticWidth) * vrx::kSyntheticHeight * 4);
        std::vector<uint32_t> packed(rgba.size() / 4);
        for (size_t i = 0; i < packed.size(); ++i) {
            rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1];
            rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255;
            packed[i] = uint32_t(rgba[i * 4]) | (uint32_t(rgba[i * 4 + 1]) << 8) |
                        (uint32_t(rgba[i * 4 + 2]) << 16) | 0xff000000u;
        }
        float decode[256]; RoomDecodeTable(decode);
        RoomMirror reference;
        if (!RoomMirrorPicture(rgba.data(), vrx::kSyntheticWidth, vrx::kSyntheticHeight,
                               vrx::kSyntheticWidth * 4, decode, reference)) throw std::runtime_error("CPU mirror failed");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "VRX room mirror probe"; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
        VkInstance instance = VK_NULL_HANDLE; Check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0; Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        if (!count) throw std::runtime_error("No Vulkan GPU");
        std::vector<VkPhysicalDevice> gpus(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, gpus.data()), "vkEnumeratePhysicalDevices");
        VkPhysicalDevice gpu = VK_NULL_HANDLE; uint32_t family = UINT32_MAX;
        for (auto candidate : gpus) {
            uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
            std::vector<VkQueueFamilyProperties> queues(n);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, queues.data());
            for (uint32_t i = 0; i < n; ++i) if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                gpu = candidate; family = i; break;
            }
            if (gpu) break;
        }
        if (!gpu) throw std::runtime_error("No Vulkan compute queue");
        VkPhysicalDeviceProperties gpuProperties{};
        vkGetPhysicalDeviceProperties(gpu, &gpuProperties);
        std::printf("Room mirror Vulkan GPU: %s\n", gpuProperties.deviceName);
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        VkDevice device = VK_NULL_HANDLE; Check(vkCreateDevice(gpu, &dci, nullptr, &device), "vkCreateDevice");
        VkQueue queue = VK_NULL_HANDLE; vkGetDeviceQueue(device, family, 0, &queue);
        Buffer source = MakeBuffer(gpu, device, packed.size() * sizeof(uint32_t));
        Buffer table = MakeBuffer(gpu, device, sizeof(decode));
        Buffer output = MakeBuffer(gpu, device, reference.texels.size() * sizeof(float));
        std::memcpy(source.mapped, packed.data(), packed.size() * sizeof(uint32_t));
        std::memcpy(table.mapped, decode, sizeof(decode));
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding = i; bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 3; lci.pBindings = bindings;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        Check(vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout), "vkCreateDescriptorSetLayout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &poolSize;
        VkDescriptorPool pool = VK_NULL_HANDLE; Check(vkCreateDescriptorPool(device, &pci, nullptr, &pool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sai.descriptorPool = pool; sai.descriptorSetCount = 1; sai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE; Check(vkAllocateDescriptorSets(device, &sai, &set), "vkAllocateDescriptorSets");
        VkDescriptorBufferInfo infos[3] = {{source.handle, 0, VK_WHOLE_SIZE}, {table.handle, 0, VK_WHOLE_SIZE},
                                            {output.handle, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set;
            writes[i].dstBinding = i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        std::ifstream shader(VRX_ROOM_MIRROR_SPV_PATH, std::ios::binary | std::ios::ate);
        if (!shader) throw std::runtime_error("Cannot open mirror SPIR-V");
        std::streamsize bytes = shader.tellg();
        if (bytes <= 0 || bytes % 4) throw std::runtime_error("Invalid mirror SPIR-V");
        std::vector<uint32_t> code(size_t(bytes) / 4);
        shader.seekg(0); shader.read(reinterpret_cast<char*>(code.data()), bytes);
        VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sci.codeSize = size_t(bytes); sci.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE; Check(vkCreateShaderModule(device, &sci, nullptr, &module), "vkCreateShaderModule");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 5 * sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1; plci.pSetLayouts = &layout; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &range;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        Check(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = module; stage.pName = "main";
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = stage; cpci.layout = pipelineLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline), "vkCreateComputePipelines");
        VkCommandPoolCreateInfo cmdPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cmdPoolInfo.queueFamilyIndex = family;
        VkCommandPool cmdPool = VK_NULL_HANDLE; Check(vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &cmdPool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo bai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        bai.commandPool = cmdPool; bai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; bai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE; Check(vkAllocateCommandBuffers(device, &bai, &cmd), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        const uint32_t params[5] = {vrx::kSyntheticWidth, vrx::kSyntheticHeight,
                                    uint32_t(reference.w), uint32_t(reference.h), uint32_t(RoomStride(vrx::kSyntheticWidth))};
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), params);
        vkCmdDispatch(cmd, (reference.w + 7) / 8, (reference.h + 7) / 8, 1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        Check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        const float* result = static_cast<const float*>(output.mapped);
        float worst = 0; size_t bad = 0;
        for (size_t i = 0; i < reference.texels.size(); ++i) {
            float diff = std::fabs(result[i] - reference.texels[i]);
            worst = std::max(worst, diff);
            if (!std::isfinite(diff) || diff > 1e-5f) ++bad;
        }
        std::printf("Vulkan/CPU room mirror: %zu of %zu channels outside 1e-5 (worst %.8f)\n",
                    bad, reference.texels.size(), worst);
        vkDestroyCommandPool(device, cmdPool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
        for (Buffer* b : {&source, &table, &output}) {
            vkUnmapMemory(device, b->memory); vkDestroyBuffer(device, b->handle, nullptr);
            vkFreeMemory(device, b->memory, nullptr);
        }
        vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
        return bad ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Room mirror probe: %s\n", error.what()); return 1;
    }
}
