// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#include "mirror.h"
#include "dxbc_patch.h"
#include "publisher.h"
#include "pipeline_stream.h"
#include "interceptor.h"
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
#include <cstdio>
#include <algorithm>

namespace vrx::dx12::mirror {
using Microsoft::WRL::ComPtr;
struct Context {
    ID3D12Device* device=nullptr; // app/child objects own its native lifetime
    bool active=true;
    bool recordOnly=false; // diagnostic: clone/record, never submit or publish
    bool cloneOnly=false; // diagnostic: no shadow recording/queue operations
    bool stateOnly=false; // diagnostic: omit transfer/query and work commands
    bool skipWork=false; // diagnostic: record state/transfers without GPU work
    bool skipCommands=false; // diagnostic: retain queue mappings, omit recording
    dxbc::Options calibration{.25f,0,true}; // native left, shifted right
    std::string reason;
};
namespace {
enum class Kind { Device,Queue,Allocator,Resource,Pipeline,Heap,List,Swapchain,MemoryHeap,QueryHeap,Signature };
struct Item {
    Kind kind;
    std::shared_ptr<Context> context;
    std::vector<void*> identities;
    ComPtr<ID3D12Resource> resource;
    ID3D12Resource* resourceAlias=nullptr; // app-owned, GPU-read-only upload input
    ComPtr<ID3D12PipelineState> pipeline;
    ID3D12PipelineState* pipelineAlias=nullptr;
    std::string pipelineReason;
    ComPtr<ID3D12Heap> memory;
    ID3D12Heap* memoryAlias=nullptr; // app-owned upload allocation
    ComPtr<ID3D12QueryHeap> queries;
    ID3D12CommandSignature* signature=nullptr; // immutable native object, app-owned
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
    ComPtr<ID3D12CommandQueue> queue;
    std::unique_ptr<xg::dx12::Publisher> publisher;
    uint64_t frame=0;
    DXGI_COLOR_SPACE_TYPE colorSpace=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    ID3D12Resource* RightResource() const {return resourceAlias?resourceAlias:resource.Get();}
    ID3D12Heap* RightMemory() const {return memoryAlias?memoryAlias:memory.Get();}
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
            if(entry.second->publisher)entry.second->publisher->Close();
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
    if(context && context->active) {
        context->active=false; context->reason=reason; ++Registry().counts.stops;
        strncpy_s(Registry().counts.reason,sizeof(Registry().counts.reason),reason,_TRUNCATE);
        for(const auto& entry:Registry().objects)
            if(entry.second->context==context && entry.second->publisher) entry.second->publisher->Close();
        vrx::dx12::Report(true);
    }
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
void TrackResource(ID3D12Resource* original,const std::shared_ptr<Item>& item) {
    if(item->description.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER){item->leftAddress=original->GetGPUVirtualAddress();item->rightAddress=item->RightResource()->GetGPUVirtualAddress();}
    Remember(original,item);++Registry().counts.resources;
}
bool HeapSupported(const D3D12_HEAP_PROPERTIES& heap,D3D12_HEAP_FLAGS flags) {
    return !(flags&(D3D12_HEAP_FLAG_SHARED|D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER|D3D12_HEAP_FLAG_HARDWARE_PROTECTED)) &&
        (heap.Type==D3D12_HEAP_TYPE_DEFAULT || heap.Type==D3D12_HEAP_TYPE_UPLOAD || heap.Type==D3D12_HEAP_TYPE_READBACK);
}
} // namespace
bool Mapper::Valid() const { return context_ && context_->active; }
ID3D12Resource* Mapper::Resource(ID3D12Resource* original) {
    if(!original) return nullptr;
    auto item=Mapped(original,Kind::Resource,context_); return item?item->RightResource():nullptr;
}
ID3D12PipelineState* Mapper::Pipeline(ID3D12PipelineState* original) {
    if(!original) return nullptr;
    auto item=Mapped(original,Kind::Pipeline,context_);
    if(item && !item->pipeline && !item->pipelineAlias)Stop(context_,item->pipelineReason.empty()?"unsupported vertex pipeline":item->pipelineReason.c_str());
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
ID3D12Heap* Mapper::Memory(ID3D12Heap* original) {
    if(!original)return nullptr;
    auto item=Mapped(original,Kind::MemoryHeap,context_);return item?item->RightMemory():nullptr;
}
ID3D12QueryHeap* Mapper::Query(ID3D12QueryHeap* original) {
    auto item=Mapped(original,Kind::QueryHeap,context_);return item?item->queries.Get():nullptr;
}
ID3D12CommandSignature* Mapper::Signature(ID3D12CommandSignature* original) {
    auto item=Mapped(original,Kind::Signature,context_);
    if(item && !item->signature)Stop(context_,"indirect command signature embeds GPU addresses or unsupported arguments");
    return item?item->signature:nullptr;
}
void Device(ID3D12Device* device) noexcept {
    if(!IsEnabled()) return;
    try {
        auto& s=Registry(); Lock lock(s.mutex); Drain(); if(Find(device)) return;
        auto context=std::make_shared<Context>(); context->device=device;
        wchar_t diagnostic[3]{};context->recordOnly=GetEnvironmentVariableW(L"VRX_DX12_RECORD_ONLY",diagnostic,3)==1 && diagnostic[0]==L'1';
        context->cloneOnly=GetEnvironmentVariableW(L"VRX_DX12_CLONE_ONLY",diagnostic,3)==1 && diagnostic[0]==L'1';context->recordOnly=context->recordOnly || context->cloneOnly;
        context->skipCommands=context->cloneOnly || (context->recordOnly && GetEnvironmentVariableW(L"VRX_DX12_SKIP_COMMANDS",diagnostic,3)==1 && diagnostic[0]==L'1');
        context->skipWork=context->recordOnly && GetEnvironmentVariableW(L"VRX_DX12_SKIP_WORK",diagnostic,3)==1 && diagnostic[0]==L'1';
        context->stateOnly=context->recordOnly && GetEnvironmentVariableW(L"VRX_DX12_STATE_ONLY",diagnostic,3)==1 && diagnostic[0]==L'1';
        if(device->GetNodeCount()!=1) Stop(context,"multiple GPU nodes are unsupported");
        context->calibration.shift=Calibration(L"VRX_DX12_SHIFT",.25f,64);
        context->calibration.convergence=Calibration(L"VRX_DX12_CONVERGENCE",0,.25f);
        if(context->calibration.shift<0 || context->calibration.convergence<0) Stop(context,"invalid calibration");
        Remember(device,New(Kind::Device,context));
    } catch(...) { }
}
void Alias(IUnknown* original,IUnknown* alias) noexcept {
    if(!alias || alias==original)return;
    try {
        Lock lock(Registry().mutex);Drain();auto item=Find(original);if(!item || Find(alias)==item)return;
        item->identities.push_back(alias);Registry().objects[alias]=item;Index(alias,item);
    } catch(...) {Failure(original,"COM interface alias observation failed");}
}
void Queue(ID3D12Device* device,ID3D12CommandQueue* queue) noexcept {
    try { Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(context) Remember(queue,New(Kind::Queue,context)); } catch(...) { }
}
bool Swapchain(ID3D12CommandQueue* queue,IDXGISwapChain* original) noexcept {
    try {
        Lock lock(Registry().mutex);Drain();auto context=Owner(queue);if(!context || !context->active) return false;
        if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) {Stop(context,"non-direct presentation queue");return false;}
        ComPtr<IDXGISwapChain3> chain;if(FAILED(original->QueryInterface(IID_PPV_ARGS(&chain)))) {Stop(context,"swapchain3 required");return false;}
        auto item=New(Kind::Swapchain,context);item->queue=queue;Remember(original,item);
        if(chain.Get()!=original) {item->identities.push_back(chain.Get());Registry().objects[chain.Get()]=item;Index(chain.Get(),item);}
        ++Registry().counts.swapchains;return true;
    } catch(...) {Failure(queue,"swapchain observation failed");return false;}
}
void BackBuffer(IDXGISwapChain* chain,ID3D12Resource* original) noexcept {
    try {
        Lock lock(Registry().mutex);Drain();auto context=Owner(chain);if(!context || !context->active || Find(original))return;
        const auto desc=original->GetDesc();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        Resource(context->device,original,&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_PRESENT,nullptr);
    } catch(...) {Failure(chain,"backbuffer observation failed");}
}
void ColorSpace(IDXGISwapChain* original,DXGI_COLOR_SPACE_TYPE color) noexcept {
    try {
        Lock lock(Registry().mutex);Drain();auto item=Find(original);if(!item || item->kind!=Kind::Swapchain)return;
        item->colorSpace=color;
        if(color!=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
            char reason[128]{};sprintf_s(reason,"presentation color space %u requires explicit HDR/gamut conversion",UINT(color));Stop(item->context,reason);
        }
    }catch(...){Failure(original,"presentation color space observation failed");}
}
void Present(IDXGISwapChain* original,UINT flags) noexcept {
    if(flags&DXGI_PRESENT_TEST)return;
    try {
        Lock lock(Registry().mutex);Drain();auto item=Find(original);if(!item || item->kind!=Kind::Swapchain || !item->context->active)return;
        if(item->context->recordOnly)return;
        if(flags&DXGI_PRESENT_DO_NOT_WAIT) {Stop(item->context,"nonblocking presentation needs a publication transaction");return;}
        wchar_t session[66]{};const auto length=GetEnvironmentVariableW(L"VRX_XG_SESSION",session,66);
        // Offscreen interception tests need no named stream. A launcher supplies
        // the session explicitly; never claim an arbitrary process-wide stream.
        if(!length)return;
        if(length>=66 || !xg::ValidSession(session)) {Stop(item->context,"invalid presentation session");return;}
        ComPtr<IDXGISwapChain3> chain;ComPtr<ID3D12Resource> left;
        if(FAILED(original->QueryInterface(IID_PPV_ARGS(&chain))) || FAILED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&left)))) {Stop(item->context,"presentation buffer unavailable");return;}
        auto resource=Mapped(left.Get(),Kind::Resource,item->context);if(!resource)return;
        if(!item->publisher) {
            const auto desc=left->GetDesc();
            if(!xg::dx12::FormatConvert::PresentationSupported(desc.Format,item->colorSpace) || desc.Width>4096 || desc.Height>4096 || desc.SampleDesc.Count!=1) {
                char reason[160]{};sprintf_s(reason,"unsupported presentation: format=%u, size=%llux%u, samples=%u",UINT(desc.Format),static_cast<unsigned long long>(desc.Width),desc.Height,desc.SampleDesc.Count);
                Stop(item->context,reason);return;
            }
            auto publisher=std::make_unique<xg::dx12::Publisher>();
            if(!publisher->Create(item->queue.Get(),session,UINT(desc.Width),desc.Height,desc.Format)) {Stop(item->context,"shared presentation stream creation failed");return;}
            item->publisher=std::move(publisher);
        }
        ++Registry().counts.presentations;
        const auto result=item->publisher->Submit(++item->frame,{left.Get(),0,D3D12_RESOURCE_STATE_PRESENT},{resource->resource.Get(),0,D3D12_RESOURCE_STATE_PRESENT});
        if(result==xg::dx12::PublishResult::Published)++Registry().counts.publications;
        else if(result==xg::dx12::PublishResult::Dropped)++Registry().counts.presentDrops;
        else Stop(item->context,"shared presentation submission failed");
    } catch(...) {Failure(original,"presentation translation failed");}
}
void Resize(IDXGISwapChain* original,UINT count,const UINT* nodes,IUnknown* const* queues) noexcept {
    try {
        Lock lock(Registry().mutex);Drain();auto item=Find(original);if(!item || item->kind!=Kind::Swapchain)return;
        if(nodes || queues) {
            if(!count) {DXGI_SWAP_CHAIN_DESC desc{};if(SUCCEEDED(original->GetDesc(&desc)))count=desc.BufferCount;}
            for(UINT i=0;i<count;++i) {
                ComPtr<ID3D12CommandQueue> queue;
                if((nodes && nodes[i]>1) || (queues && (!queues[i] || FAILED(queues[i]->QueryInterface(IID_PPV_ARGS(&queue))) || queue.Get()!=item->queue.Get()))) {
                    Stop(item->context,"multiple presentation queues or nodes need additional support");break;
                }
            }
        }
        // Drop native backbuffer references only after their copy work completes,
        // before DXGI validates that ResizeBuffers owns every buffer reference.
        item->publisher.reset();Drain();++Registry().counts.resizes;
    } catch(...) {Failure(original,"presentation resize cleanup failed");}
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
        if(!HeapSupported(*heap,flags)){Stop(context,"resource cannot be mirrored");return;}
        // UPLOAD buffers are GPU-read-only GENERIC_READ inputs. Both command
        // lists consume the app's same bytes; its fence follows the paired
        // submission and protects reuse for both eyes. Borrow without AddRef so
        // normal app Release can retire the identity, just like compute PSOs.
        if(heap->Type==D3D12_HEAP_TYPE_UPLOAD){item->resourceAlias=original;TrackResource(original,item);return;}
        if(FAILED(device->CreateCommittedResource(heap,flags,desc,initial,clear,IID_PPV_ARGS(&item->resource)))) { Stop(context,"resource cannot be mirrored"); return; }
        if(item->resource.Get()==original) { Stop(context,"resource clone aliases original"); return; }
        TrackResource(original,item);
    } catch(...) { Failure(device,"resource observation failed"); }
}
void MemoryHeap(ID3D12Device* device,ID3D12Heap* original,const D3D12_HEAP_DESC* desc) noexcept {
    try {
        Lock lock(Registry().mutex);Drain();auto context=Owner(device);if(!context || !context->active)return;
        auto item=New(Kind::MemoryHeap,context);item->heapType=desc->Properties.Type;
        if(!HeapSupported(desc->Properties,desc->Flags)){Stop(context,"memory heap cannot be mirrored");return;}
        if(item->heapType==D3D12_HEAP_TYPE_UPLOAD)item->memoryAlias=original;
        else if(FAILED(device->CreateHeap(desc,IID_PPV_ARGS(&item->memory)))){Stop(context,"memory heap cannot be mirrored");return;}
        Remember(original,item);
    }catch(...){Failure(device,"memory heap observation failed");}
}
void QueryHeap(ID3D12Device* device,ID3D12QueryHeap* original,const D3D12_QUERY_HEAP_DESC* desc) noexcept {
    try {
        Lock lock(Registry().mutex);Drain();auto context=Owner(device);if(!context || !context->active)return;
        auto item=New(Kind::QueryHeap,context);
        if(desc->NodeMask>1 || FAILED(device->CreateQueryHeap(desc,IID_PPV_ARGS(&item->queries)))){Stop(context,"query heap cannot be mirrored");return;}
        Remember(original,item);
    }catch(...){Failure(device,"query heap observation failed");}
}
void CommandSignature(ID3D12Device* device,ID3D12CommandSignature* original,const D3D12_COMMAND_SIGNATURE_DESC* desc) noexcept {
    try {
        Lock lock(Registry().mutex);Drain();auto context=Owner(device);if(!context || !context->active)return;
        auto item=New(Kind::Signature,context);bool safe=desc && desc->NodeMask<=1 && desc->NumArgumentDescs>0 && desc->NumArgumentDescs<=64 && desc->pArgumentDescs;
        if(safe)for(UINT i=0;i<desc->NumArgumentDescs;++i){const auto type=desc->pArgumentDescs[i].Type;if(type!=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW && type!=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED && type!=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH){safe=false;break;}}
        // These argument records contain counts/indices, with no embedded GPU
        // addresses. Binding-bearing signatures require GPU argument rewriting.
        if(safe)item->signature=original;Remember(original,item);
    }catch(...){Failure(device,"command signature observation failed");}
}
void PlacedResource(ID3D12Device* device,ID3D12Resource* original,ID3D12Heap* heap,UINT64 offset,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear) noexcept {
    try {
        Lock lock(Registry().mutex);Drain();auto context=Owner(device);if(!context || !context->active)return;
        auto allocation=Mapped(heap,Kind::MemoryHeap,context);if(!allocation)return;
        auto item=New(Kind::Resource,context);item->description=*desc;item->heapType=allocation->heapType;item->memory=allocation->memory;
        if(item->heapType==D3D12_HEAP_TYPE_UPLOAD){item->resourceAlias=original;TrackResource(original,item);return;}
        if(FAILED(device->CreatePlacedResource(item->memory.Get(),offset,desc,initial,clear,IID_PPV_ARGS(&item->resource)))){Stop(context,"placed resource creation failed");return;}
        TrackResource(original,item);
    }catch(...){Failure(device,"placed resource observation failed");}
}
void ReservedResource(ID3D12Device* device,ID3D12Resource* original,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear) noexcept {
    try {
        Lock lock(Registry().mutex);Drain();auto context=Owner(device);if(!context || !context->active)return;
        auto item=New(Kind::Resource,context);item->description=*desc;
        if(FAILED(device->CreateReservedResource(desc,initial,clear,IID_PPV_ARGS(&item->resource)))){Stop(context,"reserved resource creation failed");return;}
        TrackResource(original,item);
    }catch(...){Failure(device,"reserved resource observation failed");}
}
void Pipeline(ID3D12Device* device,ID3D12PipelineState* original,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc) noexcept {
    try {
        Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(!context || !context->active) return;
        auto item=New(Kind::Pipeline,context);
        item->pipelineReason=desc->HS.BytecodeLength || desc->DS.BytecodeLength?"tessellation requires final domain-stage stereo transformation":"stream output requires additional stereo handling";
        if(!desc->HS.BytecodeLength && !desc->DS.BytecodeLength && !desc->StreamOutput.NumEntries && !desc->StreamOutput.NumStrides) {
            const auto final=desc->GS.BytecodeLength?desc->GS:desc->VS;auto shader=dxbc::Rewrite(final.pShaderBytecode,final.BytecodeLength,context->calibration);
            item->pipelineReason=shader.reason;
            if(shader.status==dxbc::Status::Patched) {
                auto clone=*desc;if(desc->GS.BytecodeLength)clone.GS={shader.bytes.data(),shader.bytes.size()};else clone.VS={shader.bytes.data(),shader.bytes.size()};clone.CachedPSO={};
                item->pipelineReason="rewritten graphics pipeline creation failed";
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
void StreamPipeline(ID3D12Device* device,ID3D12PipelineState* original,const D3D12_PIPELINE_STATE_STREAM_DESC* desc) noexcept {
    try {
        Lock lock(Registry().mutex);Drain();auto context=Owner(device);if(!context || !context->active)return;
        auto parsed=stream::Parse(desc);
        if(parsed.kind==stream::Kind::Compute){ComputePipeline(device,original);return;}
        auto item=New(Kind::Pipeline,context);item->pipelineReason=parsed.reason;
        if(parsed.kind==stream::Kind::Graphics) {
            const auto final=parsed.FinalShader();auto shader=dxbc::Rewrite(final.pShaderBytecode,final.BytecodeLength,context->calibration);item->pipelineReason=shader.reason;
            if(shader.status==dxbc::Status::Patched) {
                parsed.Shader({shader.bytes.data(),shader.bytes.size()});parsed.ClearCache();auto clone=parsed.Desc();ComPtr<ID3D12Device2> newer;
                item->pipelineReason="rewritten pipeline stream creation failed";
                if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&newer))) && SUCCEEDED(newer->CreatePipelineState(&clone,IID_PPV_ARGS(&item->pipeline)))) {
                    if(item->pipeline.Get()==original){item->pipelineAlias=original;item->pipeline.Reset();}
                    ++Registry().counts.pipelines;
                }
            }
        }
        Remember(original,item);
    }catch(...){Failure(device,"pipeline stream observation failed");}
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
void Record(ID3D12GraphicsCommandList* original,Callback<ID3D12GraphicsCommandList*,Mapper&> command,RecordKind kind) noexcept {
    if(!FastFind(original)) return;
    try { Lock lock(Registry().mutex); Drain(); auto item=Find(original); if(item && item->context->active && !item->context->skipCommands && !(kind==RecordKind::Work && item->context->skipWork) && !(kind!=RecordKind::State && item->context->stateOnly)) { Mapper map(item->context); command(item->list.Get(),map); } } catch(...) { Failure(original,"command translation failed"); }
}
void Observe(ID3D12Device* device,Callback<ID3D12Device*,Mapper&> command) noexcept {
    try { Lock lock(Registry().mutex); Drain(); auto context=Owner(device); if(context && context->active) { Mapper map(context); command(device,map); } }
    catch(...) { Failure(device,"descriptor translation failed"); }
}
void QueueCommand(ID3D12CommandQueue* queue,Callback<ID3D12CommandQueue*,Mapper&> command) noexcept {
    try { Lock lock(Registry().mutex);Drain();auto context=Owner(queue);if(context && context->active && !context->cloneOnly){Mapper map(context);command(queue,map);} }
    catch(...){Failure(queue,"queue mapping translation failed");}
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
        // GPU-read-only upload inputs are shared. No CPU reads or whole-buffer
        // copies at submission; app fences follow both lists before CPU reuse.
        if(context->active && !context->recordOnly) { execute(queue,UINT(paired.size()),paired.data()); Registry().counts.replays+=count; }
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
    try { Lock lock(Registry().mutex); Drain(); auto item=Find(original); if(!item || !item->RightResource() || !item->context->active) return E_FAIL; *output=item->RightResource();(*output)->AddRef();return S_OK; } catch(...) { return E_FAIL; }
}
void Snapshot(Counts& output) noexcept {
    try { Lock lock(Registry().mutex); Drain(); output=Registry().counts; std::unordered_set<Item*> visited; for(const auto& entry:Registry().objects) visited.insert(entry.second.get()); output.liveObjects=visited.size(); } catch(...) { output={}; }
}
void Unsupported(void* object,const char* reason) noexcept { Failure(object,reason); }
} // namespace vrx::dx12::mirror
