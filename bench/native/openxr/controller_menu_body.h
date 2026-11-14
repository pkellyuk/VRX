#pragma once
#include <array>
#include <d3dcompiler.h>

// All panel casings share one stereo projection layer in the atlas's spare
// eighth tile. This keeps seven windows within SteamVR's 16-layer limit.
class MenuBodyRenderer
{
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Vertex { float p[4], color[4]; };
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3D12PipelineState> pipeline;
    Ptr<ID3D12DescriptorHeap> targets;
    Ptr<ID3D12Resource> vertices;
    UINT step = 0;
    static constexpr int EyeWidth = vrxmenu::Width/2;
    static constexpr int TileX = vrxmenu::Width, TileY = vrxmenu::Height*3;
public:
    XrView eyes[2]{{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
    bool valid = false;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    bool Init(ID3D12Device* device, const std::vector<XrSwapchainImageD3D12KHR>& images, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN)
    {
        if (format == DXGI_FORMAT_UNKNOWN) format = images[0].texture->GetDesc().Format;
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Ptr<ID3DBlob> signature, vs, ps, errors;
        const char* shader = "struct V { float4 p:POSITION; float4 c:COLOR; }; struct O { float4 p:SV_Position; float4 c:COLOR; }; O vs(V v) { O o; o.p=v.p; o.c=v.c; return o; } float4 ps(O v):SV_Target { return v.c; }";
        if (FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors)) ||
            FAILED(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root))) ||
            FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&errors)) ||
            FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&errors))) return false;
        D3D12_INPUT_ELEMENT_DESC input[]{{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = root.Get(); p.VS = {vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS = {ps->GetBufferPointer(),ps->GetBufferSize()};
        p.InputLayout = {input,2}; p.SampleMask = UINT_MAX; p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable = TRUE;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        p.NumRenderTargets = 1; p.RTVFormats[0] = format; p.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = UINT(images.size());
        if (FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&targets)))) return false;
        step = device->GetDescriptorHandleIncrementSize(hd.Type);
        auto handle = targets->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = format; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (const auto& image : images) { device->CreateRenderTargetView(image.texture,&rv,handle); handle.ptr += step; }
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = 2*7*24*sizeof(Vertex);
        buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&vertices)));
    }
    template<class Surfaces> void Draw(ID3D12GraphicsCommandList* commands, uint32_t image, Surfaces& surfaces, XrSwapchain swapchain, XrSpace space)
    {
        if (!valid || !pipeline) return;
        std::vector<int> order;
        for (int s = 0; s <= vrxmenu::MaxSnapshots; s++) if (surfaces[s].visible) order.push_back(s);
        std::sort(order.begin(),order.end(),[&](int a,int b) {
            auto da=vrxmenu::Sub(surfaces[a].pose.position,eyes[0].pose.position), db=vrxmenu::Sub(surfaces[b].pose.position,eyes[0].pose.position);
            return vrxmenu::Dot(da,da)>vrxmenu::Dot(db,db); });
        std::vector<Vertex> mesh;
        for (int e = 0; e < 2; e++) for (int s : order)
        {
            auto& panel = surfaces[s];
            // Eight millimetres deep, inset to match the visible rounded border.
            const float w = panel.size.width*(.5f-16.f/960), h = panel.size.height*(.5f-16.f/768);
            XrVector3f corners[]{{-w,h,0},{w,h,0},{w,-h,0},{-w,-h,0}};
            const auto eye = eyes[e];
            const float l=tanf(eye.fov.angleLeft), r=tanf(eye.fov.angleRight), u=tanf(eye.fov.angleUp), d=tanf(eye.fov.angleDown);
            for (int side = 0; side < 4; side++)
            {
                const float shade[] = {.20f,.13f,.075f,.15f};
                for (int k : {0,1,2,0,2,3})
                {
                    auto local = corners[(side+(k==1 || k==2 ? 1:0))%4];
                    // Extend behind the bitmap towards the room interior when pinned.
                    bool back = vrxmenu::Rotate(vrxmenu::Inverse(panel.pose.orientation),vrxmenu::Sub(eyes[e].pose.position,panel.pose.position)).z < 0;
                    local.z = (k >= 2 ? .008f : 0) * (back ? 1:-1);
                    auto world=vrxmenu::Add(panel.pose.position,vrxmenu::Rotate(panel.pose.orientation,local));
                    auto p=vrxmenu::Rotate(vrxmenu::Inverse(eye.pose.orientation),vrxmenu::Sub(world,eye.pose.position));
                    float z=-p.z;
                    mesh.push_back({{2*p.x/(r-l)-(r+l)/(r-l)*z,2*p.y/(u-d)-(u+d)/(u-d)*z,z-.01f,z},
                        {shade[side]*.7f,shade[side],shade[side]*1.2f,1}});
                }
            }
        }
        void* data = nullptr; D3D12_RANGE empty{};
        if (FAILED(vertices->Map(0,&empty,&data))) return;
        memcpy(data,mesh.data(),mesh.size()*sizeof(Vertex)); vertices->Unmap(0,nullptr);
        auto target=targets->GetCPUDescriptorHandleForHeapStart(); target.ptr += image*step;
        D3D12_RECT rect{TileX,TileY,TileX+vrxmenu::Width,TileY+vrxmenu::Height}; float clear[4]{};
        commands->ClearRenderTargetView(target,clear,1,&rect); commands->OMSetRenderTargets(1,&target,FALSE,nullptr);
        commands->SetGraphicsRootSignature(root.Get()); commands->SetPipelineState(pipeline.Get());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VERTEX_BUFFER_VIEW vb{vertices->GetGPUVirtualAddress(),UINT(mesh.size()*sizeof(Vertex)),sizeof(Vertex)}; commands->IASetVertexBuffers(0,1,&vb);
        for (int e=0;e<2;e++)
        {
            D3D12_VIEWPORT viewport{float(TileX+e*EyeWidth),float(TileY),float(EyeWidth),float(vrxmenu::Height),0,1};
            D3D12_RECT scissor{TileX+e*EyeWidth,TileY,TileX+(e+1)*EyeWidth,TileY+vrxmenu::Height};
            commands->RSSetViewports(1,&viewport); commands->RSSetScissorRects(1,&scissor);
            commands->DrawInstanced(UINT(mesh.size()/2),1,UINT(e*mesh.size()/2),0);
            views[e].pose=eyes[e].pose; views[e].fov=eyes[e].fov;
            views[e].subImage={swapchain,{{TileX+e*EyeWidth,TileY},{EyeWidth,vrxmenu::Height}},0};
        }
        layer.space=space; layer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; layer.viewCount=2; layer.views=views;
    }
};
