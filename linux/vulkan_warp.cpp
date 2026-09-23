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
constexpr VkDeviceSize depthPixels = VkDeviceSize(kSyntheticWidth) * kSyntheticHeight;
constexpr uint32_t kBindings = 6;

void Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}

// The shader's NearAt: bilinear depth-grid nearness at a colour pixel. The
// ratios are the push constants' values, so CPU and GPU do the same arithmetic.
std::vector<float> NearnessAtColor(const std::vector<float>& near01, int cw, int ch,
                                   float perX, float perY) {
    const int w = kSyntheticWidth, h = kSyntheticHeight;
    std::vector<float> out(size_t(cw) * ch);
    for (int y = 0; y < ch; ++y) for (int x = 0; x < cw; ++x) {
        const float fx = (float(x) + 0.5f) * perX - 0.5f, fy = (float(y) + 0.5f) * perY - 0.5f;
        const float x0f = std::floor(fx), y0f = std::floor(fy);
        const float tx = fx - x0f, ty = fy - y0f;
        const int x0 = std::clamp(int(x0f), 0, w - 1), x1 = std::clamp(int(x0f) + 1, 0, w - 1);
        const int y0 = std::clamp(int(y0f), 0, h - 1), y1 = std::clamp(int(y0f) + 1, 0, h - 1);
        const float a = near01[size_t(y0) * w + x0], b = near01[size_t(y0) * w + x1];
        const float c = near01[size_t(y1) * w + x0], d = near01[size_t(y1) * w + x1];
        const float top = a + tx * (b - a), bottom = c + tx * (d - c);
        out[size_t(y) * cw + x] = top + ty * (bottom - top);
    }
    return out;
}

// WarpEyeFill (stereo_warp.h) for any colour width, over nearness already at
// colour resolution. Output rows are cw pixels.
void WarpEyeAtColor(const std::vector<uint32_t>& scene, const std::vector<float>& nearColor,
                    int cw, int ch, float eyeOffset, float focalPx, float scale,
                    float invZNear, float invZFar, float nearZ, float farZ, bool doWarp,
                    unsigned char* outRGBA, float* outDepth) {
    std::vector<int> src(cw);
    std::vector<float> srcDest(cw);
    for (int y = 0; y < ch; ++y) {
        const float* nrow = nearColor.data() + size_t(y) * cw;
        std::fill(src.begin(), src.end(), -1);
        for (int x = 0; x < cw; ++x) {
            int dx = x;
            float destF = float(x);
            if (doWarp) {
                const float invZ = invZFar + nrow[x] * (invZNear - invZFar);
                const float s = scale * focalPx * eyeOffset * invZ;
                destF = float(x) - s;
                dx = x - int(std::lround(s));
            }
            if (dx < 0 || dx >= cw) continue;
            if (src[dx] < 0 || nrow[x] > nrow[src[dx]]) { src[dx] = x; srcDest[dx] = destF; }
        }
        int lastValid = -1;
        for (int x = 0; x < cw; ++x) {
            if (src[x] >= 0) { lastValid = src[x]; continue; }
            int r = x + 1;
            while (r < cw && src[r] < 0) ++r;
            FillHole(src, nrow, cw, x, r, lastValid, r < cw ? src[r] : -1, kFillMirror);
            x = r - 1;
        }
        for (int x = 0; x < cw; ++x) {
            const bool filled = src[x] < 0;
            const int s = filled ? -2 - src[x] : src[x];
            const float pos = !filled ?
                std::min(std::max(float(s) + (float(x) - srcDest[x]), 0.0f), float(cw - 1)) : float(s);
            const int i0 = int(pos), i1 = std::min(i0 + 1, cw - 1);
            const float fr = pos - i0;
            const uint32_t p0 = scene[size_t(y) * cw + i0], p1 = scene[size_t(y) * cw + i1];
            unsigned char* out = outRGBA + (size_t(y) * cw + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const float c0 = float((p0 >> (8 * c)) & 255u) / 255.0f;
                const float c1 = float((p1 >> (8 * c)) & 255u) / 255.0f;
                out[c] = static_cast<unsigned char>(std::lround((c0 + fr * (c1 - c0)) * 255.0f));
            }
            out[3] = 255;
            const float invZ = invZFar + nrow[s] * (invZNear - invZFar);
            outDepth[size_t(y) * cw + x] = (farZ / (farZ - nearZ)) * (1.0f - nearZ * invZ);
        }
    }
}
} // namespace

struct VulkanWarp::Parameters {
    uint32_t cw = 0, ch = 0, dw = kSyntheticWidth, dh = kSyntheticHeight;
    float depthPerColorX = 1.0f, depthPerColorY = 1.0f;
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
    int writeDepth = 0;
};
static_assert(sizeof(float) == 4 && sizeof(uint32_t) == 4, "push constant layout");

VulkanWarp::VulkanWarp(VkPhysicalDevice gpu, VkDevice device, VkFormat format,
                       uint32_t colorWidth, uint32_t colorHeight)
    : gpu_(gpu), device_(device), format_(format),
      colorWidth_(colorWidth), colorHeight_(colorHeight) {
    static_assert(sizeof(Parameters) == 68, "push constant layout mismatch");
    if (!colorWidth || !colorHeight || colorWidth > 4096 || colorHeight > 4096)
        throw std::runtime_error("Invalid stereo colour size");
    const VkDeviceSize colorPixels = VkDeviceSize(colorWidth) * colorHeight;
    const auto storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const auto transferSrc = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const auto transferDst = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CreateBuffer(scene_, colorPixels * 4, storage | transferDst, false);
    CreateBuffer(nearness_, depthPixels * 4, storage | transferDst, false);
    CreateBuffer(color_, 2 * colorPixels * 4, storage | transferSrc, false);
    CreateBuffer(depth_, 2 * colorPixels * 4, storage | transferSrc, false);
    CreateBuffer(source_, 2 * colorPixels * 4, storage, false);
    CreateBuffer(landed_, 2 * colorPixels * 4, storage, false);
    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        CreateBuffer(sceneStaging_[slot], colorPixels * 4, transferSrc, true);
        CreateBuffer(nearnessStaging_[slot], depthPixels * 4, transferSrc, true);
    }
    CreateBuffer(readback_, 2 * 2 * colorPixels * 4, transferDst, true);

    VkDescriptorSetLayoutBinding bindings[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = kBindings;
    layoutInfo.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorLayout_), "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindings};
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
    VkDescriptorBufferInfo bufferInfos[kBindings] = {{scene_.buffer, 0, VK_WHOLE_SIZE},
        {nearness_.buffer, 0, VK_WHOLE_SIZE}, {color_.buffer, 0, VK_WHOLE_SIZE},
        {depth_.buffer, 0, VK_WHOLE_SIZE}, {source_.buffer, 0, VK_WHOLE_SIZE},
        {landed_.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(device_, kBindings, writes, 0, nullptr);

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
    std::printf("Stereo shader: %s (colour %ux%u, depth %dx%d)\n", path, colorWidth_, colorHeight_,
                kSyntheticWidth, kSyntheticHeight);
}

VulkanWarp::~VulkanWarp() {
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    for (Buffer* buffer : {&readback_, &nearnessStaging_[0], &nearnessStaging_[1], &sceneStaging_[0],
                           &sceneStaging_[1], &landed_, &source_, &depth_, &color_, &nearness_, &scene_})
        DestroyBuffer(*buffer);
}

void VulkanWarp::CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
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
    const VkMemoryPropertyFlags wanted = hostVisible ?
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT :
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    // The CPU reads buffers the GPU writes (TRANSFER_DST); uncached memory makes
    // those reads very slow, so they prefer cached memory.
    const bool readBack = hostVisible && (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    uint32_t memoryType = UINT32_MAX;
    for (VkMemoryPropertyFlags flags : {readBack ? wanted | VK_MEMORY_PROPERTY_HOST_CACHED_BIT : wanted, wanted}) {
        for (uint32_t i = 0; i < properties.memoryTypeCount && memoryType == UINT32_MAX; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & flags) == flags) memoryType = i;
        if (memoryType != UINT32_MAX) break;
    }
    if (memoryType == UINT32_MAX) throw std::runtime_error(hostVisible ?
        "No coherent host-visible buffer memory" : "No device-local buffer memory");
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    Check(vkAllocateMemory(device_, &allocation, nullptr, &out.memory), "vkAllocateMemory");
    Check(vkBindBufferMemory(device_, out.buffer, out.memory, 0), "vkBindBufferMemory");
    if (hostVisible) Check(vkMapMemory(device_, out.memory, 0, size, 0, &out.mapped), "vkMapMemory");
}

void VulkanWarp::DestroyBuffer(Buffer& buffer) {
    if (buffer.mapped) vkUnmapMemory(device_, buffer.memory);
    if (buffer.buffer) vkDestroyBuffer(device_, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
    buffer = {};
}

void VulkanWarp::UploadColor(const std::vector<uint32_t>& rgba) {
    if (rgba.size() != size_t(colorWidth_) * colorHeight_)
        throw std::runtime_error("Stereo colour dimensions differ from the warp");
    std::memcpy(sceneStaging_[slot_].mapped, rgba.data(), rgba.size() * sizeof(uint32_t));
    colorPending_ = true;
}

void VulkanWarp::UploadNearness(const std::vector<float>& nearness) {
    if (nearness.size() != size_t(depthPixels))
        throw std::runtime_error("Depth dimensions differ from the warp");
    std::memcpy(nearnessStaging_[slot_].mapped, nearness.data(), nearness.size() * sizeof(float));
    nearnessPending_ = true;
}

void VulkanWarp::RecordUpload(VkCommandBuffer command) {
    if (!colorPending_ && !nearnessPending_) return;
    VkBufferMemoryBarrier barriers[2]{};
    uint32_t count = 0;
    auto copy = [&](const Buffer& from, const Buffer& to, VkDeviceSize size) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(command, from.buffer, to.buffer, 1, &region);
        auto& barrier = barriers[count++];
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = to.buffer;
        barrier.size = VK_WHOLE_SIZE;
    };
    if (colorPending_) copy(sceneStaging_[slot_], scene_, VkDeviceSize(colorWidth_) * colorHeight_ * 4);
    if (nearnessPending_) copy(nearnessStaging_[slot_], nearness_, depthPixels * 4);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, count, barriers, 0, nullptr);
    colorPending_ = nearnessPending_ = false;
}

void VulkanWarp::SetStereoGeometry(float screenDistance, float screenWidth, float ipd, bool depthAvailable) {
    if (std::isfinite(screenDistance) && screenDistance > 0.0f) screenDistance_ = screenDistance;
    if (std::isfinite(screenWidth) && screenWidth > 0.0f) screenWidth_ = screenWidth;
    if (std::isfinite(ipd) && ipd >= 0.04f && ipd <= 0.09f) ipd_ = ipd;
    depthAvailable_ = depthAvailable;
}

VulkanWarp::Parameters VulkanWarp::MakeParameters(bool writeDepth) const {
    Parameters params{};
    params.cw = colorWidth_;
    params.ch = colorHeight_;
    params.depthPerColorX = float(kSyntheticWidth) / float(colorWidth_);
    params.depthPerColorY = float(kSyntheticHeight) / float(colorHeight_);
    // The screen spans colorWidth_ pixels across screenWidth_ metres at
    // screenDistance_, so this is the focal length that keeps disparity true.
    params.focalPx = float(colorWidth_) * screenDistance_ / screenWidth_;
    params.scale = depthAvailable_ ? strength_ : 0.0f;
    params.invZNear -= 1.0f / screenDistance_;
    params.invZFar -= 1.0f / screenDistance_;
    params.eyeOffset = 0.5f * ipd_;
    params.bgra = format_ == VK_FORMAT_B8G8R8A8_SRGB || format_ == VK_FORMAT_B8G8R8A8_UNORM;
    params.writeDepth = writeDepth ? 1 : 0;
    return params;
}

void VulkanWarp::Record(VkCommandBuffer command, bool readback) {
    const Parameters params = MakeParameters(readback);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                            0, 1, &descriptorSet_, 0, nullptr);
    vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(command, 1, (colorHeight_ + 7) / 8, 2);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = color_.buffer;
    barrier.size = VK_WHOLE_SIZE;
    VkBufferMemoryBarrier barriers[2] = {barrier, barrier};
    barriers[1].buffer = depth_.buffer;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, readback ? 2 : 1, barriers, 0, nullptr);
    if (!readback) return;
    const VkDeviceSize eyesBytes = 2 * VkDeviceSize(colorWidth_) * colorHeight_ * 4;
    VkBufferCopy colorRegion{0, 0, eyesBytes}, depthRegion{0, eyesBytes, eyesBytes};
    vkCmdCopyBuffer(command, color_.buffer, readback_.buffer, 1, &colorRegion);
    vkCmdCopyBuffer(command, depth_.buffer, readback_.buffer, 1, &depthRegion);
    VkBufferMemoryBarrier host = barrier;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.buffer = readback_.buffer;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &host, 0, nullptr);
}

bool VulkanWarp::CompareReference(const std::vector<uint32_t>& rgba,
                                  const std::vector<float>& nearness) const {
    const int cw = int(colorWidth_), ch = int(colorHeight_);
    const size_t eyePixels = size_t(cw) * ch;
    if (rgba.size() != eyePixels || nearness.size() != size_t(depthPixels)) return false;
    const Parameters params = MakeParameters(true);
    const bool depthGrid = cw == kSyntheticWidth && ch == kSyntheticHeight;
    // At the depth grid's size, use the shared Windows reference itself.
    std::vector<unsigned char> rgb;
    std::vector<float> nearColor;
    if (depthGrid) {
        rgb.resize(eyePixels * 3);
        for (size_t i = 0; i < eyePixels; ++i)
            for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = (rgba[i] >> (8 * c)) & 255u;
    } else {
        nearColor = NearnessAtColor(nearness, cw, ch, params.depthPerColorX, params.depthPerColorY);
    }
    const size_t pitch = depthGrid ? kStereoRowPitch : size_t(cw) * 4;
    std::vector<unsigned char> reference(pitch * ch);
    std::vector<float> referenceDepth(pitch / 4 * ch);
    const auto* gpuColor = static_cast<const unsigned char*>(readback_.mapped);
    const auto* gpuDepth = reinterpret_cast<const float*>(gpuColor + 2 * eyePixels * 4);
    size_t colorDifferences = 0, colorOverTolerance = 0, depthDifferences = 0;
    int maxChannelDifference = 0;
    float maxDepthDifference = 0.0f;
    const bool bgra = params.bgra != 0;
    for (int eye = 0; eye < 2; ++eye) {
        const float offset = eye == 0 ? -params.eyeOffset : params.eyeOffset;
        if (depthGrid)
            WarpEyeFill(rgb, nearness, offset, params.focalPx, strength_,
                params.invZNear, params.invZFar, params.nearZ, params.farZ, depthAvailable_,
                kFillMirror, reference.data(), referenceDepth.data(), true);
        else
            WarpEyeAtColor(rgba, nearColor, cw, ch, offset, params.focalPx, strength_,
                params.invZNear, params.invZFar, params.nearZ, params.farZ, depthAvailable_,
                reference.data(), referenceDepth.data());
        for (int y = 0; y < ch; ++y) for (int x = 0; x < cw; ++x) {
            const size_t referenceIndex = size_t(y) * pitch + size_t(x) * 4;
            const size_t gpuIndex = size_t(eye) * eyePixels + size_t(y) * cw + x;
            for (int c = 0; c < 4; ++c) {
                const int expected = reference[referenceIndex + (bgra ? (c == 0 ? 2 : c == 2 ? 0 : c) : c)];
                const int difference = std::abs(expected - int(gpuColor[gpuIndex * 4 + c]));
                if (difference) { ++colorDifferences; maxChannelDifference = std::max(maxChannelDifference, difference); }
                if (difference > 1) ++colorOverTolerance;
            }
            const float difference = std::fabs(referenceDepth[size_t(y) * (pitch / 4) + x] - gpuDepth[gpuIndex]);
            if (difference > 0.00001f) { ++depthDifferences; maxDepthDifference = std::max(maxDepthDifference, difference); }
        }
    }
    std::printf("GPU/CPU warp check at %dx%d colour: %zu color channels differ (max %d; %zu above one byte), %zu depth pixels differ (max %.8f)\n",
        cw, ch, colorDifferences, maxChannelDifference, colorOverTolerance, depthDifferences, maxDepthDifference);
    return colorOverTolerance == 0 && depthDifferences == 0;
}
} // namespace vrx
