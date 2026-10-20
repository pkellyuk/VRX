// Test-only readback. Production transport never maps image pixels.
#pragma once
#include "gpu_transport.h"
#include <d3d12sdklayers.h>
#include <stdexcept>

namespace xg::gpu_test {
inline void Hr(HRESULT h) { if (FAILED(h)) throw std::runtime_error("D3D12 GPU transport test failed: " + std::to_string(uint32_t(h))); }
struct Snapshot {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> sbs, readback;
    ComPtr<ID3D12InfoQueue> messages;
    GpuReader reader;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t value = 0, sequence = 0;
    void Init(LUID luid, uint32_t width, uint32_t height, const std::wstring& session)
    {
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); Hr(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)));
        Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        Hr(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
        Hr(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&commands))); Hr(commands->Close());
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = width * 2; d.Height = height;
        d.DepthOrArraySize = d.MipLevels = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sbs)));
        UINT64 bytes = 0; device->GetCopyableFootprints(&d, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
        if (!reader.Open(device.Get(), session)) throw std::runtime_error("open shared GPU pair reader");
    }
    bool Read(PairFrame& out)
    {
        GpuSlotInfo frame;
        const auto status = reader.Status(sequence, frame);
        if (status != PairRead::NewPair) return false;
        Hr(allocator->Reset()); Hr(commands->Reset(allocator.Get(), nullptr));
        if (!reader.Record(queue.Get(), commands.Get(), sbs.Get(), sequence, frame)) return false;
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {sbs.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
        commands->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = sbs.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        commands->ResourceBarrier(1, &b); Hr(commands->Close());
        ID3D12CommandList* lists[] = {commands.Get()}; queue->ExecuteCommandLists(1, lists);
        if (!reader.Submitted(queue.Get())) throw std::runtime_error("signal shared release fence");
        Hr(queue->Signal(fence.Get(), ++value));
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!event) throw std::runtime_error("create test fence event");
        Hr(fence->SetEventOnCompletion(value, event)); const auto waited = WaitForSingleObject(event, 5000); CloseHandle(event);
        if (waited != WAIT_OBJECT_0) throw std::runtime_error("test copy did not finish");
        const auto& h = reader.Header();
        out.header = {PairMagic, PairVersion, PairHeaderBytes, h.width, h.height, h.width * 8, h.width * h.height * 8, 1,
            frame.sequence, frame.leftFrame, frame.rightFrame, frame.publishedMs, h.pid, 0};
        out.rgba.resize(size_t(h.width) * h.height * 8);
        const uint8_t* pixels = nullptr; Hr(readback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels))));
        for (uint32_t y = 0; y < h.height; ++y) std::memcpy(out.rgba.data() + size_t(y) * h.width * 8,
            pixels + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, h.width * 8);
        readback->Unmap(0, nullptr); sequence = frame.sequence;
        if (messages)
        {
            const auto count = messages->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T bytes = 0; messages->GetMessage(i, nullptr, &bytes); std::vector<uint8_t> data(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data()); Hr(messages->GetMessage(i, message, &bytes));
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
            }
            messages->ClearStoredMessages();
        }
        return true;
    }
};
} // namespace xg::gpu_test
