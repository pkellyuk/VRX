// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Explicit per-process diagnostics; callbacks never call graphics APIs.
#pragma once
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <mutex>
#include <string>
#include <cstdio>
#include <unordered_map>
namespace vrx::dx12::diagnostic {
inline bool Requested() noexcept {wchar_t text[3]{};return GetEnvironmentVariableW(L"VRX_DX12_DEBUG_LAYER",text,3)==1 && text[0]==L'1';}
struct State {
    std::once_flag enabled;
    std::mutex setup,output;
    std::wstring path;
    unsigned messages=0,warnings=0;
    std::unordered_map<unsigned,unsigned> warningIds;
    State(){const auto n=GetEnvironmentVariableW(L"VRX_DX12_DEBUG_LOG",nullptr,0);if(n && n<=32768){path.resize(n);const auto got=GetEnvironmentVariableW(L"VRX_DX12_DEBUG_LOG",path.data(),n);if(got && got<n)path.resize(got);else path.clear();}}
};
inline State& Settings(){static auto* state=new State;return *state;}
inline void Write(const void* device,unsigned severity,unsigned id,const char* text) noexcept {
    try {
        auto& state=Settings();if(state.path.empty())return;std::lock_guard<std::mutex> lock(state.output);
        bool limit=false;
        if(severity==unsigned(D3D12_MESSAGE_SEVERITY_WARNING)){
            if(state.warnings>=512)return;auto& n=state.warningIds[id];if(n>=8)return;++n;++state.warnings;
        }else {if(state.messages>=4097)return;limit=++state.messages==4097;}
        FILE* file=nullptr;if(_wfopen_s(&file,state.path.c_str(),L"ab") || !file)return;
        if(limit)std::fputs("VRX debug error log limit reached; further errors omitted\n",file);
        else std::fprintf(file,"%llu device=%p severity=%u id=%u %.*s\n",static_cast<unsigned long long>(GetTickCount64()),device,severity,id,8192,text?text:"");
        std::fclose(file);
    }catch(...){}
}
inline void Enable(PFN_D3D12_GET_DEBUG_INTERFACE get) noexcept {
    if(!Requested())return;
    try {std::call_once(Settings().enabled,[&]{
        Microsoft::WRL::ComPtr<ID3D12Debug> debug;
        if(get && SUCCEEDED(get(IID_PPV_ARGS(&debug)))){
            debug->EnableDebugLayer();Write(nullptr,3,0,"VRX debug layer enabled before device creation");
            Write(nullptr,3,0,"VRX warnings sampled separately from errors: 8 per ID, 512 total");
            wchar_t gpu[3]{};
            if(GetEnvironmentVariableW(L"VRX_DX12_GPU_VALIDATION",gpu,3)==1 && gpu[0]==L'1'){
                Microsoft::WRL::ComPtr<ID3D12Debug3> advanced;
                if(SUCCEEDED(debug.As(&advanced))){advanced->SetEnableGPUBasedValidation(TRUE);advanced->SetEnableSynchronizedCommandQueueValidation(TRUE);Write(nullptr,3,0,"VRX GPU-based validation enabled");}
                else Write(nullptr,1,0,"VRX GPU-based validation unavailable");
            }
        }
        else Write(nullptr,1,0,"VRX debug layer unavailable");
    });}catch(...){}
}
inline void __stdcall Message(D3D12_MESSAGE_CATEGORY,D3D12_MESSAGE_SEVERITY severity,D3D12_MESSAGE_ID id,LPCSTR description,void* context) noexcept {
    if(severity<=D3D12_MESSAGE_SEVERITY_WARNING)Write(context,unsigned(severity),unsigned(id),description);
}
inline void Name(ID3D12Object* object,const wchar_t* kind,const void* original) noexcept {
    if(!object || !Requested())return;
    wchar_t text[128]{};if(swprintf_s(text,L"VRX right %ls from %p",kind,original)>0)object->SetName(text);
}
inline void Attach(ID3D12Device* device) noexcept {
    if(!device || !Requested())return;
    try {
        auto& state=Settings();std::lock_guard<std::mutex> lock(state.setup);
        // Private data dies with its device, preventing duplicate callbacks
        // without retaining a COM reference or confusing reused addresses.
        constexpr GUID marker={0x7d958496,0x1512,0x47e8,{0xad,0x82,0x7a,0x53,0xce,0x48,0x05,0xb1}};
        DWORD cookie=0;UINT bytes=sizeof(cookie);if(SUCCEEDED(device->GetPrivateData(marker,&bytes,&cookie)))return;
        Microsoft::WRL::ComPtr<ID3D12InfoQueue1> queue;
        if(FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))){Write(device,1,0,"VRX info-queue callback unavailable");return;}
        if(SUCCEEDED(queue->RegisterMessageCallback(Message,D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS,device,&cookie))){
            if(FAILED(device->SetPrivateData(marker,sizeof(cookie),&cookie))){queue->UnregisterMessageCallback(cookie);Write(device,1,0,"VRX callback identity could not be saved");return;}
            Write(device,3,0,"VRX info-queue callback ready");
        }else Write(device,1,0,"VRX info-queue callback registration failed");
    }catch(...){}
}
} // namespace vrx::dx12::diagnostic
