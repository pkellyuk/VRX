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

    // Exercise the actual casing shaders and stereo geometry on a software GPU.
    desc.Width=vrxmenu::AtlasWidth; desc.Height=vrxmenu::AtlasHeight; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
    heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> atlas;
    assert(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&atlas))));
    std::vector<XrSwapchainImageD3D12KHR> images(1,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR}); images[0].texture=atlas.Get();
    MenuBodyRenderer bodies; assert(bodies.Init(device.Get(),images,DXGI_FORMAT_R8G8B8A8_UNORM)); bodies.valid=true;
    for (int e=0;e<2;e++) { bodies.eyes[e].pose={{0,0,0,1},{e ? .032f:-.032f,0,0}}; bodies.eyes[e].fov={-.7f,.7f,.7f,-.7f}; }
    struct Panel { XrPosef pose{{0,sinf(.3f),0,cosf(.3f)},{0,0,-1.2f}}; XrExtent2Df size{.85f,.68f}; bool visible=false; } panels[7];
    panels[0].visible=true;
    assert(SUCCEEDED(allocator->Reset())); assert(SUCCEEDED(commands->Reset(allocator.Get(),nullptr)));
    bodies.Draw(commands.Get(),0,panels,XR_NULL_HANDLE,XR_NULL_HANDLE);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=atlas.Get(); barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    commands->ResourceBarrier(1,&barrier);
    heap.Type=D3D12_HEAP_TYPE_READBACK; buffer.Width=UINT64(vrxmenu::Width)*vrxmenu::Height*4;
    ComPtr<ID3D12Resource> readback;
    assert(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))));
    from={}; from.pResource=atlas.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to={}; to.pResource=readback.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,vrxmenu::Width,vrxmenu::Height,1,vrxmenu::Width*4};
    D3D12_BOX tile{vrxmenu::Width,vrxmenu::Height*3,0,vrxmenu::Width*2,vrxmenu::Height*4,1};
    commands->CopyTextureRegion(&to,0,0,0,&from,&tile); assert(SUCCEEDED(commands->Close()));
    queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence; assert(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))));
    assert(SUCCEEDED(queue->Signal(fence.Get(),1))); HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    assert(event && SUCCEEDED(fence->SetEventOnCompletion(1,event))); assert(WaitForSingleObject(event,5000)==WAIT_OBJECT_0); CloseHandle(event);
    assert(SUCCEEDED(readback->Map(0,nullptr,&mapped)));
    int opaque[2]{};
    for (int y=0;y<vrxmenu::Height;y++) for (int x=0;x<vrxmenu::Width;x++)
        if (static_cast<uint8_t*>(mapped)[(size_t(y)*vrxmenu::Width+x)*4+3]) ++opaque[x/(vrxmenu::Width/2)];
    assert(opaque[0]>100 && opaque[1]>100 && opaque[0]<vrxmenu::Width*vrxmenu::Height/20);
    readback->Unmap(0,nullptr); assert(SUCCEEDED(device->GetDeviceRemovedReason()));
    puts("PASS: thin panel casings rasterize in both eyes with transparent interiors (WARP)");
}
