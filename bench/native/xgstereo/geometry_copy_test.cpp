#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "../openxr/geometry_copy.h"
#include "../openxr/geometry_fit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static void Hr(HRESULT result, const char* what) { Check(SUCCEEDED(result), what); }
constexpr UINT W = 18, H = 10; // deliberately not a 256-byte row pitch

static ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT width, UINT slices, DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = width; d.Height = H;
    d.DepthOrArraySize = (UINT16)slices; d.MipLevels = 1; d.Format = format;
    d.SampleDesc.Count = 1; d.Flags = flags;
    ComPtr<ID3D12Resource> result;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&result)), "create test texture");
    return result;
}
static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&result)), "create test buffer");
    return result;
}

int main(int argc,char** argv)
{
    try
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<ID3D12Device> device;ComPtr<IDXGIAdapter> adapter;
        if(argc==2 && !std::strcmp(argv[1],"--warp")){ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP adapter");}
        Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "create D3D12 device");
        ComPtr<ID3D12InfoQueue> messages;
        device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        Hr(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "create queue");
        ComPtr<ID3D12CommandAllocator> allocator;
        Hr(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&allocator)), "create allocator");
        ComPtr<ID3D12GraphicsCommandList> cl;
        Hr(device->CreateCommandList(0, qd.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&cl)), "create commands");
        Hr(cl->Close(), "close initial list");
        ComPtr<ID3D12Fence> fence;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create fence");

        auto source = Texture(device.Get(), W * 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        auto pair = Texture(device.Get(), W, 2, DXGI_FORMAT_R8G8B8A8_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto light = Texture(device.Get(), W, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        const auto sd = source->GetDesc(), pd = pair->GetDesc(), ld = light->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sf{}, pf[2]{}, lf{};
        UINT64 inputBytes = 0, pairBytes = 0, total = 0;
        device->GetCopyableFootprints(&sd, 0, 1, 0, &sf, nullptr, nullptr, &inputBytes);
        device->GetCopyableFootprints(&pd, 0, 2, 0, pf, nullptr, nullptr, &pairBytes);
        const UINT64 lightBase = (pairBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1);
        device->GetCopyableFootprints(&ld, 0, 1, lightBase, &lf, nullptr, nullptr, &total);
        total += lightBase; // GetCopyableFootprints returns a size excluding BaseOffset
        auto upload = Buffer(device.Get(), inputBytes, D3D12_HEAP_TYPE_UPLOAD);
        auto readback = Buffer(device.Get(), total, D3D12_HEAP_TYPE_READBACK);
        uint8_t* input = nullptr;
        Hr(upload->Map(0, nullptr, reinterpret_cast<void**>(&input)), "map upload");
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W * 2; ++x)
            {
                auto* px = input + sf.Offset + size_t(y) * sf.Footprint.RowPitch + x * 4;
                px[0] = x < W ? 241 : 13;
                px[1] = static_cast<uint8_t>(x < W ? x : x - W);
                px[2] = static_cast<uint8_t>(y * 17); px[3] = 255;
            }
        upload->Unmap(0, nullptr);

        // Upload on the first frame, reuse the same source on the next two.
        for (UINT iteration = 0; iteration < 3; ++iteration)
        {
            const bool stereo = iteration != 1, uploaded = iteration == 0;
            Hr(allocator->Reset(), "reset allocator"); Hr(cl->Reset(allocator.Get(), nullptr), "reset commands");
            if (uploaded)
            {
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = sf;
                dst.pResource = source.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            Check(!RecordGeometryCopy(cl.Get(), source.Get(), pair.Get(), light.Get(), W + 2, H, stereo, uploaded), "reject mismatched dimensions before recording");
            Check(RecordGeometryCopy(cl.Get(), source.Get(), pair.Get(), light.Get(), W, H, stereo, uploaded), "record real eye-slice copy");
            for (UINT eye = 0; eye < 2; ++eye)
            {
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.pResource = pair.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = eye;
                dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = pf[eye];
                cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            GeometryBarrier(cl.Get(), light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = light.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = lf;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            GeometryBarrier(cl.Get(), light.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            GeometryBarrier(cl.Get(), pair.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const auto closed = cl->Close();
            if (FAILED(closed) && messages)
                for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i)
                {
                    SIZE_T bytes = 0; messages->GetMessage(i, nullptr, &bytes);
                    std::vector<uint8_t> storage(bytes);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    if (SUCCEEDED(messages->GetMessage(i, message, &bytes))) std::fprintf(stderr, "%s\n", message->pDescription);
                }
            Hr(closed, "close commands");
            ID3D12CommandList* lists[] = {cl.Get()}; queue->ExecuteCommandLists(1, lists);
            Hr(queue->Signal(fence.Get(), iteration + 1), "signal fence");
            const auto deadline = GetTickCount64() + 10000;
            while (fence->GetCompletedValue() < iteration + 1 && GetTickCount64() < deadline) Sleep(1);
            Check(fence->GetCompletedValue() == iteration + 1, "GPU copy completed without removal");
            uint8_t* output = nullptr;
            Hr(readback->Map(0, nullptr, reinterpret_cast<void**>(&output)), "map result");
            for (UINT image = 0; image < 3; ++image)
                for (UINT y = 0; y < H; ++y)
                    for (UINT x = 0; x < W; ++x)
                    {
                        const auto& footprint = image == 2 ? lf : pf[image];
                        const auto* px = output + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch + x * 4;
                        const bool right = image == 1 && stereo;
                        Check(px[0] == (right ? 13 : 241) && px[1] == x && px[2] == y * 17 && px[3] == 255,
                            "eye identity, edge pixels, row pitch, alpha and mono lighting preserved");
                    }
            readback->Unmap(0, nullptr);
        }
        UINT64 completed=3;
        const UINT sizes[][2]={{13,13},{37,11},{3,5},{W,H}};
        for(const auto& size:sizes) {
            GeometryFit fit;ID3D12Resource* inputs[]={source.Get(),source.Get()};
            Check(fit.Create(device.Get(),inputs,2,size[0],size[1]),"create eye-isolated GPU fit");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;const auto desc=fit.Output()->GetDesc();
            device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);auto result=Buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK);
            Hr(allocator->Reset(),"reset fit allocator");Hr(cl->Reset(allocator.Get(),nullptr),"reset fit list");
            Check(!fit.Record(cl.Get(),2,false),"reject invalid source ring slot");Check(fit.Record(cl.Get(),1,false),"fit a complete stereo pair");
            GeometryBarrier(cl.Get(),fit.Output(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=fit.Output();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource=result.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            GeometryBarrier(cl.Get(),fit.Output(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);Hr(cl->Close(),"close fit list");
            ID3D12CommandList* lists[]={cl.Get()};queue->ExecuteCommandLists(1,lists);Hr(queue->Signal(fence.Get(),++completed),"signal fit completion");
            const auto until=GetTickCount64()+5000;while(fence->GetCompletedValue()<completed && GetTickCount64()<until)Sleep(1);
            Check(fence->GetCompletedValue()==completed,"fit GPU completed");uint8_t* mapped=nullptr;Hr(result->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"read fitted pair");
            const double scale=std::min(double(size[0])/W,double(size[1])/H),dx=(size[0]-W*scale)/2,dy=(size[1]-H*scale)/2;
            for(UINT eye=0;eye<2;++eye)for(UINT y=0;y<size[1];++y)for(UINT x=0;x<size[0];++x) {
                const auto* pixel=mapped+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch+(eye*size[0]+x)*4;
                const double px=x+.5-dx,py=y+.5-dy;const bool inside=px>=0 && py>=0 && px<W*scale && py<H*scale;
                const auto green=std::clamp(px/scale-.5,0.,double(W-1)),blue=std::clamp(py/scale-.5,0.,double(H-1))*17;
                Check(pixel[3]==255 && pixel[0]==(inside?(eye?13:241):0),"letterbox alpha and strict eye isolation");
                Check(std::abs(int(pixel[1])-int(std::lround(inside?green:0)))<=1 && std::abs(int(pixel[2])-int(std::lround(inside?blue:0)))<=1,"GPU fit matches independent linear-ramp reference");
            }
            result->Unmap(0,nullptr);
        }
        if (messages)
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i)
            {
                SIZE_T bytes = 0; messages->GetMessage(i, nullptr, &bytes);
                std::vector<uint8_t> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Hr(messages->GetMessage(i, message, &bytes), "read debug diagnostics");
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                { std::fprintf(stderr, "%s\n", message->pDescription); Check(false, "D3D12 validation error"); }
            }
        std::puts("PASS: D3D12 eye-slice copy, left-eye lighting, mono toggle, source reuse, invalid dimensions, GPU validation");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
