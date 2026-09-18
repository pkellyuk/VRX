#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>

// CPU references protect frame identity; fence values protect queued GPU work.
// No waits are performed here: a producer drops a frame when every slot is busy.
template <size_t Count>
class SourceRing
{
public:
    struct Frame
    {
        int index = -1;
        uint64_t seq = 0;
        uint64_t value = 0; // producer completion fence
        double time = 0;
        uint64_t layout = 0; // capture layout generation; changes on resize
    };
    using WriteRef = std::shared_ptr<Frame>;
    using ReadRef = std::shared_ptr<const Frame>;
    // Transfer: a copy-engine read (second-GPU depth hand-over), its own timeline.
    enum class Reader { Graphics, Model, Transfer };

    WriteRef Reserve(uint64_t producerDone, uint64_t graphicsDone, uint64_t modelDone, uint64_t transferDone = 0)
    {
        // D3D reports device removal as UINT64_MAX, not successful completion.
        constexpr auto removed = std::numeric_limits<uint64_t>::max();
        if (producerDone == removed || graphicsDone == removed || modelDone == removed || transferDone == removed) return {};
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t n = 0; n < Count; ++n)
        {
            const size_t i = (next_ + n) % Count;
            Slot& slot = slots_[i];
            if (!slot.frame.expired() || slot.producer > producerDone ||
                slot.graphics > graphicsDone || slot.model > modelDone || slot.transfer > transferDone) continue;
            auto frame = std::make_shared<Frame>();
            frame->index = static_cast<int>(i);
            frame->seq = ++sequence_;
            slot = {};
            slot.frame = frame;
            next_ = (i + 1) % Count;
            return frame;
        }
        return {};
    }

    // The sole producer calls this after submitting the write and its signal.
    // It must not modify the frame after publication.
    void Publish(const WriteRef& frame, uint64_t value, double time)
    {
        assert(frame);
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[frame->index];
        assert(slot.frame.lock() == frame);
        frame->value = value;
        frame->time = time;
        slot.producer = value;
        latest_ = frame;
    }

    ReadRef Latest() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

    // Register queued work BEFORE releasing the CPU reference. The texture stays
    // unavailable until the corresponding GPU fence completes, even if all CPU
    // readers have moved on. Different queues have independent timelines.
    void MarkRead(const ReadRef& frame, Reader reader, uint64_t value)
    {
        assert(frame);
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[frame->index];
        assert(slot.frame.lock() == frame);
        uint64_t& last = reader == Reader::Graphics ? slot.graphics : reader == Reader::Model ? slot.model : slot.transfer;
        if (value > last) last = value;
    }

private:
    struct Slot
    {
        std::weak_ptr<const Frame> frame;
        uint64_t producer = 0, graphics = 0, model = 0, transfer = 0;
    };
    mutable std::mutex mutex_;
    std::array<Slot, Count> slots_{};
    ReadRef latest_;
    size_t next_ = 0;
    uint64_t sequence_ = 0;
};
