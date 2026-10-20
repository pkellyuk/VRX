// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Fit a resized native stereo source into a stable headset presentation size.
#pragma once
#include "geometry_copy.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
class GeometryFit {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12RootSignature> root_;
    Ptr<ID3D12PipelineState> pipeline_;
    Ptr<ID3D12DescriptorHeap> heap_;
    Ptr<ID3D12Resource> output_;
    std::vector<Ptr<ID3D12Resource>> sources_;
    UINT iw_=0,ih_=0,ow_=0,oh_=0,stride_=0;
public:
    ID3D12Resource* Output() const {return output_.Get();}
    bool Create(ID3D12Device* device,ID3D12Resource* const* sources,UINT count,UINT width,UINT height) {
        if(!device || !sources || !count || count>64 || width<2 || height<2 || width>4096 || height>4096 || output_)return false;
        const auto first=sources[0]?sources[0]->GetDesc():D3D12_RESOURCE_DESC{};
        if(first.Width<4 || first.Width>8192 || first.Width%2 || first.Height<2 || first.Height>4096)return false;
        for(UINT i=0;i<count;++i) {
            if(!sources[i])return false;const auto d=sources[i]->GetDesc();
            if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=first.Width || d.Height!=first.Height || d.DepthOrArraySize!=1 ||
                d.MipLevels!=1 || d.SampleDesc.Count!=1 || d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM)return false;
        }
        constexpr char shader[]=R"(
cbuffer Size:register(b0){uint IW;uint IH;uint OW;uint OH;};
Texture2D<float4> source:register(t0);RWTexture2D<float4> output:register(u0);SamplerState linearClamp:register(s0);
[numthreads(8,8,1)] void CS(uint3 id:SV_DispatchThreadID){
 if(id.x>=OW || id.y>=OH || id.z>=2)return;
 float scale=min(float(OW)/IW,float(OH)/IH);float2 fit=float2(IW,IH)*scale;
 float2 p=float2(id.xy)+0.5-(float2(OW,OH)-fit)*0.5;float4 c=float4(0,0,0,1);
 if(all(p>=0) && all(p<fit)){
  float2 at=clamp(p/scale-0.5,0,float2(IW-1,IH-1));
  c=source.SampleLevel(linearClamp,(at+float2(id.z*IW,0)+0.5)/float2(2*IW,IH),0);
 }
 output[uint2(id.x+id.z*OW,id.y)]=c;
}
)";
        Ptr<ID3DBlob> cs,error,serialized;
        if(FAILED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"CS","cs_5_1",D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&cs,&error)))return false;
        D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
        D3D12_ROOT_PARAMETER p[3]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants.Num32BitValues=4;
        for(UINT i=0;i<2;++i){p[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[i+1].DescriptorTable={1,&ranges[i]};}
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;sampler.MaxAnisotropy=1;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=3;rs.pParameters=p;rs.NumStaticSamplers=1;rs.pStaticSamplers=&sampler;
        if(FAILED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) ||
            FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_))))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root_.Get();pipeline.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
        if(FAILED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&pipeline_))))return false;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;auto d=first;d.Width=UINT64(width)*2;d.Height=height;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&output_))))return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=count+1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap_))))return false;
        stride_=device->GetDescriptorHandleIncrementSize(hd.Type);auto cpu=heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Format=first.Format;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
        for(UINT i=0;i<count;++i){device->CreateShaderResourceView(sources[i],&srv,cpu);cpu.ptr+=stride_;sources_.emplace_back(sources[i]);}
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=first.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output_.Get(),nullptr,&uav,cpu);iw_=UINT(first.Width/2);ih_=first.Height;ow_=width;oh_=height;return true;
    }
    bool Record(ID3D12GraphicsCommandList* commands,UINT index,bool uploaded) const {
        if(!commands || !pipeline_ || !heap_ || !output_ || index>=sources_.size())return false;
        auto* source=sources_[index].Get();
        GeometryBarrier(commands,source,uploaded?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        GeometryBarrier(commands,output_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto* heap=heap_.Get();commands->SetDescriptorHeaps(1,&heap);commands->SetComputeRootSignature(root_.Get());commands->SetPipelineState(pipeline_.Get());
        const UINT size[]={iw_,ih_,ow_,oh_};commands->SetComputeRoot32BitConstants(0,4,size,0);
        auto handle=heap_->GetGPUDescriptorHandleForHeapStart();handle.ptr+=UINT64(index)*stride_;commands->SetComputeRootDescriptorTable(1,handle);
        handle=heap_->GetGPUDescriptorHandleForHeapStart();handle.ptr+=UINT64(sources_.size())*stride_;commands->SetComputeRootDescriptorTable(2,handle);
        commands->Dispatch((ow_+7)/8,(oh_+7)/8,2);
        GeometryBarrier(commands,source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        GeometryBarrier(commands,output_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);return true;
    }
};
