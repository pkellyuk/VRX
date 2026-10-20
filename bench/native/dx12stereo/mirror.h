// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <memory>
#include <cstdint>
namespace vrx::dx12::mirror {
struct Context;
// A synchronous, non-owning callback: lambdas live through the API call and
// are never stored. Avoid std::function allocation at every intercepted draw.
template<class... Args> class Callback {
    const void* object_;
    void (*invoke_)(const void*,Args...);
public:
    template<class Function> Callback(const Function& function) noexcept
        : object_(std::addressof(function)), invoke_([](const void* object,Args... args) {
            (*static_cast<const Function*>(object))(args...);
        }) {}
    void operator()(Args... args) const { invoke_(object_,args...); }
};
class Mapper {
    std::shared_ptr<Context> context_;
public:
    explicit Mapper(std::shared_ptr<Context> context):context_(std::move(context)) {}
    ID3D12Resource* Resource(ID3D12Resource* original);
    ID3D12PipelineState* Pipeline(ID3D12PipelineState* original);
    ID3D12GraphicsCommandList* Bundle(ID3D12GraphicsCommandList* original);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(D3D12_CPU_DESCRIPTOR_HANDLE original);
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(D3D12_GPU_DESCRIPTOR_HANDLE original);
    ID3D12DescriptorHeap* Heap(ID3D12DescriptorHeap* original);
    ID3D12Heap* Memory(ID3D12Heap* original);
    D3D12_GPU_VIRTUAL_ADDRESS Address(D3D12_GPU_VIRTUAL_ADDRESS original);
    bool Valid() const;
};
struct Counts { uint64_t replays=0,stops=0,resources=0,pipelines=0,liveObjects=0,swapchains=0,presentations=0,publications=0,presentDrops=0,resizes=0;char reason[256]{}; };
void Snapshot(Counts& output) noexcept;
void Unsupported(void*,const char*) noexcept;
// Observers run only at the outer API boundary, with internal calls suppressed.
void Device(ID3D12Device*) noexcept;
void Alias(IUnknown*,IUnknown*) noexcept;
void Queue(ID3D12Device*,ID3D12CommandQueue*) noexcept;
bool Swapchain(ID3D12CommandQueue*,IDXGISwapChain*) noexcept;
void BackBuffer(IDXGISwapChain*,ID3D12Resource*) noexcept;
void Present(IDXGISwapChain*,UINT flags) noexcept;
void Resize(IDXGISwapChain*,UINT count=0,const UINT* nodes=nullptr,IUnknown* const* queues=nullptr) noexcept;
void Allocator(ID3D12Device*,ID3D12CommandAllocator*,D3D12_COMMAND_LIST_TYPE) noexcept;
void AllocatorReset(ID3D12CommandAllocator*) noexcept;
void Resource(ID3D12Device*,ID3D12Resource*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*) noexcept;
void MemoryHeap(ID3D12Device*,ID3D12Heap*,const D3D12_HEAP_DESC*) noexcept;
void PlacedResource(ID3D12Device*,ID3D12Resource*,ID3D12Heap*,UINT64,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*) noexcept;
void ReservedResource(ID3D12Device*,ID3D12Resource*,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*) noexcept;
void Pipeline(ID3D12Device*,ID3D12PipelineState*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC*) noexcept;
void StreamPipeline(ID3D12Device*,ID3D12PipelineState*,const D3D12_PIPELINE_STATE_STREAM_DESC*) noexcept;
void ComputePipeline(ID3D12Device*,ID3D12PipelineState*) noexcept;
void Heap(ID3D12Device*,ID3D12DescriptorHeap*,const D3D12_DESCRIPTOR_HEAP_DESC*) noexcept;
void Rtv(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
void List(ID3D12Device*,ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*,UINT,D3D12_COMMAND_LIST_TYPE,bool closed) noexcept;
void Reset(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*) noexcept;
void Close(ID3D12GraphicsCommandList*) noexcept;
void Record(ID3D12GraphicsCommandList*,Callback<ID3D12GraphicsCommandList*,Mapper&>) noexcept;
void Observe(ID3D12Device*,Callback<ID3D12Device*,Mapper&>) noexcept;
void QueueCommand(ID3D12CommandQueue*,Callback<ID3D12CommandQueue*,Mapper&>) noexcept;
// Executes originals and replays in one batch. Caller supplies the trampoline.
using ExecuteFn=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
void Execute(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*,ExecuteFn) noexcept;
// Capture identity before native Release can free/reuse an address.
std::shared_ptr<void> BeforeRelease(void*) noexcept;
void Released(void*,std::shared_ptr<void>) noexcept;
HRESULT Twin(ID3D12Resource*,ID3D12Resource**) noexcept; // diagnostic only
} // namespace vrx::dx12::mirror
