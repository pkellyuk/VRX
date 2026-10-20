// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Graphics code calls ordinary D3D12 APIs. VRX's diagnostic export is queried
// only to verify that an app-local proxy observed those calls automatically.
#include "test_scene.h"
#include "interceptor.h"
#include <thread>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <cstring>

using namespace vrx::dx12::fixture;
static void Identity(Scene& scene) {
    ComPtr<IUnknown> original,fromQueue,fromList;
    Hr(scene.device.As(&original));
    Hr(scene.queue->GetDevice(IID_PPV_ARGS(&fromQueue)));
    Hr(scene.commands->GetDevice(IID_PPV_ARGS(&fromList)));
    Check(original.Get()==fromQueue.Get() && original.Get()==fromList.Get(),"native COM identity is preserved");
    ComPtr<ID3D12Device4> newer;
    if(SUCCEEDED(scene.device.As(&newer))) {
        ComPtr<IUnknown> identity; Hr(newer.As(&identity)); Check(identity.Get()==original.Get(),"newer device interface has identical IUnknown");
        ComPtr<ID3D12GraphicsCommandList> list;
        Hr(newer->CreateCommandList1(0,D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_FLAG_NONE,IID_PPV_ARGS(&list)));
        Hr(list->Reset(scene.allocator.Get(),nullptr)); Hr(list->Close());
    }
    void* unsupported=reinterpret_cast<void*>(1);
    Check(scene.device->QueryInterface(__uuidof(ID3D12Resource),&unsupported)==E_NOINTERFACE && !unsupported,"unsupported IID semantics are preserved");
}
static void Readback(Scene& scene,UINT frame) {
    const auto desc=scene.eyes->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint[2]{}; UINT64 bytes=0;
    scene.device->GetCopyableFootprints(&desc,0,2,0,footprint,nullptr,nullptr,&bytes);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=bytes;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)));
    scene.Wait(); Hr(scene.allocator->Reset()); Hr(scene.commands->Reset(scene.allocator.Get(),nullptr));
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={scene.eyes.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
    scene.commands->ResourceBarrier(1,&barrier);
    for(UINT eye=0;eye<2;++eye) {
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=scene.eyes.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex=eye;
        dst.pResource=result.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint[eye];
        scene.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); scene.commands->ResourceBarrier(1,&barrier);
    Hr(scene.commands->Close()); ID3D12CommandList* commands[]={scene.commands.Get()}; scene.queue->ExecuteCommandLists(1,commands); scene.Wait();
    const uint8_t* data=nullptr; Hr(result->Map(0,nullptr,reinterpret_cast<void**>(const_cast<uint8_t**>(&data))));
    uint64_t xsum[2]{},count[2]{};
    for(UINT eye=0;eye<2;++eye) for(UINT y=0;y<scene.height;++y) for(UINT x=0;x<scene.width;++x) {
        const auto* p=data+footprint[eye].Offset+size_t(y)*footprint[eye].Footprint.RowPitch+x*4;
        Check(p[3]==255,"native draw alpha survives interception");
        if(p[1]>100) {
            Check(abs(int(p[0])-int((frame%3+1)*255/4))<=1 && p[2]==191,"native draw color survives interception");
            xsum[eye]+=x; ++count[eye];
        }
    }
    result->Unmap(0,nullptr);
    Check(count[0]>100 && count[1]>100 && double(xsum[1])/count[1]-double(xsum[0])/count[0]>scene.width*.15,"native fixture's independent view geometry survives interception");
}
static void RootSignatureExports() {
    D3D12_ROOT_SIGNATURE_DESC desc{}; ComPtr<ID3DBlob> blob,error;
    Hr(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    ComPtr<ID3D12RootSignatureDeserializer> legacy;
    Hr(D3D12CreateRootSignatureDeserializer(blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&legacy)));
    Check(legacy->GetRootSignatureDesc()->NumParameters==0,"root signature forwarding");
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{}; versioned.Version=D3D_ROOT_SIGNATURE_VERSION_1_0;
    Hr(D3D12SerializeVersionedRootSignature(&versioned,&blob,&error));
    ComPtr<ID3D12VersionedRootSignatureDeserializer> modern;
    Hr(D3D12CreateVersionedRootSignatureDeserializer(blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&modern)));
    Check(modern->GetUnconvertedRootSignatureDesc()->Version==D3D_ROOT_SIGNATURE_VERSION_1_0,"versioned signature forwarding");
}
static void Bundles(Scene& scene) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=12;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> indices;
    Hr(scene.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&indices)));
    void* mapped=nullptr; Hr(indices->Map(0,nullptr,&mapped));
    const UINT values[]={1,2,3}; std::memcpy(mapped,values,sizeof(values)); indices->Unmap(0,nullptr);
    ComPtr<ID3D12CommandAllocator> allocators[2]; ComPtr<ID3D12GraphicsCommandList> bundles[2];
    for(UINT eye=0;eye<2;++eye) {
        Hr(scene.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE,IID_PPV_ARGS(&allocators[eye])));
        Hr(scene.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_BUNDLE,allocators[eye].Get(),scene.pipeline.Get(),IID_PPV_ARGS(&bundles[eye])));
        bundles[eye]->SetGraphicsRootSignature(scene.root.Get());
        const float constants[]={eye==0?-.2f:.2f,.25f,.5f,.75f}; bundles[eye]->SetGraphicsRoot32BitConstants(0,4,constants,0);
        bundles[eye]->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW indexView{indices->GetGPUVirtualAddress(),12,DXGI_FORMAT_R32_UINT}; bundles[eye]->IASetIndexBuffer(&indexView);
        // The signed base vertex and stack-passed instance offset must survive.
        bundles[eye]->DrawIndexedInstanced(3,1,0,-1,0); Hr(bundles[eye]->Close());
    }
    scene.Wait(); Hr(scene.allocator->Reset()); Hr(scene.commands->Reset(scene.allocator.Get(),scene.pipeline.Get()));
    scene.commands->SetGraphicsRootSignature(scene.root.Get());
    D3D12_VIEWPORT viewport{0,0,float(scene.width),float(scene.height),0,1}; D3D12_RECT rect{0,0,LONG(scene.width),LONG(scene.height)};
    scene.commands->RSSetViewports(1,&viewport); scene.commands->RSSetScissorRects(1,&rect);
    for(UINT eye=0;eye<2;++eye) {
        auto target=scene.rtv->GetCPUDescriptorHandleForHeapStart(); target.ptr+=SIZE_T(eye)*scene.increment;
        const float black[]={0,0,0,1}; scene.commands->ClearRenderTargetView(target,black,0,nullptr);
        scene.commands->OMSetRenderTargets(1,&target,FALSE,nullptr); scene.commands->ExecuteBundle(bundles[eye].Get());
    }
    Hr(scene.commands->Close()); ID3D12CommandList* lists[]={scene.commands.Get()}; scene.queue->ExecuteCommandLists(1,lists); scene.Wait();
    Readback(scene,15); scene.DebugCheck();
}
int wmain(int argc,wchar_t** argv) {
    try {
        Check(argc==2,"expected --intercept or --passthrough");
        bool intercepted=std::wstring(argv[1])==L"--intercept";
        Check(intercepted || std::wstring(argv[1])==L"--passthrough","unknown fixture mode");
        auto module=GetModuleHandleW(L"d3d12.dll"); Check(module!=nullptr,"D3D12 import loaded");
        using GetCounts=HRESULT(WINAPI*)(vrx::dx12::Counts*,UINT);
        auto counts=reinterpret_cast<GetCounts>(GetProcAddress(module,"VRX_DX12_GetCounts")); Check(counts!=nullptr,"app-local VRX proxy was loaded");
        vrx::dx12::Counts before; Hr(counts(&before,sizeof(before)));
        Check(!before.devices && !before.hookFailures,"proxy is passive before a device exists");
        Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),nullptr)==S_FALSE,"device capability probe remains S_FALSE");
        RootSignatureExports();
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        {
            Scene scene; scene.Init(); Identity(scene);
            for(UINT frame=1;frame<=12;++frame) { scene.Draw(frame); Readback(scene,frame); scene.DebugCheck(); }
            Bundles(scene);
        }
        std::mutex mutex; std::exception_ptr failure; std::vector<std::thread> workers;
        for(UINT i=0;i<3;++i) workers.emplace_back([&] {
            try {
                Scene scene; scene.Init(); Identity(scene); scene.Draw(9); Readback(scene,9); scene.DebugCheck();
            } catch(...) { std::lock_guard<std::mutex> lock(mutex); failure=std::current_exception(); }
        });
        for(auto& worker:workers) worker.join(); if(failure) std::rethrow_exception(failure);
        vrx::dx12::Counts after; Hr(counts(&after,sizeof(after)));
        Check(counts(nullptr,sizeof(after))==E_INVALIDARG && counts(&after,1)==E_INVALIDARG,"diagnostic ABI rejects invalid buffers");
        Check(after.bytes==sizeof(after) && after.version==4 && !after.hookFailures,"native hooks installed without collisions or errors");
        if(intercepted) Check(after.devices>=4 && after.queues>=4 && after.commandLists>=4 && after.resources>=8 && after.pipelines>=4 &&
            after.draws==30 && after.indexedDraws==2 && after.bundles==2 && after.resets>=30 && after.closes>=30 && after.submissions==32,"native graphics calls were intercepted once at the app boundary");
        else Check(!after.devices && !after.queues && !after.commandLists && !after.resources && !after.draws && !after.submissions,"disabled proxy never installs hooks");
        std::printf("PASS: native DX12 %s; original COM identity; pixels, root signatures, concurrent devices; draws=%llu submissions=%llu\n",
            intercepted?"interception":"passthrough",static_cast<unsigned long long>(after.draws),static_cast<unsigned long long>(after.submissions));
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
