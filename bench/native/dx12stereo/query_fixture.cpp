// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Eye-specific GPU query results drive predication without CPU query emulation.
#include "mirror_scene.h"
namespace {
ComPtr<ID3D12Resource> Buffer(Scene& scene,D3D12_HEAP_TYPE type){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=32;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> out;Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out)));return out;
}
void Transition(Scene& scene,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};scene.commands->ResourceBarrier(1,&b);}
void Results(ID3D12Resource* readback,bool timestamp){
    void* mapped=nullptr;Hr(readback->Map(0,nullptr,&mapped));UINT64 words[4]{};std::memcpy(words,mapped,sizeof(words));readback->Unmap(0,nullptr);
    Check(timestamp?words[1]>0 && words[2]>=words[1]:words[1]==1 && words[2]==0,"native GPU query values at nonzero result offsets");
}
}
int main(int argc,char** argv){try {
    const bool warp=argc==3 && !std::strcmp(argv[2],"--warp");Check(argc==2 || warp,"known query fixture adapter");
    const auto module=GetModuleHandleW(L"d3d12.dll");counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));Check(counts && twin && scope,"native query diagnostics loaded");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    {
        ComPtr<IDXGIAdapter> adapter;if(warp){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        Scene scene;scene.Init(adapter.Get());Pipeline(scene,false);
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_OCCLUSION;q.Count=2;ComPtr<ID3D12QueryHeap> occlusion,timestamps;Hr(scene.device->CreateQueryHeap(&q,IID_PPV_ARGS(&occlusion)));q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;Hr(scene.device->CreateQueryHeap(&q,IID_PPV_ARGS(&timestamps)));
        auto predicate=Buffer(scene,D3D12_HEAP_TYPE_DEFAULT),readback=Buffer(scene,D3D12_HEAP_TYPE_READBACK),time=Buffer(scene,D3D12_HEAP_TYPE_READBACK);
        for(UINT frame=1;frame<=4;++frame){
            scene.Wait();Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),scene.pipeline.Get()));scene.commands->SetGraphicsRootSignature(scene.root.Get());
            D3D12_VIEWPORT viewport{0,0,float(scene.width),float(scene.height),0,1};D3D12_RECT rect{0,0,LONG(scene.width),LONG(scene.height)};scene.commands->RSSetViewports(1,&viewport);scene.commands->RSSetScissorRects(1,&rect);scene.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            auto target=scene.rtv->GetCPUDescriptorHandleForHeapStart();const float black[]={0,0,0,1};scene.commands->ClearRenderTargetView(target,black,0,nullptr);scene.commands->OMSetRenderTargets(1,&target,FALSE,nullptr);
            const float values[]={2,float(frame+1)/32,.5f,.75f};scene.commands->SetGraphicsRoot32BitConstants(0,4,values,0);
            scene.commands->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
            scene.commands->BeginQuery(occlusion.Get(),D3D12_QUERY_TYPE_BINARY_OCCLUSION,0);scene.commands->DrawInstanced(3,1,0,0);scene.commands->EndQuery(occlusion.Get(),D3D12_QUERY_TYPE_BINARY_OCCLUSION,0);
            scene.commands->BeginQuery(occlusion.Get(),D3D12_QUERY_TYPE_BINARY_OCCLUSION,1);scene.commands->EndQuery(occlusion.Get(),D3D12_QUERY_TYPE_BINARY_OCCLUSION,1);
            scene.commands->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
            if(frame>1)Transition(scene,predicate.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            scene.commands->ResolveQueryData(occlusion.Get(),D3D12_QUERY_TYPE_BINARY_OCCLUSION,0,2,predicate.Get(),8);scene.commands->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,time.Get(),8);
            Transition(scene,predicate.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PREDICATION);scene.commands->ClearRenderTargetView(target,black,0,nullptr);
            scene.commands->SetPredication(predicate.Get(),8,D3D12_PREDICATION_OP_EQUAL_ZERO);scene.commands->DrawInstanced(3,1,0,0);
            const float wrong[]={2,.9f,.1f,.2f};scene.commands->SetGraphicsRoot32BitConstants(0,4,wrong,0);scene.commands->SetPredication(predicate.Get(),16,D3D12_PREDICATION_OP_EQUAL_ZERO);scene.commands->DrawInstanced(3,1,0,0);
            scene.commands->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);Transition(scene,predicate.Get(),D3D12_RESOURCE_STATE_PREDICATION,D3D12_RESOURCE_STATE_COPY_SOURCE);scene.commands->CopyBufferRegion(readback.Get(),0,predicate.Get(),0,32);
            Hr(scene.commands->Close());ID3D12CommandList* lists[]={scene.commands.Get()};scene.queue->ExecuteCommandLists(1,lists);scene.Wait();scene.DebugCheck();
            Check(Pixels(scene,Capture(scene),frame)>7,"eye-local query predication preserves current-frame geometry stereo");Results(readback.Get(),false);Results(time.Get(),true);
            {Diagnostic diagnostic;ComPtr<ID3D12Resource> right,clock;Hr(twin(readback.Get(),&right));Hr(twin(time.Get(),&clock));Results(right.Get(),false);Results(clock.Get(),true);}
        }
    }
    vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));Check(result.mirrorReplays==4 && result.draws==12 && !result.mirrorStops && !result.hookFailures && !result.mirrorLiveObjects,"query heaps and predication replay and retire without errors");
    std::puts("PASS: native DX12 independent occlusion/timestamp heaps, offset resolves, GPU predication and native/right-eye pixels");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
