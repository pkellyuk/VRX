// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in local D3D12 loader bridge. No work is performed under the loader lock.
#include "interceptor.h"
#include <cstring>
#include <wrl/client.h>

namespace {
HMODULE Runtime() noexcept {
    static HMODULE module = [] {
        wchar_t path[32768]{};
        const UINT length=GetSystemDirectoryW(path,32768);
        if(!length || length>=32750) return HMODULE(nullptr);
        // Absolute System32 path prevents reloading this app-local proxy.
        constexpr wchar_t suffix[]=L"\\d3d12.dll";
        std::memcpy(path+length,suffix,sizeof(suffix));
        return LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    return module;
}
template<class Fn> Fn Function(const char* name) noexcept {
    return reinterpret_cast<Fn>(Runtime()?GetProcAddress(Runtime(),name):nullptr);
}
bool Enabled() noexcept {
    wchar_t value[3]{};
    return GetEnvironmentVariableW(L"VRX_DX12_INTERCEPT",value,3)==1 && value[0]==L'1';
}
constexpr HRESULT Missing = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
extern "C" FARPROC ResolveSystemOrdinal(UINT ordinal) noexcept {
    auto function=Runtime()?GetProcAddress(Runtime(),MAKEINTRESOURCEA(ordinal)):nullptr;
    if(!function) RaiseFailFastException(nullptr,nullptr,0);
    return function;
}

extern "C" HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** output) {
    const auto real=Function<PFN_D3D12_CREATE_DEVICE>("D3D12CreateDevice");
    if(!real) { if(output) *output=nullptr; return Missing; }
    const auto result=real(adapter,level,iid,output);
    if(SUCCEEDED(result) && output && *output && Enabled()) {
        // Code patches/trampolines are process-scoped. Pin the module before
        // installing them so a client FreeLibrary cannot leave dangling jumps.
        HMODULE pinned=nullptr;
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&D3D12CreateDevice),&pinned)) {
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&device)))) vrx::dx12::Attach(device.Get());
        }
    }
    return result;
}
extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID iid,void** output) {
    const auto real=Function<PFN_D3D12_GET_DEBUG_INTERFACE>("D3D12GetDebugInterface");
    if(!real) { if(output) *output=nullptr; return Missing; } return real(iid,output);
}
extern "C" HRESULT WINAPI D3D12GetInterface(REFCLSID clsid,REFIID iid,void** output) {
    const auto real=Function<decltype(&D3D12GetInterface)>("D3D12GetInterface");
    if(!real) { if(output) *output=nullptr; return Missing; } return real(clsid,iid,output);
}
extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT count,const IID* ids,void* configurations,UINT* sizes) {
    const auto real=Function<decltype(&D3D12EnableExperimentalFeatures)>("D3D12EnableExperimentalFeatures");
    return real?real(count,ids,configurations,sizes):Missing;
}
extern "C" HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,D3D_ROOT_SIGNATURE_VERSION version,ID3DBlob** blob,ID3DBlob** error) {
    const auto real=Function<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>("D3D12SerializeRootSignature");
    if(!real) { if(blob) *blob=nullptr; if(error) *error=nullptr; return Missing; } return real(desc,version,blob,error);
}
extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(LPCVOID data,SIZE_T size,REFIID iid,void** output) {
    const auto real=Function<PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER>("D3D12CreateRootSignatureDeserializer");
    if(!real) { if(output) *output=nullptr; return Missing; } return real(data,size,iid,output);
}
extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,ID3DBlob** blob,ID3DBlob** error) {
    const auto real=Function<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>("D3D12SerializeVersionedRootSignature");
    if(!real) { if(blob) *blob=nullptr; if(error) *error=nullptr; return Missing; } return real(desc,blob,error);
}
extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(LPCVOID data,SIZE_T size,REFIID iid,void** output) {
    const auto real=Function<PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER>("D3D12CreateVersionedRootSignatureDeserializer");
    if(!real) { if(output) *output=nullptr; return Missing; } return real(data,size,iid,output);
}
extern "C" HRESULT WINAPI VRX_DX12_GetCounts(vrx::dx12::Counts* output,UINT bytes) noexcept {
    if(!output || bytes!=sizeof(*output)) return E_INVALIDARG;
    vrx::dx12::Snapshot(*output); return S_OK;
}
