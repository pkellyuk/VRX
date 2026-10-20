// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#include "publisher.h"
#include "test_scene.h"
#include "../xgstereo/gpu_test_support.h"
#include <cstdio>
#include <array>
using namespace vrx::dx12::fixture;
namespace {
constexpr UINT width=257,height=17,slices=3;
std::array<uint8_t,4> Color(UINT frame,UINT slice,UINT x,UINT y,bool opaque=false) {
    return {uint8_t(x+slice*31+frame*17),uint8_t(x*5+y*13+slice*47+frame*7),uint8_t(x*3+y*5+frame*19+slice*11),uint8_t(opaque?255:x^y^(slice*53)^(frame*29))};
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device,UINT64 bytes,D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.SampleDesc.Count=1;
    ComPtr<ID3D12Resource> result;Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,type==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&result)));return result;
}
xg::PairFrame Read(xg::gpu_test::Snapshot& snapshot) {
    xg::PairFrame pair;const auto until=GetTickCount64()+5000;while(!snapshot.Read(pair) && GetTickCount64()<until)Sleep(1);
    Check(!pair.rgba.empty(),"converted shared GPU pair arrived");return pair;
}
void Pixels(const xg::PairFrame& pair,UINT frame,uint64_t id,bool opaque) {
    Check(pair.header.leftFrame==id && pair.header.rightFrame==id && pair.header.eyeWidth==width && pair.header.eyeHeight==height,"same-frame IDs and odd native extent survive conversion");
    for(UINT eye=0;eye<2;++eye)for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        const auto expected=Color(frame,eye?0:2,x,y,opaque);const auto* pixel=pair.rgba.data()+(size_t(y)*width*2+eye*width+x)*4;
        Check(!std::memcmp(expected.data(),pixel,4),"exact encoded RGB, alpha and nonzero input slice identity");
    }
}
void Format(IUnknown* adapter,DXGI_FORMAT format) {
    Scene scene;scene.Init(adapter);
    const bool bgra=xg::dx12::FormatConvert::Needed(format),opaque=format==DXGI_FORMAT_B8G8R8X8_UNORM || format==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;desc.DepthOrArraySize=slices;desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> source;Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&source)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[slices]{};UINT64 bytes=0;scene.device->GetCopyableFootprints(&desc,0,slices,0,footprints,nullptr,nullptr,&bytes);
    auto upload=Buffer(scene.device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD),readback=Buffer(scene.device.Get(),bytes,D3D12_HEAP_TYPE_READBACK);
    const auto session=L"native-format-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(UINT(format));
    xg::dx12::Publisher publisher;
    Check(!publisher.Create(scene.queue.Get(),session,width,height,DXGI_FORMAT_R16G16B16A16_FLOAT),"unknown HDR interpretation is rejected before creating a stream");
    Check(publisher.Create(scene.queue.Get(),session,width,height,format),"create typed native format publisher");
    xg::gpu_test::Snapshot snapshot;snapshot.Init(scene.device->GetAdapterLuid(),width,height,session);
    for(UINT frame=1;frame<=5;++frame) {
        scene.Wait();void* mapped=nullptr;Hr(upload->Map(0,nullptr,&mapped));
        for(UINT slice=0;slice<slices;++slice)for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
            auto color=Color(frame,slice,x,y);if(bgra)std::swap(color[0],color[2]);
            std::memcpy(static_cast<uint8_t*>(mapped)+footprints[slice].Offset+size_t(y)*footprints[slice].Footprint.RowPitch+x*4,color.data(),4);
        }
        upload->Unmap(0,nullptr);Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),nullptr));
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={source.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST};if(frame>1)scene.commands->ResourceBarrier(1,&b);
        for(UINT slice=0;slice<slices;++slice) {
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprints[slice];
            dst.pResource=source.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=slice;scene.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);scene.commands->ResourceBarrier(1,&b);Hr(scene.commands->Close());
        ID3D12CommandList* commands[]={scene.commands.Get()};scene.queue->ExecuteCommandLists(1,commands);
        Check(publisher.Submit(frame,{source.Get(),2,D3D12_RESOURCE_STATE_RENDER_TARGET},{source.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET})==xg::dx12::PublishResult::Published,"publish current input slice data");
        Pixels(Read(snapshot),frame,frame,opaque);scene.Wait();
        // Read the game's original resource after the conversion and prove its
        // state was restored and none of its original encoded bytes changed.
        Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),nullptr));
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;scene.commands->ResourceBarrier(1,&b);
        for(UINT slice=0;slice<slices;++slice) {
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=source.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=slice;
            dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprints[slice];scene.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);scene.commands->ResourceBarrier(1,&b);Hr(scene.commands->Close());scene.queue->ExecuteCommandLists(1,commands);scene.Wait();
        Hr(readback->Map(0,nullptr,&mapped));
        for(UINT slice=0;slice<slices;++slice)for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
            auto color=Color(frame,slice,x,y);if(bgra)std::swap(color[0],color[2]);
            Check(!std::memcmp(color.data(),static_cast<uint8_t*>(mapped)+footprints[slice].Offset+size_t(y)*footprints[slice].Footprint.RowPitch+x*4,4),"source bytes and alpha remain unchanged");
        }
        readback->Unmap(0,nullptr);scene.DebugCheck();
    }
    Check(publisher.Submit(6,{scene.eyes.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET},{source.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET})==xg::dx12::PublishResult::Invalid,"reject incompatible eye format/extent before recording");
    struct Gate {ComPtr<ID3D12Fence> fence;~Gate(){if(fence)fence->Signal(1);}} gate;
    Hr(scene.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate.fence)));Hr(scene.queue->Wait(gate.fence.Get(),1));
    for(UINT frame=10;frame<13;++frame)Check(publisher.Submit(frame,{source.Get(),2,D3D12_RESOURCE_STATE_RENDER_TARGET},{source.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET})==xg::dx12::PublishResult::Published,"bounded in-flight conversion slots");
    const auto before=GetTickCount64();Check(publisher.Submit(13,{source.Get(),2,D3D12_RESOURCE_STATE_RENDER_TARGET},{source.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET})==xg::dx12::PublishResult::Dropped && GetTickCount64()-before<100,"full conversion ring drops without GPU waiting");
    Hr(gate.fence->Signal(1));scene.Wait();Pixels(Read(snapshot),5,12,opaque);scene.DebugCheck();
    std::printf("PASS: native DX12 format %u, exact RGB/alpha, source preservation, eye slices and bounded GPU conversion\n",UINT(format));
}
}
int main(int argc,char** argv) {
    try {
        ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        ComPtr<IDXGIAdapter> adapter;if(argc==2 && !std::strcmp(argv[1],"--warp")){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8X8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB})Format(adapter.Get(),format);
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
