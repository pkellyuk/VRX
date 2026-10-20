// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// GPU-only channel conversion. Copy to a private typeless source first, so a
// game's backbuffer need not allow shader views. UNORM views preserve encoded
// sRGB bytes; this pass performs no transfer-function or colour-space change.
#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
namespace xg::dx12 {
class FormatConvert {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12RootSignature> root_;
    Ptr<ID3D12PipelineState> pipeline_;
    DXGI_FORMAT storage_=DXGI_FORMAT_UNKNOWN,view_=DXGI_FORMAT_UNKNOWN;
    UINT opaque_=0;
    UINT quantize_=0;
    static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};return b;
    }
public:
    struct Slot {Ptr<ID3D12Resource> source;Ptr<ID3D12DescriptorHeap> heap;UINT width=0,height=0,stride=0;};
    static bool Needed(DXGI_FORMAT f) {
        return f==DXGI_FORMAT_R10G10B10A2_UNORM || f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f==DXGI_FORMAT_B8G8R8X8_UNORM || f==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    }
    static bool Supported(DXGI_FORMAT f) {return f==DXGI_FORMAT_R8G8B8A8_UNORM || f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || Needed(f);}
    // The transport carries SDR encoded Rec.709 values. HDR/linear/wide-gamut
    // presentation needs an explicit colour transform before publication.
    static bool PresentationSupported(DXGI_FORMAT f,DXGI_COLOR_SPACE_TYPE color){return Supported(f) && color==DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;}
    bool Create(ID3D12Device* device,DXGI_FORMAT format) {
        if(!device || !Needed(format) || pipeline_)return false;
        opaque_=format==DXGI_FORMAT_B8G8R8X8_UNORM || format==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        storage_=opaque_?DXGI_FORMAT_B8G8R8X8_TYPELESS:DXGI_FORMAT_B8G8R8A8_TYPELESS;
        view_=opaque_?DXGI_FORMAT_B8G8R8X8_UNORM:DXGI_FORMAT_B8G8R8A8_UNORM;
        if(format==DXGI_FORMAT_R10G10B10A2_UNORM){storage_=DXGI_FORMAT_R10G10B10A2_TYPELESS;view_=DXGI_FORMAT_R10G10B10A2_UNORM;}
        quantize_=format==DXGI_FORMAT_R10G10B10A2_UNORM;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT sourceSupport{view_},targetSupport{DXGI_FORMAT_R8G8B8A8_UNORM};
        if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&sourceSupport,sizeof(sourceSupport))) ||
            FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&targetSupport,sizeof(targetSupport))) ||
            !(sourceSupport.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD) || !(targetSupport.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))return false;
        constexpr char shader[]=R"(
cbuffer Size:register(b0){uint width;uint height;uint opaque;uint quantize;};
Texture2DArray<float4> source:register(t0);RWTexture2DArray<float4> output:register(u0);
[numthreads(8,8,1)]void CS(uint3 id:SV_DispatchThreadID){
 if(id.x>=width || id.y>=height || id.z>=2)return;
 float4 color=source.Load(int4(id,0));if(opaque)color.a=1;
 if(quantize)color=floor(saturate(color)*255.0+0.5)/255.0;output[id]=color;
}
)";
        Ptr<ID3DBlob> cs,serialized,error;
        if(FAILED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"CS","cs_5_1",D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&cs,&error)))return false;
        D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
        D3D12_ROOT_PARAMETER p[3]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants.Num32BitValues=4;
        for(UINT i=0;i<2;++i){p[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[i+1].DescriptorTable={1,&ranges[i]};}
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=3;desc.pParameters=p;
        if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) ||
            FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_))))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root_.Get();pipeline.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
        return SUCCEEDED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&pipeline_)));
    }
    bool CreateSlot(ID3D12Device* device,ID3D12Resource* output,Slot& slot) const {
        if(!device || !output || !pipeline_)return false;auto desc=output->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize!=2 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1 ||
            desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM || !(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
        desc.Format=storage_;desc.Flags=D3D12_RESOURCE_FLAG_NONE;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&slot.source))))return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hd.NumDescriptors=2;
        if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&slot.heap))))return false;
        slot.width=UINT(desc.Width);slot.height=desc.Height;slot.stride=device->GetDescriptorHandleIncrementSize(hd.Type);
        auto cpu=slot.heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=view_;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;srv.Texture2DArray.ArraySize=2;srv.Texture2DArray.MipLevels=1;
        device->CreateShaderResourceView(slot.source.Get(),&srv,cpu);cpu.ptr+=slot.stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R8G8B8A8_UNORM;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;uav.Texture2DArray.ArraySize=2;
        device->CreateUnorderedAccessView(output,nullptr,&uav,cpu);return true;
    }
    // Both source slices were copied and are in COPY_DEST; the shared target is
    // COMMON. The ring owner calls this only after this slot's last GPU use.
    void Record(ID3D12GraphicsCommandList* commands,const Slot& slot,ID3D12Resource* output) const {
        auto source=Barrier(slot.source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto target=Barrier(output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);commands->ResourceBarrier(1,&source);commands->ResourceBarrier(1,&target);
        auto* heap=slot.heap.Get();commands->SetDescriptorHeaps(1,&heap);commands->SetComputeRootSignature(root_.Get());commands->SetPipelineState(pipeline_.Get());
        const UINT size[]={slot.width,slot.height,opaque_,quantize_};commands->SetComputeRoot32BitConstants(0,4,size,0);
        auto gpu=slot.heap->GetGPUDescriptorHandleForHeapStart();commands->SetComputeRootDescriptorTable(1,gpu);gpu.ptr+=slot.stride;commands->SetComputeRootDescriptorTable(2,gpu);
        commands->Dispatch((slot.width+7)/8,(slot.height+7)/8,2);
        source=Barrier(slot.source.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        target=Barrier(output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);commands->ResourceBarrier(1,&source);commands->ResourceBarrier(1,&target);
    }
};
} // namespace xg::dx12
