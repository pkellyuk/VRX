// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Dynamic blend/stencil must affect actual native and replayed triangle pixels.
#include "mirror_scene.h"
int main(int argc,char** argv){try {
    const bool warp=argc==3 && !std::strcmp(argv[2],"--warp");Check(argc==2 || warp,"known output-state fixture adapter");
    const bool nullProbe=!std::strcmp(argv[1],"--null-blend") || !std::strcmp(argv[1],"--null-blend-no-debug");
    const bool latePipeline=!std::strcmp(argv[1],"--late-pipeline");
    wchar_t diagnosticMode[3]{};const bool recordOnly=GetEnvironmentVariableW(L"VRX_DX12_RECORD_ONLY",diagnosticMode,3)==1 && diagnosticMode[0]==L'1';
    const bool depthBounds=!std::strcmp(argv[1],"--depth-bounds");
    const auto module=GetModuleHandleW(L"d3d12.dll");counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));if(!nullProbe)Check(counts && twin && scope,"native output-state diagnostics loaded");
    ComPtr<ID3D12Debug> debug;if(std::strcmp(argv[1],"--null-blend-no-debug") && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    {
        ComPtr<IDXGIAdapter> adapter;if(warp){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        Scene scene;scene.Init(adapter.Get());
        wchar_t debugMode[3]{};
        if(GetEnvironmentVariableW(L"VRX_DX12_DEBUG_LAYER",debugMode,3)==1 && debugMode[0]==L'1' && scene.messages){
            ComPtr<ID3D12Device> repeated;Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&repeated)));
            Check(repeated.Get()==scene.device.Get(),"debug fixture recreates the same singleton device");
            Hr(scene.messages->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING,"VRX diagnostic application-message probe"));
            for(UINT i=0;i<4200;++i)Hr(scene.messages->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING,"VRX diagnostic warning flood"));
            Hr(scene.messages->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR,"VRX diagnostic error after warning flood"));scene.messages->ClearStoredMessages();
        }
        if(nullProbe){
            Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),nullptr));
            std::puts("Native NULL blend call: begin");std::fflush(stdout);
            scene.commands->OMSetBlendFactor(nullptr);
            std::puts("Native NULL blend call: returned");std::fflush(stdout);
            Hr(scene.commands->Close());scene.DebugCheck();std::puts("PASS: native DX12 NULL blend input accepted");return 0;
        }
        if(depthBounds){D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};Hr(scene.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2,&options,sizeof(options)));if(!options.DepthBoundsTestSupported){std::puts("SKIP: native DX12 adapter has no depth-bounds test");return 77;}}
        ComPtr<ID3DBlob> vs,ps,error;Hr(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"VS","vs_5_1",0,0,&vs,&error));Hr(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"PS","ps_5_1",0,0,&ps,&error));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=scene.root.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};p.SampleMask=UINT_MAX;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
        auto& blend=p.BlendState.RenderTarget[0];blend.BlendEnable=TRUE;blend.SrcBlend=D3D12_BLEND_BLEND_FACTOR;blend.DestBlend=D3D12_BLEND_ZERO;blend.BlendOp=D3D12_BLEND_OP_ADD;blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.RenderTargetWriteMask=15;
        p.DepthStencilState.StencilEnable=TRUE;p.DepthStencilState.StencilReadMask=p.DepthStencilState.StencilWriteMask=255;
        D3D12_DEPTH_STENCILOP_DESC stencil{D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_EQUAL};p.DepthStencilState.FrontFace=p.DepthStencilState.BackFace=stencil;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;p.DSVFormat=DXGI_FORMAT_D24_UNORM_S8_UINT;p.SampleDesc.Count=1;
        if(ModernMode())GraphicsStream(scene.device.Get(),p,scene.pipeline);else Hr(scene.device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&scene.pipeline)));
        if(depthBounds){
            using namespace vrx::dx12::stream;
            D3D12_DEPTH_STENCIL_DESC1 depth{TRUE,D3D12_DEPTH_WRITE_MASK_ZERO,D3D12_COMPARISON_FUNC_ALWAYS,TRUE,255,255,stencil,stencil,TRUE};
            D3D12_RT_FORMAT_ARRAY formats{};formats.NumRenderTargets=1;formats.RTFormats[0]=p.RTVFormats[0];
            struct {Subobject<ID3D12RootSignature*> root;Subobject<D3D12_SHADER_BYTECODE> vs,ps;Subobject<D3D12_BLEND_DESC> blend;Subobject<UINT> mask;Subobject<D3D12_RASTERIZER_DESC> raster;Subobject<D3D12_DEPTH_STENCIL_DESC1> depth;Subobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;Subobject<D3D12_RT_FORMAT_ARRAY> formats;Subobject<DXGI_FORMAT> dsv;Subobject<DXGI_SAMPLE_DESC> samples;} stream{
                {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,p.pRootSignature},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,p.VS},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,p.PS},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,p.BlendState},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,p.SampleMask},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,p.RasterizerState},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1,depth},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,p.PrimitiveTopologyType},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,formats},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,p.DSVFormat},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,p.SampleDesc}};
            ComPtr<ID3D12Device2> newer;Hr(scene.device.As(&newer));D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream),&stream};scene.pipeline.Reset();Hr(newer->CreatePipelineState(&desc,IID_PPV_ARGS(&scene.pipeline)));
        }
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=scene.width;desc.Height=scene.height;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=p.DSVFormat;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};clear.Format=p.DSVFormat;clear.DepthStencil.Depth=1;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> depth;Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&depth)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> dsv;Hr(scene.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv)));scene.device->CreateDepthStencilView(depth.Get(),nullptr,dsv->GetCPUDescriptorHandleForHeapStart());
        for(UINT frame=1;frame<=4;++frame){
            scene.Wait();Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),latePipeline?nullptr:scene.pipeline.Get()));
            if(latePipeline)scene.commands->SetPipelineState(scene.pipeline.Get());
            scene.commands->SetGraphicsRootSignature(scene.root.Get());
            const float values[]={2,frame==4?float(frame+1)/32:1.f,frame==4?.5f:1.f,frame==4?.75f:1.f};scene.commands->SetGraphicsRoot32BitConstants(0,4,values,0);
            ComPtr<ID3D12GraphicsCommandList1> extended;if(depthBounds){Hr(scene.commands.As(&extended));extended->OMSetDepthBounds(0,1);}
            D3D12_VIEWPORT viewport{0,0,float(scene.width),float(scene.height),0,1};D3D12_RECT rect{0,0,LONG(scene.width),LONG(scene.height)};scene.commands->RSSetViewports(1,&viewport);scene.commands->RSSetScissorRects(1,&rect);scene.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const auto target=scene.rtv->GetCPUDescriptorHandleForHeapStart(),stencilView=dsv->GetCPUDescriptorHandleForHeapStart();const float black[]={0,0,0,1};scene.commands->ClearRenderTargetView(target,black,0,nullptr);scene.commands->ClearDepthStencilView(stencilView,D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,depthBounds?.5f:1,UINT8(frame),0,nullptr);scene.commands->OMSetRenderTargets(1,&target,FALSE,&stencilView);
            const float wrong[]={.9f,.1f,.2f,1};scene.commands->OMSetStencilRef(frame+1);scene.commands->OMSetBlendFactor(wrong);scene.commands->DrawInstanced(3,1,0,0);
            float factor[]={float(frame+1)/32,.5f,.75f,1};scene.commands->OMSetStencilRef(frame);
            // NULL resets to ones; the caller may reuse array storage as soon
            // as the call returns. Both native and replayed pixels must agree.
            scene.commands->OMSetBlendFactor(frame==4?nullptr:factor);
            Check(factor[0]==float(frame+1)/32 && factor[1]==.5f && factor[2]==.75f && factor[3]==1,"blend recording never modifies caller memory");
            std::fill(factor,factor+4,0.f);scene.commands->DrawInstanced(3,1,0,0);
            if(depthBounds){extended->OMSetDepthBounds(frame%2?.75f:0,frame%2?1:.25f);scene.commands->OMSetBlendFactor(wrong);scene.commands->DrawInstanced(3,1,0,0);}
            Hr(scene.commands->Close());ID3D12CommandList* lists[]={scene.commands.Get()};scene.queue->ExecuteCommandLists(1,lists);scene.Wait();const auto disparity=Pixels(scene,Capture(scene,true,recordOnly),frame,recordOnly);if(!recordOnly)Check(disparity>7,"dynamic blend and stencil preserve current-frame geometry stereo");scene.DebugCheck();
        }
    }
    vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));Check(result.mirrorReplays==(recordOnly?0u:4u) && result.draws==(depthBounds?12u:8u) && !result.mirrorStops && !result.hookFailures && !result.mirrorLiveObjects,"dynamic state replays and retires without errors");
    if(ModernMode())Check(result.streamPipelines>0,"output-state fixture exercised native pipeline-stream creation");
    std::puts("PASS: native DX12 dynamic blend factor and stencil reference affect both native and replayed GPU pixels");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
