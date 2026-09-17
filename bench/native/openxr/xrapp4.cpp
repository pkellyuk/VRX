// M3b: AI depth into OpenXR - the target architecture.
//
// xrapp3 proved the effect but is CPU-bound: the model runs synchronously on the
// render thread and the stereo warp is a CPU loop. xrapp4 keeps the same maths
// and fixes the structure:
//
//   render thread (OpenXR frame loop, gfx queue)      worker thread (ml queue)
//   --------------------------------------------      ------------------------
//   scene -> GPU texture                               latest scene
//   latest COMPLETED depth -> GPU buffer   <-------    model (ORT + DirectML)
//   compute-shader forward warp, both eyes  triple      normalise -> nearness
//   copy into colour + depth swapchains     buffer      publish slot
//   xrEndFrame - never waits for the model
//
// * The render thread never blocks on the model: it always warps with the latest
//   completed depth (triple buffer, lock-free handoff).
// * The warp is a compute shader. A forward warp is a scatter with a z-test, which
//   races if parallelised per pixel - but rows are independent, so one GPU thread
//   runs one whole row, exactly like the CPU reference (WarpEye in xr_common.h).
// * --selftest runs the GPU warp and compares it pixel-for-pixel with the CPU
//   reference, so the shader is verified without needing anyone in the headset.
// * The model output still arrives on the CPU (M1: ORT cannot bind a DML device
//   output from this API surface; the readback measured as free). It is 1 MB and
//   goes straight back up as a structured buffer - no row-pitch problem.
//
// usage: xrapp4 [seconds] [--image=path] [--truth] [--no-warp] [--scale=N]
//               [--paired] [--selftest] [--debug] [--dump]
//   --paired   warp the colour frame the depth was computed FROM (no depth lag,
//              but colour updates only at the model rate). Default is latest
//              colour + latest completed depth, which is the real design.
//   --selftest run the GPU-vs-CPU warp comparison and exit
//   --debug    enable the D3D12 debug layer and print its messages at exit
//
// Does the runtime USE the submitted depth? (XR_KHR_composition_layer_depth)
//   RESULT (SteamVR 2.17.9 / PS VR2, --freeze-pose --ab=6 --depth-lie): the picture
//   behaves identically with and without the depth chain, even with a grossly false
//   depth - SteamVR IGNORES XR_KHR_composition_layer_depth. So depth submission is
//   now OFF by default (no depth swapchain, no per-frame depth copy); the stereo
//   warp is the entire effect. It stays available for runtimes that do use it.
//   --submit-depth create the depth swapchain and chain XrCompositionLayerDepthInfoKHR
//   --freeze-pose  submit a STALE view pose (re-captured every period), so the
//                  compositor has to reproject the layer to the live head pose.
//                  Rotation-only reprojection keeps the picture rigid, as if at
//                  infinity; a compositor that uses depth shows parallax inside the
//                  picture when the head TRANSLATES.
//   --ab=N         toggle the depth chain every N seconds. A block at the top centre
//                  of the picture is GREEN while depth is submitted, RED while not.
//   --depth-lie    submit a false depth: left half 0.5 m, right half 10 m. If depth
//                  is used, the halves shear apart along the centre line as the head
//                  translates - unmistakable, unlike subtle true parallax.
//   typical:  xrapp4 90 --image=... --freeze-pose --ab=6 --depth-lie

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <objbase.h>
#include <wincodec.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <onnxruntime_c_api.h>
#include "dml_provider_factory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

#include "xr_common.h"

// ---------------------------------------------------------------- logging
static std::mutex g_logMutex;
static std::chrono::steady_clock::time_point g_t0;
static thread_local const char* g_threadName = "main";

static double NowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
}

static void Log(const char* fmt, ...)
{
    if (!fmt) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lock(g_logMutex);
    printf("[%8.3f][%-6s] %s\n", NowSeconds(), g_threadName, buf);
}

// ------------------------------------------------------------------ types
struct Options
{
    double runSeconds = 75.0;
    bool doWarp = true;
    float warpScale = 1.0f;
    bool useTruth = false;
    bool doDump = false;
    bool paired = false;
    bool selfTestOnly = false;
    bool debugLayer = false;
    bool submitDepth = false;           // SteamVR ignores it (measured) - opt-in only
    bool freezePose = false;
    bool depthLie = false;
    double abSeconds = 0.0;             // 0 = no A/B toggling
    std::wstring imagePath;
};

// One completed depth result. The worker fills a slot and publishes it; the
// render thread takes it. Both buffers are UPLOAD heap, CPU-written by the worker
// only and GPU-read by the render thread only while it owns the slot.
struct DepthSlot
{
    ComPtr<ID3D12Resource> nearUp;      // float[W*H], 0 = far .. 1 = near
    float* nearMapped = nullptr;
    ComPtr<ID3D12Resource> colorUp;     // RGBA, ROW_PITCH rows - the frame the depth came from
    unsigned char* colorMapped = nullptr;
    double sceneTime = 0;
    double completeTime = 0;
    double modelMs = 0;
    float rawMin = 0, rawMax = 0;
    float back = 0, panel = 0, marker = 0;
};

static const int SLOTS = 3;
static const int SLOT_FRESH = 4;        // flag bit in readySlot
static const int RING = 3;              // in-flight render frames
static const uint32_t VIEWS = 2;

struct WarpConstants                    // must match cbuffer C in the shader
{
    uint32_t w, h, doWarp;
    float scaleFocal, invZNear, invZFar, eye0, eye1;
    float nearZ, farZ;                  // the planes declared in XrCompositionLayerDepthInfoKHR
    uint32_t indicator;                 // 0 none, 1 green block (depth submitted), 2 red block (not)
    uint32_t depthLie;                  // 1 = write the false half/half depth
};

static const float DEPTH_NEAR_Z = 0.10f, DEPTH_FAR_Z = 30.0f;

struct App
{
    Options opt;

    // OpenXR
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSwapchain colorSc = XR_NULL_HANDLE, depthSc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> cimgs, dimgs;
    LUID adapterLuid{};
    D3D_FEATURE_LEVEL minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    // D3D12: one device, two queues. ORT/DirectML submits to mlQueue from the
    // worker; the OpenXR session and the warp use gfxQueue from the render thread.
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> gfxQueue, mlQueue;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12CommandAllocator> cmdAlloc[RING];
    UINT64 frameFence[RING] = {};
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceVal = 0;
    HANDLE fenceEvent = nullptr;

    // model
    OrtEnv* env = nullptr;
    OrtSession* ortSession = nullptr;
    OrtValue* modelInValue = nullptr;
    ComPtr<ID3D12Resource> modelIn;
    float* modelInMapped = nullptr;
    std::vector<float> rawDepth;

    // warp pipeline
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> descHeap;
    ComPtr<ID3D12Resource> sceneTex;    // R8G8B8A8_UNORM, NON_PIXEL_SHADER_RESOURCE at rest
    ComPtr<ID3D12Resource> nearBuf;     // structured float buffer, NON_PIXEL_SHADER_RESOURCE at rest
    ComPtr<ID3D12Resource> colorOut;    // R8G8B8A8_TYPELESS[2], UNORDERED_ACCESS at rest
    ComPtr<ID3D12Resource> depthOut;    // R32_TYPELESS[2],      UNORDERED_ACCESS at rest
    ComPtr<ID3D12Resource> sceneUp[RING];
    unsigned char* sceneUpMapped[RING] = {};

    // depth handoff (triple buffer)
    DepthSlot slots[SLOTS];
    std::atomic<int> readySlot{ 1 };
    int writeSlot = 0;                  // owned by whoever publishes (main before the worker starts, then the worker)
    int readSlot = 2;                   // owned by the render thread
    UINT64 slotFence = 0;               // gfx fence value of the last frame that read slots[readSlot]

    // latest scene, render thread -> worker
    std::mutex sceneMutex;
    std::vector<unsigned char> sharedScene;
    double sharedSceneTime = 0;

    std::vector<unsigned char> imageRgb;
    std::thread worker;
    std::atomic<bool> stopWorker{ false };
    std::atomic<uint64_t> depthPublished{ 0 };
};

// ------------------------------------------------------------ d3d helpers
static void WaitFence(App& app, UINT64 value)
{
    if (value == 0) return;
    if (!app.fence) return;
    if (app.fence->GetCompletedValue() >= value) return;

    app.fence->SetEventOnCompletion(value, app.fenceEvent);
    WaitForSingleObject(app.fenceEvent, INFINITE);
}

static UINT64 SubmitAndSignal(App& app)
{
    ID3D12CommandList* lists[] = { app.cmdList.Get() };
    app.gfxQueue->ExecuteCommandLists(1, lists);
    app.gfxQueue->Signal(app.fence.Get(), ++app.fenceVal);
    return app.fenceVal;
}

static void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (!cl || !res) return;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}

static bool MakeBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, UINT64 bytes, D3D12_RESOURCE_FLAGS flags,
                       D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out, void** mapped)
{
    if (!device) return false;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { Log("MakeBuffer: CreateCommittedResource failed 0x%08X (%llu bytes)", (unsigned)hr, (unsigned long long)bytes); return false; }
    if (!mapped) return true;

    D3D12_RANGE nr{ 0, 0 };
    hr = out->Map(0, &nr, mapped);
    if (FAILED(hr)) { Log("MakeBuffer: Map failed 0x%08X", (unsigned)hr); return false; }
    return true;
}

static bool MakeTexture(ID3D12Device* device, DXGI_FORMAT fmt, UINT16 arraySize, D3D12_RESOURCE_FLAGS flags,
                        D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    if (!device) return false;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = W; d.Height = H; d.DepthOrArraySize = arraySize; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = flags;
    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { Log("MakeTexture: CreateCommittedResource failed 0x%08X (format %d)", (unsigned)hr, (int)fmt); return false; }
    return true;
}

// Packed RGB -> RGBA rows of ROW_PITCH bytes (the layout the texture upload uses).
static void PackRgba(const std::vector<unsigned char>& rgb, unsigned char* dst)
{
    if (!dst) return;
    if (rgb.size() != (size_t)W * H * 3) return;

    for (int y = 0; y < H; y++)
    {
        unsigned char* row = dst + (size_t)y * ROW_PITCH;
        const unsigned char* s = rgb.data() + (size_t)y * W * 3;
        for (int x = 0; x < W; x++)
        {
            row[x * 4 + 0] = s[x * 3 + 0];
            row[x * 4 + 1] = s[x * 3 + 1];
            row[x * 4 + 2] = s[x * 3 + 2];
            row[x * 4 + 3] = 255;
        }
    }
}

// ------------------------------------------------------------------ args
static bool ParseArgs(int argc, char** argv, Options* opt)
{
    if (!argv || !opt) return false;

    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        if (!a) continue;
        if (!strcmp(a, "--no-warp")) { opt->doWarp = false; continue; }
        if (!strncmp(a, "--scale=", 8)) { opt->warpScale = (float)atof(a + 8); continue; }
        if (!strcmp(a, "--truth")) { opt->useTruth = true; continue; }
        if (!strcmp(a, "--dump")) { opt->doDump = true; continue; }
        if (!strcmp(a, "--paired")) { opt->paired = true; continue; }
        if (!strcmp(a, "--selftest")) { opt->selfTestOnly = true; continue; }
        if (!strcmp(a, "--debug")) { opt->debugLayer = true; continue; }
        if (!strcmp(a, "--submit-depth")) { opt->submitDepth = true; continue; }
        if (!strcmp(a, "--freeze-pose")) { opt->freezePose = true; continue; }
        if (!strcmp(a, "--depth-lie")) { opt->depthLie = true; continue; }
        if (!strncmp(a, "--ab=", 5)) { opt->abSeconds = atof(a + 5); continue; }
        if (!strncmp(a, "--image=", 8))
        {
            std::string p(a + 8);
            opt->imagePath.assign(p.begin(), p.end());
            continue;
        }
        if (a[0] == '-') { Log("ParseArgs: unknown option %s", a); return false; }
        opt->runSeconds = atof(a);
    }
    if (opt->abSeconds < 0.0) { Log("ParseArgs: --ab must be >= 0"); return false; }
    if ((opt->abSeconds > 0.0 || opt->depthLie) && !opt->submitDepth)
    {
        Log("ParseArgs: --ab / --depth-lie imply --submit-depth");
        opt->submitDepth = true;
    }
    Log("ParseArgs: submitDepth %d freezePose %d depthLie %d ab %.1fs", (int)opt->submitDepth, (int)opt->freezePose,
        (int)opt->depthLie, opt->abSeconds);
    Log("ParseArgs: seconds %.0f warp %d scale %.2f truth %d paired %d selftest %d debug %d image '%ls'",
        opt->runSeconds, (int)opt->doWarp, opt->warpScale, (int)opt->useTruth, (int)opt->paired,
        (int)opt->selfTestOnly, (int)opt->debugLayer, opt->imagePath.c_str());
    return true;
}

// ------------------------------------------------------------ xr instance
static bool InitXrInstance(App& app)
{
    Log("InitXrInstance: enter");

    HMODULE loader = LoadLibraryW(
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll");
    if (!loader) loader = LoadLibraryW(L"openxr_loader.dll");
    if (!loader) { Log("InitXrInstance: FAIL openxr_loader.dll not found"); return false; }

    g_getProc = (PFN_xrGetInstanceProcAddr)GetProcAddress(loader, "xrGetInstanceProcAddr");
    xrEnumExt_ = (PFN_xrEnumerateInstanceExtensionProperties)GetProcAddress(loader, "xrEnumerateInstanceExtensionProperties");
    xrCreateInstance_ = (PFN_xrCreateInstance)GetProcAddress(loader, "xrCreateInstance");
    if (!g_getProc || !xrCreateInstance_) { Log("InitXrInstance: FAIL loader exports missing"); return false; }

    const char* wantExt[] = { "XR_KHR_D3D12_enable", "XR_KHR_composition_layer_depth" };
    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    strcpy_s(ci.applicationInfo.applicationName, "VRX xrapp4");
    ci.enabledExtensionCount = 2;
    ci.enabledExtensionNames = wantExt;

    XrResult r = xrCreateInstance_(&ci, &app.instance);
    if (XR_FAILED(r)) { Log("InitXrInstance: FAIL xrCreateInstance %s (is SteamVR running?)", XRStr(r)); return false; }
    if (!ResolveFns(app.instance)) { Log("InitXrInstance: FAIL resolving functions"); return false; }

    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem_(app.instance, &sgi, &app.systemId);
    if (XR_FAILED(r)) { Log("InitXrInstance: FAIL xrGetSystem %s (headset connected?)", XRStr(r)); return false; }

    XrGraphicsRequirementsD3D12KHR reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    r = xrD3D12Reqs_(app.instance, app.systemId, &reqs);
    if (XR_FAILED(r)) { Log("InitXrInstance: FAIL graphics requirements %s", XRStr(r)); return false; }
    app.adapterLuid = reqs.adapterLuid;
    app.minFeatureLevel = reqs.minFeatureLevel;

    Log("InitXrInstance: exit ok, adapter LUID %08lX:%08lX minFeatureLevel 0x%X",
        (unsigned long)app.adapterLuid.HighPart, (unsigned long)app.adapterLuid.LowPart, (unsigned)app.minFeatureLevel);
    return true;
}

// -------------------------------------------------------------------- d3d
static bool InitD3D(App& app)
{
    Log("InitD3D: enter (debug layer %d)", (int)app.opt.debugLayer);

    if (app.opt.debugLayer)
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); Log("InitD3D: debug layer enabled"); }
        else Log("InitD3D: debug layer NOT available (install the 'Graphics Tools' optional feature)");
    }

    ComPtr<IDXGIFactory4> fac;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { Log("InitD3D: FAIL CreateDXGIFactory1"); return false; }
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(fac->EnumAdapterByLuid(app.adapterLuid, IID_PPV_ARGS(&adapter)))) { Log("InitD3D: FAIL EnumAdapterByLuid"); return false; }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    Log("InitD3D: adapter %ls", ad.Description);

    if (FAILED(D3D12CreateDevice(adapter.Get(), app.minFeatureLevel, IID_PPV_ARGS(&app.device)))) { Log("InitD3D: FAIL D3D12CreateDevice"); return false; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(app.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&app.gfxQueue)))) { Log("InitD3D: FAIL gfx queue"); return false; }
    if (FAILED(app.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&app.mlQueue)))) { Log("InitD3D: FAIL ml queue"); return false; }
    app.gfxQueue->SetName(L"vrx gfx queue");
    app.mlQueue->SetName(L"vrx ml queue");

    for (int i = 0; i < RING; i++)
    {
        if (FAILED(app.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&app.cmdAlloc[i]))))
        { Log("InitD3D: FAIL command allocator %d", i); return false; }
    }
    if (FAILED(app.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, app.cmdAlloc[0].Get(), nullptr, IID_PPV_ARGS(&app.cmdList))))
    { Log("InitD3D: FAIL command list"); return false; }
    app.cmdList->Close();

    if (FAILED(app.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&app.fence)))) { Log("InitD3D: FAIL fence"); return false; }
    app.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!app.fenceEvent) { Log("InitD3D: FAIL fence event"); return false; }

    Log("InitD3D: exit ok (one device, gfx + ml queues, %d-frame ring)", RING);
    return true;
}

static void DumpDebugMessages(App& app)
{
    if (!app.opt.debugLayer) return;
    if (!app.device) return;

    ComPtr<ID3D12InfoQueue> iq;
    if (FAILED(app.device.As(&iq))) { Log("DumpDebugMessages: no info queue"); return; }

    UINT64 n = iq->GetNumStoredMessages();
    Log("DumpDebugMessages: %llu stored message(s)", (unsigned long long)n);
    UINT64 shown = 0;
    for (UINT64 i = 0; i < n && shown < 40; i++)
    {
        SIZE_T len = 0;
        iq->GetMessage(i, nullptr, &len);
        if (len == 0) continue;
        std::vector<char> mem(len);
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)mem.data();
        if (FAILED(iq->GetMessage(i, m, &len))) continue;
        if (m->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;   // corruption/error/warning only
        Log("  d3d12[%d] %s", (int)m->Severity, m->pDescription ? m->pDescription : "(null)");
        shown++;
    }
    Log("DumpDebugMessages: %llu warning-or-worse shown", (unsigned long long)shown);
}

// ------------------------------------------------------------------ model
static bool InitModel(App& app)
{
    Log("InitModel: enter");

    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ort) { Log("InitModel: FAIL no OrtApi"); return false; }
    const OrtDmlApi* dmlApi = nullptr;
    if (OrtStatus* st = ort->GetExecutionProviderApi("DML", ORT_API_VERSION, (const void**)&dmlApi)) { Fail("GetExecutionProviderApi(DML)", st); return false; }
    if (!dmlApi) { Log("InitModel: FAIL null OrtDmlApi"); return false; }

    ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "xrapp4", &app.env);
    OrtSessionOptions* so = nullptr;
    ort->CreateSessionOptions(&so);

    HMODULE dmllib = LoadLibraryW(L"DirectML.dll");
    if (!dmllib) { Log("InitModel: FAIL DirectML.dll"); return false; }
    auto createDmlDevice = (HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**))
        GetProcAddress(dmllib, "DMLCreateDevice");
    if (!createDmlDevice) { Log("InitModel: FAIL DMLCreateDevice export"); return false; }

    ComPtr<IDMLDevice> dmlDevice;
    if (FAILED(createDmlDevice(app.device.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dmlDevice)))) { Log("InitModel: FAIL DMLCreateDevice"); return false; }
    if (OrtStatus* st = dmlApi->SessionOptionsAppendExecutionProvider_DML1(so, dmlDevice.Get(), app.mlQueue.Get())) { Fail("AppendExecutionProvider_DML1", st); return false; }

    std::wstring modelPath = L"C:\\Users\\paulj\\dev\\VRX\\bench\\models\\model_fixed_686x392.onnx";
    if (OrtStatus* st = ort->CreateSession(app.env, modelPath.c_str(), so, &app.ortSession)) { Fail("CreateSession", st); return false; }

    const OrtMemoryInfo* inInfos[1] = { nullptr };
    if (OrtStatus* st = ort->SessionGetMemoryInfoForInputs(app.ortSession, inInfos, 1)) { Fail("SessionGetMemoryInfoForInputs", st); return false; }

    // zero-copy model input on our device (see M1: needs UAV access + a CPU-writable heap)
    const UINT64 modelBytes = (UINT64)3 * H * W * sizeof(float);
    int64_t modelDims[4] = { 1, 3, H, W };

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = modelBytes;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(app.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   nullptr, IID_PPV_ARGS(&app.modelIn))))
    { Log("InitModel: FAIL model input resource"); return false; }
    D3D12_RANGE nr{ 0, 0 };
    if (FAILED(app.modelIn->Map(0, &nr, (void**)&app.modelInMapped))) { Log("InitModel: FAIL map model input"); return false; }

    void* dmlAlloc = nullptr;
    if (OrtStatus* st = dmlApi->CreateGPUAllocationFromD3DResource(app.modelIn.Get(), &dmlAlloc)) { Fail("CreateGPUAllocationFromD3DResource", st); return false; }
    if (OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(const_cast<OrtMemoryInfo*>(inInfos[0]), dmlAlloc, modelBytes,
                                                            modelDims, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &app.modelInValue))
    { Fail("CreateTensorWithDataAsOrtValue", st); return false; }

    app.rawDepth.resize((size_t)W * H);
    Log("InitModel: exit ok, %dx%d on the shared device via the ml queue (zero-copy input)", H, W);
    return true;
}

// scene (packed RGB) -> model -> nearness in [0,1] (0 = far, 1 = near). The model
// gives relative disparity-like values, larger = nearer. One caller at a time.
static bool RunModel(App& app, const std::vector<unsigned char>& rgb, std::vector<float>& outNear, float* rawMin, float* rawMax)
{
    if (!rawMin || !rawMax) return false;
    if (!app.ortSession || !app.modelInMapped || !app.modelInValue) return false;
    if (rgb.size() != (size_t)W * H * 3) { Log("RunModel: bad scene size %zu", rgb.size()); return false; }

    const float mean[3] = { 0.485f, 0.456f, 0.406f };
    const float istd[3] = { 1.f / 0.229f, 1.f / 0.224f, 1.f / 0.225f };
    for (int y = 0; y < H; y++)
    {
        for (int x = 0; x < W; x++)
        {
            size_t si = ((size_t)y * W + x) * 3;
            for (int c = 0; c < 3; c++)
            {
                float v = rgb[si + c] / 255.0f;
                app.modelInMapped[((size_t)c * H + y) * W + x] = (v - mean[c]) * istd[c];
            }
        }
    }

    OrtValue* out = nullptr;
    const char* inName = "pixel_values";
    const char* outName = "predicted_depth";
    if (OrtStatus* st = ort->Run(app.ortSession, nullptr, &inName, (const OrtValue* const*)&app.modelInValue, 1, &outName, 1, &out))
    { Fail("Run", st); return false; }

    // The output is a CPU tensor, so ORT has already synchronised with the GPU.
    float* dp = nullptr;
    ort->GetTensorMutableData(out, (void**)&dp);
    if (!dp) { ort->ReleaseValue(out); Log("RunModel: null output"); return false; }
    memcpy(app.rawDepth.data(), dp, app.rawDepth.size() * sizeof(float));
    ort->ReleaseValue(out);

    float dmin = app.rawDepth[0], dmax = app.rawDepth[0];
    for (float v : app.rawDepth) { dmin = std::min(dmin, v); dmax = std::max(dmax, v); }
    float inv = (dmax - dmin) > 1e-6f ? 1.0f / (dmax - dmin) : 0.0f;
    outNear.resize(app.rawDepth.size());
    for (size_t i = 0; i < outNear.size(); i++) outNear[i] = (app.rawDepth[i] - dmin) * inv;

    *rawMin = dmin; *rawMax = dmax;
    return true;
}

// ------------------------------------------------------------- depth handoff
static bool InitSlots(App& app)
{
    Log("InitSlots: enter");
    const UINT64 nearBytes = (UINT64)W * H * sizeof(float);
    const UINT64 colorBytes = (UINT64)ROW_PITCH * H;
    for (int i = 0; i < SLOTS; i++)
    {
        DepthSlot& s = app.slots[i];
        if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_UPLOAD, nearBytes, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_GENERIC_READ, s.nearUp, (void**)&s.nearMapped)) return false;
        if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_UPLOAD, colorBytes, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_GENERIC_READ, s.colorUp, (void**)&s.colorMapped)) return false;
    }
    Log("InitSlots: exit ok, %d slots", SLOTS);
    return true;
}

// Run the model on `rgb` and publish the result. Called by the main thread once
// before the worker starts, then only by the worker.
static bool ComputeAndPublish(App& app, const std::vector<unsigned char>& rgb, double sceneTime, std::vector<float>& nearScratch)
{
    auto m0 = std::chrono::steady_clock::now();
    float rawMin = 0, rawMax = 0;
    if (!RunModel(app, rgb, nearScratch, &rawMin, &rawMax)) return false;
    double modelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m0).count();

    DepthSlot& s = app.slots[app.writeSlot];
    if (app.imageRgb.empty()) RegionMeans(nearScratch, sceneTime, s.back, s.panel, s.marker);
    if (app.opt.useTruth) MakeTruth(nearScratch, sceneTime);

    memcpy(s.nearMapped, nearScratch.data(), nearScratch.size() * sizeof(float));
    if (app.opt.paired) PackRgba(rgb, s.colorMapped);
    s.sceneTime = sceneTime;
    s.modelMs = modelMs;
    s.rawMin = rawMin; s.rawMax = rawMax;
    s.completeTime = NowSeconds();

    app.writeSlot = app.readySlot.exchange(app.writeSlot | SLOT_FRESH) & (SLOT_FRESH - 1);
    app.depthPublished++;
    return true;
}

static void WorkerMain(App* app)
{
    g_threadName = "worker";
    if (!app) { Log("WorkerMain: null app"); return; }
    Log("WorkerMain: enter");

    std::vector<unsigned char> rgb;
    std::vector<float> nearScratch;
    uint64_t runs = 0;
    while (!app->stopWorker.load())
    {
        double t = 0;
        {
            std::lock_guard<std::mutex> lock(app->sceneMutex);
            rgb = app->sharedScene;
            t = app->sharedSceneTime;
        }
        if (!ComputeAndPublish(*app, rgb, t, nearScratch)) { Log("WorkerMain: model failed, stopping"); break; }
        runs++;
    }
    Log("WorkerMain: exit after %llu runs", (unsigned long long)runs);
}

// ------------------------------------------------------------- xr session
static bool InitXrSession(App& app)
{
    Log("InitXrSession: enter");

    XrGraphicsBindingD3D12KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    binding.device = app.device.Get();
    binding.queue = app.gfxQueue.Get();
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = app.systemId;
    XrResult r = xrCreateSession_(app.instance, &sci, &app.session);
    if (XR_FAILED(r)) { Log("InitXrSession: FAIL xrCreateSession %s", XRStr(r)); return false; }

    XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace = { {0,0,0,1}, {0,0,0} };
    r = xrCreateReferenceSpace_(app.session, &rsci, &app.space);
    if (XR_FAILED(r)) { Log("InitXrSession: FAIL xrCreateReferenceSpace %s", XRStr(r)); return false; }

    uint32_t fmtCount = 0;
    xrEnumFormats_(app.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumFormats_(app.session, fmtCount, &fmtCount, formats.data());

    // The warp writes RGBA8 and R32F intermediates and copies them into the
    // swapchain images, so the swapchain formats must be in those typeless families.
    int64_t colorFmt = 0, depthFmt = 0;
    for (auto f : formats) if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { colorFmt = f; break; }
    if (!colorFmt) for (auto f : formats) if (f == DXGI_FORMAT_R8G8B8A8_UNORM) { colorFmt = f; break; }
    for (auto f : formats) if (f == DXGI_FORMAT_D32_FLOAT) { depthFmt = f; break; }
    Log("InitXrSession: formats colour %lld depth %lld (of %u offered)", (long long)colorFmt, (long long)depthFmt, fmtCount);
    if (!colorFmt) { Log("InitXrSession: FAIL runtime offers no R8G8B8A8 colour format"); return false; }
    if (!depthFmt && app.opt.submitDepth) { Log("InitXrSession: FAIL runtime offers no D32_FLOAT depth format"); return false; }

    XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sc.sampleCount = 1; sc.width = W; sc.height = H; sc.faceCount = 1;
    sc.arraySize = VIEWS; sc.mipCount = 1;

    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = colorFmt;
    r = xrCreateSwapchain_(app.session, &sc, &app.colorSc);
    if (XR_FAILED(r)) { Log("InitXrSession: FAIL colour swapchain %s", XRStr(r)); return false; }

    if (app.opt.submitDepth)
    {
        sc.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sc.format = depthFmt;
        r = xrCreateSwapchain_(app.session, &sc, &app.depthSc);
        if (XR_FAILED(r)) { Log("InitXrSession: FAIL depth swapchain %s", XRStr(r)); return false; }
    }

    uint32_t cn = 0, dn = 0;
    xrEnumImages_(app.colorSc, 0, &cn, nullptr);
    app.cimgs.assign(cn, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumImages_(app.colorSc, cn, &cn, (XrSwapchainImageBaseHeader*)app.cimgs.data());
    if (cn == 0 || !app.cimgs[0].texture) { Log("InitXrSession: FAIL no colour swapchain images"); return false; }
    D3D12_RESOURCE_DESC cd = app.cimgs[0].texture->GetDesc();
    Log("InitXrSession: colour images %u (resource format %d, array %u)", cn, (int)cd.Format, (unsigned)cd.DepthOrArraySize);

    if (!app.opt.submitDepth) { Log("InitXrSession: exit ok (no depth swapchain - depth submission is off)"); return true; }

    xrEnumImages_(app.depthSc, 0, &dn, nullptr);
    app.dimgs.assign(dn, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumImages_(app.depthSc, dn, &dn, (XrSwapchainImageBaseHeader*)app.dimgs.data());
    if (dn == 0 || !app.dimgs[0].texture) { Log("InitXrSession: FAIL no depth swapchain images"); return false; }
    D3D12_RESOURCE_DESC dd = app.dimgs[0].texture->GetDesc();
    Log("InitXrSession: exit ok, depth images %u (resource format %d, array %u)", dn, (int)dd.Format, (unsigned)dd.DepthOrArraySize);
    return true;
}

// ------------------------------------------------------------ warp shader
// Row-parallel forward warp: one GPU thread owns one whole row of one eye, so the
// scatter + z-test + hole fill are race-free and identical to WarpEye (CPU).
static const char* kWarpHlsl = R"HLSL(
cbuffer C : register(b0)
{
    uint  W;
    uint  H;
    uint  doWarp;
    float scaleFocal;   // warpScale * focalPx
    float invZNear;
    float invZFar;
    float eye0;         // eye lateral offsets in metres (left negative)
    float eye1;
    float nearZ;
    float farZ;
    uint  indicator;    // 0 none, 1 green, 2 red
    uint  depthLie;
};

Texture2D<float4>        scene    : register(t0);
StructuredBuffer<float>  nearBuf  : register(t1);   // 0 = far .. 1 = near
RWTexture2DArray<float4> outColor : register(u0);
RWTexture2DArray<float>  outDepth : register(u1);   // D3D projective depth for nearZ/farZ

#define MAXW 686

[numthreads(1, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint y = id.y;
    uint e = id.z;
    if (y >= H) return;

    float eye = (e == 0) ? eye0 : eye1;
    uint base = y * W;
    int iw = (int)W;

    int src[MAXW];
    int x;
    [loop] for (x = 0; x < iw; x++) src[x] = -1;

    // scatter: every source pixel moves by its own disparity; nearer wins
    [loop] for (x = 0; x < iw; x++)
    {
        float n = nearBuf[base + x];
        int dx = x;
        if (doWarp != 0)
        {
            float invZ = invZFar + n * (invZNear - invZFar);
            float s = scaleFocal * eye * invZ;
            int r = (s < 0.0) ? -(int)floor(-s + 0.5) : (int)floor(s + 0.5);   // lround
            dx = x - r;
        }
        if (dx >= 0 && dx < iw)
        {
            int cur = src[dx];
            if (cur < 0 || n > nearBuf[base + cur]) src[dx] = x;
        }
    }

    // hole fill from the FARTHER of the nearest valid neighbours (background)
    int lastValid = -1;
    x = 0;
    [loop] while (x < iw)
    {
        if (src[x] >= 0) { lastValid = src[x]; x++; continue; }

        int r = x + 1;
        [loop] while (r < iw && src[r] < 0) r++;
        int rightValid = (r < iw) ? src[r] : -1;
        int pick = lastValid;
        if (pick < 0 || (rightValid >= 0 && nearBuf[base + rightValid] < nearBuf[base + pick])) pick = rightValid;
        if (pick < 0) pick = x;
        [loop] for (int h = x; h < r; h++) src[h] = -2 - pick;
        x = r;
    }

    [loop] for (x = 0; x < iw; x++)
    {
        int s = (src[x] >= 0) ? src[x] : (-2 - src[x]);
        float4 col = scene.Load(int3(s, y, 0));
        float invZ = invZFar + nearBuf[base + s] * (invZNear - invZFar);
        if (depthLie != 0) invZ = (x < iw / 2) ? (1.0 / 0.5) : (1.0 / 10.0);
        if (indicator != 0 && y >= 8 && y < 32 && x >= iw / 2 - 40 && x < iw / 2 + 40)
            col = (indicator == 1) ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
        outColor[uint3(x, y, e)] = col;
        outDepth[uint3(x, y, e)] = (farZ / (farZ - nearZ)) * (1.0 - nearZ * invZ);
    }
}
)HLSL";

static bool InitWarp(App& app)
{
    Log("InitWarp: enter");

    ComPtr<ID3DBlob> cs, err;
    HRESULT hr = D3DCompile(kWarpHlsl, strlen(kWarpHlsl), "warp.hlsl", nullptr, nullptr, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err);
    if (err && err->GetBufferSize() > 1) Log("InitWarp: compiler says: %s", (const char*)err->GetBufferPointer());
    if (FAILED(hr) || !cs) { Log("InitWarp: FAIL D3DCompile 0x%08X", (unsigned)hr); return false; }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2; ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2; ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(WarpConstants) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    ComPtr<ID3DBlob> rsBlob, rsErr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr);
    if (FAILED(hr)) { Log("InitWarp: FAIL serialize root signature: %s", rsErr ? (const char*)rsErr->GetBufferPointer() : "?"); return false; }
    hr = app.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&app.rootSig));
    if (FAILED(hr)) { Log("InitWarp: FAIL CreateRootSignature 0x%08X", (unsigned)hr); return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = app.rootSig.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    hr = app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.pso));
    if (FAILED(hr)) { Log("InitWarp: FAIL CreateComputePipelineState 0x%08X", (unsigned)hr); return false; }

    // resources. Typeless intermediates so the copies into the swapchain images
    // (UNORM_SRGB / D32_FLOAT) are same-family copies whatever view they use.
    ID3D12Device* dev = app.device.Get();
    if (!MakeTexture(dev, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, app.sceneTex)) return false;
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, (UINT64)W * H * sizeof(float), D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, app.nearBuf, nullptr)) return false;
    if (!MakeTexture(dev, DXGI_FORMAT_R8G8B8A8_TYPELESS, VIEWS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.colorOut)) return false;
    if (!MakeTexture(dev, DXGI_FORMAT_R32_TYPELESS, VIEWS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.depthOut)) return false;
    for (int i = 0; i < RING; i++)
    {
        if (!MakeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, (UINT64)ROW_PITCH * H, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_GENERIC_READ, app.sceneUp[i], (void**)&app.sceneUpMapped[i])) return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&app.descHeap)))) { Log("InitWarp: FAIL descriptor heap"); return false; }
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = app.descHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(app.sceneTex.Get(), &sv, h);
    h.ptr += inc;

    D3D12_SHADER_RESOURCE_VIEW_DESC bv{};
    bv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bv.Format = DXGI_FORMAT_UNKNOWN;
    bv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    bv.Buffer.NumElements = (UINT)(W * H);
    bv.Buffer.StructureByteStride = sizeof(float);
    dev->CreateShaderResourceView(app.nearBuf.Get(), &bv, h);
    h.ptr += inc;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uv.Texture2DArray.ArraySize = VIEWS;
    dev->CreateUnorderedAccessView(app.colorOut.Get(), nullptr, &uv, h);
    h.ptr += inc;

    uv.Format = DXGI_FORMAT_R32_FLOAT;
    dev->CreateUnorderedAccessView(app.depthOut.Get(), nullptr, &uv, h);

    Log("InitWarp: exit ok (shader %zu bytes)", cs->GetBufferSize());
    return true;
}

// upload buffer (RGBA, ROW_PITCH rows) -> sceneTex
static void RecordUploadScene(App& app, ID3D12Resource* upload)
{
    if (!upload) return;

    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    Transition(cl, app.sceneTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = app.sceneTex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = W;
    src.PlacedFootprint.Footprint.Height = H;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = ROW_PITCH;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(cl, app.sceneTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// upload buffer (float[W*H]) -> nearBuf
static void RecordUploadNear(App& app, ID3D12Resource* upload)
{
    if (!upload) return;

    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    Transition(cl, app.nearBuf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyBufferRegion(app.nearBuf.Get(), 0, upload, 0, (UINT64)W * H * sizeof(float));
    Transition(cl, app.nearBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// dispatch the warp for both eyes; leaves colorOut/depthOut in COPY_SOURCE
static void RecordWarp(App& app, const WarpConstants& c)
{
    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    ID3D12DescriptorHeap* heaps[] = { app.descHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(app.rootSig.Get());
    cl->SetPipelineState(app.pso.Get());
    cl->SetComputeRoot32BitConstants(0, sizeof(WarpConstants) / 4, &c, 0);
    cl->SetComputeRootDescriptorTable(1, app.descHeap->GetGPUDescriptorHandleForHeapStart());
    cl->Dispatch(1, (H + 7) / 8, VIEWS);
    Transition(cl, app.colorOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cl, app.depthOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
}

static void RecordWarpOutputsBackToUav(App& app)
{
    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    Transition(cl, app.colorOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cl, app.depthOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// warp outputs -> the acquired swapchain images. Spec (XR_KHR_D3D12_enable):
// acquired colour images are in RENDER_TARGET, depth images in DEPTH_WRITE, and
// must be released in the same state. depthImg may be null (depth not submitted).
static void RecordCopyToSwapchain(App& app, ID3D12Resource* colorImg, ID3D12Resource* depthImg)
{
    if (!colorImg) return;

    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    Transition(cl, colorImg, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    if (depthImg) Transition(cl, depthImg, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT e = 0; e < VIEWS; e++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = e;
        src.SubresourceIndex = e;

        dst.pResource = colorImg; src.pResource = app.colorOut.Get();
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (!depthImg) continue;
        dst.pResource = depthImg; src.pResource = app.depthOut.Get();
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Transition(cl, colorImg, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (depthImg) Transition(cl, depthImg, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

// --------------------------------------------------------------- self test
// GPU warp vs the CPU reference, pixel for pixel. Returns false on mismatch.
static bool SelfTestCase(App& app, const char* name, const std::vector<unsigned char>& scene,
                         const std::vector<float>& near01, const WarpConstants& c)
{
    if (!name) return false;
    if (scene.size() != (size_t)W * H * 3 || near01.size() != (size_t)W * H) { Log("SelfTest[%s]: bad input sizes", name); return false; }
    Log("SelfTest[%s]: enter (doWarp %u scaleFocal %.2f eyes %.4f/%.4f)", name, c.doWarp, c.scaleFocal, c.eye0, c.eye1);

    ID3D12Device* dev = app.device.Get();
    ComPtr<ID3D12Resource> nearUp;
    float* nearMapped = nullptr;
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, (UINT64)W * H * sizeof(float), D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nearUp, (void**)&nearMapped)) return false;
    memcpy(nearMapped, near01.data(), near01.size() * sizeof(float));
    PackRgba(scene, app.sceneUpMapped[0]);

    // readback layout
    D3D12_RESOURCE_DESC cdesc = app.colorOut->GetDesc(), ddesc = app.depthOut->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT cfp[VIEWS], dfp[VIEWS];
    UINT64 cTotal = 0, dTotal = 0;
    dev->GetCopyableFootprints(&cdesc, 0, VIEWS, 0, cfp, nullptr, nullptr, &cTotal);
    dev->GetCopyableFootprints(&ddesc, 0, VIEWS, 0, dfp, nullptr, nullptr, &dTotal);
    ComPtr<ID3D12Resource> crb, drb;
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, cTotal, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, crb, nullptr)) return false;
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, dTotal, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, drb, nullptr)) return false;

    WaitFence(app, app.fenceVal);
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    RecordUploadScene(app, app.sceneUp[0].Get());
    RecordUploadNear(app, nearUp.Get());
    RecordWarp(app, c);
    for (UINT e = 0; e < VIEWS; e++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = e;
        dst.pResource = crb.Get(); dst.PlacedFootprint = cfp[e]; src.pResource = app.colorOut.Get();
        app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        dst.pResource = drb.Get(); dst.PlacedFootprint = dfp[e]; src.pResource = app.depthOut.Get();
        app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    RecordWarpOutputsBackToUav(app);
    app.cmdList->Close();
    auto g0 = std::chrono::steady_clock::now();
    WaitFence(app, SubmitAndSignal(app));
    double gpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count();

    unsigned char* cp = nullptr; float* dp = nullptr;
    if (FAILED(crb->Map(0, nullptr, (void**)&cp)) || FAILED(drb->Map(0, nullptr, (void**)&dp)) || !cp || !dp) { Log("SelfTest[%s]: FAIL map readback", name); return false; }

    std::vector<unsigned char> refColor((size_t)ROW_PITCH * H);
    std::vector<float> refDepth((size_t)(ROW_PITCH / 4) * H);
    const float eyes[VIEWS] = { c.eye0, c.eye1 };
    const float warpScale = 1.0f;       // folded into scaleFocal below
    size_t badColor = 0, badDepth = 0;
    double cpuMs = 0;
    for (UINT e = 0; e < VIEWS; e++)
    {
        auto c0 = std::chrono::steady_clock::now();
        WarpEye(scene, near01, eyes[e], c.scaleFocal, warpScale, c.invZNear, c.invZFar, c.nearZ, c.farZ, c.doWarp != 0, refColor.data(), refDepth.data());
        cpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();

        for (int y = 0; y < H; y++)
        {
            const unsigned char* grow = cp + cfp[e].Offset + (size_t)y * cfp[e].Footprint.RowPitch;
            const float* gdrow = (const float*)((const unsigned char*)dp + dfp[e].Offset + (size_t)y * dfp[e].Footprint.RowPitch);
            const unsigned char* rrow = refColor.data() + (size_t)y * ROW_PITCH;
            const float* rdrow = refDepth.data() + (size_t)y * (ROW_PITCH / 4);
            for (int x = 0; x < W; x++)
            {
                if (memcmp(grow + x * 4, rrow + x * 4, 4) != 0) badColor++;
                if (fabsf(gdrow[x] - rdrow[x]) > 1e-5f) badDepth++;
            }
        }
    }
    crb->Unmap(0, nullptr);
    drb->Unmap(0, nullptr);

    // A rounding tie (shift exactly on .5) may legitimately fall differently on the
    // GPU; anything beyond a handful of pixels is a real shader bug.
    const size_t total = (size_t)W * H * VIEWS;
    const size_t tolerance = total / 2000;          // 0.05 %
    bool ok = badColor <= tolerance && badDepth <= tolerance;
    Log("SelfTest[%s]: exit %s - mismatches colour %zu depth %zu of %zu px (tolerance %zu) | GPU both eyes %.2f ms incl. upload+readback, CPU reference %.2f ms",
        name, ok ? "PASS" : "FAIL", badColor, badDepth, total, tolerance, gpuMs, cpuMs);
    return ok;
}

static bool SelfTest(App& app, const std::vector<unsigned char>& inputScene, const std::vector<float>& inputNear)
{
    Log("SelfTest: enter");
    WarpConstants c{};
    c.w = W; c.h = H; c.doWarp = 1;
    c.scaleFocal = 362.0f;                 // representative: the PS VR2 shared-FOV focal
    c.invZNear = 1.0f / 1.2f; c.invZFar = 1.0f / 12.0f;
    c.eye0 = -0.0355f; c.eye1 = 0.0355f;
    c.nearZ = DEPTH_NEAR_Z; c.farZ = DEPTH_FAR_Z;

    std::vector<unsigned char> synth;
    std::vector<float> truth;
    MakeScene(synth, 1.7);
    MakeTruth(truth, 1.7);

    bool ok = SelfTestCase(app, "synthetic+truth", synth, truth, c);
    ok = SelfTestCase(app, "input+model", inputScene, inputNear, c) && ok;

    WarpConstants big = c;
    big.scaleFocal *= 4.0f;                // large disparity: exercises hole fill and edge clipping
    ok = SelfTestCase(app, "input+model x4", inputScene, inputNear, big) && ok;

    WarpConstants off = c;
    off.doWarp = 0;
    ok = SelfTestCase(app, "no-warp", inputScene, inputNear, off) && ok;

    Log("SelfTest: exit %s", ok ? "ALL PASS" : "FAILED");
    return ok;
}

// -------------------------------------------------------------- frame loop
static void RunFrameLoop(App& app)
{
    Log("RunFrameLoop: enter (%.0fs)%s%s", app.opt.runSeconds, app.opt.doWarp ? "" : " [warp disabled]",
        app.opt.paired ? " [paired colour+depth]" : " [latest colour + latest completed depth]");

    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, exitLoop = false, haveDepth = false, sceneUploaded = false, printedView = false;
    const bool dynamicScene = app.imageRgb.empty();
    double loopStart = NowSeconds(), lastReport = loopStart;
    uint64_t frames = 0, drawn = 0, depthUpdates = 0;
    uint64_t repFrames = 0, repDrawn = 0, repDepth = 0;
    double repCpuMs = 0, repAgeMs = 0;
    std::vector<unsigned char> scene;
    const DepthSlot* cur = nullptr;

    // depth-usage experiment state
    const double period = app.opt.abSeconds > 0.0 ? app.opt.abSeconds : 6.0;
    double firstDrawTime = -1.0;
    long long lastPhase = -1;
    const bool useDepthSc = app.opt.submitDepth;
    bool depthOn = useDepthSc;
    XrPosef frozenPose[VIEWS] = {};

    while (!exitLoop)
    {
        XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent_(app.instance, &ev) == XR_SUCCESS)
        {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                auto* sc = (XrEventDataSessionStateChanged*)&ev;
                static const char* nm[] = { "UNKNOWN","IDLE","READY","SYNCHRONIZED","VISIBLE","FOCUSED","STOPPING","LOSS_PENDING","EXITING" };
                int si = (int)sc->state;
                if (sc->state != state) Log("RunFrameLoop: session state -> %s", (si >= 0 && si <= 8) ? nm[si] : "?");
                state = sc->state;
                if (state == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = XR_SUCCEEDED(xrBeginSession_(app.session, &bi));
                    Log("RunFrameLoop: xrBeginSession %s", running ? "OK" : "failed");
                }
                else if (state == XR_SESSION_STATE_STOPPING) { xrEndSession_(app.session); running = false; }
                else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) exitLoop = true;
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
        if (exitLoop) break;
        if (!running)
        {
            Sleep(10);
            if (NowSeconds() - loopStart > app.opt.runSeconds) exitLoop = true;
            continue;
        }

        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        if (XR_FAILED(xrWaitFrame_(app.session, nullptr, &fs))) { Log("RunFrameLoop: xrWaitFrame failed"); break; }
        xrBeginFrame_(app.session, nullptr);
        frames++; repFrames++;

        const XrCompositionLayerBaseHeader* layers[1] = { nullptr };
        XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        XrCompositionLayerProjectionView pviews[VIEWS];
        XrCompositionLayerDepthInfoKHR dinfo[VIEWS];

        if (fs.shouldRender)
        {
            auto c0 = std::chrono::steady_clock::now();

            XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = app.space;
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            uint32_t vc = VIEWS;
            XrView views[VIEWS] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
            XrResult vr = xrLocateViews_(app.session, &vli, &vs, VIEWS, &vc, views);
            if (XR_FAILED(vr)) Log("RunFrameLoop: xrLocateViews %s", XRStr(vr));

            // Eye offset from the measured IPD (see xrapp3 / M3 findings).
            float ex = views[1].pose.position.x - views[0].pose.position.x;
            float ey = views[1].pose.position.y - views[0].pose.position.y;
            float ez = views[1].pose.position.z - views[0].pose.position.z;
            float ipd = sqrtf(ex * ex + ey * ey + ez * ez);

            // Same image in both eyes => ONE symmetric FOV and ONE orientation for
            // both views, or the asymmetric per-eye FOVs produce divergent disparity.
            float tanHalfX = 1e9f;
            for (uint32_t e = 0; e < VIEWS; e++)
            {
                tanHalfX = std::min(tanHalfX, tanf(fabsf(views[e].fov.angleLeft)));
                tanHalfX = std::min(tanHalfX, tanf(fabsf(views[e].fov.angleRight)));
            }
            if (!(tanHalfX > 0.01f && tanHalfX < 100.0f)) tanHalfX = 1.0f;
            float tanHalfY = tanHalfX * (float)H / (float)W;
            XrFovf sharedFov{ -atanf(tanHalfX), atanf(tanHalfX), atanf(tanHalfY), -atanf(tanHalfY) };
            float focalPx = (float)(W * 0.5) / tanHalfX;

            XrQuaternionf q0 = views[0].pose.orientation, q1 = views[1].pose.orientation;
            float qd = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
            float qs = qd < 0 ? -1.0f : 1.0f;
            XrQuaternionf sharedRot{ q0.x + qs * q1.x, q0.y + qs * q1.y, q0.z + qs * q1.z, q0.w + qs * q1.w };
            float qn = sqrtf(sharedRot.x * sharedRot.x + sharedRot.y * sharedRot.y + sharedRot.z * sharedRot.z + sharedRot.w * sharedRot.w);
            if (qn > 1e-6f) { sharedRot.x /= qn; sharedRot.y /= qn; sharedRot.z /= qn; sharedRot.w /= qn; }
            else sharedRot = q0;

            if (!printedView)
            {
                printedView = true;
                Log("RunFrameLoop: shared fov_x %.1f deg  ipd %.1f mm  focal %.1f px  warp scale %.2f",
                    (sharedFov.angleRight - sharedFov.angleLeft) * 57.2958f, ipd * 1000.0f, focalPx, app.opt.warpScale);
            }

            // --- depth-usage experiment: phase change => toggle depth / re-capture pose
            if (firstDrawTime < 0.0) firstDrawTime = NowSeconds();
            long long phase = (long long)((NowSeconds() - firstDrawTime) / period);
            if (phase != lastPhase)
            {
                lastPhase = phase;
                if (app.opt.abSeconds > 0.0) depthOn = (phase % 2) == 0;
                for (uint32_t e = 0; e < VIEWS; e++) { frozenPose[e] = views[e].pose; frozenPose[e].orientation = sharedRot; }
                if (app.opt.abSeconds > 0.0 || app.opt.freezePose)
                    Log("RunFrameLoop: phase %lld - depth chain %s%s%s", phase, depthOn ? "ON (green)" : "OFF (red)",
                        app.opt.freezePose ? ", pose re-captured and frozen" : "", app.opt.depthLie ? ", depth is the half/half LIE" : "");
            }

            uint32_t cIdx = 0, dIdx = 0;
            XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            bool gotColor = XR_SUCCEEDED(xrAcquireImage_(app.colorSc, &ai, &cIdx));
            bool gotDepth = useDepthSc && XR_SUCCEEDED(xrAcquireImage_(app.depthSc, &ai, &dIdx));
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            bool ok = gotColor && (gotDepth || !useDepthSc) && XR_SUCCEEDED(xrWaitImage_(app.colorSc, &wi));
            if (ok && useDepthSc) ok = XR_SUCCEEDED(xrWaitImage_(app.depthSc, &wi));

            if (ok)
            {
                const int ring = (int)(drawn % RING);
                WaitFence(app, app.frameFence[ring]);         // normally already complete
                app.cmdAlloc[ring]->Reset();
                app.cmdList->Reset(app.cmdAlloc[ring].Get(), nullptr);

                // --- latest scene: to the worker, and (unless paired) to the GPU
                double t = NowSeconds();
                if (dynamicScene)
                {
                    MakeScene(scene, t);
                    std::lock_guard<std::mutex> lock(app.sceneMutex);
                    app.sharedScene = scene;
                    app.sharedSceneTime = t;
                }
                if (!app.opt.paired && (dynamicScene || !sceneUploaded))
                {
                    PackRgba(dynamicScene ? scene : app.imageRgb, app.sceneUpMapped[ring]);
                    RecordUploadScene(app, app.sceneUp[ring].Get());
                    sceneUploaded = true;
                }

                // --- latest COMPLETED depth: take it if the worker published one
                bool tookSlot = false;
                if (app.readySlot.load() & SLOT_FRESH)
                {
                    WaitFence(app, app.slotFence);            // GPU must be done with the slot we hand back
                    app.readSlot = app.readySlot.exchange(app.readSlot) & (SLOT_FRESH - 1);
                    cur = &app.slots[app.readSlot];
                    RecordUploadNear(app, cur->nearUp.Get());
                    if (app.opt.paired) RecordUploadScene(app, cur->colorUp.Get());
                    tookSlot = true; haveDepth = true;
                    depthUpdates++; repDepth++;
                }

                if (haveDepth)
                {
                    WarpConstants c{};
                    c.w = W; c.h = H; c.doWarp = app.opt.doWarp ? 1u : 0u;
                    c.scaleFocal = app.opt.warpScale * focalPx;
                    c.invZNear = 1.0f / 1.2f;      // nearest content ~1.2 m
                    c.invZFar = 1.0f / 12.0f;      // furthest content ~12 m
                    c.eye0 = -0.5f * ipd; c.eye1 = 0.5f * ipd;
                    c.nearZ = DEPTH_NEAR_Z; c.farZ = DEPTH_FAR_Z;
                    c.indicator = (app.opt.abSeconds > 0.0) ? (depthOn ? 1u : 2u) : 0u;
                    c.depthLie = app.opt.depthLie ? 1u : 0u;
                    RecordWarp(app, c);
                    RecordCopyToSwapchain(app, app.cimgs[cIdx].texture, useDepthSc ? app.dimgs[dIdx].texture : nullptr);
                    RecordWarpOutputsBackToUav(app);
                }
                app.cmdList->Close();
                UINT64 fv = SubmitAndSignal(app);             // no CPU wait: the runtime syncs on the gfx queue
                app.frameFence[ring] = fv;
                if (tookSlot) app.slotFence = fv;

                drawn++; repDrawn++;
                if (cur) repAgeMs += (NowSeconds() - cur->completeTime) * 1000.0;
            }
            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            if (gotColor) xrReleaseImage_(app.colorSc, &ri);
            if (gotDepth) xrReleaseImage_(app.depthSc, &ri);

            if (ok && haveDepth)
            {
                for (uint32_t e = 0; e < VIEWS; e++)
                {
                    pviews[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                    pviews[e].pose = views[e].pose;
                    pviews[e].pose.orientation = sharedRot;
                    if (app.opt.freezePose) pviews[e].pose = frozenPose[e];
                    pviews[e].fov = sharedFov;
                    pviews[e].subImage.swapchain = app.colorSc;
                    pviews[e].subImage.imageRect = { {0, 0}, {W, H} };
                    pviews[e].subImage.imageArrayIndex = e;

                    dinfo[e] = { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
                    dinfo[e].subImage.swapchain = app.depthSc;
                    dinfo[e].subImage.imageRect = { {0, 0}, {W, H} };
                    dinfo[e].subImage.imageArrayIndex = e;
                    dinfo[e].minDepth = 0.0f;
                    dinfo[e].maxDepth = 1.0f;
                    dinfo[e].nearZ = DEPTH_NEAR_Z;
                    dinfo[e].farZ = DEPTH_FAR_Z;
                    pviews[e].next = depthOn ? &dinfo[e] : nullptr;
                }
                proj.space = app.space;
                proj.viewCount = VIEWS;
                proj.views = pviews;
                layers[0] = (XrCompositionLayerBaseHeader*)&proj;
            }
            repCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        }

        XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = layers[0] ? 1 : 0;
        fei.layers = layers;
        XrResult er = xrEndFrame_(app.session, &fei);
        if (XR_FAILED(er)) Log("RunFrameLoop: xrEndFrame %s", XRStr(er));

        double now = NowSeconds();
        if (now - lastReport >= 2.0)
        {
            double dt = now - lastReport;
            Log("render %.1f fps (drawn %.1f fps, cpu %.2f ms/frame) | depth %.1f updates/s, model %.1f ms, age %.1f ms | raw %.2f..%.2f | model near: back %.2f panel %.2f marker %.2f%s",
                repFrames / dt, repDrawn / dt, repDrawn ? repCpuMs / repDrawn : 0.0,
                repDepth / dt, cur ? cur->modelMs : 0.0, repDrawn ? repAgeMs / repDrawn : 0.0,
                cur ? cur->rawMin : 0.f, cur ? cur->rawMax : 0.f,
                cur ? cur->back : 0.f, cur ? cur->panel : 0.f, cur ? cur->marker : 0.f,
                app.opt.useTruth ? " [warp uses TRUTH]" : "");
            lastReport = now; repFrames = repDrawn = repDepth = 0; repCpuMs = repAgeMs = 0;
        }
        if (now - loopStart > app.opt.runSeconds) exitLoop = true;
    }

    double el = NowSeconds() - loopStart;
    Log("RunFrameLoop: exit - frames %llu (%.1f fps) drawn %llu (%.1f fps) depth updates %llu (%.1f /s), worker published %llu",
        (unsigned long long)frames, frames / (el > 0 ? el : 1), (unsigned long long)drawn, drawn / (el > 0 ? el : 1),
        (unsigned long long)depthUpdates, depthUpdates / (el > 0 ? el : 1), (unsigned long long)app.depthPublished.load());
}

// ------------------------------------------------------------------- main
static void Shutdown(App& app)
{
    Log("Shutdown: enter");
    app.stopWorker = true;
    if (app.worker.joinable()) app.worker.join();
    WaitFence(app, app.fenceVal);

    DumpDebugMessages(app);

    if (app.modelInValue) ort->ReleaseValue(app.modelInValue);
    if (app.ortSession) ort->ReleaseSession(app.ortSession);
    if (app.colorSc) xrDestroySwapchain_(app.colorSc);
    if (app.depthSc) xrDestroySwapchain_(app.depthSc);
    if (app.session) xrDestroySession_(app.session);
    if (app.instance) xrDestroyInstance_(app.instance);
    if (app.fenceEvent) CloseHandle(app.fenceEvent);
    Log("Shutdown: exit");
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_t0 = std::chrono::steady_clock::now();

    static App app;
    if (!ParseArgs(argc, argv, &app.opt)) return 1;

    if (!app.opt.imagePath.empty())
    {
        if (!LoadImageWIC(app.opt.imagePath.c_str(), app.imageRgb)) { Log("main: FAIL cannot load image %ls", app.opt.imagePath.c_str()); return 1; }
        Log("main: image %ls -> %dx%d", app.opt.imagePath.c_str(), W, H);
        if (app.opt.useTruth) { Log("main: --truth ignored with --image (no ground truth for a photo)"); app.opt.useTruth = false; }
    }

    int rc = 1;
    do
    {
        if (!InitXrInstance(app)) break;
        if (!InitD3D(app)) break;
        if (!InitModel(app)) break;
        if (!InitSlots(app)) break;
        if (!InitXrSession(app)) break;
        if (!InitWarp(app)) break;

        // First depth result, computed synchronously: the log shows the model's
        // opinion of the input even if the HMD never wakes, and the render thread
        // has a depth from its very first frame.
        std::vector<unsigned char> firstScene;
        std::vector<float> firstNear;
        if (app.imageRgb.empty()) MakeScene(firstScene, 0.0); else firstScene = app.imageRgb;
        app.sharedScene = firstScene;
        app.sharedSceneTime = 0.0;
        if (!ComputeAndPublish(app, firstScene, 0.0, firstNear)) break;
        {
            const DepthSlot& s = app.slots[app.readySlot.load() & (SLOT_FRESH - 1)];
            Log("main: first depth in %.1f ms (includes warm-up), raw %.3f..%.3f", s.modelMs, s.rawMin, s.rawMax);
            if (app.imageRgb.empty())
                Log("main: model nearness (0 far..1 near): backdrop %.2f panel %.2f marker %.2f  [truth 0.00 / 0.50 / 1.00]", s.back, s.panel, s.marker);
        }
        if (app.opt.doDump)
        {
            std::vector<unsigned char> g(firstNear.size());
            for (size_t i = 0; i < g.size(); i++) g[i] = (unsigned char)(firstNear[i] * 255.0f + 0.5f);
            WritePNM("xrapp4_scene.ppm", "P6", firstScene.data(), firstScene.size());
            WritePNM("xrapp4_near.pgm", "P5", g.data(), g.size());
        }

        if (!SelfTest(app, firstScene, firstNear)) { Log("main: GPU warp does not match the CPU reference - not presenting"); break; }
        if (app.opt.selfTestOnly) { rc = 0; break; }

        app.worker = std::thread(WorkerMain, &app);
        RunFrameLoop(app);
        rc = 0;
    } while (false);

    Shutdown(app);
    return rc;
}
