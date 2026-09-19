// me_probe: does this PC's GPU hardware motion estimator (D3D12 video motion
// estimation, fixed-function, usually the video encoder) work at VRX's depth-grid
// size, give correct vectors, and how long does it take?
//
// For each hardware adapter:
//   1. CheckFeatureSupport(D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR, NV12): block sizes,
//      precision, supported size range.
//   2. If WxH (default 686x392) is in range: two NV12 frames of a textured pattern,
//      the current one shifted by (+DX,+DY) px, EstimateMotion + ResolveMotionVector
//      Heap on a VIDEO_ENCODE queue, read the vectors back and check them.
//   3. Time submit->fence for the estimate+resolve, repeated.
//
// Pair mode (--pairs=<dir>, used by bench/xmmodel/xm_fuse.py --mv=hw): reads real
// frames from <dir>/frames.y (N luma planes, W x H bytes each) and <dir>/pairs.txt
// (lines "current reference"), estimates 8x8-block motion for each pair on one
// adapter, and writes <dir>/vectors.bin: per pair, ceil(H/8) x ceil(W/8) x (int16 x,
// int16 y) in quarter pels, pointing from the current block to where its content was
// in the reference frame.
//
// usage: me_probe.exe [--size=WxH] [--shift=DX,DY] [--runs=N]
//        me_probe.exe --pairs=<dir> [--size=WxH] [--adapter=N]
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static void Log(const char* fmt, ...)
{
    if (!fmt) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("[me_probe] %s\n", buf);
    fflush(stdout);
}

struct Options
{
    UINT w = 686, h = 392;
    int dx = 6, dy = 3;
    int runs = 200;
    std::string pairsDir;
    int adapter = -1;                   // pair mode: -1 = first adapter that supports it
};

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12VideoDevice1> video;
    ComPtr<ID3D12CommandQueue> direct, encode;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE event = nullptr;
};

static bool Wait(Gpu& g, ID3D12CommandQueue* queue)
{
    if (!queue) return false;
    const UINT64 v = ++g.fenceValue;
    if (FAILED(queue->Signal(g.fence.Get(), v))) return false;
    if (g.fence->GetCompletedValue() >= v) return true;
    if (FAILED(g.fence->SetEventOnCompletion(v, g.event))) return false;
    return WaitForSingleObject(g.event, 5000) == WAIT_OBJECT_0;
}

static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

// Smooth value noise on 4-px cells: enough texture everywhere for block matching.
static unsigned char Pattern(int x, int y)
{
    auto cell = [](int cx, int cy) -> float
    {
        unsigned h = (unsigned)(cx * 73856093) ^ (unsigned)(cy * 19349663);
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float)((h >> 8) & 255);
    };
    const int cx = (int)floorf(x / 4.0f), cy = (int)floorf(y / 4.0f);
    const float fx = x / 4.0f - cx, fy = y / 4.0f - cy;
    const float top = cell(cx, cy) * (1 - fx) + cell(cx + 1, cy) * fx;
    const float bot = cell(cx, cy + 1) * (1 - fx) + cell(cx + 1, cy + 1) * fx;
    return (unsigned char)std::clamp(top * (1 - fy) + bot * fy, 0.0f, 255.0f);
}

// NV12 texture whose luma is luma(x, y) and chroma neutral.
static ComPtr<ID3D12Resource> MakeFrame(Gpu& g, const Options& o, const std::function<unsigned char(UINT, UINT)>& luma)
{
    if (!luma) return nullptr;
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = o.w; td.Height = o.h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> tex;
    if (FAILED(g.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
    {
        Log("MakeFrame: FAIL NV12 texture");
        return nullptr;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]{};
    UINT rows[2]{};
    UINT64 rowBytes[2]{}, total = 0;
    g.device->GetCopyableFootprints(&td, 0, 2, 0, fp, rows, rowBytes, &total);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
    {
        Log("MakeFrame: FAIL upload buffer");
        return nullptr;
    }
    unsigned char* p = nullptr;
    if (FAILED(upload->Map(0, nullptr, (void**)&p)))
    {
        Log("MakeFrame: FAIL map");
        return nullptr;
    }
    for (UINT y = 0; y < rows[0]; y++)
        for (UINT x = 0; x < o.w; x++)
            p[fp[0].Offset + (UINT64)y * fp[0].Footprint.RowPitch + x] = luma(x, y);
    for (UINT y = 0; y < rows[1]; y++)
        memset(p + fp[1].Offset + (UINT64)y * fp[1].Footprint.RowPitch, 128, (size_t)rowBytes[1]);
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        Log("MakeFrame: FAIL command list");
        return nullptr;
    }
    for (UINT plane = 0; plane < 2; plane++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{ tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        dst.SubresourceIndex = plane;
        D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.PlacedFootprint = fp[plane];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    auto b = Transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    g.direct->ExecuteCommandLists(1, lists);
    if (!Wait(g, g.direct.Get()))
    {
        Log("MakeFrame: FAIL upload wait");
        return nullptr;
    }
    return tex;
}

// Estimator + heap + resolved-vector texture + readback for one block size.
struct MeSession
{
    UINT block = 8, bw = 0, bh = 0;
    ComPtr<ID3D12VideoMotionEstimator> est;
    ComPtr<ID3D12VideoMotionVectorHeap> heap;
    ComPtr<ID3D12Resource> vectors, readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    ComPtr<ID3D12CommandAllocator> ea, da;
    ComPtr<ID3D12VideoEncodeCommandList> el;
    ComPtr<ID3D12GraphicsCommandList> dl;
};

static bool InitSession(Gpu& g, const Options& o, bool use8, MeSession& m)
{
    Log("InitSession: enter %ux%u blocks %s", o.w, o.h, use8 ? "8x8" : "16x16");
    m.block = use8 ? 8 : 16;
    const auto sizeFlag = use8 ? D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_8X8 : D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_16X16;
    D3D12_VIDEO_MOTION_ESTIMATOR_DESC ed{ 0, DXGI_FORMAT_NV12, sizeFlag, D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL, { o.w, o.h, o.w, o.h } };
    HRESULT hr = g.video->CreateVideoMotionEstimator(&ed, nullptr, IID_PPV_ARGS(&m.est));
    if (FAILED(hr)) { Log("InitSession: FAIL CreateVideoMotionEstimator 0x%08lx", hr); return false; }
    D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC hd{ 0, DXGI_FORMAT_NV12, sizeFlag, D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL, { o.w, o.h, o.w, o.h } };
    hr = g.video->CreateVideoMotionVectorHeap(&hd, nullptr, IID_PPV_ARGS(&m.heap));
    if (FAILED(hr)) { Log("InitSession: FAIL CreateVideoMotionVectorHeap 0x%08lx", hr); return false; }

    m.bw = (o.w + m.block - 1) / m.block;
    m.bh = (o.h + m.block - 1) / m.block;
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT }, rb{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC vd{};
    vd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; vd.Width = m.bw; vd.Height = m.bh; vd.DepthOrArraySize = 1;
    vd.MipLevels = 1; vd.Format = DXGI_FORMAT_R16G16_SINT; vd.SampleDesc.Count = 1;
    if (FAILED(g.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &vd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m.vectors))))
    {
        Log("InitSession: FAIL vector texture");
        return false;
    }
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&vd, 0, 1, 0, &m.fp, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m.readback))))
    {
        Log("InitSession: FAIL readback buffer");
        return false;
    }
    if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, IID_PPV_ARGS(&m.ea))) ||
        FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, m.ea.Get(), nullptr, IID_PPV_ARGS(&m.el))) ||
        FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m.da))) ||
        FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m.da.Get(), nullptr, IID_PPV_ARGS(&m.dl))))
    {
        Log("InitSession: FAIL command lists");
        return false;
    }
    m.el->Close();
    m.dl->Close();
    Log("InitSession: exit %ux%u vectors", m.bw, m.bh);
    return true;
}

// Records estimate(cur vs ref) + resolve into m.el; RunEstimate executes it.
static bool RecordEstimate(const Options& o, MeSession& m, ID3D12Resource* cur, ID3D12Resource* ref)
{
    if (!cur || !ref) return false;
    if (FAILED(m.ea->Reset()) || FAILED(m.el->Reset(m.ea.Get()))) { Log("RecordEstimate: FAIL reset"); return false; }
    D3D12_RESOURCE_BARRIER in[3] = {
        Transition(cur, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
        Transition(ref, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
        Transition(m.vectors.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE) };
    m.el->ResourceBarrier(3, in);
    D3D12_VIDEO_MOTION_ESTIMATOR_OUTPUT eo{ m.heap.Get() };
    D3D12_VIDEO_MOTION_ESTIMATOR_INPUT ei{ cur, 0, ref, 0, nullptr };
    m.el->EstimateMotion(m.est.Get(), &eo, &ei);
    D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_OUTPUT ro{ m.vectors.Get(), {} };
    D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_INPUT ri{ m.heap.Get(), o.w, o.h };
    m.el->ResolveMotionVectorHeap(&ro, &ri);
    D3D12_RESOURCE_BARRIER out[3] = {
        Transition(cur, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON),
        Transition(ref, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON),
        Transition(m.vectors.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_COMMON) };
    m.el->ResourceBarrier(3, out);
    if (FAILED(m.el->Close())) { Log("RecordEstimate: FAIL close"); return false; }
    return true;
}

static bool RunEstimate(Gpu& g, MeSession& m)
{
    ID3D12CommandList* lists[] = { m.el.Get() };
    g.encode->ExecuteCommandLists(1, lists);
    if (Wait(g, g.encode.Get())) return true;
    Log("RunEstimate: FAIL wait (device removed: 0x%08lx)", g.device->GetDeviceRemovedReason());
    return false;
}

// Copies the resolved vectors to the CPU: bh rows of bw (x, y) int16 pairs.
static bool ReadVectors(Gpu& g, MeSession& m, std::vector<short>& out)
{
    if (FAILED(m.da->Reset()) || FAILED(m.dl->Reset(m.da.Get(), nullptr))) { Log("ReadVectors: FAIL reset"); return false; }
    auto toSrc = Transition(m.vectors.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m.dl->ResourceBarrier(1, &toSrc);
    D3D12_TEXTURE_COPY_LOCATION dst{ m.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = m.fp;
    D3D12_TEXTURE_COPY_LOCATION src{ m.vectors.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    m.dl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto back = Transition(m.vectors.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    m.dl->ResourceBarrier(1, &back);
    if (FAILED(m.dl->Close())) { Log("ReadVectors: FAIL close"); return false; }
    ID3D12CommandList* lists[] = { m.dl.Get() };
    g.direct->ExecuteCommandLists(1, lists);
    if (!Wait(g, g.direct.Get())) { Log("ReadVectors: FAIL wait"); return false; }

    const char* p = nullptr;
    if (FAILED(m.readback->Map(0, nullptr, (void**)&p))) { Log("ReadVectors: FAIL map"); return false; }
    out.resize((size_t)m.bw * m.bh * 2);
    for (UINT y = 0; y < m.bh; y++)
        memcpy(&out[(size_t)y * m.bw * 2], p + (UINT64)y * m.fp.Footprint.RowPitch, (size_t)m.bw * 4);
    m.readback->Unmap(0, nullptr);
    return true;
}


// Device, video device, capability check and queues for one adapter. False (and a
// logged reason) when the adapter cannot do motion estimation at o.w x o.h.
static bool OpenGpu(IDXGIAdapter1* adapter, int index, const Options& o, Gpu& g, bool& b8, bool& b16)
{
    if (!adapter) return false;
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return false;
    Log("=== adapter %d: %ls (%llu MB)", index, ad.Description, (unsigned long long)(ad.DedicatedVideoMemory >> 20));

    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
    {
        Log("  FAIL: no D3D12 device");
        return false;
    }
    if (FAILED(g.device.As(&g.video)))
    {
        Log("  UNSUPPORTED: no ID3D12VideoDevice1 (motion estimation needs it)");
        return false;
    }

    D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR caps{};
    caps.InputFormat = DXGI_FORMAT_NV12;
    if (FAILED(g.video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR, &caps, sizeof(caps))))
    {
        Log("  UNSUPPORTED: CheckFeatureSupport(D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR) failed");
        return false;
    }
    b8 = (caps.BlockSizeFlags & D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_8X8) != 0;
    b16 = (caps.BlockSizeFlags & D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_16X16) != 0;
    const bool qpel = (caps.PrecisionFlags & D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_FLAG_QUARTER_PEL) != 0;
    Log("  caps: blocks %s%s precision %s size range %ux%u .. %ux%u", b8 ? "8x8 " : "", b16 ? "16x16" : "",
        qpel ? "quarter-pel" : "none", caps.SizeRange.MinWidth, caps.SizeRange.MinHeight, caps.SizeRange.MaxWidth, caps.SizeRange.MaxHeight);
    if (!(b8 || b16) || !qpel)
    {
        Log("  UNSUPPORTED: no usable block size / precision");
        return false;
    }
    if (o.w < caps.SizeRange.MinWidth || o.h < caps.SizeRange.MinHeight || o.w > caps.SizeRange.MaxWidth || o.h > caps.SizeRange.MaxHeight)
    {
        Log("  UNSUPPORTED: %ux%u is outside the supported size range", o.w, o.h);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.direct)))) { Log("  FAIL: direct queue"); return false; }
    qd.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
    if (FAILED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.encode)))) { Log("  FAIL: video encode queue"); return false; }
    if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) { Log("  FAIL: fence"); return false; }
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.event) { Log("  FAIL: event"); return false; }
    return true;
}

// Pair mode: real frames from o.pairsDir, 8x8 vectors for every listed pair.
static bool RunPairs(Gpu& g, const Options& o)
{
    Log("RunPairs: enter dir %s", o.pairsDir.c_str());
    const size_t plane = (size_t)o.w * o.h;
    std::ifstream fy(o.pairsDir + "\\frames.y", std::ios::binary | std::ios::ate);
    if (!fy) { Log("RunPairs: FAIL cannot open frames.y"); return false; }
    const size_t bytes = (size_t)fy.tellg();
    if (bytes == 0 || bytes % plane != 0) { Log("RunPairs: FAIL frames.y is %zu bytes, not a multiple of %zu", bytes, plane); return false; }
    std::vector<unsigned char> frames(bytes);
    fy.seekg(0);
    fy.read((char*)frames.data(), (std::streamsize)bytes);
    const int n = (int)(bytes / plane);

    std::ifstream fp(o.pairsDir + "\\pairs.txt");
    if (!fp) { Log("RunPairs: FAIL cannot open pairs.txt"); return false; }
    std::vector<std::pair<int, int>> pairs;
    for (int c, r; fp >> c >> r;)
    {
        if (c < 0 || r < 0 || c >= n || r >= n) { Log("RunPairs: FAIL pair %d %d outside %d frames", c, r, n); return false; }
        pairs.emplace_back(c, r);
    }
    Log("RunPairs: %d frames, %zu pairs", n, pairs.size());

    MeSession m;
    if (!InitSession(g, o, true, m)) return false;
    std::ofstream out(o.pairsDir + "\\vectors.bin", std::ios::binary);
    if (!out) { Log("RunPairs: FAIL cannot write vectors.bin"); return false; }

    std::vector<short> v;
    std::vector<double> ms;
    for (size_t i = 0; i < pairs.size(); i++)
    {
        const unsigned char* cy = &frames[(size_t)pairs[i].first * plane];
        const unsigned char* ry = &frames[(size_t)pairs[i].second * plane];
        auto cur = MakeFrame(g, o, [&](UINT x, UINT y) { return cy[(size_t)y * o.w + x]; });
        auto ref = MakeFrame(g, o, [&](UINT x, UINT y) { return ry[(size_t)y * o.w + x]; });
        if (!cur || !ref || !RecordEstimate(o, m, cur.Get(), ref.Get())) return false;
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!RunEstimate(g, m)) return false;
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count());
        if (!ReadVectors(g, m, v)) return false;
        out.write((const char*)v.data(), (std::streamsize)(v.size() * sizeof(short)));
        if (i % 100 == 0) Log("RunPairs: pair %zu/%zu (%d -> %d)", i + 1, pairs.size(), pairs[i].first, pairs[i].second);
    }
    std::sort(ms.begin(), ms.end());
    Log("RunPairs: exit %zu pairs, %ux%u vectors each, estimate p50 %.3f ms p95 %.3f ms", pairs.size(), m.bw, m.bh,
        ms.empty() ? 0.0 : ms[ms.size() / 2], ms.empty() ? 0.0 : ms[ms.size() * 95 / 100]);
    return true;
}

static void ProbeAdapter(IDXGIAdapter1* adapter, int index, const Options& o)
{
    Gpu g;
    bool b8 = false, b16 = false;
    if (!OpenGpu(adapter, index, o, g, b8, b16))
    {
        if (g.event) CloseHandle(g.event);
        return;
    }

    for (int pass = 0; pass < 2; pass++)
    {
        const bool use8 = pass == 0;
        if (use8 ? !b8 : !b16) continue;
        Log("  -- %s blocks", use8 ? "8x8" : "16x16");
        MeSession m;
        if (!InitSession(g, o, use8, m)) continue;
        auto ref = MakeFrame(g, o, [](UINT x, UINT y) { return Pattern((int)x, (int)y); });
        auto cur = MakeFrame(g, o, [&o](UINT x, UINT y) { return Pattern((int)x - o.dx, (int)y - o.dy); });
        if (!ref || !cur || !RecordEstimate(o, m, cur.Get(), ref.Get())) continue;

        std::vector<double> ms;
        for (int r = 0; r < o.runs + 5; r++)
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            if (!RunEstimate(g, m)) break;
            if (r >= 5) ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count());
        }
        if (ms.empty()) continue;
        std::sort(ms.begin(), ms.end());
        Log("  time submit->done: p50 %.3f ms  p95 %.3f ms  max %.3f ms  (%zu runs)", ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back(), ms.size());

        std::vector<short> v;
        if (!ReadVectors(g, m, v)) continue;
        std::vector<int> vx, vy;
        int good = 0, goodNeg = 0, n = 0;
        for (UINT y = 1; y + 1 < m.bh; y++)
            for (UINT x = 1; x + 1 < m.bw; x++)
            {
                const short* e = &v[((size_t)y * m.bw + x) * 2];
                vx.push_back(e[0]); vy.push_back(e[1]); n++;
                if (abs(e[0] - o.dx * 4) <= 4 && abs(e[1] - o.dy * 4) <= 4) good++;
                if (abs(e[0] + o.dx * 4) <= 4 && abs(e[1] + o.dy * 4) <= 4) goodNeg++;
            }
        std::nth_element(vx.begin(), vx.begin() + vx.size() / 2, vx.end());
        std::nth_element(vy.begin(), vy.begin() + vy.size() / 2, vy.end());
        Log("  vectors: %ux%u blocks, median (%d, %d) quarter-pel; content moved (+%d,+%d) px = (+%d,+%d) qpel",
            m.bw, m.bh, vx[vx.size() / 2], vy[vy.size() / 2], o.dx, o.dy, o.dx * 4, o.dy * 4);
        Log("  within 1 px: %.1f%% as (+shift), %.1f%% as (-shift) of %d interior blocks",
            100.0 * good / std::max(n, 1), 100.0 * goodNeg / std::max(n, 1), n);
    }
    CloseHandle(g.event);
}

int main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        if (!a) continue;
        if (!strncmp(a, "--size=", 7)) { sscanf_s(a + 7, "%ux%u", &o.w, &o.h); continue; }
        if (!strncmp(a, "--shift=", 8)) { sscanf_s(a + 8, "%d,%d", &o.dx, &o.dy); continue; }
        if (!strncmp(a, "--runs=", 7)) { o.runs = std::max(1, atoi(a + 7)); continue; }
        if (!strncmp(a, "--pairs=", 8)) { o.pairsDir = a + 8; continue; }
        if (!strncmp(a, "--adapter=", 10)) { o.adapter = atoi(a + 10); continue; }
        Log("unknown option %s", a);
        return 2;
    }
    if (o.w < 16 || o.h < 16 || (o.w & 1) || (o.h & 1)) { Log("size must be even and at least 16x16"); return 2; }
    Log("main: enter size %ux%u shift %d,%d runs %d", o.w, o.h, o.dx, o.dy, o.runs);

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { Log("FAIL: DXGI factory"); return 1; }
    ComPtr<IDXGIAdapter1> adapter;
    if (!o.pairsDir.empty())
    {
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++, adapter.Reset())
        {
            if (o.adapter >= 0 && (int)i != o.adapter) continue;
            Gpu g;
            bool b8 = false, b16 = false;
            if (!OpenGpu(adapter.Get(), (int)i, o, g, b8, b16) || !b8)
            {
                if (g.event) CloseHandle(g.event);
                continue;
            }
            const bool ok = RunPairs(g, o);
            CloseHandle(g.event);
            Log("main: exit (pairs %s)", ok ? "OK" : "FAILED");
            return ok ? 0 : 1;
        }
        Log("main: exit - no adapter could estimate motion at %ux%u", o.w, o.h);
        return 1;
    }
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
    {
        ProbeAdapter(adapter.Get(), (int)i, o);
        adapter.Reset();
    }
    Log("main: exit");
    return 0;
}
