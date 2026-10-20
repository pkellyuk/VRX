// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <stdexcept>
#include <string>
namespace vrx::dx12::fixture {
using Microsoft::WRL::ComPtr;
inline void Hr(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("D3D12 scene HRESULT "+std::to_string(uint32_t(hr))); }
inline void Check(bool value,const char* reason) { if(!value) throw std::runtime_error(reason); }
struct Scene {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> rtv;
    ComPtr<ID3D12Resource> eyes;
    ComPtr<ID3D12Fence> completed;
    ComPtr<ID3D12InfoQueue> messages;
    uint64_t value = 0;
    UINT width = 96, height = 64, increment = 0;
    void Init() {
        Hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        Hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
        Hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands))); Hr(commands->Close());
        Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completed)));
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=width; d.Height=height;
        d.DepthOrArraySize=2; d.MipLevels=1; d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{}; clear.Format=d.Format; clear.Color[3]=1;
        Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,&clear,IID_PPV_ARGS(&eyes)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=2;
        Hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv))); increment=device->GetDescriptorHandleIncrementSize(hd.Type);
        for(UINT i=0;i<2;++i) {
            D3D12_RENDER_TARGET_VIEW_DESC v{}; v.Format=d.Format; v.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray.FirstArraySlice=i; v.Texture2DArray.ArraySize=1;
            auto h=rtv->GetCPUDescriptorHandleForHeapStart(); h.ptr+=SIZE_T(i)*increment;
            device->CreateRenderTargetView(eyes.Get(),&v,h);
        }
        D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.Num32BitValues=4; parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=1; rd.pParameters=&parameter;
        ComPtr<ID3DBlob> serialized, errors; Hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors));
        Hr(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)));
        constexpr char shader[]=R"(
cbuffer Calibration : register(b0) { float offset; float3 color; };
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 positions[3] = {float2(-0.35,-0.5),float2(0,0.5),float2(0.35,-0.5)};
    return float4(positions[id]+float2(offset,0),0.5,1);
}
float4 PS() : SV_Target { return float4(color,1); }
)";
        ComPtr<ID3DBlob> vs,ps;
        Hr(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"VS","vs_5_1",D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&vs,&errors));
        Hr(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PS","ps_5_1",D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&ps,&errors));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature=root.Get();
        p.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        p.SampleMask=UINT_MAX; p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable=TRUE; p.DepthStencilState.DepthEnable=FALSE; p.DepthStencilState.StencilEnable=FALSE;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; p.NumRenderTargets=1;
        p.RTVFormats[0]=d.Format; p.SampleDesc.Count=1;
        Hr(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline)));
    }
    void Wait() {
        Hr(queue->Signal(completed.Get(),++value));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Check(event!=nullptr,"create completion event");
        const HRESULT set=completed->SetEventOnCompletion(value,event);
        const auto waited=SUCCEEDED(set)?WaitForSingleObject(event,5000):WAIT_FAILED; CloseHandle(event); Hr(set);
        Check(waited==WAIT_OBJECT_0,"test GPU did not finish");
    }
    void Draw(UINT frame) {
        Wait(); Hr(allocator->Reset()); Hr(commands->Reset(allocator.Get(),pipeline.Get()));
        commands->SetGraphicsRootSignature(root.Get());
        D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1}; D3D12_RECT rect{0,0,LONG(width),LONG(height)};
        commands->RSSetViewports(1,&viewport); commands->RSSetScissorRects(1,&rect); commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for(UINT eye=0;eye<2;++eye) {
            auto h=rtv->GetCPUDescriptorHandleForHeapStart(); h.ptr+=SIZE_T(eye)*increment;
            const float black[]={0,0,0,1}; commands->ClearRenderTargetView(h,black,0,nullptr); commands->OMSetRenderTargets(1,&h,FALSE,nullptr);
            // Both draws consume the same simulation-frame color, with independent view offsets.
            const float constants[]={eye==0?-0.2f:0.2f,float(frame%3+1)/4.0f,0.5f,0.75f};
            commands->SetGraphicsRoot32BitConstants(0,4,constants,0); commands->DrawInstanced(3,1,0,0);
        }
        Hr(commands->Close()); ID3D12CommandList* lists[]={commands.Get()}; queue->ExecuteCommandLists(1,lists);
    }
    void DebugCheck() {
        if(!messages) return;
        for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
            SIZE_T bytes=0; Hr(messages->GetMessage(i,nullptr,&bytes)); std::vector<uint8_t> storage(bytes);
            auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); Hr(messages->GetMessage(i,m,&bytes));
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(m->pDescription);
        }
        messages->ClearStoredMessages();
    }
};

}
