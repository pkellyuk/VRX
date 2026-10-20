// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "test_scene.h"
#include "pipeline_stream.h"
namespace vrx::dx12::fixture {
inline bool ModernMode(){wchar_t text[3]{};return GetEnvironmentVariableW(L"VRX_DX12_TEST_MODERN",text,3)==1 && text[0]==L'1';}
inline D3D12_RESOURCE_DESC1 Description1(const D3D12_RESOURCE_DESC& d){return {d.Dimension,d.Alignment,d.Width,d.Height,d.DepthOrArraySize,d.MipLevels,d.Format,d.SampleDesc,d.Layout,d.Flags,{}};}
inline void Committed(ID3D12Device* device,const D3D12_HEAP_PROPERTIES& heap,const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,ComPtr<ID3D12Resource>& out,bool descriptor1=true) {
    if(!ModernMode()){Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,clear,IID_PPV_ARGS(&out)));return;}
    if(descriptor1){ComPtr<ID3D12Device8> newer;Hr(device->QueryInterface(IID_PPV_ARGS(&newer)));auto d=Description1(desc);ComPtr<ID3D12Resource2> resource;
        Hr(newer->CreateCommittedResource2(&heap,D3D12_HEAP_FLAG_NONE,&d,state,clear,nullptr,IID_PPV_ARGS(&resource)));out=resource;
    }else{ComPtr<ID3D12Device4> newer;Hr(device->QueryInterface(IID_PPV_ARGS(&newer)));ComPtr<ID3D12Resource1> resource;
        Hr(newer->CreateCommittedResource1(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,clear,nullptr,IID_PPV_ARGS(&resource)));out=resource;
    }
}
inline void PlacedUpload(ID3D12Device* device,const D3D12_RESOURCE_DESC& desc,ComPtr<ID3D12Heap>& allocation,ComPtr<ID3D12Resource>& out,bool descriptor1) {
    D3D12_HEAP_DESC heap{};heap.Properties.Type=D3D12_HEAP_TYPE_UPLOAD;heap.SizeInBytes=3*D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;heap.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Device4> newer;Hr(device->QueryInterface(IID_PPV_ARGS(&newer)));ComPtr<ID3D12Heap1> native;
    Hr(newer->CreateHeap1(&heap,nullptr,IID_PPV_ARGS(&native)));allocation=native;
    if(descriptor1){ComPtr<ID3D12Device8> newest;Hr(device->QueryInterface(IID_PPV_ARGS(&newest)));auto d=Description1(desc);ComPtr<ID3D12Resource2> resource;
        Hr(newest->CreatePlacedResource1(native.Get(),D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&resource)));out=resource;
    }else Hr(device->CreatePlacedResource(native.Get(),D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&out)));
}
inline void GraphicsStream(ID3D12Device* device,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p,ComPtr<ID3D12PipelineState>& out) {
    using namespace vrx::dx12::stream;
    struct Stream {
        Subobject<ID3D12RootSignature*> root;
        Subobject<D3D12_SHADER_BYTECODE> vs,ps,emptyCs;
        Subobject<D3D12_BLEND_DESC> blend;
        Subobject<UINT> mask;
        Subobject<D3D12_RASTERIZER_DESC> raster;
        Subobject<D3D12_DEPTH_STENCIL_DESC> depth;
        Subobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
        Subobject<D3D12_RT_FORMAT_ARRAY> formats;
        Subobject<DXGI_FORMAT> dsv;
        Subobject<DXGI_SAMPLE_DESC> samples;
        Subobject<D3D12_CACHED_PIPELINE_STATE> cache;
    };
    ComPtr<ID3D12PipelineState> native;Hr(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&native)));ComPtr<ID3DBlob> cached;Hr(native->GetCachedBlob(&cached));
    D3D12_RT_FORMAT_ARRAY formats{};formats.NumRenderTargets=p.NumRenderTargets;std::memcpy(formats.RTFormats,p.RTVFormats,sizeof(formats.RTFormats));
    Stream stream{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,p.pRootSignature},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,p.VS},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,p.PS},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,{}},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,p.BlendState},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,p.SampleMask},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,p.RasterizerState},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,p.DepthStencilState},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,p.PrimitiveTopologyType},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,formats},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,p.DSVFormat},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,p.SampleDesc},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO,{cached->GetBufferPointer(),cached->GetBufferSize()}}};
    ComPtr<ID3D12Device2> newer;Hr(device->QueryInterface(IID_PPV_ARGS(&newer)));D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream),&stream};Hr(newer->CreatePipelineState(&desc,IID_PPV_ARGS(&out)));
}
inline void ComputeStream(ID3D12Device* device,const D3D12_COMPUTE_PIPELINE_STATE_DESC& p,ComPtr<ID3D12PipelineState>& out) {
    using namespace vrx::dx12::stream;
    struct {Subobject<ID3D12RootSignature*> root;Subobject<D3D12_SHADER_BYTECODE> cs,emptyVs;} stream{
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,p.pRootSignature},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,p.CS},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,{}}};
    ComPtr<ID3D12Device2> newer;Hr(device->QueryInterface(IID_PPV_ARGS(&newer)));D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream),&stream};Hr(newer->CreatePipelineState(&desc,IID_PPV_ARGS(&out)));
}
} // namespace vrx::dx12::fixture
