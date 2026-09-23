#include "vulkan_capture_scale.h"
#include "resource_path.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace vrx {
namespace {
constexpr uint32_t kBindings = 6;
constexpr uint32_t kMaxSets = 16;
constexpr uint32_t kMaxSource = 8192;

void Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}

struct Parameters {
    uint32_t outW, outH, taps;
};
}

VulkanCaptureScale::VulkanCaptureScale(VkPhysicalDevice gpu, VkDevice device, uint32_t queueFamily,
                                       VkBuffer color, uint32_t colorWidth, uint32_t colorHeight)
    : gpu_(gpu), device_(device), family_(queueFamily), color_(color),
      colorWidth_(colorWidth), colorHeight_(colorHeight) {
    const auto tableUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CreateBuffer(xTaps_, VkDeviceSize(colorWidth) * 4 * sizeof(int32_t), tableUsage, false);
    CreateBuffer(yTaps_, VkDeviceSize(colorHeight) * 4 * sizeof(int32_t), tableUsage, false);
    CreateBuffer(xFraction_, VkDeviceSize(colorWidth) * sizeof(float), tableUsage, false);
    CreateBuffer(yFraction_, VkDeviceSize(colorHeight) * sizeof(float), tableUsage, false);

    VkDescriptorSetLayoutBinding bindings[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = kBindings;
    layoutInfo.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_), "vkCreateDescriptorSetLayout (capture scale)");
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxSets},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxSets * (kBindings - 1)}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = kMaxSets;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_), "vkCreateDescriptorPool (capture scale)");

#ifdef VRX_CAPTURE_SCALE_SPV_PATH
    const char* builtIn = VRX_CAPTURE_SCALE_SPV_PATH;
#else
    const char* builtIn = nullptr;
#endif
    const std::string shaderPath = ShaderPath("VRX_CAPTURE_SCALE_SPV", "capture_scale.comp.spv", builtIn);
    const char* path = shaderPath.c_str();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error(std::string("Cannot open capture scale shader: ") + path);
    const std::streamsize length = file.tellg();
    if (length <= 0 || length % 4) throw std::runtime_error("Invalid capture scale shader bytecode");
    std::vector<uint32_t> code(size_t(length) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), length);
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = size_t(length);
    moduleInfo.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(device_, &moduleInfo, nullptr, &module), "vkCreateShaderModule (capture scale)");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &layout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_), "vkCreatePipelineLayout (capture scale)");
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout_;
    const VkResult result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, module, nullptr);
    Check(result, "vkCreateComputePipelines (capture scale)");
    std::printf("Capture scale shader: %s\n", path);
}

VulkanCaptureScale::~VulkanCaptureScale() {
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (layout_) vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    for (Buffer* buffer : {&colorCheck_, &sourceCheck_, &yFraction_, &xFraction_, &yTaps_, &xTaps_})
        DestroyBuffer(*buffer);
}

void VulkanCaptureScale::CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(device_, &info, nullptr, &out.buffer), "vkCreateBuffer (capture scale)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, out.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gpu_, &properties);
    // Tables are device-local. The check copies are read back by the CPU and
    // prefer cached memory, where CPU reads are fast.
    const VkMemoryPropertyFlags wanted = host ?
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t memoryType = UINT32_MAX;
    for (VkMemoryPropertyFlags flags : {host ? wanted | VK_MEMORY_PROPERTY_HOST_CACHED_BIT : wanted, wanted}) {
        for (uint32_t i = 0; i < properties.memoryTypeCount && memoryType == UINT32_MAX; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & flags) == flags) memoryType = i;
        if (memoryType != UINT32_MAX) break;
    }
    if (memoryType == UINT32_MAX) throw std::runtime_error("No memory type for capture scale buffer");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    Check(vkAllocateMemory(device_, &allocation, nullptr, &out.memory), "vkAllocateMemory (capture scale)");
    Check(vkBindBufferMemory(device_, out.buffer, out.memory, 0), "vkBindBufferMemory (capture scale)");
    if (host) Check(vkMapMemory(device_, out.memory, 0, size, 0, &out.mapped), "vkMapMemory (capture scale)");
    out.size = size;
}

void VulkanCaptureScale::DestroyBuffer(Buffer& buffer) {
    if (buffer.mapped) vkUnmapMemory(device_, buffer.memory);
    if (buffer.buffer) vkDestroyBuffer(device_, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
    buffer = {};
}

// The tables are device-local and written by the command buffer itself when
// the source size changes. (Freshly written host-visible tables were not
// reliably visible to the shader on the reference NVIDIA driver: whole pages
// read as zero.) The caller's previous frame has completed before a rewrite.
void VulkanCaptureScale::UpdateTables(VkCommandBuffer command, uint32_t sourceWidth, uint32_t sourceHeight) {
    if (tables_.sourceWidth == int(sourceWidth) && tables_.sourceHeight == int(sourceHeight)) return;
    tables_ = BuildResampleTables(int(sourceWidth), int(sourceHeight), int(colorWidth_), int(colorHeight_),
        FitCapture(int(sourceWidth), int(sourceHeight), int(colorWidth_), int(colorHeight_)));
    auto update = [&](const Buffer& buffer, const void* data, size_t bytes) {
        constexpr size_t kMaxUpdate = 65536;   // vkCmdUpdateBuffer's limit per call
        for (size_t offset = 0; offset < bytes; offset += kMaxUpdate)
            vkCmdUpdateBuffer(command, buffer.buffer, offset, std::min(kMaxUpdate, bytes - offset),
                              static_cast<const char*>(data) + offset);
    };
    update(xTaps_, tables_.x.data(), tables_.x.size() * sizeof(tables_.x[0]));
    update(yTaps_, tables_.y.data(), tables_.y.size() * sizeof(tables_.y[0]));
    update(xFraction_, tables_.fx.data(), tables_.fx.size() * sizeof(float));
    update(yFraction_, tables_.fy.data(), tables_.fy.size() * sizeof(float));
    VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    written.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &written, 0, nullptr, 0, nullptr);
    std::printf("Capture scale: %ux%u source into %ux%u colour, %d x %d taps\n", sourceWidth, sourceHeight,
                colorWidth_, colorHeight_, tables_.taps, tables_.taps);
}

VkDescriptorSet VulkanCaptureScale::SetFor(VkImageView view) {
    auto found = sets_.find(view);
    if (found != sets_.end()) return found->second;
    if (sets_.size() >= kMaxSets) ForgetViews();
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = pool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(vkAllocateDescriptorSets(device_, &setInfo, &set), "vkAllocateDescriptorSets (capture scale)");
    VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bufferInfos[kBindings - 1] = {{xTaps_.buffer, 0, VK_WHOLE_SIZE},
        {yTaps_.buffer, 0, VK_WHOLE_SIZE}, {xFraction_.buffer, 0, VK_WHOLE_SIZE},
        {yFraction_.buffer, 0, VK_WHOLE_SIZE}, {color_, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (i == 0) writes[i].pImageInfo = &imageInfo;
        else writes[i].pBufferInfo = &bufferInfos[i - 1];
    }
    vkUpdateDescriptorSets(device_, kBindings, writes, 0, nullptr);
    sets_[view] = set;
    return set;
}

void VulkanCaptureScale::ForgetViews() {
    if (sets_.empty()) return;
    Check(vkResetDescriptorPool(device_, pool_, 0), "vkResetDescriptorPool (capture scale)");
    sets_.clear();
}

void VulkanCaptureScale::Record(VkCommandBuffer command, VkImage image, VkImageView view,
                                uint32_t sourceWidth, uint32_t sourceHeight, bool foreign, bool check) {
    if (!sourceWidth || !sourceHeight || sourceWidth > kMaxSource || sourceHeight > kMaxSource)
        throw std::runtime_error("Invalid capture source size");
    UpdateTables(command, sourceWidth, sourceHeight);
    const VkDescriptorSet set = SetFor(view);
    VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    acquire.srcQueueFamilyIndex = foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
    acquire.dstQueueFamilyIndex = foreign ? family_ : VK_QUEUE_FAMILY_IGNORED;
    acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    acquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | (check ? VK_ACCESS_TRANSFER_READ_BIT : 0);
    acquire.image = image;
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &acquire);
    const Parameters params{colorWidth_, colorHeight_, uint32_t(tables_.taps)};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(command, (colorWidth_ + 7) / 8, (colorHeight_ + 7) / 8, 1);
    if (check) {
        const VkDeviceSize sourceBytes = VkDeviceSize(sourceWidth) * sourceHeight * 4;
        const VkDeviceSize colorBytes = VkDeviceSize(colorWidth_) * colorHeight_ * 4;
        if (sourceCheck_.size < sourceBytes) {
            DestroyBuffer(sourceCheck_);
            CreateBuffer(sourceCheck_, sourceBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
        if (!colorCheck_.buffer) CreateBuffer(colorCheck_, colorBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {sourceWidth, sourceHeight, 1};
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_GENERAL, sourceCheck_.buffer, 1, &region);
        checkedWidth_ = sourceWidth;
        checkedHeight_ = sourceHeight;
    }
    VkImageMemoryBarrier release = acquire;
    release.srcQueueFamilyIndex = foreign ? family_ : VK_QUEUE_FAMILY_IGNORED;
    release.dstQueueFamilyIndex = foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
    release.srcAccessMask = 0;
    release.dstAccessMask = 0;
    VkBufferMemoryBarrier colorBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    colorBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    colorBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    colorBarrier.srcQueueFamilyIndex = colorBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    colorBarrier.buffer = color_;
    colorBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 1, &colorBarrier, foreign ? 1 : 0, &release);
    if (check) {
        VkBufferCopy region{0, 0, VkDeviceSize(colorWidth_) * colorHeight_ * 4};
        vkCmdCopyBuffer(command, color_, colorCheck_.buffer, 1, &region);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.size = VK_WHOLE_SIZE;
        VkBufferMemoryBarrier hosts[2] = {host, host};
        hosts[0].buffer = sourceCheck_.buffer;
        hosts[1].buffer = colorCheck_.buffer;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 2, hosts, 0, nullptr);
    }
}

std::vector<uint32_t> VulkanCaptureScale::CheckedColor() const {
    if (!colorCheck_.mapped || !checkedWidth_) return {};
    const auto* pixels = static_cast<const uint32_t*>(colorCheck_.mapped);
    return std::vector<uint32_t>(pixels, pixels + size_t(colorWidth_) * colorHeight_);
}

bool VulkanCaptureScale::CompareReference() const {
    if (!sourceCheck_.mapped || !colorCheck_.mapped || !checkedWidth_) return false;
    // The copy holds the view format's bytes: B, G, R, A for the BGRA formats.
    std::vector<uint32_t> expected;
    ScaleCaptureColor(static_cast<const unsigned char*>(sourceCheck_.mapped), size_t(checkedWidth_) * 4,
        int(checkedWidth_), int(checkedHeight_), false, int(colorWidth_), int(colorHeight_), expected, 4);
    const auto* actual = static_cast<const uint32_t*>(colorCheck_.mapped);
    size_t differing = 0;
    int worst = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] == actual[i]) continue;
        ++differing;
        for (int c = 0; c < 3; ++c)
            worst = std::max(worst, std::abs(int((expected[i] >> (8 * c)) & 255u) - int((actual[i] >> (8 * c)) & 255u)));
    }
    std::printf("GPU/CPU capture scale check (%ux%u source): %zu of %zu pixels differ (worst %d)\n",
                checkedWidth_, checkedHeight_, differing, expected.size(), worst);
    return differing == 0;
}
} // namespace vrx
