// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Single graphics queue backend. Control writes are ordered before app submits.
// GPU transport is asynchronous; the CPU reference retains bounded readback.
#pragma once
#include <vulkan/vk_layer.h>
#include "pair_transport.h"
#include "live_tuning.h"
#include "gpu_export.h"
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace xg {
#define XG_ALTERNATE_FUNCTIONS(X) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkGetBufferDeviceAddress) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdUpdateBuffer) X(vkCmdCopyImageToBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkGetFenceStatus) X(vkResetFences) X(vkWaitForFences) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkGetSwapchainImagesKHR) X(vkDeviceWaitIdle) X(vkQueueSubmit) \
    X(vkQueueSubmit2) X(vkQueueSubmit2KHR) X(vkQueuePresentKHR)

class AlternateDevice {
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    PFN_vkSetDeviceLoaderData setLoaderData_ = nullptr;
    std::function<void(const char*)> log_;
    std::mutex gate_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkBuffer control_ = VK_NULL_HANDLE, readback_ = VK_NULL_HANDLE;
    VkDeviceMemory controlMemory_ = VK_NULL_HANDLE, readMemory_ = VK_NULL_HANDLE;
    VkDeviceAddress address_ = 0;
    std::atomic<VkQueue> queue_{VK_NULL_HANDLE};
    uint32_t family_ = 0;
    float shift_ = 0, convergence_ = 0;
    std::wstring tuningPath_;
    uint64_t tuningFrame_ = UINT64_MAX, nextTuningMs_ = 0;
    bool stopped_ = false, right_ = false;
    std::atomic<bool> shiftedShader_{false};
    uint64_t frame_ = 0, leftFrame_ = 0;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> ready_;
    struct ControlSlot { VkCommandBuffer command = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; };
    std::vector<ControlSlot> controls_;
    VkCommandBuffer copyCommand_ = VK_NULL_HANDLE;
    VkFence copyFence_ = VK_NULL_HANDLE;
    bool copyPending_ = false;
    std::vector<uint8_t> leftPixels_, pairPixels_;
    PairWriter writer_;
    bool gpuRequested_ = false;
    LUID adapter_{};
    std::wstring session_;
    std::unique_ptr<GpuExporter> gpu_;
    PFN_vkGetDeviceProcAddr next_ = nullptr;
#define XG_MEMBER(fn) PFN_##fn fn##_ = nullptr;
    XG_ALTERNATE_FUNCTIONS(XG_MEMBER)
#undef XG_MEMBER

    void Stop(const char* reason)
    {
        if (!stopped_) { stopped_ = true; log_(reason); writer_.Close(); if (gpu_) gpu_->Close(); }
        leftPixels_.clear(); leftFrame_ = 0;
    }
    uint32_t MemoryType(uint32_t mask, VkMemoryPropertyFlags flags) const
    {
        for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags &&
                !(memory_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT)) return i;
        return UINT32_MAX;
    }
    bool Buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
        bool address, VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = usage;
        if (vkCreateBuffer_(device_, &ci, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements needs{}; vkGetBufferMemoryRequirements_(device_, buffer, &needs);
        const uint32_t type = MemoryType(needs.memoryTypeBits, flags);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateFlagsInfo af{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        af.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = needs.size; ai.memoryTypeIndex = type; ai.pNext = address ? &af : nullptr;
        return vkAllocateMemory_(device_, &ai, nullptr, &memory) == VK_SUCCESS &&
            vkBindBufferMemory_(device_, buffer, memory, 0) == VK_SUCCESS;
    }
    bool Command(VkCommandBuffer& command)
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        return vkAllocateCommandBuffers_(device_, &ai, &command) == VK_SUCCESS &&
            setLoaderData_(device_, command) == VK_SUCCESS;
    }
    bool Begin(VkCommandBuffer command)
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkResetCommandBuffer_(command, 0) == VK_SUCCESS && vkBeginCommandBuffer_(command, &bi) == VK_SUCCESS;
    }
    bool Control(VkQueue queue)
    {
        // Read at most five times per second, only before a new left frame.
        // A whole exported pair always uses one set of tuning values.
        if (!stopped_ && !right_ && frame_ != tuningFrame_ && !tuningPath_.empty())
        {
            tuningFrame_ = frame_;
            const auto now = GetTickCount64();
            if (now >= nextTuningMs_)
            {
                nextTuningMs_ = now + 200;
                const HANDLE file = CreateFileW(tuningPath_.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE)
                {
                    char text[128]; DWORD count = 0;
                    const bool read = ReadFile(file, text, sizeof(text), &count, nullptr) != 0;
                    CloseHandle(file);
                    float s = 0, c = 0;
                    if (read && count < sizeof(text) && ParseTuning(std::string_view(text, count), s, c) && (s != shift_ || c != convergence_))
                    {
                        shift_ = s; convergence_ = c;
                        char message[160]; std::snprintf(message, sizeof(message), "live tuning: offset %.6g, convergence %.6g", s, c); log_(message);
                    }
                }
            }
        }
        ControlSlot* selected = nullptr;
        for (auto& slot : controls_)
            if (slot.fence && vkGetFenceStatus_(device_, slot.fence) == VK_SUCCESS) { selected = &slot; break; }
        if (!selected)
        {
            // Do not wait here: app work may wait on a future CPU submission.
            if (controls_.size() >= 64) { Stop("alternate mode stopped: 64 control submissions still pending"); return false; }
            controls_.emplace_back();
            auto& slot = controls_.back();
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (!Command(slot.command)) { Stop("alternate mode stopped: command allocation failed"); return false; }
            if (vkCreateFence_(device_, &fi, nullptr, &slot.fence) != VK_SUCCESS)
            { Stop("alternate mode stopped: control fence allocation failed"); return false; }
            selected = &slot;
        }
        if (!Begin(selected->command)) { Stop("alternate mode stopped: control command reset failed"); return false; }
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier_(selected->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            1, &before, 0, nullptr, 0, nullptr);
        const float eye[2] = {stopped_ ? 0 : (right_ ? -shift_ : shift_), stopped_ ? 0 : (right_ ? -convergence_ : convergence_)};
        vkCmdUpdateBuffer_(selected->command, control_, 0, sizeof(eye), eye);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier_(selected->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0,
            1, &after, 0, nullptr, 0, nullptr);
        if (vkEndCommandBuffer_(selected->command) != VK_SUCCESS || vkResetFences_(device_, 1, &selected->fence) != VK_SUCCESS)
        { Stop("alternate mode stopped: control recording failed"); return false; }
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &selected->command;
        if (vkQueueSubmit_(queue, 1, &si, selected->fence) != VK_SUCCESS)
        { Stop("alternate mode stopped: control submission failed"); return false; }
        return true;
    }
    void ReleaseCapture()
    {
        for (auto semaphore : ready_) vkDestroySemaphore_(device_, semaphore, nullptr);
        ready_.clear(); images_.clear(); leftPixels_.clear(); leftFrame_ = 0;
        if (readback_) vkDestroyBuffer_(device_, readback_, nullptr);
        if (readMemory_) vkFreeMemory_(device_, readMemory_, nullptr);
        readback_ = VK_NULL_HANDLE; readMemory_ = VK_NULL_HANDLE;
    }
    bool Capture(VkQueue queue, const VkPresentInfoKHR& info, VkPresentInfoKHR& replacement)
    {
        const uint32_t image = info.pImageIndices[0];
        if (image >= images_.size() || !ready_[image] || copyPending_)
        { Stop("alternate mode stopped: invalid or unfinished capture"); return false; }
        if (gpu_)
        {
            if (!right_ && !gpu_->Reserve()) return false; // ring full: drop the whole pair
            if (!gpu_->HasPair()) return false;
            const auto command = gpu_->Record(images_[image], extent_, right_ ? 1 : 0);
            if (!command) { Stop("GPU geometry stopped: copy recording failed"); return false; }
            std::vector<VkSemaphore> waits;
            if (info.waitSemaphoreCount) waits.assign(info.pWaitSemaphores, info.pWaitSemaphores + info.waitSemaphoreCount);
            std::vector<uint64_t> waitValues(waits.size(), 0);
            if (!right_ && gpu_->ReleaseValue()) { waits.push_back(gpu_->Released()); waitValues.push_back(gpu_->ReleaseValue()); }
            std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkSemaphore signals[] = {ready_[image], gpu_->Ready()};
            uint64_t signalValues[] = {0, gpu_->Sequence()};
            VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timeline.waitSemaphoreValueCount = uint32_t(waitValues.size()); timeline.pWaitSemaphoreValues = waitValues.data();
            timeline.signalSemaphoreValueCount = right_ ? 2 : 1; timeline.pSignalSemaphoreValues = signalValues;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.pNext = &timeline;
            si.waitSemaphoreCount = uint32_t(waits.size()); si.pWaitSemaphores = waits.data(); si.pWaitDstStageMask = stages.data();
            si.commandBufferCount = 1; si.pCommandBuffers = &command; si.signalSemaphoreCount = right_ ? 2 : 1; si.pSignalSemaphores = signals;
            if (vkQueueSubmit_(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
            { Stop("GPU geometry stopped: copy submission failed"); return false; }
            replacement = info; replacement.waitSemaphoreCount = 1; replacement.pWaitSemaphores = &ready_[image];
            if (!right_) leftFrame_ = frame_ + 1;
            else { gpu_->Publish(leftFrame_, frame_ + 1); gpu_->Drop(); leftFrame_ = 0; }
            return true;
        }
        if (!copyCommand_ && !Command(copyCommand_)) { Stop("alternate mode stopped: readback command allocation failed"); return false; }
        if (!copyFence_)
        {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vkCreateFence_(device_, &fi, nullptr, &copyFence_) != VK_SUCCESS)
            { Stop("alternate mode stopped: readback fence allocation failed"); return false; }
        }
        if (!Begin(copyCommand_)) { Stop("alternate mode stopped: readback command reset failed"); return false; }
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images_[image]; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier_(copyCommand_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer_(copyCommand_, images_[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &copy);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier_(copyCommand_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
            0, nullptr, 0, nullptr, 1, &barrier);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier_(copyCommand_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        if (vkEndCommandBuffer_(copyCommand_) != VK_SUCCESS || vkResetFences_(device_, 1, &copyFence_) != VK_SUCCESS)
        { Stop("alternate mode stopped: readback recording failed"); return false; }
        std::vector<VkPipelineStageFlags> stages(info.waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const size_t bytes = size_t(extent_.width) * extent_.height * 4;
        // All throwing allocations precede the submission that consumes the
        // app's present semaphores. Once submitted, forwarding must use replacement.
        std::vector<uint8_t> pixels(bytes);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = info.waitSemaphoreCount; si.pWaitSemaphores = info.pWaitSemaphores; si.pWaitDstStageMask = stages.data();
        si.commandBufferCount = 1; si.pCommandBuffers = &copyCommand_;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &ready_[image];
        if (vkQueueSubmit_(queue, 1, &si, copyFence_) != VK_SUCCESS)
        { Stop("alternate mode stopped: readback submission failed"); return false; }
        copyPending_ = true;
        replacement = info; replacement.waitSemaphoreCount = 1; replacement.pWaitSemaphores = &ready_[image];
        if (vkWaitForFences_(device_, 1, &copyFence_, VK_TRUE, 5000000000ull) != VK_SUCCESS)
        { Stop("alternate mode stopped: readback timed out or device lost"); return true; }
        copyPending_ = false;
        void* mapped = nullptr;
        if (vkMapMemory_(device_, readMemory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        { Stop("alternate mode stopped: readback mapping failed"); return true; }
        std::memcpy(pixels.data(), mapped, bytes);
        vkUnmapMemory_(device_, readMemory_);
        if (format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB)
            for (size_t at = 0; at < bytes; at += 4) std::swap(pixels[at], pixels[at + 2]);
        if (!right_) { leftPixels_ = std::move(pixels); leftFrame_ = frame_ + 1; }
        else if (leftFrame_ == frame_ && leftPixels_.size() == bytes)
        {
            const size_t row = size_t(extent_.width) * 4;
            for (uint32_t y = 0; y < extent_.height; ++y)
            {
                std::memcpy(pairPixels_.data() + size_t(y) * row * 2, leftPixels_.data() + size_t(y) * row, row);
                std::memcpy(pairPixels_.data() + size_t(y) * row * 2 + row, pixels.data() + size_t(y) * row, row);
            }
            writer_.Publish(extent_.width, extent_.height, pairPixels_, leftFrame_, frame_ + 1);
            leftPixels_.clear(); leftFrame_ = 0;
        }
        return true;
    }
public:
    ~AlternateDevice() { Shutdown(); }
    VkDeviceAddress Address() const { return address_; }
    bool Active() { std::lock_guard<std::mutex> lock(gate_); return !stopped_; }
    bool IsGpu() const { return gpuRequested_; }
    void ShaderPatched() { shiftedShader_ = true; }
    bool Init(VkDevice device, PFN_vkGetDeviceProcAddr next, PFN_vkSetDeviceLoaderData setData,
        const VkPhysicalDeviceMemoryProperties& memory, uint32_t family, float shift,
        const std::wstring& session, std::function<void(const char*)> log, bool gpu = false, LUID adapter = {})
    {
        device_ = device; memory_ = memory; family_ = family; shift_ = shift; log_ = std::move(log); setLoaderData_ = setData;
        gpuRequested_ = gpu; adapter_ = adapter; session_ = session; next_ = next;
        wchar_t tuning[32768]{};
        const DWORD tuningLength = GetEnvironmentVariableW(L"VRX_XG_TUNING", tuning, 32768);
        if (tuningLength && tuningLength < 32768) tuningPath_ = tuning;
#define XG_LOAD(fn) fn##_ = reinterpret_cast<PFN_##fn>(next(device, #fn));
        XG_ALTERNATE_FUNCTIONS(XG_LOAD)
#undef XG_LOAD
        // Submit2 is optional; legacy submission and swapchains are required.
#define XG_REQUIRE(fn) if (!fn##_) return false
        XG_REQUIRE(vkCreateBuffer); XG_REQUIRE(vkDestroyBuffer); XG_REQUIRE(vkGetBufferMemoryRequirements); XG_REQUIRE(vkBindBufferMemory);
        XG_REQUIRE(vkAllocateMemory); XG_REQUIRE(vkFreeMemory); XG_REQUIRE(vkMapMemory); XG_REQUIRE(vkUnmapMemory); XG_REQUIRE(vkGetBufferDeviceAddress);
        XG_REQUIRE(vkCreateCommandPool); XG_REQUIRE(vkDestroyCommandPool); XG_REQUIRE(vkAllocateCommandBuffers); XG_REQUIRE(vkResetCommandBuffer);
        XG_REQUIRE(vkBeginCommandBuffer); XG_REQUIRE(vkEndCommandBuffer); XG_REQUIRE(vkCmdPipelineBarrier); XG_REQUIRE(vkCmdUpdateBuffer); XG_REQUIRE(vkCmdCopyImageToBuffer);
        XG_REQUIRE(vkCreateFence); XG_REQUIRE(vkDestroyFence); XG_REQUIRE(vkGetFenceStatus); XG_REQUIRE(vkResetFences); XG_REQUIRE(vkWaitForFences);
        XG_REQUIRE(vkCreateSemaphore); XG_REQUIRE(vkDestroySemaphore); XG_REQUIRE(vkGetSwapchainImagesKHR); XG_REQUIRE(vkDeviceWaitIdle);
        XG_REQUIRE(vkQueueSubmit); XG_REQUIRE(vkQueuePresentKHR);
#undef XG_REQUIRE
        if (!setLoaderData_ || (!gpuRequested_ && !writer_.Create(session))) return false;
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = family;
        if (vkCreateCommandPool_(device, &pi, nullptr, &pool_) != VK_SUCCESS ||
            !Buffer(8, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, control_, controlMemory_)) return false;
        VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; ai.buffer = control_;
        address_ = vkGetBufferDeviceAddress_(device, &ai);
        return address_ != 0;
    }
    void SetQueue(VkQueue queue, uint32_t family)
    {
        if (queue && family == family_) queue_ = queue;
    }
    VkResult Submit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
    {
        if (queue != queue_) return vkQueueSubmit_(queue, count, submits, fence);
        std::lock_guard<std::mutex> lock(gate_);
        if (count) Control(queue);
        return vkQueueSubmit_(queue, count, submits, fence);
    }
    VkResult Submit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence, bool khr)
    {
        auto next = khr ? vkQueueSubmit2KHR_ : vkQueueSubmit2_;
        if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
        if (queue != queue_) return next(queue, count, submits, fence);
        std::lock_guard<std::mutex> lock(gate_);
        if (count) Control(queue);
        return next(queue, count, submits, fence);
    }
    void Created(VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info)
    {
        std::lock_guard<std::mutex> lock(gate_);
        if (stopped_) return;
        if (swapchain_ && info.oldSwapchain != swapchain_)
        { Stop("alternate mode stopped: multiple live swapchains are unsupported"); return; }
        // Swapchain creation/replacement happens outside in-flight present calls.
        // Present's readback fence alone does not prove the old present consumed
        // its semaphore, so keep semaphores until swapchain destruction below.
        if (swapchain_)
        { Stop("alternate mode stopped: swapchain recreation requires restarting the game"); return; }
        swapchain_ = swapchain; extent_ = info.imageExtent; format_ = info.imageFormat;
        if (!gpuRequested_) pairPixels_.resize(size_t(extent_.width) * extent_.height * 8);
        uint32_t count = 0;
        if (vkGetSwapchainImagesKHR_(device_, swapchain, &count, nullptr) != VK_SUCCESS || !count)
        { Stop("alternate mode stopped: cannot enumerate swapchain images"); return; }
        images_.resize(count);
        if (vkGetSwapchainImagesKHR_(device_, swapchain, &count, images_.data()) != VK_SUCCESS)
        { Stop("alternate mode stopped: cannot read swapchain images"); return; }
        ready_.resize(count, VK_NULL_HANDLE);
        for (auto& semaphore : ready_)
        {
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (vkCreateSemaphore_(device_, &si, nullptr, &semaphore) != VK_SUCCESS)
            { Stop("alternate mode stopped: present semaphore allocation failed"); return; }
        }
        if (gpuRequested_)
        {
            gpu_ = std::make_unique<GpuExporter>();
            if (!gpu_->Init(device_, next_, setLoaderData_, family_, adapter_, extent_.width, extent_.height, session_, GpuFrameKind::Alternating))
                Stop(gpu_->Error());
            else log_("GPU texture sharing ready: three fenced array textures, no CPU pixel readback");
        }
        else if (!Buffer(VkDeviceSize(extent_.width) * extent_.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false, readback_, readMemory_))
            Stop("alternate mode stopped: readback allocation failed");
    }
    void Unsupported(const char* reason) { std::lock_guard<std::mutex> lock(gate_); Stop(reason); }
    void Destroyed(VkSwapchainKHR swapchain)
    {
        std::lock_guard<std::mutex> lock(gate_);
        if (swapchain == swapchain_) { Stop("alternate mode stopped: source swapchain destroyed"); swapchain_ = VK_NULL_HANDLE; }
    }
    VkResult Present(VkQueue queue, const VkPresentInfoKHR* info)
    {
        std::lock_guard<std::mutex> lock(gate_);
        VkPresentInfoKHR replacement{};
        bool replaced = false;
        if (!stopped_)
        {
            if (queue != queue_ || info->swapchainCount != 1 || info->pSwapchains[0] != swapchain_)
                Stop("alternate mode stopped: present queue/swapchain does not match the single-queue source");
            // Engines may present clears/loading screens before creating a
            // vertex shader. Keep the stream empty until a supported shader
            // appears; never publish those initial mono frames as stereo.
            else if (shiftedShader_) replaced = Capture(queue, *info, replacement);
        }
        const VkResult result = vkQueuePresentKHR_(queue, replaced ? &replacement : info);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        {
            if (!stopped_ && shiftedShader_) { ++frame_; right_ = !right_; }
        }
        else Stop("alternate mode stopped: presentation failed; discarding the pending eye pair");
        return result;
    }
    void Shutdown()
    {
        if (!device_) return;
        writer_.Close();
        if (vkDeviceWaitIdle_) vkDeviceWaitIdle_(device_);
        gpu_.reset();
        if (vkDestroySemaphore_) ReleaseCapture();
        if (copyFence_) vkDestroyFence_(device_, copyFence_, nullptr);
        for (const auto& slot : controls_) vkDestroyFence_(device_, slot.fence, nullptr);
        if (pool_) vkDestroyCommandPool_(device_, pool_, nullptr);
        if (control_) vkDestroyBuffer_(device_, control_, nullptr);
        if (controlMemory_) vkFreeMemory_(device_, controlMemory_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
};
} // namespace xg
