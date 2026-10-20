// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Ordinary windowed DX12 app. Only the diagnostic consumer knows about VRX.
#include "mirror_scene.h"
#include "../xgstereo/gpu_test_support.h"
#include <memory>
namespace {
struct Window {
    HWND handle=CreateWindowW(L"STATIC",L"VRX native DX12 presentation fixture",WS_OVERLAPPEDWINDOW,0,0,160,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ~Window(){if(handle)DestroyWindow(handle);}
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
        const std::string argument=argv[1];const bool serve=argument.rfind("--serve-gpu=",0)==0;
        const bool legacy=argument=="--legacy",warp=argc==3 && std::strcmp(argv[2],"--warp")==0;
        Check(legacy || serve || argument=="--present","known presentation fixture mode");
        Check(argc!=3 || warp,"known adapter option");
        const auto module=GetModuleHandleW(L"d3d12.dll");
        counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));
        Check(counts && scope,"native proxy diagnostics loaded");
        const auto suffix=serve?argument.substr(12):std::string{};
        const auto session=serve?std::wstring(suffix.begin(),suffix.end()):L"dx12-present-"+std::to_wstring(GetCurrentProcessId());
        Check(xg::ValidSession(session) && SetEnvironmentVariableW(L"VRX_XG_SESSION",session.c_str()),"set isolated stream name");
        ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        Window window;Check(window.handle!=nullptr,"create owned test window");
        std::unique_ptr<xg::gpu_test::Snapshot> consumer;
        {
            // Factory creation deliberately predates device interception.
            ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            ComPtr<IDXGIAdapter> adapter;if(warp)Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
            Scene scene;scene.Init(adapter.Get());Pipeline(scene);
            ComPtr<IDXGISwapChain3> chain;
            if(legacy) {
                DXGI_SWAP_CHAIN_DESC d{};d.BufferDesc.Width=scene.width;d.BufferDesc.Height=scene.height;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;d.OutputWindow=window.handle;d.Windowed=TRUE;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
                ComPtr<IDXGISwapChain> base;Hr(factory->CreateSwapChain(scene.queue.Get(),&d,&base));Hr(base.As(&chain));
            } else {
                DXGI_SWAP_CHAIN_DESC1 d{};d.Width=scene.width;d.Height=scene.height;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
                ComPtr<IDXGISwapChain1> base;Hr(factory->CreateSwapChainForHwnd(scene.queue.Get(),window.handle,&d,nullptr,nullptr,&base));Hr(base.As(&chain));
            }
            if(serve) {
                for(UINT frame=1;frame<10000;++frame) {
                    scene.eyes.Reset();Hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&scene.eyes)));
                    scene.device->CreateRenderTargetView(scene.eyes.Get(),nullptr,scene.rtv->GetCPUDescriptorHandleForHeapStart());
                    Frame(scene,frame%24,2,false,frame%3==0,nullptr,true);Hr(chain->Present(0,0));scene.DebugCheck();Sleep(5);
                }
                return 0;
            }
            for(UINT frame=1;frame<=18;++frame) {
                if(frame==7 || frame==13) {
                    scene.Wait();scene.eyes.Reset();scene.width=frame==7?129:97;scene.height=frame==7?73:65;
                    if(frame==7)Hr(chain->ResizeBuffers(2,scene.width,scene.height,DXGI_FORMAT_UNKNOWN,0));
                    else {const UINT nodes[]={1,1};IUnknown* queues[]={scene.queue.Get(),scene.queue.Get()};Hr(chain->ResizeBuffers1(2,scene.width,scene.height,DXGI_FORMAT_UNKNOWN,0,nodes,queues));}
                }
                scene.eyes.Reset();Hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&scene.eyes)));
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
        {Diagnostic diagnostic;consumer.reset();}
        vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));
        Check(result.version==3 && !result.hookFailures && !result.mirrorStops,"presentation has no hook or replay failures");
        Check(result.swapchains==1 && result.presentations==18 && result.publications==18 && !result.presentDrops && result.resizes==2,"one publication per real Present and two resize transactions");
        Check(result.devices==1 && result.mirrorReplays==18 && result.draws==12 && result.indexedDraws==6,"the app renders once; internal publisher and consumer are excluded from interception");
        Check(!result.mirrorLiveObjects,"swapchain and all shadow objects retire after app teardown");
        std::puts("PASS: native DX12 swapchain Present/Present1, GPU same-frame stereo, two resizes, odd extents, HUD, original view and cleanup");return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
