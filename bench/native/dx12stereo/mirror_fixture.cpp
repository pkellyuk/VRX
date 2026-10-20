// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// The scene records ONE native view. Only diagnostics know about the replay.
#include "test_scene.h"
#include "interceptor.h"
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
using namespace vrx::dx12::fixture;
namespace {
using CountsFn=HRESULT(WINAPI*)(vrx::dx12::Counts*,UINT);
using TwinFn=HRESULT(WINAPI*)(ID3D12Resource*,ID3D12Resource**);
using ScopeFn=void(WINAPI*)(BOOL);
CountsFn counts=nullptr; TwinFn twin=nullptr; ScopeFn scope=nullptr;
struct Diagnostic { Diagnostic(){scope(TRUE);} ~Diagnostic(){scope(FALSE);} };
constexpr char source[]=R"(
cbuffer Frame:register(b0){float clipW;float3 color;};
float4 VS(uint id:SV_VertexID):SV_Position {
 float2 p[3]={float2(-0.35,-0.5),float2(0,0.5),float2(0.35,-0.5)};
 return float4(p[id]*clipW,0.5*clipW,clipW);
}
float4 PS():SV_Target{return float4(color,1);}
)";
void Pipeline(Scene& scene) {
    ComPtr<ID3DBlob> vs,ps,error;
    Hr(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"VS","vs_5_1",0,0,&vs,&error));
    Hr(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"PS","ps_5_1",0,0,&ps,&error));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature=scene.root.Get(); p.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    p.SampleMask=UINT_MAX; p.BlendState.RenderTarget[0].RenderTargetWriteMask=15;
    p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable=TRUE;
    p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; p.NumRenderTargets=1; p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; p.SampleDesc.Count=1;
    Hr(scene.device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&scene.pipeline)));
}
void Frame(Scene& scene,UINT number,float w,bool stop=false,bool indexed=false) {
    scene.Wait(); Hr(scene.allocator->Reset()); Hr(scene.commands->Reset(scene.allocator.Get(),scene.pipeline.Get()));
    scene.commands->SetGraphicsRootSignature(scene.root.Get());
    D3D12_VIEWPORT viewport{0,0,float(scene.width),float(scene.height),0,1}; D3D12_RECT rect{0,0,LONG(scene.width),LONG(scene.height)};
    scene.commands->RSSetViewports(1,&viewport); scene.commands->RSSetScissorRects(1,&rect); scene.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto target=scene.rtv->GetCPUDescriptorHandleForHeapStart();
    const float black[]={0,0,0,1}; scene.commands->ClearRenderTargetView(target,black,0,nullptr); scene.commands->OMSetRenderTargets(1,&target,FALSE,nullptr);
    ComPtr<ID3D12DescriptorHeap> unsupported;
    if(stop) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; desc.NumDescriptors=1; desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Hr(scene.device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&unsupported))); auto* heap=unsupported.Get(); scene.commands->SetDescriptorHeaps(1,&heap);
    }
    const float values[]={w,float(number+1)/32,.5f,.75f}; scene.commands->SetGraphicsRoot32BitConstants(0,4,values,0);
    ComPtr<ID3D12Resource> indices;
    if(indexed) {
        D3D12_HEAP_PROPERTIES upload{}; upload.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=16; d.Height=d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hr(scene.device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&indices)));
        void* mapped=nullptr; Hr(indices->Map(0,nullptr,&mapped)); const UINT data[]={99,0,1,2}; std::memcpy(mapped,data,sizeof(data)); indices->Unmap(0,nullptr);
        D3D12_INDEX_BUFFER_VIEW view{indices->GetGPUVirtualAddress()+4,12,DXGI_FORMAT_R32_UINT}; scene.commands->IASetIndexBuffer(&view);
        scene.commands->DrawIndexedInstanced(3,1,0,0,0);
    } else scene.commands->DrawInstanced(3,1,0,0);
    Hr(scene.commands->Close()); ID3D12CommandList* commands[]={scene.commands.Get()}; scene.queue->ExecuteCommandLists(1,commands); scene.Wait();
}
std::vector<uint8_t> Capture(Scene& scene,bool expected=true,bool native=false) {
    Diagnostic diagnostic;
    ComPtr<ID3D12Resource> right;
    auto hr=native?E_FAIL:twin(scene.eyes.Get(),&right);
    if(native) right=scene.eyes;
    else if(expected) Hr(hr); else { Check(FAILED(hr) && !right,"stopped replay exposes no valid twin"); right=scene.eyes; }
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> commands;
    Hr(scene.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    Hr(scene.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));
    const auto desc=scene.eyes->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint[2]{}; UINT64 bytes=0;
    scene.device->GetCopyableFootprints(&desc,0,2,0,footprint,nullptr,nullptr,&bytes);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=bytes;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback; Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    ID3D12Resource* textures[]={scene.eyes.Get(),right.Get()};
    for(UINT eye=0;eye<2;++eye) {
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={textures[eye],0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE}; commands->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=textures[eye]; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint[eye]; commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter); commands->ResourceBarrier(1,&b);
    }
    Hr(commands->Close()); ID3D12CommandList* lists[]={commands.Get()}; scene.queue->ExecuteCommandLists(1,lists); scene.Wait();
    const uint8_t* mapped=nullptr; Hr(readback->Map(0,nullptr,reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped))));
    std::vector<uint8_t> result(size_t(scene.width)*scene.height*8);
    for(UINT eye=0;eye<2;++eye) for(UINT y=0;y<scene.height;++y)
        std::memcpy(result.data()+(size_t(y)*scene.width*2+eye*scene.width)*4,mapped+footprint[eye].Offset+size_t(y)*footprint[eye].Footprint.RowPitch,scene.width*4);
    readback->Unmap(0,nullptr); scene.DebugCheck(); return result;
}
double Pixels(Scene& scene,const std::vector<uint8_t>& pixels,UINT number,bool identical=false) {
    uint64_t sum[2]{},count[2]{};
    for(UINT eye=0;eye<2;++eye) for(UINT y=0;y<scene.height;++y) for(UINT x=0;x<scene.width;++x) {
        const auto* p=pixels.data()+(size_t(y)*scene.width*2+eye*scene.width+x)*4;
        Check(p[3]==255,"eye alpha preserved");
        if(p[1]>100) {
            Check(std::abs(int(p[0])-int((number+1)*255/32))<=1 && std::abs(int(p[1])-128)<=1 && std::abs(int(p[2])-191)<=1,"both eyes contain the current simulation frame color");
            sum[eye]+=x; ++count[eye];
        }
        if(identical && !eye) Check(!std::memcmp(p,p+scene.width*4,4),"HUD or native fallback pixels are identical");
    }
    Check(count[0]>100 && count[1]>100,"both eye triangles are populated");
    if(std::abs(double(sum[0])/count[0]-47.5)>=.1) throw std::runtime_error("original centroid at frame "+std::to_string(number)+": "+std::to_string(double(sum[0])/count[0]));
    return double(sum[1])/count[1]-double(sum[0])/count[0];
}
} // namespace
int main(int argc,char** argv) {
    try {
        bool unsupported=argc>1 && std::strcmp(argv[1],"--unsupported")==0;
        bool warp=argc>2 && std::strcmp(argv[2],"--warp")==0;
        const auto module=GetModuleHandleW(L"d3d12.dll");
        counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts")); twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin")); scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));
        Check(counts && twin && scope,"local native mirror bridge loaded");
        ComPtr<ID3D12Debug> debug; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        double distance[2]{};
        {
            ComPtr<IDXGIAdapter> adapter; if(warp){ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
            Scene scene; scene.Init(adapter.Get()); Pipeline(scene);
            { Diagnostic native; Frame(scene,0,2,false,true); Pixels(scene,Capture(scene,true,true),0,true); }
            for(UINT frame=1;frame<=18;++frame) {
                float w=frame>=17?1.0f:frame%2?2.0f:4.0f; Frame(scene,frame,w,false,frame%3==0);
                auto disparity=Pixels(scene,Capture(scene),frame,w==1);
                if(w!=1) { Check(disparity>(w==2?7:3),"replayed shader produces real geometry disparity"); distance[frame%2]=disparity; }
            }
            Check(distance[1]/distance[0]>1.7 && distance[1]/distance[0]<2.3,"replay disparity decreases with depth");
            if(unsupported) { Frame(scene,19,2,true); Pixels(scene,Capture(scene,false),19,true); }
        }
        vrx::dx12::Counts result; Hr(counts(&result,sizeof(result)));
        Check(result.version==2 && !result.hookFailures && result.mirrorReplays==18 && result.mirrorResources>=1 && result.mirrorPipelines>=2,"one replay per app submission, with isolated resources and patched pipelines");
        Check(result.mirrorStops==(unsupported?1u:0u),"unsupported commands stop only stereo replay");
        Check(result.draws==(unsupported?13u:12u) && result.indexedDraws==6,"the app issues one draw per simulation frame");
        Check(!result.mirrorLiveObjects,"all original identities and shadow objects retire after app release");
        std::printf("PASS: native DX12 command/resource mirror; 18 same-frame replays, disparity %.2f/%.2f; original output, HUD, lifetime cleanup%s\n",distance[1],distance[0],unsupported?", unsupported-command fallback":"");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
