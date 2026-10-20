// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#include "mirror_scene.h"
int main(int argc,char** argv) {
    try {
        bool unsupported=argc>1 && std::strcmp(argv[1],"--unsupported")==0;
        bool compute=argc>1 && std::strcmp(argv[1],"--compute")==0;
        bool bindings=compute || (argc>1 && std::strcmp(argv[1],"--bindings")==0);
        bool warp=argc>2 && std::strcmp(argv[2],"--warp")==0;
        Check(argc>=2 && argc<=3 && (unsupported || bindings || std::strcmp(argv[1],"--mirror")==0) && (argc!=3 || warp),"recognized fixture mode and adapter option");
        const auto module=GetModuleHandleW(L"d3d12.dll");
        counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts")); twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin")); scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));
        Check(counts && twin && scope,"local native mirror bridge loaded");
        ComPtr<ID3D12Debug> debug; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        double distance[2]{};
        {
            ComPtr<IDXGIAdapter> adapter; if(warp){ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
            Scene scene; scene.Init(adapter.Get()); Bindings views;if(bindings) views.Init(scene,compute);Pipeline(scene,bindings);
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
        Check(result.version==3 && !result.hookFailures && result.mirrorReplays==18 && result.mirrorResources>=1 && result.mirrorPipelines>=2,"one replay per app submission, with isolated resources and patched pipelines");
        Check(result.mirrorStops==(unsupported?1u:0u),"unsupported commands stop only stereo replay");
        Check(result.draws==(bindings?30u:unsupported?13u:12u) && result.indexedDraws==6,"the app issues expected draws per simulation frame");
        Check(!result.mirrorLiveObjects,"all original identities and shadow objects retire after app release");
        std::printf("PASS: native DX12 command/resource mirror; 18 same-frame replays, disparity %.2f/%.2f; original output, HUD, lifetime cleanup%s%s\n",distance[1],distance[0],unsupported?", unsupported-command fallback":"",compute?", GPU compute-to-graphics dependencies":bindings?", SRV/CBV descriptors, root buffers and depth occlusion":"");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
