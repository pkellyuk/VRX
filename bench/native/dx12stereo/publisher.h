// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Native D3D12 same-frame output. No Vulkan translation or CPU pixel transfer.
#pragma once
#include "../xgstereo/gpu_transport.h"
#include <memory>
#include <mutex>

namespace xg::dx12 {
enum class PublishResult { Published, Dropped, Invalid, Failed };
struct Eye {
    ID3D12Resource* texture = nullptr;
    UINT subresource = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
};
// Submit after both eyes' rendering has been enqueued on this DIRECT queue.
// The caller must serialize other submissions to that queue with this call,
// and supply accurate subresource states. States are restored before returning
// control to subsequent GPU commands. CPU return does not imply GPU completion.
// Different queues must establish their own rendering->copy fence dependency.
class Publisher {
    struct Slot {
        ComPtr<ID3D12Resource> texture;
        ComPtr<ID3D12Fence> ready, released;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        // D3D12 command lists do not retain resources used by recorded commands.
        ComPtr<ID3D12Resource> left, right;
        uint64_t pending = 0;
    };
    std::unique_ptr<GpuWriter> writer_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    std::array<Slot, GpuSlots> slots_;
    uint32_t width_ = 0, height_ = 0;
    uint64_t lastFrame_ = 0;
    bool failed_ = false;
    std::mutex mutex_;

    bool Matches(const Eye& eye) const {
        if (!eye.texture) return false;
        const auto d = eye.texture->GetDesc();
        // Typed RGBA8 only in this first producer. HDR, typeless, MSAA and mip
        // conversion need explicit resolve/conversion rather than unsafe copies.
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != width_ || d.Height != height_ ||
            d.Format != DXGI_FORMAT_R8G8B8A8_UNORM || d.SampleDesc.Count != 1 || d.MipLevels != 1 ||
            eye.subresource >= d.DepthOrArraySize) return false;
        ComPtr<ID3D12Device> owner;
        ComPtr<IUnknown> a, b;
        return SUCCEEDED(eye.texture->GetDevice(IID_PPV_ARGS(&owner))) &&
            SUCCEEDED(owner.As(&a)) && SUCCEEDED(device_.As(&b)) && a.Get() == b.Get();
    }
    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* texture, UINT subresource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {texture, subresource, before, after}; return b;
    }
public:
    Publisher() = default;
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    ~Publisher() {
        // Retire command allocators and resource references only after GPU use.
        // A lost device reports UINT64_MAX and requires no further fence wait.
        // A caller must let the queue run; never destroy while it waits on a
        // fence that only this thread could signal.
        if (writer_) writer_->Close();
        for (auto& s : slots_) if (s.pending && s.ready && s.ready->GetCompletedValue() < s.pending) {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (event) {
                if (SUCCEEDED(s.ready->SetEventOnCompletion(s.pending, event))) WaitForSingleObject(event, INFINITE);
                CloseHandle(event);
            }
        }
    }
    bool Create(ID3D12CommandQueue* queue, const std::wstring& session, uint32_t width, uint32_t height) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (writer_ || !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || !GpuShape(width, height)) return false;
        ComPtr<ID3D12Device> device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device->GetNodeCount() != 1) return false;
        auto writer = std::make_unique<GpuWriter>();
        if (!writer->Create(session, device->GetAdapterLuid(), width, height, GpuFrameKind::SameFrame)) return false;
        // GpuWriter owns shared allocations on its own D3D12 device. Open them
        // on the game's device before recording the native game's queue work.
        std::array<Slot, GpuSlots> slots;
        for (uint32_t i = 0; i < GpuSlots; ++i) {
            auto& s = slots[i];
            if (FAILED(device->OpenSharedHandle(writer->Handle(i,0), IID_PPV_ARGS(&s.texture))) ||
                FAILED(device->OpenSharedHandle(writer->Handle(i,1), IID_PPV_ARGS(&s.ready))) ||
                FAILED(device->OpenSharedHandle(writer->Handle(i,2), IID_PPV_ARGS(&s.released))) ||
                FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr, IID_PPV_ARGS(&s.commands))) ||
                FAILED(s.commands->Close())) return false;
        }
        writer_ = std::move(writer); slots_ = std::move(slots); device_ = std::move(device); queue_ = queue; width_ = width; height_ = height; return true;
    }
    PublishResult Submit(uint64_t frame, const Eye& left, const Eye& right) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!writer_ || failed_) return PublishResult::Failed;
        if (!frame || frame <= lastFrame_ || !Matches(left) || !Matches(right) ||
            (left.texture == right.texture && left.subresource == right.subresource)) return PublishResult::Invalid;
        uint64_t sequence = 0, release = 0;
        const int index = writer_->Reserve(sequence, release);
        if (index < 0) return PublishResult::Dropped;
        auto& s = slots_[index];
        auto fail = [&] { failed_ = true; writer_->Close(); return PublishResult::Failed; };
        if (FAILED(s.allocator->Reset()) || FAILED(s.commands->Reset(s.allocator.Get(), nullptr))) return fail();
        s.left = left.texture; s.right = right.texture;
        auto target = Transition(s.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        s.commands->ResourceBarrier(1, &target);
        const Eye eyes[] = {left, right};
        for (UINT i = 0; i < 2; ++i) {
            const auto& eye = eyes[i];
            auto b = Transition(eye.texture, eye.subresource, eye.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (eye.state != D3D12_RESOURCE_STATE_COPY_SOURCE) s.commands->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = eye.texture; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = eye.subresource;
            dst.pResource = s.texture.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = i;
            s.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            if (eye.state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter); s.commands->ResourceBarrier(1, &b);
            }
        }
        std::swap(target.Transition.StateBefore, target.Transition.StateAfter); s.commands->ResourceBarrier(1, &target);
        if (FAILED(s.commands->Close()) || FAILED(queue_->Wait(s.released.Get(), release))) return fail();
        ID3D12CommandList* lists[] = {s.commands.Get()}; queue_->ExecuteCommandLists(1, lists);
        if (FAILED(queue_->Signal(s.ready.Get(), sequence))) {
            // Device failure: no successful signal is available to retire this
            // list. Keep its references alive until destruction/device removal.
            return fail();
        }
        s.pending = sequence; lastFrame_ = frame;
        const auto result = writer_->TryPublish(uint32_t(index), sequence, frame, frame);
        if (result == GpuPublish::Invalid) return fail();
        return result == GpuPublish::Published ? PublishResult::Published : PublishResult::Dropped;
    }
};
} // namespace xg::dx12
