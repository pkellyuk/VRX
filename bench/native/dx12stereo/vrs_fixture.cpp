// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Actual shading granularity, image bindings and resets must affect both eyes.
#include "mirror_scene.h"
namespace {
constexpr char shader[]=R"(
cbuffer Frame:register(b0){float clipW;float3 frameColor;};
float4 VS(uint id:SV_VertexID):SV_Position {
 float2 p[6]={float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
 return float4(p[id]*clipW,0.5*clipW,clipW);
}
float4 PS(float4 p:SV_Position):SV_Target {
 return float4(frameColor.x,uint(p.x)&1,uint(p.y)&1,1);
}
)";
void Transition(Scene& scene,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};scene.commands->ResourceBarrier(1,&b);
}
}
int main(int argc,char** argv){try {
    const bool warp=argc==3 && !std::strcmp(argv[2],"--warp");Check(argc==2 || warp,"known shading-rate fixture adapter");
    const auto module=GetModuleHandleW(L"d3d12.dll");counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));Check(counts && twin && scope,"native shading-rate diagnostics loaded");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    {
        ComPtr<IDXGIAdapter> adapter;if(warp){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        Scene scene;scene.Init(adapter.Get());D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};Hr(scene.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,&options,sizeof(options)));
        if(options.VariableShadingRateTier<D3D12_VARIABLE_SHADING_RATE_TIER_2){std::puts("SKIP: native DX12 adapter has no Tier2 variable-rate shading");return 77;}
        Check(options.ShadingRateImageTileSize>0,"valid shading-rate tile size");
        ComPtr<ID3DBlob> vs,ps,error;Hr(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"VS","vs_5_1",D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&vs,&error));Hr(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PS","ps_5_1",D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&ps,&error));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=scene.root.Get();pipeline.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pipeline.PS={ps->GetBufferPointer(),ps->GetBufferSize()};pipeline.SampleMask=UINT_MAX;pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pipeline.RasterizerState.DepthClipEnable=TRUE;pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=15;pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pipeline.SampleDesc.Count=1;Hr(scene.device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&scene.pipeline)));
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=(scene.width+options.ShadingRateImageTileSize-1)/options.ShadingRateImageTileSize;desc.Height=(scene.height+options.ShadingRateImageTileSize-1)/options.ShadingRateImageTileSize;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8_UINT;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> image;Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;scene.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;heap.Type=D3D12_HEAP_TYPE_UPLOAD;ComPtr<ID3D12Resource> upload;Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
        for(UINT frame=1;frame<=8;++frame){
            const UINT mode=(frame-1)%4;const bool coarse=mode==0 || mode==2;void* data=nullptr;Hr(upload->Map(0,nullptr,&data));std::memset(data,mode==1?D3D12_SHADING_RATE_1X1:D3D12_SHADING_RATE_2X2,size_t(bytes));upload->Unmap(0,nullptr);
            scene.Wait();Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),scene.pipeline.Get()));scene.commands->SetGraphicsRootSignature(scene.root.Get());const float values[]={2,float(frame+1)/16,0,0};scene.commands->SetGraphicsRoot32BitConstants(0,4,values,0);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;to.pResource=image.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;scene.commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);Transition(scene,image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
            ComPtr<ID3D12GraphicsCommandList5> extended;Hr(scene.commands.As(&extended));D3D12_SHADING_RATE_COMBINER combine[]={D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_OVERRIDE};extended->RSSetShadingRate(D3D12_SHADING_RATE_1X1,combine);extended->RSSetShadingRateImage(image.Get());
            if(mode==2){extended->RSSetShadingRate(D3D12_SHADING_RATE_2X2,nullptr);extended->RSSetShadingRateImage(nullptr);}else if(mode==3)extended->RSSetShadingRateImage(nullptr);
            D3D12_VIEWPORT viewport{0,0,float(scene.width),float(scene.height),0,1};D3D12_RECT rect{0,0,LONG(scene.width),LONG(scene.height)};scene.commands->RSSetViewports(1,&viewport);scene.commands->RSSetScissorRects(1,&rect);scene.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);const auto target=scene.rtv->GetCPUDescriptorHandleForHeapStart();const float black[]={0,0,0,1};scene.commands->ClearRenderTargetView(target,black,0,nullptr);scene.commands->OMSetRenderTargets(1,&target,FALSE,nullptr);scene.commands->DrawInstanced(6,1,0,0);extended->RSSetShadingRateImage(nullptr);Transition(scene,image.Get(),D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);Hr(scene.commands->Close());ID3D12CommandList* lists[]={scene.commands.Get()};scene.queue->ExecuteCommandLists(1,lists);scene.Wait();
            auto pixels=Capture(scene);for(UINT eye=0;eye<2;++eye){UINT transitions=0;for(UINT y=24;y<40;++y)for(UINT x=32;x<63;++x){const auto* p=pixels.data()+(size_t(y)*scene.width*2+eye*scene.width+x)*4;Check(std::abs(int(p[0])-int((frame+1)*255/16))<=1 && p[3]==255,"both eye interiors encode this simulation frame");if(p[1]!=p[5])++transitions;}Check(coarse?transitions<20:transitions>450,"base/image shading rates change actual native and replayed GPU granularity");}
            const auto offset=size_t(scene.height/2)*scene.width*8+8;Check(pixels[offset]>0 && pixels[offset+scene.width*4]==0,"native centering and shifted right-eye edge remain distinct");scene.DebugCheck();
        }
    }
    vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));Check(result.mirrorReplays==8 && !result.mirrorStops && !result.hookFailures && !result.mirrorLiveObjects,"shading-rate state replays and retires cleanly");std::puts("PASS: native DX12 variable-rate shading; base rates, image rates, null/default resets, same-frame GPU pixels and geometry disparity");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
