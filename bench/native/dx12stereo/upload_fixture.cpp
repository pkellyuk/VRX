// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Read-only upload inputs are shared; every GPU-written result stays private.
#include "mirror_scene.h"
#include <array>
namespace {
ComPtr<ID3D12Resource> Buffer(Scene& scene,UINT64 bytes,D3D12_HEAP_TYPE type) {
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.SampleDesc.Count=1;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;ComPtr<ID3D12Resource> result;
    Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)));return result;
}
void Same(ID3D12Resource* resource,bool shared) {
    Diagnostic diagnostic;ComPtr<ID3D12Resource> right;Hr(twin(resource,&right));
    Check((right.Get()==resource)==shared,"only GPU-read-only upload inputs share native storage");
    if(shared)Check(right->GetGPUVirtualAddress()==resource->GetGPUVirtualAddress(),"upload GPU addresses are preserved");
}
void Bytes(ID3D12Resource* resource,const std::array<UINT,4>& expected) {
    void* data=nullptr;const D3D12_RANGE range{0,sizeof(expected)};Hr(resource->Map(0,&range,&data));const bool same=!std::memcmp(data,expected.data(),sizeof(expected));const D3D12_RANGE noWrite{0,0};resource->Unmap(0,&noWrite);Check(same,"both GPU results consume the latest CPU-written bytes");
}
}
int main(int argc,char** argv) {try {
    const bool warp=argc==3 && !std::strcmp(argv[2],"--warp");Check(argc==2 || warp,"known upload fixture adapter");
    const auto module=GetModuleHandleW(L"d3d12.dll");counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));Check(counts && twin && scope,"native upload diagnostics loaded");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    const auto started=GetTickCount64();
    {
        ComPtr<IDXGIAdapter> adapter;if(warp){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        Scene scene;scene.Init(adapter.Get());
        // A mostly untouched large input must not trigger per-submit scans or
        // copies. Small committed and overlapping placed inputs exercise data.
        auto large=Buffer(scene,64*1024*1024,D3D12_HEAP_TYPE_UPLOAD),committed=Buffer(scene,256,D3D12_HEAP_TYPE_UPLOAD);
        auto output=Buffer(scene,64,D3D12_HEAP_TYPE_DEFAULT),readback=Buffer(scene,64,D3D12_HEAP_TYPE_READBACK);
        Same(large.Get(),true);Same(committed.Get(),true);Same(output.Get(),false);Same(readback.Get(),false);
        constexpr UINT64 alignment=D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        D3D12_HEAP_DESC heap{};heap.Properties.Type=D3D12_HEAP_TYPE_UPLOAD;heap.SizeInBytes=2*alignment;heap.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;ComPtr<ID3D12Heap> memory;Hr(scene.device->CreateHeap(&heap,IID_PPV_ARGS(&memory)));
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=2*alignment;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.SampleDesc.Count=1;
        ComPtr<ID3D12Resource> placed[2];Hr(scene.device->CreatePlacedResource(memory.Get(),0,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&placed[0])));desc.Width=alignment;Hr(scene.device->CreatePlacedResource(memory.Get(),alignment,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&placed[1])));Same(placed[0].Get(),true);Same(placed[1].Get(),true);
        // Keep the maps live across recording, execution and fence-protected
        // reuse, as an engine's upload ring does.
        void* mapped[3]{};ID3D12Resource* inputs[]={committed.Get(),placed[0].Get(),placed[1].Get()};const D3D12_RANGE noRead{0,0};for(UINT i=0;i<3;++i)Hr(inputs[i]->Map(0,&noRead,&mapped[i]));
        for(auto type:{D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_TYPE_COMPUTE,D3D12_COMMAND_LIST_TYPE_COPY}) {
            D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=type;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
            Hr(scene.device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)));Hr(scene.device->CreateCommandAllocator(type,IID_PPV_ARGS(&allocator)));Hr(scene.device->CreateCommandList(0,type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));Hr(list->Close());
            for(UINT frame=1;frame<=6;++frame) {
                Hr(allocator->Reset());Hr(list->Reset(allocator.Get(),nullptr));const UINT index=frame%3;const UINT64 offset=index==1?alignment+32:32;
                if(index){D3D12_RESOURCE_BARRIER alias{};alias.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;alias.Aliasing={nullptr,inputs[index]};list->ResourceBarrier(1,&alias);}
                list->CopyBufferRegion(output.Get(),0,inputs[index],offset,16);
                D3D12_RESOURCE_BARRIER transition{};transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;transition.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&transition);list->CopyBufferRegion(readback.Get(),0,output.Get(),0,16);std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);list->ResourceBarrier(1,&transition);const auto closed=list->Close();scene.DebugCheck();Hr(closed);
                const std::array<UINT,4> expected{frame,UINT(type),0x1234abcd+frame,0x98760000+frame};std::memcpy(static_cast<BYTE*>(mapped[index])+offset,expected.data(),sizeof(expected));
                ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);Hr(queue->Signal(scene.completed.Get(),++scene.value));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"create upload completion event");const auto set=scene.completed->SetEventOnCompletion(scene.value,event);const auto wait=SUCCEEDED(set)?WaitForSingleObject(event,5000):WAIT_FAILED;CloseHandle(event);Hr(set);Check(wait==WAIT_OBJECT_0,"paired upload use completes before CPU reuse");
                Bytes(readback.Get(),expected);{Diagnostic diagnostic;ComPtr<ID3D12Resource> right;Hr(twin(readback.Get(),&right));Bytes(right.Get(),expected);}scene.DebugCheck();
            }
        }
        for(UINT i=0;i<3;++i)inputs[i]->Unmap(0,nullptr);
    }
    vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));Check(result.mirrorReplays==18 && !result.mirrorStops && !result.hookFailures && !result.mirrorLiveObjects,"shared upload inputs retire without pinning app resources");
    std::printf("PASS: native DX12 shared upload inputs; committed/placed overlaps, persistent maps, late writes, 18 direct/compute/copy batches, independent GPU outputs and cleanup; %llu ms\n",static_cast<unsigned long long>(GetTickCount64()-started));return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
