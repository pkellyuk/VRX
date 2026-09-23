#include "vulkan_warp.h"
#include "stereo_warp.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace vrx {
namespace {
constexpr VkDeviceSize pixels = VkDeviceSize(kSyntheticWidth) * kSyntheticHeight;

void Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}

struct Parameters {
    float focalPx = float(kSyntheticWidth);
    float scale = 1.0f;
    float invZNear = 1.0f / 1.2f;
    float invZFar = 1.0f / 12.0f;
    float nearZ = 0.1f;
    float farZ = 100.0f;
    float eyeOffset = 0.032f;
    int fillMode = kFillMirror;
    int subpixel = 1;
    int bgra = 0;
};
static_assert(sizeof(Parameters) == 40, "push constant layout mismatch");
}

VulkanWarp::VulkanWarp(VkPhysicalDevice gpu, VkDevice device, VkFormat format)
    : gpu_(gpu), device_(device), format_(format) {
    CreateBuffer(scene_, pixels * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    CreateBuffer(nearness_, pixels * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    CreateBuffer(color_, 2 * pixels * 4,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    CreateBuffer(depth_, 2 * pixels * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorLayout_), "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    Check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &descriptorLayout_;
    Check(vkAllocateDescriptorSets(device_, &setInfo, &descriptorSet_), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo bufferInfos[4] = {{scene_.buffer, 0, VK_WHOLE_SIZE},
        {nearness_.buffer, 0, VK_WHOLE_SIZE}, {color_.buffer, 0, VK_WHOLE_SIZE},
        {depth_.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);

    const char* env = std::getenv("VRX_WARP_SPV");
#ifdef VRX_STEREO_SPV_PATH
    const char* defaultPath = VRX_STEREO_SPV_PATH;
#else
    const char* defaultPath = "stereo_warp.comp.spv";
#endif
    const char* path = env && *env ? env : defaultPath;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error(std::string("Cannot open stereo shader: ") + path);
    const std::streamsize length = file.tellg();
    if (length <= 0 || length % 4) throw std::runtime_error("Invalid stereo shader bytecode");
    std::vector<uint32_t> code(size_t(length) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), length);
    if (!file) throw std::runtime_error("Cannot read stereo shader bytecode");
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = size_t(length);
    moduleInfo.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(device_, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = pipelineLayout_;
    VkResult result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, module, nullptr);
    Check(result, "vkCreateComputePipelines");
    std::printf("Stereo shader: %s\n", path);
}

VulkanWarp::~VulkanWarp() {
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    DestroyBuffer(depth_);
    DestroyBuffer(color_);
    DestroyBuffer(nearness_);
    DestroyBuffer(scene_);
}

void VulkanWarp::CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(device_, &info, nullptr, &out.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, out.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gpu_, &properties);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memoryType = i; break;
        }
    }
    if (memoryType == UINT32_MAX) throw std::runtime_error("No coherent host-visible buffer memory");
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    Check(vkAllocateMemory(device_, &allocation, nullptr, &out.memory), "vkAllocateMemory");
    Check(vkBindBufferMemory(device_, out.buffer, out.memory, 0), "vkBindBufferMemory");
    Check(vkMapMemory(device_, out.memory, 0, size, 0, &out.mapped), "vkMapMemory");
}

void VulkanWarp::DestroyBuffer(Buffer& buffer) {
    if (buffer.mapped) vkUnmapMemory(device_, buffer.memory);
    if (buffer.buffer) vkDestroyBuffer(device_, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
}

void VulkanWarp::Upload(const std::vector<unsigned char>& rgb, const std::vector<float>& nearness) {
    if (rgb.size() != size_t(pixels) * 3 || nearness.size() != size_t(pixels))
        throw std::runtime_error("Synthetic scene dimensions differ from shader");
    auto* packed = static_cast<uint32_t*>(scene_.mapped);
    for (size_t i = 0; i < size_t(pixels); ++i)
        packed[i] = uint32_t(rgb[i * 3]) | (uint32_t(rgb[i * 3 + 1]) << 8) |
                    (uint32_t(rgb[i * 3 + 2]) << 16) | 0xff000000u;
    std::memcpy(nearness_.mapped, nearness.data(), size_t(pixels) * sizeof(float));
}

void VulkanWarp::SetStereoGeometry(float screenDistance, float ipd, bool depthAvailable) {
    if (std::isfinite(screenDistance) && screenDistance > 0.0f) screenDistance_ = screenDistance;
    if (std::isfinite(ipd) && ipd >= 0.04f && ipd <= 0.09f) ipd_ = ipd;
    depthAvailable_ = depthAvailable;
}

void VulkanWarp::Record(VkCommandBuffer command) {
    Parameters params{};
    params.scale = depthAvailable_ ? strength_ : 0.0f;
    params.invZNear -= 1.0f / screenDistance_;
    params.invZFar -= 1.0f / screenDistance_;
    params.eyeOffset = 0.5f * ipd_;
    params.bgra = format_ == VK_FORMAT_B8G8R8A8_SRGB || format_ == VK_FORMAT_B8G8R8A8_UNORM;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                            0, 1, &descriptorSet_, 0, nullptr);
    vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(command, 1, kSyntheticHeight, 2);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = color_.buffer;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
    barrier.buffer = depth_.buffer;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
}

bool VulkanWarp::CompareReference(const std::vector<unsigned char>& rgb,
                                  const std::vector<float>& nearness) const {
    std::vector<unsigned char> reference(size_t(kStereoRowPitch) * kSyntheticHeight);
    std::vector<float> referenceDepth(size_t(kStereoRowPitch / 4) * kSyntheticHeight);
    const auto* gpuColor = static_cast<const unsigned char*>(color_.mapped);
    const auto* gpuDepth = static_cast<const float*>(depth_.mapped);
    size_t colorDifferences = 0, colorOverTolerance = 0, depthDifferences = 0;
    int maxChannelDifference = 0;
    float maxDepthDifference = 0.0f;
    const bool bgra = format_ == VK_FORMAT_B8G8R8A8_SRGB || format_ == VK_FORMAT_B8G8R8A8_UNORM;
    const float invZNear = 1.0f / 1.2f - 1.0f / screenDistance_;
    const float invZFar = 1.0f / 12.0f - 1.0f / screenDistance_;
    for (int eye = 0; eye < 2; ++eye) {
        const float offset = eye == 0 ? -0.5f * ipd_ : 0.5f * ipd_;
        WarpEyeFill(rgb, nearness, offset, float(kSyntheticWidth), strength_,
            invZNear, invZFar, 0.1f, 100.0f, depthAvailable_, kFillMirror,
            reference.data(), referenceDepth.data(), true);
        for (int y = 0; y < kSyntheticHeight; ++y) for (int x = 0; x < kSyntheticWidth; ++x) {
            size_t referenceIndex = size_t(y) * kStereoRowPitch + size_t(x) * 4;
            size_t gpuIndex = (size_t(eye) * pixels + size_t(y) * kSyntheticWidth + x) * 4;
            for (int c = 0; c < 4; ++c) {
                int expected = reference[referenceIndex + (bgra ? (c == 0 ? 2 : c == 2 ? 0 : c) : c)];
                int difference = std::abs(expected - int(gpuColor[gpuIndex + c]));
                if (difference) { ++colorDifferences; maxChannelDifference = std::max(maxChannelDifference, difference); }
                if (difference > 1) ++colorOverTolerance;
            }
            float difference = std::fabs(referenceDepth[size_t(y) * (kStereoRowPitch / 4) + x] -
                                         gpuDepth[size_t(eye) * pixels + size_t(y) * kSyntheticWidth + x]);
            if (difference > 0.00001f) { ++depthDifferences; maxDepthDifference = std::max(maxDepthDifference, difference); }
        }
    }
    std::printf("GPU/CPU warp check: %zu color channels differ (max %d; %zu above one byte), %zu depth pixels differ (max %.8f)\n",
        colorDifferences, maxChannelDifference, colorOverTolerance, depthDifferences, maxDepthDifference);
    return colorOverTolerance == 0 && depthDifferences == 0;
}
} // namespace vrx
