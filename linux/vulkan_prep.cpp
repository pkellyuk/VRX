#include "vulkan_prep.h"
#include "resource_path.h"
#include "model_prep_cpu.h"
#include "synthetic_scene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace vrx {
namespace {
void Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}
struct Parameters {
    uint32_t sourceWidth = kSyntheticWidth;
    uint32_t sourceHeight = kSyntheticHeight;
    uint32_t outputWidth = VulkanPrep::width;
    uint32_t outputHeight = VulkanPrep::height;
    uint32_t taps = 2;
    uint32_t exactLoad = 0;
    float cropX = 0;
    float cropY = 0;
    float cropSize = 1;
    uint32_t normalize = 0;  // ZipDepth normalises internally.
};
static_assert(sizeof(Parameters) == 40, "model prep push constant mismatch");


}

VulkanPrep::VulkanPrep(VkPhysicalDevice gpu, VkDevice device, VkBuffer packedScene,
                       uint32_t sourceWidth, uint32_t sourceHeight)
    : device_(device), sourceWidth_(sourceWidth), sourceHeight_(sourceHeight) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = VkDeviceSize(3) * width * height * sizeof(float);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        Check(vkCreateBuffer(device_, &bufferInfo, nullptr, &output_[slot]), "vkCreateBuffer(model input)");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, output_[slot], &requirements);
        // The CPU reads the model input for every live frame; uncached (write-
        // combined) memory makes those reads very slow, so prefer cached memory.
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t memoryType = UINT32_MAX;
        for (VkMemoryPropertyFlags wanted : {host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, host}) {
            for (uint32_t i = 0; i < properties.memoryTypeCount && memoryType == UINT32_MAX; ++i)
                if ((requirements.memoryTypeBits & (1u << i)) &&
                    (properties.memoryTypes[i].propertyFlags & wanted) == wanted) memoryType = i;
            if (memoryType != UINT32_MAX) break;
        }
        if (memoryType == UINT32_MAX) throw std::runtime_error("No coherent model input buffer memory");
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        Check(vkAllocateMemory(device_, &allocation, nullptr, &memory_[slot]), "vkAllocateMemory(model input)");
        Check(vkBindBufferMemory(device_, output_[slot], memory_[slot], 0), "vkBindBufferMemory(model input)");
        Check(vkMapMemory(device_, memory_[slot], 0, bufferInfo.size, 0, &mapped_[slot]), "vkMapMemory(model input)");
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorLayout_), "vkCreateDescriptorSetLayout(model prep)");
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kFrameSlots};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFrameSlots;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    Check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool(model prep)");
    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = descriptorPool_;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &descriptorLayout_;
        Check(vkAllocateDescriptorSets(device_, &setInfo, &descriptorSet_[slot]), "vkAllocateDescriptorSets(model prep)");
        VkDescriptorBufferInfo inputInfo{packedScene, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo outputInfo{output_[slot], 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descriptorSet_[slot];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = i == 0 ? &inputInfo : &outputInfo;
        }
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }

#ifdef VRX_PREP_SPV_PATH
    const char* builtIn = VRX_PREP_SPV_PATH;
#else
    const char* builtIn = nullptr;
#endif
    const std::string shaderPath = ShaderPath("VRX_PREP_SPV", "model_prep.comp.spv", builtIn);
    const char* path = shaderPath.c_str();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error(std::string("Cannot open model prep shader: ") + path);
    const std::streamsize length = file.tellg();
    if (length <= 0 || length % 4) throw std::runtime_error("Invalid model prep shader bytecode");
    std::vector<uint32_t> code(size_t(length) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), length);
    if (!file) throw std::runtime_error("Cannot read model prep shader bytecode");
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = size_t(length);
    moduleInfo.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(device_, &moduleInfo, nullptr, &module), "vkCreateShaderModule(model prep)");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_), "vkCreatePipelineLayout(model prep)");
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
    Check(result, "vkCreateComputePipelines(model prep)");
    std::printf("Model prep shader: %s\n", path);
}

VulkanPrep::~VulkanPrep() {
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        if (mapped_[slot]) vkUnmapMemory(device_, memory_[slot]);
        if (output_[slot]) vkDestroyBuffer(device_, output_[slot], nullptr);
        if (memory_[slot]) vkFreeMemory(device_, memory_[slot], nullptr);
    }
}

void VulkanPrep::Record(VkCommandBuffer command) {
    Parameters params{};
    params.sourceWidth = sourceWidth_;
    params.sourceHeight = sourceHeight_;
    params.taps = uint32_t(ModelPrepTaps(int(sourceWidth_)));
    params.exactLoad = sourceWidth_ == uint32_t(width) && sourceHeight_ == uint32_t(height);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout_, 0, 1, &descriptorSet_[slot_], 0, nullptr);
    vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(params), &params);
    vkCmdDispatch(command, (width + 7) / 8, (height + 7) / 8, 1);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = output_[slot_];
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
}

bool VulkanPrep::CompareReference(const std::vector<uint32_t>& rgba) const {
    if (rgba.size() != size_t(sourceWidth_) * sourceHeight_) return false;
    const auto* got = ModelInput();
    size_t bad = 0;
    float worst = 0.0f;
    constexpr float tolerance = 0.0028f; // Windows sampled-path threshold.
    const size_t plane = size_t(width) * height;
    const auto expected = PrepareModelInput(rgba.data(), int(sourceWidth_), int(sourceHeight_));
    for (size_t i = 0; i < expected.size(); ++i) {
        const float actual = got[i];
        const float delta = std::fabs(actual - expected[i]);
        if (!std::isfinite(actual) || delta > tolerance) ++bad;
        worst = std::max(worst, delta);
    }
    std::printf("GPU/CPU ZipDepth input check: %zu of %zu values outside %.4f (worst %.7f)\n",
        bad, 3 * plane, tolerance, worst);
    return bad == 0;
}
} // namespace vrx
