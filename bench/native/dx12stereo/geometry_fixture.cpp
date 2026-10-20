// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Geometry-stage scaling exposes an incorrectly applied vertex-stage offset.
#include "mirror_scene.h"
#include "dxbc_patch.h"
namespace {
constexpr char geometrySource[]=R"(
cbuffer Frame:register(b0){float clipW;float3 color;};
struct Input{float4 position:SV_Position;};
struct Output{float4 position:SV_Position;float2 uv:TEXCOORD0;};
[maxvertexcount(3)]void GS(triangle Input input[3],inout TriangleStream<Output> stream){
 if(input[0].position.w<0)return;
 for(uint i=0;i<3;++i){
  Output o;o.position=input[i].position;o.position.x*=0.5;o.uv=float2(0.5,1);
  if(i==1)stream.Append(o);else stream.Append(o);
 }stream.RestartStrip();
}
float4 PS(Output input):SV_Target{return float4(color*float3(input.uv.x*2,1,input.uv.y),1);}
)";
void PipelineWithGeometry(Scene& scene,bool stream){
    ComPtr<ID3DBlob> vs,gs,ps,error;Hr(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"VS","vs_5_1",0,0,&vs,&error));
    Hr(D3DCompile(geometrySource,sizeof(geometrySource),nullptr,nullptr,nullptr,"GS",stream?"gs_5_1":"gs_5_0",D3DCOMPILE_SKIP_OPTIMIZATION|D3DCOMPILE_DEBUG,0,&gs,&error));
    Hr(D3DCompile(geometrySource,sizeof(geometrySource),nullptr,nullptr,nullptr,"PS","ps_5_1",0,0,&ps,&error));
    const auto patched=vrx::dx12::dxbc::Rewrite(gs->GetBufferPointer(),gs->GetBufferSize(),{.4f,0,true});
    if(patched.status!=vrx::dx12::dxbc::Status::Patched)throw std::runtime_error(patched.reason);
    Check(vrx::dx12::dxbc::ValidChecksum(patched.bytes.data(),patched.bytes.size()),"rewritten geometry shader has a valid checksum");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=scene.root.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.GS={gs->GetBufferPointer(),gs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};p.SampleMask=UINT_MAX;p.BlendState.RenderTarget[0].RenderTargetWriteMask=15;
    p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;p.SampleDesc.Count=1;
    if(stream)GraphicsStream(scene.device.Get(),p,scene.pipeline);else Hr(scene.device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&scene.pipeline)));
}
}
int main(int argc,char** argv){try {
    const bool warp=argc==3 && !std::strcmp(argv[2],"--warp"),stream=argc>=2 && !std::strcmp(argv[1],"--modern");Check(argc==2 || warp,"known geometry fixture adapter");
    const auto module=GetModuleHandleW(L"d3d12.dll");counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));Check(counts && twin && scope,"native geometry diagnostics loaded");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    {
        ComPtr<IDXGIAdapter> adapter;if(warp){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        Scene scene;scene.Init(adapter.Get());PipelineWithGeometry(scene,stream);
        for(UINT frame=1;frame<=6;++frame){
            const float w=frame==6?1.f:frame%2?2.f:4.f;Frame(scene,frame,w,false,frame%3==0);const auto disparity=Pixels(scene,Capture(scene),frame,w==1);
            if(w!=1)Check(std::abs(disparity-(w==2?9.6:4.8))<.7,"stereo is applied after geometry scaling at every emission, without double-shifting the vertex stage");
        }
    }
    vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));Check(result.mirrorReplays==6 && !result.mirrorStops && !result.hookFailures && !result.mirrorLiveObjects && (!stream || result.streamPipelines==1),"geometry-stage replay retires cleanly through the requested factory");
    std::puts("PASS: native DX12 geometry-stage emissions, final-stage disparity/depth, varyings, W=1 HUD and native/stream factories");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
