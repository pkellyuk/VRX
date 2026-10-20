// Cooperative same-frame renderer contract. The renderer supplies two isolated
// eye draw lists made from one immutable simulation snapshot. This never replays
// an arbitrary game's full submit stream (uploads/queries/history are not safe
// to duplicate). Integration into an unaware game needs an engine/DXVK adapter.
#pragma once
#include "gpu_export.h"

namespace xg {
class SameFrameProducer {
    GpuExporter exporter_;
    VkQueue queue_ = VK_NULL_HANDLE;
    PFN_vkQueueSubmit submit_ = nullptr;
    VkExtent2D extent_{};
    uint64_t previousFrame_ = 0;
public:
    bool Init(VkDevice device, PFN_vkGetDeviceProcAddr next, VkQueue queue, uint32_t family,
        LUID adapter, VkExtent2D extent, const std::wstring& session)
    {
        queue_ = queue; extent_ = extent;
        submit_ = reinterpret_cast<PFN_vkQueueSubmit>(next(device, "vkQueueSubmit"));
        return submit_ && exporter_.Init(device, next, nullptr, family, adapter, extent.width, extent.height, session, GpuFrameKind::SameFrame);
    }
    // Both lists must use the same immutable inputs and have independent
    // temporal histories. The caller owns their lifetime until GPU completion.
    // Scene copies/clears are permitted only when the renderer recorded them
    // explicitly for that eye. A failed reservation drops the complete frame.
    bool Submit(uint64_t simulationFrame, VkCommandBuffer left, VkImage leftImage, VkCommandBuffer right, VkImage rightImage,
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        if (!simulationFrame || simulationFrame <= previousFrame_ || !left || !right || left == right || !leftImage || !rightImage || leftImage == rightImage) return false;
        previousFrame_ = simulationFrame;
        if (!exporter_.Reserve()) return false;
        const auto copyLeft = exporter_.Record(leftImage, extent_, 0, finalLayout);
        const auto copyRight = exporter_.Record(rightImage, extent_, 1, finalLayout);
        if (!copyLeft || !copyRight) { exporter_.Drop(); return false; }
        VkCommandBuffer commands[] = {left, copyLeft, right, copyRight};
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const auto released = exporter_.Released(), ready = exporter_.Ready();
        const uint64_t wait = exporter_.ReleaseValue(), signal = exporter_.Sequence();
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = wait ? 1 : 0; timeline.pWaitSemaphoreValues = &wait;
        timeline.signalSemaphoreValueCount = 1; timeline.pSignalSemaphoreValues = &signal;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext = &timeline;
        submit.waitSemaphoreCount = wait ? 1 : 0; submit.pWaitSemaphores = &released; submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 4; submit.pCommandBuffers = commands; submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &ready;
        if (submit_(queue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) { exporter_.Drop(); return false; }
        // Equal frame IDs are published only after both explicit eye lists
        // have been scheduled in one GPU submission from this snapshot.
        const bool published = exporter_.Publish(simulationFrame, simulationFrame);
        exporter_.Drop(); return published;
    }
};
} // namespace xg
