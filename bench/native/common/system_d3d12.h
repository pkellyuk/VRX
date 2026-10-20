// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// VRX transport owns native Windows resources, even inside a translated game.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
namespace vrx {
inline HMODULE SystemGraphicsModule(const wchar_t* name) {
    wchar_t directory[MAX_PATH]{};
    const auto n=GetSystemDirectoryW(directory,MAX_PATH);
    if(!n || n>=MAX_PATH)return nullptr;
    const std::wstring path=std::wstring(directory,n)+L"\\"+name;
    // Absolute paths avoid resolving an already loaded app-local DLL by basename.
    return LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
}
inline HRESULT SystemDxgiFactory(REFIID iid,void** out) {
    if(!out)return E_POINTER;*out=nullptr;
    // Retain modules for the lifetime of all returned COM objects.
    static const auto module=SystemGraphicsModule(L"dxgi.dll");
    using Create=HRESULT(WINAPI*)(REFIID,void**);
    static const auto create=module?reinterpret_cast<Create>(GetProcAddress(module,"CreateDXGIFactory1")):nullptr;
    return create?create(iid,out):HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
inline HRESULT SystemD3d12Device(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** out) {
    if(!out)return E_POINTER;*out=nullptr;
    static const auto module=SystemGraphicsModule(L"d3d12.dll");
    using Create=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
    static const auto create=module?reinterpret_cast<Create>(GetProcAddress(module,"D3D12CreateDevice")):nullptr;
    return create?create(adapter,level,iid,out):HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
}
