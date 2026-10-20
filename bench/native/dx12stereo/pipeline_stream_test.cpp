// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline_stream.h"
#include <cstdio>
#include <stdexcept>
using namespace vrx::dx12::stream;
static void Check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
template<class T> Parsed ParseObject(T& value){D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(value),&value};return Parse(&desc);}
int main(){try {
    static_assert(sizeof(Subobject<UINT>)==8 && offsetof(Subobject<D3D12_SHADER_BYTECODE>,value)==8,"SDK ABI includes different internal padding for scalar and pointer values");
    char code[4]{};const D3D12_SHADER_BYTECODE shader{code,sizeof(code)};
    struct Graphics {
        Subobject<UINT> mask;
        Subobject<D3D12_SHADER_BYTECODE> vs;
        Subobject<D3D12_SHADER_BYTECODE> ps;
        Subobject<D3D12_CACHED_PIPELINE_STATE> cache;
        Subobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    } graphics{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,UINT_MAX},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,shader},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,shader},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO,{code,sizeof(code)}},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE}};
    auto parsed=ParseObject(graphics);Check(parsed.kind==Kind::Graphics && parsed.vs.pShaderBytecode==code,"mixed scalar and pointer ABI parsed");
    Check(parsed.ps.pShaderBytecode==code && parsed.ps.BytecodeLength==sizeof(code),"pixel shader metadata retained");
    auto copy=parsed.Desc();Check(copy.SizeInBytes==sizeof(graphics) && !std::memcmp(copy.pPipelineStateSubobjectStream,&graphics,sizeof(graphics)),"all untouched pipeline settings preserved");
    char replacement[8]{};parsed.Shader({replacement,sizeof(replacement)});parsed.ClearCache();copy=parsed.Desc();auto patched=Parse(&copy);
    Check(patched.kind==Kind::Graphics && patched.vs.pShaderBytecode==replacement && patched.vs.BytecodeLength==sizeof(replacement),"vertex replacement lives only in copied stream");
    Check(patched.ps.pShaderBytecode==code && patched.ps.BytecodeLength==sizeof(code),"vertex replacement leaves pixel bytecode unchanged");
    D3D12_CACHED_PIPELINE_STATE cache{};std::memcpy(&cache,static_cast<uint8_t*>(copy.pPipelineStateSubobjectStream)+patched.cache,sizeof(cache));
    Check(!cache.pCachedBlob && !cache.CachedBlobSizeInBytes && graphics.vs.value.pShaderBytecode==code && graphics.cache.value.pCachedBlob==code,"rewritten pipeline clears old cache without touching caller memory");
    auto malformed=D3D12_PIPELINE_STATE_STREAM_DESC{sizeof(graphics)-1,&graphics};Check(Parse(&malformed).kind==Kind::Invalid,"partial word rejected");
    malformed={65544,&graphics};Check(Parse(&malformed).kind==Kind::Invalid,"oversized stream rejected before allocation");
    malformed={sizeof(graphics),reinterpret_cast<uint8_t*>(&graphics)+1};Check(Parse(&malformed).kind==Kind::Invalid,"unaligned stream rejected before reading");
    malformed={sizeof(Subobject<UINT>)+sizeof(Subobject<D3D12_SHADER_BYTECODE>)-8,&graphics};Check(Parse(&malformed).kind==Kind::Invalid,"truncated shader descriptor rejected");
    struct Shaders {Subobject<D3D12_SHADER_BYTECODE> vs,cs,gs;} shaders{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,shader},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,{}},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS,{}}};
    Check(ParseObject(shaders).kind==Kind::Graphics,"empty irrelevant stages are legal");shaders.cs.value=shader;
    Check(ParseObject(shaders).kind==Kind::Invalid,"two active pipeline classes rejected");shaders.vs.value={};shaders.gs.value=shader;
    Check(ParseObject(shaders).kind==Kind::Compute,"runtime ignores graphics-only stages in compute streams");shaders.vs.value=shader;shaders.cs.value={};
    auto geometry=ParseObject(shaders);Check(geometry.kind==Kind::Graphics && geometry.FinalShader().pShaderBytecode==code,"geometry is selected as the final position-producing stage");
    geometry.Shader({replacement,sizeof(replacement)});copy=geometry.Desc();auto final=Parse(&copy);Check(final.gs.pShaderBytecode==replacement && final.vs.pShaderBytecode==code && shaders.gs.value.pShaderBytecode==code,"final-stage replacement preserves original vertex and caller streams");shaders.gs.type=D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
    Check(ParseObject(shaders).kind==Kind::Invalid,"duplicate subobject rejected");
    auto unknown=Subobject<UINT>{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE(23),0};Check(ParseObject(unknown).kind==Kind::Invalid,"reserved enum hole is not accepted as an object");
    struct Versions {Subobject<D3D12_SHADER_BYTECODE> vs;Subobject<D3D12_RASTERIZER_DESC1> old;Subobject<D3D12_RASTERIZER_DESC2> newer;} versions{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,shader},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1,{}},{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2,{}}};
    Check(ParseObject(versions).kind==Kind::Invalid,"conflicting state versions rejected");
    std::puts("PASS: native pipeline stream ABI, bounds, duplicate/version rejection, graphics/compute classification, isolated shader/cache replacement");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
