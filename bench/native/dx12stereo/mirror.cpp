// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#include "mirror.h"
#include "dxbc_patch.h"
#include <wrl/client.h>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <array>
#include <atomic>
#include <new>
#include <algorithm>

namespace vrx::dx12::mirror {
using Microsoft::WRL::ComPtr;
struct Context {
    ID3D12Device* device=nullptr; // app/child objects own its native lifetime
    bool active=true;
    dxbc::Options calibration{.25f,0,true}; // native left, shifted right
    std::string reason;
};
namespace {
enum class Kind { Device,Queue,Allocator,Resource,Pipeline,Heap,List };
struct Item {
    Kind kind;
    std::shared_ptr<Context> context;
    std::vector<void*> identities;
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12PipelineState> pipeline;
    ID3D12PipelineState* pipelineAlias=nullptr;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandAllocator> listAllocator;
    D3D12_HEAP_TYPE heapType=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    uint64_t leftAddress=0,rightAddress=0;
    D3D12_CPU_DESCRIPTOR_HANDLE leftCpu{},rightCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE leftGpu{},rightGpu{};
    UINT descriptors=0,stride=0;
    bool closed=false;
};
struct State {
    // Native internal Release callbacks can re-enter while a shadow call runs.
    std::recursive_mutex mutex;
    std::unordered_map<void*,std::shared_ptr<Item>> objects;
    using Bucket=std::vector<std::pair<void*,std::shared_ptr<Item>>>;
    std::array<std::shared_ptr<const Bucket>,2048> index;
    struct Pending { std::shared_ptr<Item> item; Pending* next=nullptr; };
    std::atomic<Pending*> retired{nullptr};
    std::atomic<bool> trackingLost{false};
    Counts counts;
};
State& Registry() { static auto* state=new State; return *state; }
using Lock=std::lock_guard<std::recursive_mutex>;
std::shared_ptr<Item> Find(void* p) { const auto i=Registry().objects.find(p); return i==Registry().objects.end()?nullptr:i->second; }
size_t BucketFor(void* p) { auto value=reinterpret_cast<uintptr_t>(p); return ((value>>4)^(value>>17))%Registry().index.size(); }
std::shared_ptr<Item> FastFind(void* p) {
    const auto bucket=std::atomic_load(&Registry().index[BucketFor(p)]);
    if(bucket) for(const auto& entry:*bucket) if(entry.first==p) return entry.second;
    return {};
}
void Index(void* p,const std::shared_ptr<Item>& item,const std::shared_ptr<Item>& remove={}) {
    auto& target=Registry().index[BucketFor(p)]; const auto old=std::atomic_load(&target);
    auto next=std::make_shared<State::Bucket>(old?*old:State::Bucket{});
    auto found=std::find_if(next->begin(),next->end(),[&](const auto& entry){return entry.first==p;});
    if(item) { if(found==next->end()) next->emplace_back(p,item); else found->second=item; }
    else if(found!=next->end() && found->second==remove) next->erase(found);
    std::atomic_store(&target,std::shared_ptr<const State::Bucket>(next));
}
void Drain() {
    if(Registry().trackingLost.load()) {
        for(const auto& entry:Registry().objects) {
            auto& context=entry.second->context;
            if(context->active) { context->active=false; ++Registry().counts.stops; }
        }
    }
    // Release callbacks never wait for this mutex or any GPU operation. Apply
    // retirement at the next API boundary; identity tokens prevent ABA erasure.
    while(auto* node=Registry().retired.exchange(nullptr)) {
        while(node) {
            auto* next=node->next; auto item=node->item;
            for(auto identity:item->identities) {
                auto found=Registry().objects.find(identity);
                if(found!=Registry().objects.end() && found->second==item) { Registry().objects.erase(found); Index(identity,{},item); }
            }
            if(item->kind==Kind::Device) item->context->active=false;
            delete node; node=next;
        }
    }
}
std::shared_ptr<Context> Owner(void* p) { auto object=Find(p); return object?object->context:nullptr; }
void Stop(const std::shared_ptr<Context>& context,const char* reason) {
    if(context && context->active) { context->active=false; context->reason=reason; ++Registry().counts.stops; }
}
void Failure(void* original,const char* reason) noexcept { try { Lock lock(Registry().mutex); Drain(); Stop(Owner(original),reason); } catch(...) {} }
bool IsEnabled() { wchar_t value[3]{}; return GetEnvironmentVariableW(L"VRX_DX12_MIRROR",value,3)==1 && value[0]==L'1'; }
float Calibration(const wchar_t* name,float fallback,float maximum) {
    char text[64]{};
    wchar_t wide[64]{}; const auto size=GetEnvironmentVariableW(name,wide,64);
    if(!size) return fallback;
    if(size>=64) return -1;
    for(DWORD i=0;i<size;++i) { if(wide[i]>127) return -1; text[i]=char(wide[i]); }
    float value=0; auto parsed=std::from_chars(text,text+size,value);
    return parsed.ec==std::errc{} && parsed.ptr==text+size && std::isfinite(value) && value>=0 && value<=maximum?value:-1;
}
void Remember(IUnknown* original,const std::shared_ptr<Item>& item) {
    auto& s=Registry();
    item->identities.push_back(original); s.objects[original]=item; Index(original,item);
    ComPtr<IUnknown> identity;
    if(SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&identity))) && identity.Get()!=original) {
        item->identities.push_back(identity.Get()); s.objects[identity.Get()]=item; Index(identity.Get(),item);
    }
}
std::shared_ptr<Item> New(Kind kind,const std::shared_ptr<Context>& context) {
    auto item=std::make_shared<Item>(); item->kind=kind; item->context=context; return item;
}
std::shared_ptr<Item> Mapped(void* original,Kind kind,const std::shared_ptr<Context>& context) {
    auto item=Find(original);
    if(!item || item->kind!=kind || item->context!=context) { Stop(context,"untracked or foreign resource/state"); return nullptr; }
    return item;
}
} // namespace
bool Mapper::Valid() const { return context_ && context_->active; }
ID3D12Resource* Mapper::Resource(ID3D12Resource* original) {
    if(!original) return nullptr;
    auto item=Mapped(original,Kind::Resource,context_); return item?item->resource.Get():nullptr;
}
ID3D12PipelineState* Mapper::Pipeline(ID3D12PipelineState* original) {
    if(!original) return nullptr;
    auto item=Mapped(original,Kind::Pipeline,context_);
    if(item && !item->pipeline && !item->pipelineAlias) Stop(context_,"unsupported vertex pipeline");
    return item?(item->pipelineAlias?item->pipelineAlias:item->pipeline.Get()):nullptr;
}
ID3D12GraphicsCommandList* Mapper::Bundle(ID3D12GraphicsCommandList* original) {
    auto item=Mapped(original,Kind::List,context_);
    if(item && !item->closed) Stop(context_,"open bundle referenced");
    return item?item->list.Get():nullptr;
}
D3D12_CPU_DESCRIPTOR_HANDLE Mapper::Cpu(D3D12_CPU_DESCRIPTOR_HANDLE original) {
    if(!original.ptr) return {};
    for(const auto& entry:Registry().objects) {
        const auto& item=entry.second;
        if(item->kind!=Kind::Heap || item->context!=context_ || original.ptr<item->leftCpu.ptr) continue;
        const auto delta=original.ptr-item->leftCpu.ptr;
        if(item->stride && delta%item->stride==0 && delta/item->stride<item->descriptors) return {item->rightCpu.ptr+delta};
    }
    Stop(context_,"descriptor handle is not tracked"); return {};
}
D3D12_GPU_VIRTUAL_ADDRESS Mapper::Address(D3D12_GPU_VIRTUAL_ADDRESS original) {
    if(!original) return 0;
    for(const auto& entry:Registry().objects) {
        const auto& item=entry.second;
        if(item->kind==Kind::Resource && item->context==context_ && item->leftAddress && original>=item->leftAddress &&
            original-item->leftAddress<item->description.Width) return item->rightAddress+(original-item->leftAddress);
    }
    Stop(context_,"GPU virtual address is not tracked"); return 0;
}
D3D12_GPU_DESCRIPTOR_HANDLE Mapper::Gpu(D3D12_GPU_DESCRIPTOR_HANDLE original) {
    if(!original.ptr) return {};
    for(const auto& entry:Registry().objects) {
        const auto& item=entry.second;
        if(item->kind!=Kind::Heap || item->context!=context_ || !item->leftGpu.ptr || original.ptr<item->leftGpu.ptr) continue;
        const auto delta=original.ptr-item->leftGpu.ptr;
        if(item->stride && delta%item->stride==0 && delta/item->stride<item->descriptors) return {item->rightGpu.ptr+delta};
    }
    Stop(context_,"GPU descriptor handle is not tracked"); return {};
}
ID3D12DescriptorHeap* Mapper::Heap(ID3D12DescriptorHeap* original) {
    auto item=Mapped(original,Kind::Heap,context_); return item?item->heap.Get():nullptr;
}
void Device(ID3D12Device* device) noexcept {
    if(!IsEnabled()) return;
    try {
        auto& s=Registry(); Lock lock(s.mutex); Drain(); if(Find(device)) return;
        auto context=std::make_shared<Context>(); context->device=device;
        if(device->GetNodeCount()!=1) Stop(context,"multiple GPU nodes are unsupported");
        context->calibration.shift=Calibration(L"VRX_DX12_SHIFT",.25f,64);
        context->calibration.convergence=Calibration(L"VRX_DX12_CONVERGENCE",0,.25f);
        if(context->calibration.shift<0 || context->calibration.convergence<0) Stop(context,"invalid calibration");
        Remember(device,New(Kind::Device,context));
    } catch(...) { }
}
void Queue(ID3D12Device* device,ID3D12CommandQueue* queue) noexcept {
    try { Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(context) Remember(queue,New(Kind::Queue,context)); } catch(...) { }
}
void Allocator(ID3D12Device* device,ID3D12CommandAllocator* original,D3D12_COMMAND_LIST_TYPE type) noexcept {
    try {
        Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(!context || !context->active) return;
        auto item=New(Kind::Allocator,context);
        if(FAILED(device->CreateCommandAllocator(type,IID_PPV_ARGS(&item->allocator)))) { Stop(context,"allocator creation failed"); return; }
        Remember(original,item);
    } catch(...) { Failure(device,"allocator observation failed"); }
}
void AllocatorReset(ID3D12CommandAllocator* original) noexcept {
    try { Lock lock(Registry().mutex); Drain(); auto item=Find(original); if(item && item->allocator && item->context->active && FAILED(item->allocator->Reset())) Stop(item->context,"allocator reset failed"); } catch(...) { }
}
void Resource(ID3D12Device* device,ID3D12Resource* original,const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear) noexcept {
    try {
        Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(!context || !context->active) return;
        auto item=New(Kind::Resource,context); item->description=*desc; item->heapType=heap->Type;
        if(flags&D3D12_HEAP_FLAG_SHARED || flags&D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER || (heap->Type!=D3D12_HEAP_TYPE_DEFAULT && heap->Type!=D3D12_HEAP_TYPE_UPLOAD && heap->Type!=D3D12_HEAP_TYPE_READBACK) ||
            FAILED(device->CreateCommittedResource(heap,flags,desc,initial,clear,IID_PPV_ARGS(&item->resource)))) { Stop(context,"resource cannot be mirrored"); return; }
        if(item->resource.Get()==original) { Stop(context,"resource clone aliases original"); return; }
        if(desc->Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) { item->leftAddress=original->GetGPUVirtualAddress(); item->rightAddress=item->resource->GetGPUVirtualAddress(); }
        Remember(original,item); ++Registry().counts.resources;
    } catch(...) { Failure(device,"resource observation failed"); }
}
void Pipeline(ID3D12Device* device,ID3D12PipelineState* original,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc) noexcept {
    try {
        Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(!context || !context->active) return;
        auto item=New(Kind::Pipeline,context);
        if(!desc->GS.BytecodeLength && !desc->HS.BytecodeLength && !desc->DS.BytecodeLength && !desc->StreamOutput.NumEntries) {
            auto shader=dxbc::Rewrite(desc->VS.pShaderBytecode,desc->VS.BytecodeLength,context->calibration);
            if(shader.status==dxbc::Status::Patched) {
                auto clone=*desc; clone.VS={shader.bytes.data(),shader.bytes.size()}; clone.CachedPSO={};
                if(SUCCEEDED(device->CreateGraphicsPipelineState(&clone,IID_PPV_ARGS(&item->pipeline)))) {
                    if(item->pipeline.Get()==original) { item->pipelineAlias=original; item->pipeline.Reset(); }
                    ++Registry().counts.pipelines;
                }
            }
        }
        // An unsupported pipeline is harmless until a mirrored list uses it.
        Remember(original,item);
    } catch(...) { Failure(device,"pipeline observation failed"); }
}
void Heap(ID3D12Device* device,ID3D12DescriptorHeap* original,const D3D12_DESCRIPTOR_HEAP_DESC* desc) noexcept {
    try {
        Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(!context || !context->active) return;
        auto item=New(Kind::Heap,context);
        if(FAILED(device->CreateDescriptorHeap(desc,IID_PPV_ARGS(&item->heap)))) { Stop(context,"descriptor heap creation failed"); return; }
        item->leftCpu=original->GetCPUDescriptorHandleForHeapStart(); item->rightCpu=item->heap->GetCPUDescriptorHandleForHeapStart();
        if(desc->Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
            item->leftGpu=original->GetGPUDescriptorHandleForHeapStart(); item->rightGpu=item->heap->GetGPUDescriptorHandleForHeapStart();
        }
        item->descriptors=desc->NumDescriptors; item->stride=device->GetDescriptorHandleIncrementSize(desc->Type); Remember(original,item);
    } catch(...) { Failure(device,"descriptor heap observation failed"); }
}
void ComputePipeline(ID3D12Device* device,ID3D12PipelineState* original) noexcept {
    try {
        Lock lock(Registry().mutex); Drain();auto context=Owner(device);if(!context || !context->active) return;
        auto item=New(Kind::Pipeline,context);item->pipelineAlias=original;
        // Compute bytecode is immutable. Its root bindings and writable resources
        // are translated on the replay list, including eye-specific GPU results.
        Remember(original,item);++Registry().counts.pipelines;
    } catch(...) {Failure(device,"compute pipeline observation failed");}
}
void Rtv(ID3D12Device* device,ID3D12Resource* resource,const D3D12_RENDER_TARGET_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept {
    try {
        Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(!context || !context->active) return;
        Mapper map(context); auto right=map.Resource(resource); auto target=map.Cpu(handle);
        if(map.Valid()) device->CreateRenderTargetView(right,desc,target);
    } catch(...) { Failure(device,"view observation failed"); }
}
void List(ID3D12Device* device,ID3D12GraphicsCommandList* original,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline,UINT node,D3D12_COMMAND_LIST_TYPE type,bool closed) noexcept {
    try {
        Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(!context || !context->active) return;
        auto item=New(Kind::List,context); item->closed=closed;
        if(!closed) {
            auto allocation=Mapped(allocator,Kind::Allocator,context); Mapper map(context); auto pso=map.Pipeline(pipeline);
            if(!allocation || !map.Valid()) return;
            item->listAllocator=allocation->allocator;
            if(FAILED(device->CreateCommandList(node,type,item->listAllocator.Get(),pso,IID_PPV_ARGS(&item->list)))) { Stop(context,"command list creation failed"); return; }
        } else {
            ComPtr<ID3D12Device4> newer;
            if(FAILED(device->QueryInterface(IID_PPV_ARGS(&newer))) || FAILED(newer->CreateCommandList1(node,type,D3D12_COMMAND_LIST_FLAG_NONE,IID_PPV_ARGS(&item->list)))) { Stop(context,"closed command list creation failed"); return; }
        }
        Remember(original,item);
    } catch(...) { Failure(device,"list observation failed"); }
}
void Reset(ID3D12GraphicsCommandList* original,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline) noexcept {
    try {
        Lock lock(Registry().mutex); Drain(); auto item=Find(original); if(!item || !item->context->active) return;
        auto allocation=Mapped(allocator,Kind::Allocator,item->context); Mapper map(item->context); auto pso=map.Pipeline(pipeline);
        if(!allocation || !map.Valid()) return;
        if(FAILED(item->list->Reset(allocation->allocator.Get(),pso))) { Stop(item->context,"command list reset failed"); return; }
        item->listAllocator=allocation->allocator; item->closed=false;
    } catch(...) { }
}
void Close(ID3D12GraphicsCommandList* original) noexcept {
    try { Lock lock(Registry().mutex); Drain(); auto item=Find(original); if(item && item->context->active) { if(FAILED(item->list->Close())) Stop(item->context,"command list close failed"); else item->closed=true; } } catch(...) { }
}
void Record(ID3D12GraphicsCommandList* original,Callback<ID3D12GraphicsCommandList*,Mapper&> command) noexcept {
    if(!FastFind(original)) return;
    try { Lock lock(Registry().mutex); Drain(); auto item=Find(original); if(item && item->context->active) { Mapper map(item->context); command(item->list.Get(),map); } } catch(...) { Failure(original,"command translation failed"); }
}
void Observe(ID3D12Device* device,Callback<ID3D12Device*,Mapper&> command) noexcept {
    try { Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(context && context->active) { Mapper map(context); command(device,map); } }
    catch(...) { Failure(device,"descriptor translation failed"); }
}
void Execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists,ExecuteFn execute) noexcept {
    try {
        Lock lock(Registry().mutex); Drain(); auto context=Owner(queue);
        if(!context || !context->active || !count || count>65536 || !lists) { execute(queue,count,lists); return; }
        std::vector<ID3D12CommandList*> paired(lists,lists+count);
        for(UINT i=0;i<count;++i) {
            auto list=Mapped(lists[i],Kind::List,context);
            if(!list || !list->closed) { Stop(context,"untracked or open command list submitted"); break; }
            paired.push_back(list->list.Get());
        }
        // Upload snapshots are captured before either submission. The app's
        // fences follow the entire paired batch, protecting reuse of both eyes.
        std::unordered_set<Item*> visited;
        for(const auto& entry:Registry().objects) {
            auto item=entry.second;
            if(item->context!=context || item->kind!=Kind::Resource || item->heapType!=D3D12_HEAP_TYPE_UPLOAD || !visited.insert(item.get()).second) continue;
            auto* original=static_cast<ID3D12Resource*>(item->identities[0]); void* source=nullptr; void* target=nullptr;
            const D3D12_RANGE noRead{0,0};
            const D3D12_RANGE readRange{0,SIZE_T(item->description.Width)};
            if(FAILED(original->Map(0,&readRange,&source))) { Stop(context,"upload snapshot failed"); break; }
            const auto mapped=item->resource->Map(0,&noRead,&target);
            if(SUCCEEDED(mapped)) { std::memcpy(target,source,size_t(item->description.Width)); item->resource->Unmap(0,nullptr); }
            original->Unmap(0,&noRead);
            if(FAILED(mapped)) { Stop(context,"upload shadow mapping failed"); break; }
        }
        if(context->active) { execute(queue,UINT(paired.size()),paired.data()); Registry().counts.replays+=count; }
        else execute(queue,count,lists);
    } catch(...) { Failure(queue,"submission translation failed"); execute(queue,count,lists); }
}
std::shared_ptr<void> BeforeRelease(void* original) noexcept { try { return FastFind(original); } catch(...) { return {}; } }
void Released(void*,std::shared_ptr<void> token) noexcept {
    try {
        auto item=std::static_pointer_cast<Item>(token); if(!item) return;
        auto* node=new(std::nothrow) State::Pending{item,nullptr};
        if(!node) { Registry().trackingLost.store(true); return; }
        auto& retired=Registry().retired; auto* head=retired.load();
        do { node->next=head; } while(!retired.compare_exchange_weak(head,node));
    } catch(...) { }
}
HRESULT Twin(ID3D12Resource* original,ID3D12Resource** output) noexcept {
    if(!output) return E_POINTER; *output=nullptr;
    try { Lock lock(Registry().mutex); Drain(); auto item=Find(original); if(!item || !item->resource || !item->context->active) return E_FAIL; return item->resource.CopyTo(output); } catch(...) { return E_FAIL; }
}
void Snapshot(Counts& output) noexcept {
    try { Lock lock(Registry().mutex); Drain(); output=Registry().counts; std::unordered_set<Item*> visited; for(const auto& entry:Registry().objects) visited.insert(entry.second.get()); output.liveObjects=visited.size(); } catch(...) { output={}; }
}
void Unsupported(void* object,const char* reason) noexcept { Failure(object,reason); }
} // namespace vrx::dx12::mirror
