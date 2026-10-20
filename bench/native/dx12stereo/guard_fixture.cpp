// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Unsupported stream-output state stops replay while native GPU results survive.
#include "mirror_scene.h"
int main(int argc,char** argv){try {
    const bool warp=argc==3 && !std::strcmp(argv[2],"--warp");Check(argc==2 || warp,"known guard fixture adapter");
    const bool write=!std::strcmp(argv[1],"--extended-guard=write"),protect=!std::strcmp(argv[1],"--extended-guard=protected");
    const char* reason=write?"WriteBufferImmediate":protect?"SetProtectedResourceSession":"SOSetTargets";
    const auto module=GetModuleHandleW(L"d3d12.dll");
    counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));Check(counts && twin && scope,"native guard diagnostics loaded");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    {
        ComPtr<IDXGIAdapter> adapter;if(warp){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        Scene scene;scene.Init(adapter.Get());Pipeline(scene,false);Frame(scene,1,2,false,false);Check(Pixels(scene,Capture(scene),1)>7,"stereo works before the unsupported query");
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=1;ComPtr<ID3D12QueryHeap> queries;Hr(scene.device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries)));
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=16;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> output;Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&output)));
        ComPtr<ID3D12Resource> immediate;if(write){heap.Type=D3D12_HEAP_TYPE_DEFAULT;Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&immediate)));}
        scene.Wait();Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),nullptr));
        const char marker[]="native marker";scene.commands->SetMarker(0,marker,sizeof(marker));
        vrx::dx12::Counts before;Hr(counts(&before,sizeof(before)));Check(!before.mirrorStops,"passive annotations never stop stereo");
        if(write){
            D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};Hr(scene.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&options,sizeof(options)));Check((options.WriteBufferImmediateSupportFlags&D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT)!=0,"adapter supports native direct immediate writes");
            ComPtr<ID3D12GraphicsCommandList2> extended;Hr(scene.commands.As(&extended));D3D12_WRITEBUFFERIMMEDIATE_PARAMETER value{immediate->GetGPUVirtualAddress(),0x1234abcd};extended->WriteBufferImmediate(1,&value,nullptr);
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={immediate.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE};scene.commands->ResourceBarrier(1,&barrier);
            scene.commands->CopyBufferRegion(output.Get(),8,immediate.Get(),0,4);
        }else if(protect){ComPtr<ID3D12GraphicsCommandList3> extended;Hr(scene.commands.As(&extended));extended->SetProtectedResourceSession(nullptr);}
        else scene.commands->SOSetTargets(0,0,nullptr);
        scene.commands->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        scene.commands->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,1,output.Get(),0);Hr(scene.commands->Close());
        ID3D12CommandList* lists[]={scene.commands.Get()};scene.queue->ExecuteCommandLists(1,lists);scene.Wait();scene.DebugCheck();
        void* data=nullptr;Hr(output->Map(0,nullptr,&data));const auto timestamp=*static_cast<const UINT64*>(data);const auto immediateValue=static_cast<const UINT*>(data)[2];output->Unmap(0,nullptr);Check(timestamp!=0,"native timestamp and resolve still execute after stereo stops");if(write)Check(immediateValue==0x1234abcd,"native immediate GPU write survives the stereo guard");
        Frame(scene,2,2,false,false);Pixels(scene,Capture(scene,false),2,true);
    }
    vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));
    Check(result.mirrorReplays==1 && result.mirrorStops==1 && !result.hookFailures && !result.mirrorLiveObjects && std::strstr(result.reason,reason),"unsupported recording stops only replay and all tracked objects retire");
    std::puts("PASS: native DX12 unsupported-command guard, original GPU query results and continued native rendering");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
