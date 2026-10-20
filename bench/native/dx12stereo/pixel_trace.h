// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Bounded local shader diagnostics, only in an explicitly requested probe run.
#pragma once
#include "replay_probe.h"
#include <d3dcompiler.h>
#include <unordered_set>
namespace vrx::dx12::diagnostic {
struct PixelTrace {
    std::vector<BYTE> bytes;
    uint64_t hash=0;
    void Capture(const D3D12_SHADER_BYTECODE& shader,size_t& budget){
        static const bool requested=ProbesRequested();if(!requested || !shader.pShaderBytecode || !shader.BytecodeLength || shader.BytecodeLength>1024*1024 || shader.BytecodeLength>64*1024*1024-budget)return;
        const auto* begin=static_cast<const BYTE*>(shader.pShaderBytecode);bytes.assign(begin,begin+shader.BytecodeLength);budget+=shader.BytecodeLength;hash=14695981039346656037ull;for(auto byte:bytes)hash=(hash^byte)*1099511628211ull;
    }
    void Dump() const {
        if(bytes.empty() || Settings().path.empty())return;
        static std::unordered_set<uint64_t> saved;if(saved.count(hash) || saved.size()>=128)return;
        Microsoft::WRL::ComPtr<ID3DBlob> assembly;if(FAILED(D3DDisassemble(bytes.data(),bytes.size(),0,nullptr,&assembly)) || assembly->GetBufferSize()>8*1024*1024){Write(nullptr,1,0,"VRX pixel shader disassembly failed or exceeded bounds");return;}
        wchar_t suffix[64]{};swprintf_s(suffix,L".shader-%llx.ps.asm",static_cast<unsigned long long>(hash));const auto path=Settings().path+suffix;FILE* file=nullptr;if(_wfopen_s(&file,path.c_str(),L"wb") || !file){Write(nullptr,1,0,"VRX pixel shader trace could not be saved");return;}
        const auto size=assembly->GetBufferSize();const bool written=std::fwrite(assembly->GetBufferPointer(),1,size,file)==size;std::fclose(file);if(written)saved.insert(hash);else Write(nullptr,1,0,"VRX pixel shader trace write failed");
    }
};
} // namespace vrx::dx12::diagnostic
