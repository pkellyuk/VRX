// M4: live desktop / window capture -> AI depth -> stereo 3D in the headset.
//
// xrapp4 proved the structure (worker-thread model, compute-shader warp, 120 fps)
// on a CPU-side 686x392 picture. xrapp5 makes the input real and GPU-resident:
//
//   capture thread (D3D11, Windows.Graphics.Capture)
//       monitor/window frame -> copy into a ring of SHARED textures -> shared fence
//                    |                                   |
//   worker thread (ml queue)                 render thread (gfx queue, OpenXR)
//       prep shader: source texture ->           latest source texture (full res)
//         model input buffer (NCHW,              + latest COMPLETED depth (686x392)
//         box-filtered, normalised)              -> forward-warp compute shader at
//       model (ORT + DirectML)                      COLOUR resolution, depth sampled
//       percentile + time-smoothed                  bilinearly
//         normalisation -> publish slot          -> colour swapchain -> xrEndFrame
//
// * The picture never touches the CPU: capture texture -> (shared) -> D3D12 SRV for
//   both the warp and the model-input prep shader. Only the model's 1 MB output
//   comes back (M1: ORT cannot bind a DML device output from this API surface).
// * Source textures are ALLOW_SIMULTANEOUS_ACCESS: one writer, many readers across
//   queues/devices, no resource barriers. Cross-queue ordering is by fence
//   (ID3D12CommandQueue::Wait), never by stalling a CPU thread.
// * Colour is presented at up to 1920 px wide; depth stays at the model's 686x392.
// * Depth normalisation is robust (0.5 / 99.5 percentiles, not min/max) and
//   smoothed over time, so one bright outlier or a changing scene does not make the
//   whole picture's depth "breathe". --no-smooth restores per-frame min/max.
// * Every start runs a self-test: GPU warp vs the CPU reference (WarpEye) and GPU
//   model-input prep vs the CPU preprocessing, so both shaders are verified without
//   a headset.
//
// usage: xrapp5 [seconds] [source] [options]
//   source:  --capture            primary monitor (default if no other source given)
//            --monitor=N          Nth monitor (0-based, EnumDisplayMonitors order)
//            --window=TEXT        first visible top-level window whose title contains TEXT
//            --image=PATH         still image
//            --synthetic          the moving test scene (with --truth: ground-truth depth)
//   options: --scale=N --no-warp --paired --no-smooth --tau=SECONDS
//            --selftest --debug --dump --submit-depth --freeze-pose --ab=N --depth-lie

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <objbase.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

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

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;

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
    printf("[%8.3f][%-7s] %s\n", NowSeconds(), g_threadName, buf);
}

// ------------------------------------------------------------------ types
enum class SourceKind { Capture, Image, Synthetic };

struct Options
{
    double runSeconds = 75.0;
    SourceKind source = SourceKind::Capture;
    int monitorIndex = -1;              // -1 = primary
    std::wstring windowTitle;
    std::wstring imagePath;
    bool doWarp = true;
    float warpScale = 1.0f;
    bool useTruth = false;
    bool doDump = false;
    bool paired = false;
    bool selfTestOnly = false;
    bool debugLayer = false;
    bool smooth = true;
    double smoothTau = 0.4;             // seconds
    bool submitDepth = false;           // SteamVR ignores it (measured) - opt-in only
    bool freezePose = false;
    bool depthLie = false;
    double abSeconds = 0.0;
};

static const int SLOTS = 3;
static const int SLOT_FRESH = 4;        // flag bit in readySlot
static const int RING = 3;              // in-flight render frames
static const int SRC_RING = 8;          // source textures (one writer, readers lag by a few frames at most)
static const int MAX_COLOR_W = 1920;
static const uint32_t VIEWS = 2;
static const float DEPTH_NEAR_Z = 0.10f, DEPTH_FAR_Z = 30.0f;

// One completed depth result (worker -> render thread).
struct DepthSlot
{
    ComPtr<ID3D12Resource> nearUp;      // UPLOAD heap, float[W*H], 0 = far .. 1 = near
    float* nearMapped = nullptr;
    int srcIndex = -1;                  // the source texture the depth was computed from
    double sceneTime = 0;
    double completeTime = 0;
    double modelMs = 0;
    float lo = 0, hi = 0;               // normalisation range actually used
    float back = 0, panel = 0, marker = 0;
};

// The newest source frame. Readers make their queue Wait() on (fence, value).
struct SourcePub
{
    int index = -1;
    ID3D12Fence* fence = nullptr;
    UINT64 value = 0;
    double time = 0;
    uint64_t seq = 0;
};

struct WarpConstants                    // must match cbuffer C in kWarpHlsl
{
    uint32_t cw, ch, dw, dh;
    uint32_t doWarp;
    float scaleFocal, invZNear, invZFar;
    float eye0, eye1, nearZ, farZ;
    uint32_t indicator, depthLie, writeDepth, exactLoad;
};

struct PrepConstants                    // must match cbuffer C in kPrepHlsl
{
    uint32_t dw, dh, taps, exactLoad;
};

// Everything the warp writes to, at one colour resolution.
struct WarpTarget
{
    int cw = 0, ch = 0;
    ComPtr<ID3D12Resource> colorOut;    // R8G8B8A8_TYPELESS[2], UNORDERED_ACCESS at rest
    ComPtr<ID3D12Resource> depthOut;    // R32_TYPELESS[2],      UNORDERED_ACCESS at rest
    ComPtr<ID3D12Resource> scratch;     // uint[cw*ch*2*2]: per-row scatter state
    UINT tableIndex = 0;                // first of 4 descriptors: near SRV, colour UAV, depth UAV, scratch UAV
};

struct RangeSmoother
{
    bool have = false;
    float lo = 0, hi = 1;
    double lastTime = 0;
};

struct Capture
{
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D11Texture2D> tex11[SRC_RING];
    wgc::GraphicsCaptureItem item{ nullptr };
    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    wdx::Direct3D11::IDirect3DDevice rtDevice{ nullptr };
    std::thread thread;
    std::atomic<uint64_t> frames{ 0 };
};

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

    // D3D12: one device, two queues (see xrapp4)
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> gfxQueue, mlQueue;
    ComPtr<ID3D12GraphicsCommandList> cmdList;           // render/main thread, gfx queue
    ComPtr<ID3D12CommandAllocator> cmdAlloc[RING];
    UINT64 frameFence[RING] = {};
    ComPtr<ID3D12Fence> fence;                           // gfx queue
    UINT64 fenceVal = 0;
    HANDLE fenceEvent = nullptr;
    ComPtr<ID3D12GraphicsCommandList> mlCmdList;         // worker thread, ml queue (prep shader)
    ComPtr<ID3D12CommandAllocator> mlCmdAlloc;
    ComPtr<ID3D12Fence> mlFence;
    UINT64 mlFenceVal = 0;
    HANDLE mlFenceEvent = nullptr;

    // source
    int srcW = 0, srcH = 0;                              // source texture size
    int colorW = 0, colorH = 0;                          // presented size
    DXGI_FORMAT srcFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource> srcTex[SRC_RING];
    ComPtr<ID3D12Resource> srcUp[RING];                  // CPU sources only
    unsigned char* srcUpMapped[RING] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT srcFootprint{};
    int srcNext = 0;
    std::mutex pubMutex;
    SourcePub pub;
    std::vector<unsigned char> imageRgb;
    ComPtr<ID3D12Fence> captureFence;                    // shared with D3D11
    UINT64 captureFenceVal = 0;
    Capture cap;

    // model
    OrtEnv* env = nullptr;
    OrtSession* ortSession = nullptr;
    OrtValue* modelInValue = nullptr;
    ComPtr<ID3D12Resource> modelIn;                      // DEFAULT heap, written by the prep shader
    std::vector<float> rawDepth;
    RangeSmoother smoother;

    // shaders
    ComPtr<ID3D12RootSignature> warpRootSig, prepRootSig;
    ComPtr<ID3D12PipelineState> warpPso, prepPso;
    ComPtr<ID3D12DescriptorHeap> descHeap;
    UINT descInc = 0;
    ComPtr<ID3D12Resource> nearBuf;                      // structured float[W*H]
    WarpTarget mainTarget, testTarget;
    ComPtr<ID3D12Resource> testSrc;                      // WxH RGBA source for the self-test

    // depth handoff (triple buffer)
    DepthSlot slots[SLOTS];
    std::atomic<int> readySlot{ 1 };
    int writeSlot = 0;
    int readSlot = 2;
    UINT64 slotFence = 0;

    std::thread worker;
    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> depthPublished{ 0 };
};

// descriptor heap layout
static const UINT DESC_SRC0 = 0;                         // SRC_RING source SRVs
static const UINT DESC_MAIN_TABLE = SRC_RING;            // 4
static const UINT DESC_TEST_SRC = SRC_RING + 4;          // 1
static const UINT DESC_TEST_TABLE = SRC_RING + 5;        // 4
static const UINT DESC_COUNT = SRC_RING + 9;

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

static bool MakeTexture(ID3D12Device* device, int w, int h, DXGI_FORMAT fmt, UINT16 arraySize, D3D12_RESOURCE_FLAGS flags,
                        D3D12_HEAP_FLAGS heapFlags, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    if (!device) return false;
    if (w <= 0 || h <= 0) return false;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = (UINT64)w; d.Height = (UINT)h; d.DepthOrArraySize = arraySize; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = flags;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = fmt;
    const bool isRt = (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    HRESULT hr = device->CreateCommittedResource(&hp, heapFlags, &d, state, isRt ? &cv : nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { Log("MakeTexture: CreateCommittedResource failed 0x%08X (%dx%d format %d)", (unsigned)hr, w, h, (int)fmt); return false; }
    return true;
}

// A source texture: one writer (capture device / gfx queue), readers on both
// queues. SIMULTANEOUS_ACCESS textures stay in COMMON and need no barriers.
static bool MakeSourceTexture(App& app, int w, int h, DXGI_FORMAT fmt, bool shared, ComPtr<ID3D12Resource>& out)
{
    return MakeTexture(app.device.Get(), w, h, fmt, 1,
                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
                       shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, out);
}

static D3D12_CPU_DESCRIPTOR_HANDLE CpuDesc(App& app, UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = app.descHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * app.descInc;
    return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE GpuDesc(App& app, UINT index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = app.descHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)index * app.descInc;
    return h;
}

static void MakeTextureSrv(App& app, ID3D12Resource* tex, DXGI_FORMAT fmt, UINT index)
{
    if (!tex) return;

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    app.device->CreateShaderResourceView(tex, &sv, CpuDesc(app, index));
}

// Packed RGB -> RGBA rows with the given pitch.
static void PackRgba(const std::vector<unsigned char>& rgb, int w, int h, unsigned char* dst, UINT rowPitch)
{
    if (!dst) return;
    if (rgb.size() != (size_t)w * h * 3) return;

    for (int y = 0; y < h; y++)
    {
        unsigned char* row = dst + (size_t)y * rowPitch;
        const unsigned char* s = rgb.data() + (size_t)y * w * 3;
        for (int x = 0; x < w; x++)
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

    auto widen = [](const char* s) { std::string p(s); return std::wstring(p.begin(), p.end()); };
    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        if (!a || !a[0]) continue;
        if (!strcmp(a, "--capture")) { opt->source = SourceKind::Capture; continue; }
        if (!strncmp(a, "--monitor=", 10)) { opt->source = SourceKind::Capture; opt->monitorIndex = atoi(a + 10); continue; }
        if (!strncmp(a, "--window=", 9)) { opt->source = SourceKind::Capture; opt->windowTitle = widen(a + 9); continue; }
        if (!strncmp(a, "--image=", 8)) { opt->source = SourceKind::Image; opt->imagePath = widen(a + 8); continue; }
        if (!strcmp(a, "--synthetic")) { opt->source = SourceKind::Synthetic; continue; }
        if (!strcmp(a, "--no-warp")) { opt->doWarp = false; continue; }
        if (!strncmp(a, "--scale=", 8)) { opt->warpScale = (float)atof(a + 8); continue; }
        if (!strcmp(a, "--truth")) { opt->useTruth = true; continue; }
        if (!strcmp(a, "--dump")) { opt->doDump = true; continue; }
        if (!strcmp(a, "--paired")) { opt->paired = true; continue; }
        if (!strcmp(a, "--selftest")) { opt->selfTestOnly = true; continue; }
        if (!strcmp(a, "--debug")) { opt->debugLayer = true; continue; }
        if (!strcmp(a, "--no-smooth")) { opt->smooth = false; continue; }
        if (!strncmp(a, "--tau=", 6)) { opt->smoothTau = atof(a + 6); continue; }
        if (!strcmp(a, "--submit-depth")) { opt->submitDepth = true; continue; }
        if (!strcmp(a, "--freeze-pose")) { opt->freezePose = true; continue; }
        if (!strcmp(a, "--depth-lie")) { opt->depthLie = true; continue; }
        if (!strncmp(a, "--ab=", 5)) { opt->abSeconds = atof(a + 5); continue; }
        if (a[0] == '-') { Log("ParseArgs: unknown option %s", a); return false; }
        opt->runSeconds = atof(a);
    }
    if (opt->abSeconds < 0.0 || opt->smoothTau <= 0.0) { Log("ParseArgs: --ab must be >= 0 and --tau > 0"); return false; }
    if ((opt->abSeconds > 0.0 || opt->depthLie) && !opt->submitDepth) { Log("ParseArgs: --ab / --depth-lie imply --submit-depth"); opt->submitDepth = true; }
    if (opt->useTruth && opt->source != SourceKind::Synthetic) { Log("ParseArgs: --truth only applies to --synthetic, ignored"); opt->useTruth = false; }

    Log("ParseArgs: source %s (monitor %d, window '%ls', image '%ls') seconds %.0f",
        opt->source == SourceKind::Capture ? "capture" : opt->source == SourceKind::Image ? "image" : "synthetic",
        opt->monitorIndex, opt->windowTitle.c_str(), opt->imagePath.c_str(), opt->runSeconds);
    Log("ParseArgs: warp %d scale %.2f truth %d paired %d smooth %d tau %.2fs submitDepth %d selftest %d debug %d",
        (int)opt->doWarp, opt->warpScale, (int)opt->useTruth, (int)opt->paired, (int)opt->smooth, opt->smoothTau,
        (int)opt->submitDepth, (int)opt->selfTestOnly, (int)opt->debugLayer);
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
    strcpy_s(ci.applicationInfo.applicationName, "VRX xrapp5");
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
    if (FAILED(fac->EnumAdapterByLuid(app.adapterLuid, IID_PPV_ARGS(&app.adapter)))) { Log("InitD3D: FAIL EnumAdapterByLuid"); return false; }
    DXGI_ADAPTER_DESC1 ad{};
    app.adapter->GetDesc1(&ad);
    Log("InitD3D: adapter %ls", ad.Description);

    if (FAILED(D3D12CreateDevice(app.adapter.Get(), app.minFeatureLevel, IID_PPV_ARGS(&app.device)))) { Log("InitD3D: FAIL D3D12CreateDevice"); return false; }

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

    if (FAILED(app.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&app.mlCmdAlloc)))) { Log("InitD3D: FAIL ml allocator"); return false; }
    if (FAILED(app.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, app.mlCmdAlloc.Get(), nullptr, IID_PPV_ARGS(&app.mlCmdList))))
    { Log("InitD3D: FAIL ml command list"); return false; }
    app.mlCmdList->Close();

    if (FAILED(app.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&app.fence)))) { Log("InitD3D: FAIL fence"); return false; }
    if (FAILED(app.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&app.mlFence)))) { Log("InitD3D: FAIL ml fence"); return false; }
    app.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    app.mlFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!app.fenceEvent || !app.mlFenceEvent) { Log("InitD3D: FAIL fence events"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = DESC_COUNT;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(app.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&app.descHeap)))) { Log("InitD3D: FAIL descriptor heap"); return false; }
    app.descInc = app.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    Log("InitD3D: exit ok (one device, gfx + ml queues, %d-frame ring, %u descriptors)", RING, DESC_COUNT);
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
        if (m->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
        Log("  d3d12[%d] %s", (int)m->Severity, m->pDescription ? m->pDescription : "(null)");
        shown++;
    }
    Log("DumpDebugMessages: %llu warning-or-worse shown", (unsigned long long)shown);
}

// ---------------------------------------------------------------- capture
struct MonitorEnum { std::vector<HMONITOR> list; };

static BOOL CALLBACK MonitorEnumProc(HMONITOR mon, HDC, LPRECT, LPARAM lp)
{
    MonitorEnum* me = (MonitorEnum*)lp;
    if (!me) return FALSE;
    me->list.push_back(mon);
    return TRUE;
}

struct WindowFind { std::wstring needle; HWND found = nullptr; std::wstring title; };

static BOOL CALLBACK WindowEnumProc(HWND hwnd, LPARAM lp)
{
    WindowFind* wf = (WindowFind*)lp;
    if (!wf) return FALSE;
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t title[512] = {};
    if (GetWindowTextW(hwnd, title, 511) <= 0) return TRUE;
    std::wstring t(title), lt(t), ln(wf->needle);
    for (auto& c : lt) c = (wchar_t)towlower(c);
    for (auto& c : ln) c = (wchar_t)towlower(c);
    if (lt.find(ln) == std::wstring::npos) return TRUE;

    wf->found = hwnd;
    wf->title = t;
    return FALSE;
}

// Creates the capture item + D3D11 side, and sets app.srcW/srcH/srcFormat.
static bool InitCaptureItem(App& app)
{
    Log("InitCaptureItem: enter");
    Capture& c = app.cap;

    if (!wgc::GraphicsCaptureSession::IsSupported()) { Log("InitCaptureItem: FAIL Windows.Graphics.Capture not supported on this OS"); return false; }

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    HRESULT hr = E_FAIL;
    if (!app.opt.windowTitle.empty())
    {
        WindowFind wf;
        wf.needle = app.opt.windowTitle;
        EnumWindows(WindowEnumProc, (LPARAM)&wf);
        if (!wf.found) { Log("InitCaptureItem: FAIL no visible window with '%ls' in its title", app.opt.windowTitle.c_str()); return false; }
        Log("InitCaptureItem: window '%ls'", wf.title.c_str());
        hr = interop->CreateForWindow(wf.found, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(c.item));
    }
    else
    {
        HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        if (app.opt.monitorIndex >= 0)
        {
            MonitorEnum me;
            EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, (LPARAM)&me);
            if (app.opt.monitorIndex >= (int)me.list.size()) { Log("InitCaptureItem: FAIL monitor %d of %zu", app.opt.monitorIndex, me.list.size()); return false; }
            mon = me.list[app.opt.monitorIndex];
        }
        Log("InitCaptureItem: monitor %d%s", app.opt.monitorIndex, app.opt.monitorIndex < 0 ? " (primary)" : "");
        hr = interop->CreateForMonitor(mon, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(c.item));
    }
    if (FAILED(hr) || !c.item) { Log("InitCaptureItem: FAIL create capture item 0x%08X", (unsigned)hr); return false; }

    auto size = c.item.Size();
    if (size.Width <= 0 || size.Height <= 0) { Log("InitCaptureItem: FAIL item size %dx%d", size.Width, size.Height); return false; }
    app.srcW = size.Width;
    app.srcH = size.Height;
    app.srcFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    // D3D11 device on the SAME adapter (shared handles do not cross adapters)
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    hr = D3D11CreateDevice(app.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &fl, 1, D3D11_SDK_VERSION, &c.dev, nullptr, &c.ctx);
    if (FAILED(hr)) { Log("InitCaptureItem: FAIL D3D11CreateDevice 0x%08X", (unsigned)hr); return false; }
    if (FAILED(c.ctx.As(&c.ctx4))) { Log("InitCaptureItem: FAIL ID3D11DeviceContext4 (fence support)"); return false; }

    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(c.dev.As(&dxgiDev))) { Log("InitCaptureItem: FAIL IDXGIDevice"); return false; }
    winrt::com_ptr<::IInspectable> insp;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.Get(), insp.put());
    if (FAILED(hr)) { Log("InitCaptureItem: FAIL CreateDirect3D11DeviceFromDXGIDevice 0x%08X", (unsigned)hr); return false; }
    c.rtDevice = insp.as<wdx::Direct3D11::IDirect3DDevice>();

    Log("InitCaptureItem: exit ok, %dx%d", app.srcW, app.srcH);
    return true;
}

// Opens the D3D12 source ring + fence on the D3D11 device and starts the session.
static bool StartCapture(App& app)
{
    Log("StartCapture: enter");
    Capture& c = app.cap;

    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11Device5> dev5;
    if (FAILED(c.dev.As(&dev1)) || FAILED(c.dev.As(&dev5))) { Log("StartCapture: FAIL ID3D11Device1/5"); return false; }

    for (int i = 0; i < SRC_RING; i++)
    {
        HANDLE h = nullptr;
        HRESULT hr = app.device->CreateSharedHandle(app.srcTex[i].Get(), nullptr, GENERIC_ALL, nullptr, &h);
        if (FAILED(hr)) { Log("StartCapture: FAIL CreateSharedHandle(tex %d) 0x%08X", i, (unsigned)hr); return false; }
        hr = dev1->OpenSharedResource1(h, IID_PPV_ARGS(&c.tex11[i]));
        CloseHandle(h);
        if (FAILED(hr)) { Log("StartCapture: FAIL OpenSharedResource1(tex %d) 0x%08X", i, (unsigned)hr); return false; }
    }

    if (FAILED(app.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&app.captureFence)))) { Log("StartCapture: FAIL shared fence"); return false; }
    HANDLE fh = nullptr;
    HRESULT hr = app.device->CreateSharedHandle(app.captureFence.Get(), nullptr, GENERIC_ALL, nullptr, &fh);
    if (FAILED(hr)) { Log("StartCapture: FAIL CreateSharedHandle(fence) 0x%08X", (unsigned)hr); return false; }
    hr = dev5->OpenSharedFence(fh, IID_PPV_ARGS(&c.fence11));
    CloseHandle(fh);
    if (FAILED(hr)) { Log("StartCapture: FAIL OpenSharedFence 0x%08X", (unsigned)hr); return false; }

    winrt::Windows::Graphics::SizeInt32 size{ app.srcW, app.srcH };
    c.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(c.rtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    c.session = c.pool.CreateCaptureSession(c.item);
    try { c.session.IsBorderRequired(false); }
    catch (const winrt::hresult_error& e) { Log("StartCapture: capture border cannot be disabled (0x%08X) - continuing", (unsigned)e.code().value); }
    c.session.StartCapture();

    Log("StartCapture: exit ok");
    return true;
}

static void CaptureMain(App* app)
{
    g_threadName = "capture";
    if (!app) { Log("CaptureMain: null app"); return; }
    Log("CaptureMain: enter");

    Capture& c = app->cap;
    int next = 0;
    int poolW = app->srcW, poolH = app->srcH;
    while (!app->stop.load())
    {
        wgc::Direct3D11CaptureFrame frame{ nullptr };
        try { frame = c.pool.TryGetNextFrame(); }
        catch (const winrt::hresult_error& e) { Log("CaptureMain: TryGetNextFrame failed 0x%08X", (unsigned)e.code().value); break; }
        if (!frame) { Sleep(1); continue; }

        auto cs = frame.ContentSize();
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex))) || !tex) { Log("CaptureMain: frame has no texture"); continue; }

        // The ring keeps its initial size; a resized window is copied top-left.
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        D3D11_BOX box{ 0, 0, 0, (UINT)std::min<int>({ (int)td.Width, cs.Width, app->srcW }), (UINT)std::min<int>({ (int)td.Height, cs.Height, app->srcH }), 1 };
        c.ctx->CopySubresourceRegion(c.tex11[next].Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
        UINT64 v = ++app->captureFenceVal;
        c.ctx4->Signal(c.fence11.Get(), v);
        c.ctx->Flush();

        {
            std::lock_guard<std::mutex> lock(app->pubMutex);
            app->pub.index = next;
            app->pub.fence = app->captureFence.Get();
            app->pub.value = v;
            app->pub.time = NowSeconds();
            app->pub.seq++;
        }
        c.frames++;
        next = (next + 1) % SRC_RING;

        if (cs.Width != poolW || cs.Height != poolH)
        {
            Log("CaptureMain: content size changed %dx%d -> %dx%d, recreating frame pool", poolW, poolH, cs.Width, cs.Height);
            poolW = cs.Width; poolH = cs.Height;
            frame = nullptr;
            c.pool.Recreate(c.rtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, cs);
        }
    }

    try { if (c.session) c.session.Close(); if (c.pool) c.pool.Close(); }
    catch (const winrt::hresult_error&) {}
    Log("CaptureMain: exit after %llu frames", (unsigned long long)c.frames.load());
}

// ----------------------------------------------------------------- source
// Decides the source size, creates the source ring and its SRVs.
static bool InitSource(App& app)
{
    Log("InitSource: enter");

    if (app.opt.source == SourceKind::Capture)
    {
        if (!InitCaptureItem(app)) return false;
    }
    else if (app.opt.source == SourceKind::Image)
    {
        if (!LoadImageWICSized(app.opt.imagePath.c_str(), 0, 0, MAX_COLOR_W, &app.srcW, &app.srcH, app.imageRgb))
        { Log("InitSource: FAIL cannot load image %ls", app.opt.imagePath.c_str()); return false; }
        app.srcFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    else
    {
        app.srcW = W; app.srcH = H;
        app.srcFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    // presented size: the source, shrunk to MAX_COLOR_W wide, even dimensions
    double k = app.srcW > MAX_COLOR_W ? (double)MAX_COLOR_W / app.srcW : 1.0;
    app.colorW = std::max(2, (int)(app.srcW * k + 0.5) & ~1);
    app.colorH = std::max(2, (int)(app.srcH * k + 0.5) & ~1);

    const bool shared = app.opt.source == SourceKind::Capture;
    for (int i = 0; i < SRC_RING; i++)
    {
        if (!MakeSourceTexture(app, app.srcW, app.srcH, app.srcFormat, shared, app.srcTex[i])) return false;
        MakeTextureSrv(app, app.srcTex[i].Get(), app.srcFormat, DESC_SRC0 + i);
    }

    if (!shared)
    {
        D3D12_RESOURCE_DESC td = app.srcTex[0]->GetDesc();
        UINT64 total = 0;
        app.device->GetCopyableFootprints(&td, 0, 1, 0, &app.srcFootprint, nullptr, nullptr, &total);
        for (int i = 0; i < RING; i++)
        {
            if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_GENERIC_READ, app.srcUp[i], (void**)&app.srcUpMapped[i])) return false;
        }
    }
    else if (!StartCapture(app)) return false;

    Log("InitSource: exit ok, source %dx%d (format %d) presented at %dx%d, depth at %dx%d",
        app.srcW, app.srcH, (int)app.srcFormat, app.colorW, app.colorH, W, H);
    return true;
}

// CPU picture -> the next source-ring texture, on the gfx queue. Records only;
// the caller submits, then calls PublishSource with the resulting fence value.
static int RecordCpuSource(App& app, const std::vector<unsigned char>& rgb, int ringSlot)
{
    if (ringSlot < 0 || ringSlot >= RING) return -1;
    if (rgb.size() != (size_t)app.srcW * app.srcH * 3) { Log("RecordCpuSource: bad picture size %zu", rgb.size()); return -1; }

    PackRgba(rgb, app.srcW, app.srcH, app.srcUpMapped[ringSlot] + app.srcFootprint.Offset, app.srcFootprint.Footprint.RowPitch);
    int k = app.srcNext;
    app.srcNext = (app.srcNext + 1) % SRC_RING;

    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = app.srcTex[k].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = app.srcUp[ringSlot].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = app.srcFootprint;
    app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);   // simultaneous-access: no barriers
    return k;
}

static void PublishSource(App& app, int index, ID3D12Fence* fence, UINT64 value, double time)
{
    if (index < 0 || !fence) return;

    std::lock_guard<std::mutex> lock(app.pubMutex);
    app.pub.index = index;
    app.pub.fence = fence;
    app.pub.value = value;
    app.pub.time = time;
    app.pub.seq++;
}

static SourcePub LatestSource(App& app)
{
    std::lock_guard<std::mutex> lock(app.pubMutex);
    return app.pub;
}

// ---------------------------------------------------------------- shaders
// Row-parallel forward warp at COLOUR resolution; depth (DW x DH) is sampled
// bilinearly. One GPU thread owns one whole row of one eye, so the scatter + z-test
// + hole fill are race-free and identical to WarpEye (CPU) when the sizes match.
static const char* kWarpHlsl = R"HLSL(
cbuffer C : register(b0)
{
    uint  CW; uint CH; uint DW; uint DH;
    uint  doWarp;
    float scaleFocal;   // warpScale * focalPx (colour pixels)
    float invZNear;
    float invZFar;
    float eye0;         // eye lateral offsets in metres (left negative)
    float eye1;
    float nearZ;
    float farZ;
    uint  indicator;    // 0 none, 1 green, 2 red
    uint  depthLie;
    uint  writeDepth;
    uint  exactLoad;    // source is exactly CW x CH: Load() instead of filtering
};

Texture2D<float4>        scene    : register(t0);
StructuredBuffer<float>  nearBuf  : register(t1);   // DW x DH, 0 = far .. 1 = near
RWTexture2DArray<float4> outColor : register(u0);
RWTexture2DArray<float>  outDepth : register(u1);   // D3D projective depth for nearZ/farZ
RWStructuredBuffer<uint> scratch  : register(u2);   // [0,N): source x per dest, [N,2N): its nearness; N = CW*CH*2
SamplerState             samp     : register(s0);

float NearAt(int x, int y)
{
    float fx = ((float)x + 0.5) * (float)DW / (float)CW - 0.5;
    float fy = ((float)y + 0.5) * (float)DH / (float)CH - 0.5;
    float x0f = floor(fx), y0f = floor(fy);
    float tx = fx - x0f, ty = fy - y0f;
    int x0 = clamp((int)x0f, 0, (int)DW - 1), x1 = clamp((int)x0f + 1, 0, (int)DW - 1);
    int y0 = clamp((int)y0f, 0, (int)DH - 1), y1 = clamp((int)y0f + 1, 0, (int)DH - 1);
    float a = nearBuf[y0 * DW + x0], b = nearBuf[y0 * DW + x1];
    float c = nearBuf[y1 * DW + x0], d = nearBuf[y1 * DW + x1];
    float top = a + tx * (b - a);
    float bot = c + tx * (d - c);
    return top + ty * (bot - top);
}

[numthreads(1, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint y = id.y;
    uint e = id.z;
    if (y >= CH) return;

    float eye = (e == 0) ? eye0 : eye1;
    int iw = (int)CW;
    uint N = CW * CH * 2;
    uint base = (e * CH + y) * CW;
    const uint NONE = 0xFFFFFFFF;

    int x;
    [loop] for (x = 0; x < iw; x++) scratch[base + x] = NONE;

    // scatter: every source pixel moves by its own disparity; nearer wins
    [loop] for (x = 0; x < iw; x++)
    {
        float n = NearAt(x, (int)y);
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
            uint cur = scratch[base + dx];
            if (cur == NONE || n > asfloat(scratch[N + base + dx]))
            {
                scratch[base + dx] = (uint)x;
                scratch[N + base + dx] = asuint(n);
            }
        }
    }

    // hole fill from the FARTHER of the nearest valid neighbours (background)
    int lastValid = -1;
    float lastNear = 0;
    x = 0;
    [loop] while (x < iw)
    {
        uint sv = scratch[base + x];
        if (sv != NONE) { lastValid = (int)sv; lastNear = asfloat(scratch[N + base + x]); x++; continue; }

        int r = x + 1;
        [loop] while (r < iw && scratch[base + r] == NONE) r++;
        int rightValid = -1;
        float rightNear = 0;
        if (r < iw) { rightValid = (int)scratch[base + r]; rightNear = asfloat(scratch[N + base + r]); }

        int pick = lastValid;
        float pickNear = lastNear;
        if (pick < 0 || (rightValid >= 0 && rightNear < pickNear)) { pick = rightValid; pickNear = rightNear; }
        if (pick < 0) { pick = x; pickNear = NearAt(x, (int)y); }
        [loop] for (int h = x; h < r; h++)
        {
            scratch[base + h] = (uint)pick;
            scratch[N + base + h] = asuint(pickNear);
        }
        x = r;      // lastValid/lastNear deliberately unchanged: fills are not sources
    }

    [loop] for (x = 0; x < iw; x++)
    {
        int s = (int)scratch[base + x];
        float4 col;
        if (exactLoad != 0) col = scene.Load(int3(s, y, 0));
        else col = scene.SampleLevel(samp, float2(((float)s + 0.5) / (float)CW, ((float)y + 0.5) / (float)CH), 0);
        col.a = 1.0;

        if (indicator != 0 && y >= CH / 49 && y < CH / 12 && x >= iw / 2 - iw / 17 && x < iw / 2 + iw / 17)
            col = (indicator == 1) ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
        outColor[uint3(x, y, e)] = col;

        if (writeDepth != 0)
        {
            float invZ = invZFar + asfloat(scratch[N + base + x]) * (invZNear - invZFar);
            if (depthLie != 0) invZ = (x < iw / 2) ? (1.0 / 0.5) : (1.0 / 10.0);
            outDepth[uint3(x, y, e)] = (farZ / (farZ - nearZ)) * (1.0 - nearZ * invZ);
        }
    }
}
)HLSL";

// Source texture (any size, RGBA or BGRA view) -> the model's input tensor:
// NCHW float, ImageNet-normalised, box-filtered down with taps x taps bilinear taps.
static const char* kPrepHlsl = R"HLSL(
cbuffer C : register(b0)
{
    uint DW; uint DH; uint taps; uint exactLoad;
};

Texture2D<float4>         scene   : register(t0);
RWStructuredBuffer<float> modelIn : register(u0);
SamplerState              samp    : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DW || id.y >= DH) return;

    float3 v = float3(0, 0, 0);
    if (exactLoad != 0)
    {
        v = scene.Load(int3(id.xy, 0)).rgb;
    }
    else
    {
        [loop] for (uint j = 0; j < taps; j++)
        {
            [loop] for (uint i = 0; i < taps; i++)
            {
                float2 uv = float2(((float)id.x + ((float)i + 0.5) / (float)taps) / (float)DW,
                                   ((float)id.y + ((float)j + 0.5) / (float)taps) / (float)DH);
                v += scene.SampleLevel(samp, uv, 0).rgb;
            }
        }
        v /= (float)(taps * taps);
    }

    const float3 mean = float3(0.485, 0.456, 0.406);
    const float3 istd = float3(1.0 / 0.229, 1.0 / 0.224, 1.0 / 0.225);
    float3 o = (v - mean) * istd;
    uint plane = DW * DH;
    uint p = id.y * DW + id.x;
    modelIn[p] = o.r;
    modelIn[plane + p] = o.g;
    modelIn[2 * plane + p] = o.b;
}
)HLSL";

static bool CompileCs(const char* name, const char* src, ComPtr<ID3DBlob>& out)
{
    if (!name || !src) return false;

    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
    if (err && err->GetBufferSize() > 1) Log("CompileCs[%s]: compiler says: %s", name, (const char*)err->GetBufferPointer());
    if (FAILED(hr) || !out) { Log("CompileCs[%s]: FAIL 0x%08X", name, (unsigned)hr); return false; }
    return true;
}

static bool MakeRootSig(App& app, UINT numConstants, bool uavTable, ComPtr<ID3D12RootSignature>& out)
{
    D3D12_DESCRIPTOR_RANGE srcRange{};
    srcRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srcRange.NumDescriptors = 1; srcRange.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE tbl[2]{};
    tbl[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tbl[0].NumDescriptors = 1; tbl[0].BaseShaderRegister = 1; tbl[0].OffsetInDescriptorsFromTableStart = 0;
    tbl[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    tbl[1].NumDescriptors = 3; tbl[1].BaseShaderRegister = 0; tbl[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = numConstants;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srcRange;
    if (uavTable)
    {
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 2;
        params[2].DescriptorTable.pDescriptorRanges = tbl;
    }
    else
    {
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;     // the model input buffer, as a root UAV
        params[2].Descriptor.ShaderRegister = 0;
    }

    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &ss;
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { Log("MakeRootSig: FAIL serialize: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    hr = app.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
    if (FAILED(hr)) { Log("MakeRootSig: FAIL CreateRootSignature 0x%08X", (unsigned)hr); return false; }
    return true;
}

static bool MakeWarpTarget(App& app, int cw, int ch, UINT tableIndex, WarpTarget& t)
{
    Log("MakeWarpTarget: enter %dx%d table %u", cw, ch, tableIndex);
    ID3D12Device* dev = app.device.Get();
    t.cw = cw; t.ch = ch; t.tableIndex = tableIndex;

    if (!MakeTexture(dev, cw, ch, DXGI_FORMAT_R8G8B8A8_TYPELESS, VIEWS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, t.colorOut)) return false;
    if (!MakeTexture(dev, cw, ch, DXGI_FORMAT_R32_TYPELESS, VIEWS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, t.depthOut)) return false;
    const UINT scratchElems = (UINT)cw * (UINT)ch * VIEWS * 2;
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, (UINT64)scratchElems * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, t.scratch, nullptr)) return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC bv{};
    bv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bv.Format = DXGI_FORMAT_UNKNOWN;
    bv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    bv.Buffer.NumElements = (UINT)(W * H);
    bv.Buffer.StructureByteStride = sizeof(float);
    dev->CreateShaderResourceView(app.nearBuf.Get(), &bv, CpuDesc(app, tableIndex + 0));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uv.Texture2DArray.ArraySize = VIEWS;
    dev->CreateUnorderedAccessView(t.colorOut.Get(), nullptr, &uv, CpuDesc(app, tableIndex + 1));
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    dev->CreateUnorderedAccessView(t.depthOut.Get(), nullptr, &uv, CpuDesc(app, tableIndex + 2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    sv.Buffer.NumElements = scratchElems;
    sv.Buffer.StructureByteStride = 4;
    dev->CreateUnorderedAccessView(t.scratch.Get(), nullptr, &sv, CpuDesc(app, tableIndex + 3));

    Log("MakeWarpTarget: exit ok");
    return true;
}

static bool InitShaders(App& app)
{
    Log("InitShaders: enter");

    ComPtr<ID3DBlob> warpCs, prepCs;
    if (!CompileCs("warp.hlsl", kWarpHlsl, warpCs)) return false;
    if (!CompileCs("prep.hlsl", kPrepHlsl, prepCs)) return false;
    if (!MakeRootSig(app, sizeof(WarpConstants) / 4, true, app.warpRootSig)) return false;
    if (!MakeRootSig(app, sizeof(PrepConstants) / 4, false, app.prepRootSig)) return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = app.warpRootSig.Get();
    pd.CS = { warpCs->GetBufferPointer(), warpCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.warpPso)))) { Log("InitShaders: FAIL warp PSO"); return false; }
    pd.pRootSignature = app.prepRootSig.Get();
    pd.CS = { prepCs->GetBufferPointer(), prepCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.prepPso)))) { Log("InitShaders: FAIL prep PSO"); return false; }

    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_DEFAULT, (UINT64)W * H * sizeof(float), D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, app.nearBuf, nullptr)) return false;
    if (!MakeWarpTarget(app, app.colorW, app.colorH, DESC_MAIN_TABLE, app.mainTarget)) return false;
    if (!MakeWarpTarget(app, W, H, DESC_TEST_TABLE, app.testTarget)) return false;
    if (!MakeSourceTexture(app, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false, app.testSrc)) return false;
    MakeTextureSrv(app, app.testSrc.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DESC_TEST_SRC);

    Log("InitShaders: exit ok (warp %zu bytes, prep %zu bytes)", warpCs->GetBufferSize(), prepCs->GetBufferSize());
    return true;
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
static void RecordWarp(App& app, WarpTarget& t, UINT srcDescIndex, const WarpConstants& c)
{
    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    ID3D12DescriptorHeap* heaps[] = { app.descHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(app.warpRootSig.Get());
    cl->SetPipelineState(app.warpPso.Get());
    cl->SetComputeRoot32BitConstants(0, sizeof(WarpConstants) / 4, &c, 0);
    cl->SetComputeRootDescriptorTable(1, GpuDesc(app, srcDescIndex));
    cl->SetComputeRootDescriptorTable(2, GpuDesc(app, t.tableIndex));
    cl->Dispatch(1, ((UINT)t.ch + 7) / 8, VIEWS);
    Transition(cl, t.colorOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cl, t.depthOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
}

static void RecordWarpOutputsBackToUav(App& app, WarpTarget& t)
{
    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    Transition(cl, t.colorOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cl, t.depthOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// warp outputs -> the acquired swapchain images. Spec (XR_KHR_D3D12_enable):
// acquired colour images are in RENDER_TARGET, depth images in DEPTH_WRITE, and
// must be released in the same state. depthImg may be null (depth not submitted).
static void RecordCopyToSwapchain(App& app, WarpTarget& t, ID3D12Resource* colorImg, ID3D12Resource* depthImg)
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

        dst.pResource = colorImg; src.pResource = t.colorOut.Get();
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (!depthImg) continue;
        dst.pResource = depthImg; src.pResource = t.depthOut.Get();
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Transition(cl, colorImg, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (depthImg) Transition(cl, depthImg, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE);
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

    ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "xrapp5", &app.env);
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

    // Model input: a VRAM buffer the prep shader writes and DirectML reads - the
    // picture never visits the CPU. (M1: a resource handed to DML needs UAV access.)
    const UINT64 modelBytes = (UINT64)3 * H * W * sizeof(float);
    int64_t modelDims[4] = { 1, 3, H, W };
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_DEFAULT, modelBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.modelIn, nullptr)) return false;

    void* dmlAlloc = nullptr;
    if (OrtStatus* st = dmlApi->CreateGPUAllocationFromD3DResource(app.modelIn.Get(), &dmlAlloc)) { Fail("CreateGPUAllocationFromD3DResource", st); return false; }
    if (OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(const_cast<OrtMemoryInfo*>(inInfos[0]), dmlAlloc, modelBytes,
                                                            modelDims, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &app.modelInValue))
    { Fail("CreateTensorWithDataAsOrtValue", st); return false; }

    app.rawDepth.resize((size_t)W * H);
    Log("InitModel: exit ok, %dx%d on the shared device via the ml queue (GPU-resident input)", H, W);
    return true;
}

// Records + submits the prep shader on the ml queue: source texture -> model input.
// GPU-side ordering only: the queue waits for the source's fence, and ORT's own
// submissions to the same queue follow it. Returns the ml fence value signalled
// after the dispatch (callers other than the self-test need not wait on it).
static UINT64 SubmitPrep(App& app, UINT srcDescIndex, int srcW, int srcH, ID3D12Fence* srcFence, UINT64 srcValue)
{
    if (srcW <= 0 || srcH <= 0) return 0;

    PrepConstants pc{};
    pc.dw = W; pc.dh = H;
    pc.exactLoad = (srcW == W && srcH == H) ? 1u : 0u;
    pc.taps = (uint32_t)std::min(4, std::max(1, (srcW + W - 1) / W));

    app.mlCmdAlloc->Reset();
    app.mlCmdList->Reset(app.mlCmdAlloc.Get(), nullptr);
    ID3D12GraphicsCommandList* cl = app.mlCmdList.Get();
    ID3D12DescriptorHeap* heaps[] = { app.descHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(app.prepRootSig.Get());
    cl->SetPipelineState(app.prepPso.Get());
    cl->SetComputeRoot32BitConstants(0, sizeof(PrepConstants) / 4, &pc, 0);
    cl->SetComputeRootDescriptorTable(1, GpuDesc(app, srcDescIndex));
    cl->SetComputeRootUnorderedAccessView(2, app.modelIn->GetGPUVirtualAddress());
    cl->Dispatch((W + 7) / 8, (H + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = app.modelIn.Get();
    cl->ResourceBarrier(1, &uav);
    cl->Close();

    if (srcFence) app.mlQueue->Wait(srcFence, srcValue);
    ID3D12CommandList* lists[] = { cl };
    app.mlQueue->ExecuteCommandLists(1, lists);
    app.mlQueue->Signal(app.mlFence.Get(), ++app.mlFenceVal);
    return app.mlFenceVal;
}

static void WaitMlFence(App& app, UINT64 value)
{
    if (value == 0) return;
    if (app.mlFence->GetCompletedValue() >= value) return;

    app.mlFence->SetEventOnCompletion(value, app.mlFenceEvent);
    WaitForSingleObject(app.mlFenceEvent, INFINITE);
}

// Runs the model on whatever the prep shader put in the input buffer.
static bool RunModelRaw(App& app)
{
    if (!app.ortSession || !app.modelInValue) return false;

    OrtValue* out = nullptr;
    const char* inName = "pixel_values";
    const char* outName = "predicted_depth";
    if (OrtStatus* st = ort->Run(app.ortSession, nullptr, &inName, (const OrtValue* const*)&app.modelInValue, 1, &outName, 1, &out))
    { Fail("Run", st); return false; }

    // The output is a CPU tensor, so ORT has already synchronised with the GPU.
    float* dp = nullptr;
    ort->GetTensorMutableData(out, (void**)&dp);
    if (!dp) { ort->ReleaseValue(out); Log("RunModelRaw: null output"); return false; }
    memcpy(app.rawDepth.data(), dp, app.rawDepth.size() * sizeof(float));
    ort->ReleaseValue(out);
    return true;
}

// Robust, time-smoothed normalisation range for the model's relative output.
//  * 0.5 / 99.5 percentiles instead of min/max: one hot pixel cannot rescale the
//    whole picture's depth.
//  * exponential smoothing with time constant tau: the range drifts instead of
//    jumping frame to frame, which is what makes depth "breathe" on video.
//  * a large jump (scene cut) snaps immediately rather than gliding through it.
static void SmoothRange(RangeSmoother& rs, const std::vector<float>& raw, double now, bool smooth, double tau, float* lo, float* hi)
{
    if (!lo || !hi) return;
    if (raw.empty()) { *lo = 0; *hi = 1; return; }

    float dmin = raw[0], dmax = raw[0];
    for (float v : raw) { dmin = std::min(dmin, v); dmax = std::max(dmax, v); }
    if (!smooth || (dmax - dmin) < 1e-6f) { *lo = dmin; *hi = dmax; rs.have = false; return; }

    const int BINS = 1024;
    uint32_t hist[BINS] = {};
    const float k = (float)(BINS - 1) / (dmax - dmin);
    for (float v : raw) hist[(int)((v - dmin) * k)]++;

    const size_t loCount = (size_t)(raw.size() * 0.005), hiCount = (size_t)(raw.size() * 0.995);
    size_t acc = 0;
    int loBin = 0, hiBin = BINS - 1;
    bool gotLo = false;
    for (int i = 0; i < BINS; i++)
    {
        acc += hist[i];
        if (!gotLo && acc > loCount) { loBin = i; gotLo = true; }
        if (acc >= hiCount) { hiBin = i; break; }
    }
    float newLo = dmin + loBin / k, newHi = dmin + (hiBin + 1) / k;
    if (newHi - newLo < 1e-6f) { newLo = dmin; newHi = dmax; }

    const float range = rs.hi - rs.lo;
    const bool cut = !rs.have || (fabsf(newLo - rs.lo) + fabsf(newHi - rs.hi)) > 0.6f * range;
    if (cut)
    {
        rs.lo = newLo; rs.hi = newHi; rs.have = true;
    }
    else
    {
        float a = 1.0f - expf(-(float)((now - rs.lastTime) / tau));
        a = std::min(1.0f, std::max(0.0f, a));
        rs.lo += a * (newLo - rs.lo);
        rs.hi += a * (newHi - rs.hi);
    }
    rs.lastTime = now;
    *lo = rs.lo; *hi = rs.hi;
}

// ------------------------------------------------------------- depth handoff
static bool InitSlots(App& app)
{
    Log("InitSlots: enter");
    for (int i = 0; i < SLOTS; i++)
    {
        DepthSlot& s = app.slots[i];
        if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_UPLOAD, (UINT64)W * H * sizeof(float), D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_GENERIC_READ, s.nearUp, (void**)&s.nearMapped)) return false;
    }
    Log("InitSlots: exit ok, %d slots", SLOTS);
    return true;
}

// Source frame -> prep -> model -> normalise -> publish. Called by the main thread
// once before the worker starts, then only by the worker.
static bool ComputeAndPublish(App& app, const SourcePub& src, std::vector<float>& nearScratch)
{
    if (src.index < 0) return false;

    auto m0 = std::chrono::steady_clock::now();
    SubmitPrep(app, DESC_SRC0 + (UINT)src.index, app.srcW, app.srcH, src.fence, src.value);
    if (!RunModelRaw(app)) return false;
    double modelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m0).count();

    float lo = 0, hi = 1;
    SmoothRange(app.smoother, app.rawDepth, NowSeconds(), app.opt.smooth, app.opt.smoothTau, &lo, &hi);
    const float inv = (hi - lo) > 1e-6f ? 1.0f / (hi - lo) : 0.0f;
    nearScratch.resize(app.rawDepth.size());
    for (size_t i = 0; i < nearScratch.size(); i++)
        nearScratch[i] = std::min(1.0f, std::max(0.0f, (app.rawDepth[i] - lo) * inv));

    DepthSlot& s = app.slots[app.writeSlot];
    if (app.opt.source == SourceKind::Synthetic) RegionMeans(nearScratch, src.time, s.back, s.panel, s.marker);
    if (app.opt.useTruth) MakeTruth(nearScratch, src.time);

    memcpy(s.nearMapped, nearScratch.data(), nearScratch.size() * sizeof(float));
    s.srcIndex = src.index;
    s.sceneTime = src.time;
    s.modelMs = modelMs;
    s.lo = lo; s.hi = hi;
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

    std::vector<float> nearScratch;
    uint64_t runs = 0, lastSeq = 0;
    while (!app->stop.load())
    {
        SourcePub src = LatestSource(*app);
        // A captured desktop that is not changing delivers no new frames - do not
        // burn the GPU recomputing identical depth. (Image/synthetic keep running:
        // they double as the throughput benchmark.)
        if (app->opt.source == SourceKind::Capture && src.seq == lastSeq) { Sleep(2); continue; }
        lastSeq = src.seq;

        if (!ComputeAndPublish(*app, src, nearScratch)) { Log("WorkerMain: model failed, stopping"); break; }
        runs++;
    }
    Log("WorkerMain: exit after %llu runs", (unsigned long long)runs);
}

// ------------------------------------------------------------- xr session
static bool InitXrSession(App& app)
{
    Log("InitXrSession: enter (swapchain %dx%d)", app.colorW, app.colorH);

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

    int64_t colorFmt = 0, depthFmt = 0;
    for (auto f : formats) if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { colorFmt = f; break; }
    if (!colorFmt) for (auto f : formats) if (f == DXGI_FORMAT_R8G8B8A8_UNORM) { colorFmt = f; break; }
    for (auto f : formats) if (f == DXGI_FORMAT_D32_FLOAT) { depthFmt = f; break; }
    Log("InitXrSession: formats colour %lld depth %lld (of %u offered)", (long long)colorFmt, (long long)depthFmt, fmtCount);
    if (!colorFmt) { Log("InitXrSession: FAIL runtime offers no R8G8B8A8 colour format"); return false; }
    if (!depthFmt && app.opt.submitDepth) { Log("InitXrSession: FAIL runtime offers no D32_FLOAT depth format"); return false; }

    XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sc.sampleCount = 1; sc.width = (uint32_t)app.colorW; sc.height = (uint32_t)app.colorH; sc.faceCount = 1;
    sc.arraySize = VIEWS; sc.mipCount = 1;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = colorFmt;
    r = xrCreateSwapchain_(app.session, &sc, &app.colorSc);
    if (XR_FAILED(r)) { Log("InitXrSession: FAIL colour swapchain %s", XRStr(r)); return false; }

    uint32_t cn = 0;
    xrEnumImages_(app.colorSc, 0, &cn, nullptr);
    app.cimgs.assign(cn, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumImages_(app.colorSc, cn, &cn, (XrSwapchainImageBaseHeader*)app.cimgs.data());
    if (cn == 0 || !app.cimgs[0].texture) { Log("InitXrSession: FAIL no colour swapchain images"); return false; }
    D3D12_RESOURCE_DESC cd = app.cimgs[0].texture->GetDesc();
    Log("InitXrSession: colour images %u (%llux%u, resource format %d, array %u)", cn, (unsigned long long)cd.Width, cd.Height,
        (int)cd.Format, (unsigned)cd.DepthOrArraySize);

    if (!app.opt.submitDepth) { Log("InitXrSession: exit ok (no depth swapchain - SteamVR ignores it, see M3 findings)"); return true; }

    sc.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = depthFmt;
    r = xrCreateSwapchain_(app.session, &sc, &app.depthSc);
    if (XR_FAILED(r)) { Log("InitXrSession: FAIL depth swapchain %s", XRStr(r)); return false; }
    uint32_t dn = 0;
    xrEnumImages_(app.depthSc, 0, &dn, nullptr);
    app.dimgs.assign(dn, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumImages_(app.depthSc, dn, &dn, (XrSwapchainImageBaseHeader*)app.dimgs.data());
    if (dn == 0 || !app.dimgs[0].texture) { Log("InitXrSession: FAIL no depth swapchain images"); return false; }
    Log("InitXrSession: exit ok, depth images %u", dn);
    return true;
}

// --------------------------------------------------------------- self test
// Uploads a WxH picture into the self-test source texture (synchronously).
static bool UploadTestSource(App& app, const std::vector<unsigned char>& rgb)
{
    if (rgb.size() != (size_t)W * H * 3) return false;

    D3D12_RESOURCE_DESC td = app.testSrc->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    app.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> up;
    unsigned char* mapped = nullptr;
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, up, (void**)&mapped)) return false;
    PackRgba(rgb, W, H, mapped + fp.Offset, fp.Footprint.RowPitch);

    WaitFence(app, app.fenceVal);
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = app.testSrc.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    app.cmdList->Close();
    WaitFence(app, SubmitAndSignal(app));
    return true;
}

// GPU warp vs the CPU reference at WxH, pixel for pixel.
static bool SelfTestWarp(App& app, const char* name, const std::vector<unsigned char>& scene,
                         const std::vector<float>& near01, const WarpConstants& c)
{
    if (!name) return false;
    if (scene.size() != (size_t)W * H * 3 || near01.size() != (size_t)W * H) { Log("SelfTestWarp[%s]: bad input sizes", name); return false; }
    Log("SelfTestWarp[%s]: enter (doWarp %u scaleFocal %.2f eyes %.4f/%.4f)", name, c.doWarp, c.scaleFocal, c.eye0, c.eye1);

    if (!UploadTestSource(app, scene)) return false;
    ID3D12Device* dev = app.device.Get();
    WarpTarget& t = app.testTarget;

    ComPtr<ID3D12Resource> nearUp;
    float* nearMapped = nullptr;
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, (UINT64)W * H * sizeof(float), D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nearUp, (void**)&nearMapped)) return false;
    memcpy(nearMapped, near01.data(), near01.size() * sizeof(float));

    D3D12_RESOURCE_DESC cdesc = t.colorOut->GetDesc(), ddesc = t.depthOut->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT cfp[VIEWS], dfp[VIEWS];
    UINT64 cTotal = 0, dTotal = 0;
    dev->GetCopyableFootprints(&cdesc, 0, VIEWS, 0, cfp, nullptr, nullptr, &cTotal);
    dev->GetCopyableFootprints(&ddesc, 0, VIEWS, 0, dfp, nullptr, nullptr, &dTotal);
    ComPtr<ID3D12Resource> crb, drb;
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, cTotal, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, crb, nullptr)) return false;
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, dTotal, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, drb, nullptr)) return false;

    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    RecordUploadNear(app, nearUp.Get());
    RecordWarp(app, t, DESC_TEST_SRC, c);
    for (UINT e = 0; e < VIEWS; e++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = e;
        dst.pResource = crb.Get(); dst.PlacedFootprint = cfp[e]; src.pResource = t.colorOut.Get();
        app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        dst.pResource = drb.Get(); dst.PlacedFootprint = dfp[e]; src.pResource = t.depthOut.Get();
        app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    RecordWarpOutputsBackToUav(app, t);
    app.cmdList->Close();
    WaitFence(app, SubmitAndSignal(app));

    unsigned char* cp = nullptr; float* dp = nullptr;
    if (FAILED(crb->Map(0, nullptr, (void**)&cp)) || FAILED(drb->Map(0, nullptr, (void**)&dp)) || !cp || !dp) { Log("SelfTestWarp[%s]: FAIL map readback", name); return false; }

    std::vector<unsigned char> refColor((size_t)ROW_PITCH * H);
    std::vector<float> refDepth((size_t)(ROW_PITCH / 4) * H);
    const float eyes[VIEWS] = { c.eye0, c.eye1 };
    size_t badColor = 0, badDepth = 0;
    for (UINT e = 0; e < VIEWS; e++)
    {
        WarpEye(scene, near01, eyes[e], c.scaleFocal, 1.0f, c.invZNear, c.invZFar, c.nearZ, c.farZ, c.doWarp != 0, refColor.data(), refDepth.data());
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

    const size_t total = (size_t)W * H * VIEWS;
    const size_t tolerance = total / 2000;          // 0.05 % - rounding ties only
    bool ok = badColor <= tolerance && badDepth <= tolerance;
    Log("SelfTestWarp[%s]: exit %s - mismatches colour %zu depth %zu of %zu px (tolerance %zu)", name, ok ? "PASS" : "FAIL", badColor, badDepth, total, tolerance);
    return ok;
}

// GPU model-input prep vs the CPU preprocessing, for a WxH source (exact path).
static bool SelfTestPrep(App& app, const std::vector<unsigned char>& scene)
{
    Log("SelfTestPrep: enter");
    if (!UploadTestSource(app, scene)) return false;

    UINT64 v = SubmitPrep(app, DESC_TEST_SRC, W, H, nullptr, 0);
    WaitMlFence(app, v);

    const UINT64 bytes = (UINT64)3 * W * H * sizeof(float);
    ComPtr<ID3D12Resource> rb;
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb, nullptr)) return false;
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    Transition(app.cmdList.Get(), app.modelIn.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    app.cmdList->CopyBufferRegion(rb.Get(), 0, app.modelIn.Get(), 0, bytes);
    Transition(app.cmdList.Get(), app.modelIn.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    app.cmdList->Close();
    WaitFence(app, SubmitAndSignal(app));

    float* gp = nullptr;
    if (FAILED(rb->Map(0, nullptr, (void**)&gp)) || !gp) { Log("SelfTestPrep: FAIL map readback"); return false; }
    const float mean[3] = { 0.485f, 0.456f, 0.406f };
    const float istd[3] = { 1.f / 0.229f, 1.f / 0.224f, 1.f / 0.225f };
    size_t bad = 0;
    float worst = 0;
    for (int c = 0; c < 3; c++)
    {
        for (int y = 0; y < H; y++)
        {
            for (int x = 0; x < W; x++)
            {
                float ref = (scene[((size_t)y * W + x) * 3 + c] / 255.0f - mean[c]) * istd[c];
                float d = fabsf(gp[((size_t)c * H + y) * W + x] - ref);
                worst = std::max(worst, d);
                if (d > 1e-5f) bad++;
            }
        }
    }
    rb->Unmap(0, nullptr);

    bool ok = bad == 0;
    Log("SelfTestPrep: exit %s - %zu of %zu values differ from the CPU preprocessing (worst |diff| %.2e)", ok ? "PASS" : "FAIL", bad, (size_t)3 * W * H, worst);
    return ok;
}

static bool SelfTest(App& app)
{
    Log("SelfTest: enter");
    WarpConstants c{};
    c.cw = W; c.ch = H; c.dw = W; c.dh = H;
    c.doWarp = 1;
    c.scaleFocal = 362.0f;
    c.invZNear = 1.0f / 1.2f; c.invZFar = 1.0f / 12.0f;
    c.eye0 = -0.0355f; c.eye1 = 0.0355f;
    c.nearZ = DEPTH_NEAR_Z; c.farZ = DEPTH_FAR_Z;
    c.writeDepth = 1; c.exactLoad = 1;

    std::vector<unsigned char> synth;
    std::vector<float> truth;
    MakeScene(synth, 1.7);
    MakeTruth(truth, 1.7);

    // a smooth-gradient depth exercises sub-pixel-varying disparity and stretching
    std::vector<float> ramp((size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            ramp[(size_t)y * W + x] = 0.5f + 0.5f * sinf(x * 0.021f) * cosf(y * 0.017f);

    bool ok = SelfTestPrep(app, synth);
    ok = SelfTestWarp(app, "synthetic+truth", synth, truth, c) && ok;
    ok = SelfTestWarp(app, "synthetic+ramp", synth, ramp, c) && ok;
    WarpConstants big = c;
    big.scaleFocal *= 4.0f;
    ok = SelfTestWarp(app, "synthetic+ramp x4", synth, ramp, big) && ok;
    WarpConstants off = c;
    off.doWarp = 0;
    ok = SelfTestWarp(app, "no-warp", synth, truth, off) && ok;

    Log("SelfTest: exit %s", ok ? "ALL PASS" : "FAILED");
    return ok;
}

// -------------------------------------------------------------- frame loop
static void RunFrameLoop(App& app)
{
    Log("RunFrameLoop: enter (%.0fs)%s%s", app.opt.runSeconds, app.opt.doWarp ? "" : " [warp disabled]",
        app.opt.paired ? " [paired colour+depth]" : " [latest colour + latest completed depth]");

    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, exitLoop = false, haveDepth = false, printedView = false;
    double loopStart = NowSeconds(), lastReport = loopStart;
    uint64_t frames = 0, drawn = 0, depthUpdates = 0;
    uint64_t repFrames = 0, repDrawn = 0, repDepth = 0, lastCapFrames = 0;
    double repCpuMs = 0, repAgeMs = 0;
    std::vector<unsigned char> scene;
    const DepthSlot* cur = nullptr;
    const bool useDepthSc = app.opt.submitDepth;
    WarpTarget& target = app.mainTarget;

    const double period = app.opt.abSeconds > 0.0 ? app.opt.abSeconds : 6.0;
    double firstDrawTime = -1.0;
    long long lastPhase = -1;
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

            float ex = views[1].pose.position.x - views[0].pose.position.x;
            float ey = views[1].pose.position.y - views[0].pose.position.y;
            float ez = views[1].pose.position.z - views[0].pose.position.z;
            float ipd = sqrtf(ex * ex + ey * ey + ez * ez);

            // Same image in both eyes => ONE symmetric FOV and ONE orientation for
            // both views (see M3 findings). Vertical FOV from the picture's aspect.
            float tanHalfX = 1e9f;
            for (uint32_t e = 0; e < VIEWS; e++)
            {
                tanHalfX = std::min(tanHalfX, tanf(fabsf(views[e].fov.angleLeft)));
                tanHalfX = std::min(tanHalfX, tanf(fabsf(views[e].fov.angleRight)));
            }
            if (!(tanHalfX > 0.01f && tanHalfX < 100.0f)) tanHalfX = 1.0f;
            float tanHalfY = tanHalfX * (float)target.ch / (float)target.cw;
            XrFovf sharedFov{ -atanf(tanHalfX), atanf(tanHalfX), atanf(tanHalfY), -atanf(tanHalfY) };
            float focalPx = (float)(target.cw * 0.5) / tanHalfX;

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
                Log("RunFrameLoop: shared fov %.1f x %.1f deg  ipd %.1f mm  focal %.1f px  warp scale %.2f  (%.1f px/deg)",
                    (sharedFov.angleRight - sharedFov.angleLeft) * 57.2958f, (sharedFov.angleUp - sharedFov.angleDown) * 57.2958f,
                    ipd * 1000.0f, focalPx, app.opt.warpScale, target.cw / ((sharedFov.angleRight - sharedFov.angleLeft) * 57.2958f));
            }

            if (firstDrawTime < 0.0) firstDrawTime = NowSeconds();
            long long phase = (long long)((NowSeconds() - firstDrawTime) / period);
            if (phase != lastPhase)
            {
                lastPhase = phase;
                if (app.opt.abSeconds > 0.0) depthOn = (phase % 2) == 0;
                for (uint32_t e = 0; e < VIEWS; e++) { frozenPose[e] = views[e].pose; frozenPose[e].orientation = sharedRot; }
                if (app.opt.abSeconds > 0.0 || app.opt.freezePose)
                    Log("RunFrameLoop: phase %lld - depth chain %s%s", phase, depthOn ? "ON (green)" : "OFF (red)",
                        app.opt.freezePose ? ", pose re-captured and frozen" : "");
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
                WaitFence(app, app.frameFence[ring]);
                app.cmdAlloc[ring]->Reset();
                app.cmdList->Reset(app.cmdAlloc[ring].Get(), nullptr);

                // --- latest source frame
                int newSrc = -1;
                double t = NowSeconds();
                if (app.opt.source == SourceKind::Synthetic)
                {
                    MakeScene(scene, t);
                    newSrc = RecordCpuSource(app, scene, ring);
                }
                SourcePub latest = LatestSource(app);
                int srcIndex = newSrc >= 0 ? newSrc : latest.index;

                // --- latest COMPLETED depth
                bool tookSlot = false;
                if (app.readySlot.load() & SLOT_FRESH)
                {
                    WaitFence(app, app.slotFence);
                    app.readSlot = app.readySlot.exchange(app.readSlot) & (SLOT_FRESH - 1);
                    cur = &app.slots[app.readSlot];
                    RecordUploadNear(app, cur->nearUp.Get());
                    tookSlot = true; haveDepth = true;
                    depthUpdates++; repDepth++;
                }
                if (app.opt.paired && cur && cur->srcIndex >= 0) srcIndex = cur->srcIndex;

                if (haveDepth && srcIndex >= 0)
                {
                    // a captured frame comes from another device: make the gfx queue
                    // wait for it on the GPU timeline (no CPU stall)
                    if (newSrc < 0 && latest.fence && latest.fence != app.fence.Get() && srcIndex == latest.index)
                        app.gfxQueue->Wait(latest.fence, latest.value);

                    WarpConstants c{};
                    c.cw = (uint32_t)target.cw; c.ch = (uint32_t)target.ch; c.dw = W; c.dh = H;
                    c.doWarp = app.opt.doWarp ? 1u : 0u;
                    c.scaleFocal = app.opt.warpScale * focalPx;
                    c.invZNear = 1.0f / 1.2f;
                    c.invZFar = 1.0f / 12.0f;
                    c.eye0 = -0.5f * ipd; c.eye1 = 0.5f * ipd;
                    c.nearZ = DEPTH_NEAR_Z; c.farZ = DEPTH_FAR_Z;
                    c.indicator = (app.opt.abSeconds > 0.0) ? (depthOn ? 1u : 2u) : 0u;
                    c.depthLie = app.opt.depthLie ? 1u : 0u;
                    c.writeDepth = useDepthSc ? 1u : 0u;
                    c.exactLoad = (app.srcW == target.cw && app.srcH == target.ch) ? 1u : 0u;
                    RecordWarp(app, target, DESC_SRC0 + (UINT)srcIndex, c);
                    RecordCopyToSwapchain(app, target, app.cimgs[cIdx].texture, useDepthSc ? app.dimgs[dIdx].texture : nullptr);
                    RecordWarpOutputsBackToUav(app, target);
                }
                app.cmdList->Close();
                UINT64 fv = SubmitAndSignal(app);
                app.frameFence[ring] = fv;
                if (tookSlot) app.slotFence = fv;
                if (newSrc >= 0) PublishSource(app, newSrc, app.fence.Get(), fv, t);

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
                    pviews[e].subImage.imageRect = { {0, 0}, {target.cw, target.ch} };
                    pviews[e].subImage.imageArrayIndex = e;

                    dinfo[e] = { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
                    dinfo[e].subImage.swapchain = app.depthSc;
                    dinfo[e].subImage.imageRect = { {0, 0}, {target.cw, target.ch} };
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
            uint64_t capFrames = app.cap.frames.load();
            Log("render %.1f fps (drawn %.1f, cpu %.2f ms/frame) | capture %.1f fps | depth %.1f updates/s, model %.1f ms, age %.1f ms, range %.2f..%.2f%s",
                repFrames / dt, repDrawn / dt, repDrawn ? repCpuMs / repDrawn : 0.0,
                (capFrames - lastCapFrames) / dt,
                repDepth / dt, cur ? cur->modelMs : 0.0, repDrawn ? repAgeMs / repDrawn : 0.0,
                cur ? cur->lo : 0.f, cur ? cur->hi : 0.f,
                app.opt.useTruth ? " [warp uses TRUTH]" : "");
            if (app.opt.source == SourceKind::Synthetic && cur)
                Log("       model near: back %.2f panel %.2f marker %.2f", cur->back, cur->panel, cur->marker);
            lastReport = now; repFrames = repDrawn = repDepth = 0; repCpuMs = repAgeMs = 0;
            lastCapFrames = capFrames;
        }
        if (now - loopStart > app.opt.runSeconds) exitLoop = true;
    }

    double el = NowSeconds() - loopStart;
    Log("RunFrameLoop: exit - frames %llu (%.1f fps) drawn %llu (%.1f fps) depth updates %llu (%.1f /s), worker published %llu, captured %llu",
        (unsigned long long)frames, frames / (el > 0 ? el : 1), (unsigned long long)drawn, drawn / (el > 0 ? el : 1),
        (unsigned long long)depthUpdates, depthUpdates / (el > 0 ? el : 1), (unsigned long long)app.depthPublished.load(),
        (unsigned long long)app.cap.frames.load());
}

// ------------------------------------------------------------------- main
// Puts the first source frame in place and publishes it (synchronously).
static bool FirstSourceFrame(App& app)
{
    Log("FirstSourceFrame: enter");

    if (app.opt.source == SourceKind::Capture)
    {
        app.cap.thread = std::thread(CaptureMain, &app);
        const double deadline = NowSeconds() + 5.0;
        while (LatestSource(app).index < 0 && NowSeconds() < deadline) Sleep(5);
        if (LatestSource(app).index < 0) { Log("FirstSourceFrame: FAIL no captured frame within 5 s"); return false; }
        Log("FirstSourceFrame: exit ok (first captured frame arrived)");
        return true;
    }

    std::vector<unsigned char> first;
    if (app.opt.source == SourceKind::Image) first = app.imageRgb; else MakeScene(first, 0.0);
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    int k = RecordCpuSource(app, first, 0);
    app.cmdList->Close();
    if (k < 0) return false;
    UINT64 fv = SubmitAndSignal(app);
    WaitFence(app, fv);
    PublishSource(app, k, app.fence.Get(), fv, 0.0);
    Log("FirstSourceFrame: exit ok (source texture %d)", k);
    return true;
}

static void Shutdown(App& app)
{
    Log("Shutdown: enter");
    app.stop = true;
    if (app.worker.joinable()) app.worker.join();
    if (app.cap.thread.joinable()) app.cap.thread.join();
    WaitFence(app, app.fenceVal);

    DumpDebugMessages(app);

    if (app.modelInValue) ort->ReleaseValue(app.modelInValue);
    if (app.ortSession) ort->ReleaseSession(app.ortSession);
    if (app.colorSc) xrDestroySwapchain_(app.colorSc);
    if (app.depthSc) xrDestroySwapchain_(app.depthSc);
    if (app.session) xrDestroySession_(app.session);
    if (app.instance) xrDestroyInstance_(app.instance);
    if (app.fenceEvent) CloseHandle(app.fenceEvent);
    if (app.mlFenceEvent) CloseHandle(app.mlFenceEvent);
    Log("Shutdown: exit");
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_t0 = std::chrono::steady_clock::now();
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    static App app;
    if (!ParseArgs(argc, argv, &app.opt)) return 1;

    int rc = 1;
    do
    {
        if (!InitXrInstance(app)) break;
        if (!InitD3D(app)) break;
        if (!InitModel(app)) break;
        if (!InitSlots(app)) break;
        if (!InitSource(app)) break;
        if (!InitXrSession(app)) break;
        if (!InitShaders(app)) break;

        if (!SelfTest(app)) { Log("main: GPU shaders do not match the CPU reference - not presenting"); break; }
        if (app.opt.selfTestOnly) { rc = 0; break; }

        if (!FirstSourceFrame(app)) break;

        // First depth result synchronously, so the render thread has depth from
        // its first frame and the log shows the model's numbers even if the HMD
        // never wakes.
        std::vector<float> firstNear;
        if (!ComputeAndPublish(app, LatestSource(app), firstNear)) break;
        {
            const DepthSlot& s = app.slots[app.readySlot.load() & (SLOT_FRESH - 1)];
            Log("main: first depth in %.1f ms (includes warm-up), range %.3f..%.3f", s.modelMs, s.lo, s.hi);
            if (app.opt.source == SourceKind::Synthetic)
                Log("main: model nearness (0 far..1 near): backdrop %.2f panel %.2f marker %.2f  [truth 0.00 / 0.50 / 1.00]", s.back, s.panel, s.marker);
        }
        if (app.opt.doDump)
        {
            std::vector<unsigned char> g(firstNear.size());
            for (size_t i = 0; i < g.size(); i++) g[i] = (unsigned char)(firstNear[i] * 255.0f + 0.5f);
            WritePNM("xrapp5_near.pgm", "P5", g.data(), g.size());
        }

        app.worker = std::thread(WorkerMain, &app);
        RunFrameLoop(app);
        rc = 0;
    } while (false);

    Shutdown(app);
    return rc;
}
