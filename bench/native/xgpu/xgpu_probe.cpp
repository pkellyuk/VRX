// Cross-adapter transfer probe: can a model-input-sized frame move from the GPU
// that renders the game (source) to a second GPU (target) fast enough, with
// GPU-side synchronisation only?
//
//   source device: local DEFAULT buffer --copy--> cross-adapter shared buffer
//                  signal cross-adapter shared fence
//   target device: wait on that fence (GPU side) --copy--> local DEFAULT buffer
//                  signal its own fence; CPU waits only to time the round trip
//
// Every run verifies the bytes that arrive (a pattern that changes per run), so a
// fast but stale transfer cannot pass.
//
// usage: xgpu_probe [sourceAdapter=0] [targetAdapter=1] [bytes=3096576] [runs=200]
//   default bytes = 672 x 384 x 3 floats (ZipDepth input, NCHW float)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

static bool Ok(HRESULT hr, const char* what)
{
    if (SUCCEEDED(hr)) return true;
    printf("FAIL: %s (0x%08lX)\n", what, (unsigned long)hr);
    return false;
}

struct Gpu
{
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;             // local completion
    UINT64 fenceValue = 0;
    HANDLE event = nullptr;
    wchar_t name[128] = {};
};

static bool InitGpu(IDXGIFactory4* factory, UINT index, Gpu& g)
{
    if (!factory) return false;
    if (factory->EnumAdapters1(index, &g.adapter) == DXGI_ERROR_NOT_FOUND) { printf("FAIL: no adapter %u\n", index); return false; }
    DXGI_ADAPTER_DESC1 d{};
    g.adapter->GetDesc1(&d);
    wcsncpy_s(g.name, d.Description, _TRUNCATE);
    if (!Ok(D3D12CreateDevice(g.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device)), "D3D12CreateDevice")) return false;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!Ok(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)), "CreateCommandQueue")) return false;
    if (!Ok(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc)), "CreateCommandAllocator")) return false;
    if (!Ok(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc.Get(), nullptr, IID_PPV_ARGS(&g.list)), "CreateCommandList")) return false;
    g.list->Close();
    if (!Ok(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)), "CreateFence")) return false;
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g.event != nullptr;
}

static bool Buffer(ID3D12Device* dev, D3D12_HEAP_TYPE heap, UINT64 bytes, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    if (!dev) return false;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return Ok(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&out)), "CreateCommittedResource");
}

static void WaitLocal(Gpu& g)
{
    g.queue->Signal(g.fence.Get(), ++g.fenceValue);
    if (g.fence->GetCompletedValue() >= g.fenceValue) return;
    g.fence->SetEventOnCompletion(g.fenceValue, g.event);
    WaitForSingleObject(g.event, INFINITE);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const UINT srcIndex = argc > 1 ? (UINT)atoi(argv[1]) : 0;
    const UINT dstIndex = argc > 2 ? (UINT)atoi(argv[2]) : 1;
    const UINT64 bytes = argc > 3 ? _strtoui64(argv[3], nullptr, 10) : 672ull * 384 * 3 * 4;
    const int runs = argc > 4 ? atoi(argv[4]) : 200;
    if (bytes == 0 || bytes % 4 || runs <= 0 || srcIndex == dstIndex) { printf("usage: xgpu_probe src dst bytes(multiple of 4) runs\n"); return 1; }

    if (getenv("XGPU_DEBUG"))
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); printf("D3D12 debug layer on\n"); }
    }
    ComPtr<IDXGIFactory4> factory;
    if (!Ok(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return 1;
    Gpu src, dst;
    if (!InitGpu(factory.Get(), srcIndex, src) || !InitGpu(factory.Get(), dstIndex, dst)) return 1;
    wprintf(L"source: adapter %u %s\ntarget: adapter %u %s\n", srcIndex, src.name, dstIndex, dst.name);

    D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
    src.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt));
    printf("cross-adapter row-major texture support (source): %d\n", (int)opt.CrossAdapterRowMajorTextureSupported);

    // Cross-adapter heap (the pattern of Microsoft's D3D12HeterogeneousMultiadapter
    // sample): a shared heap created on the source and opened on the target, with a
    // buffer placed on it from each side. It lives in memory both GPUs can reach.
    const UINT64 heapBytes = (bytes + D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1) & ~(UINT64)(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1);
    D3D12_HEAP_DESC heapDesc{};
    heapDesc.SizeInBytes = heapBytes;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    ComPtr<ID3D12Heap> heapSrc, heapDst;
    if (!Ok(src.device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heapSrc)), "CreateHeap(cross-adapter)")) return 1;
    HANDLE h = nullptr;
    if (!Ok(src.device->CreateSharedHandle(heapSrc.Get(), nullptr, GENERIC_ALL, nullptr, &h), "CreateSharedHandle(heap)")) return 1;
    HRESULT hr = dst.device->OpenSharedHandle(h, IID_PPV_ARGS(&heapDst));
    CloseHandle(h);
    if (!Ok(hr, "OpenSharedHandle(heap) on target")) return 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    ComPtr<ID3D12Resource> sharedSrc, sharedDst;
    if (!Ok(src.device->CreatePlacedResource(heapSrc.Get(), 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sharedSrc)), "CreatePlacedResource(source)")) return 1;
    if (!Ok(dst.device->CreatePlacedResource(heapDst.Get(), 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sharedDst)), "CreatePlacedResource(target)")) return 1;

    // Cross-adapter fence: source signals, target's queue waits on the GPU timeline.
    ComPtr<ID3D12Fence> fenceSrc, fenceDst;
    if (!Ok(src.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&fenceSrc)), "create cross-adapter fence")) return 1;
    if (!Ok(src.device->CreateSharedHandle(fenceSrc.Get(), nullptr, GENERIC_ALL, nullptr, &h), "CreateSharedHandle(fence)")) return 1;
    hr = dst.device->OpenSharedHandle(h, IID_PPV_ARGS(&fenceDst));
    CloseHandle(h);
    if (!Ok(hr, "OpenSharedHandle(fence) on target")) return 1;

    // Local buffers: source data (VRAM), its upload staging, target VRAM, target readback.
    ComPtr<ID3D12Resource> srcLocal, srcUpload, dstLocal, dstReadback;
    if (!Buffer(src.device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON, srcLocal)) return 1;
    if (!Buffer(src.device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ, srcUpload)) return 1;
    if (!Buffer(dst.device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON, dstLocal)) return 1;
    if (!Buffer(dst.device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST, dstReadback)) return 1;
    uint32_t* up = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (!Ok(srcUpload->Map(0, &none, (void**)&up), "Map upload")) return 1;

    const size_t words = (size_t)(bytes / 4);
    std::vector<double> total, sourceSide;
    UINT64 crossValue = 0;
    int verified = 0, corrupt = 0;

    for (int run = -10; run < runs; ++run)                 // 10 warm-up runs
    {
        // A new pattern each run, written into the source's VRAM (not timed).
        for (size_t i = 0; i < words; ++i) up[i] = (uint32_t)(i * 2654435761u) ^ (uint32_t)(run + 12345);
        src.alloc->Reset(); src.list->Reset(src.alloc.Get(), nullptr);
        src.list->CopyBufferRegion(srcLocal.Get(), 0, srcUpload.Get(), 0, bytes);
        src.list->Close();
        ID3D12CommandList* l0[] = { src.list.Get() };
        src.queue->ExecuteCommandLists(1, l0);
        WaitLocal(src);

        // --- timed: source VRAM -> shared -> target VRAM, GPU-synchronised ---
        const auto t0 = std::chrono::steady_clock::now();
        src.alloc->Reset(); src.list->Reset(src.alloc.Get(), nullptr);
        src.list->CopyBufferRegion(sharedSrc.Get(), 0, srcLocal.Get(), 0, bytes);
        src.list->Close();
        ID3D12CommandList* l1[] = { src.list.Get() };
        src.queue->ExecuteCommandLists(1, l1);
        src.queue->Signal(fenceSrc.Get(), ++crossValue);

        dst.alloc->Reset(); dst.list->Reset(dst.alloc.Get(), nullptr);
        dst.list->CopyBufferRegion(dstLocal.Get(), 0, sharedDst.Get(), 0, bytes);
        dst.list->Close();
        dst.queue->Wait(fenceDst.Get(), crossValue);           // GPU-side wait, no CPU stall
        ID3D12CommandList* l2[] = { dst.list.Get() };
        dst.queue->ExecuteCommandLists(1, l2);
        WaitLocal(dst);                                        // CPU waits only to take the time
        const auto t1 = std::chrono::steady_clock::now();
        const double srcDone = fenceSrc->GetCompletedValue() >= crossValue ? 1 : 0;
        (void)srcDone;

        // --- verify (not timed) ---
        dst.alloc->Reset(); dst.list->Reset(dst.alloc.Get(), nullptr);
        dst.list->CopyBufferRegion(dstReadback.Get(), 0, dstLocal.Get(), 0, bytes);
        dst.list->Close();
        ID3D12CommandList* l3[] = { dst.list.Get() };
        dst.queue->ExecuteCommandLists(1, l3);
        WaitLocal(dst);
        uint32_t* got = nullptr;
        D3D12_RANGE all{ 0, (SIZE_T)bytes };
        if (!Ok(dstReadback->Map(0, &all, (void**)&got), "Map readback")) return 1;
        bool same = true;
        for (size_t i = 0; i < words && same; ++i)
            same = got[i] == ((uint32_t)(i * 2654435761u) ^ (uint32_t)(run + 12345));
        dstReadback->Unmap(0, &none);
        if (run >= 0)
        {
            total.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            same ? ++verified : ++corrupt;
        }
    }

    std::sort(total.begin(), total.end());
    auto P = [&](double q) { return total[std::min(total.size() - 1, (size_t)(q * total.size()))]; };
    printf("\n--- %llu bytes (%.2f MB), %d runs ---\n", (unsigned long long)bytes, bytes / 1048576.0, runs);
    printf("source VRAM -> shared -> target VRAM, GPU-synchronised: p50 %.3f ms  p95 %.3f ms  min %.3f ms\n", P(.5), P(.95), total.front());
    printf("effective throughput at p50: %.2f GB/s (data crosses PCIe twice: source->shared, shared->target)\n", bytes / (P(.5) / 1000.0) / 1e9);
    printf("data verified: %d/%d runs identical%s\n", verified, runs, corrupt ? "   ** CORRUPTION DETECTED **" : "");
    return corrupt ? 2 : 0;
}
