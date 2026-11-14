#pragma once
#include <future>
#include <fstream>
#include <filesystem>

// Own command allocator/fence: snapshot readback never resets the frame renderer
// or waits for the GPU on the headset thread. File IO runs on a worker.
class ScreenSnapshotCapture
{
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> commands;
    Ptr<ID3D12Resource> readback;
    Ptr<ID3D12Resource> sourceTexture;
    Ptr<ID3D12Fence> fence;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT width = 0, height = 0;
    bool pending = false, rgba = false;
    std::future<std::wstring> writer;
public:
    bool Busy() const { return pending || writer.valid(); }
    bool Start(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* source)
    {
        if (Busy() || !source) return false;
        const auto desc = source->GetDesc();
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return false;
        width = UINT(desc.Width); height = desc.Height; rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
        sourceTexture = source;
        UINT64 bytes = 0; device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes;
        buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands))) ||
            FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) return false;
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.pResource = source;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.pResource = readback.Get(); to.PlacedFootprint = footprint;
        // Source-ring textures support simultaneous reads and decay to COMMON.
        commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        if (FAILED(commands->Close())) return false;
        ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1,lists);
        pending = SUCCEEDED(queue->Signal(fence.Get(),1));
        return pending;
    }
    bool Poll(std::wstring& path)
    {
        if (writer.valid() && writer.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        { path = writer.get(); return true; }
        if (!pending || fence->GetCompletedValue() < 1) return false;
        pending = false;
        void* mapped = nullptr;
        if (FAILED(readback->Map(0,nullptr,&mapped))) { path.clear(); return true; }
        const auto* data = static_cast<const uint8_t*>(mapped);
        std::vector<uint8_t> pixels(size_t(width)*height*4);
        for (UINT y = 0; y < height; ++y)
            memcpy(pixels.data()+size_t(y)*width*4,data+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch,size_t(width)*4);
        readback->Unmap(0,nullptr);
        sourceTexture.Reset();
        const UINT w = width, h = height; const bool swap = rgba;
        writer = std::async(std::launch::async,[pixels = std::move(pixels),w,h,swap]() mutable -> std::wstring
        {
            if (swap) for (size_t p = 0; p < pixels.size(); p += 4) std::swap(pixels[p],pixels[p+2]);
            for (size_t p = 3; p < pixels.size(); p += 4) pixels[p] = 255;
            wchar_t folder[MAX_PATH]{}, file[MAX_PATH]{};
            if (!GetTempPathW(MAX_PATH,folder) || !GetTempFileNameW(folder,L"VRX",0,file)) return {};
            BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header)+sizeof(BITMAPINFOHEADER);
            header.bfSize = header.bfOffBits+DWORD(pixels.size());
            BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = LONG(w); info.biHeight = -LONG(h);
            info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB; info.biSizeImage = DWORD(pixels.size());
            std::ofstream out(std::filesystem::path(file),std::ios::binary);
            out.write(reinterpret_cast<const char*>(&header),sizeof(header)); out.write(reinterpret_cast<const char*>(&info),sizeof(info));
            out.write(reinterpret_cast<const char*>(pixels.data()),pixels.size()); out.close();
            if (!out) { DeleteFileW(file); return {}; }
            return file;
        });
        return false;
    }
    void Discard()
    {
        if (pending && fence && fence->GetCompletedValue() < 1)
        {
            HANDLE event = CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if (event) { if (SUCCEEDED(fence->SetEventOnCompletion(1,event))) WaitForSingleObject(event,2000); CloseHandle(event); }
        }
        if (writer.valid()) { const auto path = writer.get(); if (!path.empty()) DeleteFileW(path.c_str()); }
        pending = false; sourceTexture.Reset(); readback.Reset(); commands.Reset(); allocator.Reset(); fence.Reset();
    }
    ~ScreenSnapshotCapture() { Discard(); }
};
