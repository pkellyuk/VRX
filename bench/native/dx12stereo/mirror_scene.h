// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// The scene records ONE native view. Only diagnostics know about the replay.
#pragma once
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
#ifdef BINDINGS
Buffer<float4> colors:register(t0);
StructuredBuffer<float4> rootColor:register(t1);
cbuffer Tint:register(b1){float4 tint;};
cbuffer Gain:register(b2){float4 gain;};
#endif
float4 VS(uint id:SV_VertexID):SV_Position {
 float2 p[3]={float2(-0.35,-0.5),float2(0,0.5),float2(0.35,-0.5)};
 return float4(p[id]*clipW,
#ifdef BINDINGS
 color.z
#else
 0.5
#endif
 *clipW,clipW);
}
float4 PS():SV_Target{
#ifdef BINDINGS
 return colors[0]*tint*gain*rootColor[0]*color.x;
#else
 return float4(color,1);
#endif
}
)";
struct Bindings {
    ComPtr<ID3D12Resource> buffer,depth,generated;
    ComPtr<ID3D12DescriptorHeap> staging,visible,dsv;
    ComPtr<ID3D12RootSignature> computeRoot;
    ComPtr<ID3D12PipelineState> computePipeline;
    UINT stride=0;
    void Init(Scene& scene,bool compute=false) {
        D3D12_ROOT_PARAMETER p[4]{};
        p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants.Num32BitValues=4;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,2,0,1};
        p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[1].DescriptorTable={2,ranges};
        p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;p[2].Descriptor.ShaderRegister=1;
        p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[3].Descriptor.ShaderRegister=1;
        D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=4;root.pParameters=p;
        ComPtr<ID3DBlob> blob,error;Hr(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
        Hr(scene.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&scene.root)));
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=1024;
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buffer)));
        if(compute) {
            heap.Type=D3D12_HEAP_TYPE_DEFAULT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&generated)));
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=5;
        Hr(scene.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&staging)));
        hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;Hr(scene.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&visible)));
        stride=scene.device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.FirstElement=16;srv.Buffer.NumElements=1;
        scene.device->CreateShaderResourceView(compute?generated.Get():buffer.Get(),&srv,Cpu(staging.Get(),1));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{buffer->GetGPUVirtualAddress(),256};scene.device->CreateConstantBufferView(&cbv,Cpu(staging.Get(),3));
        heap.Type=D3D12_HEAP_TYPE_DEFAULT;desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=scene.width;desc.Height=scene.height;
        desc.Format=DXGI_FORMAT_D32_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_CLEAR_VALUE clear{};clear.Format=desc.Format;clear.DepthStencil.Depth=1;
        Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&depth)));
        hd={};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;Hr(scene.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv)));
        scene.device->CreateDepthStencilView(depth.Get(),nullptr,dsv->GetCPUDescriptorHandleForHeapStart());
        if(compute) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.FirstElement=16;uav.Buffer.NumElements=1;uav.Buffer.StructureByteStride=16;
            scene.device->CreateUnorderedAccessView(generated.Get(),nullptr,&uav,Cpu(staging.Get(),4));
            D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};D3D12_ROOT_PARAMETER parameters[5]{};
            parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=4;
            parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};
            parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[3].Descriptor.ShaderRegister=1;
            parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[4].Descriptor.ShaderRegister=1;
            D3D12_ROOT_SIGNATURE_DESC computeDesc{};computeDesc.NumParameters=5;computeDesc.pParameters=parameters;
            Hr(D3D12SerializeRootSignature(&computeDesc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
            Hr(scene.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&computeRoot)));
            constexpr char cs[]=R"(
cbuffer Scale:register(b0){float4 scale;};
cbuffer Tint:register(b1){float4 tint;};
StructuredBuffer<float4> input:register(t0);
RWStructuredBuffer<float4> output:register(u0);
RWStructuredBuffer<float4> rootOutput:register(u1);
[numthreads(1,1,1)] void CS(){
 float4 value=input[0]*float4(scale.xyz,1)*tint*2;
 if(scale.w>0)output[0]=value;else rootOutput[0]=value;
}
)";
            ComPtr<ID3DBlob> shader;Hr(D3DCompile(cs,sizeof(cs),nullptr,nullptr,nullptr,"CS","cs_5_1",0,0,&shader,&error));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=computeRoot.Get();pipeline.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            Hr(scene.device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&computePipeline)));
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* heap,UINT index) const {
        auto handle=heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=SIZE_T(index)*stride;return handle;
    }
    void Bind(Scene& scene,UINT frame) {
        void* data=nullptr;Hr(buffer->Map(0,nullptr,&data));const float one[]={1,1,1,1},gain[]={2,2,2,2},tint[]={.5f,.5f,.5f,.5f},color[]={float(frame+1)/32,.5f,.75f,1};
        std::memcpy(data,gain,16);std::memcpy(static_cast<char*>(data)+256,color,16);
        std::memcpy(static_cast<char*>(data)+512,tint,16);std::memcpy(static_cast<char*>(data)+768,one,16);buffer->Unmap(0,nullptr);
        scene.device->CopyDescriptorsSimple(1,Cpu(visible.Get(),2),Cpu(staging.Get(),1),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto dst=Cpu(visible.Get(),3),src=Cpu(staging.Get(),3);const UINT size=1;
        scene.device->CopyDescriptors(1,&dst,frame%2?nullptr:&size,1,&src,frame%2?nullptr:&size,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto* shaderHeap=visible.Get();scene.commands->SetDescriptorHeaps(1,&shaderHeap);
        if(generated) {
            auto destination=Cpu(visible.Get(),4),sourceHandle=Cpu(staging.Get(),4);
            scene.device->CopyDescriptors(1,&destination,nullptr,1,&sourceHandle,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            scene.commands->SetComputeRootSignature(computeRoot.Get());scene.commands->SetPipelineState(computePipeline.Get());
            auto uav=visible->GetGPUDescriptorHandleForHeapStart();uav.ptr+=4*stride;scene.commands->SetComputeRootDescriptorTable(1,uav);
            scene.commands->SetComputeRootShaderResourceView(2,buffer->GetGPUVirtualAddress()+256);
            scene.commands->SetComputeRootUnorderedAccessView(3,generated->GetGPUVirtualAddress()+256);
            scene.commands->SetComputeRootConstantBufferView(4,buffer->GetGPUVirtualAddress()+512);
            scene.commands->SetComputeRoot32BitConstants(0,4,one,0);scene.commands->SetComputeRoot32BitConstant(0,frame%2?0x3f800000:0,3);
            scene.commands->Dispatch(1,1,1);
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={generated.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
            scene.commands->ResourceBarrier(1,&barrier);scene.commands->SetPipelineState(scene.pipeline.Get());
        }
        auto gpu=visible->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=2*stride;scene.commands->SetGraphicsRootDescriptorTable(1,gpu);
        scene.commands->SetGraphicsRootConstantBufferView(2,buffer->GetGPUVirtualAddress()+512);
        scene.commands->SetGraphicsRootShaderResourceView(3,buffer->GetGPUVirtualAddress()+768);
        auto target=scene.rtv->GetCPUDescriptorHandleForHeapStart(),depthView=dsv->GetCPUDescriptorHandleForHeapStart();
        scene.commands->ClearDepthStencilView(depthView,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        scene.commands->OMSetRenderTargets(1,&target,FALSE,&depthView);
    }
    void End(Scene& scene) {
        if(!generated)return;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={generated.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        scene.commands->ResourceBarrier(1,&barrier);
    }
};
inline void Pipeline(Scene& scene,bool bindings=false,DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM) {
    ComPtr<ID3DBlob> vs,ps,error;
    const D3D_SHADER_MACRO defines[]={{"BINDINGS","1"},{nullptr,nullptr}};
    Hr(D3DCompile(source,sizeof(source),nullptr,bindings?defines:nullptr,nullptr,"VS","vs_5_1",0,0,&vs,&error));
    Hr(D3DCompile(source,sizeof(source),nullptr,bindings?defines:nullptr,nullptr,"PS","ps_5_1",0,0,&ps,&error));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature=scene.root.Get(); p.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    p.SampleMask=UINT_MAX; p.BlendState.RenderTarget[0].RenderTargetWriteMask=15;
    p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable=TRUE;
    p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; p.NumRenderTargets=1; p.RTVFormats[0]=format; p.SampleDesc.Count=1;
    if(bindings) {p.DSVFormat=DXGI_FORMAT_D32_FLOAT;p.DepthStencilState.DepthEnable=TRUE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;}
    Hr(scene.device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&scene.pipeline)));
}
inline void Frame(Scene& scene,UINT number,float w,bool stop=false,bool indexed=false,Bindings* bindings=nullptr,bool presentation=false) {
    scene.Wait(); Hr(scene.allocator->Reset()); Hr(scene.commands->Reset(scene.allocator.Get(),scene.pipeline.Get()));
    scene.commands->SetGraphicsRootSignature(scene.root.Get());
    D3D12_VIEWPORT viewport{0,0,float(scene.width),float(scene.height),0,1}; D3D12_RECT rect{0,0,LONG(scene.width),LONG(scene.height)};
    scene.commands->RSSetViewports(1,&viewport); scene.commands->RSSetScissorRects(1,&rect); scene.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto target=scene.rtv->GetCPUDescriptorHandleForHeapStart();
    if(presentation) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={scene.eyes.Get(),0,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET};scene.commands->ResourceBarrier(1,&b);
    }
    const float black[]={0,0,0,1}; scene.commands->ClearRenderTargetView(target,black,0,nullptr); scene.commands->OMSetRenderTargets(1,&target,FALSE,nullptr);
    ComPtr<ID3D12DescriptorHeap> unsupported;
    if(stop) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; desc.NumDescriptors=1; desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Hr(scene.device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&unsupported))); auto* heap=unsupported.Get(); scene.commands->SetDescriptorHeaps(1,&heap);
        scene.commands->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
    const float values[]={w,float(number+1)/32,.5f,.75f}; scene.commands->SetGraphicsRoot32BitConstants(0,4,values,0);
    if(bindings) {bindings->Bind(scene,number);const float front[]={w,1,0,.5f};scene.commands->SetGraphicsRoot32BitConstants(0,4,front,0);}
    ComPtr<ID3D12Resource> indices;
    if(indexed) {
        D3D12_HEAP_PROPERTIES upload{}; upload.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=16; d.Height=d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hr(scene.device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&indices)));
        void* mapped=nullptr; Hr(indices->Map(0,nullptr,&mapped)); const UINT data[]={99,0,1,2}; std::memcpy(mapped,data,sizeof(data)); indices->Unmap(0,nullptr);
        D3D12_INDEX_BUFFER_VIEW view{indices->GetGPUVirtualAddress()+4,12,DXGI_FORMAT_R32_UINT}; scene.commands->IASetIndexBuffer(&view);
        scene.commands->DrawIndexedInstanced(3,1,0,0,0);
    } else scene.commands->DrawInstanced(3,1,0,0);
    if(bindings) {
        const float hidden[]={w,0,0,.75f};scene.commands->SetGraphicsRoot32BitConstants(0,4,hidden,0);
        scene.commands->DrawInstanced(3,1,0,0);
        bindings->End(scene);
    }
    if(presentation) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={scene.eyes.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT};scene.commands->ResourceBarrier(1,&b);
    }
    Hr(scene.commands->Close()); ID3D12CommandList* commands[]={scene.commands.Get()}; scene.queue->ExecuteCommandLists(1,commands); scene.Wait();
}
inline std::vector<uint8_t> Capture(Scene& scene,bool expected=true,bool native=false) {
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
inline double Pixels(Scene& scene,const std::vector<uint8_t>& pixels,UINT number,bool identical=false) {
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
    if(std::abs(double(sum[0])/count[0]-(double(scene.width)-1)/2)>=.1) throw std::runtime_error("original centroid at frame "+std::to_string(number)+": "+std::to_string(double(sum[0])/count[0]));
    return double(sum[1])/count[1]-double(sum[0])/count[0];
}
} // namespace
