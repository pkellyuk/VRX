// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#include "mirror_scene.h"
#include "status.h"
static void AliasCopies(Scene& scene) {
    constexpr UINT64 alignment=D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    D3D12_HEAP_DESC heap{};heap.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;heap.SizeInBytes=3*alignment;heap.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> memory;Hr(scene.device->CreateHeap(&heap,IID_PPV_ARGS(&memory)));
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=2*alignment;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> a,b,upload,readback;
    Hr(scene.device->CreatePlacedResource(memory.Get(),0,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&a)));
    desc.Width=alignment;Hr(scene.device->CreatePlacedResource(memory.Get(),alignment,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&b)));
    desc.Width=16;D3D12_HEAP_PROPERTIES cpu{};cpu.Type=D3D12_HEAP_TYPE_UPLOAD;Committed(scene.device.Get(),cpu,desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,upload,false);
    cpu.Type=D3D12_HEAP_TYPE_READBACK;Committed(scene.device.Get(),cpu,desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,readback);
    const UINT expected[]={0x12345678,0xabcdef01,0x55aa00ff,0x98765432};void* data=nullptr;Hr(upload->Map(0,nullptr,&data));std::memcpy(data,expected,sizeof(expected));upload->Unmap(0,nullptr);
    scene.Wait();Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),nullptr));
    D3D12_RESOURCE_BARRIER alias{};alias.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;alias.Aliasing={nullptr,a.Get()};scene.commands->ResourceBarrier(1,&alias);
    scene.commands->CopyBufferRegion(a.Get(),alignment,upload.Get(),0,16);
    alias.Aliasing={a.Get(),b.Get()};scene.commands->ResourceBarrier(1,&alias);
    D3D12_RESOURCE_BARRIER state{};state.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;state.Transition={b.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};scene.commands->ResourceBarrier(1,&state);
    scene.commands->CopyBufferRegion(readback.Get(),0,b.Get(),0,16);Hr(scene.commands->Close());ID3D12CommandList* lists[]={scene.commands.Get()};scene.queue->ExecuteCommandLists(1,lists);scene.Wait();
    Hr(readback->Map(0,nullptr,&data));Check(!std::memcmp(data,expected,16),"native overlapping placed buffers retain GPU-written bytes");readback->Unmap(0,nullptr);
    {Diagnostic diagnostic;ComPtr<ID3D12Resource> right;Hr(twin(readback.Get(),&right));Hr(right->Map(0,nullptr,&data));Check(!std::memcmp(data,expected,16),"right-eye placed resources retain the same heap overlap, not independent committed storage");right->Unmap(0,nullptr);}
    scene.DebugCheck();
}
int main(int argc,char** argv) {
    try {
        bool unsupported=argc>1 && std::strcmp(argv[1],"--unsupported")==0;
        const bool modern=argc>1 && std::strcmp(argv[1],"--modern")==0;
        if(modern)Check(SetEnvironmentVariableW(L"VRX_DX12_TEST_MODERN",L"1")!=0,"enable modern fixture factories");
        bool compute=modern || (argc>1 && std::strcmp(argv[1],"--compute")==0);
        bool bindings=compute || (argc>1 && std::strcmp(argv[1],"--bindings")==0);
        bool warp=argc>2 && std::strcmp(argv[2],"--warp")==0;
        Check(argc>=2 && argc<=3 && (unsupported || bindings || std::strcmp(argv[1],"--mirror")==0) && (argc!=3 || warp),"recognized fixture mode and adapter option");
        const auto module=GetModuleHandleW(L"d3d12.dll");
        counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts")); twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin")); scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));
        Check(counts && twin && scope,"local native mirror bridge loaded");
        const auto session=L"native-mirror-status-"+std::to_wstring(GetCurrentProcessId());Check(SetEnvironmentVariableW(L"VRX_XG_SESSION",session.c_str())!=0,"set isolated diagnostics session");
        ComPtr<ID3D12Debug> debug; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        double distance[2]{};
        {
            ComPtr<IDXGIAdapter> adapter; if(warp){ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
            Scene scene; scene.Init(adapter.Get()); Bindings views;if(bindings) views.Init(scene,compute);Pipeline(scene,bindings);
            if(modern)AliasCopies(scene);
            { Diagnostic native; Frame(scene,0,2,false,true,bindings?&views:nullptr); Pixels(scene,Capture(scene,true,true),0,true); }
            for(UINT frame=1;frame<=18;++frame) {
                float w=frame>=17?1.0f:frame%2?2.0f:4.0f; Frame(scene,frame,w,false,frame%3==0,bindings?&views:nullptr);
                auto disparity=Pixels(scene,Capture(scene),frame,w==1);
                if(w!=1) { Check(disparity>(w==2?7:3),"replayed shader produces real geometry disparity"); distance[frame%2]=disparity; }
            }
            Check(distance[1]/distance[0]>1.7 && distance[1]/distance[0]<2.3,"replay disparity decreases with depth");
            if(unsupported) { Frame(scene,19,2,true); Pixels(scene,Capture(scene,false),19,true); }
        }
        vrx::dx12::Counts result; Hr(counts(&result,sizeof(result)));
        Check(result.version==4 && !result.hookFailures && result.mirrorReplays==(modern?19u:18u) && result.mirrorResources>=1 && result.mirrorPipelines>=2,"one replay per app submission, with isolated resources and patched pipelines");
        Check(result.mirrorStops==(unsupported?1u:0u),"unsupported commands stop only stereo replay");
        Check(result.draws==(bindings?30u:unsupported?13u:12u) && result.indexedDraws==6,"the app issues expected draws per simulation frame");
        Check(!result.mirrorLiveObjects,"all original identities and shadow objects retire after app release");
        if(modern)Check(result.streamPipelines>=2 && result.memoryHeaps>=7 && result.placedResources>=8 && result.modernResources>=5,"modern pipeline/resource factories were actually intercepted");
        if(unsupported)Check(std::strstr(result.reason,"SOSetTargets")!=nullptr,"unsupported command has an actionable stop reason");
        vrx::dx12::StatusReader status;vrx::dx12::StatusFrame frame;
        Check(status.Open(session) && status.Read(frame,GetCurrentProcessId()) && frame.mirroring && frame.counts.mirrorReplays==result.mirrorReplays &&
            frame.counts.streamPipelines==result.streamPipelines && !std::strcmp(frame.counts.reason,result.reason),"live bridge status exposes the same counts and stop reason over bounded IPC");
        std::printf("PASS: native DX12 command/resource mirror; 18 same-frame replays, disparity %.2f/%.2f; original output, HUD, lifetime cleanup%s%s\n",distance[1],distance[0],unsupported?", unsupported-command fallback":"",compute?", GPU compute-to-graphics dependencies":bindings?", SRV/CBV descriptors, root buffers and depth occlusion":"");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
