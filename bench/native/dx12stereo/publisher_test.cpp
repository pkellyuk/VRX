// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#include "publisher.h"
#include "test_scene.h"
#include "../xgstereo/gpu_test_support.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <stdexcept>

using namespace xg;
using xg::gpu_test::Hr;
static void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct Scene : vrx::dx12::fixture::Scene {
    xg::dx12::Eye Eye(UINT slice) { return {eyes.Get(),slice,D3D12_RESOURCE_STATE_RENDER_TARGET}; }
};
static PairFrame Read(xg::gpu_test::Snapshot& snapshot) {
    const auto until=GetTickCount64()+5000; PairFrame frame;
    while(!snapshot.Read(frame) && GetTickCount64()<until) Sleep(1);
    Check(!frame.rgba.empty(),"native same-frame pair did not arrive"); return frame;
}
static void Pixels(const PairFrame& frame, UINT expected) {
    Check(frame.header.leftFrame==expected && frame.header.rightFrame==expected,"both eyes must come from requested simulation frame");
    const auto w=frame.header.eyeWidth,h=frame.header.eyeHeight;
    uint64_t sum[2]{}, count[2]{};
    for(UINT eye=0;eye<2;++eye) for(UINT y=0;y<h;++y) for(UINT x=0;x<w;++x) {
        const auto at=(size_t(y)*w*2+eye*w+x)*4;
        const auto* pixel=frame.rgba.data()+at;
        Check(pixel[3]==255,"full eye image alpha");
        if(pixel[1]>100) {
            Check(abs(int(pixel[0])-int((expected%3+1)*255/4))<=1 && pixel[2]==191,"same-frame color is preserved through GPU sharing");
            sum[eye]+=x; ++count[eye];
        }
    }
    Check(count[0]>100 && count[1]>100 && double(count[0])/double(count[1])>.95 && double(count[0])/double(count[1])<1.05,"both full-height triangles survive copies");
    Check(double(sum[1])/count[1]-double(sum[0])/count[0]>w*.15,"geometry view offsets survive independently");
}
int wmain(int argc,wchar_t** argv) {
    try {
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        Scene scene;
        if(argc==2 && std::wstring(argv[1])==L"--full-resolution") { scene.width=2560; scene.height=1440; }
        if(argc==2 && std::wstring(argv[1])==L"--odd-extent") { scene.width=97; scene.height=65; }
        scene.Init();
        std::wstring session=L"native-dx12-"+std::to_wstring(GetCurrentProcessId());
        bool serve=false;
        if(argc==2 && std::wstring(argv[1]).rfind(L"--serve-gpu=",0)==0) { session=std::wstring(argv[1]).substr(12); serve=true; }
        xg::dx12::Publisher producer;
        Check(producer.Create(scene.queue.Get(),session,scene.width,scene.height),"create native D3D12 publisher");
        if(serve) {
            for(UINT frame=1;frame<10000;++frame) {
                scene.Draw(frame);
                const auto status=producer.Submit(frame,scene.Eye(0),scene.Eye(1));
                Check(status==xg::dx12::PublishResult::Published || status==xg::dx12::PublishResult::Dropped,"serve native frame");
                scene.Wait(); scene.DebugCheck(); Sleep(5);
            }
            return 0;
        }
        xg::gpu_test::Snapshot snapshot; snapshot.Init(scene.device->GetAdapterLuid(),scene.width,scene.height,session);
        { xg::dx12::Publisher duplicate; Check(!duplicate.Create(scene.queue.Get(),session,scene.width,scene.height),"duplicate session cannot replace a live producer"); }
        Check(!producer.Create(scene.queue.Get(),session,scene.width,scene.height),"producer cannot be initialized twice");
        {
            ComPtr<ID3D12CommandQueue> copy; D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_COPY;
            Hr(scene.device->CreateCommandQueue(&q,IID_PPV_ARGS(&copy)));
            xg::dx12::Publisher retry;
            Check(!retry.Create(copy.Get(),session+L"-retry",scene.width,scene.height),"copy queues do not accept render-state transitions");
            Check(!retry.Create(scene.queue.Get(),session+L"-retry",1,64),"invalid extent is rejected before allocating");
            Check(retry.Create(scene.queue.Get(),session+L"-retry",scene.width,scene.height),"initialization can recover after invalid inputs");
        }
        Check(producer.Submit(0,scene.Eye(0),scene.Eye(1))==xg::dx12::PublishResult::Invalid,"reject frame zero");
        Check(producer.Submit(1,scene.Eye(0),scene.Eye(0))==xg::dx12::PublishResult::Invalid,"reject a mono resource passed as stereo");
        Check(producer.Submit(1,scene.Eye(0),scene.Eye(2))==xg::dx12::PublishResult::Invalid,"reject out-of-range array slice");
        for(UINT frame=1;frame<=9;++frame) {
            scene.Draw(frame);
            Check(producer.Submit(frame,scene.Eye(0),scene.Eye(1))==xg::dx12::PublishResult::Published,"publish native geometry frame");
            Pixels(Read(snapshot),frame); scene.Wait(); scene.DebugCheck();
            Check(producer.Submit(frame,scene.Eye(0),scene.Eye(1))==xg::dx12::PublishResult::Invalid,"reject duplicate simulation frame");
        }
        // Stall queue execution deliberately. Reservations must stop at the ring
        // bound, without resetting in-flight allocators or blocking on the GPU.
        struct Gate { ComPtr<ID3D12Fence> fence; ~Gate() { if(fence) fence->Signal(1); } } gate;
        Hr(scene.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate.fence)));
        Hr(scene.queue->Wait(gate.fence.Get(),1));
        for(UINT frame=20;frame<23;++frame)
            Check(producer.Submit(frame,scene.Eye(0),scene.Eye(1))==xg::dx12::PublishResult::Published,"queue bounded in-flight copies");
        const auto before=GetTickCount64();
        Check(producer.Submit(23,scene.Eye(0),scene.Eye(1))==xg::dx12::PublishResult::Dropped,"full native ring drops safely");
        Check(GetTickCount64()-before<100,"full native ring never waits for GPU completion");
        Hr(gate.fence->Signal(1)); scene.Wait(); Read(snapshot); scene.DebugCheck();
        scene.Draw(24); Check(producer.Submit(24,scene.Eye(0),scene.Eye(1))==xg::dx12::PublishResult::Published,"recover after full-ring drop");
        Pixels(Read(snapshot),24); scene.Wait(); scene.DebugCheck();
        std::puts("PASS: native DX12 draws, same-frame shared textures, independent eye geometry, state restoration, in-flight ring pressure and recovery");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
