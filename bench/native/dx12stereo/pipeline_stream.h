// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// SDK stream ABI: each enum/value pair is independently pointer-aligned.
#pragma once
#include <d3d12.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
namespace vrx::dx12::stream {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4324) // The SDK stream ABI explicitly requires this padding.
#endif
template<class T> struct alignas(void*) Subobject {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;T value;};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
enum class Kind {Invalid,Unsupported,Graphics,Compute};
struct Parsed {
    Kind kind=Kind::Invalid;
    size_t vertex=SIZE_MAX,cache=SIZE_MAX;
    size_t geometry=SIZE_MAX;
    D3D12_SHADER_BYTECODE vs{};
    D3D12_SHADER_BYTECODE gs{};
    std::string reason;
    std::vector<uint64_t> storage;
    D3D12_PIPELINE_STATE_STREAM_DESC Desc(){return {storage.size()*sizeof(uint64_t),storage.data()};}
    D3D12_SHADER_BYTECODE FinalShader() const{return gs.BytecodeLength?gs:vs;}
    void Shader(const D3D12_SHADER_BYTECODE& value){std::memcpy(reinterpret_cast<uint8_t*>(storage.data())+(gs.BytecodeLength?geometry:vertex),&value,sizeof(value));}
    void ClearCache(){if(cache!=SIZE_MAX){const D3D12_CACHED_PIPELINE_STATE empty{};std::memcpy(reinterpret_cast<uint8_t*>(storage.data())+cache,&empty,sizeof(empty));}}
};
inline Parsed Parse(const D3D12_PIPELINE_STATE_STREAM_DESC* desc) {
    Parsed out;
    const auto fail=[&](const char* reason){out.reason=reason;out.kind=Kind::Invalid;return out;};
    if(!desc || !desc->pPipelineStateSubobjectStream || !desc->SizeInBytes || desc->SizeInBytes>65536 ||
        desc->SizeInBytes%sizeof(void*) || reinterpret_cast<uintptr_t>(desc->pPipelineStateSubobjectStream)%alignof(void*))return fail("invalid pipeline stream bounds/alignment");
    const auto* bytes=static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
    std::array<bool,D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID> seen{};
    bool vertex=false,compute=false,mesh=false,unsupported=false;
    for(size_t offset=0;offset<desc->SizeInBytes;) {
        if(desc->SizeInBytes-offset<sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))return fail("truncated pipeline subobject type");
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;std::memcpy(&type,bytes+offset,sizeof(type));
        if(UINT(type)>=seen.size() || seen[type])return fail("unknown or duplicate pipeline subobject");
        seen[type]=true;size_t size=0,value=0;
#define FIELD(Name,T) case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##Name: size=sizeof(Subobject<T>);value=offsetof(Subobject<T>,value);break;
        switch(type) {
            FIELD(ROOT_SIGNATURE,ID3D12RootSignature*)
            FIELD(VS,D3D12_SHADER_BYTECODE) FIELD(PS,D3D12_SHADER_BYTECODE) FIELD(DS,D3D12_SHADER_BYTECODE) FIELD(HS,D3D12_SHADER_BYTECODE)
            FIELD(GS,D3D12_SHADER_BYTECODE) FIELD(CS,D3D12_SHADER_BYTECODE) FIELD(AS,D3D12_SHADER_BYTECODE) FIELD(MS,D3D12_SHADER_BYTECODE)
            FIELD(STREAM_OUTPUT,D3D12_STREAM_OUTPUT_DESC) FIELD(BLEND,D3D12_BLEND_DESC) FIELD(SAMPLE_MASK,UINT)
            FIELD(RASTERIZER,D3D12_RASTERIZER_DESC) FIELD(RASTERIZER1,D3D12_RASTERIZER_DESC1) FIELD(RASTERIZER2,D3D12_RASTERIZER_DESC2)
            FIELD(DEPTH_STENCIL,D3D12_DEPTH_STENCIL_DESC) FIELD(DEPTH_STENCIL1,D3D12_DEPTH_STENCIL_DESC1) FIELD(DEPTH_STENCIL2,D3D12_DEPTH_STENCIL_DESC2)
            FIELD(INPUT_LAYOUT,D3D12_INPUT_LAYOUT_DESC) FIELD(IB_STRIP_CUT_VALUE,D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)
            FIELD(PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE) FIELD(RENDER_TARGET_FORMATS,D3D12_RT_FORMAT_ARRAY)
            FIELD(DEPTH_STENCIL_FORMAT,DXGI_FORMAT) FIELD(SAMPLE_DESC,DXGI_SAMPLE_DESC) FIELD(NODE_MASK,UINT)
            FIELD(CACHED_PSO,D3D12_CACHED_PIPELINE_STATE) FIELD(FLAGS,D3D12_PIPELINE_STATE_FLAGS) FIELD(VIEW_INSTANCING,D3D12_VIEW_INSTANCING_DESC)
            default:return fail("unknown pipeline subobject");
        }
#undef FIELD
        if(size>desc->SizeInBytes-offset)return fail("truncated pipeline subobject value");
        const auto at=offset+value;
        switch(type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
                out.vertex=at;std::memcpy(&out.vs,bytes+at,sizeof(out.vs));vertex=out.vs.pShaderBytecode && out.vs.BytecodeLength;break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: {
                D3D12_SHADER_BYTECODE shader{};std::memcpy(&shader,bytes+at,sizeof(shader));const bool active=shader.pShaderBytecode && shader.BytecodeLength;
                if(type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS)compute=active;
                else if(type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS)mesh=active;
                else if(type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS){out.geometry=at;out.gs=shader;}
                else if(active){unsupported=true;if(out.reason.empty())out.reason="tessellation requires final domain-stage stereo transformation";}
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:out.cache=at;break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
                D3D12_STREAM_OUTPUT_DESC so{};std::memcpy(&so,bytes+at,sizeof(so));if(so.NumEntries || so.NumStrides){unsupported=true;if(out.reason.empty())out.reason="stream output requires additional stereo handling";}break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
                D3D12_VIEW_INSTANCING_DESC view{};std::memcpy(&view,bytes+at,sizeof(view));if(view.ViewInstanceCount){unsupported=true;out.reason="view instancing requires stereo view mapping; count="+std::to_string(view.ViewInstanceCount);}break;
            }
            default:break;
        }
        offset+=size;
    }
    const auto depthCount=UINT(seen[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL])+UINT(seen[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1])+UINT(seen[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2]);
    const auto rasterCount=UINT(seen[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER])+UINT(seen[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1])+UINT(seen[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2]);
    if(depthCount>1 || rasterCount>1 || UINT(vertex)+UINT(compute)+UINT(mesh)!=1)return fail("conflicting pipeline definitions");
    // Other graphics fields in a compute stream are ignored by the runtime.
    if(compute){out.kind=Kind::Compute;return out;}
    if(mesh || unsupported){out.kind=Kind::Unsupported;if(mesh)out.reason="mesh shaders require final-stage stereo transformation";return out;}
    out.kind=Kind::Graphics;out.storage.resize(desc->SizeInBytes/sizeof(uint64_t));std::memcpy(out.storage.data(),bytes,desc->SizeInBytes);return out;
}
} // namespace vrx::dx12::stream
