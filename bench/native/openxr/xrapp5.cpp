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
//   (ID3D12CommandQueue::Wait). CPU frame references and reader-completion fences
//   prevent reuse; incoming frames are dropped if the source ring is full.
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
//            --window=TEXT        unique visible non-terminal window with matching title
//            --exe=NAME.exe       unique visible window owned by this executable
//            --check-source       report selected window and exit without starting VR
//            --image=PATH         still image
//            --synthetic          the moving test scene (with --truth: ground-truth depth)
//   options: --scale=N --no-warp --paired --no-smooth --tau=SECONDS
//            --no-foreground       disable experimental foreground crop passes
//            --fill=mirror|stretch  disocclusion fill (default mirror, see FillHole)
//            --dilate=N             widen foreground depth by N depth px (default 2)
//            --dump                 write the depth map and BOTH eyes as warped by the GPU,
//                                   for each fill mode, at the presented resolution
//            --selftest --debug --dump --submit-depth --freeze-pose --ab=N --depth-lie
//            --test-depth-failures=N  diagnostic: fail the first N depth attempts
//            --head-locked          follow your head (default fixed screen; '=' recenters)
//            --keep-dashboard       skip the SteamVR startup dashboard-close request
//   keys:    '=' recenter; F8 dismiss SteamVR dashboard (keys also reach the game)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "steamvr_dashboard.h"
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
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

#include "xr_common.h"
#include "source_ring.h"
#include "capture_scaler.h"
#include "xr_frame_guard.h"
#include "screen_anchor.h"
#include "capture_window.h"
#include "desktop_control.h"
#include "foreground_refinement.h"

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

static std::filesystem::path ExecutableDirectory()
{
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    if (!count || count >= std::size(path)) throw std::runtime_error("Cannot locate renderer executable");
    return std::filesystem::path(path).parent_path();
}

// What the engine needs to know about a depth model. The renderer's depth grid
// stays W x H (xr_common.h); RunModelRaw resamples each model's output into it, so
// normalisation, dilation, foreground refinement and the warp are model-agnostic.
// Both models output relative inverse depth (larger = nearer).
struct ModelSpec
{
    const char* name;
    const wchar_t* file;
    const char* inName;
    const char* outName;
    int inW, inH;                       // fixed NCHW input size; output is the same size
    bool imagenetNorm;                  // prep applies ImageNet mean/std (else RGB 0..1)
    // Foreground widening, in depth-grid pixels, that covers this model's soft edges
    // (see DilateNear). Measured on GPU-warped eye dumps of a near panel over a
    // textured scene: the smallest values with no ghost lines at side edges and no
    // skewed rows at the top edge.
    int dilateH, dilateV;
};

// Depth Anything V2 Small, fixed-shape export (bench/make_fixed_shape.py).
static const ModelSpec kDepthAnythingV2 = { "Depth Anything V2 Small", L"model_fixed_686x392.onnx",
    "pixel_values", "predicted_depth", 686, 392, true, 2, 2 };

// ZipDepth-base (MIT), faithful FP16 export by bench/zipdepth_export.py. Sides must be
// multiples of 32; 672x384 keeps the 1.75 aspect. It normalises internally. Measured
// 2.0 ms vs 13.1 ms for DA-V2 on this DirectML stack (bench/native/dmlgpu probe).
static const ModelSpec kZipDepth = { "ZipDepth-base", L"zipdepth_faithful_fp16_672x384.onnx",
    "image", "depth", 672, 384, false, 3, 3 };

static std::filesystem::path ModelPath(const wchar_t* file)
{
    if (!file) throw std::runtime_error("No depth model file specified.");

    const auto directory = ExecutableDirectory();
    const auto packaged = directory / L"models" / file;
    if (std::filesystem::is_regular_file(packaged)) return packaged;
    for (auto parent = directory; !parent.empty();)
    {
        const auto candidate = parent / L"bench" / L"models" / file;
        if (std::filesystem::is_regular_file(candidate)) return candidate;
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    throw std::runtime_error("Depth model missing. Reinstall VRX or restore engine/models/" + winrt::to_string(file) + ".");
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
    std::wstring executable;
    bool checkSource = false;
    bool checkPackage = false;
    std::wstring controlPath, executablePath;
    DWORD capturePid = 0;
    HWND captureHwnd = nullptr;
    std::wstring imagePath;
    bool doWarp = true;
    float warpScale = 1.0f;
    bool useTruth = false;
    bool doDump = false;
    bool paired = false;
    bool foreground = true;
    bool boostGpuPriority = true;      // best effort; --normal-gpu-priority disables the experiment
    // Which GPU runs the depth model: -1 = the headset GPU (default), -2 = automatic
    // second GPU (the largest other hardware adapter), N = DXGI adapter index.
    int depthAdapter = -1;
    const ModelSpec* model = &kZipDepth;          // default; --model=dav2 selects Depth Anything V2
    bool selfTestOnly = false;
    bool debugLayer = false;
    bool smooth = true;
    int fillMode = FILL_MIRROR;
    int dilate = -1;                    // depth pixels; -1 = the model's own value (ModelSpec)
    int dilateV = -1;                   // vertical depth pixels; -1 = the model's own value
    double smoothTau = 0.4;             // seconds
    bool submitDepth = false;           // SteamVR ignores it (measured) - opt-in only
    bool freezePose = false;
    bool headLocked = false;           // default: fixed screen; '=' recenters
    bool keepDashboard = false;
    bool depthLie = false;
    double abSeconds = 0.0;
    int testDepthFailures = 0;          // diagnostic: inject initial inference failures
};

static const int SLOTS = 3;
static const int SLOT_FRESH = 4;        // flag bit in readySlot
static const int RING = 3;              // in-flight render frames
static const int SRC_RING = 8;          // source textures; references and GPU fences guard reuse
using SourceFrames = SourceRing<SRC_RING>;
using SourceRef = SourceFrames::ReadRef;
static const int MAX_COLOR_W = 1920;
static const uint32_t VIEWS = 2;
static const float DEPTH_NEAR_Z = 0.10f, DEPTH_FAR_Z = 30.0f;

// One completed depth result (worker -> render thread).
struct DepthSlot
{
    ComPtr<ID3D12Resource> nearUp;      // UPLOAD heap, float[W*H], 0 = far .. 1 = near
    float* nearMapped = nullptr;
    SourceRef source;                  // retains the exact colour frame paired with this depth
    double sceneTime = 0;
    double completeTime = 0;
    double modelMs = 0;
    float lo = 0, hi = 0;               // normalisation range actually used
    float back = 0, panel = 0, marker = 0;
};

struct WarpConstants                    // must match cbuffer C in kWarpHlsl
{
    uint32_t cw, ch, dw, dh;
    uint32_t doWarp;
    float scaleFocal, invZNear, invZFar;
    float eye0, eye1, nearZ, farZ;
    uint32_t indicator, depthLie, writeDepth, exactLoad;
    uint32_t fillMode;
    float mirrorTol;
};

struct PrepConstants                    // must match cbuffer C in kPrepHlsl
{
    uint32_t dw, dh, taps, exactLoad;   // dw x dh = the model's input size
    float cropX = 0, cropY = 0, cropSize = 1;
    uint32_t normalize = 1;             // 1: ImageNet mean/std, 0: RGB 0..1 (model normalises)
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
    ComPtr<ID3D11RenderTargetView> rtv11[SRC_RING];
    CaptureScaler scaler;
    winrt::event_token closedToken{};
    bool closedRegistered = false;
    std::atomic<bool> closed{ false };
    wgc::GraphicsCaptureItem item{ nullptr };
    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    wdx::Direct3D11::IDirect3DDevice rtDevice{ nullptr };
    std::thread thread;
    std::atomic<uint64_t> frames{ 0 };
    HWND hwnd = nullptr;                // window capture only; frames are cropped to its client area
    int frameW = 0, frameH = 0;         // captured frame (pool) size - the whole window; srcW/srcH may be smaller
    RECT crop{};                        // last client crop applied (for change logging)
    bool cropping = false;
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
    SourceFrames sources;
    std::atomic<uint64_t> sourceDrops{ 0 };
    std::vector<unsigned char> imageRgb;
    ComPtr<ID3D12Fence> captureFence;                    // shared with D3D11
    UINT64 captureFenceVal = 0;
    Capture cap;

    // model
    OrtEnv* env = nullptr;
    OrtSession* ortSession = nullptr;
    OrtValue* modelInValue = nullptr;
    const OrtDmlApi* dmlApi = nullptr;
    ComPtr<IDMLDevice> dmlDevice;
    void* dmlAlloc = nullptr;           // DML wrapper around modelIn; freed on reload
    // Written by the render thread from the desktop control file; the worker
    // compares it with opt.model and reloads between passes (null = no request).
    std::atomic<const ModelSpec*> requestedModel{ nullptr };
    ComPtr<ID3D12Resource> modelIn;                      // DEFAULT heap on the inference GPU; DirectML reads it

    // Depth inference placement. One GPU: inferDevice == device, inferQueue ==
    // mlQueue and prepOut == modelIn. Second GPU: the prep shader still runs on the
    // headset GPU (captured frames live there) and writes prepOut; that is copied
    // through a cross-adapter shared heap into modelIn on the second GPU, where the
    // model runs. Only GPU-side fences order the hand-over.
    bool secondGpu = false;
    std::wstring inferName;
    ComPtr<ID3D12Device> inferDevice;
    ComPtr<ID3D12CommandQueue> inferQueue;
    ComPtr<ID3D12CommandAllocator> inferCmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> inferCmdList;
    ComPtr<ID3D12Fence> inferFence;                      // inference GPU: copy-in completion
    UINT64 inferFenceVal = 0;
    HANDLE inferFenceEvent = nullptr;
    ComPtr<ID3D12Resource> prepOut;                      // headset GPU: what the prep shader writes
    ComPtr<ID3D12Heap> crossHeap, crossHeapInfer;        // one shared heap, opened on each GPU
    ComPtr<ID3D12Resource> crossBuf, crossBufInfer;      // the same memory as seen by each GPU
    UINT64 crossBytes = 0;
    ComPtr<ID3D12Fence> crossFence, crossFenceInfer;     // headset GPU signals, inference GPU waits
    UINT64 crossVal = 0;
    std::vector<float> modelOut;        // model output at the model's own size
    struct ResampleTap { int i0, i1; float t; };
    std::vector<ResampleTap> resampleX, resampleY;   // model output -> depth grid, built once
    std::vector<float> rawDepth;        // model output resampled to the W x H depth grid
    RangeSmoother smoother;
    ForegroundTracker foregroundTracker;
    ForegroundBudget foregroundBudget;
    std::atomic<bool> foregroundEnabled{ true };
    unsigned foregroundAttempts = 0, foregroundAccepted = 0;
    double foregroundLogTime = 0;
    uint64_t foregroundLayout = 0;

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
    std::thread dashboardWorker;
    std::atomic<bool> dashboardBusy{ false };
    bool steamVrRuntime = false;
    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> depthPublished{ 0 };
    std::atomic<bool> depthHealthy{ false };
    int testDepthFailuresLeft = 0;      // main until worker starts, then worker only
};

// descriptor heap layout
static const UINT DESC_SRC0 = 0;                         // SRC_RING source SRVs
static const UINT DESC_MAIN_TABLE = SRC_RING;            // 4
static const UINT DESC_TEST_SRC = SRC_RING + 4;          // 1
static const UINT DESC_TEST_TABLE = SRC_RING + 5;        // 4
static const UINT DESC_COUNT = SRC_RING + 9;

static ID3D12Fence* SourceFence(App& app)
{
    return app.opt.source == SourceKind::Capture ? app.captureFence.Get() : app.fence.Get();
}

static SourceFrames::WriteRef ReserveSource(App& app)
{
    auto frame = app.sources.Reserve(SourceFence(app)->GetCompletedValue(),
        app.fence->GetCompletedValue(), app.mlFence->GetCompletedValue());
    if (!frame) app.sourceDrops++;
    return frame;
}

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

    auto widen = [](const char* s) { return std::wstring(winrt::to_hstring(s)); };
    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        if (!a || !a[0]) continue;
        if (!strcmp(a, "--capture")) { opt->source = SourceKind::Capture; continue; }
        if (!strncmp(a, "--monitor=", 10)) { opt->source = SourceKind::Capture; opt->monitorIndex = atoi(a + 10); continue; }
        if (!strncmp(a, "--window=", 9)) { opt->source = SourceKind::Capture; opt->windowTitle = widen(a + 9); continue; }
        if (!strncmp(a, "--exe=", 6))
        {
            if (!a[6]) { Log("ParseArgs: --exe requires an executable filename"); return false; }
            opt->source = SourceKind::Capture; opt->executable = widen(a + 6); continue;
        }
        if (!strcmp(a, "--check-source")) { opt->checkSource = true; continue; }
        if (!strncmp(a, "--control=", 10)) { opt->controlPath = widen(a + 10); continue; }
        if (!strncmp(a, "--exe-path=", 11)) { opt->executablePath = widen(a + 11); continue; }
        if (!strncmp(a, "--pid=", 6)) { opt->capturePid = strtoul(a + 6, nullptr, 10); if (!opt->capturePid) return false; continue; }
        if (!strncmp(a, "--hwnd=", 7)) { opt->captureHwnd = (HWND)(uintptr_t)strtoull(a + 7, nullptr, 0); if (!opt->captureHwnd) return false; continue; }
        if (!strncmp(a, "--image=", 8)) { opt->source = SourceKind::Image; opt->imagePath = widen(a + 8); continue; }
        if (!strcmp(a, "--synthetic")) { opt->source = SourceKind::Synthetic; continue; }
        if (!strcmp(a, "--no-warp")) { opt->doWarp = false; continue; }
        if (!strncmp(a, "--scale=", 8)) { opt->warpScale = (float)atof(a + 8); continue; }
        if (!strcmp(a, "--truth")) { opt->useTruth = true; continue; }
        if (!strcmp(a, "--dump")) { opt->doDump = true; continue; }
        if (!strcmp(a, "--paired")) { opt->paired = true; continue; }
        if (!strcmp(a, "--check-package")) { opt->checkPackage = true; continue; }
        if (!strcmp(a, "--no-foreground")) { opt->foreground = false; continue; }
        if (!strcmp(a, "--normal-gpu-priority")) { opt->boostGpuPriority = false; continue; }
        if (!strcmp(a, "--depth-gpu=same")) { opt->depthAdapter = -1; continue; }
        if (!strcmp(a, "--depth-gpu=auto")) { opt->depthAdapter = -2; continue; }
        if (!strncmp(a, "--depth-gpu=", 12))
        {
            char* end = nullptr;
            const long index = strtol(a + 12, &end, 10);
            if (!end || *end || index < 0 || index > 15) { Log("ParseArgs: --depth-gpu must be same, auto or an adapter index 0..15"); return false; }
            opt->depthAdapter = (int)index;
            continue;
        }
        if (!strcmp(a, "--model=zipdepth")) { opt->model = &kZipDepth; continue; }
        if (!strcmp(a, "--model=dav2")) { opt->model = &kDepthAnythingV2; continue; }
        if (!strcmp(a, "--selftest")) { opt->selfTestOnly = true; continue; }
        if (!strcmp(a, "--debug")) { opt->debugLayer = true; continue; }
        if (!strncmp(a, "--test-depth-failures=", 22)) { opt->testDepthFailures = atoi(a + 22); continue; }
        if (!strcmp(a, "--no-smooth")) { opt->smooth = false; continue; }
        if (!strcmp(a, "--fill=mirror")) { opt->fillMode = FILL_MIRROR; continue; }
        if (!strcmp(a, "--fill=stretch")) { opt->fillMode = FILL_STRETCH; continue; }
        if (!strncmp(a, "--dilate=", 9)) { opt->dilate = atoi(a + 9); continue; }
        if (!strncmp(a, "--dilate-v=", 11)) { opt->dilateV = atoi(a + 11); continue; }
        if (!strncmp(a, "--tau=", 6)) { opt->smoothTau = atof(a + 6); continue; }
        if (!strcmp(a, "--submit-depth")) { opt->submitDepth = true; continue; }
        if (!strcmp(a, "--freeze-pose")) { opt->freezePose = true; continue; }
        if (!strcmp(a, "--head-locked")) { opt->headLocked = true; continue; }
        if (!strcmp(a, "--keep-dashboard")) { opt->keepDashboard = true; continue; }
        if (!strcmp(a, "--depth-lie")) { opt->depthLie = true; continue; }
        if (!strncmp(a, "--ab=", 5)) { opt->abSeconds = atof(a + 5); continue; }
        if (a[0] == '-') { Log("ParseArgs: unknown option %s", a); return false; }
        opt->runSeconds = atof(a);
    }
    if (opt->dilate < -1 || opt->dilate > 16 || opt->dilateV < -1 || opt->dilateV > 16) { Log("ParseArgs: --dilate and --dilate-v must be 0..16"); return false; }
    if (opt->executable.find_first_of(L"\\/") != std::wstring::npos)
    { Log("ParseArgs: --exe takes a filename, for example helldivers2.exe"); return false; }
    if (opt->checkSource && (opt->source != SourceKind::Capture || (opt->windowTitle.empty() && opt->executable.empty())))
    { Log("ParseArgs: --check-source requires --exe or --window"); return false; }
    if (opt->testDepthFailures < 0 || opt->testDepthFailures > 10) { Log("ParseArgs: --test-depth-failures must be 0..10"); return false; }
    if (opt->abSeconds < 0.0 || opt->smoothTau <= 0.0) { Log("ParseArgs: --ab must be >= 0 and --tau > 0"); return false; }
    if ((opt->abSeconds > 0.0 || opt->depthLie) && !opt->submitDepth) { Log("ParseArgs: --ab / --depth-lie imply --submit-depth"); opt->submitDepth = true; }
    if (opt->submitDepth || opt->freezePose)
    {
        Log("ParseArgs: projection diagnostics imply --head-locked");
        opt->headLocked = true;
    }
    Log("ParseArgs: screen %s; '=' recenters the fixed screen", opt->headLocked ? "head-following" : "fixed in room");
    if (opt->useTruth && opt->source != SourceKind::Synthetic) { Log("ParseArgs: --truth only applies to --synthetic, ignored"); opt->useTruth = false; }

    Log("ParseArgs: source %s (monitor %d, window '%ls', image '%ls') seconds %.0f",
        opt->source == SourceKind::Capture ? "capture" : opt->source == SourceKind::Image ? "image" : "synthetic",
        opt->monitorIndex, opt->windowTitle.c_str(), opt->imagePath.c_str(), opt->runSeconds);
    Log("ParseArgs: fill %s dilate %d vertical %d (-1 = per model)", opt->fillMode == FILL_MIRROR ? "mirror" : "stretch", opt->dilate, opt->dilateV);
    Log("ParseArgs: depth model %s (%dx%d)", opt->model->name, opt->model->inW, opt->model->inH);
    Log("ParseArgs: depth GPU %s", opt->depthAdapter == -1 ? "same as headset" : opt->depthAdapter == -2 ? "auto (second GPU)" : "adapter index");
    Log("ParseArgs: warp %d scale %.2f truth %d paired %d smooth %d tau %.2fs submitDepth %d selftest %d debug %d",
        (int)opt->doWarp, opt->warpScale, (int)opt->useTruth, (int)opt->paired, (int)opt->smooth, opt->smoothTau,
        (int)opt->submitDepth, (int)opt->selfTestOnly, (int)opt->debugLayer);
    return true;
}

// ------------------------------------------------------------ xr instance
static bool InitXrInstance(App& app)
{
    Log("InitXrInstance: enter");

    HMODULE loader = LoadLibraryW((ExecutableDirectory() / L"openxr_loader.dll").c_str());
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
    PFN_xrGetInstanceProperties getProperties = nullptr;
    if (XR_SUCCEEDED(g_getProc(app.instance, "xrGetInstanceProperties", (PFN_xrVoidFunction*)&getProperties)) && getProperties)
    {
        XrInstanceProperties properties{ XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(getProperties(app.instance, &properties)))
        {
            app.steamVrRuntime = strstr(properties.runtimeName, "SteamVR") != nullptr;
            Log("InitXrInstance: runtime %s", properties.runtimeName);
        }
    }

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
// Largest-memory hardware adapter other than the headset one, or the requested index.
static bool PickSecondAdapter(App& app, IDXGIFactory4* fac, ComPtr<IDXGIAdapter1>& out)
{
    if (!fac) return false;
    ComPtr<IDXGIAdapter1> best;
    SIZE_T bestMemory = 0;
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> each;
        if (fac->EnumAdapters1(i, &each) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        each->GetDesc1(&d);
        const bool headset = d.AdapterLuid.LowPart == app.adapterLuid.LowPart && d.AdapterLuid.HighPart == app.adapterLuid.HighPart;
        const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        Log("PickSecondAdapter: adapter %u %ls, %llu MB%s%s", i, d.Description, (unsigned long long)(d.DedicatedVideoMemory >> 20),
            headset ? " [headset GPU]" : "", software ? " [software]" : "");
        if (app.opt.depthAdapter >= 0)
        {
            if ((int)i != app.opt.depthAdapter) continue;
            if (headset) { Log("PickSecondAdapter: adapter %u is the headset GPU; nothing to offload", i); return false; }
            if (software) { Log("PickSecondAdapter: adapter %u is a software adapter; refused", i); return false; }
            out = each;
            return true;
        }
        if (headset || software || d.DedicatedVideoMemory <= bestMemory) continue;
        best = each;
        bestMemory = d.DedicatedVideoMemory;
    }
    if (app.opt.depthAdapter >= 0) { Log("PickSecondAdapter: no adapter %d", app.opt.depthAdapter); return false; }
    if (!best) { Log("PickSecondAdapter: no second hardware GPU found"); return false; }
    out = best;
    return true;
}

// Creates the inference device, its queue and the cross-adapter hand-over:
// a shared heap (created on the headset GPU, opened on the inference GPU) holding
// one buffer large enough for any model's input, and a cross-adapter fence.
static bool InitSecondGpu(App& app, IDXGIFactory4* fac)
{
    Log("InitSecondGpu: enter (requested %s)", app.opt.depthAdapter == -2 ? "auto" : "adapter index");
    if (!fac || !app.device) return false;

    ComPtr<IDXGIAdapter1> adapter;
    if (!PickSecondAdapter(app, fac, adapter)) return false;
    DXGI_ADAPTER_DESC1 d{};
    adapter->GetDesc1(&d);
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&app.inferDevice));
    if (FAILED(hr)) { Log("InitSecondGpu: FAIL D3D12CreateDevice on %ls (0x%08lX)", d.Description, (unsigned long)hr); return false; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(app.inferDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&app.inferQueue)))) { Log("InitSecondGpu: FAIL queue"); return false; }
    app.inferQueue->SetName(L"vrx inference queue (second GPU)");
    if (FAILED(app.inferDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&app.inferCmdAlloc)))) { Log("InitSecondGpu: FAIL allocator"); return false; }
    if (FAILED(app.inferDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, app.inferCmdAlloc.Get(), nullptr, IID_PPV_ARGS(&app.inferCmdList))))
    { Log("InitSecondGpu: FAIL command list"); return false; }
    app.inferCmdList->Close();
    if (FAILED(app.inferDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&app.inferFence)))) { Log("InitSecondGpu: FAIL fence"); return false; }
    app.inferFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!app.inferFenceEvent) { Log("InitSecondGpu: FAIL fence event"); return false; }

    // Largest input of any model the worker may switch to live.
    UINT64 largest = 0;
    for (const ModelSpec* spec : { &kDepthAnythingV2, &kZipDepth })
        largest = std::max<UINT64>(largest, (UINT64)3 * spec->inW * spec->inH * sizeof(float));
    const UINT64 align = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    app.crossBytes = (largest + align - 1) & ~(align - 1);

    D3D12_HEAP_DESC heap{};
    heap.SizeInBytes = app.crossBytes;
    heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.Alignment = align;
    heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    hr = app.device->CreateHeap(&heap, IID_PPV_ARGS(&app.crossHeap));
    if (FAILED(hr)) { Log("InitSecondGpu: FAIL cross-adapter heap (0x%08lX)", (unsigned long)hr); return false; }
    HANDLE handle = nullptr;
    hr = app.device->CreateSharedHandle(app.crossHeap.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(hr)) { Log("InitSecondGpu: FAIL heap handle (0x%08lX)", (unsigned long)hr); return false; }
    hr = app.inferDevice->OpenSharedHandle(handle, IID_PPV_ARGS(&app.crossHeapInfer));
    CloseHandle(handle);
    if (FAILED(hr)) { Log("InitSecondGpu: FAIL open heap on %ls (0x%08lX)", d.Description, (unsigned long)hr); return false; }

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = app.crossBytes; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    if (FAILED(app.device->CreatePlacedResource(app.crossHeap.Get(), 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&app.crossBuf))) ||
        FAILED(app.inferDevice->CreatePlacedResource(app.crossHeapInfer.Get(), 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&app.crossBufInfer))))
    { Log("InitSecondGpu: FAIL cross-adapter buffer"); return false; }

    hr = app.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&app.crossFence));
    if (FAILED(hr)) { Log("InitSecondGpu: FAIL cross-adapter fence (0x%08lX)", (unsigned long)hr); return false; }
    hr = app.device->CreateSharedHandle(app.crossFence.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(hr)) { Log("InitSecondGpu: FAIL fence handle (0x%08lX)", (unsigned long)hr); return false; }
    hr = app.inferDevice->OpenSharedHandle(handle, IID_PPV_ARGS(&app.crossFenceInfer));
    CloseHandle(handle);
    if (FAILED(hr)) { Log("InitSecondGpu: FAIL open fence on %ls (0x%08lX)", d.Description, (unsigned long)hr); return false; }

    app.inferName = d.Description;
    app.secondGpu = true;
    Log("InitSecondGpu: exit ok - depth runs on %ls; %llu KB hand-over buffer in a cross-adapter heap",
        d.Description, (unsigned long long)(app.crossBytes >> 10));
    return true;
}

static void ReleaseSecondGpu(App& app)
{
    Log("ReleaseSecondGpu: enter (%s)", app.secondGpu ? "active" : "partial or unused");
    app.secondGpu = false;
    app.crossFenceInfer.Reset(); app.crossFence.Reset();
    app.crossBufInfer.Reset(); app.crossBuf.Reset();
    app.crossHeapInfer.Reset(); app.crossHeap.Reset();
    app.inferCmdList.Reset(); app.inferCmdAlloc.Reset();
    app.inferFence.Reset();
    if (app.inferFenceEvent) { CloseHandle(app.inferFenceEvent); app.inferFenceEvent = nullptr; }
    app.inferQueue.Reset(); app.inferDevice.Reset();
    Log("ReleaseSecondGpu: exit");
}

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

    // Best-effort scheduling experiment. Keep rendering and inference at the
    // same queue priority; realtime inference could preempt headset rendering.
    // Queue HIGH is process-relative. The process class requests the boost
    // relative to other applications; neither request guarantees throughput.
    if (app.opt.boostGpuPriority)
    {
        typedef LONG(WINAPI* SetGpuClassFn)(HANDLE, int);
        HMODULE gdi = LoadLibraryW(L"gdi32.dll");
        SetGpuClassFn setClass = gdi ? (SetGpuClassFn)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass") : nullptr;
        const int D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH = 4;
        LONG st = setClass ? setClass(GetCurrentProcess(), D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH) : -1;
        Log("InitD3D: process GPU scheduling class HIGH -> %s (status 0x%08lX)", st == 0 ? "ok" : "not applied", (unsigned long)st);
        if (gdi) FreeLibrary(gdi);
    }
    else Log("InitD3D: GPU priority boost disabled");

    auto createQueue = [&](const char* name, ComPtr<ID3D12CommandQueue>& queue) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (app.opt.boostGpuPriority)
        {
            D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY support{};
            support.CommandListType = qd.Type;
            support.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
            HRESULT check = app.device->CheckFeatureSupport(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, &support, sizeof(support));
            if (SUCCEEDED(check) && support.PriorityForTypeIsSupported)
            {
                qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
                HRESULT hr = app.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
                if (SUCCEEDED(hr)) { Log("InitD3D: %s queue priority HIGH", name); return true; }
                Log("InitD3D: %s HIGH queue refused (0x%08lX); using NORMAL", name, (unsigned long)hr);
                queue.Reset();
            }
            else Log("InitD3D: %s HIGH queue unsupported (check 0x%08lX); using NORMAL", name, (unsigned long)check);
        }
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        HRESULT hr = app.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr)) { Log("InitD3D: FAIL %s NORMAL queue (0x%08lX)", name, (unsigned long)hr); return false; }
        Log("InitD3D: %s queue priority NORMAL", name);
        return true;
    };
    if (!createQueue("gfx", app.gfxQueue) || !createQueue("ml", app.mlQueue)) return false;
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

    // Depth inference runs on the headset GPU unless a second GPU was requested and
    // can be set up completely; any failure there falls back to one GPU.
    app.inferDevice = app.device;
    app.inferQueue = app.mlQueue;
    app.inferName = ad.Description;
    if (app.opt.depthAdapter != -1 && !InitSecondGpu(app, fac.Get()))
    {
        ReleaseSecondGpu(app);
        app.inferDevice = app.device;
        app.inferQueue = app.mlQueue;
        app.inferName = ad.Description;
        Log("InitD3D: second-GPU depth unavailable; depth runs on the headset GPU");
    }

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

static HWND FindCaptureWindow(const Options& opt)
{
    CaptureWindowSearch search;
    search.title = opt.windowTitle; search.executable = opt.executable;
    search.pid = opt.capturePid; search.hwnd = opt.captureHwnd; search.fullPath = opt.executablePath;
    if (!EnumWindows(EnumerateCaptureWindow, reinterpret_cast<LPARAM>(&search)))
    { Log("Capture selection: window enumeration failed (%lu)", GetLastError()); return nullptr; }
    for (const auto& match : search.matches)
        Log("Capture selection: HWND %p PID %lu exe '%s' title '%s'", (void*)match.hwnd,
            match.pid, winrt::to_string(match.executable).c_str(), winrt::to_string(match.title).c_str());
    if (search.matches.size() != 1)
    {
        Log("Capture selection: expected one game window, found %zu for exe '%s', title '%s'. Open the game or narrow --exe/--window; not capturing another source.",
            search.matches.size(), winrt::to_string(opt.executable).c_str(), winrt::to_string(opt.windowTitle).c_str());
        return nullptr;
    }
    return search.matches.front().hwnd;
}

// Pin the verified source before XR startup can change desktop focus/visibility.
// Retain the capture item, not just an HWND which Windows could later reuse.
static bool SelectCaptureItem(App& app)
{
    Capture& c = app.cap;
    if (c.item) return !c.closed.load();

    if (!wgc::GraphicsCaptureSession::IsSupported()) { Log("InitCaptureItem: FAIL Windows.Graphics.Capture not supported on this OS"); return false; }

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    HRESULT hr = E_FAIL;
    if (!app.opt.windowTitle.empty() || !app.opt.executable.empty())
    {
        const HWND hwnd = FindCaptureWindow(app.opt);
        if (!hwnd) return false;
        hr = interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(c.item));
        c.hwnd = hwnd;
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
    c.closedToken = c.item.Closed([&c](auto&&, auto&&) { c.closed = true; });
    c.closedRegistered = true;
    Log("Capture selection: retained capture item for startup");
    return true;
}

// Initializes the D3D11 side using the already selected capture item.
static bool InitCaptureItem(App& app)
{
    Log("InitCaptureItem: enter");
    Capture& c = app.cap;
    if (!SelectCaptureItem(app)) { Log("InitCaptureItem: FAIL source unavailable or closed during startup"); return false; }

    auto size = c.item.Size();
    if (size.Width <= 0 || size.Height <= 0) { Log("InitCaptureItem: FAIL item size %dx%d", size.Width, size.Height); return false; }
    app.srcW = size.Width;
    app.srcH = size.Height;
    c.frameW = size.Width;
    c.frameH = size.Height;
    RECT crop{};
    if (c.hwnd && QueryClientCrop(c.hwnd, size.Width, size.Height, crop))
    {
        // Present the game's client area only: a windowed game's frame includes a
        // 1 px border (a light line round the screen in the headset) and title bar.
        app.srcW = crop.right - crop.left;
        app.srcH = crop.bottom - crop.top;
        Log("InitCaptureItem: window %dx%d, client area %dx%d at (%ld,%ld) - frame and title bar excluded",
            size.Width, size.Height, app.srcW, app.srcH, crop.left, crop.top);
    }
    else if (c.hwnd) Log("InitCaptureItem: client area unavailable; capturing the whole window %dx%d", size.Width, size.Height);
    app.srcFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    // D3D11 device on the SAME adapter (shared handles do not cross adapters)
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(app.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &fl, 1, D3D11_SDK_VERSION, &c.dev, nullptr, &c.ctx);
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
        if (FAILED(c.dev->CreateRenderTargetView(c.tex11[i].Get(), nullptr, &c.rtv11[i])))
        { Log("StartCapture: FAIL capture render target %d", i); return false; }
    }
    if (FAILED(c.scaler.Init(c.dev.Get()))) { Log("StartCapture: FAIL resize shaders"); return false; }

    if (FAILED(app.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&app.captureFence)))) { Log("StartCapture: FAIL shared fence"); return false; }
    HANDLE fh = nullptr;
    HRESULT hr = app.device->CreateSharedHandle(app.captureFence.Get(), nullptr, GENERIC_ALL, nullptr, &fh);
    if (FAILED(hr)) { Log("StartCapture: FAIL CreateSharedHandle(fence) 0x%08X", (unsigned)hr); return false; }
    hr = dev5->OpenSharedFence(fh, IID_PPV_ARGS(&c.fence11));
    CloseHandle(fh);
    if (FAILED(hr)) { Log("StartCapture: FAIL OpenSharedFence 0x%08X", (unsigned)hr); return false; }

    winrt::Windows::Graphics::SizeInt32 size{ c.frameW, c.frameH };   // whole window, not the cropped source
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
    int poolW = c.frameW, poolH = c.frameH;
    uint64_t layout = 0;
    bool apartmentInitialized = false;
    try
    {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    apartmentInitialized = true;
    while (!app->stop.load())
    {
        if (c.closed.load()) { Log("CaptureMain: source closed; stopping playback"); app->stop = true; break; }
        auto frame = c.pool.TryGetNextFrame();
        if (!frame) { Sleep(1); continue; }

        auto cs = frame.ContentSize();
        if (cs.Width <= 0 || cs.Height <= 0) { Sleep(10); continue; }
        if (cs.Width != poolW || cs.Height != poolH)
        {
            Log("CaptureMain: resize %dx%d -> %dx%d; fitting into %dx%d", poolW, poolH,
                cs.Width, cs.Height, app->srcW, app->srcH);
            frame = nullptr;
            c.pool.Recreate(c.rtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, cs);
            poolW = cs.Width; poolH = cs.Height;
            ++layout;
            continue; // the old pool's surface may be clipped; wait for the new size
        }
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> tex;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&tex)));
        if (!tex) winrt::throw_hresult(E_POINTER);

        auto source = ReserveSource(*app);
        if (source)
        {
            // Window capture: crop every frame to the client area (it moves if the
            // window is restyled, resized or changes DPI). Whole frame if unknown.
            RECT crop{};
            const bool cropping = c.hwnd && QueryClientCrop(c.hwnd, cs.Width, cs.Height, crop);
            if (cropping != c.cropping || (cropping && !EqualRect(&crop, &c.crop)))
            {
                if (cropping) Log("CaptureMain: client area %ldx%ld at (%ld,%ld) of %dx%d frame", crop.right - crop.left,
                    crop.bottom - crop.top, crop.left, crop.top, cs.Width, cs.Height);
                else if (c.hwnd) Log("CaptureMain: client area unavailable; using the whole %dx%d frame", cs.Width, cs.Height);
                c.cropping = cropping; c.crop = crop;
            }
            winrt::check_hresult(c.scaler.Copy(c.dev.Get(), c.ctx.Get(), tex.Get(), cs.Width, cs.Height,
                c.tex11[source->index].Get(), c.rtv11[source->index].Get(), app->srcW, app->srcH, cropping ? &crop : nullptr));
            UINT64 v = ++app->captureFenceVal;
            if (FAILED(c.ctx4->Signal(c.fence11.Get(), v)))
            {
                Log("CaptureMain: source fence signal failed, stopping");
                app->stop = true;
                break;
            }
            c.ctx->Flush();
            source->layout = layout;
            app->sources.Publish(source, v, NowSeconds());
            c.frames++;
        }

    }
    }
    catch (const winrt::hresult_error& e) { Log("CaptureMain: failure 0x%08X; stopping playback", (unsigned)e.code().value); app->stop = true; }
    catch (const std::exception& e) { Log("CaptureMain: %s; stopping playback", e.what()); app->stop = true; }
    catch (...) { Log("CaptureMain: unexpected failure; stopping playback"); app->stop = true; }

    try {
        if (c.closedRegistered) { c.item.Closed(c.closedToken); c.closedRegistered = false; }
        if (c.session) c.session.Close(); if (c.pool) c.pool.Close();
    }
    catch (const winrt::hresult_error&) {}
    if (apartmentInitialized) winrt::uninit_apartment();
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
// the caller submits, then calls sources.Publish with the resulting fence value.
static SourceFrames::WriteRef RecordCpuSource(App& app, const std::vector<unsigned char>& rgb, int ringSlot)
{
    if (ringSlot < 0 || ringSlot >= RING) return {};
    if (rgb.size() != (size_t)app.srcW * app.srcH * 3) { Log("RecordCpuSource: bad picture size %zu", rgb.size()); return {}; }

    auto frame = ReserveSource(app);
    if (!frame) return {};

    PackRgba(rgb, app.srcW, app.srcH, app.srcUpMapped[ringSlot] + app.srcFootprint.Offset, app.srcFootprint.Footprint.RowPitch);

    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = app.srcTex[frame->index].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = app.srcUp[ringSlot].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = app.srcFootprint;
    app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);   // simultaneous-access: no barriers
    return frame;
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
    uint  fillMode;     // 0 stretch, 1 mirror (see FillHole in xr_common.h)
    float mirrorTol;
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
        float n = (doWarp != 0 || writeDepth != 0) ? NearAt(x, (int)y) : 0.0;
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

    // hole fill from the FARTHER neighbour (background side) - same as FillHole (CPU)
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

        int h;
        if (lastValid < 0 && rightValid < 0)
        {
            float xn = NearAt(x, (int)y);
            [loop] for (h = x; h < r; h++) { scratch[base + h] = (uint)x; scratch[N + base + h] = asuint(xn); }
        }
        else
        {
            bool useLeft = lastValid >= 0 && !(rightValid >= 0 && rightNear < lastNear);
            int anchor = useLeft ? lastValid : rightValid;
            float anchorNear = useLeft ? lastNear : rightNear;

            if (fillMode == 0)
            {
                [loop] for (h = x; h < r; h++) { scratch[base + h] = (uint)anchor; scratch[N + base + h] = asuint(anchorNear); }
            }
            else
            {
                // reflect the background outward from the anchor; hold the last good
                // pixel if the reflection reaches something nearer than the anchor
                int good = anchor;
                float goodNear = anchorNear;
                int n = r - x;
                [loop] for (int k = 1; k <= n; k++)
                {
                    h = useLeft ? (x - 1 + k) : (r - k);
                    int cand = clamp(useLeft ? anchor - k : anchor + k, 0, iw - 1);
                    float cn = NearAt(cand, (int)y);
                    if (cn <= anchorNear + mirrorTol) { good = cand; goodNear = cn; }
                    scratch[base + h] = (uint)good;
                    scratch[N + base + h] = asuint(goodNear);
                }
            }
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
// NCHW float at the model's input size, box-filtered down with taps x taps bilinear
// taps, ImageNet-normalised unless the model normalises internally.
static const char* kPrepHlsl = R"HLSL(
cbuffer C : register(b0)
{
    uint DW; uint DH; uint taps; uint exactLoad;
    float cropX; float cropY; float cropSize; uint normalize;
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
                v += scene.SampleLevel(samp, float2(cropX, cropY) + uv * cropSize, 0).rgb;
            }
        }
        v /= (float)(taps * taps);
    }

    const float3 mean = float3(0.485, 0.456, 0.406);
    const float3 istd = float3(1.0 / 0.229, 1.0 / 0.224, 1.0 / 0.225);
    float3 o = normalize != 0 ? (v - mean) * istd : v;
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
static bool LoadModel(App& app);

// One-time ONNX Runtime + DirectML setup. The model itself is loaded by LoadModel,
// which the worker can call again to switch models during playback.
static bool InitModel(App& app)
{
    Log("InitModel: enter");

    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ort) { Log("InitModel: FAIL no OrtApi"); return false; }
    if (OrtStatus* st = ort->GetExecutionProviderApi("DML", ORT_API_VERSION, (const void**)&app.dmlApi)) { Fail("GetExecutionProviderApi(DML)", st); return false; }
    if (!app.dmlApi) { Log("InitModel: FAIL null OrtDmlApi"); return false; }

    ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "xrapp5", &app.env);

    HMODULE dmllib = LoadLibraryW(L"DirectML.dll");
    if (!dmllib) { Log("InitModel: FAIL DirectML.dll"); return false; }
    auto createDmlDevice = (HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**))
        GetProcAddress(dmllib, "DMLCreateDevice");
    if (!createDmlDevice) { Log("InitModel: FAIL DMLCreateDevice export"); return false; }
    if (FAILED(createDmlDevice(app.inferDevice.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&app.dmlDevice)))) { Log("InitModel: FAIL DMLCreateDevice"); return false; }
    Log("InitModel: DirectML on %ls%s", app.inferName.c_str(), app.secondGpu ? " (second GPU)" : "");

    const bool ok = LoadModel(app);
    Log("InitModel: exit %s", ok ? "ok" : "FAIL");
    return ok;
}

// Releases the current model's session and input buffer. Caller guarantees no
// ml-queue work that uses them is still in flight (see SwitchModel).
static void ReleaseModel(App& app)
{
    Log("ReleaseModel: enter (%s)", app.opt.model ? app.opt.model->name : "none");
    if (app.modelInValue) { ort->ReleaseValue(app.modelInValue); app.modelInValue = nullptr; }
    if (app.dmlAlloc && app.dmlApi)
    {
        if (OrtStatus* st = app.dmlApi->FreeGPUAllocation(app.dmlAlloc)) { Fail("FreeGPUAllocation", st); ort->ReleaseStatus(st); }
        app.dmlAlloc = nullptr;
    }
    if (app.ortSession) { ort->ReleaseSession(app.ortSession); app.ortSession = nullptr; }
    app.modelIn.Reset();
    app.prepOut.Reset();
    Log("ReleaseModel: exit");
}

// Loads app.opt.model: ORT session on the shared device/ml queue, GPU input buffer,
// output resample tables.
static bool LoadModel(App& app)
{
    if (!app.opt.model || !app.env || !app.dmlApi || !app.dmlDevice) { Log("LoadModel: FAIL not initialised"); return false; }
    const ModelSpec& spec = *app.opt.model;
    Log("LoadModel: enter (%s)", spec.name);

    OrtSessionOptions* so = nullptr;
    if (OrtStatus* st = ort->CreateSessionOptions(&so)) { Fail("CreateSessionOptions", st); return false; }
    if (OrtStatus* st = app.dmlApi->SessionOptionsAppendExecutionProvider_DML1(so, app.dmlDevice.Get(), app.inferQueue.Get()))
    { Fail("AppendExecutionProvider_DML1", st); ort->ReleaseSessionOptions(so); return false; }

    std::wstring modelPath = ModelPath(spec.file).wstring();
    Log("LoadModel: loading %s (%s, input %dx%d '%s' -> '%s', %s)", winrt::to_string(modelPath).c_str(), spec.name,
        spec.inW, spec.inH, spec.inName, spec.outName, spec.imagenetNorm ? "ImageNet-normalised" : "RGB 0..1");
    OrtStatus* created = ort->CreateSession(app.env, modelPath.c_str(), so, &app.ortSession);
    ort->ReleaseSessionOptions(so);
    if (created) { Fail("CreateSession", created); app.ortSession = nullptr; return false; }

    const OrtMemoryInfo* inInfos[1] = { nullptr };
    if (OrtStatus* st = ort->SessionGetMemoryInfoForInputs(app.ortSession, inInfos, 1)) { Fail("SessionGetMemoryInfoForInputs", st); return false; }

    // Model input: a VRAM buffer the prep shader writes and DirectML reads - the
    // picture never visits the CPU. (M1: a resource handed to DML needs UAV access.)
    const UINT64 modelBytes = (UINT64)3 * spec.inH * spec.inW * sizeof(float);
    int64_t modelDims[4] = { 1, 3, spec.inH, spec.inW };
    if (!MakeBuffer(app.inferDevice.Get(), D3D12_HEAP_TYPE_DEFAULT, modelBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.modelIn, nullptr)) return false;
    if (!app.secondGpu) app.prepOut = app.modelIn;
    else if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_DEFAULT, modelBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.prepOut, nullptr)) return false;
    if (app.secondGpu && modelBytes > app.crossBytes) { Log("LoadModel: FAIL input %llu bytes exceeds the hand-over buffer", (unsigned long long)modelBytes); return false; }

    if (OrtStatus* st = app.dmlApi->CreateGPUAllocationFromD3DResource(app.modelIn.Get(), &app.dmlAlloc)) { Fail("CreateGPUAllocationFromD3DResource", st); return false; }
    if (OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(const_cast<OrtMemoryInfo*>(inInfos[0]), app.dmlAlloc, modelBytes,
                                                            modelDims, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &app.modelInValue))
    { Fail("CreateTensorWithDataAsOrtValue", st); return false; }

    app.modelOut.assign((size_t)spec.inW * spec.inH, 0.f);
    app.rawDepth.resize((size_t)W * H);

    // Pixel-centre bilinear taps (same convention as SampleDepth), computed once so
    // the per-pass resample is a table walk rather than 269k clamped lookups.
    auto taps = [](int src, int dst) {
        std::vector<App::ResampleTap> t(dst);
        for (int i = 0; i < dst; i++)
        {
            const float p = std::clamp((i + .5f) / dst * src - .5f, 0.f, float(src - 1));
            const int i0 = int(p);
            t[i] = { i0, std::min(i0 + 1, src - 1), p - i0 };
        }
        return t;
    };
    app.resampleX = taps(spec.inW, W);
    app.resampleY = taps(spec.inH, H);
    Log("LoadModel: exit ok, %s %dx%d on the shared device via the ml queue (GPU-resident input)%s", spec.name,
        spec.inW, spec.inH, (spec.inW == W && spec.inH == H) ? "" : ", output resampled to the depth grid");
    return true;
}

// Records + submits the prep shader on the ml queue: source texture -> model input.
// GPU-side ordering only: the queue waits for the source's fence, and ORT's own
// submissions to the same queue follow it. Returns the ml fence value signalled
// after the dispatch (callers other than the self-test need not wait on it).
static void WaitInferFence(App& app, UINT64 value);

static UINT64 SubmitPrep(App& app, UINT srcDescIndex, int srcW, int srcH, ID3D12Fence* srcFence, UINT64 srcValue,
    const DepthCrop& crop = {})
{
    if (srcW <= 0 || srcH <= 0) return 0;

    const ModelSpec& spec = *app.opt.model;
    PrepConstants pc{};
    pc.dw = (uint32_t)spec.inW; pc.dh = (uint32_t)spec.inH;
    pc.exactLoad = (srcW == spec.inW && srcH == spec.inH && crop.size == 1 && crop.x == 0 && crop.y == 0) ? 1u : 0u;
    pc.cropX = crop.x; pc.cropY = crop.y; pc.cropSize = crop.size;
    pc.taps = (uint32_t)std::clamp(int(std::ceil(srcW * crop.size / spec.inW)), 1, 4);
    pc.normalize = spec.imagenetNorm ? 1u : 0u;

    app.mlCmdAlloc->Reset();
    app.mlCmdList->Reset(app.mlCmdAlloc.Get(), nullptr);
    ID3D12GraphicsCommandList* cl = app.mlCmdList.Get();
    ID3D12DescriptorHeap* heaps[] = { app.descHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(app.prepRootSig.Get());
    cl->SetPipelineState(app.prepPso.Get());
    cl->SetComputeRoot32BitConstants(0, sizeof(PrepConstants) / 4, &pc, 0);
    cl->SetComputeRootDescriptorTable(1, GpuDesc(app, srcDescIndex));
    cl->SetComputeRootUnorderedAccessView(2, app.prepOut->GetGPUVirtualAddress());
    cl->Dispatch((spec.inW + 7) / 8, (spec.inH + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = app.prepOut.Get();
    cl->ResourceBarrier(1, &uav);

    const UINT64 inputBytes = (UINT64)3 * spec.inW * spec.inH * sizeof(float);
    if (app.secondGpu)
    {
        // The previous hand-over must have left the shared buffer before it is
        // overwritten. Inference runs after it on the same queue and has returned
        // by now, so this is normally already complete.
        WaitInferFence(app, app.inferFenceVal);
        Transition(cl, app.prepOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyBufferRegion(app.crossBuf.Get(), 0, app.prepOut.Get(), 0, inputBytes);
        Transition(cl, app.prepOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    cl->Close();

    if (srcFence) app.mlQueue->Wait(srcFence, srcValue);
    ID3D12CommandList* lists[] = { cl };
    app.mlQueue->ExecuteCommandLists(1, lists);
    app.mlQueue->Signal(app.mlFence.Get(), ++app.mlFenceVal);
    if (!app.secondGpu) return app.mlFenceVal;

    // Inference GPU: wait (on the GPU) for the hand-over, copy it into the model's
    // input, then ORT's own work follows on the same queue.
    app.mlQueue->Signal(app.crossFence.Get(), ++app.crossVal);
    app.inferCmdAlloc->Reset();
    app.inferCmdList->Reset(app.inferCmdAlloc.Get(), nullptr);
    ID3D12GraphicsCommandList* il = app.inferCmdList.Get();
    Transition(il, app.modelIn.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    il->CopyBufferRegion(app.modelIn.Get(), 0, app.crossBufInfer.Get(), 0, inputBytes);
    Transition(il, app.modelIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    il->Close();
    app.inferQueue->Wait(app.crossFenceInfer.Get(), app.crossVal);
    ID3D12CommandList* inferLists[] = { il };
    app.inferQueue->ExecuteCommandLists(1, inferLists);
    app.inferQueue->Signal(app.inferFence.Get(), ++app.inferFenceVal);
    return app.mlFenceVal;
}

static void WaitInferFence(App& app, UINT64 value)
{
    if (!app.secondGpu || value == 0 || !app.inferFence) return;
    if (app.inferFence->GetCompletedValue() >= value) return;

    app.inferFence->SetEventOnCompletion(value, app.inferFenceEvent);
    WaitForSingleObject(app.inferFenceEvent, INFINITE);
}

static void WaitMlFence(App& app, UINT64 value);

// All submitted depth GPU work (prep, and on a second GPU the hand-over) complete.
static void WaitDepthIdle(App& app)
{
    if (app.mlFence && app.mlFenceVal) WaitMlFence(app, app.mlFenceVal);
    WaitInferFence(app, app.inferFenceVal);
}

static void WaitMlFence(App& app, UINT64 value)
{
    if (value == 0) return;
    if (app.mlFence->GetCompletedValue() >= value) return;

    app.mlFence->SetEventOnCompletion(value, app.mlFenceEvent);
    WaitForSingleObject(app.mlFenceEvent, INFINITE);
}

// modelOut (model size) -> rawDepth (W x H grid), pixel-centre bilinear via the
// tap tables built in InitModel. Verified against SampleDepth by SelfTestResample.
static void ResampleToGrid(App& app)
{
    const ModelSpec& spec = *app.opt.model;
    if (app.modelOut.size() != (size_t)spec.inW * spec.inH) return;
    if (app.rawDepth.size() != (size_t)W * H) return;
    if (app.resampleX.size() != (size_t)W || app.resampleY.size() != (size_t)H) return;

    const float* src = app.modelOut.data();
    for (int y = 0; y < H; y++)
    {
        const App::ResampleTap ty = app.resampleY[y];
        const float* r0 = src + (size_t)ty.i0 * spec.inW;
        const float* r1 = src + (size_t)ty.i1 * spec.inW;
        float* out = app.rawDepth.data() + (size_t)y * W;
        for (int x = 0; x < W; x++)
        {
            const App::ResampleTap tx = app.resampleX[x];
            const float top = r0[tx.i0] + (r0[tx.i1] - r0[tx.i0]) * tx.t;
            const float bot = r1[tx.i0] + (r1[tx.i1] - r1[tx.i0]) * tx.t;
            out[x] = top + (bot - top) * ty.t;
        }
    }
}

// Runs the model on whatever the prep shader put in the input buffer.
static bool RunModelRaw(App& app)
{
    if (!app.ortSession || !app.modelInValue) return false;

    const ModelSpec& spec = *app.opt.model;
    OrtValue* out = nullptr;
    const char* inName = spec.inName;
    const char* outName = spec.outName;
    if (OrtStatus* st = ort->Run(app.ortSession, nullptr, &inName, (const OrtValue* const*)&app.modelInValue, 1, &outName, 1, &out))
    { Fail("Run", st); ort->ReleaseStatus(st); return false; }

    // The output is a CPU tensor, so ORT has already synchronised with the GPU.
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (OrtStatus* st = ort->GetTensorTypeAndShape(out, &info))
    { Fail("GetTensorTypeAndShape", st); ort->ReleaseStatus(st); ort->ReleaseValue(out); return false; }
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    size_t count = 0;
    OrtStatus* metadataError = ort->GetTensorElementType(info, &type);
    if (!metadataError) metadataError = ort->GetTensorShapeElementCount(info, &count);
    ort->ReleaseTensorTypeAndShapeInfo(info);
    if (metadataError || type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || count != app.modelOut.size())
    {
        if (metadataError) { Fail("depth metadata", metadataError); ort->ReleaseStatus(metadataError); }
        else Log("RunModelRaw: incompatible depth output (type %d, elements %zu)", (int)type, count);
        ort->ReleaseValue(out); return false;
    }
    float* dp = nullptr;
    if (OrtStatus* st = ort->GetTensorMutableData(out, (void**)&dp))
    { Fail("GetTensorMutableData", st); ort->ReleaseStatus(st); ort->ReleaseValue(out); return false; }
    if (!dp) { ort->ReleaseValue(out); Log("RunModelRaw: null output"); return false; }
    memcpy(app.modelOut.data(), dp, app.modelOut.size() * sizeof(float));
    ort->ReleaseValue(out);
    for (float v : app.modelOut)
        if (!std::isfinite(v)) { Log("RunModelRaw: non-finite depth output"); return false; }

    // Resample to the renderer's W x H depth grid (pixel-centre bilinear). For the
    // native 686x392 model this is a plain copy, so its path is unchanged.
    if (spec.inW == W && spec.inH == H)
    {
        app.rawDepth = app.modelOut;
        return true;
    }
    ResampleToGrid(app);
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
    for (float v : raw) hist[std::clamp((int)((v - dmin) * k), 0, BINS - 1)]++;

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
static void PublishDepth(App& app, const SourceRef& src, const std::vector<float>& depth,
    float lo, float hi, double modelMs)
{
    auto preparedDepth = depth;
    DepthSlot& s = app.slots[app.writeSlot];
    if (app.opt.source == SourceKind::Synthetic) RegionMeans(preparedDepth, src->time, s.back, s.panel, s.marker);
    if (app.opt.useTruth) MakeTruth(preparedDepth, src->time);
    // Worker thread: opt.model is the model that produced this depth.
    const int dilateH = app.opt.dilate >= 0 ? app.opt.dilate : app.opt.model->dilateH;
    const int dilateV = app.opt.dilateV >= 0 ? app.opt.dilateV : app.opt.model->dilateV;
    DilateNear(preparedDepth, W, H, dilateH, dilateV);
    memcpy(s.nearMapped, preparedDepth.data(), preparedDepth.size() * sizeof(float));
    s.source = src; s.sceneTime = src->time; s.modelMs = modelMs;
    s.lo = lo; s.hi = hi; s.completeTime = NowSeconds();
    app.writeSlot = app.readySlot.exchange(app.writeSlot | SLOT_FRESH) & (SLOT_FRESH - 1);
    app.depthPublished++; app.depthHealthy = true;
}

static bool ComputeAndPublish(App& app, const SourceRef& src, std::vector<float>& nearScratch)
{
    if (!src) return false;
    if (app.testDepthFailuresLeft > 0)
    {
        --app.testDepthFailuresLeft;
        Log("ComputeAndPublish: injected depth failure (%d remaining)", app.testDepthFailuresLeft);
        return false;
    }

    auto m0 = std::chrono::steady_clock::now();
    UINT64 prepDone = SubmitPrep(app, DESC_SRC0 + (UINT)src->index, app.srcW, app.srcH, SourceFence(app), src->value);
    app.sources.MarkRead(src, SourceFrames::Reader::Model, prepDone);
    if (!RunModelRaw(app)) return false;
    double modelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m0).count();

    float lo = 0, hi = 1;
    SmoothRange(app.smoother, app.rawDepth, NowSeconds(), app.opt.smooth, app.opt.smoothTau, &lo, &hi);
    const float inv = (hi - lo) > 1e-6f ? 1.0f / (hi - lo) : 0.0f;
    nearScratch.resize(app.rawDepth.size());
    for (size_t i = 0; i < nearScratch.size(); i++)
        nearScratch[i] = std::min(1.0f, std::max(0.0f, (app.rawDepth[i] - lo) * inv));

    // Make global depth available immediately; an optional crop must never hold
    // back this result or mutate a slot the render thread may be reading.
    PublishDepth(app, src, nearScratch, lo, hi, modelMs);
    if (!app.foregroundEnabled.load() || app.opt.useTruth)
    { app.foregroundTracker.Reset(); return true; }
    DepthCrop crop;
    if (app.foregroundLayout != src->layout)
    { app.foregroundTracker.Reset(); app.foregroundLayout = src->layout; }
    const double now = NowSeconds();
    const bool persistent = app.foregroundTracker.Update(nearScratch, W, H, src->time, crop);
    if (!persistent || app.stop.load() || !app.foregroundBudget.CanRun(now, (now-src->time)*1000, modelMs)) return true;
    const double begin = NowSeconds();
    ++app.foregroundAttempts;
    prepDone = SubmitPrep(app, DESC_SRC0 + (UINT)src->index, app.srcW, app.srcH, SourceFence(app), src->value, crop);
    app.sources.MarkRead(src, SourceFrames::Reader::Model, prepDone);
    const bool good = RunModelRaw(app);
    // A failed optional pass must still retire GPU prep before the next dispatch.
    WaitDepthIdle(app);
    const double costMs = (NowSeconds()-begin)*1000;
    app.foregroundBudget.Complete(NowSeconds(), costMs);
    if (good && app.foregroundEnabled.load() && !app.stop.load() && (NowSeconds()-src->time)<.20 &&
        FuseForeground(nearScratch, app.rawDepth, W, H, crop))
    {
        PublishDepth(app, src, nearScratch, lo, hi, modelMs+costMs);
        ++app.foregroundAccepted;
    }
    if (NowSeconds()>=app.foregroundLogTime)
    {
        Log("Foreground refinement: %u accepted / %u attempts; last crop %.2f %.2f size %.2f, extra %.1f ms",
            app.foregroundAccepted, app.foregroundAttempts, crop.x, crop.y, crop.size, costMs);
        app.foregroundLogTime=NowSeconds()+5;
    }
    return true;
}

// Worker thread only, between passes: replace the depth model. Depth pauses for the
// load (~1-2 s); colour keeps presenting, and the stale-depth guard shows it flat
// meanwhile. On failure the previous model is restored.
static bool SwitchModel(App& app, const ModelSpec* next)
{
    if (!next || next == app.opt.model) return true;
    const ModelSpec* previous = app.opt.model;
    Log("SwitchModel: enter (%s -> %s)", previous ? previous->name : "none", next->name);
    const double t0 = NowSeconds();

    WaitDepthIdle(app);                      // no prep, hand-over or inference may still use the old buffers
    ReleaseModel(app);
    app.opt.model = next;
    bool ok = LoadModel(app);
    if (!ok)
    {
        Log("SwitchModel: %s failed to load; restoring %s", next->name, previous ? previous->name : "none");
        ReleaseModel(app);
        app.opt.model = previous;
        if (!previous || !LoadModel(app)) { Log("SwitchModel: exit FAIL (no model loaded)"); return false; }
    }
    // Output ranges differ completely between models (e.g. 0..6 vs 0..0.17), so the
    // smoothed normalisation and the foreground tracker must start again.
    app.smoother.have = false;
    app.foregroundTracker.Reset();
    Log("SwitchModel: exit %s, now %s (%.0f ms)", ok ? "ok" : "restored", app.opt.model->name, (NowSeconds() - t0) * 1000);
    return true;
}

static void WorkerMain(App* app)
{
    g_threadName = "worker";
    if (!app) { Log("WorkerMain: null app"); return; }
    Log("WorkerMain: enter");

    std::vector<float> nearScratch;
    uint64_t runs = 0, lastSeq = 0;
    int failures = 0;
    try
    {
    while (!app->stop.load())
    {
        const ModelSpec* wanted = app->requestedModel.load();
        if (wanted && wanted != app->opt.model && !SwitchModel(*app, wanted))
        { app->depthHealthy = false; Log("WorkerMain: no depth model; flat viewing"); break; }

        SourceRef src = app->sources.Latest();
        // A captured desktop that is not changing delivers no new frames - do not
        // burn the GPU recomputing identical depth. (Image/synthetic keep running:
        // they double as the throughput benchmark.)
        if (!src || (app->opt.source == SourceKind::Capture && src->seq == lastSeq)) { Sleep(2); continue; }
        if (!ComputeAndPublish(*app, src, nearScratch))
        {
            app->depthHealthy = false;
            app->smoother.have = false;
            app->foregroundTracker.Reset();
            ++failures;
            Log("WorkerMain: depth failed (%d/3); flat viewing%s", failures,
                failures < 3 ? ", retrying in 1 second" : ", restart playback to retry");
            if (failures >= 3) break;
            // Run may have failed after queuing prep; do not reset its allocator
            // until that dispatch is complete, including on retry of a static frame.
            WaitDepthIdle(*app);
            for (int i = 0; i < 50 && !app->stop.load(); ++i) Sleep(20);
            continue;
        }
        if (failures) Log("WorkerMain: depth recovered");
        failures = 0;
        lastSeq = src->seq;
        runs++;
    }
    }
    catch (const std::exception& e) { app->depthHealthy = false; Log("WorkerMain: %s; flat viewing", e.what()); }
    catch (...) { app->depthHealthy = false; Log("WorkerMain: unexpected failure; flat viewing"); }
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
    Log("SelfTestWarp[%s]: enter (doWarp %u fill %u scaleFocal %.2f eyes %.4f/%.4f)", name, c.doWarp, c.fillMode, c.scaleFocal, c.eye0, c.eye1);

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
        WarpEyeFill(scene, near01, eyes[e], c.scaleFocal, 1.0f, c.invZNear, c.invZFar, c.nearZ, c.farZ, c.doWarp != 0, (int)c.fillMode, refColor.data(), refDepth.data());
        for (int y = 0; y < H; y++)
        {
            const unsigned char* grow = cp + cfp[e].Offset + (size_t)y * cfp[e].Footprint.RowPitch;
            const float* gdrow = (const float*)((const unsigned char*)dp + dfp[e].Offset + (size_t)y * dfp[e].Footprint.RowPitch);
            const unsigned char* rrow = refColor.data() + (size_t)y * ROW_PITCH;
            const float* rdrow = refDepth.data() + (size_t)y * (ROW_PITCH / 4);
            for (int x = 0; x < W; x++)
            {
                if (memcmp(grow + x * 4, rrow + x * 4, 4) != 0) badColor++;
                if (c.writeDepth && fabsf(gdrow[x] - rdrow[x]) > 1e-5f) badDepth++;
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
static bool SelfTestPrep(App& app, const std::vector<unsigned char>& scene, const DepthCrop& crop = {})
{
    Log("SelfTestPrep: enter (crop %.2f %.2f size %.2f)", crop.x, crop.y, crop.size);
    if (!UploadTestSource(app, scene)) return false;

    UINT64 v = SubmitPrep(app, DESC_TEST_SRC, W, H, nullptr, 0, crop);
    WaitMlFence(app, v);

    // The test source is W x H; the model input may be a different size.
    const ModelSpec& spec = *app.opt.model;
    const int MW = spec.inW, MH = spec.inH;
    const UINT64 bytes = (UINT64)3 * MW * MH * sizeof(float);
    ComPtr<ID3D12Resource> rb;
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb, nullptr)) return false;
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    Transition(app.cmdList.Get(), app.prepOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    app.cmdList->CopyBufferRegion(rb.Get(), 0, app.prepOut.Get(), 0, bytes);
    Transition(app.cmdList.Get(), app.prepOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    app.cmdList->Close();
    WaitFence(app, SubmitAndSignal(app));

    float* gp = nullptr;
    if (FAILED(rb->Map(0, nullptr, (void**)&gp)) || !gp) { Log("SelfTestPrep: FAIL map readback"); return false; }
    const float mean[3] = { 0.485f, 0.456f, 0.406f };
    const float istd[3] = { 1.f / 0.229f, 1.f / 0.224f, 1.f / 0.225f };
    size_t bad = 0;
    float worst = 0;
    // Mirror SubmitPrep's choice of path exactly: direct Load, or taps x taps
    // bilinear samples averaged.
    const bool exact = (MW == W && MH == H && crop.size == 1 && crop.x == 0 && crop.y == 0);
    const int taps = std::clamp(int(std::ceil(W * crop.size / MW)), 1, 4);
    std::vector<float> channel(W*H);
    for (int c = 0; c < 3; c++)
    {
        for (size_t i=0; i<channel.size(); ++i) channel[i]=scene[i*3+c]/255.f;
        for (int y = 0; y < MH; y++)
        {
            for (int x = 0; x < MW; x++)
            {
                float pixel = 0;
                if (exact) pixel = channel[y*W+x];
                else
                {
                    for (int j = 0; j < taps; j++)
                        for (int i = 0; i < taps; i++)
                            pixel += SampleDepth(channel, W, H,
                                crop.x + (x + (i + .5f) / taps) / MW * crop.size,
                                crop.y + (y + (j + .5f) / taps) / MH * crop.size);
                    pixel /= float(taps * taps);
                }
                float ref = spec.imagenetNorm ? (pixel - mean[c]) * istd[c] : pixel;
                // Compare in 0..1 pixel units so one tolerance fits both normalisations.
                float d = fabsf(gp[((size_t)c * MH + y) * MW + x] - ref) / (spec.imagenetNorm ? istd[c] : 1.f);
                worst = std::max(worst, d);
                // Sampled path: GPU bilinear weights are fixed-point (~1/256).
                if (d > (exact ? 3e-6f : .0028f)) bad++;
            }
        }
    }
    rb->Unmap(0, nullptr);

    bool ok = bad == 0;
    Log("SelfTestPrep: exit %s - %zu of %zu values differ from the CPU preprocessing (%dx%d, %d taps, worst |diff| %.2e px)",
        ok ? "PASS" : "FAIL", bad, (size_t)3 * MW * MH, MW, MH, exact ? 0 : taps, worst);
    return ok;
}

// Exercise actual model inference on both full and cropped GPU input without
// relying on headset visibility. Alignment may legitimately reject this fixture.
static bool SelfTestForegroundModel(App& app)
{
    std::vector<unsigned char> scene;
    MakeScene(scene, 1.0);
    if (!UploadTestSource(app, scene)) return false;
    SubmitPrep(app, DESC_TEST_SRC, W, H, nullptr, 0);
    if (!RunModelRaw(app)) return false;
    auto base = app.rawDepth;
    const auto range = std::minmax_element(base.begin(), base.end());
    const float lo=*range.first, span=*range.second-lo;
    if (span<1e-6f) return false;
    for (auto& v:base) v=(v-lo)/span;
    const DepthCrop crop{.15f,.15f,.65f};
    SubmitPrep(app, DESC_TEST_SRC, W, H, nullptr, 0, crop);
    if (!RunModelRaw(app)) return false;
    const bool fused=FuseForeground(base,app.rawDepth,W,H,crop);
    Log("SelfTestForegroundModel: PASS full/crop inference finite; alignment %s", fused ? "accepted" : "safely rejected");
    return true;
}

// The table-driven model-output resample vs the reference SampleDepth, on a
// deterministic non-smooth pattern (a smooth one would hide tap/offset mistakes).
// Second GPU only: after one prep + hand-over, the model input on the inference
// GPU must be byte-identical to what the prep shader wrote on the headset GPU.
static bool SelfTestTransfer(App& app, const std::vector<unsigned char>& scene)
{
    if (!app.secondGpu) return true;
    Log("SelfTestTransfer: enter (%ls)", app.inferName.c_str());
    if (!UploadTestSource(app, scene)) { Log("SelfTestTransfer: FAIL test source upload"); return false; }

    const ModelSpec& spec = *app.opt.model;
    const UINT64 bytes = (UINT64)3 * spec.inW * spec.inH * sizeof(float);
    const double t0 = NowSeconds();
    SubmitPrep(app, DESC_TEST_SRC, W, H, nullptr, 0);
    WaitDepthIdle(app);
    const double handOverMs = (NowSeconds() - t0) * 1000;

    ComPtr<ID3D12Resource> rbPrep, rbModel;
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rbPrep, nullptr) ||
        !MakeBuffer(app.inferDevice.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rbModel, nullptr))
    { Log("SelfTestTransfer: FAIL readback buffers"); return false; }

    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    Transition(app.cmdList.Get(), app.prepOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    app.cmdList->CopyBufferRegion(rbPrep.Get(), 0, app.prepOut.Get(), 0, bytes);
    Transition(app.cmdList.Get(), app.prepOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    app.cmdList->Close();
    WaitFence(app, SubmitAndSignal(app));

    app.inferCmdAlloc->Reset();
    app.inferCmdList->Reset(app.inferCmdAlloc.Get(), nullptr);
    ID3D12GraphicsCommandList* il = app.inferCmdList.Get();
    Transition(il, app.modelIn.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    il->CopyBufferRegion(rbModel.Get(), 0, app.modelIn.Get(), 0, bytes);
    Transition(il, app.modelIn.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    il->Close();
    ID3D12CommandList* lists[] = { il };
    app.inferQueue->ExecuteCommandLists(1, lists);
    app.inferQueue->Signal(app.inferFence.Get(), ++app.inferFenceVal);
    WaitInferFence(app, app.inferFenceVal);

    const unsigned char* a = nullptr;
    const unsigned char* b = nullptr;
    if (FAILED(rbPrep->Map(0, nullptr, (void**)&a)) || FAILED(rbModel->Map(0, nullptr, (void**)&b)) || !a || !b)
    { Log("SelfTestTransfer: FAIL map readback"); return false; }
    size_t differ = 0;
    for (UINT64 i = 0; i < bytes; i += 4)
        if (memcmp(a + i, b + i, 4) != 0) ++differ;
    rbPrep->Unmap(0, nullptr);
    rbModel->Unmap(0, nullptr);

    const bool ok = differ == 0;
    Log("SelfTestTransfer: exit %s - %zu of %llu input values differ between GPUs (prep + hand-over %.2f ms, first run)",
        ok ? "PASS" : "FAIL", differ, (unsigned long long)(bytes / 4), handOverMs);
    return ok;
}

static bool SelfTestResample(App& app)
{
    const ModelSpec& spec = *app.opt.model;
    Log("SelfTestResample: enter (%dx%d -> %dx%d)", spec.inW, spec.inH, W, H);
    if (spec.inW == W && spec.inH == H) { Log("SelfTestResample: exit PASS (same size - plain copy)"); return true; }

    const std::vector<float> saved = app.modelOut;
    for (size_t i = 0; i < app.modelOut.size(); i++)
        app.modelOut[i] = float((i * 2654435761u) % 1000u) / 1000.f;
    ResampleToGrid(app);

    float worst = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            const float ref = SampleDepth(app.modelOut, spec.inW, spec.inH, (x + .5f) / W, (y + .5f) / H);
            worst = std::max(worst, std::abs(app.rawDepth[(size_t)y * W + x] - ref));
        }
    app.modelOut = saved;

    const bool ok = worst < 1e-5f;
    Log("SelfTestResample: exit %s (worst |diff| %.2e vs SampleDepth)", ok ? "PASS" : "FAIL", worst);
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
    ok = SelfTestResample(app) && ok;
    ok = SelfTestTransfer(app, synth) && ok;
    ok = SelfTestPrep(app, synth, DepthCrop{.17f,.23f,.5f}) && ok;
    c.mirrorTol = MIRROR_TOL;
    for (uint32_t mode : { (uint32_t)FILL_STRETCH, (uint32_t)FILL_MIRROR })
    {
        const char* mn = mode == (uint32_t)FILL_MIRROR ? "mirror" : "stretch";
        char name[64];
        WarpConstants m = c;
        m.fillMode = mode;
        snprintf(name, sizeof(name), "%s synthetic+truth", mn);
        ok = SelfTestWarp(app, name, synth, truth, m) && ok;
        snprintf(name, sizeof(name), "%s synthetic+ramp", mn);
        ok = SelfTestWarp(app, name, synth, ramp, m) && ok;

        WarpConstants anchored = m;
        anchored.invZNear -= 1.0f / ScreenAnchor::distance;
        anchored.invZFar -= 1.0f / ScreenAnchor::distance;
        anchored.writeDepth = 0;
        snprintf(name, sizeof(name), "%s anchored stereo", mn);
        ok = SelfTestWarp(app, name, synth, ramp, anchored) && ok;

        // large disparity: wide holes, so the mirror walks far and meets its guard
        WarpConstants big = m;
        big.scaleFocal *= 4.0f;
        snprintf(name, sizeof(name), "%s synthetic+truth x4", mn);
        ok = SelfTestWarp(app, name, synth, truth, big) && ok;
        snprintf(name, sizeof(name), "%s synthetic+ramp x4", mn);
        ok = SelfTestWarp(app, name, synth, ramp, big) && ok;
    }
    WarpConstants off = c;
    off.doWarp = 0;
    ok = SelfTestWarp(app, "no-warp", synth, truth, off) && ok;
    off.writeDepth = 0;
    std::vector<float> invalidDepth((size_t)W * H, std::numeric_limits<float>::quiet_NaN());
    ok = SelfTestWarp(app, "flat-fallback-invalid-depth", synth, invalidDepth, off) && ok;

    Log("SelfTest: exit %s", ok ? "ALL PASS" : "FAILED");
    return ok;
}

// -------------------------------------------------------------- frame loop
static void RunFrameLoop(App& app)
{
    Log("RunFrameLoop: enter (%.0fs)%s%s", app.opt.runSeconds, app.opt.doWarp ? "" : " [warp disabled]",
        app.opt.paired ? " [paired colour+depth]" : " [latest colour + latest completed depth]");

    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, exitLoop = false, printedView = false;
    bool trackingWasValid = true, wasStereo = false;
    uint64_t invalidTrackingFrames = 0;
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
    double dashboardVisibleSince = -1.0;
    bool dashboardStartupAttempted = false, dashboardCloseRequested = false;
    bool dashboardKeyWasDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    long long lastPhase = -1;
    bool depthOn = useDepthSc;
    XrPosef frozenPose[VIEWS] = {};
    ScreenAnchor screen;
    // Poll without taking keyboard focus or consuming game input. Held keys
    // trigger once; a request survives temporary tracking loss.
    const SHORT equalsMapping = VkKeyScanW(L'=');
    int recenterKey = equalsMapping == -1 ? VK_OEM_PLUS : LOBYTE(equalsMapping);
    int menuKey = VK_F8;
    DesktopSettings desktop;
    unsigned lastRecenter = 0, lastMenu = 0;
    double nextControlRead = 0;
    screen.keyWasDown = (GetAsyncKeyState(recenterKey) & 0x8000) != 0;

    while (!exitLoop && !app.stop.load())
    {
        if (!app.opt.controlPath.empty() && NowSeconds() >= nextControlRead)
        {
            nextControlRead = NowSeconds() + .1;
            DesktopSettings next;
            if (ReadDesktopSettings(app.opt.controlPath, next))
            {
                if (next.stop) { Log("Desktop requested stop"); break; }
                if (next.width != desktop.width || next.distance != desktop.distance || next.height != desktop.height ||
                    next.horizontal != desktop.horizontal || next.strength != desktop.strength || next.follow != desktop.follow || next.stereo != desktop.stereo)
                    Log("Desktop settings applied: width %.2f distance %.2f height %.2f horizontal %.2f strength %.2f follow %d stereo %d",
                        next.width, next.distance, next.height, next.horizontal, next.strength, next.follow, next.stereo);
                if (next.recenter != lastRecenter) screen.pending = true;
                if (next.menu != lastMenu) dashboardCloseRequested = true;
                if (recenterKey != next.recenterKey) screen.keyWasDown = (GetAsyncKeyState(next.recenterKey) & 0x8000) != 0;
                if (menuKey != next.menuKey) dashboardKeyWasDown = (GetAsyncKeyState(next.menuKey) & 0x8000) != 0;
                desktop = next; lastRecenter = next.recenter; lastMenu = next.menu;
                recenterKey = next.recenterKey; menuKey = next.menuKey;
                app.opt.warpScale = next.strength; app.opt.doWarp = next.stereo != 0;
                app.opt.keepDashboard = next.autoDismiss == 0;
                if (app.opt.paired != (next.paired != 0))
                    Log("Frame matching: %s", next.paired ? "enabled (matching colour and depth; added delay)" : "disabled (latest colour)");
                app.opt.paired = next.paired != 0;
                const bool foreground = next.foreground != 0 && next.stereo != 0;
                if (app.foregroundEnabled.exchange(foreground) != foreground)
                    Log("Foreground refinement: %s", foreground ? "enabled" : "disabled");
                if (next.version >= 4)
                {
                    const ModelSpec* model = next.fastModel ? &kZipDepth : &kDepthAnythingV2;
                    if (app.requestedModel.exchange(model) != model)
                        Log("Depth model requested: %s", model->name);
                }
            }
        }
        screen.Key((GetAsyncKeyState(recenterKey) & 0x8000) != 0);
        const bool dashboardKeyDown = (GetAsyncKeyState(menuKey) & 0x8000) != 0;
        if (dashboardKeyDown && !dashboardKeyWasDown) dashboardCloseRequested = true;
        dashboardKeyWasDown = dashboardKeyDown;
        XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
        XrResult pollResult = XR_SUCCESS;
        while ((pollResult = xrPollEvent_(app.instance, &ev)) == XR_SUCCESS)
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
                    if (!running) { app.stop = true; exitLoop = true; }
                }
                else if (state == XR_SESSION_STATE_STOPPING)
                {
                    if (XR_FAILED(xrEndSession_(app.session))) { app.stop = true; exitLoop = true; }
                    running = false;
                }
                else if (state == XR_SESSION_STATE_EXITING) exitLoop = true;
                else if (state == XR_SESSION_STATE_LOSS_PENDING) { app.stop = true; exitLoop = true; }
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
        if (XR_FAILED(pollResult)) { Log("RunFrameLoop: event polling failed %s", XRStr(pollResult)); app.stop = true; exitLoop = true; }
        if (exitLoop) break;
        if (!running)
        {
            Sleep(10);
            if (app.opt.runSeconds > 0 && NowSeconds() - loopStart > app.opt.runSeconds) exitLoop = true;
            continue;
        }

        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        if (XR_FAILED(xrWaitFrame_(app.session, nullptr, &fs))) { Log("RunFrameLoop: xrWaitFrame failed"); app.stop = true; break; }
        if (XR_FAILED(xrBeginFrame_(app.session, nullptr))) { Log("RunFrameLoop: xrBeginFrame failed"); app.stop = true; break; }
        frames++; repFrames++;

        const XrCompositionLayerBaseHeader* layers[VIEWS] = {};
        XrCompositionLayerQuad quads[VIEWS] = {};
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
            const bool validTracking = ValidStereoViews(vr, vc, vs.viewStateFlags);
            if (validTracking != trackingWasValid)
                Log("RunFrameLoop: tracking %s (locate %s, views %u, flags %llu)",
                    validTracking ? "restored" : "unavailable; submitting empty frames", XRStr(vr), vc,
                    (unsigned long long)vs.viewStateFlags);
            trackingWasValid = validTracking;
            if (!validTracking) ++invalidTrackingFrames;
            if (validTracking)
            {

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

            if (!app.opt.headLocked)
            {
                if (!app.opt.controlPath.empty() && desktop.follow) screen.pending = true;
                if (screen.Place(views[0].pose, views[1].pose, sharedRot, tanHalfX,
                    float(target.ch) / target.cw) && !desktop.follow)
                    Log("RunFrameLoop: screen recentered using the current headset direction");
                if (!app.opt.controlPath.empty()) screen.Adjust(desktop.width, desktop.distance,
                    desktop.height, desktop.horizontal, float(target.ch) / target.cw);
                // Keep size and disparity calibration stable after placement.
                focalPx = target.cw * (app.opt.controlPath.empty() ? ScreenAnchor::distance : desktop.distance) / screen.size.width;
            }

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

            XrReadyImage colorImage, depthImage;
            bool ok = colorImage.Acquire(app.colorSc, xrAcquireImage_, xrWaitImage_);
            if (ok && useDepthSc) ok = depthImage.Acquire(app.depthSc, xrAcquireImage_, xrWaitImage_);
            if (!ok) { Log("RunFrameLoop: swapchain acquire/wait failed (%d, %d)",
                (int)colorImage.result, (int)depthImage.result); app.stop = true; }
            const uint32_t cIdx = colorImage.index, dIdx = depthImage.index;
            bool drewSource = false, stereo = false;

            if (ok)
            {
                const int ring = (int)(drawn % RING);
                WaitFence(app, app.frameFence[ring]);
                app.cmdAlloc[ring]->Reset();
                app.cmdList->Reset(app.cmdAlloc[ring].Get(), nullptr);

                // --- latest source frame
                SourceFrames::WriteRef newSrc;
                double t = NowSeconds();
                if (app.opt.source == SourceKind::Synthetic)
                {
                    MakeScene(scene, t);
                    newSrc = RecordCpuSource(app, scene, ring);
                    if (newSrc) newSrc->time = t;
                }
                SourceRef source = newSrc ? SourceRef(newSrc) : app.sources.Latest();

                // --- latest COMPLETED depth
                bool tookSlot = false;
                if (app.readySlot.load() & SLOT_FRESH)
                {
                    WaitFence(app, app.slotFence);
                    app.readSlot = app.readySlot.exchange(app.readSlot) & (SLOT_FRESH - 1);
                    cur = &app.slots[app.readSlot];
                    RecordUploadNear(app, cur->nearUp.Get());
                    tookSlot = true;
                    depthUpdates++; repDepth++;
                }
                stereo = app.opt.doWarp && source && cur && cur->source &&
                    source->layout == cur->source->layout &&
                    UsableDepth(app.depthHealthy.load(), source->seq, source->time,
                        cur->source->seq, cur->source->time);
                if (stereo != wasStereo)
                    Log("RunFrameLoop: %s", stereo ? "stereo depth available" : "flat viewing (depth unavailable or stale)");
                wasStereo = stereo;
                if (app.opt.paired && stereo) source = cur->source;

                if (source)
                {
                    // a captured frame comes from another device: make the gfx queue
                    // wait for it on the GPU timeline (no CPU stall)
                    if (SourceFence(app) != app.fence.Get())
                        app.gfxQueue->Wait(SourceFence(app), source->value);

                    WarpConstants c{};
                    c.cw = (uint32_t)target.cw; c.ch = (uint32_t)target.ch; c.dw = W; c.dh = H;
                    c.doWarp = stereo ? 1u : 0u;
                    c.scaleFocal = app.opt.warpScale * focalPx;
                    c.invZNear = 1.0f / 1.2f;
                    c.invZFar = 1.0f / 12.0f;
                    if (!app.opt.headLocked)
                    {
                        // The compositor already supplies the screen-plane
                        // disparity. Warp only the depth relative to that plane.
                        const float screenDistance = app.opt.controlPath.empty() ? ScreenAnchor::distance : desktop.distance;
                        c.invZNear -= 1.0f / screenDistance;
                        c.invZFar -= 1.0f / screenDistance;
                    }
                    c.eye0 = -0.5f * ipd; c.eye1 = 0.5f * ipd;
                    c.nearZ = DEPTH_NEAR_Z; c.farZ = DEPTH_FAR_Z;
                    c.indicator = (app.opt.abSeconds > 0.0) ? (depthOn ? 1u : 2u) : 0u;
                    c.depthLie = app.opt.depthLie ? 1u : 0u;
                    c.writeDepth = useDepthSc && stereo ? 1u : 0u;
                    c.exactLoad = (app.srcW == target.cw && app.srcH == target.ch) ? 1u : 0u;
                    c.fillMode = (uint32_t)app.opt.fillMode;
                    c.mirrorTol = MIRROR_TOL;
                    RecordWarp(app, target, DESC_SRC0 + (UINT)source->index, c);
                    RecordCopyToSwapchain(app, target, app.cimgs[cIdx].texture, useDepthSc ? app.dimgs[dIdx].texture : nullptr);
                    RecordWarpOutputsBackToUav(app, target);
                    drewSource = true;
                }
                app.cmdList->Close();
                UINT64 fv = SubmitAndSignal(app);
                if (drewSource) app.sources.MarkRead(source, SourceFrames::Reader::Graphics, fv);
                app.frameFence[ring] = fv;
                if (tookSlot) app.slotFence = fv;
                if (newSrc) app.sources.Publish(newSrc, fv, t);

                if (drewSource) { drawn++; repDrawn++; }
                if (cur) repAgeMs += (NowSeconds() - cur->completeTime) * 1000.0;
            }
            const bool releasedColor = colorImage.Release(xrReleaseImage_);
            const bool releasedDepth = depthImage.Release(xrReleaseImage_);
            if (!releasedColor || !releasedDepth) { Log("RunFrameLoop: swapchain release failed"); ok = false; app.stop = true; }

            if (ok && drewSource)
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
                    pviews[e].next = depthOn && stereo ? &dinfo[e] : nullptr;
                }
                proj.space = app.space;
                proj.viewCount = VIEWS;
                proj.views = pviews;
                layers[0] = (XrCompositionLayerBaseHeader*)&proj;
                if (!app.opt.headLocked)
                {
                    for (uint32_t e = 0; e < VIEWS; ++e)
                    {
                        quads[e] = { XR_TYPE_COMPOSITION_LAYER_QUAD };
                        quads[e].space = app.space;
                        quads[e].eyeVisibility = e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                        quads[e].subImage = pviews[e].subImage;
                        quads[e].pose = screen.pose;
                        quads[e].size = screen.size;
                        layers[e] = (const XrCompositionLayerBaseHeader*)&quads[e];
                    }
                }
            }
            repCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
            } // valid tracking; otherwise xrEndFrame below submits zero layers
        }

        XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = layers[0] ? (app.opt.headLocked ? 1u : VIEWS) : 0u;
        fei.layers = layers;
        XrResult er = xrEndFrame_(app.session, &fei);
        if (XR_FAILED(er)) { Log("RunFrameLoop: xrEndFrame %s", XRStr(er)); app.stop = true; }
        // SteamVR can create its startup dashboard AFTER the first frame.
        // Allow one second of visible playback before the single dismissal.
        // Never fight menus opened later; network work stays off this thread.
        const bool visiblePicture = XR_SUCCEEDED(er) && fei.layerCount &&
            (state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED);
        if (!visiblePicture) dashboardVisibleSince = -1.0;
        else if (dashboardVisibleSince < 0.0) dashboardVisibleSince = NowSeconds();
        const bool startupCloseDue = visiblePicture && NowSeconds() - dashboardVisibleSince >= 1.0 &&
            !app.opt.keepDashboard && !dashboardStartupAttempted;
        if (dashboardCloseRequested && !app.steamVrRuntime)
        {
            Log("SteamVR dashboard: F8 only applies to the SteamVR runtime");
            dashboardCloseRequested = false;
        }
        if (app.steamVrRuntime && (startupCloseDue || dashboardCloseRequested) && !app.dashboardBusy.load())
        {
            // A finished worker remains joinable. Reap it before an explicit
            // later key press starts a new request; never block the frame loop
            // on an in-flight network operation.
            if (app.dashboardWorker.joinable()) app.dashboardWorker.join();
            const bool fromKey = dashboardCloseRequested;
            dashboardCloseRequested = false;
            dashboardStartupAttempted = true;
            app.dashboardBusy = true;
            app.dashboardWorker = std::thread([&app, fromKey] {
                g_threadName = "menu";
                const DWORD result = RequestSteamVrDashboardClose();
                if (result == ERROR_SUCCESS)
                    Log("SteamVR dashboard: %s close request sent and connection closed cleanly (visual confirmation required)", fromKey ? "F8" : "startup");
                else Log("SteamVR dashboard: close unavailable (Windows error %lu); playback continues", result);
                app.dashboardBusy = false;
            });
        }

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
        if (app.opt.runSeconds > 0 && now - loopStart > app.opt.runSeconds) exitLoop = true;
    }

    double el = NowSeconds() - loopStart;
    Log("RunFrameLoop: skipped %llu frames for invalid tracking", (unsigned long long)invalidTrackingFrames);
    Log("RunFrameLoop: exit - frames %llu (%.1f fps) drawn %llu (%.1f fps) depth updates %llu (%.1f /s), worker published %llu, captured %llu",
        (unsigned long long)frames, frames / (el > 0 ? el : 1), (unsigned long long)drawn, drawn / (el > 0 ? el : 1),
        (unsigned long long)depthUpdates, depthUpdates / (el > 0 ? el : 1), (unsigned long long)app.depthPublished.load(),
        (unsigned long long)app.cap.frames.load());
}

// ------------------------------------------------------------------- dump
static bool WritePpmRgba(const char* path, const unsigned char* rgba, int w, int h, UINT rowPitch)
{
    if (!path || !rgba) return false;
    if (w <= 0 || h <= 0) return false;

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) { Log("WritePpmRgba: cannot write %s", path); return false; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<unsigned char> row((size_t)w * 3);
    for (int y = 0; y < h; y++)
    {
        const unsigned char* s = rgba + (size_t)y * rowPitch;
        for (int x = 0; x < w; x++) { row[x * 3] = s[x * 4]; row[x * 3 + 1] = s[x * 4 + 1]; row[x * 3 + 2] = s[x * 4 + 2]; }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

// Warps the published depth + source on the GPU exactly as the frame loop does
// (presented resolution, bilinear depth, given fill mode) and writes both eyes.
// View parameters are the PS VR2 shared-FOV figures from the M3/M4 logs, since no
// frame loop is running yet: 86.9 deg horizontal, IPD 71 mm.
static bool DumpEyes(App& app, int fillMode, const char* tag)
{
    if (!tag) return false;
    Log("DumpEyes: enter (fill %s)", tag);

    const DepthSlot& slot = app.slots[app.readySlot.load() & (SLOT_FRESH - 1)];
    SourceRef source = slot.source;
    if (!source) { Log("DumpEyes: FAIL no published depth"); return false; }
    WarpTarget& t = app.mainTarget;
    ID3D12Device* dev = app.device.Get();

    D3D12_RESOURCE_DESC cdesc = t.colorOut->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT cfp[VIEWS];
    UINT64 cTotal = 0;
    dev->GetCopyableFootprints(&cdesc, 0, VIEWS, 0, cfp, nullptr, nullptr, &cTotal);
    ComPtr<ID3D12Resource> crb;
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, cTotal, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, crb, nullptr)) return false;

    const float tanHalfX = tanf(86.9f * 0.5f * 3.14159265f / 180.0f);
    WarpConstants c{};
    c.cw = (uint32_t)t.cw; c.ch = (uint32_t)t.ch; c.dw = W; c.dh = H;
    c.doWarp = app.opt.doWarp ? 1u : 0u;
    c.scaleFocal = app.opt.warpScale * (float)(t.cw * 0.5) / tanHalfX;
    c.invZNear = 1.0f / 1.2f; c.invZFar = 1.0f / 12.0f;
    if (!app.opt.headLocked)
    {
        // As the frame loop does for the fixed screen (default 3 m): content behind
        // the screen plane gets uncrossed disparity (outward in each eye).
        const float screenDistance = 3.0f;
        c.invZNear -= 1.0f / screenDistance;
        c.invZFar -= 1.0f / screenDistance;
    }
    c.eye0 = -0.0355f; c.eye1 = 0.0355f;
    c.nearZ = DEPTH_NEAR_Z; c.farZ = DEPTH_FAR_Z;
    c.exactLoad = (app.srcW == t.cw && app.srcH == t.ch) ? 1u : 0u;
    c.fillMode = (uint32_t)fillMode;
    c.mirrorTol = MIRROR_TOL;

    WaitFence(app, app.fenceVal);
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    RecordUploadNear(app, slot.nearUp.Get());
    RecordWarp(app, t, DESC_SRC0 + (UINT)source->index, c);
    for (UINT e = 0; e < VIEWS; e++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.pResource = crb.Get(); dst.PlacedFootprint = cfp[e];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = t.colorOut.Get(); src.SubresourceIndex = e;
        app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    RecordWarpOutputsBackToUav(app, t);
    app.cmdList->Close();
    if (SourceFence(app) != app.fence.Get()) app.gfxQueue->Wait(SourceFence(app), source->value);
    UINT64 fv = SubmitAndSignal(app);
    app.sources.MarkRead(source, SourceFrames::Reader::Graphics, fv);
    WaitFence(app, fv);

    unsigned char* cp = nullptr;
    if (FAILED(crb->Map(0, nullptr, (void**)&cp)) || !cp) { Log("DumpEyes: FAIL map readback"); return false; }
    bool ok = true;
    for (UINT e = 0; e < VIEWS; e++)
    {
        char path[64];
        snprintf(path, sizeof(path), "xrapp5_eye%u_%s.ppm", e, tag);
        ok = WritePpmRgba(path, cp + cfp[e].Offset, t.cw, t.ch, cfp[e].Footprint.RowPitch) && ok;
        Log("DumpEyes: wrote %s (%dx%d)", path, t.cw, t.ch);
    }
    crb->Unmap(0, nullptr);
    Log("DumpEyes: exit %s (focal %.1f px, max disparity per eye %.1f px)", ok ? "ok" : "FAIL",
        c.scaleFocal, c.scaleFocal * 0.0355f * c.invZNear);
    return ok;
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
        while (!app.sources.Latest() && !app.stop.load() && NowSeconds() < deadline) Sleep(5);
        if (!app.sources.Latest()) { Log("FirstSourceFrame: FAIL no captured frame within 5 s"); return false; }
        Log("FirstSourceFrame: exit ok (first captured frame arrived)");
        return true;
    }

    std::vector<unsigned char> first;
    if (app.opt.source == SourceKind::Image) first = app.imageRgb; else MakeScene(first, 0.0);
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    auto source = RecordCpuSource(app, first, 0);
    app.cmdList->Close();
    if (!source) return false;
    UINT64 fv = SubmitAndSignal(app);
    WaitFence(app, fv);
    app.sources.Publish(source, fv, 0.0);
    Log("FirstSourceFrame: exit ok (source texture %d)", source->index);
    return true;
}

static void Shutdown(App& app)
{
    if (app.dashboardWorker.joinable()) app.dashboardWorker.join();
    Log("Shutdown: enter");
    app.stop = true;
    if (app.worker.joinable()) app.worker.join();
    if (app.cap.thread.joinable()) app.cap.thread.join();
    WaitFence(app, app.fenceVal);
    WaitDepthIdle(app);
    // Capture may have started before its polling thread was created.
    try {
        if (app.cap.closedRegistered) { app.cap.item.Closed(app.cap.closedToken); app.cap.closedRegistered = false; }
        if (app.cap.session) app.cap.session.Close();
        if (app.cap.pool) app.cap.pool.Close();
    } catch (const winrt::hresult_error&) {}

    DumpDebugMessages(app);

    if (ort) ReleaseModel(app);
    if (app.colorSc) xrDestroySwapchain_(app.colorSc);
    if (app.depthSc) xrDestroySwapchain_(app.depthSc);
    if (app.session) xrDestroySession_(app.session);
    if (app.instance) xrDestroyInstance_(app.instance);
    if (app.fenceEvent) CloseHandle(app.fenceEvent);
    if (app.mlFenceEvent) CloseHandle(app.mlFenceEvent);
    if (app.secondGpu || app.inferFenceEvent) ReleaseSecondGpu(app);
    Log("Shutdown: exit (source frames dropped under backpressure: %llu)",
        (unsigned long long)app.sourceDrops.load());
}

int wmain(int argc, wchar_t** wideArgv)
{
    std::vector<std::string> arguments;
    std::vector<char*> argv;
    for (int i = 0; i < argc; ++i) arguments.push_back(winrt::to_string(wideArgv[i]));
    for (auto& arg : arguments) argv.push_back(arg.data());
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_t0 = std::chrono::steady_clock::now();
    // Physical-pixel window geometry for the client-area crop: without this, a
    // DPI-unaware process gets scaled client rects that do not match the captured
    // frame (Windows display scaling of 150% would put the crop in the wrong place).
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        Log("main: per-monitor DPI awareness unavailable (error %lu); window crops fall back to the whole frame when inconsistent", GetLastError());
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    static App app;
    if (!ParseArgs(argc, argv.data(), &app.opt)) return 1;
    app.testDepthFailuresLeft = app.opt.testDepthFailures;
    app.foregroundEnabled = app.opt.foreground && app.opt.doWarp;

    int rc = 1;
    try
    {
    if (app.opt.checkPackage)
    {
        Log("Package check: model %s", winrt::to_string(ModelPath(app.opt.model->file).wstring()).c_str());
        for (const auto* name : { L"openxr_loader.dll", L"DirectML.dll" })
        {
            HMODULE module = LoadLibraryW((ExecutableDirectory() / name).c_str());
            if (!module) { Log("Package check: missing dependency (%lu)", GetLastError()); return 1; }
            FreeLibrary(module);
        }
        Log("Package check: PASS (model and native dependencies available; no VR session started)");
        return 0;
    }
    if (app.opt.checkSource) return FindCaptureWindow(app.opt) ? 0 : 1;
    if (!app.opt.controlPath.empty())
    {
        DesktopSettings initial;
        if (!ReadDesktopSettings(app.opt.controlPath, initial) || app.opt.headLocked)
        { Log("Desktop control: invalid settings or incompatible projection diagnostic"); return 1; }
        app.foregroundEnabled = initial.foreground != 0 && initial.stereo != 0;
        app.opt.paired = initial.paired != 0;
        if (initial.version >= 4)
        {
            app.opt.model = initial.fastModel ? &kZipDepth : &kDepthAnythingV2;
            app.requestedModel = app.opt.model;
            Log("Desktop control: depth model %s", app.opt.model->name);
        }
    }
    do
    {
        if (app.opt.source == SourceKind::Capture && !SelectCaptureItem(app)) break;
        if (!InitXrInstance(app)) break;
        if (!InitD3D(app)) break;
        if (!InitModel(app)) break;
        if (!InitSlots(app)) break;
        if (!InitSource(app)) break;
        // The self-test needs the headset's GPU (from the runtime) but not a VR
        // session, so it also runs while the headset is asleep or disconnected.
        if (!app.opt.selfTestOnly && !InitXrSession(app)) break;
        if (!InitShaders(app)) break;

        if (!SelfTest(app)) { Log("main: GPU shaders do not match the CPU reference - not presenting"); break; }
        if (app.opt.selfTestOnly) { if (SelfTestForegroundModel(app)) rc = 0; break; }

        if (!FirstSourceFrame(app)) break;

        // First depth result synchronously, so the render thread has depth from
        // its first frame and the log shows the model's numbers even if the HMD
        // never wakes.
        std::vector<float> firstNear;
        if (ComputeAndPublish(app, app.sources.Latest(), firstNear))
        {
            const DepthSlot& s = app.slots[app.readySlot.load() & (SLOT_FRESH - 1)];
            Log("main: first depth in %.1f ms (includes warm-up), range %.3f..%.3f", s.modelMs, s.lo, s.hi);
            if (app.opt.source == SourceKind::Synthetic)
                Log("main: model nearness (0 far..1 near): backdrop %.2f panel %.2f marker %.2f  [truth 0.00 / 0.50 / 1.00]", s.back, s.panel, s.marker);
        }
        else
        {
            Log("main: initial depth failed; starting flat playback and worker retries");
            WaitDepthIdle(app);
        }
        if (app.opt.doDump)
        {
            if (!app.depthHealthy.load()) { Log("main: cannot dump eyes without valid depth"); break; }
            std::vector<unsigned char> g(firstNear.size());
            for (size_t i = 0; i < g.size(); i++) g[i] = (unsigned char)(firstNear[i] * 255.0f + 0.5f);
            WritePNM("xrapp5_near.pgm", "P5", g.data(), g.size());
            if (!DumpEyes(app, FILL_STRETCH, "stretch")) break;
            if (!DumpEyes(app, FILL_MIRROR, "mirror")) break;
        }

        app.worker = std::thread(WorkerMain, &app);
        RunFrameLoop(app);
        rc = app.stop.load() || !app.depthHealthy.load() ? 1 : 0;
    } while (false);
    }
    catch (const winrt::hresult_error& e) { Log("main: failure 0x%08X", (unsigned)e.code().value); }
    catch (const std::exception& e) { Log("main: %s", e.what()); }
    catch (...) { Log("main: unexpected failure"); }

    Shutdown(app);
    return rc;
}
