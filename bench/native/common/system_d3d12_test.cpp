#include "system_d3d12.h"
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
static void Hr(HRESULT value,const char* text){Check(SUCCEEDED(value),text);}
int main(int argc,char**){try{
    const auto applicationModule=LoadLibraryW(L"d3d12.dll");Check(applicationModule!=nullptr,"load application-selected D3D12");
    const auto nativeModule=vrx::SystemGraphicsModule(L"d3d12.dll");Check(nativeModule!=nullptr,"load System32 D3D12");
    if(argc>1)Check(applicationModule!=nativeModule,"translated DLL and native transport DLL are distinct");
    ComPtr<IDXGIFactory4> factory;Hr(vrx::SystemDxgiFactory(IID_PPV_ARGS(&factory)),"native DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;Hr(factory->EnumAdapters1(0,&adapter),"native adapter");
    ComPtr<ID3D12Device> device;Hr(vrx::SystemD3d12Device(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"native D3D12 device");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=64;desc.Height=64;desc.DepthOrArraySize=2;desc.MipLevels=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    ComPtr<ID3D12Resource> texture;Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)),"native shared array texture");
    HANDLE handle=nullptr;Hr(device->CreateSharedHandle(texture.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"native resource handle");Check(handle!=nullptr,"resource handle created");CloseHandle(handle);
    ComPtr<ID3D12Fence> fence;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"native shared fence");
    handle=nullptr;Hr(device->CreateSharedHandle(fence.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"native fence handle");Check(handle!=nullptr,"fence handle created");CloseHandle(handle);
    std::puts("PASS: System32 D3D12 transport resources and fences, isolated from app-local translation");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
