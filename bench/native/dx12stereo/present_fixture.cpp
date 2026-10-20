// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Ordinary windowed DX12 app. Only the diagnostic consumer knows about VRX.
#include "mirror_scene.h"
#include "../xgstereo/gpu_test_support.h"
#include <memory>
namespace {
struct Window {
    HWND handle=CreateWindowW(L"STATIC",L"VRX native DX12 presentation fixture",WS_OVERLAPPEDWINDOW,0,0,160,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ~Window(){if(handle && IsWindow(handle))DestroyWindow(handle);}
    bool Pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}return IsWindow(handle)!=FALSE;}
};
xg::PairFrame Read(xg::gpu_test::Snapshot& snapshot) {
    Diagnostic diagnostic;xg::PairFrame pair;const auto until=GetTickCount64()+5000;
    while(!snapshot.Read(pair) && GetTickCount64()<until)Sleep(1);
    Check(!pair.rgba.empty(),"native swapchain pair arrived through shared GPU textures");return pair;
}
}
int main(int argc,char** argv) {
    try {
        Check(argc>=2 && argc<=3,"presentation fixture mode supplied");
        const std::string argument=argv[1];const bool resizeServe=argument.rfind("--serve-resize-gpu=",0)==0;
        const bool serve=resizeServe || argument.rfind("--serve-gpu=",0)==0;
        const bool legacy=argument=="--legacy",warp=argc==3 && std::strcmp(argv[2],"--warp")==0;
        const bool stopReplay=serve && argc==3 && std::strcmp(argv[2],"--stop-replay")==0;
        Check(legacy || serve || argument=="--present","known presentation fixture mode");
        Check(argc!=3 || warp || stopReplay || (serve && std::string(argv[2]).rfind("--fixture-token=",0)==0),"known adapter/fixture option");
        wchar_t formatOption[3]{};const bool bgra=GetEnvironmentVariableW(L"VRX_DX12_TEST_BGRA",formatOption,3)==1 && formatOption[0]==L'1';
        const bool ten=GetEnvironmentVariableW(L"VRX_DX12_TEST_TEN",formatOption,3)==1 && formatOption[0]==L'1';
        auto format=ten?DXGI_FORMAT_R10G10B10A2_UNORM:bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        const auto module=GetModuleHandleW(L"d3d12.dll");
        counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));
        Check((counts && scope) || (serve && !counts && !scope),"native proxy diagnostics loaded outside the ordinary serving mode");
        const auto suffix=serve?argument.substr(resizeServe?19:12):std::string{};
        const auto session=serve?std::wstring(suffix.begin(),suffix.end()):L"dx12-present-"+std::to_wstring(GetCurrentProcessId());
        Check(xg::ValidSession(session) && SetEnvironmentVariableW(L"VRX_XG_SESSION",session.c_str()),"set isolated stream name");
        ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        Window window;Check(window.handle!=nullptr,"create owned test window");
        if(serve)ShowWindow(window.handle,SW_SHOWNOACTIVATE);
        std::unique_ptr<xg::gpu_test::Snapshot> consumer;
        {
            // Factory creation deliberately predates device interception.
            ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            ComPtr<IDXGIAdapter> adapter;if(warp)Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
            Scene scene;scene.Init(adapter.Get());Pipeline(scene,false,format);
            ComPtr<IDXGISwapChain3> chain;
            if(legacy) {
                DXGI_SWAP_CHAIN_DESC d{};d.BufferDesc.Width=scene.width;d.BufferDesc.Height=scene.height;d.BufferDesc.Format=format;
                d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;d.OutputWindow=window.handle;d.Windowed=TRUE;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
                ComPtr<IDXGISwapChain> base;Hr(factory->CreateSwapChain(scene.queue.Get(),&d,&base));Hr(base.As(&chain));
            } else {
                DXGI_SWAP_CHAIN_DESC1 d{};d.Width=scene.width;d.Height=scene.height;d.Format=format;
                d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
                ComPtr<IDXGISwapChain1> base;Hr(factory->CreateSwapChainForHwnd(scene.queue.Get(),window.handle,&d,nullptr,nullptr,&base));Hr(base.As(&chain));
            }
            Hr(chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
            Check(FAILED(chain->SetColorSpace1(DXGI_COLOR_SPACE_CUSTOM)),"native runtime rejects invalid color space without stopping replay");
            if(serve) {
                struct Events {HANDLE ready[2]{},stop=nullptr;~Events(){for(auto event:ready)if(event)CloseHandle(event);if(stop)CloseHandle(stop);}} events;
                events.stop=CreateEventW(nullptr,TRUE,FALSE,(L"Local\\VRX.dx12."+session+L".fixture-stop").c_str());Check(events.stop!=nullptr,"create fixture stop event");
                if(resizeServe)for(UINT i=0;i<2;++i){const auto name=L"Local\\VRX.geometry."+session+L".resize-check."+std::to_wstring(i);events.ready[i]=CreateEventW(nullptr,FALSE,FALSE,name.c_str());Check(events.ready[i]!=nullptr,"create resize handshake");}
                UINT step=0;
                for(UINT frame=1;frame<10000;++frame) {
                    if(!window.Pump())break;
                    if(WaitForSingleObject(events.stop,0)==WAIT_OBJECT_0)break;
                    if(resizeServe && step<2 && WaitForSingleObject(events.ready[step],0)==WAIT_OBJECT_0) {
                        scene.Wait();scene.eyes.Reset();scene.width=step?97:129;scene.height=step?65:73;
                        if(bgra && step==1)format=DXGI_FORMAT_R8G8B8A8_UNORM;
                        Hr(chain->ResizeBuffers(2,scene.width,scene.height,format,0));if(bgra && step==1)Pipeline(scene,false,format);++step;
                    }
                    scene.eyes.Reset();Hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&scene.eyes)));
                    scene.device->CreateRenderTargetView(scene.eyes.Get(),nullptr,scene.rtv->GetCPUDescriptorHandleForHeapStart());
                    Frame(scene,frame%24,2,stopReplay && frame>=2,frame%3==0,nullptr,true);Hr(chain->Present(0,0));scene.DebugCheck();Sleep(5);
                }
                scene.Wait();scene.eyes.Reset();chain.Reset();
                if(counts){vrx::dx12::Counts retired;Hr(counts(&retired,sizeof(retired)));}
            }
            else {
            for(UINT frame=1;frame<=18;++frame) {
                if(frame==7 || frame==13) {
                    scene.Wait();scene.eyes.Reset();scene.width=frame==7?129:97;scene.height=frame==7?73:65;
                    if(bgra && frame==13)format=DXGI_FORMAT_R8G8B8A8_UNORM;
                    if(frame==7)Hr(chain->ResizeBuffers(2,scene.width,scene.height,DXGI_FORMAT_UNKNOWN,0));
                    else {const UINT nodes[]={1,1};IUnknown* queues[]={scene.queue.Get(),scene.queue.Get()};Hr(chain->ResizeBuffers1(2,scene.width,scene.height,format,0,nodes,queues));}
                    if(bgra && frame==13)Pipeline(scene,false,format);
                }
                scene.eyes.Reset();Hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&scene.eyes)));
                Check(scene.eyes->GetDesc().Format==format,"native swapchain uses the requested source format");
                scene.device->CreateRenderTargetView(scene.eyes.Get(),nullptr,scene.rtv->GetCPUDescriptorHandleForHeapStart());
                vrx::dx12::Counts before;Hr(counts(&before,sizeof(before)));Hr(chain->Present(0,DXGI_PRESENT_TEST));
                vrx::dx12::Counts tested;Hr(counts(&tested,sizeof(tested)));Check(before.presentations==tested.presentations,"test-only Present never publishes");
                const float w=frame>=17?1.0f:frame%2?2.0f:4.0f;Frame(scene,frame,w,false,frame%3==0,nullptr,true);
                if(frame%2)Hr(chain->Present(0,0));else {DXGI_PRESENT_PARAMETERS p{};Hr(chain->Present1(0,0,&p));}
                if(frame==1 || frame==7 || frame==13) {
                    Diagnostic diagnostic;
                    if(consumer) {xg::GpuSlotInfo old;Check(consumer->reader.Status(0,old)==xg::PairRead::Invalid,"old reader detects replaced swapchain generation");}
                    auto next=std::make_unique<xg::gpu_test::Snapshot>();next->Init(scene.device->GetAdapterLuid(),scene.width,scene.height,session);
                    consumer=std::move(next);
                }
                auto pair=Read(*consumer);Check(pair.header.leftFrame==frame && pair.header.rightFrame==frame,"both eye IDs identify this single app frame");
                Check(pair.header.eyeWidth==scene.width && pair.header.eyeHeight==scene.height,"swapchain extent preserved per eye");
                const auto disparity=Pixels(scene,pair.rgba,frame,w==1);if(w!=1)Check(disparity>(w==2?7:3),"swapchain geometry stereo survives GPU publication");
                scene.Wait();scene.DebugCheck();
            }
            scene.eyes.Reset();scene.Wait();chain.Reset();
            vrx::dx12::Counts retired;xg::GpuSlotInfo closed;const auto until=GetTickCount64()+5000;
            while(GetTickCount64()<until) {Hr(counts(&retired,sizeof(retired)));if(consumer->reader.Status(0,closed)==xg::PairRead::Closed)break;Sleep(1);}
            Check(consumer->reader.Status(0,closed)==xg::PairRead::Closed,"swapchain destruction closes the shared stream");
            }
        }
        if(serve && !counts){std::puts("PASS: ordinary native DX12 fixture exited without a stereo bridge");return 0;}
        {Diagnostic diagnostic;consumer.reset();}
        vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));
        if(serve) {
            Check(!result.hookFailures && result.mirrorStops<=(stopReplay?1u:0u) && !result.mirrorLiveObjects,"serving fixture retires native and shadow objects cleanly");
            std::puts("PASS: serving fixture shut down cleanly");return 0;
        }
        Check(result.version==4 && !result.hookFailures && !result.mirrorStops,"presentation has no hook or replay failures");
        Check(result.swapchains==1 && result.presentations==18 && result.publications==18 && !result.presentDrops && result.resizes==2,"one publication per real Present and two resize transactions");
        Check(result.devices==1 && result.mirrorReplays==18 && result.draws==12 && result.indexedDraws==6,"the app renders once; internal publisher and consumer are excluded from interception");
        Check(!result.mirrorLiveObjects,"swapchain and all shadow objects retire after app teardown");
        std::puts("PASS: native DX12 swapchain Present/Present1, GPU same-frame stereo, two resizes, odd extents, HUD, original view and cleanup");return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
