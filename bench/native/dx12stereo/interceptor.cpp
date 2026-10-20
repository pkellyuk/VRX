// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Native command interception foundation. Original COM objects and their IIDs
// are preserved; no proxy objects are passed into the graphics driver.
#include "interceptor.h"
#include "dx12_slots.h"
#include "mirror.h"
#include "status.h"
#include <MinHook.h>
#include <wrl/client.h>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <type_traits>
#include <algorithm>

namespace vrx::dx12 {
using Microsoft::WRL::ComPtr;
namespace {
struct Hook { void* detour = nullptr; void* original = nullptr; };
// Hooks stay installed for the lifetime of the side-loaded module. Do not
// unload it while graphics threads can still execute a trampoline.
struct Registry {
    std::recursive_mutex mutex;
    std::unordered_map<void*, Hook> hooks;
    std::atomic<uint64_t> devices{0}, queues{0}, lists{0}, resources{0}, pipelines{0};
    std::atomic<uint64_t> closes{0}, resets{0}, draws{0}, indexed{0}, bundles{0}, submits{0}, failures{0};
    std::atomic<uint64_t> streams{0},memory{0},placed{0},modern{0};
    bool initialized = false;
    bool dxgi = false;
};
Registry& State() { static auto* state = new Registry; return *state; }
thread_local unsigned callDepth = 0;
struct Call {
    bool outer = callDepth++ == 0;
    ~Call() { --callDepth; }
};
void* Entry(void* object, unsigned index) { return (*static_cast<void***>(object))[index]; }
template<class Function> Function Original(void* object, unsigned index) {
    auto& state = State(); std::lock_guard<std::recursive_mutex> lock(state.mutex);
    const auto found = state.hooks.find(Entry(object,index));
    // Every detour is enabled only after its trampoline was registered.
    if(found == state.hooks.end()) std::terminate();
    return reinterpret_cast<Function>(found->second.original);
}
void Add(Registry& state, void* object, unsigned index, void* detour) {
    void* entry = Entry(object,index);
    const auto found = state.hooks.find(entry);
    if(found != state.hooks.end()) {
        if(found->second.detour != detour) {
            // A driver can fold two method implementations to one address.
            // Their signatures may differ: leave that entry in native mode.
            // Keep its trampoline allocated for already-entered calls.
            MH_QueueDisableHook(entry); found->second.detour=nullptr; ++state.failures;
        }
        return;
    }
    void* original = nullptr;
    if(MH_CreateHook(entry,detour,&original) != MH_OK) { ++state.failures; return; }
    state.hooks.emplace(entry,Hook{detour,original});
    if(MH_QueueEnableHook(entry) != MH_OK) ++state.failures;
}
ULONG STDMETHODCALLTYPE Release(void* object) {
    Call call; using Fn=ULONG(STDMETHODCALLTYPE*)(void*);
    auto token=mirror::BeforeRelease(object);
    const auto result=Original<Fn>(object,2)(object);
    if(!result) mirror::Released(object,std::move(token));
    return result;
}
void Lifetime(IUnknown* object) noexcept {
    try {
        ComPtr<IUnknown> identity; object->QueryInterface(IID_PPV_ARGS(&identity));
        auto& state=State(); std::lock_guard<std::recursive_mutex> lock(state.mutex);
        Add(state,object,2,reinterpret_cast<void*>(&Release));
        if(identity) Add(state,identity.Get(),2,reinterpret_cast<void*>(&Release));
        if(MH_ApplyQueued()!=MH_OK) ++state.failures;
    } catch(...) { ++State().failures; }
}
template<class Observe> void ResourceCreated(ID3D12Device*,void* returned,Observe observe) {
    ++State().resources;ComPtr<ID3D12Resource> resource;auto* original=static_cast<IUnknown*>(returned);
    if(SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&resource)))) {
        Lifetime(resource.Get());Lifetime(original);observe(resource.Get());mirror::Alias(resource.Get(),original);
    }
}
template<class Observe> void PipelineCreated(ID3D12Device*,void* returned,Observe observe) {
    ++State().pipelines;ComPtr<ID3D12PipelineState> pipeline;auto* original=static_cast<IUnknown*>(returned);
    if(SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&pipeline)))) {
        Lifetime(pipeline.Get());Lifetime(original);observe(pipeline.Get());mirror::Alias(pipeline.Get(),original);
    }
}
void MemoryCreated(ID3D12Device* device,void* returned,const D3D12_HEAP_DESC* desc) {
    ++State().memory;ComPtr<ID3D12Heap> heap;auto* original=static_cast<IUnknown*>(returned);
    if(SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&heap)))) {
        Lifetime(heap.Get());Lifetime(original);mirror::MemoryHeap(device,heap.Get(),desc);mirror::Alias(heap.Get(),original);
    }
}
HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* object) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
    const auto result = Original<Fn>(object,slot::ID3D12GraphicsCommandList_Close)(object);
    if(call.outer && SUCCEEDED(result)) { ++State().closes; mirror::Close(object); } return result;
}
HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList* object,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
    const auto result = Original<Fn>(object,slot::ID3D12GraphicsCommandList_Reset)(object,allocator,pipeline);
    if(call.outer && SUCCEEDED(result)) { ++State().resets; mirror::Reset(object,allocator,pipeline); } return result;
}
void STDMETHODCALLTYPE Draw(ID3D12GraphicsCommandList* object,UINT vertices,UINT instances,UINT firstVertex,UINT firstInstance) {
    Call call;
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT);
    if(call.outer) ++State().draws;
    Original<Fn>(object,slot::ID3D12GraphicsCommandList_DrawInstanced)(object,vertices,instances,firstVertex,firstInstance);
    if(call.outer) mirror::Record(object,"DrawInstanced",[&](auto* q,auto&){ q->DrawInstanced(vertices,instances,firstVertex,firstInstance); },mirror::RecordKind::Work);
}
void STDMETHODCALLTYPE DrawIndexed(ID3D12GraphicsCommandList* object,UINT indices,UINT instances,UINT firstIndex,INT baseVertex,UINT firstInstance) {
    Call call;
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,INT,UINT);
    if(call.outer) ++State().indexed;
    Original<Fn>(object,slot::ID3D12GraphicsCommandList_DrawIndexedInstanced)(object,indices,instances,firstIndex,baseVertex,firstInstance);
    if(call.outer) mirror::Record(object,"DrawIndexedInstanced",[&](auto* q,auto&){ q->DrawIndexedInstanced(indices,instances,firstIndex,baseVertex,firstInstance); },mirror::RecordKind::Work);
}
void STDMETHODCALLTYPE Bundle(ID3D12GraphicsCommandList* object,ID3D12GraphicsCommandList* bundle) {
    Call call;
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12GraphicsCommandList*);
    if(call.outer) ++State().bundles;
    Original<Fn>(object,slot::ID3D12GraphicsCommandList_ExecuteBundle)(object,bundle);
    if(call.outer) mirror::Record(object,"ExecuteBundle",[&](auto* q,auto& map){ auto right=map.Bundle(bundle); if(map.Valid()) q->ExecuteBundle(right); },mirror::RecordKind::Work);
}
// These calls carry only immutable values or immutable root-signature objects.
#define SIMPLE(Name,Parameters,Native,Replay) \
void STDMETHODCALLTYPE Name Parameters { \
    Call call; Original<decltype(&Name)>(object,slot::ID3D12GraphicsCommandList_##Name) Native; \
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto&){q->Name Replay;}); \
}
SIMPLE(RSSetViewports,(ID3D12GraphicsCommandList* object,UINT count,const D3D12_VIEWPORT* values),(object,count,values),(count,values))
SIMPLE(RSSetScissorRects,(ID3D12GraphicsCommandList* object,UINT count,const D3D12_RECT* values),(object,count,values),(count,values))
SIMPLE(IASetPrimitiveTopology,(ID3D12GraphicsCommandList* object,D3D12_PRIMITIVE_TOPOLOGY value),(object,value),(value))
SIMPLE(OMSetStencilRef,(ID3D12GraphicsCommandList* object,UINT value),(object,value),(value))
SIMPLE(SetGraphicsRootSignature,(ID3D12GraphicsCommandList* object,ID3D12RootSignature* value),(object,value),(value))
SIMPLE(SetGraphicsRoot32BitConstant,(ID3D12GraphicsCommandList* object,UINT index,UINT value,UINT offset),(object,index,value,offset),(index,value,offset))
SIMPLE(SetGraphicsRoot32BitConstants,(ID3D12GraphicsCommandList* object,UINT index,UINT count,const void* data,UINT offset),(object,index,count,data,offset),(index,count,data,offset))
SIMPLE(SetComputeRootSignature,(ID3D12GraphicsCommandList* object,ID3D12RootSignature* value),(object,value),(value))
SIMPLE(SetComputeRoot32BitConstant,(ID3D12GraphicsCommandList* object,UINT index,UINT value,UINT offset),(object,index,value,offset),(index,value,offset))
SIMPLE(SetComputeRoot32BitConstants,(ID3D12GraphicsCommandList* object,UINT index,UINT count,const void* data,UINT offset),(object,index,count,data,offset),(index,count,data,offset))
#undef SIMPLE
// NULL is the documented {1,1,1,1} default. The installed native runtime
// dereferences NULL even without interception (hardware and WARP), so pass its
// equivalent explicit value to both native and replay recording paths.
void STDMETHODCALLTYPE OMSetBlendFactor(ID3D12GraphicsCommandList* object,const FLOAT values[4]) {
    Call call;FLOAT snapshot[]={1,1,1,1};if(values)std::copy_n(values,4,snapshot);
    // Each list consumes its own call synchronously. Snapshot caller storage
    // before either call and leave the native state update last; the game can
    // reuse its input array immediately after this method returns.
    if(call.outer)mirror::Record(object,__func__,[&](auto* q,auto&){q->OMSetBlendFactor(snapshot);});
    Original<decltype(&OMSetBlendFactor)>(object,slot::ID3D12GraphicsCommandList_OMSetBlendFactor)(object,snapshot);
}
void STDMETHODCALLTYPE SetPipelineState(ID3D12GraphicsCommandList* object,ID3D12PipelineState* value) {
    Call call; Original<decltype(&SetPipelineState)>(object,slot::ID3D12GraphicsCommandList_SetPipelineState)(object,value);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){auto right=map.Pipeline(value); if(map.Valid()) q->SetPipelineState(right);});
}
void STDMETHODCALLTYPE ClearState(ID3D12GraphicsCommandList* object,ID3D12PipelineState* value) {
    Call call; Original<decltype(&ClearState)>(object,slot::ID3D12GraphicsCommandList_ClearState)(object,value);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){auto right=map.Pipeline(value); if(map.Valid()) q->ClearState(right);});
}
void STDMETHODCALLTYPE OMSetRenderTargets(ID3D12GraphicsCommandList* object,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* values,BOOL contiguous,const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    Call call; Original<decltype(&OMSetRenderTargets)>(object,slot::ID3D12GraphicsCommandList_OMSetRenderTargets)(object,count,values,contiguous,depth);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){
        if(count>8 || (count && !values)) { mirror::Unsupported(object,"invalid render target range"); return; }
        D3D12_CPU_DESCRIPTOR_HANDLE right[8]{}; for(UINT i=0;i<(contiguous && count?1:count);++i) right[i]=map.Cpu(values[i]);
        const auto rightDepth=depth?map.Cpu(*depth):D3D12_CPU_DESCRIPTOR_HANDLE{};
        if(map.Valid()) q->OMSetRenderTargets(count,count?right:nullptr,contiguous,depth?&rightDepth:nullptr);
    });
}
void STDMETHODCALLTYPE ClearRenderTargetView(ID3D12GraphicsCommandList* object,D3D12_CPU_DESCRIPTOR_HANDLE target,const FLOAT color[4],UINT count,const D3D12_RECT* rects) {
    Call call; Original<decltype(&ClearRenderTargetView)>(object,slot::ID3D12GraphicsCommandList_ClearRenderTargetView)(object,target,color,count,rects);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){auto right=map.Cpu(target); if(map.Valid()) q->ClearRenderTargetView(right,color,count,rects);},mirror::RecordKind::Transfer);
}
void STDMETHODCALLTYPE ResourceBarrier(ID3D12GraphicsCommandList* object,UINT count,const D3D12_RESOURCE_BARRIER* values) {
    Call call; Original<decltype(&ResourceBarrier)>(object,slot::ID3D12GraphicsCommandList_ResourceBarrier)(object,count,values);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){
        std::vector<D3D12_RESOURCE_BARRIER> right(values,values+count);
        for(auto& b:right) {
            if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) b.Transition.pResource=map.Resource(b.Transition.pResource);
            else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV) b.UAV.pResource=map.Resource(b.UAV.pResource);
            else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) { b.Aliasing.pResourceBefore=map.Resource(b.Aliasing.pResourceBefore); b.Aliasing.pResourceAfter=map.Resource(b.Aliasing.pResourceAfter); }
            else { mirror::Unsupported(object,"unknown resource barrier"); return; }
        }
        if(map.Valid()) q->ResourceBarrier(count,right.data());
    },mirror::RecordKind::Transfer);
}
void STDMETHODCALLTYPE IASetIndexBuffer(ID3D12GraphicsCommandList* object,const D3D12_INDEX_BUFFER_VIEW* value) {
    Call call; Original<decltype(&IASetIndexBuffer)>(object,slot::ID3D12GraphicsCommandList_IASetIndexBuffer)(object,value);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){D3D12_INDEX_BUFFER_VIEW right{}; if(value){right=*value;right.BufferLocation=map.Address(value->BufferLocation);} if(map.Valid()) q->IASetIndexBuffer(value?&right:nullptr);});
}
void STDMETHODCALLTYPE IASetVertexBuffers(ID3D12GraphicsCommandList* object,UINT start,UINT count,const D3D12_VERTEX_BUFFER_VIEW* values) {
    Call call; Original<decltype(&IASetVertexBuffers)>(object,slot::ID3D12GraphicsCommandList_IASetVertexBuffers)(object,start,count,values);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){
        if(!values) { q->IASetVertexBuffers(start,count,nullptr); return; }
        std::vector<D3D12_VERTEX_BUFFER_VIEW> right(values,values+count); for(auto& view:right) view.BufferLocation=map.Address(view.BufferLocation);
        if(map.Valid()) q->IASetVertexBuffers(start,count,right.data());
    });
}
void STDMETHODCALLTYPE CopyResource(ID3D12GraphicsCommandList* object,ID3D12Resource* dst,ID3D12Resource* src) {
    Call call; Original<decltype(&CopyResource)>(object,slot::ID3D12GraphicsCommandList_CopyResource)(object,dst,src);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){auto d=map.Resource(dst),s=map.Resource(src);if(map.Valid())q->CopyResource(d,s);},mirror::RecordKind::Transfer);
}
void STDMETHODCALLTYPE CopyBufferRegion(ID3D12GraphicsCommandList* object,ID3D12Resource* dst,UINT64 offset,ID3D12Resource* src,UINT64 source,UINT64 bytes) {
    Call call; Original<decltype(&CopyBufferRegion)>(object,slot::ID3D12GraphicsCommandList_CopyBufferRegion)(object,dst,offset,src,source,bytes);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){auto d=map.Resource(dst),s=map.Resource(src);if(map.Valid())q->CopyBufferRegion(d,offset,s,source,bytes);},mirror::RecordKind::Transfer);
}
void STDMETHODCALLTYPE CopyTextureRegion(ID3D12GraphicsCommandList* object,const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* box) {
    Call call; Original<decltype(&CopyTextureRegion)>(object,slot::ID3D12GraphicsCommandList_CopyTextureRegion)(object,dst,x,y,z,src,box);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){auto d=*dst,s=*src;d.pResource=map.Resource(dst->pResource);s.pResource=map.Resource(src->pResource);if(map.Valid())q->CopyTextureRegion(&d,x,y,z,&s,box);},mirror::RecordKind::Transfer);
}
void STDMETHODCALLTYPE SetDescriptorHeaps(ID3D12GraphicsCommandList* object,UINT count,ID3D12DescriptorHeap* const* heaps) {
    Call call; Original<decltype(&SetDescriptorHeaps)>(object,slot::ID3D12GraphicsCommandList_SetDescriptorHeaps)(object,count,heaps);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){
        if(count>2 || (count && !heaps)) { mirror::Unsupported(object,"invalid shader-visible heap count"); return; }
        ID3D12DescriptorHeap* right[2]{}; for(UINT i=0;i<count;++i) right[i]=map.Heap(heaps[i]);
        if(map.Valid()) q->SetDescriptorHeaps(count,right);
    });
}
void STDMETHODCALLTYPE CopyTiles(ID3D12GraphicsCommandList* object,ID3D12Resource* tiled,const D3D12_TILED_RESOURCE_COORDINATE* start,const D3D12_TILE_REGION_SIZE* size,ID3D12Resource* buffer,UINT64 offset,D3D12_TILE_COPY_FLAGS flags) {
    Call call;Original<decltype(&CopyTiles)>(object,slot::ID3D12GraphicsCommandList_CopyTiles)(object,tiled,start,size,buffer,offset,flags);
    if(call.outer)mirror::Record(object,__func__,[&](auto* q,auto& map){auto r=map.Resource(tiled),b=map.Resource(buffer);if(map.Valid())q->CopyTiles(r,start,size,b,offset,flags);},mirror::RecordKind::Transfer);
}
void STDMETHODCALLTYPE ClearDepthStencilView(ID3D12GraphicsCommandList* object,D3D12_CPU_DESCRIPTOR_HANDLE handle,D3D12_CLEAR_FLAGS flags,FLOAT depth,UINT8 stencil,UINT count,const D3D12_RECT* rects) {
    Call call; Original<decltype(&ClearDepthStencilView)>(object,slot::ID3D12GraphicsCommandList_ClearDepthStencilView)(object,handle,flags,depth,stencil,count,rects);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){auto right=map.Cpu(handle); if(map.Valid()) q->ClearDepthStencilView(right,flags,depth,stencil,count,rects);},mirror::RecordKind::Transfer);
}
void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* object,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    Call call; Original<decltype(&SetGraphicsRootDescriptorTable)>(object,slot::ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable)(object,index,handle);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){auto right=map.Gpu(handle); if(map.Valid()) q->SetGraphicsRootDescriptorTable(index,right);});
}
void STDMETHODCALLTYPE SetComputeRootDescriptorTable(ID3D12GraphicsCommandList* object,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    Call call; Original<decltype(&SetComputeRootDescriptorTable)>(object,slot::ID3D12GraphicsCommandList_SetComputeRootDescriptorTable)(object,index,handle);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){auto right=map.Gpu(handle); if(map.Valid()) q->SetComputeRootDescriptorTable(index,right);});
}
#define ROOT_ADDRESS(Name) \
void STDMETHODCALLTYPE Name(ID3D12GraphicsCommandList* object,UINT index,D3D12_GPU_VIRTUAL_ADDRESS address) { \
    Call call; Original<decltype(&Name)>(object,slot::ID3D12GraphicsCommandList_##Name)(object,index,address); \
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto& map){auto right=map.Address(address);if(map.Valid()) q->Name(index,right);}); \
}
ROOT_ADDRESS(SetGraphicsRootConstantBufferView)
ROOT_ADDRESS(SetGraphicsRootShaderResourceView)
ROOT_ADDRESS(SetGraphicsRootUnorderedAccessView)
ROOT_ADDRESS(SetComputeRootConstantBufferView)
ROOT_ADDRESS(SetComputeRootShaderResourceView)
ROOT_ADDRESS(SetComputeRootUnorderedAccessView)
#undef ROOT_ADDRESS
void STDMETHODCALLTYPE SetPredication(ID3D12GraphicsCommandList* object,ID3D12Resource* resource,UINT64 offset,D3D12_PREDICATION_OP operation) {
    Call call; Original<decltype(&SetPredication)>(object,slot::ID3D12GraphicsCommandList_SetPredication)(object,resource,offset,operation);
    if(call.outer)mirror::Record(object,__func__,[&](auto* q,auto& map){auto r=map.Resource(resource);if(map.Valid())q->SetPredication(r,offset,operation);});
}
#define QUERY(Name) \
void STDMETHODCALLTYPE Name(ID3D12GraphicsCommandList* object,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE type,UINT index){ \
    Call call;Original<decltype(&Name)>(object,slot::ID3D12GraphicsCommandList_##Name)(object,heap,type,index); \
    if(call.outer)mirror::Record(object,__func__,[&](auto* q,auto& map){auto h=map.Query(heap);if(map.Valid())q->Name(h,type,index);},mirror::RecordKind::Transfer); \
}
QUERY(BeginQuery)
QUERY(EndQuery)
#undef QUERY
void STDMETHODCALLTYPE ResolveQueryData(ID3D12GraphicsCommandList* object,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE type,UINT first,UINT count,ID3D12Resource* buffer,UINT64 offset){
    Call call;Original<decltype(&ResolveQueryData)>(object,slot::ID3D12GraphicsCommandList_ResolveQueryData)(object,heap,type,first,count,buffer,offset);
    if(call.outer)mirror::Record(object,__func__,[&](auto* q,auto& map){auto h=map.Query(heap);auto r=map.Resource(buffer);if(map.Valid())q->ResolveQueryData(h,type,first,count,r,offset);},mirror::RecordKind::Transfer);
}
// Forward the native call exactly, but never submit a second eye whose command
// stream silently omits state, clears, queries or GPU-generated arguments.
#define GUARDED(Name,Parameters,Arguments) \
void STDMETHODCALLTYPE Name Parameters { \
    Call call;Original<decltype(&Name)>(object,slot::ID3D12GraphicsCommandList_##Name) Arguments; \
    if(call.outer)mirror::Unsupported(object,#Name " is not mirrored yet"); \
}
GUARDED(SOSetTargets,(ID3D12GraphicsCommandList* object,UINT start,UINT count,const D3D12_STREAM_OUTPUT_BUFFER_VIEW* views),(object,start,count,views))
#undef GUARDED
void STDMETHODCALLTYPE ExecuteIndirect(ID3D12GraphicsCommandList* object,ID3D12CommandSignature* signature,UINT count,ID3D12Resource* buffer,UINT64 offset,ID3D12Resource* counter,UINT64 counterOffset){
    Call call;Original<decltype(&ExecuteIndirect)>(object,slot::ID3D12GraphicsCommandList_ExecuteIndirect)(object,signature,count,buffer,offset,counter,counterOffset);
    if(call.outer)mirror::Record(object,__func__,[&](auto* q,auto& map){auto s=map.Signature(signature);auto b=map.Resource(buffer),c=map.Resource(counter);if(map.Valid())q->ExecuteIndirect(s,count,b,offset,c,counterOffset);},mirror::RecordKind::Work);
}
#define UAV_CLEAR(Name,T) \
void STDMETHODCALLTYPE Name(ID3D12GraphicsCommandList* object,D3D12_GPU_DESCRIPTOR_HANDLE gpu,D3D12_CPU_DESCRIPTOR_HANDLE cpu,ID3D12Resource* resource,const T values[4],UINT count,const D3D12_RECT* rects){ \
    Call call;Original<decltype(&Name)>(object,slot::ID3D12GraphicsCommandList_##Name)(object,gpu,cpu,resource,values,count,rects); \
    if(call.outer)mirror::Record(object,__func__,[&](auto* q,auto& map){auto g=map.Gpu(gpu);auto c=map.Cpu(cpu);auto r=map.Resource(resource);if(map.Valid())q->Name(g,c,r,values,count,rects);},mirror::RecordKind::Transfer); \
}
UAV_CLEAR(ClearUnorderedAccessViewUint,UINT)
UAV_CLEAR(ClearUnorderedAccessViewFloat,FLOAT)
#undef UAV_CLEAR
void STDMETHODCALLTYPE ResolveSubresource(ID3D12GraphicsCommandList* object,ID3D12Resource* dst,UINT target,ID3D12Resource* src,UINT source,DXGI_FORMAT format){
    Call call;Original<decltype(&ResolveSubresource)>(object,slot::ID3D12GraphicsCommandList_ResolveSubresource)(object,dst,target,src,source,format);
    if(call.outer)mirror::Record(object,__func__,[&](auto* q,auto& map){auto d=map.Resource(dst),s=map.Resource(src);if(map.Valid())q->ResolveSubresource(d,target,s,source,format);},mirror::RecordKind::Transfer);
}
void STDMETHODCALLTYPE DiscardResource(ID3D12GraphicsCommandList* object,ID3D12Resource* resource,const D3D12_DISCARD_REGION* region){
    Call call;Original<decltype(&DiscardResource)>(object,slot::ID3D12GraphicsCommandList_DiscardResource)(object,resource,region);
    if(call.outer)mirror::Record(object,__func__,[&](auto* q,auto& map){auto r=map.Resource(resource);if(map.Valid())q->DiscardResource(r,region);},mirror::RecordKind::Transfer);
}
void STDMETHODCALLTYPE Dispatch(ID3D12GraphicsCommandList* object,UINT x,UINT y,UINT z) {
    Call call; Original<decltype(&Dispatch)>(object,slot::ID3D12GraphicsCommandList_Dispatch)(object,x,y,z);
    if(call.outer) mirror::Record(object,__func__,[&](auto* q,auto&){q->Dispatch(x,y,z);},mirror::RecordKind::Work);
}
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* object,UINT count,ID3D12CommandList* const* lists);
// Preserve the exact SDK calling convention and parameter types. Querying an
// extended interface alone is harmless; using an untranslated recording call
// stops stereo while the original command continues to execute normally.
const char* ExtendedReason(unsigned index) {
    switch(index) {
#define EXT_INTERFACE_BEGIN(Interface)
#define EXT_METHOD(Interface,Name) case slot::Interface##_##Name: return #Name " is not mirrored yet";
#define EXT_INTERFACE_END()
#include "dx12_extended_guards.inc"
#undef EXT_INTERFACE_BEGIN
#undef EXT_METHOD
#undef EXT_INTERFACE_END
    default:return "untranslated extended command-list call";
    }
}
template<unsigned Slot,class Signature> struct ExtendedGuard;
template<unsigned Slot,class Interface,class Result,class... Args>
struct ExtendedGuard<Slot,Result(STDMETHODCALLTYPE Interface::*)(Args...)> {
    static_assert(std::is_same_v<Result,void>,"extended recording return type needs explicit handling");
    static void STDMETHODCALLTYPE Invoke(Interface* object,Args... args) {
        Call call;using Fn=void(STDMETHODCALLTYPE*)(Interface*,Args...);
        Original<Fn>(object,Slot)(object,args...);
        if(call.outer)mirror::Unsupported(object,ExtendedReason(Slot));
    }
};
template<class Interface,class Replay>
void ExtendedRecord(ID3D12GraphicsCommandList* object,const char* name,const Replay& replay) {
    mirror::Record(object,name,[&](auto* list,auto& map){
        ComPtr<Interface> extended;
        if(FAILED(list->QueryInterface(IID_PPV_ARGS(&extended))))mirror::Unsupported(object,"shadow command-list interface is unavailable");
        else replay(extended.Get(),map);
    });
}
template<>struct ExtendedGuard<slot::ID3D12GraphicsCommandList1_OMSetDepthBounds,decltype(&ID3D12GraphicsCommandList1::OMSetDepthBounds)> {
    static void STDMETHODCALLTYPE Invoke(ID3D12GraphicsCommandList1* object,FLOAT minimum,FLOAT maximum) {
        Call call;Original<decltype(&Invoke)>(object,slot::ID3D12GraphicsCommandList1_OMSetDepthBounds)(object,minimum,maximum);
        if(call.outer)ExtendedRecord<ID3D12GraphicsCommandList1>(object,"OMSetDepthBounds",[&](auto* list,auto&){list->OMSetDepthBounds(minimum,maximum);});
    }
};
template<>struct ExtendedGuard<slot::ID3D12GraphicsCommandList5_RSSetShadingRate,decltype(&ID3D12GraphicsCommandList5::RSSetShadingRate)> {
    static void STDMETHODCALLTYPE Invoke(ID3D12GraphicsCommandList5* object,D3D12_SHADING_RATE rate,const D3D12_SHADING_RATE_COMBINER* combiners) {
        Call call;Original<decltype(&Invoke)>(object,slot::ID3D12GraphicsCommandList5_RSSetShadingRate)(object,rate,combiners);
        if(call.outer)ExtendedRecord<ID3D12GraphicsCommandList5>(object,"RSSetShadingRate",[&](auto* list,auto&){list->RSSetShadingRate(rate,combiners);});
    }
};
template<>struct ExtendedGuard<slot::ID3D12GraphicsCommandList5_RSSetShadingRateImage,decltype(&ID3D12GraphicsCommandList5::RSSetShadingRateImage)> {
    static void STDMETHODCALLTYPE Invoke(ID3D12GraphicsCommandList5* object,ID3D12Resource* image) {
        Call call;Original<decltype(&Invoke)>(object,slot::ID3D12GraphicsCommandList5_RSSetShadingRateImage)(object,image);
        if(call.outer)ExtendedRecord<ID3D12GraphicsCommandList5>(object,"RSSetShadingRate",[&](auto* list,auto& map){auto right=map.Resource(image);if(map.Valid())list->RSSetShadingRateImage(right);});
    }
};
void List(ID3D12GraphicsCommandList* object) noexcept {
    try {
    auto& state = State(); std::lock_guard<std::recursive_mutex> lock(state.mutex);
    Add(state,object,slot::ID3D12GraphicsCommandList_Close,reinterpret_cast<void*>(&Close));
    Add(state,object,slot::ID3D12GraphicsCommandList_Reset,reinterpret_cast<void*>(&Reset));
    Add(state,object,slot::ID3D12GraphicsCommandList_DrawInstanced,reinterpret_cast<void*>(&Draw));
    Add(state,object,slot::ID3D12GraphicsCommandList_DrawIndexedInstanced,reinterpret_cast<void*>(&DrawIndexed));
    Add(state,object,slot::ID3D12GraphicsCommandList_ExecuteBundle,reinterpret_cast<void*>(&Bundle));
#define RECORD_HOOK(Name) Add(state,object,slot::ID3D12GraphicsCommandList_##Name,reinterpret_cast<void*>(&Name));
    RECORD_HOOK(RSSetViewports) RECORD_HOOK(RSSetScissorRects) RECORD_HOOK(IASetPrimitiveTopology)
    RECORD_HOOK(SetGraphicsRootSignature) RECORD_HOOK(SetGraphicsRoot32BitConstant) RECORD_HOOK(SetGraphicsRoot32BitConstants)
    RECORD_HOOK(SetPipelineState) RECORD_HOOK(ClearState) RECORD_HOOK(OMSetRenderTargets) RECORD_HOOK(ClearRenderTargetView)
    RECORD_HOOK(ResourceBarrier) RECORD_HOOK(IASetIndexBuffer) RECORD_HOOK(IASetVertexBuffers)
    RECORD_HOOK(CopyResource) RECORD_HOOK(CopyBufferRegion) RECORD_HOOK(CopyTextureRegion)
    RECORD_HOOK(SetDescriptorHeaps) RECORD_HOOK(Dispatch)
    RECORD_HOOK(ClearDepthStencilView) RECORD_HOOK(SetGraphicsRootDescriptorTable)
    RECORD_HOOK(SetGraphicsRootConstantBufferView) RECORD_HOOK(SetGraphicsRootShaderResourceView) RECORD_HOOK(SetGraphicsRootUnorderedAccessView)
    RECORD_HOOK(SetPredication) RECORD_HOOK(CopyTiles)
    RECORD_HOOK(ResolveSubresource) RECORD_HOOK(OMSetBlendFactor) RECORD_HOOK(OMSetStencilRef) RECORD_HOOK(SOSetTargets)
    RECORD_HOOK(ClearUnorderedAccessViewUint) RECORD_HOOK(ClearUnorderedAccessViewFloat) RECORD_HOOK(DiscardResource)
    RECORD_HOOK(BeginQuery) RECORD_HOOK(EndQuery) RECORD_HOOK(ResolveQueryData) RECORD_HOOK(ExecuteIndirect)
    RECORD_HOOK(SetComputeRootSignature) RECORD_HOOK(SetComputeRoot32BitConstant) RECORD_HOOK(SetComputeRoot32BitConstants)
    RECORD_HOOK(SetComputeRootDescriptorTable) RECORD_HOOK(SetComputeRootConstantBufferView)
    RECORD_HOOK(SetComputeRootShaderResourceView) RECORD_HOOK(SetComputeRootUnorderedAccessView)
#undef RECORD_HOOK
#define EXT_INTERFACE_BEGIN(Interface) { ComPtr<Interface> extended; if(SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&extended)))) { Lifetime(extended.Get()); mirror::Alias(object,extended.Get());
#define EXT_METHOD(Interface,Name) { using Guard=ExtendedGuard<slot::Interface##_##Name,decltype(&Interface::Name)>; Add(state,extended.Get(),slot::Interface##_##Name,reinterpret_cast<void*>(&Guard::Invoke)); }
#define EXT_INTERFACE_END() } }
#include "dx12_extended_guards.inc"
#undef EXT_INTERFACE_BEGIN
#undef EXT_METHOD
#undef EXT_INTERFACE_END
    if(MH_ApplyQueued() != MH_OK) ++state.failures;
    ++state.lists;
    } catch(...) { ++State().failures; }
}
void STDMETHODCALLTYPE UpdateTileMappings(ID3D12CommandQueue* object,ID3D12Resource* resource,UINT regions,const D3D12_TILED_RESOURCE_COORDINATE* starts,const D3D12_TILE_REGION_SIZE* sizes,ID3D12Heap* heap,UINT ranges,const D3D12_TILE_RANGE_FLAGS* flags,const UINT* offsets,const UINT* counts,D3D12_TILE_MAPPING_FLAGS mapping) {
    Call call;Original<decltype(&UpdateTileMappings)>(object,slot::ID3D12CommandQueue_UpdateTileMappings)(object,resource,regions,starts,sizes,heap,ranges,flags,offsets,counts,mapping);
    if(call.outer)mirror::QueueCommand(object,[&](auto* q,auto& map){auto r=map.Resource(resource);auto h=map.Memory(heap);if(map.Valid())q->UpdateTileMappings(r,regions,starts,sizes,h,ranges,flags,offsets,counts,mapping);});
}
void STDMETHODCALLTYPE CopyTileMappings(ID3D12CommandQueue* object,ID3D12Resource* dst,const D3D12_TILED_RESOURCE_COORDINATE* to,ID3D12Resource* src,const D3D12_TILED_RESOURCE_COORDINATE* from,const D3D12_TILE_REGION_SIZE* size,D3D12_TILE_MAPPING_FLAGS flags) {
    Call call;Original<decltype(&CopyTileMappings)>(object,slot::ID3D12CommandQueue_CopyTileMappings)(object,dst,to,src,from,size,flags);
    if(call.outer)mirror::QueueCommand(object,[&](auto* q,auto& map){auto d=map.Resource(dst),s=map.Resource(src);if(map.Valid())q->CopyTileMappings(d,to,s,from,size,flags);});
}
void Queue(ID3D12CommandQueue* queue) noexcept {
    try {
        auto& state = State(); std::lock_guard<std::recursive_mutex> lock(state.mutex);
        Add(state,queue,slot::ID3D12CommandQueue_ExecuteCommandLists,reinterpret_cast<void*>(&Execute));
        Add(state,queue,slot::ID3D12CommandQueue_UpdateTileMappings,reinterpret_cast<void*>(&UpdateTileMappings));
        Add(state,queue,slot::ID3D12CommandQueue_CopyTileMappings,reinterpret_cast<void*>(&CopyTileMappings));
        if(MH_ApplyQueued() != MH_OK) ++state.failures;
        ++state.queues;
    } catch(...) { ++State().failures; }
}
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* object,UINT count,ID3D12CommandList* const* lists) {
    Call call;
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
    if(call.outer) ++State().submits;
    const auto execute=Original<Fn>(object,slot::ID3D12CommandQueue_ExecuteCommandLists);
    if(call.outer && State().failures.load())mirror::Unsupported(object,"native command interception is incomplete");
    if(call.outer){mirror::Execute(object,count,lists,execute);Report();}else execute(object,count,lists);
}
HRESULT STDMETHODCALLTYPE CreateQueue(ID3D12Device* object,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID iid,void** output) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,void**);
    const auto result = Original<Fn>(object,slot::ID3D12Device_CreateCommandQueue)(object,desc,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        ComPtr<ID3D12CommandQueue> queue;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&queue)))) { Queue(queue.Get()); Lifetime(queue.Get()); mirror::Queue(object,queue.Get()); }
    }
    return result;
}
HRESULT STDMETHODCALLTYPE CreateList(ID3D12Device* object,UINT node,D3D12_COMMAND_LIST_TYPE type,ID3D12CommandAllocator* allocator,
    ID3D12PipelineState* pipeline,REFIID iid,void** output) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_COMMAND_LIST_TYPE,ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**);
    const auto result = Original<Fn>(object,slot::ID3D12Device_CreateCommandList)(object,node,type,allocator,pipeline,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        ComPtr<ID3D12GraphicsCommandList> list;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&list)))) { mirror::List(object,list.Get(),allocator,pipeline,node,type,false); List(list.Get()); Lifetime(list.Get()); mirror::Alias(list.Get(),static_cast<IUnknown*>(*output)); }
    }
    return result;
}
HRESULT STDMETHODCALLTYPE CreateList1(ID3D12Device4* object,UINT node,D3D12_COMMAND_LIST_TYPE type,D3D12_COMMAND_LIST_FLAGS flags,REFIID iid,void** output) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*,UINT,D3D12_COMMAND_LIST_TYPE,D3D12_COMMAND_LIST_FLAGS,REFIID,void**);
    const auto result = Original<Fn>(object,slot::ID3D12Device4_CreateCommandList1)(object,node,type,flags,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        ComPtr<ID3D12GraphicsCommandList> list;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&list)))) {
            if(flags!=D3D12_COMMAND_LIST_FLAG_NONE) mirror::Unsupported(object,"command list flags are unsupported");
            else mirror::List(object,list.Get(),nullptr,nullptr,node,type,true);
            List(list.Get()); Lifetime(list.Get()); mirror::Alias(list.Get(),static_cast<IUnknown*>(*output));
        }
    }
    return result;
}
HRESULT STDMETHODCALLTYPE CreatePipeline(ID3D12Device* object,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,REFIID iid,void** output) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,REFIID,void**);
    const auto result = Original<Fn>(object,slot::ID3D12Device_CreateGraphicsPipelineState)(object,desc,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        PipelineCreated(object,*output,[&](auto* pipeline){mirror::Pipeline(object,pipeline,desc);});
    } return result;
}
HRESULT STDMETHODCALLTYPE CreateResource(ID3D12Device* object,const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** output) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    const auto result = Original<Fn>(object,slot::ID3D12Device_CreateCommittedResource)(object,heap,flags,desc,initial,clear,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        ResourceCreated(object,*output,[&](auto* resource){mirror::Resource(object,resource,heap,flags,desc,initial,clear);});
    } return result;
}
HRESULT STDMETHODCALLTYPE CreateStream(ID3D12Device2* object,const D3D12_PIPELINE_STATE_STREAM_DESC* desc,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreateStream)>(object,slot::ID3D12Device2_CreatePipelineState)(object,desc,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output){++State().streams;PipelineCreated(object,*output,[&](auto* pipeline){mirror::StreamPipeline(object,pipeline,desc);});}
    return result;
}
HRESULT STDMETHODCALLTYPE CreateMemory(ID3D12Device* object,const D3D12_HEAP_DESC* desc,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreateMemory)>(object,slot::ID3D12Device_CreateHeap)(object,desc,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output)MemoryCreated(object,*output,desc);return result;
}
HRESULT STDMETHODCALLTYPE CreatePlaced(ID3D12Device* object,ID3D12Heap* heap,UINT64 offset,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreatePlaced)>(object,slot::ID3D12Device_CreatePlacedResource)(object,heap,offset,desc,initial,clear,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output){++State().placed;ResourceCreated(object,*output,[&](auto* resource){mirror::PlacedResource(object,resource,heap,offset,desc,initial,clear);});}return result;
}
HRESULT STDMETHODCALLTYPE CreateMemory1(ID3D12Device4* object,const D3D12_HEAP_DESC* desc,ID3D12ProtectedResourceSession* protectedSession,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreateMemory1)>(object,slot::ID3D12Device4_CreateHeap1)(object,desc,protectedSession,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output){if(protectedSession)mirror::Unsupported(object,"protected memory heaps are unsupported");MemoryCreated(object,*output,desc);}return result;
}
HRESULT STDMETHODCALLTYPE CreateResource1(ID3D12Device4* object,const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,ID3D12ProtectedResourceSession* protectedSession,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreateResource1)>(object,slot::ID3D12Device4_CreateCommittedResource1)(object,heap,flags,desc,initial,clear,protectedSession,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output){++State().modern;if(protectedSession)mirror::Unsupported(object,"protected resources are unsupported");ResourceCreated(object,*output,[&](auto* resource){mirror::Resource(object,resource,heap,flags,desc,initial,clear);});}return result;
}
D3D12_RESOURCE_DESC Legacy(const D3D12_RESOURCE_DESC1& desc) {
    return {desc.Dimension,desc.Alignment,desc.Width,desc.Height,desc.DepthOrArraySize,desc.MipLevels,desc.Format,desc.SampleDesc,desc.Layout,desc.Flags};
}
bool Feedback(const D3D12_RESOURCE_DESC1& desc){return desc.SamplerFeedbackMipRegion.Width || desc.SamplerFeedbackMipRegion.Height || desc.SamplerFeedbackMipRegion.Depth;}
HRESULT STDMETHODCALLTYPE CreateResource2(ID3D12Device8* object,const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC1* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,ID3D12ProtectedResourceSession* protectedSession,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreateResource2)>(object,slot::ID3D12Device8_CreateCommittedResource2)(object,heap,flags,desc,initial,clear,protectedSession,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output){
        ++State().modern;if(protectedSession || Feedback(*desc))mirror::Unsupported(object,"protected resources or sampler feedback require additional handling");
        const auto base=Legacy(*desc);ResourceCreated(object,*output,[&](auto* resource){mirror::Resource(object,resource,heap,flags,&base,initial,clear);});
    }return result;
}
HRESULT STDMETHODCALLTYPE CreatePlaced1(ID3D12Device8* object,ID3D12Heap* heap,UINT64 offset,const D3D12_RESOURCE_DESC1* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreatePlaced1)>(object,slot::ID3D12Device8_CreatePlacedResource1)(object,heap,offset,desc,initial,clear,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output){
        ++State().placed;++State().modern;if(Feedback(*desc))mirror::Unsupported(object,"sampler feedback resources require additional handling");
        const auto base=Legacy(*desc);ResourceCreated(object,*output,[&](auto* resource){mirror::PlacedResource(object,resource,heap,offset,&base,initial,clear);});
    }return result;
}
HRESULT STDMETHODCALLTYPE CreateReserved(ID3D12Device* object,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreateReserved)>(object,slot::ID3D12Device_CreateReservedResource)(object,desc,initial,clear,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output)ResourceCreated(object,*output,[&](auto* resource){mirror::ReservedResource(object,resource,desc,initial,clear);});return result;
}
HRESULT STDMETHODCALLTYPE CreateReserved1(ID3D12Device4* object,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,ID3D12ProtectedResourceSession* protectedSession,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreateReserved1)>(object,slot::ID3D12Device4_CreateReservedResource1)(object,desc,initial,clear,protectedSession,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output){
        ++State().modern;if(protectedSession)mirror::Unsupported(object,"protected reserved resources cannot be mirrored");
        ResourceCreated(object,*output,[&](auto* resource){mirror::ReservedResource(object,resource,desc,initial,clear);});
    }return result;
}
HRESULT STDMETHODCALLTYPE AllocatorReset(ID3D12CommandAllocator* object) {
    Call call; auto result=Original<decltype(&AllocatorReset)>(object,slot::ID3D12CommandAllocator_Reset)(object);
    if(call.outer && SUCCEEDED(result)) mirror::AllocatorReset(object); return result;
}
HRESULT STDMETHODCALLTYPE CreateAllocator(ID3D12Device* object,D3D12_COMMAND_LIST_TYPE type,REFIID iid,void** output) {
    Call call; auto result=Original<decltype(&CreateAllocator)>(object,slot::ID3D12Device_CreateCommandAllocator)(object,type,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        ComPtr<ID3D12CommandAllocator> allocator;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&allocator)))) {
            { auto& s=State(); std::lock_guard<std::recursive_mutex> lock(s.mutex); Add(s,allocator.Get(),slot::ID3D12CommandAllocator_Reset,reinterpret_cast<void*>(&AllocatorReset)); if(MH_ApplyQueued()!=MH_OK) ++s.failures; }
            Lifetime(allocator.Get()); mirror::Allocator(object,allocator.Get(),type);
        }
    } return result;
}
HRESULT STDMETHODCALLTYPE CreateCommandSignature(ID3D12Device* object,const D3D12_COMMAND_SIGNATURE_DESC* desc,ID3D12RootSignature* root,REFIID iid,void** output){
    Call call;const auto result=Original<decltype(&CreateCommandSignature)>(object,slot::ID3D12Device_CreateCommandSignature)(object,desc,root,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output){auto* original=static_cast<IUnknown*>(*output);ComPtr<ID3D12CommandSignature> signature;
        if(SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&signature)))){Lifetime(signature.Get());Lifetime(original);mirror::CommandSignature(object,signature.Get(),desc);mirror::Alias(signature.Get(),original);}
    }return result;
}
HRESULT STDMETHODCALLTYPE CreateQueryHeap(ID3D12Device* object,const D3D12_QUERY_HEAP_DESC* desc,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreateQueryHeap)>(object,slot::ID3D12Device_CreateQueryHeap)(object,desc,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output){
        auto* original=static_cast<IUnknown*>(*output);ComPtr<ID3D12QueryHeap> queries;
        if(SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&queries)))){Lifetime(queries.Get());Lifetime(original);mirror::QueryHeap(object,queries.Get(),desc);mirror::Alias(queries.Get(),original);}
    }return result;
}
HRESULT STDMETHODCALLTYPE CreateHeap(ID3D12Device* object,const D3D12_DESCRIPTOR_HEAP_DESC* desc,REFIID iid,void** output) {
    Call call; auto result=Original<decltype(&CreateHeap)>(object,slot::ID3D12Device_CreateDescriptorHeap)(object,desc,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        ComPtr<ID3D12DescriptorHeap> heap;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&heap)))) { Lifetime(heap.Get()); mirror::Heap(object,heap.Get(),desc); }
    } return result;
}
void STDMETHODCALLTYPE CreateRtv(ID3D12Device* object,ID3D12Resource* resource,const D3D12_RENDER_TARGET_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    Call call; Original<decltype(&CreateRtv)>(object,slot::ID3D12Device_CreateRenderTargetView)(object,resource,desc,handle);
    if(call.outer) mirror::Rtv(object,resource,desc,handle);
}
void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Device* object,ID3D12Resource* resource,const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    Call call; Original<decltype(&CreateDepthStencilView)>(object,slot::ID3D12Device_CreateDepthStencilView)(object,resource,desc,handle);
    if(call.outer) mirror::Observe(object,[&](auto* d,auto& map){auto r=map.Resource(resource);auto h=map.Cpu(handle);if(map.Valid()) d->CreateDepthStencilView(r,desc,h);});
}
HRESULT STDMETHODCALLTYPE CreateComputePipelineState(ID3D12Device* object,const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&CreateComputePipelineState)>(object,slot::ID3D12Device_CreateComputePipelineState)(object,desc,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        PipelineCreated(object,*output,[&](auto* pipeline){mirror::ComputePipeline(object,pipeline);});
    } return result;
}
void STDMETHODCALLTYPE CreateConstantBufferView(ID3D12Device* object,const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    Call call; Original<decltype(&CreateConstantBufferView)>(object,slot::ID3D12Device_CreateConstantBufferView)(object,desc,handle);
    if(call.outer) mirror::Observe(object,[&](auto* d,auto& map){
        D3D12_CONSTANT_BUFFER_VIEW_DESC right{}; if(desc){right=*desc;right.BufferLocation=map.Address(desc->BufferLocation);}
        auto h=map.Cpu(handle);if(map.Valid())d->CreateConstantBufferView(desc?&right:nullptr,h);
    });
}
void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Device* object,ID3D12Resource* resource,const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    Call call; Original<decltype(&CreateShaderResourceView)>(object,slot::ID3D12Device_CreateShaderResourceView)(object,resource,desc,handle);
    if(call.outer) mirror::Observe(object,[&](auto* d,auto& map){
        if(desc && desc->ViewDimension==D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE) {mirror::Unsupported(object,"raytracing SRV is unsupported");return;}
        auto r=map.Resource(resource);auto h=map.Cpu(handle);if(map.Valid())d->CreateShaderResourceView(r,desc,h);
    });
}
void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Device* object,ID3D12Resource* resource,ID3D12Resource* counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    Call call; Original<decltype(&CreateUnorderedAccessView)>(object,slot::ID3D12Device_CreateUnorderedAccessView)(object,resource,counter,desc,handle);
    if(call.outer) mirror::Observe(object,[&](auto* d,auto& map){auto r=map.Resource(resource),c=map.Resource(counter);auto h=map.Cpu(handle);if(map.Valid())d->CreateUnorderedAccessView(r,c,desc,h);});
}
void STDMETHODCALLTYPE CreateSampler(ID3D12Device* object,const D3D12_SAMPLER_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    Call call; Original<decltype(&CreateSampler)>(object,slot::ID3D12Device_CreateSampler)(object,desc,handle);
    if(call.outer) mirror::Observe(object,[&](auto* d,auto& map){auto h=map.Cpu(handle);if(map.Valid())d->CreateSampler(desc,h);});
}
void STDMETHODCALLTYPE CopyDescriptorsSimple(ID3D12Device* object,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE dst,D3D12_CPU_DESCRIPTOR_HANDLE src,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    Call call; Original<decltype(&CopyDescriptorsSimple)>(object,slot::ID3D12Device_CopyDescriptorsSimple)(object,count,dst,src,type);
    if(call.outer && count) mirror::Observe(object,[&](auto* d,auto& map){
        auto target=map.Cpu(dst),source=map.Cpu(src); const auto stride=d->GetDescriptorHandleIncrementSize(type);
        map.Cpu({dst.ptr+SIZE_T(count-1)*stride});map.Cpu({src.ptr+SIZE_T(count-1)*stride});
        if(map.Valid())d->CopyDescriptorsSimple(count,target,source,type);
    });
}
void STDMETHODCALLTYPE CopyDescriptors(ID3D12Device* object,UINT dstCount,const D3D12_CPU_DESCRIPTOR_HANDLE* dst,const UINT* dstSizes,UINT srcCount,const D3D12_CPU_DESCRIPTOR_HANDLE* src,const UINT* srcSizes,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    Call call; Original<decltype(&CopyDescriptors)>(object,slot::ID3D12Device_CopyDescriptors)(object,dstCount,dst,dstSizes,srcCount,src,srcSizes,type);
    if(call.outer) mirror::Observe(object,[&](auto* d,auto& map){
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> target(dstCount),source(srcCount); const auto stride=d->GetDescriptorHandleIncrementSize(type);
        const auto ranges=[&](UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* values,const UINT* sizes,auto& right){
            for(UINT i=0;i<count;++i) {const auto size=sizes?sizes[i]:1; if(!size){right[i]=values[i];continue;}right[i]=map.Cpu(values[i]);map.Cpu({values[i].ptr+SIZE_T(size-1)*stride});}
        };
        ranges(dstCount,dst,dstSizes,target);ranges(srcCount,src,srcSizes,source);
        if(map.Valid())d->CopyDescriptors(dstCount,target.data(),dstSizes,srcCount,source.data(),srcSizes,type);
    });
}
HRESULT STDMETHODCALLTYPE SetColorSpace1(IDXGISwapChain3* object,DXGI_COLOR_SPACE_TYPE color) {
    Call call;const auto result=Original<decltype(&SetColorSpace1)>(object,slot::IDXGISwapChain3_SetColorSpace1)(object,color);
    if(call.outer && SUCCEEDED(result))mirror::ColorSpace(object,color);return result;
}
HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* object,UINT sync,UINT flags) {
    Call call;if(call.outer){if(State().failures.load())mirror::Unsupported(object,"native command interception is incomplete");mirror::Present(object,flags);}
    return Original<decltype(&Present)>(object,slot::IDXGISwapChain_Present)(object,sync,flags);
}
HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* object,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters) {
    Call call;if(call.outer){if(State().failures.load())mirror::Unsupported(object,"native command interception is incomplete");mirror::Present(object,flags);}
    return Original<decltype(&Present1)>(object,slot::IDXGISwapChain1_Present1)(object,sync,flags,parameters);
}
HRESULT STDMETHODCALLTYPE GetBuffer(IDXGISwapChain* object,UINT index,REFIID iid,void** output) {
    Call call;const auto result=Original<decltype(&GetBuffer)>(object,slot::IDXGISwapChain_GetBuffer)(object,index,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        ComPtr<ID3D12Resource> resource;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&resource)))) {Lifetime(resource.Get());mirror::BackBuffer(object,resource.Get());}
    }return result;
}
HRESULT STDMETHODCALLTYPE ResizeBuffers(IDXGISwapChain* object,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags) {
    Call call;if(call.outer)mirror::Resize(object);
    return Original<decltype(&ResizeBuffers)>(object,slot::IDXGISwapChain_ResizeBuffers)(object,count,width,height,format,flags);
}
HRESULT STDMETHODCALLTYPE ResizeBuffers1(IDXGISwapChain3* object,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags,const UINT* nodes,IUnknown* const* queues) {
    Call call;if(call.outer)mirror::Resize(object,count,nodes,queues);
    return Original<decltype(&ResizeBuffers1)>(object,slot::IDXGISwapChain3_ResizeBuffers1)(object,count,width,height,format,flags,nodes,queues);
}
void Chain(IUnknown* device,IDXGISwapChain* chain) noexcept {
    try {
        if(!chain || !device)return;
        ComPtr<ID3D12CommandQueue> queue;
        if(FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || !mirror::Swapchain(queue.Get(),chain))return;
        Lifetime(chain);ComPtr<IDXGISwapChain3> newer;chain->QueryInterface(IID_PPV_ARGS(&newer));if(newer)Lifetime(newer.Get());
        auto& state=State();std::lock_guard<std::recursive_mutex> lock(state.mutex);
        Add(state,chain,slot::IDXGISwapChain_Present,reinterpret_cast<void*>(&Present));
        Add(state,chain,slot::IDXGISwapChain_GetBuffer,reinterpret_cast<void*>(&GetBuffer));
        Add(state,chain,slot::IDXGISwapChain_ResizeBuffers,reinterpret_cast<void*>(&ResizeBuffers));
        if(newer) {
            Add(state,newer.Get(),slot::IDXGISwapChain1_Present1,reinterpret_cast<void*>(&Present1));
            Add(state,newer.Get(),slot::IDXGISwapChain3_ResizeBuffers1,reinterpret_cast<void*>(&ResizeBuffers1));
            Add(state,newer.Get(),slot::IDXGISwapChain3_SetColorSpace1,reinterpret_cast<void*>(&SetColorSpace1));
        }
        if(MH_ApplyQueued()!=MH_OK)++state.failures;
    } catch(...) {++State().failures;mirror::Unsupported(chain,"swapchain hook installation failed");}
}
HRESULT STDMETHODCALLTYPE CreateSwapChain(IDXGIFactory* object,IUnknown* device,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** output) {
    Call call;const auto result=Original<decltype(&CreateSwapChain)>(object,slot::IDXGIFactory_CreateSwapChain)(object,device,desc,output);
    if(call.outer && SUCCEEDED(result) && output)Chain(device,*output);return result;
}
HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IDXGIFactory2* object,IUnknown* device,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* restrictOutput,IDXGISwapChain1** output) {
    Call call;const auto result=Original<decltype(&CreateSwapChainForHwnd)>(object,slot::IDXGIFactory2_CreateSwapChainForHwnd)(object,device,hwnd,desc,fullscreen,restrictOutput,output);
    if(call.outer && SUCCEEDED(result) && output)Chain(device,*output);return result;
}
HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IDXGIFactory2* object,IUnknown* device,IUnknown* window,const DXGI_SWAP_CHAIN_DESC1* desc,IDXGIOutput* restrictOutput,IDXGISwapChain1** output) {
    Call call;const auto result=Original<decltype(&CreateSwapChainForCoreWindow)>(object,slot::IDXGIFactory2_CreateSwapChainForCoreWindow)(object,device,window,desc,restrictOutput,output);
    if(call.outer && SUCCEEDED(result) && output)Chain(device,*output);return result;
}
HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IDXGIFactory2* object,IUnknown* device,const DXGI_SWAP_CHAIN_DESC1* desc,IDXGIOutput* restrictOutput,IDXGISwapChain1** output) {
    Call call;const auto result=Original<decltype(&CreateSwapChainForComposition)>(object,slot::IDXGIFactory2_CreateSwapChainForComposition)(object,device,desc,restrictOutput,output);
    if(call.outer && SUCCEEDED(result) && output)Chain(device,*output);return result;
}
void PresentationHooks() {
    Call internal;
    auto& state=State();std::lock_guard<std::recursive_mutex> lock(state.mutex);if(state.dxgi)return;
    // Factory instances may predate device creation. Hook the native method
    // entrypoints shared by those instances before the app creates its chain.
    wchar_t path[32768]{};const auto length=GetSystemDirectoryW(path,32768);
    if(!length || length>=32750) {++state.failures;return;}
    constexpr wchar_t suffix[]=L"\\dxgi.dll";std::memcpy(path+length,suffix,sizeof(suffix));
    static HMODULE module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<HRESULT(WINAPI*)(REFIID,void**)>(module?GetProcAddress(module,"CreateDXGIFactory1"):nullptr);
    ComPtr<IDXGIFactory2> factory;
    if(!create || FAILED(create(IID_PPV_ARGS(&factory)))) {++state.failures;return;}
    Add(state,factory.Get(),slot::IDXGIFactory_CreateSwapChain,reinterpret_cast<void*>(&CreateSwapChain));
#define FACTORY_HOOK(Name) Add(state,factory.Get(),slot::IDXGIFactory2_##Name,reinterpret_cast<void*>(&Name));
    FACTORY_HOOK(CreateSwapChainForHwnd) FACTORY_HOOK(CreateSwapChainForCoreWindow) FACTORY_HOOK(CreateSwapChainForComposition)
#undef FACTORY_HOOK
    if(MH_ApplyQueued()!=MH_OK)++state.failures;else state.dxgi=true;
}
} // namespace
void Attach(ID3D12Device* device) noexcept {
    try {
        auto& state = State(); std::unique_lock<std::recursive_mutex> lock(state.mutex);
        if(!state.initialized) {
            if(MH_Initialize() != MH_OK) { ++state.failures; return; }
            state.initialized = true;
        }
        Add(state,device,slot::ID3D12Device_CreateCommandQueue,reinterpret_cast<void*>(&CreateQueue));
        Add(state,device,slot::ID3D12Device_CreateCommandAllocator,reinterpret_cast<void*>(&CreateAllocator));
        Add(state,device,slot::ID3D12Device_CreateDescriptorHeap,reinterpret_cast<void*>(&CreateHeap));
        Add(state,device,slot::ID3D12Device_CreateRenderTargetView,reinterpret_cast<void*>(&CreateRtv));
#define DEVICE_HOOK(Name) Add(state,device,slot::ID3D12Device_##Name,reinterpret_cast<void*>(&Name));
        DEVICE_HOOK(CreateDepthStencilView) DEVICE_HOOK(CreateConstantBufferView) DEVICE_HOOK(CreateShaderResourceView)
        DEVICE_HOOK(CreateUnorderedAccessView) DEVICE_HOOK(CreateSampler) DEVICE_HOOK(CopyDescriptors) DEVICE_HOOK(CopyDescriptorsSimple)
        DEVICE_HOOK(CreateComputePipelineState)
        DEVICE_HOOK(CreateQueryHeap)
        DEVICE_HOOK(CreateCommandSignature)
#undef DEVICE_HOOK
        Add(state,device,slot::ID3D12Device_CreateCommandList,reinterpret_cast<void*>(&CreateList));
        Add(state,device,slot::ID3D12Device_CreateGraphicsPipelineState,reinterpret_cast<void*>(&CreatePipeline));
        Add(state,device,slot::ID3D12Device_CreateCommittedResource,reinterpret_cast<void*>(&CreateResource));
        Add(state,device,slot::ID3D12Device_CreateHeap,reinterpret_cast<void*>(&CreateMemory));
        Add(state,device,slot::ID3D12Device_CreatePlacedResource,reinterpret_cast<void*>(&CreatePlaced));
        Add(state,device,slot::ID3D12Device_CreateReservedResource,reinterpret_cast<void*>(&CreateReserved));
        ComPtr<ID3D12Device2> streams;ComPtr<ID3D12Device4> newer;ComPtr<ID3D12Device8> newest;
        if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&streams))))Add(state,streams.Get(),slot::ID3D12Device2_CreatePipelineState,reinterpret_cast<void*>(&CreateStream));
        if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&newer)))) {
            Add(state,newer.Get(),slot::ID3D12Device4_CreateCommandList1,reinterpret_cast<void*>(&CreateList1));
            Add(state,newer.Get(),slot::ID3D12Device4_CreateCommittedResource1,reinterpret_cast<void*>(&CreateResource1));
            Add(state,newer.Get(),slot::ID3D12Device4_CreateHeap1,reinterpret_cast<void*>(&CreateMemory1));
            Add(state,newer.Get(),slot::ID3D12Device4_CreateReservedResource1,reinterpret_cast<void*>(&CreateReserved1));
        }
        if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&newest)))) {
            Add(state,newest.Get(),slot::ID3D12Device8_CreateCommittedResource2,reinterpret_cast<void*>(&CreateResource2));
            Add(state,newest.Get(),slot::ID3D12Device8_CreatePlacedResource1,reinterpret_cast<void*>(&CreatePlaced1));
        }
        if(MH_ApplyQueued() != MH_OK) ++state.failures;
        ++state.devices;
        lock.unlock();Lifetime(device);mirror::Device(device);
        for(auto* alias:{static_cast<IUnknown*>(streams.Get()),static_cast<IUnknown*>(newer.Get()),static_cast<IUnknown*>(newest.Get())})if(alias){Lifetime(alias);mirror::Alias(device,alias);}
        PresentationHooks();Report(true);
    } catch(...) { ++State().failures; }
}
void Snapshot(Counts& output) noexcept {
    auto& s = State(); output = {};
    output.devices=s.devices; output.queues=s.queues; output.commandLists=s.lists; output.resources=s.resources; output.pipelines=s.pipelines;
    output.closes=s.closes; output.resets=s.resets; output.draws=s.draws; output.indexedDraws=s.indexed; output.bundles=s.bundles;
    output.submissions=s.submits; output.hookFailures=s.failures;
    mirror::Counts replay; mirror::Snapshot(replay);
    output.mirrorReplays=replay.replays; output.mirrorStops=replay.stops; output.mirrorResources=replay.resources; output.mirrorPipelines=replay.pipelines; output.mirrorLiveObjects=replay.liveObjects;
    output.swapchains=replay.swapchains;output.presentations=replay.presentations;output.publications=replay.publications;output.presentDrops=replay.presentDrops;output.resizes=replay.resizes;
    output.streamPipelines=s.streams;output.memoryHeaps=s.memory;output.placedResources=s.placed;output.modernResources=s.modern;std::memcpy(output.reason,replay.reason,sizeof(output.reason));
}
void DiagnosticScope(bool enter) noexcept { if(enter) ++callDepth; else if(callDepth) --callDepth; }
bool ApiOuter() noexcept {return callDepth==0;}
void Report(bool force) noexcept {
    static std::atomic<uint64_t> updated{0};thread_local bool reporting=false;if(reporting)return;
    struct Guard {bool& flag;explicit Guard(bool& value):flag(value){flag=true;}~Guard(){flag=false;}} guard(reporting);
    try {
        static auto* writer=[]()->StatusWriter* {
            wchar_t session[66]{};const auto length=GetEnvironmentVariableW(L"VRX_XG_SESSION",session,66);if(!length || length>=66)return nullptr;
            wchar_t mode[3]{};const bool mirroring=GetEnvironmentVariableW(L"VRX_DX12_MIRROR",mode,3)==1 && mode[0]==L'1';
            auto value=std::make_unique<StatusWriter>();if(!value->Create(session,mirroring))return nullptr;return value.release();
        }();
        if(!writer)return;const auto now=GetTickCount64();auto previous=updated.load();
        if(!force && (now-previous<100 || !updated.compare_exchange_strong(previous,now)))return;
        if(force)updated.store(now);Counts snapshot;Snapshot(snapshot);writer->Publish(snapshot);
    }catch(...){} // Optional diagnostics must never prevent native rendering.
}
} // namespace vrx::dx12
