// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Recording diagnostics must leave shadow GPU storage untouched.
#include "mirror_scene.h"
int main(int argc,char** argv){try {
    const bool warp=argc==3 && !std::strcmp(argv[2],"--warp");Check(argc==2 || warp,"known record-only adapter");
    const auto module=GetModuleHandleW(L"d3d12.dll");counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));Check(counts && twin && scope,"native record-only diagnostics loaded");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    {
        ComPtr<IDXGIAdapter> adapter;if(warp){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        Scene scene;scene.Init(adapter.Get());Pipeline(scene);
        {
            Diagnostic diagnostic;ComPtr<ID3D12Resource> right;Hr(twin(scene.eyes.Get(),&right));
            ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;desc.NumDescriptors=1;Hr(scene.device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&heap)));
            D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;view.Texture2DArray.ArraySize=1;scene.device->CreateRenderTargetView(right.Get(),&view,heap->GetCPUDescriptorHandleForHeapStart());
            ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;Hr(scene.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));Hr(scene.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));const float sentinel[]={.125f,.25f,.75f,1};commands->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(),sentinel,0,nullptr);Hr(commands->Close());ID3D12CommandList* lists[]={commands.Get()};scene.queue->ExecuteCommandLists(1,lists);scene.Wait();
        }
        for(UINT frame=1;frame<=18;++frame){
            Frame(scene,frame,2,false,frame%3==0);Pixels(scene,Capture(scene,true,true),frame,true);const auto pixels=Capture(scene);
            for(UINT y=0;y<scene.height;++y)for(UINT x=0;x<scene.width;++x){const auto* p=pixels.data()+(size_t(y)*scene.width*2+scene.width+x)*4;Check(std::abs(int(p[0])-32)<=1 && std::abs(int(p[1])-64)<=1 && std::abs(int(p[2])-191)<=1 && p[3]==255,"recorded shadow commands never overwrite the GPU sentinel");}
            scene.DebugCheck();
        }
    }
    vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));Check(!result.mirrorReplays && !result.mirrorStops && !result.hookFailures && !result.mirrorLiveObjects && result.mirrorPipelines>=2 && result.mirrorResources>0,"record-only diagnostics clone and retire without reporting stereo replay");std::puts("PASS: native DX12 record-only diagnostics; native frame pixels preserved, shadow GPU sentinel unchanged, zero replay and complete cleanup");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
