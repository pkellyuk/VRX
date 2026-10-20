// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Real IA mesh inputs and camera CBVs uploaded on a separate COPY queue.
#include "mirror_scene.h"
int main(int argc,char** argv){try {
    const bool warp=argc==3 && !std::strcmp(argv[2],"--warp");Check(argc==2 || warp,"known mesh fixture adapter");
    const auto module=GetModuleHandleW(L"d3d12.dll");counts=reinterpret_cast<CountsFn>(GetProcAddress(module,"VRX_DX12_GetCounts"));twin=reinterpret_cast<TwinFn>(GetProcAddress(module,"VRX_DX12_GetTwin"));scope=reinterpret_cast<ScopeFn>(GetProcAddress(module,"VRX_DX12_DiagnosticScope"));Check(counts && twin && scope,"mesh diagnostics loaded");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    {
        ComPtr<IDXGIAdapter> adapter;if(warp){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));}
        Scene scene;scene.Init(adapter.Get());
        D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=1;rd.pParameters=&parameter;rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob,error;Hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));scene.root.Reset();Hr(scene.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&scene.root)));
        constexpr char shader[]=R"(
cbuffer Camera:register(b0){float clipW;float3 color;};
float4 VS(float3 position:POSITION):SV_Position{return float4(position*clipW,clipW);}
float4 PS():SV_Target{return float4(color,1);}
)";
        ComPtr<ID3DBlob> vs,ps;Hr(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"VS","vs_5_1",0,0,&vs,&error));Hr(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PS","ps_5_1",0,0,&ps,&error));
        const D3D12_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=scene.root.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};p.InputLayout={&element,1};p.SampleMask=UINT_MAX;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask=15;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;p.SampleDesc.Count=1;
        scene.pipeline.Reset();if(ModernMode())GraphicsStream(scene.device.Get(),p,scene.pipeline);else Hr(scene.device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&scene.pipeline)));
        D3D12_HEAP_DESC heap{};heap.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;heap.SizeInBytes=3*D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;heap.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        ComPtr<ID3D12Heap> memory;Hr(scene.device->CreateHeap(&heap,IID_PPV_ARGS(&memory)));
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=1024;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> mesh,camera,upload;Hr(scene.device->CreatePlacedResource(memory.Get(),D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&mesh)));Hr(scene.device->CreatePlacedResource(memory.Get(),2*D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&camera)));
        D3D12_HEAP_PROPERTIES input{};input.Type=D3D12_HEAP_TYPE_UPLOAD;Hr(scene.device->CreateCommittedResource(&input,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
        D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_COPY;ComPtr<ID3D12CommandQueue> copy;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12Fence> ready;
        Hr(scene.device->CreateCommandQueue(&qd,IID_PPV_ARGS(&copy)));Hr(scene.device->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&allocator)));Hr(scene.device->CreateCommandList(0,qd.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));Hr(commands->Close());Hr(scene.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ready)));
        for(UINT frame=1;frame<=12;++frame){
            scene.Wait();Hr(allocator->Reset());Hr(commands->Reset(allocator.Get(),nullptr));
            void* bytes=nullptr;Hr(upload->Map(0,nullptr,&bytes));std::memset(bytes,0,1024);
            // Nonzero resource/view offsets and base vertex exercise address translation.
            const float vertices[]={9,9,9,-.35f,-.5f,.5f,0,.5f,.5f,.35f,-.5f,.5f};const UINT indices[]={99,0,1,2};const float values[]={frame%2?1.f:2.f,float(frame+1)/32,.5f,.75f};
            std::memcpy(static_cast<BYTE*>(bytes)+64,vertices,sizeof(vertices));std::memcpy(static_cast<BYTE*>(bytes)+256,indices,sizeof(indices));std::memcpy(static_cast<BYTE*>(bytes)+512,values,sizeof(values));upload->Unmap(0,nullptr);
            commands->CopyBufferRegion(mesh.Get(),0,upload.Get(),0,512);commands->CopyBufferRegion(camera.Get(),256,upload.Get(),512,256);Hr(commands->Close());ID3D12CommandList* uploaded[]={commands.Get()};copy->ExecuteCommandLists(1,uploaded);Hr(copy->Signal(ready.Get(),frame));Hr(scene.queue->Wait(ready.Get(),frame));
            Hr(scene.allocator->Reset());Hr(scene.commands->Reset(scene.allocator.Get(),nullptr));scene.commands->SetPipelineState(scene.pipeline.Get());scene.commands->SetGraphicsRootSignature(scene.root.Get());scene.commands->SetGraphicsRootConstantBufferView(0,camera->GetGPUVirtualAddress()+256);
            D3D12_VIEWPORT viewport{0,0,float(scene.width),float(scene.height),0,1};D3D12_RECT rect{0,0,LONG(scene.width),LONG(scene.height)};scene.commands->RSSetViewports(1,&viewport);scene.commands->RSSetScissorRects(1,&rect);scene.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const D3D12_VERTEX_BUFFER_VIEW vertex{mesh->GetGPUVirtualAddress()+64,UINT(sizeof(vertices)),12};const D3D12_INDEX_BUFFER_VIEW index{mesh->GetGPUVirtualAddress()+256,UINT(sizeof(indices)),DXGI_FORMAT_R32_UINT};scene.commands->IASetVertexBuffers(0,1,&vertex);scene.commands->IASetIndexBuffer(&index);
            const auto target=scene.rtv->GetCPUDescriptorHandleForHeapStart();const float black[]={0,0,0,1};scene.commands->ClearRenderTargetView(target,black,0,nullptr);scene.commands->OMSetRenderTargets(1,&target,FALSE,nullptr);scene.commands->DrawIndexedInstanced(3,1,1,1,0);Hr(scene.commands->Close());ID3D12CommandList* rendered[]={scene.commands.Get()};scene.queue->ExecuteCommandLists(1,rendered);scene.Wait();
            const auto pixels=Capture(scene);const auto shift=Pixels(scene,pixels,frame);wchar_t zero[8]{};const bool identical=(GetEnvironmentVariableW(L"VRX_DX12_SHIFT",zero,8)==1 && zero[0]==L'0') || frame%2!=0;Check(identical?std::abs(shift)<.1:shift>7,"mesh replay has calibrated disparity");if(identical)Pixels(scene,pixels,frame,true);scene.DebugCheck();
        }
    }
    vrx::dx12::Counts result;Hr(counts(&result,sizeof(result)));Check(result.mirrorReplays==24 && !result.mirrorStops && !result.hookFailures && !result.mirrorLiveObjects,"mesh copy/direct batches replay and retire");std::puts("PASS: native DX12 placed mesh buffers, COPY queue dependencies, GPU camera CBVs, 12 same-frame pixel comparisons and retirement");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
