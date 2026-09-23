// Compare Vulkan MIRROR and EMIT passes with the portable room.h reference.
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
        std::puts("vrx-room-gpu-probe: compare Vulkan room MIRROR, EMIT and LIGHT passes against the CPU reference"); return 0;
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
        constexpr int glowW = 64, glowH = 64;
        std::vector<unsigned char> glowRgba(size_t(glowW) * glowH * 4);
        std::vector<uint32_t> packedGlow(size_t(glowW) * glowH);
        for (int y = 0; y < glowH; ++y) for (int x = 0; x < glowW; ++x) {
            size_t i = size_t(y) * glowW + x;
            glowRgba[i * 4] = static_cast<unsigned char>(40 + 2 * x);
            glowRgba[i * 4 + 1] = static_cast<unsigned char>(30 + y);
            glowRgba[i * 4 + 2] = 120;
            glowRgba[i * 4 + 3] = 255;
            packedGlow[i] = uint32_t(glowRgba[i * 4]) | (uint32_t(glowRgba[i * 4 + 1]) << 8) |
                            (uint32_t(glowRgba[i * 4 + 2]) << 16) | 0xff000000u;
        }
        RoomInputs roomInputs;
        roomInputs.W = 2.0f;
        roomInputs.H = roomInputs.W * vrx::kSyntheticHeight / vrx::kSyntheticWidth;
        roomInputs.eye[2] = 2.0f;
        Room room;
        if (!BuildRoom(roomInputs, room)) throw std::runtime_error("CPU room geometry failed");
        RoomEmitterLayout emitterLayout = RoomLayout(roomInputs.W, roomInputs.H, glowW, glowH);
        std::vector<RoomEmitter> geometry;
        if (!BuildRoomEmitters(room, Cylinder(), roomInputs.W, roomInputs.H,
                               roomInputs.W * 0.7f, roomInputs.H * 0.7f,
                               emitterLayout, geometry)) throw std::runtime_error("CPU room emitters failed");
        size_t activeGlow = 0, inactiveGlow = 0;
        for (int e = emitterLayout.gridX * emitterLayout.gridY; e < emitterLayout.lampIndex(); ++e)
            (geometry[size_t(e)].n[3] != 0.0f ? activeGlow : inactiveGlow)++;
        if (!activeGlow || !inactiveGlow) throw std::runtime_error("Glow fixture lacks active and inactive blocks");
        RoomLook look; look.light = 30; look.glass = 60; look.reflect = 25;
        RoomShading shading = MakeRoomShading(room, 30, 0x304060, roomInputs.W * roomInputs.H, look);
        if (!shading.finish || !shading.lightOn) throw std::runtime_error("Finish fixture is inactive");
        float decode[256]; RoomDecodeTable(decode);
        RoomMirror reference;
        if (!RoomMirrorPicture(rgba.data(), vrx::kSyntheticWidth, vrx::kSyntheticHeight,
                               vrx::kSyntheticWidth * 4, decode, reference)) throw std::runtime_error("CPU mirror failed");
        std::vector<RoomEmitter> expectedEmitters = geometry;
        constexpr float alpha = 0.6f;
        if (!RoomEmitRadiance(emitterLayout, rgba.data(), vrx::kSyntheticWidth,
                              vrx::kSyntheticHeight, vrx::kSyntheticWidth * 4,
                              glowRgba.data(), glowW * 4, true, alpha, decode,
                              expectedEmitters, shading.lightL))
            throw std::runtime_error("CPU emitter reduction failed");
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
        std::printf("Room Vulkan GPU: %s\n", gpuProperties.deviceName);
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        VkDevice device = VK_NULL_HANDLE; Check(vkCreateDevice(gpu, &dci, nullptr, &device), "vkCreateDevice");
        VkQueue queue = VK_NULL_HANDLE; vkGetDeviceQueue(device, family, 0, &queue);
        Buffer source = MakeBuffer(gpu, device, packed.size() * sizeof(uint32_t));
        Buffer table = MakeBuffer(gpu, device, sizeof(decode));
        const size_t outputBytes = std::max(reference.texels.size() * sizeof(float),
                                            geometry.size() * sizeof(RoomEmitter));
        Buffer output = MakeBuffer(gpu, device, outputBytes);
        Buffer glowBuffer = MakeBuffer(gpu, device, packedGlow.size() * sizeof(uint32_t));
        constexpr size_t lightmapValues = size_t(kRoomFaces) * kRoomLightmap * kRoomLightmap * 4;
        Buffer lightmap = MakeBuffer(gpu, device, lightmapValues * sizeof(float));
        std::memcpy(source.mapped, packed.data(), packed.size() * sizeof(uint32_t));
        std::memcpy(table.mapped, decode, sizeof(decode));
        std::memcpy(glowBuffer.mapped, packedGlow.data(), packedGlow.size() * sizeof(uint32_t));
        VkDescriptorSetLayoutBinding bindings[5]{};
        for (uint32_t i = 0; i < 5; ++i) {
            bindings[i].binding = i; bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 5; lci.pBindings = bindings;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        Check(vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout), "vkCreateDescriptorSetLayout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &poolSize;
        VkDescriptorPool pool = VK_NULL_HANDLE; Check(vkCreateDescriptorPool(device, &pci, nullptr, &pool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sai.descriptorPool = pool; sai.descriptorSetCount = 1; sai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE; Check(vkAllocateDescriptorSets(device, &sai, &set), "vkAllocateDescriptorSets");
        VkDescriptorBufferInfo infos[5] = {{source.handle, 0, VK_WHOLE_SIZE}, {table.handle, 0, VK_WHOLE_SIZE},
                                            {output.handle, 0, VK_WHOLE_SIZE}, {glowBuffer.handle, 0, VK_WHOLE_SIZE},
                                            {lightmap.handle, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[5]{};
        for (uint32_t i = 0; i < 5; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set;
            writes[i].dstBinding = i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
        std::ifstream shader(VRX_ROOM_MIRROR_SPV_PATH, std::ios::binary | std::ios::ate);
        if (!shader) throw std::runtime_error("Cannot open mirror SPIR-V");
        std::streamsize bytes = shader.tellg();
        if (bytes <= 0 || bytes % 4) throw std::runtime_error("Invalid mirror SPIR-V");
        std::vector<uint32_t> code(size_t(bytes) / 4);
        shader.seekg(0); shader.read(reinterpret_cast<char*>(code.data()), bytes);
        VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sci.codeSize = size_t(bytes); sci.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE; Check(vkCreateShaderModule(device, &sci, nullptr, &module), "vkCreateShaderModule");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
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
        std::memcpy(output.mapped, geometry.data(), geometry.size() * sizeof(RoomEmitter));
        std::ifstream emitShader(VRX_ROOM_EMIT_SPV_PATH, std::ios::binary | std::ios::ate);
        if (!emitShader) throw std::runtime_error("Cannot open EMIT SPIR-V");
        std::streamsize emitBytes = emitShader.tellg();
        if (emitBytes <= 0 || emitBytes % 4) throw std::runtime_error("Invalid EMIT SPIR-V");
        std::vector<uint32_t> emitCode(size_t(emitBytes) / 4);
        emitShader.seekg(0); emitShader.read(reinterpret_cast<char*>(emitCode.data()), emitBytes);
        sci.codeSize = size_t(emitBytes); sci.pCode = emitCode.data();
        VkShaderModule emitModule = VK_NULL_HANDLE;
        Check(vkCreateShaderModule(device, &sci, nullptr, &emitModule), "vkCreateShaderModule EMIT");
        stage.module = emitModule;
        cpci.stage = stage;
        VkPipeline emitPipeline = VK_NULL_HANDLE;
        Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &emitPipeline),
              "vkCreateComputePipelines EMIT");
        struct EmitParams {
            uint32_t srcW, srcH, stride, gridX, gridY, glowW, glowH, blocksX,
                     blocksY, glowBlock, emitterCount, glowOn;
            float alpha;
            uint32_t pad[3];
            float lightL[4];
        } emitParams{};
        static_assert(sizeof(EmitParams) == 80);
        emitParams.srcW = vrx::kSyntheticWidth; emitParams.srcH = vrx::kSyntheticHeight;
        emitParams.stride = RoomStride(vrx::kSyntheticWidth);
        emitParams.gridX = emitterLayout.gridX; emitParams.gridY = emitterLayout.gridY;
        emitParams.glowW = glowW; emitParams.glowH = glowH;
        emitParams.blocksX = emitterLayout.blocksX; emitParams.blocksY = emitterLayout.blocksY;
        emitParams.glowBlock = emitterLayout.block; emitParams.emitterCount = emitterLayout.count();
        emitParams.glowOn = 1; emitParams.alpha = alpha;
        std::copy_n(shading.lightL, 3, emitParams.lightL);
        Check(vkResetCommandPool(device, cmdPool, 0), "vkResetCommandPool");
        Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer EMIT");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, emitPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(emitParams), &emitParams);
        vkCmdDispatch(cmd, emitParams.emitterCount, 1, 1);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer EMIT");
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit EMIT");
        Check(vkQueueWaitIdle(queue), "vkQueueWaitIdle EMIT");
        const auto* actualEmitters = static_cast<const RoomEmitter*>(output.mapped);
        size_t emitBad = 0; float emitWorst = 0;
        for (size_t e = 0; e < expectedEmitters.size(); ++e)
            for (int ch = 0; ch < 3; ++ch) {
                float diff = std::fabs(actualEmitters[e].L[ch] - expectedEmitters[e].L[ch]);
                emitWorst = std::max(emitWorst, diff);
                if (!std::isfinite(diff) || diff > 1e-5f) ++emitBad;
            }
        if (expectedEmitters[size_t(emitterLayout.lampIndex())].L[0] <= 0.0f)
            throw std::runtime_error("Ceiling lamp fixture was not lit");
        std::printf("Vulkan/CPU room EMIT: %zu of %zu channels outside 1e-5 (worst %.8f); %zu active/%zu inactive glow blocks, lamp lit\n",
                    emitBad, expectedEmitters.size() * 3, emitWorst, activeGlow, inactiveGlow);
        std::ifstream lightShader(VRX_ROOM_LIGHT_SPV_PATH, std::ios::binary | std::ios::ate);
        if (!lightShader) throw std::runtime_error("Cannot open LIGHT SPIR-V");
        std::streamsize lightBytes = lightShader.tellg();
        if (lightBytes <= 0 || lightBytes % 4) throw std::runtime_error("Invalid LIGHT SPIR-V");
        std::vector<uint32_t> lightCode(size_t(lightBytes) / 4);
        lightShader.seekg(0); lightShader.read(reinterpret_cast<char*>(lightCode.data()), lightBytes);
        sci.codeSize = size_t(lightBytes); sci.pCode = lightCode.data();
        VkShaderModule lightModule = VK_NULL_HANDLE;
        Check(vkCreateShaderModule(device, &sci, nullptr, &lightModule), "vkCreateShaderModule LIGHT");
        stage.module = lightModule;
        cpci.stage = stage;
        VkPipeline lightPipeline = VK_NULL_HANDLE;
        Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &lightPipeline),
              "vkCreateComputePipelines LIGHT");
        struct LightParams {
            float bounds[4], geometry[4], shading[4], worldCount[4];
        } lightParams{};
        static_assert(sizeof(LightParams) == 64);
        lightParams.bounds[0] = room.X; lightParams.bounds[1] = room.yF;
        lightParams.bounds[2] = room.yC; lightParams.bounds[3] = room.zB;
        lightParams.geometry[0] = room.g; lightParams.geometry[1] = room.zSide;
        lightParams.geometry[2] = kRoomShadeFloor; lightParams.geometry[3] = kRoomShadeCeiling;
        lightParams.shading[0] = shading.rhoWall; lightParams.shading[1] = shading.rhoFloor;
        lightParams.shading[2] = shading.rhoCeiling;
        lightParams.shading[3] = RoomBounceScale(shading);
        std::copy_n(shading.world, 3, lightParams.worldCount);
        lightParams.worldCount[3] = float(expectedEmitters.size());
        Check(vkResetCommandPool(device, cmdPool, 0), "vkResetCommandPool LIGHT");
        Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer LIGHT");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lightPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lightParams), &lightParams);
        vkCmdDispatch(cmd, kRoomLightmap / 8, kRoomLightmap / 8, kRoomFaces);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer LIGHT");
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit LIGHT");
        Check(vkQueueWaitIdle(queue), "vkQueueWaitIdle LIGHT");
        const float* gpuLight = static_cast<const float*>(lightmap.mapped);
        size_t lightBad = 0; float lightWorst = 0;
        for (int face = 0; face < kRoomFaces; ++face)
            for (int y = 0; y < kRoomLightmap; ++y)
                for (int x = 0; x < kRoomLightmap; ++x) {
                    float cpu[3];
                    if (!RoomTexel(room, shading, expectedEmitters, face, x, y, cpu))
                        throw std::runtime_error("CPU lightmap texel failed");
                    size_t base = ((size_t(face) * kRoomLightmap + y) * kRoomLightmap + x) * 4;
                    for (int ch = 0; ch < 3; ++ch) {
                        float diff = std::fabs(gpuLight[base + ch] - cpu[ch]);
                        lightWorst = std::max(lightWorst, diff);
                        if (!std::isfinite(diff) || diff > 1e-5f) ++lightBad;
                    }
                }
        std::printf("Vulkan/CPU room LIGHT: %zu of %zu channels outside 1e-5 (worst %.8f)\n",
                    lightBad, lightmapValues / 4 * 3, lightWorst);
        vkDestroyPipeline(device, lightPipeline, nullptr);
        vkDestroyShaderModule(device, lightModule, nullptr);
        vkDestroyPipeline(device, emitPipeline, nullptr);
        vkDestroyShaderModule(device, emitModule, nullptr);
        vkDestroyCommandPool(device, cmdPool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
        for (Buffer* b : {&source, &table, &output, &glowBuffer, &lightmap}) {
            vkUnmapMemory(device, b->memory); vkDestroyBuffer(device, b->handle, nullptr);
            vkFreeMemory(device, b->memory, nullptr);
        }
        vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
        return bad || emitBad || lightBad ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Room GPU probe: %s\n", error.what()); return 1;
    }
}
