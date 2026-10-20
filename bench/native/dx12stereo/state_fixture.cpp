// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Dynamic blend/stencil must affect actual native and replayed triangle pixels.
#include "mirror_scene.h"
int main(int argc,char** argv){try {
    const bool warp=argc==3 && !std::strcmp(argv[2],"--warp");Check(argc==2 || warp,"known output-state fixture adapter");
    const auto module=GetModuleHandleW(L"d3d12.dll");counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));Check(counts && twin && scope,"native output-state diagnostics loaded");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    {
        ComPtr<IDXGIAdapter> adapter;if(warp){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        Scene scene;scene.Init(adapter.Get());
        ComPtr<ID3DBlob> vs,ps,error;Hr(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"VS","vs_5_1",0,0,&vs,&error));Hr(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"PS","ps_5_1",0,0,&ps,&error));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=scene.root.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};p.SampleMask=UINT_MAX;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
        auto& blend=p.BlendState.RenderTarget[0];blend.BlendEnable=TRUE;blend.SrcBlend=D3D12_BLEND_BLEND_FACTOR;blend.DestBlend=D3D12_BLEND_ZERO;blend.BlendOp=D3D12_BLEND_OP_ADD;blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.RenderTargetWriteMask=15;
        p.DepthStencilState.StencilEnable=TRUE;p.DepthStencilState.StencilReadMask=p.DepthStencilState.StencilWriteMask=255;
        D3D12_DEPTH_STENCILOP_DESC stencil{D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_EQUAL};p.DepthStencilState.FrontFace=p.DepthStencilState.BackFace=stencil;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;p.DSVFormat=DXGI_FORMAT_D24_UNORM_S8_UINT;p.SampleDesc.Count=1;
        Hr(scene.device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&scene.pipeline)));
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=scene.width;desc.Height=scene.height;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=p.DSVFormat;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};clear.Format=p.DSVFormat;clear.DepthStencil.Depth=1;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> depth;Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&depth)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> dsv;Hr(scene.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv)));scene.device->CreateDepthStencilView(depth.Get(),nullptr,dsv->GetCPUDescriptorHandleForHeapStart());
        for(UINT frame=1;frame<=4;++frame){
            scene.Wait();Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),scene.pipeline.Get()));scene.commands->SetGraphicsRootSignature(scene.root.Get());
            const float values[]={2,1,1,1};scene.commands->SetGraphicsRoot32BitConstants(0,4,values,0);
            D3D12_VIEWPORT viewport{0,0,float(scene.width),float(scene.height),0,1};D3D12_RECT rect{0,0,LONG(scene.width),LONG(scene.height)};scene.commands->RSSetViewports(1,&viewport);scene.commands->RSSetScissorRects(1,&rect);scene.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const auto target=scene.rtv->GetCPUDescriptorHandleForHeapStart(),stencilView=dsv->GetCPUDescriptorHandleForHeapStart();const float black[]={0,0,0,1};scene.commands->ClearRenderTargetView(target,black,0,nullptr);scene.commands->ClearDepthStencilView(stencilView,D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,1,UINT8(frame),0,nullptr);scene.commands->OMSetRenderTargets(1,&target,FALSE,&stencilView);
            const float wrong[]={.9f,.1f,.2f,1};scene.commands->OMSetStencilRef(frame+1);scene.commands->OMSetBlendFactor(wrong);scene.commands->DrawInstanced(3,1,0,0);
            const float factor[]={float(frame+1)/32,.5f,.75f,1};scene.commands->OMSetStencilRef(frame);scene.commands->OMSetBlendFactor(factor);scene.commands->DrawInstanced(3,1,0,0);
            Hr(scene.commands->Close());ID3D12CommandList* lists[]={scene.commands.Get()};scene.queue->ExecuteCommandLists(1,lists);scene.Wait();Check(Pixels(scene,Capture(scene),frame)>7,"dynamic blend and stencil preserve current-frame geometry stereo");scene.DebugCheck();
        }
    }
    vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));Check(result.mirrorReplays==4 && result.draws==8 && !result.mirrorStops && !result.hookFailures && !result.mirrorLiveObjects,"dynamic state replays and retires without errors");
    std::puts("PASS: native DX12 dynamic blend factor and stencil reference affect both native and replayed GPU pixels");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
