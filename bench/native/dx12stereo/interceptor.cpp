// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Native command interception foundation. Original COM objects and their IIDs
// are preserved; no proxy objects are passed into the graphics driver.
#include "interceptor.h"
#include "dx12_slots.h"
#include <MinHook.h>
#include <wrl/client.h>
#include <mutex>
#include <unordered_map>
#include <atomic>

namespace vrx::dx12 {
using Microsoft::WRL::ComPtr;
namespace {
struct Hook { void* detour = nullptr; void* original = nullptr; };
// Hooks stay installed for the lifetime of the side-loaded module. Do not
// unload it while graphics threads can still execute a trampoline.
struct Registry {
    std::mutex mutex;
    std::unordered_map<void*, Hook> hooks;
    std::atomic<uint64_t> devices{0}, queues{0}, lists{0}, resources{0}, pipelines{0};
    std::atomic<uint64_t> closes{0}, resets{0}, draws{0}, indexed{0}, bundles{0}, submits{0}, failures{0};
    bool initialized = false;
};
Registry& State() { static auto* state = new Registry; return *state; }
thread_local unsigned callDepth = 0;
struct Call {
    bool outer = callDepth++ == 0;
    ~Call() { --callDepth; }
};
void* Entry(void* object, unsigned index) { return (*static_cast<void***>(object))[index]; }
template<class Function> Function Original(void* object, unsigned index) {
    auto& state = State(); std::lock_guard<std::mutex> lock(state.mutex);
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
HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* object) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
    const auto result = Original<Fn>(object,slot::ID3D12GraphicsCommandList_Close)(object);
    if(call.outer && SUCCEEDED(result)) ++State().closes; return result;
}
HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList* object,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
    const auto result = Original<Fn>(object,slot::ID3D12GraphicsCommandList_Reset)(object,allocator,pipeline);
    if(call.outer && SUCCEEDED(result)) ++State().resets; return result;
}
void STDMETHODCALLTYPE Draw(ID3D12GraphicsCommandList* object,UINT vertices,UINT instances,UINT firstVertex,UINT firstInstance) {
    Call call;
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT);
    if(call.outer) ++State().draws;
    Original<Fn>(object,slot::ID3D12GraphicsCommandList_DrawInstanced)(object,vertices,instances,firstVertex,firstInstance);
}
void STDMETHODCALLTYPE DrawIndexed(ID3D12GraphicsCommandList* object,UINT indices,UINT instances,UINT firstIndex,INT baseVertex,UINT firstInstance) {
    Call call;
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,INT,UINT);
    if(call.outer) ++State().indexed;
    Original<Fn>(object,slot::ID3D12GraphicsCommandList_DrawIndexedInstanced)(object,indices,instances,firstIndex,baseVertex,firstInstance);
}
void STDMETHODCALLTYPE Bundle(ID3D12GraphicsCommandList* object,ID3D12GraphicsCommandList* bundle) {
    Call call;
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12GraphicsCommandList*);
    if(call.outer) ++State().bundles;
    Original<Fn>(object,slot::ID3D12GraphicsCommandList_ExecuteBundle)(object,bundle);
}
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* object,UINT count,ID3D12CommandList* const* lists);
void List(ID3D12GraphicsCommandList* object) noexcept {
    try {
    auto& state = State(); std::lock_guard<std::mutex> lock(state.mutex);
    Add(state,object,slot::ID3D12GraphicsCommandList_Close,reinterpret_cast<void*>(&Close));
    Add(state,object,slot::ID3D12GraphicsCommandList_Reset,reinterpret_cast<void*>(&Reset));
    Add(state,object,slot::ID3D12GraphicsCommandList_DrawInstanced,reinterpret_cast<void*>(&Draw));
    Add(state,object,slot::ID3D12GraphicsCommandList_DrawIndexedInstanced,reinterpret_cast<void*>(&DrawIndexed));
    Add(state,object,slot::ID3D12GraphicsCommandList_ExecuteBundle,reinterpret_cast<void*>(&Bundle));
    if(MH_ApplyQueued() != MH_OK) ++state.failures;
    ++state.lists;
    } catch(...) { ++State().failures; }
}
void Queue(ID3D12CommandQueue* queue) noexcept {
    try {
        auto& state = State(); std::lock_guard<std::mutex> lock(state.mutex);
        Add(state,queue,slot::ID3D12CommandQueue_ExecuteCommandLists,reinterpret_cast<void*>(&Execute));
        if(MH_ApplyQueued() != MH_OK) ++state.failures;
        ++state.queues;
    } catch(...) { ++State().failures; }
}
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* object,UINT count,ID3D12CommandList* const* lists) {
    Call call;
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
    if(call.outer) ++State().submits;
    Original<Fn>(object,slot::ID3D12CommandQueue_ExecuteCommandLists)(object,count,lists);
}
HRESULT STDMETHODCALLTYPE CreateQueue(ID3D12Device* object,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID iid,void** output) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,void**);
    const auto result = Original<Fn>(object,slot::ID3D12Device_CreateCommandQueue)(object,desc,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        ComPtr<ID3D12CommandQueue> queue;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&queue)))) Queue(queue.Get());
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
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&list)))) List(list.Get());
    }
    return result;
}
HRESULT STDMETHODCALLTYPE CreateList1(ID3D12Device4* object,UINT node,D3D12_COMMAND_LIST_TYPE type,D3D12_COMMAND_LIST_FLAGS flags,REFIID iid,void** output) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*,UINT,D3D12_COMMAND_LIST_TYPE,D3D12_COMMAND_LIST_FLAGS,REFIID,void**);
    const auto result = Original<Fn>(object,slot::ID3D12Device4_CreateCommandList1)(object,node,type,flags,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) {
        ComPtr<ID3D12GraphicsCommandList> list;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&list)))) List(list.Get());
    }
    return result;
}
HRESULT STDMETHODCALLTYPE CreatePipeline(ID3D12Device* object,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,REFIID iid,void** output) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,REFIID,void**);
    const auto result = Original<Fn>(object,slot::ID3D12Device_CreateGraphicsPipelineState)(object,desc,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) ++State().pipelines; return result;
}
HRESULT STDMETHODCALLTYPE CreateResource(ID3D12Device* object,const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** output) {
    Call call;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    const auto result = Original<Fn>(object,slot::ID3D12Device_CreateCommittedResource)(object,heap,flags,desc,initial,clear,iid,output);
    if(call.outer && SUCCEEDED(result) && output && *output) ++State().resources; return result;
}
} // namespace
void Attach(ID3D12Device* device) noexcept {
    try {
        auto& state = State(); std::lock_guard<std::mutex> lock(state.mutex);
        if(!state.initialized) {
            if(MH_Initialize() != MH_OK) { ++state.failures; return; }
            state.initialized = true;
        }
        Add(state,device,slot::ID3D12Device_CreateCommandQueue,reinterpret_cast<void*>(&CreateQueue));
        Add(state,device,slot::ID3D12Device_CreateCommandList,reinterpret_cast<void*>(&CreateList));
        Add(state,device,slot::ID3D12Device_CreateGraphicsPipelineState,reinterpret_cast<void*>(&CreatePipeline));
        Add(state,device,slot::ID3D12Device_CreateCommittedResource,reinterpret_cast<void*>(&CreateResource));
        ComPtr<ID3D12Device4> newer;
        if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&newer))))
            Add(state,newer.Get(),slot::ID3D12Device4_CreateCommandList1,reinterpret_cast<void*>(&CreateList1));
        if(MH_ApplyQueued() != MH_OK) ++state.failures;
        ++state.devices;
    } catch(...) { ++State().failures; }
}
void Snapshot(Counts& output) noexcept {
    auto& s = State(); output = {};
    output.devices=s.devices; output.queues=s.queues; output.commandLists=s.lists; output.resources=s.resources; output.pipelines=s.pipelines;
    output.closes=s.closes; output.resets=s.resets; output.draws=s.draws; output.indexedDraws=s.indexed; output.bundles=s.bundles;
    output.submissions=s.submits; output.hookFailures=s.failures;
}
} // namespace vrx::dx12
