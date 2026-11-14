// Compile the complete optional Windows/OpenXR/GPU adapter without a headset or
// model files. This catches SDK/type errors independently of the full renderer.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "controller_menu.h"
#include "snapshot_capture.h"
#include <cassert>
#include <cstdio>
int main()
{
    ControllerMenu menu; menu.Configure(L"");
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device;
    assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    assert(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    assert(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC queueDesc{};
    assert(SUCCEEDED(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue))));
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 2; desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    ComPtr<ID3D12Resource> source, upload;
    assert(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&source))));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes;
    buffer.Height = buffer.SampleDesc.Count = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    assert(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload))));
    void* mapped = nullptr; assert(SUCCEEDED(upload->Map(0,nullptr,&mapped)));
    const uint8_t colours[]{255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
    for (int y = 0; y < 2; ++y) memcpy(static_cast<uint8_t*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,colours+y*8,8);
    upload->Unmap(0,nullptr);
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> commands;
    assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))));
    assert(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands))));
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.pResource = upload.Get(); from.PlacedFootprint = fp;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.pResource = source.Get();
    commands->CopyTextureRegion(&to,0,0,0,&from,nullptr); assert(SUCCEEDED(commands->Close()));
    ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1,lists);
    ScreenSnapshotCapture capture; assert(capture.Start(device.Get(),queue.Get(),source.Get()));
    std::wstring path; bool complete = false; const auto deadline = GetTickCount64()+5000;
    while (!(complete = capture.Poll(path)) && GetTickCount64() < deadline) Sleep(2);
    assert(complete && !path.empty());
    std::ifstream image(std::filesystem::path(path),std::ios::binary);
    BITMAPFILEHEADER header{}; BITMAPINFOHEADER info{}; uint8_t pixels[16]{};
    image.read(reinterpret_cast<char*>(&header),sizeof(header)); image.read(reinterpret_cast<char*>(&info),sizeof(info));
    image.read(reinterpret_cast<char*>(pixels),sizeof(pixels)); image.close();
    assert(header.bfType == 0x4d42 && info.biWidth == 2 && info.biHeight == -2);
    assert(pixels[0] == 0 && pixels[2] == 255 && pixels[8] == 255 && pixels[10] == 0);
    assert(DeleteFileW(path.c_str()));
    puts("PASS: asynchronous GPU snapshot readback, bitmap orientation/colour and temporary-file cleanup (WARP)");
}
