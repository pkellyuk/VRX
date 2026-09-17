// M1 probe: zero-copy, GPU-resident input for the ONNX Runtime DirectML EP.
//
// What this establishes (and what it cost to find out):
//  - a CPU-hosted OrtValue caps the DML EP at ~21 ms and graph capture silently
//    ignores new CPU input, so the input must be a D3D12 resource on the EP's
//    own device, wrapped via OrtDmlApi::CreateGPUAllocationFromD3DResource;
//  - GetDMLCommandQueue returns a BORROWED reference - Release()ing it frees the
//    queue the EP still needs and CreateSession then faults;
//  - such a resource MUST allow UAV access (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
//    or every Run fails with E_INVALIDARG inside the DML command recorder;
//  - with a GPU-resident input there is no CPU readback to implicitly wait on,
//    so timing needs an explicit fence on the EP's queue;
//  - and every number is validated with a live-input check, because three
//    different configurations produced fast-but-meaningless timings.
//
// usage: dmlgpu <model.onnx> <height> [width] [warmup] [iters] [key=value ...]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <onnxruntime_c_api.h>
#include "dml_provider_factory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

static const OrtApi* api = nullptr;

static int Fail(const char* what, OrtStatus* st)
{
    printf("FAIL: %s -> %s\n", what, st ? api->GetErrorMessage(st) : "(null status)");
    return 1;
}

static double Checksum(const float* p, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::string modelPath = argc > 1 ? argv[1]
        : "C:\\Users\\paulj\\dev\\VRX\\bench\\models\\model_fixed_518.onnx";
    int H      = argc > 2 ? atoi(argv[2]) : 518;
    int W      = argc > 3 ? atoi(argv[3]) : H;
    int warmup = argc > 4 ? atoi(argv[4]) : 10;
    int iters  = argc > 5 ? atoi(argv[5]) : 50;

    bool ownDevice = false;
    std::vector<std::pair<std::string, std::string>> cfg;
    for (int i = 6; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--own-device") { ownDevice = true; continue; }
        size_t eq = a.find('=');
        if (eq != std::string::npos && eq > 0) cfg.push_back({ a.substr(0, eq), a.substr(eq + 1) });
    }

    api = OrtGetApiBase()->GetApi(ORT_API_VERSION);

    OrtEnv* env = nullptr;
    if (OrtStatus* st = api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "dmlgpu", &env)) return Fail("CreateEnv", st);

    OrtSessionOptions* so = nullptr;
    if (OrtStatus* st = api->CreateSessionOptions(&so)) return Fail("CreateSessionOptions", st);

    const OrtDmlApi* dml = nullptr;
    if (OrtStatus* st = api->GetExecutionProviderApi("DML", ORT_API_VERSION, (const void**)&dml))
        return Fail("GetExecutionProviderApi(DML)", st);
    if (!dml) { printf("FAIL: DML provider api not available\n"); return 1; }

    printf("model : %s\n", modelPath.c_str());
    printf("input : %dx%d  (%d px, %d patches)   runs: %d warmup + %d timed\n",
           H, W, H * W, (H / 14) * (W / 14), warmup, iters);

    // ------------------------------------------------------------------
    // Device acquisition. Two modes:
    //   default      - let ORT create the DML EP (and its device) itself.
    //   --own-device - create the D3D12 device and queue here, make an
    //                  IDMLDevice on it, and hand both to ORT via Ex_DML.
    //                  This is what M3 needs: the OpenXR session, the depth
    //                  swapchain and the model must all live on ONE device.
    // ------------------------------------------------------------------
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> ownQueue;
    ID3D12CommandQueue* queue = nullptr;   // borrowed in default mode

    if (ownDevice)
    {
        HMODULE dmllib = LoadLibraryW(L"DirectML.dll");
        if (!dmllib) { printf("FAIL: DirectML.dll not loadable\n"); return 1; }
        auto createDmlDevice = (HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**))
            GetProcAddress(dmllib, "DMLCreateDevice");
        if (!createDmlDevice) { printf("FAIL: no DMLCreateDevice export\n"); return 1; }

        ComPtr<IDXGIFactory4> fac;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { printf("FAIL: CreateDXGIFactory1\n"); return 1; }
        ComPtr<IDXGIAdapter1> ad;
        if (FAILED(fac->EnumAdapters1(0, &ad))) { printf("FAIL: EnumAdapters1\n"); return 1; }
        { DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d); wprintf(L"device: %s (created here)\n", d.Description); }

        if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
        { printf("FAIL: D3D12CreateDevice\n"); return 1; }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&ownQueue))))
        { printf("FAIL: CreateCommandQueue\n"); return 1; }

        ComPtr<IDMLDevice> dmlDevice;
        HRESULT hr = createDmlDevice(dev.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dmlDevice));
        if (FAILED(hr)) { printf("FAIL: DMLCreateDevice 0x%08lx\n", (unsigned long)hr); return 1; }

        if (OrtStatus* st = dml->SessionOptionsAppendExecutionProvider_DML1(so, dmlDevice.Get(), ownQueue.Get()))
            return Fail("SessionOptionsAppendExecutionProvider_DML1", st);

        queue = ownQueue.Get();
        printf("ORT   : shares our device + queue (Ex_DML)\n");
    }
    else
    {
        if (OrtStatus* st = dml->SessionOptionsAppendExecutionProvider_DML(so, 0))
            return Fail("SessionOptionsAppendExecutionProvider_DML", st);

        // Borrowed reference - do NOT Release() this or CreateSession will fault.
        if (OrtStatus* st = dml->GetDMLCommandQueue(so, &queue)) return Fail("GetDMLCommandQueue", st);
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev)))) { printf("FAIL: queue->GetDevice\n"); return 1; }
        printf("ORT   : its own device (default DML append)\n");
    }

    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { printf("FAIL: CreateFence\n"); return 1; }
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto waitForGpu = [&]() {
        ++fenceValue;
        queue->Signal(fence.Get(), fenceValue);
        fence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    };

    for (auto& kv : cfg)
    {
        if (OrtStatus* st = api->AddSessionConfigEntry(so, kv.first.c_str(), kv.second.c_str()))
            return Fail(("AddSessionConfigEntry " + kv.first).c_str(), st);
        printf("config: %s=%s\n", kv.first.c_str(), kv.second.c_str());
    }

    auto t0 = std::chrono::steady_clock::now();
    OrtSession* session = nullptr;
    std::wstring wmodel(modelPath.begin(), modelPath.end());
    if (OrtStatus* st = api->CreateSession(env, wmodel.c_str(), so, &session)) return Fail("CreateSession", st);
    auto t1 = std::chrono::steady_clock::now();
    printf("load  : %.0f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());

    const OrtMemoryInfo* inInfos[1] = { nullptr };
    if (OrtStatus* st = api->SessionGetMemoryInfoForInputs(session, inInfos, 1))
        return Fail("SessionGetMemoryInfoForInputs", st);
    OrtMemoryInfo* inMemInfo = const_cast<OrtMemoryInfo*>(inInfos[0]);

    // ---- input: a GPU-local buffer the CPU can also rewrite each frame -----
    const size_t inElems = (size_t)3 * H * W;
    const UINT64 inBytes = (UINT64)inElems * sizeof(float);
    int64_t inDims[4] = { 1, 3, H, W };

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = inBytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;   // required by DML

    ComPtr<ID3D12Resource> inRes;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                            IID_PPV_ARGS(&inRes))))
    { printf("FAIL: CreateCommittedResource\n"); return 1; }

    float* inMapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(inRes->Map(0, &noRead, (void**)&inMapped))) { printf("FAIL: Map\n"); return 1; }

    void* dmlAlloc = nullptr;
    if (OrtStatus* st = dml->CreateGPUAllocationFromD3DResource(inRes.Get(), &dmlAlloc))
        return Fail("CreateGPUAllocationFromD3DResource", st);

    OrtValue* inValue = nullptr;
    if (OrtStatus* st = api->CreateTensorWithDataAsOrtValue(inMemInfo, dmlAlloc, inBytes,
                                                            inDims, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                            &inValue))
        return Fail("CreateTensorWithDataAsOrtValue", st);

    printf("input : D3D12 GPU buffer %llu bytes, zero-copy (live)\n", (unsigned long long)inBytes);

    const char* inName  = "pixel_values";
    const char* outName = "predicted_depth";

    OrtValue* lastOut = nullptr;
    auto runOnce = [&]() -> bool {
        if (lastOut) { api->ReleaseValue(lastOut); lastOut = nullptr; }
        if (OrtStatus* st = api->Run(session, nullptr, &inName, (const OrtValue* const*)&inValue, 1,
                                     &outName, 1, &lastOut))
        { Fail("Run", st); return false; }
        return true;
    };

    const size_t outN = (size_t)H * W;
    auto readOut = [&](float* dst) -> bool {
        float* p = nullptr;
        if (OrtStatus* st = api->GetTensorMutableData(lastOut, (void**)&p)) { Fail("GetTensorMutableData", st); return false; }
        memcpy(dst, p, outN * sizeof(float));
        return true;
    };

    for (size_t i = 0; i < inElems; i++) inMapped[i] = 0.25f;
    printf("warmup: ");
    for (int i = 0; i < warmup; i++) { if (!runOnce()) return 1; waitForGpu(); printf("."); }
    printf(" done\n");

    std::vector<float> a(outN), b(outN);

    // ---- live-input check -------------------------------------------------
    // Uniform fills are a weak test for a depth model: a flat field produces a
    // flat depth map whatever its brightness, so the outputs barely differ and
    // a stale buffer can look "live". Use a spatial ramp and its inverse.
    auto fillRamp = [&](bool inverse) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                float v = (float)x / (float)(W - 1);
                if (inverse) v = 1.0f - v;
                for (int c = 0; c < 3; c++) inMapped[((size_t)c * H + y) * W + x] = v;
            }
    };

    fillRamp(false);
    if (!runOnce()) return 1;
    waitForGpu();
    if (!readOut(a.data())) return 1;

    fillRamp(true);
    if (!runOnce()) return 1;
    waitForGpu();
    if (!readOut(b.data())) return 1;

    double maxDiff = 0;
    size_t nDiff = 0;
    for (size_t i = 0; i < outN; i++)
    {
        double d = fabs((double)a[i] - (double)b[i]);
        if (d > maxDiff) maxDiff = d;
        if (d > 1e-3) nDiff++;
    }
    bool live = maxDiff > 1e-3;

    printf("sum(ramp)   : %.3f\n", Checksum(a.data(), outN));
    printf("sum(invramp): %.3f\n", Checksum(b.data(), outN));
    printf("live check  : maxDiff=%.4f over %zu/%zu elements\n", maxDiff, nDiff, outN);

    // ---- timing: fence-synchronised, no CPU readback in the loop ----------
    for (size_t i = 0; i < inElems; i++) inMapped[i] = 0.4f;
    std::vector<double> ms(iters);
    auto wall0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; i++)
    {
        auto p0 = std::chrono::steady_clock::now();
        if (!runOnce()) return 1;
        waitForGpu();
        auto p1 = std::chrono::steady_clock::now();
        ms[i] = std::chrono::duration<double, std::milli>(p1 - p0).count();
    }
    auto wall1 = std::chrono::steady_clock::now();

    std::sort(ms.begin(), ms.end());
    auto P = [&](double q) { return ms[std::min(ms.size() - 1, (size_t)(q * ms.size()))]; };
    double mean = 0; for (double v : ms) mean += v; mean /= ms.size();

    printf("\n--- DirectML, GPU-resident input, %dx%d, %d patches ---\n", H, W, (H / 14) * (W / 14));
    printf("p50  : %.2f ms  (%.1f fps)\n", P(0.50), 1000.0 / P(0.50));
    printf("mean : %.2f ms   p95: %.2f ms   min: %.2f ms\n", mean, P(0.95), ms.front());
    printf("wall : %.0f ms for %d runs\n", std::chrono::duration<double, std::milli>(wall1 - wall0).count(), iters);
    printf("budget: 90Hz=11.1ms 120Hz=8.3ms 144Hz=6.9ms\n");
    printf("LIVE GPU INPUT    : %s\n", live ? "YES - output tracks the D3D12 buffer" : "NO - STALE/BROKEN");
    printf("VERDICT: %s%s\n", P(0.50) < 11.1 ? "fits 90 Hz" : "over 90 Hz",
           live ? "" : "   [timing NOT trustworthy - input not live]");

    if (lastOut) api->ReleaseValue(lastOut);
    api->ReleaseValue(inValue);
    api->ReleaseSession(session);
    api->ReleaseSessionOptions(so);
    api->ReleaseEnv(env);
    return 0;
}
