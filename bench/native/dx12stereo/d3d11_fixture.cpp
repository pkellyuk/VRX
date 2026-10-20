// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// An unmodified-D3D11 launch fixture: no VRX API, Vulkan, or transport calls.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void Hr(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("D3D11 fixture HRESULT "+std::to_string(uint32_t(hr))); }
static LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window,message,w,l);
}
int wmain(int argc,wchar_t** argv) {
    HWND window=nullptr;
    try {
        WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"VRX.D3D11.Fixture";
        if(!RegisterClassW(&wc)) throw std::runtime_error("register fixture class");
        window=CreateWindowW(wc.lpszClassName,L"VRX DX11 integration fixture",WS_OVERLAPPEDWINDOW,
            40,40,336,280,nullptr,nullptr,wc.hInstance,nullptr);
        if(!window) throw std::runtime_error("create fixture window");
        ShowWindow(window,SW_SHOWNOACTIVATE);
        DXGI_SWAP_CHAIN_DESC swap{}; swap.BufferDesc.Width=320; swap.BufferDesc.Height=240;
        swap.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; swap.SampleDesc.Count=1;
        swap.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; swap.BufferCount=2;
        swap.OutputWindow=window; swap.Windowed=TRUE; swap.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; ComPtr<IDXGISwapChain> chain;
        Hr(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
            &swap,&chain,&device,nullptr,&context));
        ComPtr<ID3D11Texture2D> back; ComPtr<ID3D11RenderTargetView> target;
        Hr(chain->GetBuffer(0,IID_PPV_ARGS(&back))); Hr(device->CreateRenderTargetView(back.Get(),nullptr,&target));
        constexpr char source[]=R"(
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 p[3]={float2(-0.7,-1),float2(0,1),float2(0.7,-1)};
    return float4(p[id],1,2);
}
float4 PS() : SV_Target { return float4(0.25,0.5,0.75,1); }
)";
        ComPtr<ID3DBlob> vs,ps,error;
        Hr(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"VS","vs_5_0",D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&vs,&error));
        Hr(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"PS","ps_5_0",D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&ps,&error));
        ComPtr<ID3D11VertexShader> vertex; ComPtr<ID3D11PixelShader> pixel;
        Hr(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex));
        Hr(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel));
        D3D11_VIEWPORT viewport{0,0,320,240,0,1}; context->RSSetViewports(1,&viewport);
        context->VSSetShader(vertex.Get(),nullptr,0); context->PSSetShader(pixel.Get(),nullptr,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto* view=target.Get(); context->OMSetRenderTargets(1,&view,nullptr);
        bool selfTest=argc==2 && std::wstring(argv[1])==L"--self-test";
        bool done=false; UINT frames=0; const auto deadline=GetTickCount64()+120000;
        while(!done && GetTickCount64()<deadline) {
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message==WM_QUIT) done=true;
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if(done) break;
            const float clear[]={0,0,0,1}; context->ClearRenderTargetView(view,clear);
            context->Draw(3,0); Hr(chain->Present(0,0)); ++frames;
            if(selfTest && frames==8) break;
            Sleep(10);
        }
        context->ClearState(); context->Flush();
        if(IsWindow(window)) DestroyWindow(window);
        std::printf("PASS: ordinary D3D11 fixture rendered %u frames and exited normally\n",frames);
        return 0;
    } catch(const std::exception& e) {
        if(IsWindow(window)) DestroyWindow(window);
        std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1;
    }
}
