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
//            --steady               steady depth: blend in the previous depth, moved by the GPU's
//                                   hardware motion estimator, where the motion is verified
//            --fuse                 fuse ZipDepth with Depth Anything V2 (runs alongside on the
//                                   depth GPU; its late result is moved to the current frame)
//            --delayed              hold the game image back by the measured depth delay, so the
//                                   newest depth lines up with it (smooth; --paired is exact but
//                                   updates at the depth rate). See frame_timing.h / XSYNC.md
//            --curve=N              curve the screen: 0 flat (default) .. 100 fully wrapped; a
//                                   curved screen is ray-cast per eye (screen_curve.h, XCURVE.md)
//            --ambilight            spill the picture's edge colours around the screen
//            --ambilight-strength=N the glow's brightness next to the screen, 0..100 (default 85)
//            --world=RRGGBB         the colour around the screen (default 000000, black)
//            --room=N               a room lit by the screen round it: 0 off (default) .. 100
//                                   (wall brightness); needs the fixed screen (room.h, XROOM.md)
//            --room-glass=N         the room's side walls, back wall and ceiling as glass in
//                                   slim frames, over a ground to the horizon: 0 solid
//                                   (default) .. 100 clear; also 1 m floor tiles
//            --room-reflect=N       reflections in the room's glass and floor (the screen
//                                   mirrored, from each eye): 0 none (default) .. 100; also
//                                   the frames and tiles, like --room-glass
//            --room-light=N         the room's ceiling light above and behind you: 0 off
//                                   (default) .. 100
//            --room-light-colour=RRGGBB  its colour (default FFB46B, 3000 K)
//            --room-v10-eye         diagnostic A/B: draw the room with the kept v10 eye pass
//            --bench-room           with --selftest: time the room's passes offline at the
//                                   headset's eye size and write room-bench.csv (BenchRoom)
//            --bench-fov=L,R,U,D    the benchmark's left-eye FOV in degrees (mirrored for the
//                                   right eye); the playback log prints the headset's own
//            --bench-boost          do not lock the GPU clocks for the benchmark (the headset
//                                   runs at boost clocks; locked ones vary less)
//            --bench-lock           lock them even while SteamVR's compositor runs (the lock
//                                   slows the headset's frames too, so it is not the default)
//            --head-locked          follow your head (default fixed screen; '=' recenters)
//            --keep-dashboard       skip the SteamVR startup dashboard-close request
//   keys:    '=' recenter; F8 dismiss SteamVR dashboard (keys also reach the game)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
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
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

#include "xr_common.h"
#include "gpu_choice.h"
#include "source_ring.h"
#include "capture_scaler.h"
#include "xr_frame_guard.h"
#include "screen_anchor.h"
#include "capture_window.h"
#include "desktop_control.h"
#include "foreground_refinement.h"
#include "depth_fusion.h"
#include "motion_estimator.h"
#include "frame_timing.h"
#include "ambilight.h"
#include "screen_curve.h"
#include "room.h"

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
    // Which GPU runs the depth model (gpu_choice.h): same (default), auto, a GPU
    // name + which card of that name, or a DXGI index for diagnostics.
    GpuRequest depthGpu;
    bool listGpus = false;              // --list-gpus: print the GPUs for the desktop app and exit
    // Second GPU only. true: the capture thread box-filters each frame to model size
    // and the headset GPU's copy engine sends it across (no depth work waits on its
    // graphics/compute engines). false: model-input prep on the headset GPU.
    bool frameTransfer = true;
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
    bool steady = false;                // --steady (the desktop app sets it per game, live)
    bool fuse = false;                  // --fuse (likewise)
    bool delayed = false;               // --delayed: game frame timing "delayed to depth" (paired wins if both)
    bool subpixel = true;               // sub-pixel warp (no depth banding); --whole-pixel restores the old warp
    float curve = 0.0f;                 // curved screen: 0 flat .. 1 fully wrapped (screen_curve.h)
    bool ambilight = false;             // spill the picture's edge colours around the screen
    int ambiStrength = kAmbiDefaultStrength;    // percent: the glow's brightness next to the screen
    uint32_t worldColor = 0;            // 0xRRGGBB around the screen (0 = black, no layer)
    int room = 0;                       // the room lit by the screen: 0 off, 1..100 (wall brightness)
    int roomGlass = 0;                  // the room's glass walls: 0 solid .. 100 clear (room.h RoomLook)
    int roomReflect = 0;                // the reflections in its glass and floor: 0 none .. 100 (room.h RoomLook)
    int roomLight = 0;                  // the room light: 0 off .. 100 (room.h RoomLook)
    uint32_t roomLightColor = kRoomLightDefault;    // its colour, 0xRRGGBB
    bool roomV10Eye = false;            // --room-v10-eye: the kept v10 eye pass (A/B diagnostic)
    bool benchRoom = false;             // --bench-room (with --selftest): the offline room benchmark
    bool benchBoost = false;            // --bench-boost: leave the GPU clocks unlocked while benchmarking
    bool benchLock = false;             // --bench-lock: lock the GPU clocks even while SteamVR's compositor runs
    float benchFov[4] = { -52.0f, 43.5f, 48.7f, -48.7f };   // --bench-fov: left eye L, R, U, D degrees (estimates)
};

static const int SLOTS = 3;
static const int SLOT_FRESH = 4;        // flag bit in readySlot
static const int RING = 3;              // in-flight render frames
static const UINT TIME_SLOT = 5;        // GPU timestamps per frame slot (App::timeHeap)
// Source textures; references and GPU fences guard reuse. 12: "delayed to depth" keeps
// up to DELAYED_HISTORY recent frames on top of the depth slots and capture in flight.
static const int SRC_RING = 12;
static const size_t DELAYED_HISTORY = 6;
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
    uint32_t subpixel;
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
    // Second-GPU frame transfer: the source ring as shader input, and a parallel
    // ring of model-size frames the copy engine sends to the inference GPU.
    ComPtr<ID3D11ShaderResourceView> srv11[SRC_RING];
    ComPtr<ID3D11Texture2D> small11[SRC_RING];
    ComPtr<ID3D11RenderTargetView> smallRtv11[SRC_RING];
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

    // Frame transfer (second GPU, capture source): model-size frames made by the
    // capture thread, sent by the headset GPU's copy engine, unpacked on the
    // inference GPU. Crop passes still use the prep path above.
    bool frameTransfer = false;
    int xferW = 0, xferH = 0;                            // the largest model input; frames are this size
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT xferFootprint{};  // BGRA8 rows in the shared buffer
    UINT64 xferBytes = 0;                                // exact frame size: the last row is not padded to the pitch
    ComPtr<ID3D12Resource> smallTex[SRC_RING];           // headset GPU, shared with the capture device
    ComPtr<ID3D12CommandQueue> xferQueue;                // headset GPU copy engine
    ComPtr<ID3D12CommandAllocator> xferAlloc;
    ComPtr<ID3D12GraphicsCommandList> xferList;
    ComPtr<ID3D12Fence> xferFence;
    UINT64 xferFenceVal = 0;
    HANDLE xferFenceEvent = nullptr;
    ComPtr<ID3D12Resource> frameLocal;                   // inference GPU: the received frame
    ComPtr<ID3D12RootSignature> unpackRootSig;           // inference GPU: frame -> model input
    ComPtr<ID3D12PipelineState> unpackPso;
    std::vector<float> modelOut;        // model output at the model's own size
    struct ResampleTap { int i0, i1; float t; };
    std::vector<ResampleTap> resampleX, resampleY;   // model output -> depth grid, built once
    std::vector<float> rawDepth;        // model output resampled to the W x H depth grid
    std::vector<double> passMs;         // worker: full-pass latencies (submit to model output on the CPU) since the last summary
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

    // Ambilight (ambilight.h): one small glow texture, kept between frames so the
    // surround can drift, copied into its own quad layer behind the screen.
    ComPtr<ID3D12RootSignature> ambiRootSig;
    ComPtr<ID3D12PipelineState> ambiPso, ambiRingPso;
    ComPtr<ID3D12Resource> ambiRing;                     // float4[2 * kAmbiRingMax]: the border ring
    ComPtr<ID3D12Resource> ambiHist;                     // RGBA16F: the temporal blend's memory
    ComPtr<ID3D12Resource> ambiTex;
    int ambiW = 0, ambiH = 0;
    XrSwapchain ambiSc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> aimgs;
    bool ambiHistory = false;                            // false: the glow holds nothing usable yet

    // Curved screen (screen_curve.h): a cylinder ray-cast for each eye into eye
    // buffers at the runtime's recommended size, submitted as a projection layer.
    // The swapchain and buffers are made on first use.
    ComPtr<ID3D12RootSignature> curveRootSig;
    ComPtr<ID3D12PipelineState> curvePso;
    int64_t colorFormat = 0;                             // the swapchain format chosen at session start
    int eyeW = 0, eyeH = 0;
    XrSwapchain eyeSc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> eimgs;
    ComPtr<ID3D12Resource> eyeOut;                       // UNORM UAV [2]; SRGB swapchains cannot be UAVs
    bool eyeFailed = false;                              // could not be made: the screen stays flat
    ComPtr<ID3D12Resource> testEyeOut;                   // TEST_EYE_W x TEST_EYE_H [2]

    // The room (room.h, XROOM.md): made up front (small) so the option applies live
    // and the self-test always runs; only the eye buffers are made on first use.
    ComPtr<ID3D12RootSignature> roomRootSig, curveRoomRootSig;
    ComPtr<ID3D12PipelineState> roomEmitPso, roomLightPso, curveRoomPso;
    ComPtr<ID3D12PipelineState> curveRoomLookPso;        // the room's eye pass with the v11 controls' code (RoomEyePso)
    ComPtr<ID3D12PipelineState> curveRoomV10Pso;         // the kept v10 eye pass (--room-v10-eye, the benchmark's case D)
    ComPtr<ID3D12Resource> roomLut;                      // float[256]: the sRGB decode
    ComPtr<ID3D12Resource> roomEmitters;                 // float4[6 * kRoomMaxEmitters]
    ComPtr<ID3D12Resource> roomLight;                    // RGBA16F, 6 faces x kRoomLightmap^2
    ComPtr<ID3D12PipelineState> roomMirrorPso;           // MIRROR: the mirror picture (kRoomHlsl, ROOM_MIRROR)
    ComPtr<ID3D12Resource> roomMirror;                   // RGBA16F, kRoomMirrorW x kRoomMirrorMaxH (the first mirrorH rows used), UAV at rest
    ComPtr<ID3D12Resource> roomGeomUp[RING + 1];         // emitter geometry to copy in; [RING] is the self-test's
    unsigned char* roomGeomMapped[RING + 1] = {};
    ComPtr<ID3D12Resource> roomCbUp;                     // RoomConstants, 512 B per frame slot + 1 for the self-test
    unsigned char* roomCbMapped = nullptr;
    XrSpace stage = XR_NULL_HANDLE;                      // the real floor (STAGE), if the runtime has one

    // GPU time of the screen/room passes: TIME_SLOT timestamps per frame slot. The room
    // writes all five (T0 before its light, T1 after EMIT, T2 after LIGHT, T3 after the
    // eye pass, T4 after the copy); a curved screen without the room writes T0 and T1
    // (begin and end), as before the split.
    ComPtr<ID3D12QueryHeap> timeHeap;
    ComPtr<ID3D12Resource> timeReadback;
    const UINT64* timeMapped = nullptr;
    double timeFreq = 0;
    UINT timeCount[RING] = {};                           // timestamps pending in each slot: 0, 2 or 5

    // World colour behind a flat screen: a projection layer of one colour (a curved
    // screen fills its own eye buffers with it instead).
    XrSwapchain worldSc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> wimgs;
    ComPtr<ID3D12Resource> worldUp[RING];                // UPLOAD, one per frame slot
    unsigned char* worldMapped[RING] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT worldFp[VIEWS] = {};

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

    // Depth steadying and two-model fusion (depth_fusion.h, XMMODEL.md). Set by the
    // render thread from the desktop settings; everything below them is owned by the
    // depth worker (and by main before the worker starts), except the anchor mailboxes.
    std::atomic<bool> steadyEnabled{ false }, fuseEnabled{ false };
    // Grid image: each depth frame box-filtered to the W x H depth grid as RGBA8 and
    // read back - luma for the motion estimator, colour for the anchor model.
    ComPtr<ID3D12PipelineState> gridPso;                 // root signature: prepRootSig
    ComPtr<ID3D12Resource> gridBuf, gridReadback;
    uint32_t* gridMapped = nullptr;
    ComPtr<ID3D12CommandAllocator> gridAlloc;
    ComPtr<ID3D12GraphicsCommandList> gridList;
    MotionEstimator motion;
    bool motionTried = false;           // the estimator is created on first use
    int motionCurSlot = 0;              // slots 0/1 alternate current/previous; 2 = anchor
    // Steadying: the previous output, its luma and frame.
    fusion::Image steadyPrevOut, steadyPrevLuma;
    uint64_t steadyPrevSeq = 0, postLayout = 0;
    bool steadyHave = false;
    // Fusion: recent fast-model near maps by frame, for fitting the anchor onto them.
    std::deque<std::pair<uint64_t, fusion::Image>> fastHistory;
    uint64_t anchorUsedId = 0;          // the anchor result currently in motion slot 2
    float anchorGa = 1, anchorGb = 0;
    bool anchorFit = false;
    std::vector<double> postMs;         // worker: steady/fuse CPU+GPU time per pass
    double postStageMs[8] = {};         // per-stage totals since the last report (see PostProcessDepth)
    unsigned postFused = 0, postSteadied = 0, postPasses = 0;
    double postTrustSum = 0, postAgeSum = 0;
    bool motionUnavailableLogged = false, fuseModelLogged = false;

    // Anchor model (Depth Anything V2) on its own thread and queue on the depth GPU.
    struct AnchorJob { std::vector<uint32_t> rgba; fusion::Image luma; uint64_t seq = 0; double time = 0; bool valid = false; };
    struct AnchorResult { fusion::Image nearMap, luma; uint64_t seq = 0, id = 0; double time = 0; };
    std::mutex anchorMutex;
    std::condition_variable anchorCv;
    AnchorJob anchorJob;                // latest request (overwritten; the anchor takes the newest)
    AnchorResult anchorResult;          // latest result
    std::thread anchorThread;
    std::atomic<bool> anchorFailed{ false };
    std::atomic<unsigned> anchorPasses{ 0 };
    AnchorResult anchorCurrent;         // depth worker's copy of the result in use
    UINT64 gridLast = 0;                // ml fence value of the last grid pass
};

// descriptor heap layout
static const UINT DESC_SRC0 = 0;                         // SRC_RING source SRVs
static const UINT DESC_MAIN_TABLE = SRC_RING;            // 4
static const UINT DESC_TEST_SRC = SRC_RING + 4;          // 1
static const UINT DESC_TEST_TABLE = SRC_RING + 5;        // 4
static const UINT DESC_AMBI_UAV = SRC_RING + 9;          // 3: the ambilight glow texture, its border ring, its history
// The curve and room tables are 5 wide: their fifth entry (+4) is v11's mirror picture
// (an SRV for the eye pass, a UAV for the room's passes).
static const UINT DESC_CURVE_MAIN = SRC_RING + 12;       // 5: warped pictures, glow, eye buffers, room lightmap, mirror picture
static const UINT DESC_CURVE_TEST = SRC_RING + 17;       // 5: the same for the self-test
static const UINT DESC_ROOM = SRC_RING + 22;             // 5: decode table, glow, emitters (UAV), lightmap (UAV), mirror picture (UAV)
static const UINT DESC_CURVE_BENCH = SRC_RING + 27;      // 5: the same as DESC_CURVE_MAIN for the room benchmark (--bench-room)
static const UINT DESC_COUNT = SRC_RING + 32;
static const int WORLD_W = 8;                            // the world colour layer: one colour needs few pixels
static const int TEST_EYE_W = 256, TEST_EYE_H = 192;     // self-test eye buffers

// The glow texture: small (the compositor stretches it over a soft gradient) and
// the same shape as the layer, which is the screen plus a margin on every side.
static ID3D12Fence* SourceFence(App& app)
{
    return app.opt.source == SourceKind::Capture ? app.captureFence.Get() : app.fence.Get();
}

static SourceFrames::WriteRef ReserveSource(App& app)
{
    auto frame = app.sources.Reserve(SourceFence(app)->GetCompletedValue(),
        app.fence->GetCompletedValue(), app.mlFence->GetCompletedValue(),
        app.xferFence ? app.xferFence->GetCompletedValue() : 0);
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
        if (!strncmp(a, "--curve=", 8))
        {
            const double percent = atof(a + 8);
            opt->curve = (float)(percent < 0 ? 0 : (percent > 100 ? 100 : percent)) * 0.01f;
            continue;
        }
        if (!strcmp(a, "--ambilight")) { opt->ambilight = true; continue; }
        if (!strncmp(a, "--room=", 7))
        {
            const int percent = atoi(a + 7);
            opt->room = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
            continue;
        }
        if (!strncmp(a, "--room-glass=", 13))
        {
            const int percent = atoi(a + 13);
            opt->roomGlass = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
            continue;
        }
        if (!strncmp(a, "--room-reflect=", 15))
        {
            const int percent = atoi(a + 15);
            opt->roomReflect = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
            continue;
        }
        if (!strncmp(a, "--room-light=", 13))
        {
            const int percent = atoi(a + 13);
            opt->roomLight = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
            continue;
        }
        if (!strncmp(a, "--room-light-colour=", 20))
        {
            const char* hex = a + 20;
            if (*hex == '#') hex++;
            char* end = nullptr;
            const unsigned long rgb = strtoul(hex, &end, 16);
            if (!end || *end || end - hex != 6) { Log("ParseArgs: --room-light-colour takes RRGGBB (e.g. FFB46B): %s", a); return false; }
            opt->roomLightColor = (uint32_t)rgb;
            continue;
        }
        if (!strcmp(a, "--room-v10-eye")) { opt->roomV10Eye = true; continue; }
        if (!strcmp(a, "--bench-room")) { opt->benchRoom = true; continue; }
        if (!strcmp(a, "--bench-boost")) { opt->benchBoost = true; continue; }
        if (!strcmp(a, "--bench-lock")) { opt->benchLock = true; continue; }
        if (!strncmp(a, "--bench-fov=", 12))
        {
            float f[4] = {};
            char tail = 0;
            if (sscanf_s(a + 12, "%f,%f,%f,%f%c", &f[0], &f[1], &f[2], &f[3], &tail, 1) != 4)
            { Log("ParseArgs: --bench-fov takes four angles in degrees, left,right,up,down (e.g. -52,43.5,48.7,-48.7): %s", a); return false; }
            bool sane = f[0] < f[1] && f[3] < f[2];
            for (float v : f) sane = sane && std::isfinite(v) && std::fabs(v) < 85.0f;
            if (!sane) { Log("ParseArgs: --bench-fov needs left < right and down < up, each within +-85 degrees: %s", a); return false; }
            memcpy(opt->benchFov, f, sizeof(f));
            continue;
        }
        if (!strncmp(a, "--ambilight-strength=", 21))
        {
            const int percent = atoi(a + 21);
            opt->ambiStrength = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
            continue;
        }
        if (!strncmp(a, "--world=", 8))
        {
            const char* hex = a + 8;
            if (*hex == '#') hex++;
            char* end = nullptr;
            const unsigned long rgb = strtoul(hex, &end, 16);
            if (!end || *end || end - hex != 6) { printf("bad --world=RRGGBB: %s\n", a); return false; }
            opt->worldColor = (uint32_t)rgb;
            continue;
        }
        if (!strcmp(a, "--truth")) { opt->useTruth = true; continue; }
        if (!strcmp(a, "--dump")) { opt->doDump = true; continue; }
        if (!strcmp(a, "--paired")) { opt->paired = true; continue; }
        if (!strcmp(a, "--steady")) { opt->steady = true; continue; }
        if (!strcmp(a, "--fuse")) { opt->fuse = true; continue; }
        if (!strcmp(a, "--delayed")) { opt->delayed = true; continue; }
        if (!strcmp(a, "--whole-pixel")) { opt->subpixel = false; continue; }
        if (!strcmp(a, "--check-package")) { opt->checkPackage = true; continue; }
        if (!strcmp(a, "--no-foreground")) { opt->foreground = false; continue; }
        if (!strcmp(a, "--normal-gpu-priority")) { opt->boostGpuPriority = false; continue; }
        if (!strcmp(a, "--xgpu-transfer=frame")) { opt->frameTransfer = true; continue; }
        if (!strcmp(a, "--xgpu-transfer=prep")) { opt->frameTransfer = false; continue; }
        if (!strcmp(a, "--list-gpus")) { opt->listGpus = true; continue; }
        if (!strncmp(a, "--depth-gpu=", 12))
        {
            // Arguments arrive as UTF-8 (wmain converts); GPU names may be non-ASCII.
            if (!ParseGpuRequest(std::wstring(winrt::to_hstring(a + 12)), opt->depthGpu))
            { Log("ParseArgs: --depth-gpu must be same, auto, an adapter index or name:<GPU name>#<n>"); return false; }
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
    if (opt->benchRoom && !opt->selfTestOnly) { Log("ParseArgs: --bench-room only runs with --selftest, ignored"); opt->benchRoom = false; }
    if (opt->benchRoom)
        Log("ParseArgs: room benchmark after the self-test - left eye fov %.1f / %.1f / %.1f / %.1f deg (mirrored for the right), GPU clocks %s",
            opt->benchFov[0], opt->benchFov[1], opt->benchFov[2], opt->benchFov[3],
            opt->benchBoost ? "unlocked (--bench-boost)"
                            : (opt->benchLock ? "locked if Developer Mode allows, even while SteamVR's compositor runs (--bench-lock)"
                                              : "locked if Developer Mode allows and SteamVR's compositor is not running"));
    if (opt->roomV10Eye) Log("ParseArgs: the room's eye pass is the kept v10 copy (--room-v10-eye, A/B diagnostic)");
    if (opt->roomGlass > 0)
        Log("ParseArgs: room glass %d%% (frames, 1 m floor tiles and the ground beyond the glass)%s", opt->roomGlass,
            opt->room > 0 ? "" : " (it needs the room: --room above 0)");
    if (opt->roomReflect > 0)
        Log("ParseArgs: room reflections %d%% (the screen mirrored in the glass and the floor; frames and 1 m floor tiles)%s", opt->roomReflect,
            opt->room > 0 ? "" : " (it needs the room: --room above 0)");
    if (opt->roomLight > 0 || opt->roomLightColor != kRoomLightDefault)
        Log("ParseArgs: room light %d%%, colour #%06X%s", opt->roomLight, (unsigned)opt->roomLightColor,
            opt->room > 0 ? "" : " (it needs the room: --room above 0)");

    Log("ParseArgs: source %s (monitor %d, window '%ls', image '%ls') seconds %.0f",
        opt->source == SourceKind::Capture ? "capture" : opt->source == SourceKind::Image ? "image" : "synthetic",
        opt->monitorIndex, opt->windowTitle.c_str(), opt->imagePath.c_str(), opt->runSeconds);
    Log("ParseArgs: fill %s dilate %d vertical %d (-1 = per model)", opt->fillMode == FILL_MIRROR ? "mirror" : "stretch", opt->dilate, opt->dilateV);
    Log("ParseArgs: depth model %s (%dx%d)", opt->model->name, opt->model->inW, opt->model->inH);
    Log("ParseArgs: steady depth %s, model fusion %s, warp %s", opt->steady ? "on" : "off", opt->fuse ? "on" : "off",
        opt->subpixel ? "sub-pixel" : "whole pixels");
    const auto& g = opt->depthGpu;
    Log("ParseArgs: depth GPU %s%ls%s%s", g.kind == GpuRequest::Kind::Same ? "same as headset" : g.kind == GpuRequest::Kind::Auto ? "auto (another GPU)" :
        g.kind == GpuRequest::Kind::Index ? "adapter index" : "named: ", g.kind == GpuRequest::Kind::Named ? g.name.c_str() : L"",
        g.kind == GpuRequest::Kind::Named ? (g.nth ? " (not the first of that name)" : "") : "",
        g.kind == GpuRequest::Kind::Same ? "" : opt->frameTransfer ? ", frames sent by the copy engine" : ", prep on the headset GPU");
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
// All adapters in DXGI order, as gpu_choice.h sees them.
static std::vector<GpuEntry> EnumerateGpus(IDXGIFactory4* fac, const LUID* headsetLuid, std::vector<ComPtr<IDXGIAdapter1>>* adapters)
{
    std::vector<GpuEntry> gpus;
    if (!fac) return gpus;
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> each;
        if (fac->EnumAdapters1(i, &each) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        each->GetDesc1(&d);
        GpuEntry g;
        g.name = d.Description;
        g.memoryMB = (unsigned long long)(d.DedicatedVideoMemory >> 20);
        g.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        g.headset = headsetLuid && d.AdapterLuid.LowPart == headsetLuid->LowPart && d.AdapterLuid.HighPart == headsetLuid->HighPart;
        gpus.push_back(g);
        if (adapters) adapters->push_back(each);
    }
    return gpus;
}

// The adapter the depth request resolves to; false means stay on the headset GPU.
static bool PickSecondAdapter(App& app, IDXGIFactory4* fac, ComPtr<IDXGIAdapter1>& out)
{
    if (!fac) return false;
    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    const auto gpus = EnumerateGpus(fac, &app.adapterLuid, &adapters);
    for (size_t i = 0; i < gpus.size(); ++i)
        Log("PickSecondAdapter: adapter %zu %ls, %llu MB%s%s", i, gpus[i].name.c_str(), gpus[i].memoryMB,
            gpus[i].headset ? " [headset GPU]" : "", gpus[i].software ? " [software]" : "");
    std::wstring reason;
    const int chosen = ChooseDepthAdapter(gpus, app.opt.depthGpu, reason);
    if (chosen < 0) { Log("PickSecondAdapter: staying on the headset GPU - %ls", reason.c_str()); return false; }
    Log("PickSecondAdapter: adapter %d %ls (%ls)", chosen, gpus[size_t(chosen)].name.c_str(), reason.c_str());
    out = adapters[size_t(chosen)];
    return true;
}

// --list-gpus: one line per adapter for the desktop app's GPU picker:
//   GPU|<index>|<nth card with this name>|<dedicated MB>|<software 0/1>|<name>
// Runs before any VR runtime or model work, so it never starts SteamVR.
static int ListGpus()
{
    ComPtr<IDXGIFactory4> fac;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { printf("GPUERROR|CreateDXGIFactory1 failed\n"); return 1; }
    const auto gpus = EnumerateGpus(fac.Get(), nullptr, nullptr);
    for (size_t i = 0; i < gpus.size(); ++i)
        printf("GPU|%zu|%d|%llu|%d|%s\n", i, NthOfName(gpus, i), gpus[i].memoryMB, gpus[i].software ? 1 : 0,
            winrt::to_string(gpus[i].name).c_str());
    return 0;
}

// Creates the inference device, its queue and the cross-adapter hand-over:
// a shared heap (created on the headset GPU, opened on the inference GPU) holding
// one buffer large enough for any model's input, and a cross-adapter fence.
static bool InitSecondGpu(App& app, IDXGIFactory4* fac)
{
    Log("InitSecondGpu: enter");
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

static bool InitFrameTransfer(App& app);
static void ReleaseFrameTransfer(App& app);

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
    if (app.opt.depthGpu.kind != GpuRequest::Kind::Same && !InitSecondGpu(app, fac.Get()))
    {
        ReleaseSecondGpu(app);
        app.inferDevice = app.device;
        app.inferQueue = app.mlQueue;
        app.inferName = ad.Description;
        Log("InitD3D: second-GPU depth unavailable; depth runs on the headset GPU");
    }
    if (app.secondGpu && app.opt.frameTransfer && !InitFrameTransfer(app))
    {
        ReleaseFrameTransfer(app);
        Log("InitD3D: frame transfer unavailable; the second GPU receives prepared input instead");
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

// Prints warning-or-worse debug-layer messages for one device.
static void DumpDeviceMessages(ID3D12Device* device, const char* label)
{
    if (!device || !label) return;

    ComPtr<ID3D12InfoQueue> iq;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&iq)))) { Log("DumpDebugMessages[%s]: no info queue", label); return; }

    UINT64 n = iq->GetNumStoredMessages();
    Log("DumpDebugMessages[%s]: %llu stored message(s)", label, (unsigned long long)n);
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
    Log("DumpDebugMessages[%s]: %llu warning-or-worse shown", label, (unsigned long long)shown);
}

static void DumpDebugMessages(App& app)
{
    if (!app.opt.debugLayer) return;
    DumpDeviceMessages(app.device.Get(), "headset GPU");
    if (app.secondGpu && app.inferDevice) DumpDeviceMessages(app.inferDevice.Get(), "second GPU");
    // A removed device explains itself here rather than failing silently later.
    if (app.device && FAILED(app.device->GetDeviceRemovedReason())) Log("DumpDebugMessages: headset GPU REMOVED 0x%08lX", (unsigned long)app.device->GetDeviceRemovedReason());
    if (app.secondGpu && app.inferDevice && FAILED(app.inferDevice->GetDeviceRemovedReason()))
        Log("DumpDebugMessages: second GPU REMOVED 0x%08lX", (unsigned long)app.inferDevice->GetDeviceRemovedReason());
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
        if (!app.frameTransfer) continue;
        if (FAILED(c.dev->CreateShaderResourceView(c.tex11[i].Get(), nullptr, &c.srv11[i])))
        { Log("StartCapture: FAIL source view %d", i); return false; }
        hr = app.device->CreateSharedHandle(app.smallTex[i].Get(), nullptr, GENERIC_ALL, nullptr, &h);
        if (FAILED(hr)) { Log("StartCapture: FAIL CreateSharedHandle(model-size frame %d) 0x%08X", i, (unsigned)hr); return false; }
        hr = dev1->OpenSharedResource1(h, IID_PPV_ARGS(&c.small11[i]));
        CloseHandle(h);
        if (FAILED(hr)) { Log("StartCapture: FAIL OpenSharedResource1(model-size frame %d) 0x%08X", i, (unsigned)hr); return false; }
        if (FAILED(c.dev->CreateRenderTargetView(c.small11[i].Get(), nullptr, &c.smallRtv11[i])))
        { Log("StartCapture: FAIL model-size render target %d", i); return false; }
    }
    if (app.frameTransfer) Log("StartCapture: capture also makes %dx%d frames for the second GPU", app.xferW, app.xferH);
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
            // Second GPU: the model-size frame is made here, in work this GPU is
            // already doing for the picture, so depth adds no later wait on it.
            if (app->frameTransfer)
                winrt::check_hresult(c.scaler.Downscale(c.ctx.Get(), c.srv11[source->index].Get(), app->srcW,
                    c.smallRtv11[source->index].Get(), app->xferW, app->xferH));
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
        if (app.frameTransfer && shared && !MakeSourceTexture(app, app.xferW, app.xferH, app.srcFormat, true, app.smallTex[i])) return false;
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
    uint  subpixel;     // 1: sample at the fractional source position (no depth banding)
};

Texture2D<float4>        scene    : register(t0);
StructuredBuffer<float>  nearBuf  : register(t1);   // DW x DH, 0 = far .. 1 = near
RWTexture2DArray<float4> outColor : register(u0);
RWTexture2DArray<float>  outDepth : register(u1);   // D3D projective depth for nearZ/farZ
// [0,N): source x per dest, [N,2N): its nearness, [2N,3N): where that source pixel
// really lands (fractional), for sub-pixel sampling; N = CW*CH*2
RWStructuredBuffer<uint> scratch  : register(u2);
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
        float destF = (float)x;
        if (doWarp != 0)
        {
            float invZ = invZFar + n * (invZNear - invZFar);
            float s = scaleFocal * eye * invZ;
            int r = (s < 0.0) ? -(int)floor(-s + 0.5) : (int)floor(s + 0.5);   // lround
            dx = x - r;
            destF = (float)x - s;
        }
        if (dx >= 0 && dx < iw)
        {
            uint cur = scratch[base + dx];
            if (cur == NONE || n > asfloat(scratch[N + base + dx]))
            {
                scratch[base + dx] = (uint)x;
                scratch[N + base + dx] = asuint(n);
                scratch[2 * N + base + dx] = asuint(destF);
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
            [loop] for (h = x; h < r; h++) { scratch[base + h] = (uint)x; scratch[N + base + h] = asuint(xn); scratch[2 * N + base + h] = asuint((float)h); }
        }
        else
        {
            bool useLeft = lastValid >= 0 && !(rightValid >= 0 && rightNear < lastNear);
            int anchor = useLeft ? lastValid : rightValid;
            float anchorNear = useLeft ? lastNear : rightNear;

            if (fillMode == 0)
            {
                [loop] for (h = x; h < r; h++) { scratch[base + h] = (uint)anchor; scratch[N + base + h] = asuint(anchorNear); scratch[2 * N + base + h] = asuint((float)h); }
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
                    scratch[2 * N + base + h] = asuint((float)h);
                }
            }
        }
        x = r;      // lastValid/lastNear deliberately unchanged: fills are not sources
    }

    [loop] for (x = 0; x < iw; x++)
    {
        int s = (int)scratch[base + x];
        float4 col;
        if (subpixel != 0)
        {
            // The source pixel `s` lands at scratch[2N+...]; the content that lands
            // exactly here is at s + (x - that). Blended in float rather than by the
            // sampler, so the GPU matches the CPU reference (SelfTestWarp).
            float pos = clamp((float)s + ((float)x - asfloat(scratch[2 * N + base + x])), 0.0, (float)(iw - 1));
            int i0 = (int)pos;
            int i1 = min(i0 + 1, iw - 1);
            float fr = pos - (float)i0;
            float4 c0, c1;
            if (exactLoad != 0) { c0 = scene.Load(int3(i0, y, 0)); c1 = scene.Load(int3(i1, y, 0)); }
            else
            {
                c0 = scene.SampleLevel(samp, float2(((float)i0 + 0.5) / (float)CW, ((float)y + 0.5) / (float)CH), 0);
                c1 = scene.SampleLevel(samp, float2(((float)i1 + 0.5) / (float)CW, ((float)y + 0.5) / (float)CH), 0);
            }
            col = lerp(c0, c1, fr);
        }
        else if (exactLoad != 0) col = scene.Load(int3(s, y, 0));
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

// Source texture -> the depth grid (DW x DH) as packed RGBA8 (R in the low byte), with
// kPrepHlsl's box filter. Read back for steadying/fusion: its luma drives the hardware
// motion estimator and its colour is the anchor model's input. Same constants as prep.
static const char* kGridHlsl = R"HLSL(
cbuffer C : register(b0)
{
    uint DW; uint DH; uint taps; uint exactLoad;
    float cropX; float cropY; float cropSize; uint normalize;
};

Texture2D<float4>        scene : register(t0);
RWStructuredBuffer<uint> grid  : register(u0);
SamplerState             samp  : register(s0);

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
    uint3 c = (uint3)round(saturate(v) * 255.0);
    grid[id.y * DW + id.x] = c.r | (c.g << 8) | (c.b << 16) | (255u << 24);
}
)HLSL";

// Inference GPU, frame transfer: received BGRA8 frame (SW x SH rows, `pitch` bytes
// apart) -> the model's NCHW float input (DW x DH). Bilinear with pixel centres and
// clamped edges when the sizes differ (same convention as SampleDepth), normalised
// like kPrepHlsl.
static const char* kUnpackHlsl = R"HLSL(
cbuffer C : register(b0)
{
    uint SW; uint SH; uint pitch; uint DW;
    uint DH; uint normalize; uint pad0; uint pad1;
};

ByteAddressBuffer         frame   : register(t0);
RWStructuredBuffer<float> modelIn : register(u0);

float3 Fetch(int x, int y)
{
    x = clamp(x, 0, (int)SW - 1);
    y = clamp(y, 0, (int)SH - 1);
    uint v = frame.Load(y * pitch + x * 4);           // bytes B, G, R, A
    return float3((v >> 16) & 255, (v >> 8) & 255, v & 255) / 255.0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DW || id.y >= DH) return;

    float3 c;
    if (SW == DW && SH == DH)
    {
        c = Fetch(id.x, id.y);
    }
    else
    {
        float fx = clamp(((float)id.x + 0.5) * (float)SW / (float)DW - 0.5, 0.0, (float)SW - 1);
        float fy = clamp(((float)id.y + 0.5) * (float)SH / (float)DH - 0.5, 0.0, (float)SH - 1);
        int x0 = (int)fx, y0 = (int)fy;
        float a = fx - x0, b = fy - y0;
        float3 top = Fetch(x0, y0) * (1 - a) + Fetch(x0 + 1, y0) * a;
        float3 bot = Fetch(x0, y0 + 1) * (1 - a) + Fetch(x0 + 1, y0 + 1) * a;
        c = top * (1 - b) + bot * b;
    }

    const float3 mean = float3(0.485, 0.456, 0.406);
    const float3 istd = float3(1.0 / 0.229, 1.0 / 0.224, 1.0 / 0.225);
    float3 o = normalize != 0 ? (c - mean) * istd : c;
    uint plane = DW * DH;
    uint p = id.y * DW + id.x;
    modelIn[p] = o.r;
    modelIn[plane + p] = o.g;
    modelIn[2 * plane + p] = o.b;
}
)HLSL";

// Source picture -> the glow texture around the screen (see ambilight.h, which
// holds the same arithmetic as the CPU reference SelfTestAmbilight compares with).
static const char* kAmbiHlsl = R"HLSL(
// Compiled twice: with AMBI_RING defined it builds the ring (one thread per point),
// without it the glow (one thread per glow pixel). See ambilight.h.
cbuffer C : register(b0)
{
    uint  GW; uint GH; uint SRCW; uint SRCH;
    float rectW; float rectH; float screenW; float screenH;
    float marginM; float intensity; float blend; uint reset;
    float soft; float bezel; uint ringN; uint linearBlend;
};

// srgb.h: the sRGB transfer function, for blending in linear light.
float3 Dec(float3 c) { return c <= 0.04045 ? c / 12.92 : pow(max((c + 0.055) / 1.055, 0.0), 2.4); }
float3 Enc(float3 l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(max(l, 0.0), 1.0 / 2.4) - 0.055; }

static const int TAPS = 4;          // kAmbiTaps
static const float DEPTH = 0.06;    // kAmbiDepth

Texture2D<float4>          scene : register(t0);
RWTexture2D<float4>        glow  : register(u0);
RWStructuredBuffer<float4> ring  : register(u1);   // per point: colour, then position (x, y)
RWTexture2D<float4>        hist  : register(u2);   // the glow at 16-bit float: the temporal blend's memory

void Place(uint i, out float x, out float y, out float tx, out float ty, out float nx, out float ny)
{
    float W = screenW, H = screenH;
    float s = ((float)i + 0.5) / (float)ringN * (2.0 * (W + H));
    tx = 0; ty = 0; nx = 0; ny = 0;
    if (s < W) { x = -0.5 * W + s; y = 0.5 * H; tx = 1; ny = -1; return; }
    s -= W;
    if (s < H) { x = 0.5 * W; y = 0.5 * H - s; ty = -1; nx = -1; return; }
    s -= H;
    if (s < W) { x = 0.5 * W - s; y = -0.5 * H; tx = -1; ny = 1; return; }
    s -= W;
    x = -0.5 * W; y = -0.5 * H + s; ty = 1; nx = 1;
}

#ifdef AMBI_RING
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= ringN) return;
    float x, y, tx, ty, nx, ny;
    Place(i, x, y, tx, ty, nx, ny);
    float spacing = 2.0 * (screenW + screenH) / (float)ringN;
    float depth = DEPTH * (screenW < screenH ? screenW : screenH);
    float3 sum = float3(0, 0, 0);
    [unroll] for (int j = 0; j < TAPS; j++)
    {
        [unroll] for (int k = 0; k < TAPS; k++)
        {
            float along = (((float)k + 0.5) / (float)TAPS - 0.5) * spacing;
            float inw = ((float)j + 0.5) / (float)TAPS * depth;
            float qx = x + tx * along + nx * inw, qy = y + ty * along + ny * inw;
            int px = clamp((int)floor((qx / screenW + 0.5) * (float)SRCW - 0.5 + 0.5), 0, (int)SRCW - 1);
            int py = clamp((int)floor((0.5 - qy / screenH) * (float)SRCH - 0.5 + 0.5), 0, (int)SRCH - 1);
            sum += scene.Load(int3(px, py, 0)).rgb;
        }
    }
    ring[i * 2] = float4(sum / (float)(TAPS * TAPS), 0);
    ring[i * 2 + 1] = float4(x, y, 0, 0);
}
#else
groupshared float4 sRing[2 * 256];  // kAmbiRingMax

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    for (uint k = gi; k < ringN * 2; k += 64) sRing[k] = ring[k];
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= GW || id.y >= GH) return;

    float px = (((float)id.x + 0.5) / (float)GW - 0.5) * rectW;
    float py = (0.5 - ((float)id.y + 0.5) / (float)GH) * rectH;
    float ox = abs(px) - 0.5 * screenW, oy = abs(py) - 0.5 * screenH;
    float dx = ox > 0.0 ? ox : 0.0, dy = oy > 0.0 ? oy : 0.0;
    float dist = sqrt(dx * dx + dy * dy) / marginM;

    float4 result = float4(0, 0, 0, 0);
    if (dist > 0.0 && dist < 1.0)
    {
        float a = (1.0 - dist) * (1.0 - dist) * intensity;
        if (bezel > 0.0)
        {
            float tb = (dist / bezel < 1.0) ? dist / bezel : 1.0;
            a *= tb * tb * (3.0 - 2.0 * tb);
        }
        // Every border point lights this one, the nearest most: the glow follows the
        // local colour next to the screen and softens further out.
        float soft2 = soft * soft;
        float sumW = 0.0;
        float3 sum = float3(0, 0, 0);
        [loop] for (uint i = 0; i < ringN; i++)
        {
            float4 pos = sRing[i * 2 + 1];
            float rx = px - pos.x, ry = py - pos.y;
            float r2 = rx * rx + ry * ry + soft2;
            float w = 1.0 / (r2 * sqrt(r2));
            sumW += w;
            sum += w * sRing[i * 2].rgb;
        }
        // Premultiplied, in linear light for an sRGB swapchain (the compositor
        // decodes, then blends); right over black even if it ignores the alpha.
        if (sumW > 0.0)
        {
            float3 colour = sum / sumW;
            result = float4(linearBlend != 0 ? Enc(Dec(colour) * a) : colour * a, a);
        }
    }
    // The blend keeps its history at 16-bit float: in 8 bits a fade-out stalls a few
    // steps short of its target and leaves a tint behind.
    float4 prev = hist[id.xy];
    float4 next = (reset != 0) ? result : prev + (result - prev) * blend;
    hist[id.xy] = next;
    glow[id.xy] = next;
}
#endif
)HLSL";

// Curved screen: one ray per sample from the eye through its pixel, against the
// cylinder (screen_curve.h, CylinderHit), then against the glow just behind it.
// The same arithmetic as CurvedPixel, which SelfTestCurve compares with.
static const char* kCurveHlsl = R"HLSL(
cbuffer C : register(b0)
{
    uint  EW; uint EH; uint glowOn; uint linearBlend;
    float R; float halfWrap; float halfW; float halfH;
    float glowHalfW; float glowHalfH; float glowRadius; float pad1;
    float4 world;                   // the world colour (rgb), as stored
    float4 eyeData[10];             // per eye: origin, rotation rows 0..2, (tanL, tanR, tanU, tanD)
};

Texture2DArray<float4>   picture : register(t0);    // each eye's depth-warped picture
Texture2D<float4>        glow    : register(t1);    // premultiplied ambilight glow
RWTexture2DArray<float4> outEye  : register(u0);
SamplerState             samp    : register(s0);

static const float2 SUB[4] = { float2(-0.375, -0.125), float2(0.125, -0.375), float2(0.375, 0.125), float2(-0.125, 0.375) };

// srgb.h: the sRGB transfer function, for blending in linear light.
float3 Dec(float3 c) { return c <= 0.04045 ? c / 12.92 : pow(max((c + 0.055) / 1.055, 0.0), 2.4); }
float3 Enc(float3 l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(max(l, 0.0), 1.0 / 2.4) - 0.055; }

void Ray(uint e, float px, float py, out float3 o, out float3 d)
{
    float4 tans = eyeData[e * 5 + 4];
    float tx = tans.x + px / (float)EW * (tans.y - tans.x);
    float ty = tans.z + py / (float)EH * (tans.w - tans.z);
    float4 r0 = eyeData[e * 5 + 1], r1 = eyeData[e * 5 + 2], r2 = eyeData[e * 5 + 3];
    o = eyeData[e * 5 + 0].xyz;
    d = float3(r0.x * tx + r0.y * ty - r0.z, r1.x * tx + r1.y * ty - r1.z, r2.x * tx + r2.y * ty - r2.z);
}

// ArcHit in screen_curve.h: the first hit on a vertical cylinder of radius rho round
// the axis at z = axisZ, on the far side, within the angle and height limits.
bool ArcHit(float axisZ, float rho, float maxPhi, float maxY, float3 o, float3 d, out float phi, out float y)
{
    phi = 0; y = 0;
    float a = d.x * d.x + d.z * d.z;
    if (!(a > 1e-12)) return false;
    float b = 2.0 * (o.x * d.x + (o.z - axisZ) * d.z);
    float c = o.x * o.x + o.z * o.z - 2.0 * o.z * axisZ + (axisZ - rho) * (axisZ + rho);
    float disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return false;
    float sq = sqrt(disc);
    float q = -0.5 * (b + (b < 0.0 ? -sq : sq));
    float t0 = q / a;
    float t1 = (q != 0.0) ? c / q : t0;
    if (t0 > t1) { float sw = t0; t0 = t1; t1 = sw; }
    [unroll] for (int i = 0; i < 2; i++)
    {
        float t = (i == 0) ? t0 : t1;
        if (!(t > 0.0)) continue;
        float hx = o.x + t * d.x, hy = o.y + t * d.y, hz = o.z + t * d.z;
        float toward = axisZ - hz;
        if (!(toward > 0.0)) continue;
        float angle = atan2(hx, toward);
        if (abs(angle) > maxPhi || abs(hy) > maxY) continue;
        phi = angle; y = hy;
        return true;
    }
    return false;
}

bool Screen(float3 o, float3 d, out float2 uv)
{
    uv = float2(0, 0);
    float phi, y;
    if (!ArcHit(R, R, halfWrap, halfH, o, d, phi, y)) return false;
    uv = float2((R * phi) / (2.0 * halfW) + 0.5, 0.5 - y / (2.0 * halfH));
    return true;
}

bool Glow(float3 o, float3 d, out float2 uv)
{
    uv = float2(0, 0);
    if (glowOn == 0) return false;
    float phi, y;
    if (!ArcHit(R, glowRadius, glowHalfW / R, glowHalfH, o, d, phi, y)) return false;
    uv = float2((R * phi) / (2.0 * glowHalfW) + 0.5, 0.5 - y / (2.0 * glowHalfH));
    return true;
}

// CurveSampleColour: the picture; the glow over the world colour; or the world.
float3 Colour(int kind, float2 uv, uint e)
{
    float3 c = world.rgb;
    if (kind == 0) c = picture.SampleLevel(samp, float3(uv, (float)e), 0).rgb;
    else if (kind == 1)
    {
        float4 g = glow.SampleLevel(samp, uv, 0);
        c = linearBlend != 0 ? Enc(Dec(g.rgb) + Dec(world.rgb) * (1.0 - g.a)) : g.rgb + world.rgb * (1.0 - g.a);
    }
    return c;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= EW || id.y >= EH) return;
    uint e = id.z;

    int kind[4];
    float2 uv[4];
    [unroll] for (int s = 0; s < 4; s++)
    {
        float3 o, d;
        Ray(e, (float)id.x + 0.5 + SUB[s].x, (float)id.y + 0.5 + SUB[s].y, o, d);
        float2 hit;
        if (Screen(o, d, hit)) { kind[s] = 0; uv[s] = hit; }
        else if (Glow(o, d, hit)) { kind[s] = 1; uv[s] = hit; }
        else { kind[s] = 2; uv[s] = float2(0, 0); }
    }

    // All four agree (almost every pixel): one sample. At an edge: all four.
    float3 col = float3(0, 0, 0);
    if (kind[0] == kind[1] && kind[1] == kind[2] && kind[2] == kind[3])
    {
        float mu = (uv[0].x + uv[1].x + uv[2].x + uv[3].x) * 0.25;
        float mv = (uv[0].y + uv[1].y + uv[2].y + uv[3].y) * 0.25;
        col = Colour(kind[0], float2(mu, mv), e);
    }
    else
    {
        [unroll] for (int k = 0; k < 4; k++) col += Colour(kind[k], uv[k], e) * 0.25;
    }
    outEye[uint3(id.x, id.y, e)] = float4(col, 1.0);
}
)HLSL";

// ------------------------------------------------------------------------ room
// The room lit by the screen and the ambilight (room.h, XROOM.md). Its shaders are
// built from pieces so that each carries exactly what it needs: the constants and
// the geometry are shared by the lightmap passes (kRoomHlsl, compiled with ROOM_EMIT
// for the first pass) and by the room's eye pass (kCurveHlsl's code up to its main,
// then kCurveRoomHlsl). The plain curve pass is compiled from kCurveHlsl alone, so
// it is exactly what it was.
// Every room shader starts with room.h's RoomHlslDefines(): the constants it shares
// with the CPU (ROOM_PI, ROOM_FACE_*, ROOM_FLAG_*, ROOM_KIND_*, ...), generated from
// the C++ values so that the two cannot drift.

// RoomConstants (room.h), as a constant buffer; ROOM_REGISTER is b0 in the lightmap
// passes and b1 in the eye pass (whose b0 is the curve's root constants). Rows 0-10 are
// v10's; row 11 is the finish (how clear the glass is, the reflections and the coat's mean
// reflectance - not "reflect", an HLSL intrinsic), row 12 the frames (the bays and the
// crossbar); rows 13-15 are the room light (its panel, its emitted radiance with w = the
// panel fits, its seen radiance); row 16 is the mirror picture's size in use (0 while
// Reflections is 0); rows 17-20 are the eye pass's reciprocals and the screen's bounds.
// The 512-byte buffer's rows 21-31 are padding.
static const char* kRoomCbufferHlsl = R"HLSL(
cbuffer RoomC : register(ROOM_REGISTER)
{
    float X; float yF; float yC; float zB;
    float gap; float roomR; float Rg; float phiA;
    float sinA; float cosA; float xa; float za;
    float sMax; float zSide; float roomGlowHalfW; float roomGlowHalfH;
    float rhoWall; float rhoFloor; float rhoCeiling; float bounceScale;
    float4 roomWorld;
    float shadeFloor; float shadeCeiling; float alpha; float screenW;
    float screenH; float rpad0; float rpad1; float rpad2;
    uint flags; uint gridX; uint gridY; uint emitterCount;
    uint srcW; uint srcH; uint stride; uint glowW;
    uint glowH; uint blocksX; uint blocksY; uint glowBlock;
    float roomGlass; float roomReflect; float roomFbar; float rpad3;
    float pitchSide; float pitchBack; float transomY; float rpad4;
    float lightX0; float lightX1; float lightZ0; float lightZ1;
    float4 lightL; float4 lightSeen;
    uint mirrorW; uint mirrorH; uint upad0; uint upad1;
    float invH; float inv2X; float invSide; float invFloorZ;
    float inv2sMax; float invGlowW; float invGlowH; float tanA;
    float wingS; float scrTanWrap; float scrBoxX; float scrBoxY;
    float scrBoxZ; float rpad6; float rpad7; float rpad8;
};
)HLSL";

// The room's shape (room.h: FrontDepth, FrontS, FrontPoint, RoomFacePoint, RoomInside).
static const char* kRoomGeomHlsl = R"HLSL(
static const float PI = ROOM_PI;

float FrontDepthH(float x)
{
    if ((flags & ROOM_FLAG_CURVED) == 0) return -gap;
    float ax = abs(x);
    if (ax <= xa) return roomR - sqrt(max(Rg * Rg - ax * ax, 0.0));
    return za + (ax - xa) * (sinA / cosA);
}

float FrontSH(float x)
{
    if ((flags & ROOM_FLAG_CURVED) == 0) return x;
    float ax = abs(x), sgn = x < 0.0 ? -1.0 : 1.0;
    if (ax <= xa) return sgn * roomR * asin(min(ax / Rg, 1.0));
    return sgn * (roomR * phiA + (ax - xa) / cosA * (roomR / Rg));
}

void FrontPointH(float s, out float x, out float z, out float nx, out float nz)
{
    if ((flags & ROOM_FLAG_CURVED) == 0) { x = s; z = -gap; nx = 0.0; nz = 1.0; return; }
    float sa = abs(s), sgn = s < 0.0 ? -1.0 : 1.0;
    if (sa <= roomR * phiA)
    {
        float phi = sa / roomR;
        x = sgn * Rg * sin(phi); z = roomR - Rg * cos(phi);
        nx = -sgn * sin(phi); nz = cos(phi);
        return;
    }
    float along = (sa - roomR * phiA) * (Rg / roomR);
    x = sgn * (xa + along * cosA); z = za + along * sinA;
    nx = -sgn * sinA; nz = cosA;
}

void FacePointH(uint face, float u, float v, out float3 p, out float3 n)
{
    float h = yC - yF;
    p = float3(0, 0, 0); n = float3(0, 0, 0);
    if (face == ROOM_FACE_FRONT)
    {
        float x, z, nx, nz;
        FrontPointH(-sMax + u * 2.0 * sMax, x, z, nx, nz);
        p = float3(x, yC - v * h, z); n = float3(nx, 0.0, nz);
    }
    else if (face == ROOM_FACE_LEFT) { p = float3(-X, yC - v * h, zSide + u * (zB - zSide)); n = float3(1, 0, 0); }
    else if (face == ROOM_FACE_RIGHT) { p = float3(X, yC - v * h, zSide + u * (zB - zSide)); n = float3(-1, 0, 0); }
    else if (face == ROOM_FACE_FLOOR) { p = float3(-X + u * 2.0 * X, yF, -gap + v * (zB + gap)); n = float3(0, 1, 0); }
    else if (face == ROOM_FACE_CEILING) { p = float3(-X + u * 2.0 * X, yC, -gap + v * (zB + gap)); n = float3(0, -1, 0); }
    else { p = float3(-X + u * 2.0 * X, yC - v * h, zB); n = float3(0, 0, -1); }
}

bool InsideH(float3 p)
{
    return abs(p.x) < X && p.y > yF && p.y < yC && p.z < zB && p.z > FrontDepthH(p.x);
}
)HLSL";

// The lightmap passes. ROOM_EMIT: one 256-thread group per emitter works out its
// radiance (RoomEmitRadiance: exact pixel means through the decode table, summed in
// the same order as the CPU); the room light, the last emitter, takes the constants'.
// ROOM_MIRROR (only with Reflections above 0): one thread per texel of the mirror picture
// (RoomMirrorPicture). Otherwise: one thread per lightmap texel gathers every emitter
// (RoomTexel), streaming them through group-shared memory.
static const char* kRoomHlsl = R"HLSL(
Texture2D<float4>          scene    : register(t0);
StructuredBuffer<float>    lut      : register(t1);   // sRGB decode of each byte
Texture2D<float4>          glow     : register(t2);   // the ambilight (8-bit, premultiplied, encoded)
RWStructuredBuffer<float4> emit     : register(u0);   // 6 per emitter: 4 corners, normal, radiance
RWTexture2DArray<float4>   lightmap : register(u1);   // 6 faces x 64 x 64, linear radiance
RWTexture2D<float4>        mirrorOut : register(u2);  // the mirror picture, linear (mirrorW x mirrorH in use)

float3 Decode(float3 v)
{
    uint3 b = (uint3)(saturate(v) * 255.0 + 0.5);
    return float3(lut[b.x], lut[b.y], lut[b.z]);
}

#ifdef ROOM_EMIT
groupshared float3 sPart[ROOM_GROUP_THREADS];

[numthreads(ROOM_GROUP_THREADS, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    uint e = gid.x;
    if (e >= emitterCount) return;
    // The room light (room.h RoomEmitRadiance's lightL), before the glow's branch: the lamp
    // is active whenever its panel fits, and as a glow block its first row would lie past
    // the glow's last (glowH - gy0 wraps round). Uniform per group, so no thread is left
    // at the barriers below.
    uint lampIndex = gridX * gridY + blocksX * blocksY;
    if (e >= lampIndex)
    {
        if (gi == 0) emit[e * ROOM_EMITTER_FLOAT4S + 5] = float4(lightL.rgb, 0.0);
        return;
    }
    uint patches = gridX * gridY;
    bool isScreen = e < patches;
    float3 sum = float3(0, 0, 0);
    uint count = 0;
    if (isScreen)
    {
        uint i = e % gridX, j = e / gridX;
        uint x0 = (i * srcW) / gridX, x1 = ((i + 1) * srcW) / gridX;
        uint y0 = (j * srcH) / gridY, y1 = ((j + 1) * srcH) / gridY;
        uint nx = (x1 - x0 + stride - 1) / stride, ny = (y1 - y0 + stride - 1) / stride;
        count = nx * ny;
        for (uint k = gi; k < count; k += ROOM_GROUP_THREADS)
            sum += Decode(scene.Load(int3(x0 + (k % nx) * stride, y0 + (k / nx) * stride, 0)).rgb);
    }
    else if ((flags & ROOM_FLAG_GLOW) != 0 && emit[e * ROOM_EMITTER_FLOAT4S + 4].w != 0.0)
    {
        uint b = e - patches;
        uint gx0 = (b % blocksX) * glowBlock, gy0 = (b / blocksX) * glowBlock;
        uint bw = min(glowBlock, glowW - gx0), bh = min(glowBlock, glowH - gy0);
        count = bw * bh;
        for (uint k = gi; k < count; k += ROOM_GROUP_THREADS)
            sum += Decode(glow.Load(int3(gx0 + k % bw, gy0 + k / bw, 0)).rgb);
    }
    sPart[gi] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint st = ROOM_GROUP_THREADS / 2; st > 0; st >>= 1)
    {
        if (gi < st) sPart[gi] += sPart[gi + st];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        float3 mean = count > 0 ? sPart[0] / (float)count : float3(0, 0, 0);
        float3 L = emit[e * ROOM_EMITTER_FLOAT4S + 5].rgb;
        L = isScreen ? L + (mean - L) * alpha : mean;
        emit[e * ROOM_EMITTER_FLOAT4S + 5] = float4(L, 0.0);
    }
}
#elif defined(ROOM_MIRROR)
// room.h RoomMirrorPicture: texel (i, j) of the mirror picture is the exact mean of its
// RoomGridBox of source pixels, every stride-th, through the decode table, summed row by
// row with a precise accumulator in the CPU's order.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= mirrorW || id.y >= mirrorH) return;
    uint x0 = (id.x * srcW) / mirrorW, x1 = ((id.x + 1) * srcW) / mirrorW;
    uint y0 = (id.y * srcH) / mirrorH, y1 = ((id.y + 1) * srcH) / mirrorH;
    precise float3 sum = float3(0, 0, 0);
    uint count = 0;
    for (uint y = y0; y < y1; y += stride)
        for (uint x = x0; x < x1; x += stride)
        {
            sum += Decode(scene.Load(int3(x, y, 0)).rgb);
            count++;
        }
    mirrorOut[id.xy] = float4(count > 0 ? sum / (float)count : float3(0, 0, 0), 1.0);
}
#else
groupshared float4 sEm[128 * ROOM_EMITTER_FLOAT4S];

// room.h RoomLambertQuad: pi x the exact form factor to a quad.
float LambertQuad(float3 p, float3 n, float3 c0, float3 c1, float3 c2, float3 c3)
{
    float3 c[4] = { c0, c1, c2, c3 };
    float3 u[4];
    [unroll] for (int k = 0; k < 4; k++)
    {
        float3 v = c[k] - p;
        float len = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (!(len > 1e-9)) return 0.0;
        u[k] = v / len;
    }
    float sum = 0.0;
    [unroll] for (int k2 = 0; k2 < 4; k2++)
    {
        float3 a = u[k2], b = u[(k2 + 1) & 3];
        float cx = a.y * b.z - a.z * b.y, cy = a.z * b.x - a.x * b.z, cz = a.x * b.y - a.y * b.x;
        float cl = sqrt(cx * cx + cy * cy + cz * cz);
        if (!(cl > 1e-12)) continue;
        float theta = atan2(cl, a.x * b.x + a.y * b.y + a.z * b.z);
        sum += theta * (n.x * cx + n.y * cy + n.z * cz) / cl;
    }
    return 0.5 * abs(sum);
}

// room.h RoomFormFactor, for emitter m of the shared chunk.
float FormFactorS(float3 p, float3 n, uint m)
{
    uint m6 = m * ROOM_EMITTER_FLOAT4S;
    float4 q0 = sEm[m6 + 0], q1 = sEm[m6 + 1], q2 = sEm[m6 + 2], q3 = sEm[m6 + 3], nq = sEm[m6 + 4];
    float cx = 0.5 * (q0.x + q2.x), cy = 0.5 * (q0.y + q2.y), cz = 0.5 * (q0.z + q2.z);
    float3 v = float3(cx - p.x, cy - p.y, cz - p.z);
    float np = n.x * v.x + n.y * v.y + n.z * v.z;
    float nqv = -(nq.x * v.x + nq.y * v.y + nq.z * v.z);
    if (!(np > 0.0) || !(nqv > 0.0)) return 0.0;
    float r2 = v.x * v.x + v.y * v.y + v.z * v.z;
    float area = q0.w, diag2 = q1.w;
    if (r2 >= 4.0 * diag2) return area * (np * nqv / r2) / (r2 + area / PI);
    return LambertQuad(p, n, q0.xyz, q1.xyz, q2.xyz, q3.xyz);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint face = id.z;
    float3 p, n;
    FacePointH(face, ((float)id.x + 0.5) / (float)ROOM_LIGHTMAP, ((float)id.y + 0.5) / (float)ROOM_LIGHTMAP, p, n);
    // room.h RoomLightPoint: floor and ceiling texels behind a curved front are lit
    // from just inside it (bilinear sampling at the wall's base blends them in).
    if ((flags & ROOM_FLAG_CURVED) != 0 && (face == ROOM_FACE_FLOOR || face == ROOM_FACE_CEILING))
    {
        float front = FrontDepthH(p.x) + ROOM_INSIDE_FRONT;
        if (p.z < front) p.z = front;
    }
    float3 E = float3(0, 0, 0), flux = float3(0, 0, 0);
    for (uint base = 0; base < emitterCount; base += 128)
    {
        uint chunk = min(128u, emitterCount - base);
        for (uint k = gi; k < chunk * ROOM_EMITTER_FLOAT4S; k += 64) sEm[k] = emit[base * ROOM_EMITTER_FLOAT4S + k];
        GroupMemoryBarrierWithGroupSync();
        for (uint m = 0; m < chunk; m++)
        {
            if (sEm[m * ROOM_EMITTER_FLOAT4S + 4].w == 0.0) continue;
            float G = FormFactorS(p, n, m);
            float a = PI * sEm[m * ROOM_EMITTER_FLOAT4S].w;
            float3 L = sEm[m * ROOM_EMITTER_FLOAT4S + 5].rgb;
            E += L * G;
            flux += L * a;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    float rho = face == ROOM_FACE_FLOOR ? rhoFloor : (face == ROOM_FACE_CEILING ? rhoCeiling : rhoWall);
    float shade = face == ROOM_FACE_FLOOR ? shadeFloor : (face == ROOM_FACE_CEILING ? shadeCeiling : 1.0);
    float3 Lt = (rho / PI) * (E + flux * bounceScale) + shade * roomWorld.rgb;
    lightmap[uint3(id.x, id.y, face)] = float4(Lt, 1.0);
}
#endif
)HLSL";

// The room's eye pass: appended to kCurveHlsl's code before its main (so Ray, Dec and
// Enc are shared), after the room's constants and geometry. It tests the screen and the
// room with its own functions: kCurveHlsl's Screen and ArcHit stay as the plain pass has
// them. room.h on the CPU: RoomScreenHit, RoomArcRoot, RoomExitFast, RoomExitOnFace,
// RoomFillHit, RoomClassifyRay, RoomRayDiff, RoomPlaneFootprint, RoomBar, RoomPulse,
// RoomCeilingPanel, RoomFrameCoverage, RoomHash, RoomTileFactor, RoomEnvironment,
// RoomFresnel, RoomMirror::Sample, RoomScreenWideHit, RoomReflection, RoomSurface,
// RoomSampleColour and RoomPixel.
// It is compiled twice (InitShaders): with ROOM_LOOK 0 (curveRoomPso), the v11 controls'
// code is left out, which makes it Stage 1's pass exactly, for a room whose controls are
// all 0; with ROOM_LOOK 1 (curveRoomLookPso), for a room with the room light, the finish
// (Glass) or the reflections on (RoomLookOn). Only the look variant reads the mirror
// picture (t3). The light's code, even behind its flag, cost the plain pass
// registers and about 4% of its time (curved, 60%: 0.916 against 0.876 ms eye min).
static const char* kCurveRoomHlsl = R"HLSL(
Texture2DArray<float4> lightmap : register(t2);
#if ROOM_LOOK
Texture2D<float4> mirrorPic : register(t3);          // the mirror picture (MIRROR), its first mirrorH rows
#endif

static const int BAYER[64] = ROOM_BAYER;           // room.h kRoomBayer

// room.h RoomHit: the face, the surface it lies on (the face, or a curved front's wing),
// its chart coordinates and the glow's (s, y).
struct RoomHitH { int face; int part; float u; float v; float s; float y; };

float3 RoomAt(float3 o, float3 d, float t)
{
    return float3(o.x + t * d.x, o.y + t * d.y, o.z + t * d.z);
}

// room.h FrontDepth, with the wings' slope from the constants.
float FrontDepthR(float x)
{
    if ((flags & ROOM_FLAG_CURVED) == 0) return -gap;
    float ax = abs(x);
    if (ax <= xa) return roomR - sqrt(max(Rg * Rg - ax * ax, 0.0));
    return za + (ax - xa) * tanA;
}

// room.h RoomArcRoot: ArcHit's quadratic, returning the root itself, with the angle
// limit as a tangent (|hx| <= tanMax * toward) in place of an atan2 per root.
bool RoomArcRoot(float axisZ, float rho, float tanMax, float maxY, float3 o, float3 d, out float t, out float3 h)
{
    t = 0.0; h = float3(0, 0, 0);
    float a = d.x * d.x + d.z * d.z;
    if (!(a > 1e-12)) return false;
    float b = 2.0 * (o.x * d.x + (o.z - axisZ) * d.z);
    float c = o.x * o.x + o.z * o.z - 2.0 * o.z * axisZ + (axisZ - rho) * (axisZ + rho);
    float disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return false;
    float sq = sqrt(disc);
    float q = -0.5 * (b + (b < 0.0 ? -sq : sq));
    float t0 = q / a;
    float t1 = (q != 0.0) ? c / q : t0;
    if (t0 > t1) { float sw = t0; t0 = t1; t1 = sw; }
    [unroll] for (int i = 0; i < 2; i++)
    {
        float tr = (i == 0) ? t0 : t1;
        if (!(tr > 0.0)) continue;
        float x = o.x + tr * d.x, y = o.y + tr * d.y, z = o.z + tr * d.z;
        float toward = axisZ - z;
        if (!(toward > 0.0) || abs(y) > maxY || abs(x) > tanMax * toward) continue;
        t = tr; h = float3(x, y, z);
        return true;
    }
    return false;
}

// room.h RoomScreenHit: the slab test against the screen's box, then the quadratic with
// the tan test, and atan2 only on a hit, for u.
bool RoomScreenH(float3 o, float3 d, float3 invD, out float2 uv)
{
    uv = float2(0, 0);
    if (!(R > 0.0)) return false;
    float3 t1 = (float3(-scrBoxX, -scrBoxY, -ROOM_SCREEN_PAD) - o) * invD, t2 = (float3(scrBoxX, scrBoxY, scrBoxZ) - o) * invD;
    float3 tn = min(t1, t2), tf = max(t1, t2);
    float enter = max(max(tn.x, tn.y), tn.z), leave = min(min(tf.x, tf.y), tf.z);
    if (!(leave >= max(enter, 0.0))) return false;
    float t;
    float3 h;
    if (!RoomArcRoot(R, R, scrTanWrap, halfH, o, d, t, h)) return false;
    float phi = atan2(h.x, R - h.z);
    uv = float2((R * phi) / (2.0 * halfW) + 0.5, 0.5 - h.y / (2.0 * halfH));
    return true;
}

// room.h RoomWingT: the wing on `side` (-1 left, +1 right); 0 if the ray does not leave there.
float RoomWingT(float side, float3 o, float3 d)
{
    float wnx = -side * sinA, wnz = cosA;
    float nd = wnx * d.x + wnz * d.z;
    if (!(nd < 0.0)) return 0.0;
    float tw = (wnx * (side * xa - o.x) + wnz * (za - o.z)) / nd;
    if (side * (o.x + tw * d.x) < xa) return 0.0;
    return tw > 0.0 ? tw : 0.0;
}

// room.h RoomArcT: the curved front's arc.
float RoomArcTH(float3 o, float3 d)
{
    float t;
    float3 h;
    if (!RoomArcRoot(roomR, Rg, tanA, 3.0e38, o, d, t, h)) return 0.0;
    return t;
}

// room.h RoomFillHit: the chart coordinates of point p on surface `part`, for every path.
void FillHitH(int part, float3 p, out RoomHitH h)
{
    int face = part >= ROOM_PART_WING_L ? ROOM_FACE_FRONT : part;
    h.face = face; h.part = part; h.u = 0.0; h.v = 0.0; h.s = 0.0; h.y = p.y;
    if (face == ROOM_FACE_FRONT)
    {
        if ((flags & ROOM_FLAG_CURVED) == 0) h.s = p.x;
        else if (part == ROOM_FACE_FRONT) h.s = roomR * atan2(p.x, roomR - p.z);
        else h.s = (part == ROOM_PART_WING_L ? -1.0 : 1.0) * (roomR * phiA + (abs(p.x) - xa) * wingS);
        h.u = (h.s + sMax) * inv2sMax; h.v = (yC - p.y) * invH;
    }
    else if (face == ROOM_FACE_LEFT || face == ROOM_FACE_RIGHT) { h.u = (p.z - zSide) * invSide; h.v = (yC - p.y) * invH; }
    else if (face == ROOM_FACE_FLOOR || face == ROOM_FACE_CEILING) { h.u = (p.x + X) * inv2X; h.v = (p.z + gap) * invFloorZ; }
    else { h.u = (p.x + X) * inv2X; h.v = (yC - p.y) * invH; }
}

// room.h RoomExitFast: the exit, planes first; the curved front only when the box's exit
// lies in front of it. No inside check: the caller has made it, once per thread.
bool RoomExitFastH(float3 o, float3 d, float3 invD, out RoomHitH h)
{
    float best = 3.0e38, t;
    int part = -1;
    if (d.x > 0.0) { t = (X - o.x) * invD.x; if (t > 0.0 && t < best) { best = t; part = ROOM_FACE_RIGHT; } }
    if (d.x < 0.0) { t = (-X - o.x) * invD.x; if (t > 0.0 && t < best) { best = t; part = ROOM_FACE_LEFT; } }
    if (d.y < 0.0) { t = (yF - o.y) * invD.y; if (t > 0.0 && t < best) { best = t; part = ROOM_FACE_FLOOR; } }
    if (d.y > 0.0) { t = (yC - o.y) * invD.y; if (t > 0.0 && t < best) { best = t; part = ROOM_FACE_CEILING; } }
    if (d.z > 0.0) { t = (zB - o.z) * invD.z; if (t > 0.0 && t < best) { best = t; part = ROOM_FACE_BACK; } }
    if ((flags & ROOM_FLAG_CURVED) == 0)
    {
        if (d.z < 0.0) { t = (-gap - o.z) * invD.z; if (t > 0.0 && t < best) { best = t; part = ROOM_FACE_FRONT; } }
    }
    else
    {
        bool front = part < 0;
        if (!front)
        {
            float3 pb = RoomAt(o, d, best);
            front = pb.z < FrontDepthR(pb.x);
        }
        if (front)
        {
            t = RoomArcTH(o, d); if (t > 0.0 && t < best) { best = t; part = ROOM_FACE_FRONT; }
            t = RoomWingT(-1.0, o, d); if (t > 0.0 && t < best) { best = t; part = ROOM_PART_WING_L; }
            t = RoomWingT(1.0, o, d); if (t > 0.0 && t < best) { best = t; part = ROOM_PART_WING_R; }
        }
    }
    FillHitH(part, RoomAt(o, d, best), h);
    return part >= 0;
}

// room.h RoomExitOnFace: the exit if it is on surface `part` - that surface's t, and a
// check that the point lies in the room's closure there (the room is convex).
bool RoomExitOnFaceH(int part, float3 o, float3 d, float3 invD, out RoomHitH h)
{
    float t = -1.0;
    if (part == ROOM_FACE_RIGHT) { if (d.x > 0.0) t = (X - o.x) * invD.x; }
    else if (part == ROOM_FACE_LEFT) { if (d.x < 0.0) t = (-X - o.x) * invD.x; }
    else if (part == ROOM_FACE_FLOOR) { if (d.y < 0.0) t = (yF - o.y) * invD.y; }
    else if (part == ROOM_FACE_CEILING) { if (d.y > 0.0) t = (yC - o.y) * invD.y; }
    else if (part == ROOM_FACE_BACK) { if (d.z > 0.0) t = (zB - o.z) * invD.z; }
    else if (part == ROOM_FACE_FRONT)
    {
        if ((flags & ROOM_FLAG_CURVED) == 0) { if (d.z < 0.0) t = (-gap - o.z) * invD.z; }
        else t = RoomArcTH(o, d);
    }
    else if (part == ROOM_PART_WING_L) t = RoomWingT(-1.0, o, d);
    else if (part == ROOM_PART_WING_R) t = RoomWingT(1.0, o, d);
    float3 p = RoomAt(o, d, t);
    bool closure;
    if (part == ROOM_FACE_LEFT || part == ROOM_FACE_RIGHT) closure = p.y >= yF && p.y <= yC && p.z >= zSide && p.z <= zB;
    else if (part == ROOM_FACE_FLOOR || part == ROOM_FACE_CEILING) closure = abs(p.x) <= X && p.z >= FrontDepthR(p.x) && p.z <= zB;
    else closure = abs(p.x) <= X && p.y >= yF && p.y <= yC;
    FillHitH(part, p, h);
    return t > 0.0 && t < 3.0e38 && closure;
}

// room.h RoomClassifyRay. Sample kinds: 0 picture, 1 + face, ROOM_KIND_FOOTPRINT (7) the
// flat screen's footprint, ROOM_KIND_OUTSIDE (8) outside. `inside`: the eye is in the room
// (once per thread); `part`: the surface of the pixel's last full exit, -1 before one.
int ClassifyR(float3 o, float3 d, bool inside, inout int part, out float2 uv, out float2 guv)
{
    uv = float2(0, 0); guv = float2(0, 0);
    float3 invD = 1.0 / d;
    if ((flags & ROOM_FLAG_FLAT_LAYER) == 0)
    {
        if (RoomScreenH(o, d, invD, uv)) return 0;
    }
    else if (d.z < 0.0)
    {
        float t = -o.z * invD.z;
        float hx = o.x + t * d.x, hy = o.y + t * d.y;
        if (t > 0.0 && abs(hx) <= 0.5 * screenW && abs(hy) <= 0.5 * screenH) return ROOM_KIND_FOOTPRINT;
    }
    if (!inside) return ROOM_KIND_OUTSIDE;
    RoomHitH h = (RoomHitH)0;
    bool found = false;
    if (part >= 0) found = RoomExitOnFaceH(part, o, d, invD, h);
    if (!found)
    {
        found = RoomExitFastH(o, d, invD, h);
        if (found) part = h.part;
    }
    if (!found) return ROOM_KIND_OUTSIDE;
    uv = float2(h.u, h.v);
    if (h.face == ROOM_FACE_FRONT) guv = float2(h.s * invGlowW + 0.5, 0.5 - h.y * invGlowH);
    return 1 + h.face;
}

#if ROOM_LOOK
// room.h RoomRayDiff: how the ray's direction moves per pixel across (Dx) and down (Dy).
void RayDiffH(uint e, out float3 Dx, out float3 Dy)
{
    float4 tans = eyeData[e * 5 + 4];
    float4 r0 = eyeData[e * 5 + 1], r1 = eyeData[e * 5 + 2], r2 = eyeData[e * 5 + 3];
    float sx = (tans.y - tans.x) / (float)EW, sy = (tans.w - tans.z) / (float)EH;
    Dx = float3(r0.x * sx, r1.x * sx, r2.x * sx);
    Dy = float3(r0.y * sy, r1.y * sy, r2.y * sy);
}

// room.h RoomPlaneFootprint: how far the hit at t on a plane with normal n moves per pixel.
void FootprintH(float t, float3 d, float3 n, float3 Dx, float3 Dy, out float3 Px, out float3 Py)
{
    float nd = n.x * d.x + n.y * d.y + n.z * d.z;
    float kx = (n.x * Dx.x + n.y * Dx.y + n.z * Dx.z) / nd, ky = (n.x * Dy.x + n.y * Dy.y + n.z * Dy.z) / nd;
    Px = float3(t * (Dx.x - d.x * kx), t * (Dx.y - d.y * kx), t * (Dx.z - d.z * kx));
    Py = float3(t * (Dy.x - d.x * ky), t * (Dy.y - d.y * ky), t * (Dy.z - d.z * ky));
}

// room.h RoomBar: a bar of width w centred on c, box-filtered over a footprint f round s.
float BarH(float s, float c, float w, float f)
{
    return saturate((min(s + 0.5 * f, c + 0.5 * w) - max(s - 0.5 * f, c - 0.5 * w)) / f);
}

// room.h RoomCeilingPanel: how much of the pixel's footprint where its ray meets the
// ceiling (y = yC, normal down) the room light's panel covers.
float CeilingPanelH(float3 o, float3 d, float3 Dx, float3 Dy)
{
    if (!(d.y > 0.0)) return 0.0;
    float t = (yC - o.y) / d.y;
    float3 p = RoomAt(o, d, t);
    float3 Px, Py;
    FootprintH(t, d, float3(0.0, -1.0, 0.0), Dx, Dy, Px, Py);
    float fx = clamp(abs(Px.x) + abs(Py.x), ROOM_FOOT_MIN, ROOM_FOOT_MAX), fz = clamp(abs(Px.z) + abs(Py.z), ROOM_FOOT_MIN, ROOM_FOOT_MAX);
    return BarH(p.x, 0.5 * (lightX0 + lightX1), lightX1 - lightX0, fx) * BarH(p.z, 0.5 * (lightZ0 + lightZ1), lightZ1 - lightZ0, fz);
}

// room.h RoomFootprintAxis.
float FootAxisH(float a, float b)
{
    return clamp(abs(a) + abs(b), ROOM_FOOT_MIN, ROOM_FOOT_MAX);
}

// room.h RoomPulse: bars of width w centred on the multiples of P, box-filtered over f round s.
float PulseH(float s, float P, float w, float f)
{
    float u0 = s + 0.5 * w - 0.5 * f, u1 = s + 0.5 * w + 0.5 * f;
    float k0 = floor(u0 / P), k1 = floor(u1 / P);
    float r0 = u0 - k0 * P, r1 = u1 - k1 * P;
    if (k0 == k1 && r1 <= w) return 1.0;
    return saturate(((k1 - k0) * w + min(r1, w) - min(r0, w)) / f);
}

// room.h RoomFrameCoverage: the frames' cover of a footprint fa x fb round p on a wall or the ceiling.
float FrameH(int face, float3 p, float fa, float fb)
{
    float cv, ch;
    if (face == ROOM_FACE_LEFT || face == ROOM_FACE_RIGHT) cv = PulseH(p.z - zSide, pitchSide, ROOM_FRAME_W, fa);
    else cv = PulseH(p.x + X, pitchBack, ROOM_FRAME_W, fa);
    if (face == ROOM_FACE_CEILING) ch = PulseH(p.z - zSide, pitchSide, ROOM_FRAME_W, fb);
    else ch = min(1.0, BarH(p.y, yF, ROOM_FRAME_W, fb) + BarH(p.y, transomY, ROOM_FRAME_W, fb) + BarH(p.y, yC, ROOM_FRAME_W, fb));
    return 1.0 - (1.0 - cv) * (1.0 - ch);
}

// room.h RoomHash: a 1 m floor tile's tone, 0..65535.
uint HashH(int i, int j)
{
    uint k = ((uint)(i + 1024) * 73856093u) ^ ((uint)(j + 1024) * 19349663u);
    k ^= k >> 13;
    k *= 0x5bd1e995u;
    k ^= k >> 15;
    return k & 0xFFFFu;
}

// room.h RoomTileFactor: the tiles' factor tau at floor point p, footprint fx x fz, and
// the grout lines' cover.
float TileH(float3 p, float fx, float fz, out float grout)
{
    grout = 1.0 - (1.0 - PulseH(p.x, 1.0, ROOM_GROUT_W, fx)) * (1.0 - PulseH(p.z, 1.0, ROOM_GROUT_W, fz));
    float h = (float)HashH((int)floor(p.x), (int)floor(p.z));
    float tone = 1.0 + ROOM_TILE_VAR * (h / 65535.0 - 0.5) * saturate(1.0 - 2.0 * max(fx, fz));
    return (1.0 - ROOM_GROUT * grout) * tone;
}

// room.h RoomEnvironment: the sky, the horizon or the ground (with its 1 m grid, filtered
// over the footprint from Dx and Dy, or its mean for a reflected ray, and fog) that a ray
// sees beyond the glass, tinted by the world colour.
float3 EnvH(float3 o, float3 d, float3 Dx, float3 Dy, bool filtered)
{
    float3 w = roomWorld.rgb;
    float3 hc = ROOM_ENV_HORIZON * w;
    float len = sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    float dy = d.y / len;
    if (dy >= 0.0) return hc + (ROOM_ENV_ZENITH * w - hc) * sqrt(dy);
    if (dy > -ROOM_ENV_HORIZON_DY) return hc;
    float tg = (yF - o.y) / d.y;
    float3 q = RoomAt(o, d, tg);
    float grid = ROOM_ENV_LINE_MEAN;
    if (filtered)
    {
        float3 Px, Py;
        FootprintH(tg, d, float3(0.0, 1.0, 0.0), Dx, Dy, Px, Py);
        grid = 1.0 - (1.0 - PulseH(q.x, 1.0, ROOM_ENV_LINE_W, FootAxisH(Px.x, Py.x))) * (1.0 - PulseH(q.z, 1.0, ROOM_ENV_LINE_W, FootAxisH(Px.z, Py.z)));
    }
    float fog = 1.0 - exp(-tg * len / ROOM_ENV_FOG);
    float3 G = ROOM_ENV_GROUND * w * (1.0 + ROOM_ENV_LINE * grid);
    return G + (hc - G) * fog;
}

// room.h RoomFresnel: the coat's reflectance at c = |cos| of the ray's angle to the face.
float FresnelH(float c)
{
    float q = 1.0 - c, q2 = q * q, q5 = q2 * q2 * q;
    return roomReflect * (ROOM_GLASS_F0 + (1.0 - ROOM_GLASS_F0) * q5);
}

// room.h RoomMirror::Sample: the mirror picture, bilinear by hand over Load, edges clamped
// to the mirrorW x mirrorH in use (the texture's other rows are never read).
float3 MirrorH(float2 uv)
{
    float x = uv.x * (float)mirrorW - 0.5, y = uv.y * (float)mirrorH - 0.5;
    float fx0 = floor(x), fy0 = floor(y), fx = x - fx0, fy = y - fy0;
    int w1 = (int)mirrorW - 1, h1 = (int)mirrorH - 1;
    int x0 = clamp((int)fx0, 0, w1), x1 = clamp((int)fx0 + 1, 0, w1), y0 = clamp((int)fy0, 0, h1), y1 = clamp((int)fy0 + 1, 0, h1);
    float3 a = mirrorPic.Load(int3(x0, y0, 0)).rgb, b = mirrorPic.Load(int3(x1, y0, 0)).rgb;
    float3 c = mirrorPic.Load(int3(x0, y1, 0)).rgb, e = mirrorPic.Load(int3(x1, y1, 0)).rgb;
    float3 top = a + fx * (b - a), bot = c + fx * (e - c);
    return top + fy * (bot - top);
}

// room.h RoomScreenWideHit: the screen as a reflected ray (o, d, invD = 1/d) sees it, within
// `widen` of its outline - the curved screen (the slab test against its box grown by widen,
// then atan2 for the angle: a secondary path) or the flat layer's rectangle. Its uv, the
// ray's t there and how far inside the outline (es < 0 outside).
bool ScreenWideH(float3 o, float3 d, float3 invD, float widen, out float2 uv, out float ts, out float es)
{
    uv = float2(0, 0); ts = 0.0; es = 0.0;
    if ((flags & ROOM_FLAG_FLAT_LAYER) != 0)
    {
        if (!(d.z < 0.0) || !(screenW > 0.0) || !(screenH > 0.0)) return false;
        float tf = -o.z / d.z;
        float x = o.x + tf * d.x, y = o.y + tf * d.y;
        float hw = 0.5 * screenW, hh = 0.5 * screenH;
        if (!(tf > 0.0) || abs(x) > hw + widen || abs(y) > hh + widen) return false;
        ts = tf; uv = float2(x / screenW + 0.5, 0.5 - y / screenH);
        es = min(hw - abs(x), hh - abs(y));
        return true;
    }
    if (!(R > 0.0) || !(halfW > 0.0) || !(halfH > 0.0)) return false;
    float3 s1 = (float3(-scrBoxX - widen, -scrBoxY - widen, -ROOM_SCREEN_PAD - widen) - o) * invD;
    float3 s2 = (float3(scrBoxX + widen, scrBoxY + widen, scrBoxZ + widen) - o) * invD;
    float3 sn = min(s1, s2), sf = max(s1, s2);
    float enter = max(max(sn.x, sn.y), sn.z), leave = min(min(sf.x, sf.y), sf.z);
    if (!(leave >= max(enter, 0.0))) return false;
    float a = d.x * d.x + d.z * d.z;
    if (!(a > 1e-12)) return false;
    float b = 2.0 * (o.x * d.x + (o.z - R) * d.z);
    float cc = o.x * o.x + o.z * o.z - 2.0 * o.z * R;
    float disc = b * b - 4.0 * a * cc;
    if (disc < 0.0) return false;
    float sq = sqrt(disc);
    float q = -0.5 * (b + (b < 0.0 ? -sq : sq));
    float t0 = q / a;
    float t1 = (q != 0.0) ? cc / q : t0;
    if (t0 > t1) { float sw = t0; t0 = t1; t1 = sw; }
    [unroll] for (int i = 0; i < 2; i++)
    {
        float tr = (i == 0) ? t0 : t1;
        if (!(tr > 0.0)) continue;
        float x = o.x + tr * d.x, y = o.y + tr * d.y, z = o.z + tr * d.z;
        float toward = R - z;
        if (!(toward > 0.0)) continue;
        float phi = atan2(x, toward);
        if (abs(phi) > halfWrap + widen / R || abs(y) > halfH + widen) continue;
        ts = tr;
        uv = float2((R * phi) / (2.0 * halfW) + 0.5, 0.5 - y / (2.0 * halfH));
        es = min(R * (halfWrap - abs(phi)), halfH - abs(y));
        return true;
    }
    return false;
}

// room.h RoomReflection: what the coat of a sample on `face` mirrors. The sample's ray
// (direction d, differentials Dx, Dy) met the face at p, t; the reflected ray starts
// ROOM_NUDGE along itself from p, and each eye sees its own mirror image. The screen is the mirror
// picture with a soft edge over the footprint (t + t_s)(|Dx| + |Dy|); round or behind it
// the room's face where the ray leaves, as the finish's secondary surfaces (no frames,
// tiles or further reflections); `own` if there is no exit.
float3 ReflectH(int face, float3 p, float3 d, float t, float3 Dx, float3 Dy, float3 own)
{
    float3 d2 = d;
    if (face == ROOM_FACE_LEFT || face == ROOM_FACE_RIGHT) d2.x = -d2.x;
    else if (face == ROOM_FACE_FLOOR || face == ROOM_FACE_CEILING) d2.y = -d2.y;
    else d2.z = -d2.z;
    float3 o2 = RoomAt(p, d2, ROOM_NUDGE / sqrt(d2.x * d2.x + d2.y * d2.y + d2.z * d2.z));
    float spread = sqrt(Dx.x * Dx.x + Dx.y * Dx.y + Dx.z * Dx.z) + sqrt(Dy.x * Dy.x + Dy.y * Dy.y + Dy.z * Dy.z);
    float3 invD = 1.0 / d2;
    float3 S = float3(0, 0, 0);
    float ks = 0.0, ts, es;
    float2 suv;
    if (ScreenWideH(o2, d2, invD, ROOM_REFL_FOOT_MAX, suv, ts, es))
    {
        float fw = min(max(spread * (t + ts), ROOM_FOOT_MIN), ROOM_REFL_FOOT_MAX);
        ks = min(max(es / fw + 0.5, 0.0), 1.0);
        if (ks > 0.0) S = MirrorH(suv);
    }
    if (ks >= 1.0) return S;
    float3 B = own;
    RoomHitH h = (RoomHitH)0;
    if (RoomExitFastH(o2, d2, invD, h))
    {
        float3 Ld2 = lightmap.SampleLevel(samp, float3(h.u, h.v, (float)h.face), 0).rgb;
        if (h.face == ROOM_FACE_FRONT)
        {
            B = Ld2;
            float2 g2 = float2(h.s * invGlowW + 0.5, 0.5 - h.y * invGlowH);
            if ((flags & ROOM_FLAG_GLOW) != 0 && g2.x >= 0.0 && g2.x <= 1.0 && g2.y >= 0.0 && g2.y <= 1.0)
                B += Dec(glow.SampleLevel(samp, g2, 0).rgb);
        }
        else if (h.face == ROOM_FACE_FLOOR) B = (1.0 - ROOM_FLOOR_GLOSS * roomFbar) * Ld2;
        else
        {
            float3 env = float3(0, 0, 0);
            if (roomGlass > 0.0) env = EnvH(o2, d2, Dx, Dy, false);
            B = (1.0 - roomGlass) * (1.0 - roomFbar) * Ld2 + roomGlass * env;
            if (h.face == ROOM_FACE_CEILING)
            {
                float t2 = (yC - o2.y) * invD.y;
                float3 p2 = RoomAt(o2, d2, t2);
                float f2 = min(max(spread * (t + t2), ROOM_FOOT_MIN), ROOM_FOOT_MAX);
                float k2 = BarH(p2.x, 0.5 * (lightX0 + lightX1), lightX1 - lightX0, f2) * BarH(p2.z, 0.5 * (lightZ0 + lightZ1), lightZ1 - lightZ0, f2);
                B = k2 * (Ld2 + lightSeen.rgb) + (1.0 - k2) * B;
            }
        }
    }
    return ks * S + (1.0 - ks) * B;
}

// room.h RoomSurface: a room surface's radiance L (the lightmap's Ld on entry) at a sample
// on `face` with ray (o, d): the room light's panel on the ceiling, and with the finish on
// the floor's tiles, the frames, the glass with what lies beyond it, the panel as a fitting
// and, with the reflections on, what the coat mirrors (one ReflectH per sample).
float3 SurfaceH(int face, float3 L, float3 o, float3 d, float3 Dx, float3 Dy)
{
    if (face == ROOM_FACE_FRONT) return L;
    if ((flags & ROOM_FLAG_FINISH) == 0)
    {
        if (face == ROOM_FACE_CEILING && (flags & ROOM_FLAG_LIGHT) != 0) L += CeilingPanelH(o, d, Dx, Dy) * lightSeen.rgb;
        return L;
    }
    int axis = (face == ROOM_FACE_LEFT || face == ROOM_FACE_RIGHT) ? 0 : ((face == ROOM_FACE_FLOOR || face == ROOM_FACE_CEILING) ? 1 : 2);
    float plane = face == ROOM_FACE_LEFT ? -X : (face == ROOM_FACE_RIGHT ? X : (face == ROOM_FACE_FLOOR ? yF : (face == ROOM_FACE_CEILING ? yC : zB)));
    float sgn = (face == ROOM_FACE_LEFT || face == ROOM_FACE_FLOOR) ? 1.0 : -1.0;
    float3 n = axis == 0 ? float3(sgn, 0, 0) : (axis == 1 ? float3(0, sgn, 0) : float3(0, 0, sgn));
    float oa = axis == 0 ? o.x : (axis == 1 ? o.y : o.z), da = axis == 0 ? d.x : (axis == 1 ? d.y : d.z);
    float t = (plane - oa) / da;
    if (!(t > 0.0) || !(t < 3.0e38)) return L;
    float3 p = RoomAt(o, d, t);
    float3 Px, Py;
    FootprintH(t, d, n, Dx, Dy, Px, Py);
    bool refl = (flags & ROOM_FLAG_REFLECT) != 0;
    float cosn = abs(da) / sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    // base: the sample without its reflection (the floor's tiles, or a pane); own: its coated
    // light for a reflected ray that finds no exit; fr: the coat's reflectance (Ff or F),
    // 0 where no reflection is traced - over a frame, over the panel, in the floor's grout.
    float3 base, own;
    float fr = 0.0, phi = 0.0, kappa = 0.0;
    if (face == ROOM_FACE_FLOOR)
    {
        float grout;
        float tau = TileH(p, FootAxisH(Px.x, Py.x), FootAxisH(Px.z, Py.z), grout);
        float coat = 1.0 - ROOM_FLOOR_GLOSS * roomFbar * (1.0 - grout);
        base = coat * tau * L;
        own = coat * L;
        if (refl) fr = ROOM_FLOOR_GLOSS * FresnelH(cosn) * (1.0 - grout);
    }
    else
    {
        if (face == ROOM_FACE_LEFT || face == ROOM_FACE_RIGHT) phi = FrameH(face, p, FootAxisH(Px.z, Py.z), FootAxisH(Px.y, Py.y));
        else if (face == ROOM_FACE_CEILING) phi = FrameH(face, p, FootAxisH(Px.x, Py.x), FootAxisH(Px.z, Py.z));
        else phi = FrameH(face, p, FootAxisH(Px.x, Py.x), FootAxisH(Px.y, Py.y));
        float3 env = float3(0, 0, 0);
        if (roomGlass > 0.0) env = EnvH(o, d, Dx, Dy, true);
        base = (1.0 - roomGlass) * (1.0 - roomFbar) * L + roomGlass * env;
        own = (1.0 - roomFbar) * L;
        if (face == ROOM_FACE_CEILING)
            kappa = BarH(p.x, 0.5 * (lightX0 + lightX1), lightX1 - lightX0, FootAxisH(Px.x, Py.x)) *
                    BarH(p.z, 0.5 * (lightZ0 + lightZ1), lightZ1 - lightZ0, FootAxisH(Px.z, Py.z));
        if (refl && (1.0 - phi) * (1.0 - kappa) > 0.0) fr = FresnelH(cosn);
    }
    if (fr > 0.0) base = fr * ReflectH(face, p, d, t, Dx, Dy, own) + (1.0 - fr) * base;
    if (face == ROOM_FACE_FLOOR) return base;
    float3 lg = phi * L + (1.0 - phi) * base;
    if (face != ROOM_FACE_CEILING) return lg;
    return kappa * (L + lightSeen.rgb) + (1.0 - kappa) * lg;
}
#endif

// room.h RoomSampleColour, without the v11 controls (LookColour adds them).
float3 RoomColour(int kind, float2 uv, float2 guv, uint e)
{
    float3 c = world.rgb;
    if (kind == 0) c = picture.SampleLevel(samp, float3(uv, (float)e), 0).rgb;
    else if (kind == ROOM_KIND_FOOTPRINT) c = float3(0, 0, 0);
    else if (kind >= 1 && kind <= ROOM_FACES)
    {
        float3 L = lightmap.SampleLevel(samp, float3(uv, (float)(kind - 1)), 0).rgb;
        if (kind == 1 + ROOM_FACE_FRONT && (flags & ROOM_FLAG_GLOW) != 0 && guv.x >= 0.0 && guv.x <= 1.0 && guv.y >= 0.0 && guv.y <= 1.0)
            L += Dec(glow.SampleLevel(samp, guv, 0).rgb);
        c = Enc(max(L, 0.0));
    }
    return c;
}

#if ROOM_LOOK
// room.h RoomSampleColour with the v11 controls: a room surface other than the front goes
// through SurfaceH with the sample's ray (o, d); everything else is RoomColour's.
float3 LookColour(int kind, float2 uv, float2 guv, uint e, float3 o, float3 d, float3 Dx, float3 Dy)
{
    if (kind <= 1 + ROOM_FACE_FRONT || kind > ROOM_FACES) return RoomColour(kind, uv, guv, e);
    float3 L = lightmap.SampleLevel(samp, float3(uv, (float)(kind - 1)), 0).rgb;
    return Enc(max(SurfaceH(kind - 1, L, o, d, Dx, Dy), 0.0));
}
#endif

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= EW || id.y >= EH) return;
    uint e = id.z;

    // Every ray starts at the eye: whether it is in the room, once.
    bool inside = InsideH(eyeData[e * 5 + 0].xyz);
    int part = -1;
    int kind[4];
    float2 uv[4], guv[4];
    [unroll] for (int s = 0; s < 4; s++)
    {
        float3 o, d;
        Ray(e, (float)id.x + 0.5 + SUB[s].x, (float)id.y + 0.5 + SUB[s].y, o, d);
        kind[s] = ClassifyR(o, d, inside, part, uv[s], guv[s]);
    }
    bool agree = kind[0] == kind[1] && kind[1] == kind[2] && kind[2] == kind[3];
    // The v11 controls (ROOM_LOOK only): a room sample goes through SurfaceH - when the four
    // rays agree, with the pixel's centre ray (the exact mean of their directions),
    // otherwise each with its own. (One loop over the samples instead measured slower
    // with the light on, and with the reflections on it added 0.28-0.32 ms to the curved
    // eye pass, though it made the shader half the size.)
    float3 col = float3(0, 0, 0);
#if ROOM_LOOK
    float3 Dx, Dy;
    RayDiffH(e, Dx, Dy);
#endif
    if (agree)
    {
        float2 m = float2((uv[0].x + uv[1].x + uv[2].x + uv[3].x) * 0.25, (uv[0].y + uv[1].y + uv[2].y + uv[3].y) * 0.25);
        float2 mg = float2((guv[0].x + guv[1].x + guv[2].x + guv[3].x) * 0.25, (guv[0].y + guv[1].y + guv[2].y + guv[3].y) * 0.25);
#if ROOM_LOOK
        float3 o, d;
        Ray(e, (float)id.x + 0.5, (float)id.y + 0.5, o, d);
        col = LookColour(kind[0], m, mg, e, o, d, Dx, Dy);
#else
        col = RoomColour(kind[0], m, mg, e);
#endif
        if ((flags & ROOM_FLAG_DITHER) != 0 && kind[0] >= 1 && kind[0] <= ROOM_FACES)
        {
            uint off = e != 0 ? 4 : 0;
            col += (((float)BAYER[((id.x + off) & 7) + ((id.y + off) & 7) * 8] + 0.5) / 64.0 - 0.5) / 255.0;
        }
    }
    else
    {
        [unroll] for (int k = 0; k < 4; k++)
        {
#if ROOM_LOOK
            float3 o, d;
            Ray(e, (float)id.x + 0.5 + SUB[k].x, (float)id.y + 0.5 + SUB[k].y, o, d);
            col += LookColour(kind[k], uv[k], guv[k], e, o, d, Dx, Dy) * 0.25;
#else
            col += RoomColour(kind[k], uv[k], guv[k], e) * 0.25;
#endif
        }
    }
    outEye[uint3(id.x, id.y, e)] = float4(col, 1.0);
}
)HLSL";

// The v10 eye pass, verbatim: compiled into curveRoomV10Pso for the --room-v10-eye A/B
// diagnostic and the room benchmark's case D (room.h: room_v10). It follows the shared
// head (kRoomCbufferHlsl declares RoomConstants' rows 0-20 in front of it) but reads only
// rows 0-10 and never t3, so it runs with the room's current root signature and
// constants. It ignores the v11 controls: with the room light on, its lightmap has the
// light (EMIT and LIGHT are shared) but the ceiling shows no panel. Remove it once the
// headset numbers are in.
static const char* kCurveRoomV10Hlsl = R"HLSL(
Texture2DArray<float4> lightmap : register(t2);

static const int BAYER[64] = {
     0, 32,  8, 40,  2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37, 63, 31, 55, 23, 61, 29, 53, 21 };

struct RoomHitH { int face; float u; float v; float s; float y; };

bool RoomExitH(float3 o, float3 d, out RoomHitH h)
{
    h.face = -1; h.u = 0.0; h.v = 0.0; h.s = 0.0; h.y = 0.0;
    if (!InsideH(o)) return false;
    float best = 3.0e38, t;
    int face = -1;
    float frontPhi = 0.0;
    bool onArc = false;
    if (d.x > 0.0) { t = (X - o.x) / d.x; if (t > 0.0 && t < best) { best = t; face = 2; } }
    if (d.x < 0.0) { t = (-X - o.x) / d.x; if (t > 0.0 && t < best) { best = t; face = 1; } }
    if (d.y < 0.0) { t = (yF - o.y) / d.y; if (t > 0.0 && t < best) { best = t; face = 3; } }
    if (d.y > 0.0) { t = (yC - o.y) / d.y; if (t > 0.0 && t < best) { best = t; face = 4; } }
    if (d.z > 0.0) { t = (zB - o.z) / d.z; if (t > 0.0 && t < best) { best = t; face = 5; } }
    if ((flags & 1) == 0)
    {
        if (d.z < 0.0) { t = (-gap - o.z) / d.z; if (t > 0.0 && t < best) { best = t; face = 0; } }
    }
    else
    {
        float phi, yy;
        if (ArcHit(roomR, Rg, phiA, 3.0e38, o, d, phi, yy))
        {
            float hx = Rg * sin(phi), hz = roomR - Rg * cos(phi);
            t = abs(d.x) > abs(d.z) ? (hx - o.x) / d.x : (hz - o.z) / d.z;
            if (t > 0.0 && t < best) { best = t; face = 0; frontPhi = phi; onArc = true; }
        }
        [unroll] for (int side = -1; side <= 1; side += 2)
        {
            float wnx = -(float)side * sinA, wnz = cosA;
            float wpx = (float)side * xa, wpz = za;
            float nd = wnx * d.x + wnz * d.z;
            if (!(nd < 0.0)) continue;
            float tw = (wnx * (wpx - o.x) + wnz * (wpz - o.z)) / nd;
            float hx2 = o.x + tw * d.x;
            if ((float)side * hx2 < xa) continue;
            if (tw > 0.0 && tw < best) { best = tw; face = 0; onArc = false; }
        }
    }
    if (face < 0) return false;
    float3 p = float3(o.x + best * d.x, o.y + best * d.y, o.z + best * d.z);
    float hgt = yC - yF;
    h.face = face; h.y = p.y;
    if (face == 0)
    {
        h.s = (flags & 1) == 0 ? p.x : (onArc ? roomR * frontPhi : FrontSH(p.x));
        h.u = (h.s + sMax) / (2.0 * sMax); h.v = (yC - p.y) / hgt;
    }
    else if (face == 1 || face == 2) { h.u = (p.z - zSide) / (zB - zSide); h.v = (yC - p.y) / hgt; }
    else if (face == 3 || face == 4) { h.u = (p.x + X) / (2.0 * X); h.v = (p.z + gap) / (zB + gap); }
    else { h.u = (p.x + X) / (2.0 * X); h.v = (yC - p.y) / hgt; }
    return true;
}

// Sample kinds: 0 picture, 1 + face, 7 the flat screen's footprint, 8 outside.
int Classify(float3 o, float3 d, out float2 uv, out float2 guv)
{
    uv = float2(0, 0); guv = float2(0, 0);
    if ((flags & 2) == 0)
    {
        if (Screen(o, d, uv)) return 0;
    }
    else if (d.z < 0.0)
    {
        float t = -o.z / d.z;
        float hx = o.x + t * d.x, hy = o.y + t * d.y;
        if (t > 0.0 && abs(hx) <= 0.5 * screenW && abs(hy) <= 0.5 * screenH) return 7;
    }
    RoomHitH h;
    if (!RoomExitH(o, d, h)) return 8;
    uv = float2(h.u, h.v);
    if (h.face == 0) guv = float2(h.s / (2.0 * roomGlowHalfW) + 0.5, 0.5 - h.y / (2.0 * roomGlowHalfH));
    return 1 + h.face;
}

float3 RoomColour(int kind, float2 uv, float2 guv, uint e)
{
    float3 c = world.rgb;
    if (kind == 0) c = picture.SampleLevel(samp, float3(uv, (float)e), 0).rgb;
    else if (kind == 7) c = float3(0, 0, 0);
    else if (kind >= 1 && kind <= 6)
    {
        float3 L = lightmap.SampleLevel(samp, float3(uv, (float)(kind - 1)), 0).rgb;
        if (kind == 1 && (flags & 4) != 0 && guv.x >= 0.0 && guv.x <= 1.0 && guv.y >= 0.0 && guv.y <= 1.0)
            L += Dec(glow.SampleLevel(samp, guv, 0).rgb);
        c = Enc(max(L, 0.0));
    }
    return c;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= EW || id.y >= EH) return;
    uint e = id.z;

    int kind[4];
    float2 uv[4], guv[4];
    [unroll] for (int s = 0; s < 4; s++)
    {
        float3 o, d;
        Ray(e, (float)id.x + 0.5 + SUB[s].x, (float)id.y + 0.5 + SUB[s].y, o, d);
        kind[s] = Classify(o, d, uv[s], guv[s]);
    }
    bool agree = kind[0] == kind[1] && kind[1] == kind[2] && kind[2] == kind[3];
    float3 col = float3(0, 0, 0);
    if (agree)
    {
        float2 m = float2((uv[0].x + uv[1].x + uv[2].x + uv[3].x) * 0.25, (uv[0].y + uv[1].y + uv[2].y + uv[3].y) * 0.25);
        float2 mg = float2((guv[0].x + guv[1].x + guv[2].x + guv[3].x) * 0.25, (guv[0].y + guv[1].y + guv[2].y + guv[3].y) * 0.25);
        col = RoomColour(kind[0], m, mg, e);
        if ((flags & 8) != 0 && kind[0] >= 1 && kind[0] <= 6)
        {
            uint off = e != 0 ? 4 : 0;
            col += (((float)BAYER[((id.x + off) & 7) + ((id.y + off) & 7) * 8] + 0.5) / 64.0 - 0.5) / 255.0;
        }
    }
    else
    {
        [unroll] for (int k = 0; k < 4; k++) col += RoomColour(kind[k], uv[k], guv[k], e) * 0.25;
    }
    outEye[uint3(id.x, id.y, e)] = float4(col, 1.0);
}
)HLSL";

struct UnpackConstants                  // must match cbuffer C in kUnpackHlsl
{
    uint32_t sw, sh, pitch, dw, dh, normalize, pad0, pad1;
};

static bool CompileCs(const char* name, const char* src, ComPtr<ID3DBlob>& out);

// Copy queue, frame buffer and unpack pipeline for the frame-transfer path.
static bool InitFrameTransfer(App& app)
{
    Log("InitFrameTransfer: enter");
    if (!app.secondGpu || !app.device || !app.inferDevice) return false;

    int w = 0, h = 0;
    for (const ModelSpec* spec : { &kDepthAnythingV2, &kZipDepth }) { w = std::max(w, spec->inW); h = std::max(h, spec->inH); }
    app.xferW = w; app.xferH = h;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = (UINT64)w; td.Height = (UINT)h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    UINT64 total = 0;
    app.device->GetCopyableFootprints(&td, 0, 1, 0, &app.xferFootprint, nullptr, nullptr, &total);
    if (total > app.crossBytes) { Log("InitFrameTransfer: FAIL %llu-byte frame exceeds the hand-over buffer", (unsigned long long)total); return false; }
    app.xferBytes = total;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(app.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&app.xferQueue)))) { Log("InitFrameTransfer: FAIL copy queue"); return false; }
    app.xferQueue->SetName(L"vrx copy engine (frames to second GPU)");
    if (FAILED(app.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&app.xferAlloc)))) { Log("InitFrameTransfer: FAIL allocator"); return false; }
    if (FAILED(app.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, app.xferAlloc.Get(), nullptr, IID_PPV_ARGS(&app.xferList))))
    { Log("InitFrameTransfer: FAIL command list"); return false; }
    app.xferList->Close();
    if (FAILED(app.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&app.xferFence)))) { Log("InitFrameTransfer: FAIL fence"); return false; }
    app.xferFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!app.xferFenceEvent) { Log("InitFrameTransfer: FAIL fence event"); return false; }

    if (!MakeBuffer(app.inferDevice.Get(), D3D12_HEAP_TYPE_DEFAULT, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, app.frameLocal, nullptr))
    { Log("InitFrameTransfer: FAIL frame buffer"); return false; }

    ComPtr<ID3DBlob> cs;
    if (!CompileCs("unpack.hlsl", kUnpackHlsl, cs)) return false;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(UnpackConstants) / 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters = params;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    { Log("InitFrameTransfer: FAIL root signature: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    if (FAILED(app.inferDevice->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&app.unpackRootSig))))
    { Log("InitFrameTransfer: FAIL CreateRootSignature"); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = app.unpackRootSig.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(app.inferDevice->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.unpackPso)))) { Log("InitFrameTransfer: FAIL unpack pipeline"); return false; }

    app.frameTransfer = true;
    Log("InitFrameTransfer: exit ok - %dx%d BGRA frames (%llu KB, row pitch %u) via the copy engine", w, h,
        (unsigned long long)(total >> 10), app.xferFootprint.Footprint.RowPitch);
    return true;
}

static void ReleaseFrameTransfer(App& app)
{
    Log("ReleaseFrameTransfer: enter");
    app.frameTransfer = false;
    app.unpackPso.Reset(); app.unpackRootSig.Reset(); app.frameLocal.Reset();
    app.xferList.Reset(); app.xferAlloc.Reset(); app.xferQueue.Reset(); app.xferFence.Reset();
    if (app.xferFenceEvent) { CloseHandle(app.xferFenceEvent); app.xferFenceEvent = nullptr; }
    for (auto& t : app.smallTex) t.Reset();
    Log("ReleaseFrameTransfer: exit");
}

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

// Constants, the source picture (t0), the glow texture (u0), the border ring (u1) and
// the glow's 16-bit history (u2).
// The glow is both read and written, one thread per pixel, so the temporal blend
// needs no copy.
static bool MakeAmbiRootSig(App& app, ComPtr<ID3D12RootSignature>& out)
{
    D3D12_DESCRIPTOR_RANGE srcRange{};
    srcRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srcRange.NumDescriptors = 1; srcRange.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE glowRange{};
    glowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    glowRange.NumDescriptors = 3; glowRange.BaseShaderRegister = 0;         // u0 glow, u1 ring, u2 history

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(AmbiConstants) / 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srcRange;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &glowRange;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters = params;
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { Log("MakeAmbiRootSig: FAIL serialize: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    hr = app.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
    if (FAILED(hr)) { Log("MakeAmbiRootSig: FAIL CreateRootSignature 0x%08X", (unsigned)hr); return false; }
    return true;
}

// Constants, then one table: the warped pictures (t0), the glow (t1), the eye
// buffers (u0); a linear clamping sampler for both reads.
// With `room`, the table also holds the room's lightmap and mirror picture (t2 and t3,
// after the eye buffers) and RoomConstants come as a root CBV (b1): 56 + 1 + 2 = 59 of 64
// DWORDs (a table costs one DWORD however many ranges it holds).
static bool MakeCurveRootSig(App& app, ComPtr<ID3D12RootSignature>& out, bool room = false)
{
    static_assert(sizeof(CurveConstants) / 4 + 1 + 2 <= 64, "curve root signature: constants + table + CBV");
    D3D12_DESCRIPTOR_RANGE tbl[3]{};
    tbl[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tbl[0].NumDescriptors = 2; tbl[0].BaseShaderRegister = 0; tbl[0].OffsetInDescriptorsFromTableStart = 0;
    tbl[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    tbl[1].NumDescriptors = 1; tbl[1].BaseShaderRegister = 0; tbl[1].OffsetInDescriptorsFromTableStart = 2;
    tbl[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tbl[2].NumDescriptors = 2; tbl[2].BaseShaderRegister = 2; tbl[2].OffsetInDescriptorsFromTableStart = 3;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(CurveConstants) / 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = room ? 3 : 2;
    params[1].DescriptorTable.pDescriptorRanges = tbl;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 1;

    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = room ? 3 : 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &ss;
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { Log("MakeCurveRootSig: FAIL serialize: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    hr = app.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
    if (FAILED(hr)) { Log("MakeCurveRootSig: FAIL CreateRootSignature 0x%08X", (unsigned)hr); return false; }
    return true;
}

// The room's lightmap passes: RoomConstants as a root CBV (b0), the picture (t0, one
// per frame), then the decode table, glow, emitters, lightmap and mirror picture (u0-u2).
static bool MakeRoomRootSig(App& app, ComPtr<ID3D12RootSignature>& out)
{
    D3D12_DESCRIPTOR_RANGE srcRange{};
    srcRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srcRange.NumDescriptors = 1; srcRange.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE tbl[2]{};
    tbl[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tbl[0].NumDescriptors = 2; tbl[0].BaseShaderRegister = 1; tbl[0].OffsetInDescriptorsFromTableStart = 0;
    tbl[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    tbl[1].NumDescriptors = 3; tbl[1].BaseShaderRegister = 0; tbl[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srcRange;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 2;
    params[2].DescriptorTable.pDescriptorRanges = tbl;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters = params;
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { Log("MakeRoomRootSig: FAIL serialize: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    hr = app.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
    if (FAILED(hr)) { Log("MakeRoomRootSig: FAIL CreateRootSignature 0x%08X", (unsigned)hr); return false; }
    return true;
}

// SRV of a warp target's two warped pictures, for the curved pass.
static void MakePictureSrv(App& app, ID3D12Resource* colorOut, UINT index)
{
    if (!colorOut) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    v.Texture2DArray.MipLevels = 1;
    v.Texture2DArray.ArraySize = VIEWS;
    app.device->CreateShaderResourceView(colorOut, &v, CpuDesc(app, index));
}

// UAV of a pair of eye buffers.
static void MakeEyeUav(App& app, ID3D12Resource* eyes, UINT index)
{
    if (!eyes) return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    u.Texture2DArray.ArraySize = VIEWS;
    app.device->CreateUnorderedAccessView(eyes, nullptr, &u, CpuDesc(app, index));
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
    const UINT scratchElems = (UINT)cw * (UINT)ch * VIEWS * 3;   // source, nearness, sub-pixel position
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

    ComPtr<ID3DBlob> warpCs, prepCs, gridCs;
    if (!CompileCs("warp.hlsl", kWarpHlsl, warpCs)) return false;
    if (!CompileCs("prep.hlsl", kPrepHlsl, prepCs)) return false;
    if (!CompileCs("grid.hlsl", kGridHlsl, gridCs)) return false;
    if (!MakeRootSig(app, sizeof(WarpConstants) / 4, true, app.warpRootSig)) return false;
    if (!MakeRootSig(app, sizeof(PrepConstants) / 4, false, app.prepRootSig)) return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = app.warpRootSig.Get();
    pd.CS = { warpCs->GetBufferPointer(), warpCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.warpPso)))) { Log("InitShaders: FAIL warp PSO"); return false; }
    pd.pRootSignature = app.prepRootSig.Get();
    pd.CS = { prepCs->GetBufferPointer(), prepCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.prepPso)))) { Log("InitShaders: FAIL prep PSO"); return false; }
    pd.CS = { gridCs->GetBufferPointer(), gridCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.gridPso)))) { Log("InitShaders: FAIL grid PSO"); return false; }

    // Grid image for steadying/fusion: 1 MB each, made up front so options can be
    // switched on live.
    const UINT64 gridBytes = (UINT64)W * H * sizeof(uint32_t);
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_DEFAULT, gridBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.gridBuf, nullptr)) return false;
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_READBACK, gridBytes, D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_COPY_DEST, app.gridReadback, nullptr)) return false;
    if (FAILED(app.gridReadback->Map(0, nullptr, (void**)&app.gridMapped)) || !app.gridMapped) { Log("InitShaders: FAIL grid readback map"); return false; }
    if (FAILED(app.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&app.gridAlloc))) ||
        FAILED(app.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, app.gridAlloc.Get(), nullptr, IID_PPV_ARGS(&app.gridList))))
    { Log("InitShaders: FAIL grid command list"); return false; }
    app.gridList->Close();

    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_DEFAULT, (UINT64)W * H * sizeof(float), D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, app.nearBuf, nullptr)) return false;
    if (!MakeWarpTarget(app, app.colorW, app.colorH, DESC_MAIN_TABLE, app.mainTarget)) return false;
    if (!MakeWarpTarget(app, W, H, DESC_TEST_TABLE, app.testTarget)) return false;
    if (!MakeSourceTexture(app, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false, app.testSrc)) return false;
    MakeTextureSrv(app, app.testSrc.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DESC_TEST_SRC);

    // Ambilight: built even when the option is off, so it can be switched live.
    static_assert(kAmbiTaps == 4 && kAmbiRingMax == 256, "kAmbiHlsl hardcodes TAPS = 4 and a 256-point shared ring");
    ComPtr<ID3DBlob> ambiCs, ambiRingCs;
    const std::string ringSource = std::string("#define AMBI_RING 1\n") + kAmbiHlsl;
    if (!CompileCs("ambilight.hlsl", kAmbiHlsl, ambiCs)) return false;
    if (!CompileCs("ambilight-ring.hlsl", ringSource.c_str(), ambiRingCs)) return false;
    if (!MakeAmbiRootSig(app, app.ambiRootSig)) return false;
    pd.pRootSignature = app.ambiRootSig.Get();
    pd.CS = { ambiCs->GetBufferPointer(), ambiCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.ambiPso)))) { Log("InitShaders: FAIL ambilight PSO"); return false; }
    pd.CS = { ambiRingCs->GetBufferPointer(), ambiRingCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.ambiRingPso)))) { Log("InitShaders: FAIL ambilight ring PSO"); return false; }
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_DEFAULT, (UINT64)2 * kAmbiRingMax * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.ambiRing, nullptr)) return false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_UNKNOWN;
    rv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    rv.Buffer.NumElements = 2 * kAmbiRingMax;
    rv.Buffer.StructureByteStride = 16;
    app.device->CreateUnorderedAccessView(app.ambiRing.Get(), nullptr, &rv, CpuDesc(app, DESC_AMBI_UAV + 1));
    AmbiSizeFor(app.colorW, app.colorH, &app.ambiW, &app.ambiH);
    if (app.ambiW <= 0 || app.ambiH <= 0) { Log("InitShaders: FAIL ambilight size (colour %dx%d)", app.colorW, app.colorH); return false; }
    if (!MakeTexture(app.device.Get(), app.ambiW, app.ambiH, DXGI_FORMAT_R8G8B8A8_TYPELESS, 1,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.ambiTex)) return false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC gv{};
    gv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    gv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    app.device->CreateUnorderedAccessView(app.ambiTex.Get(), nullptr, &gv, CpuDesc(app, DESC_AMBI_UAV));
    if (!MakeTexture(app.device.Get(), app.ambiW, app.ambiH, DXGI_FORMAT_R16G16B16A16_FLOAT, 1,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.ambiHist)) return false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC hv{};
    hv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    hv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    app.device->CreateUnorderedAccessView(app.ambiHist.Get(), nullptr, &hv, CpuDesc(app, DESC_AMBI_UAV + 2));
    app.ambiHistory = false;
    Log("InitShaders: ambilight glow %dx%d (margin %.0f%% of the screen width)", app.ambiW, app.ambiH, kAmbiMargin * 100.0f);

    // Curved screen: pipeline and every descriptor except the playback eye
    // buffers, which are made when a curve is first asked for.
    ComPtr<ID3DBlob> curveCs;
    if (!CompileCs("curve.hlsl", kCurveHlsl, curveCs)) return false;
    if (!MakeCurveRootSig(app, app.curveRootSig)) return false;
    pd.pRootSignature = app.curveRootSig.Get();
    pd.CS = { curveCs->GetBufferPointer(), curveCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.curvePso)))) { Log("InitShaders: FAIL curve PSO"); return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC glowSrv{};
    glowSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    glowSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    glowSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    glowSrv.Texture2D.MipLevels = 1;
    MakePictureSrv(app, app.mainTarget.colorOut.Get(), DESC_CURVE_MAIN + 0);
    app.device->CreateShaderResourceView(app.ambiTex.Get(), &glowSrv, CpuDesc(app, DESC_CURVE_MAIN + 1));
    MakePictureSrv(app, app.testTarget.colorOut.Get(), DESC_CURVE_TEST + 0);
    app.device->CreateShaderResourceView(app.ambiTex.Get(), &glowSrv, CpuDesc(app, DESC_CURVE_TEST + 1));
    if (!MakeTexture(app.device.Get(), TEST_EYE_W, TEST_EYE_H, DXGI_FORMAT_R8G8B8A8_TYPELESS, VIEWS,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.testEyeOut)) return false;
    MakeEyeUav(app, app.testEyeOut.Get(), DESC_CURVE_TEST + 2);
    Log("InitShaders: curved screen pass ready (%zu bytes)", curveCs->GetBufferSize());

    // The room. Its eye pass is the curve shader's code up to its main, then the
    // room's; the plain curve shader above is compiled from its own text alone. Every
    // room shader starts with the constants it shares with room.h, generated from the
    // C++ values (RoomHlslDefines).
    Log("InitShaders: room - building");
    static_assert(kRoomLightmap == 64, "kRoomHlsl hardcodes a 64 x 64 lightmap");
    const std::string curveText(kCurveHlsl);
    const size_t curveMain = curveText.find("[numthreads(8, 8, 1)]\nvoid main");
    if (curveMain == std::string::npos) { Log("InitShaders: FAIL room - the curve shader's main was not found"); return false; }
    const std::string roomDefines = RoomHlslDefines();
    size_t defineCount = 0;
    for (size_t at = roomDefines.find("#define "); at != std::string::npos; at = roomDefines.find("#define ", at + 1)) defineCount++;
    Log("InitShaders: room - %zu shared constants for the HLSL (%zu bytes of #defines)", defineCount, roomDefines.size());
    const std::string roomEyeHead = std::string("#define ROOM_REGISTER b1\n") + roomDefines + curveText.substr(0, curveMain) +
                                    kRoomCbufferHlsl + kRoomGeomHlsl;
    const std::string roomEyeText = std::string("#define ROOM_LOOK 0\n") + roomEyeHead + kCurveRoomHlsl;
    const std::string roomEyeLookText = std::string("#define ROOM_LOOK 1\n") + roomEyeHead + kCurveRoomHlsl;
    const std::string roomEyeV10Text = roomEyeHead + kCurveRoomV10Hlsl;
    const std::string roomLightText = std::string("#define ROOM_REGISTER b0\n") + roomDefines + kRoomCbufferHlsl + kRoomGeomHlsl + kRoomHlsl;
    const std::string roomEmitText = std::string("#define ROOM_EMIT 1\n") + roomLightText;
    const std::string roomMirrorText = std::string("#define ROOM_MIRROR 1\n") + roomLightText;
    ComPtr<ID3DBlob> roomEyeCs, roomEyeLookCs, roomEyeV10Cs, roomLightCs, roomEmitCs, roomMirrorCs;
    // The kept v10 eye pass and the eye pass with the v11 controls compile on their own
    // threads (D3DCompile is re-entrant), so that they do not add their seconds to every start.
    bool v10Compiled = false, lookCompiled = false;
    std::thread v10Compile([&]() { v10Compiled = CompileCs("curve-room-v10.hlsl", roomEyeV10Text.c_str(), roomEyeV10Cs); });
    std::thread lookCompile([&]() { lookCompiled = CompileCs("curve-room-look.hlsl", roomEyeLookText.c_str(), roomEyeLookCs); });
    const bool roomCompiled = CompileCs("curve-room.hlsl", roomEyeText.c_str(), roomEyeCs) &&
                              CompileCs("room-light.hlsl", roomLightText.c_str(), roomLightCs) &&
                              CompileCs("room-emit.hlsl", roomEmitText.c_str(), roomEmitCs) &&
                              CompileCs("room-mirror.hlsl", roomMirrorText.c_str(), roomMirrorCs);
    v10Compile.join();
    lookCompile.join();
    if (!roomCompiled || !v10Compiled || !lookCompiled)
    { Log("InitShaders: FAIL room shaders (v10 eye pass %s, look eye pass %s)", v10Compiled ? "ok" : "failed", lookCompiled ? "ok" : "failed"); return false; }
    if (!MakeCurveRootSig(app, app.curveRoomRootSig, true)) return false;
    if (!MakeRoomRootSig(app, app.roomRootSig)) return false;
    pd.pRootSignature = app.curveRoomRootSig.Get();
    pd.CS = { roomEyeCs->GetBufferPointer(), roomEyeCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.curveRoomPso)))) { Log("InitShaders: FAIL room eye PSO"); return false; }
    pd.CS = { roomEyeLookCs->GetBufferPointer(), roomEyeLookCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.curveRoomLookPso)))) { Log("InitShaders: FAIL room look eye PSO"); return false; }
    pd.CS = { roomEyeV10Cs->GetBufferPointer(), roomEyeV10Cs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.curveRoomV10Pso)))) { Log("InitShaders: FAIL room v10 eye PSO"); return false; }
    pd.pRootSignature = app.roomRootSig.Get();
    pd.CS = { roomLightCs->GetBufferPointer(), roomLightCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.roomLightPso)))) { Log("InitShaders: FAIL room light PSO"); return false; }
    pd.CS = { roomEmitCs->GetBufferPointer(), roomEmitCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.roomEmitPso)))) { Log("InitShaders: FAIL room emit PSO"); return false; }
    pd.CS = { roomMirrorCs->GetBufferPointer(), roomMirrorCs->GetBufferSize() };
    if (FAILED(app.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&app.roomMirrorPso)))) { Log("InitShaders: FAIL room mirror PSO"); return false; }

    const UINT64 emitterBytes = (UINT64)kRoomMaxEmitters * sizeof(RoomEmitter);
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256 * sizeof(float), D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_COPY_DEST, app.roomLut, nullptr)) return false;
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_DEFAULT, emitterBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.roomEmitters, nullptr)) return false;
    for (int i = 0; i <= RING; i++)
        if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_UPLOAD, emitterBytes, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_GENERIC_READ, app.roomGeomUp[i], (void**)&app.roomGeomMapped[i])) return false;
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_UPLOAD, (UINT64)(RING + 1) * sizeof(RoomConstants), D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_GENERIC_READ, app.roomCbUp, (void**)&app.roomCbMapped)) return false;
    if (!MakeTexture(app.device.Get(), kRoomLightmap, kRoomLightmap, DXGI_FORMAT_R16G16B16A16_FLOAT, kRoomFaces,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.roomLight)) return false;
    if (!MakeTexture(app.device.Get(), kRoomMirrorW, kRoomMirrorMaxH, DXGI_FORMAT_R16G16B16A16_FLOAT, 1,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.roomMirror)) return false;
    {
        // The decode table, once.
        ComPtr<ID3D12Resource> up;
        float* mapped = nullptr;
        if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256 * sizeof(float), D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_GENERIC_READ, up, (void**)&mapped) || !mapped) return false;
        RoomDecodeTable(mapped);
        WaitFence(app, app.fenceVal);
        app.cmdAlloc[0]->Reset();
        app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
        app.cmdList->CopyBufferRegion(app.roomLut.Get(), 0, up.Get(), 0, 256 * sizeof(float));
        Transition(app.cmdList.Get(), app.roomLut.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        app.cmdList->Close();
        WaitFence(app, SubmitAndSignal(app));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC lutSrv{};
    lutSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lutSrv.Format = DXGI_FORMAT_UNKNOWN;
    lutSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    lutSrv.Buffer.NumElements = 256;
    lutSrv.Buffer.StructureByteStride = sizeof(float);
    app.device->CreateShaderResourceView(app.roomLut.Get(), &lutSrv, CpuDesc(app, DESC_ROOM + 0));
    app.device->CreateShaderResourceView(app.ambiTex.Get(), &glowSrv, CpuDesc(app, DESC_ROOM + 1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC emUav{};
    emUav.Format = DXGI_FORMAT_UNKNOWN;
    emUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    emUav.Buffer.NumElements = kRoomMaxEmitters * kRoomEmitterFloat4s;
    emUav.Buffer.StructureByteStride = 16;
    app.device->CreateUnorderedAccessView(app.roomEmitters.Get(), nullptr, &emUav, CpuDesc(app, DESC_ROOM + 2));
    D3D12_UNORDERED_ACCESS_VIEW_DESC lmUav{};
    lmUav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lmUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    lmUav.Texture2DArray.ArraySize = kRoomFaces;
    app.device->CreateUnorderedAccessView(app.roomLight.Get(), nullptr, &lmUav, CpuDesc(app, DESC_ROOM + 3));
    D3D12_UNORDERED_ACCESS_VIEW_DESC mirrorUav{};
    mirrorUav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    mirrorUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    app.device->CreateUnorderedAccessView(app.roomMirror.Get(), nullptr, &mirrorUav, CpuDesc(app, DESC_ROOM + 4));
    D3D12_SHADER_RESOURCE_VIEW_DESC lmSrv{};
    lmSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lmSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lmSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    lmSrv.Texture2DArray.MipLevels = 1;
    lmSrv.Texture2DArray.ArraySize = kRoomFaces;
    app.device->CreateShaderResourceView(app.roomLight.Get(), &lmSrv, CpuDesc(app, DESC_CURVE_MAIN + 3));
    app.device->CreateShaderResourceView(app.roomLight.Get(), &lmSrv, CpuDesc(app, DESC_CURVE_TEST + 3));
    app.device->CreateShaderResourceView(app.roomMirror.Get(), nullptr, CpuDesc(app, DESC_CURVE_MAIN + 4));
    app.device->CreateShaderResourceView(app.roomMirror.Get(), nullptr, CpuDesc(app, DESC_CURVE_TEST + 4));
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = TIME_SLOT * RING;
        UINT64 freq = 0;
        if (FAILED(app.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&app.timeHeap))) || FAILED(app.gfxQueue->GetTimestampFrequency(&freq)) || freq == 0 ||
            !MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_READBACK, (UINT64)TIME_SLOT * RING * sizeof(UINT64), D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, app.timeReadback, nullptr) ||
            FAILED(app.timeReadback->Map(0, nullptr, (void**)&app.timeMapped)))
        {
            Log("InitShaders: GPU timestamps unavailable - the room's GPU time will not be reported");
            app.timeHeap.Reset();
        }
        else app.timeFreq = (double)freq;
    }
    Log("InitShaders: room ready (eye %zu, look eye %zu, v10 eye %zu, light %zu, emit %zu, mirror %zu bytes; up to %d emitters, %d x %d x %d lightmap; "
        "%d x %d mirror picture; %zu-byte constants)", roomEyeCs->GetBufferSize(), roomEyeLookCs->GetBufferSize(), roomEyeV10Cs->GetBufferSize(),
        roomLightCs->GetBufferSize(), roomEmitCs->GetBufferSize(), roomMirrorCs->GetBufferSize(), kRoomMaxEmitters,
        kRoomFaces, kRoomLightmap, kRoomLightmap, kRoomMirrorW, kRoomMirrorMaxH, sizeof(RoomConstants));

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

// The glow for this frame -> ambiTex, and on into the acquired glow image.
// glowImg may be null (self-test: the readback copy is recorded by the caller).
static void RecordAmbilight(App& app, UINT srcDescIndex, const AmbiConstants& c, ID3D12Resource* glowImg)
{
    if (!app.ambiPso || !app.ambiRingPso || !app.ambiTex || !app.ambiRing || !app.ambiHist) return;
    if (!AmbiConstantsUsable(c)) return;

    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    ID3D12DescriptorHeap* heaps[] = { app.descHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(app.ambiRootSig.Get());
    cl->SetComputeRoot32BitConstants(0, sizeof(AmbiConstants) / 4, &c, 0);
    cl->SetComputeRootDescriptorTable(1, GpuDesc(app, srcDescIndex));
    cl->SetComputeRootDescriptorTable(2, GpuDesc(app, DESC_AMBI_UAV));
    cl->SetPipelineState(app.ambiRingPso.Get());
    cl->Dispatch((c.ringN + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER ringDone{};
    ringDone.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ringDone.UAV.pResource = app.ambiRing.Get();
    cl->ResourceBarrier(1, &ringDone);
    cl->SetPipelineState(app.ambiPso.Get());
    cl->Dispatch((c.gw + 7) / 8, (c.gh + 7) / 8, 1);
    if (!glowImg) return;

    Transition(cl, app.ambiTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cl, glowImg, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = glowImg;
    src.pResource = app.ambiTex.Get();
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(cl, glowImg, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(cl, app.ambiTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// The glow around a screen of this size, at this strength (0..1).
// The compositor blends layers in linear light when the swapchain is sRGB (it
// decodes each texel); with a plain UNORM swapchain it blends the stored values.
static bool LinearBlend(const App& app) { return app.colorFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }

static AmbiConstants MakeAmbiConstants(const App& app, int srcW, int srcH, float screenW, float screenH, float strength, bool reset,
                                       bool linearBlend)
{
    AmbiConstants a{};
    if (srcW <= 0 || srcH <= 0 || !(screenW > 0) || !(screenH > 0)) return a;
    a.gw = (uint32_t)app.ambiW; a.gh = (uint32_t)app.ambiH;
    a.srcW = (uint32_t)srcW; a.srcH = (uint32_t)srcH;
    a.screenW = screenW; a.screenH = screenH;
    a.marginM = kAmbiMargin * screenW;
    a.rectW = screenW + 2 * a.marginM;
    a.rectH = screenH + 2 * a.marginM;
    a.intensity = strength < 0 ? 0.0f : (strength > 1 ? 1.0f : strength);
    a.blend = kAmbiBlend;
    a.reset = reset ? 1u : 0u;
    a.soft = kAmbiSoft * screenW;
    a.bezel = kAmbiBezel;
    a.ringN = kAmbiRing;
    a.linearBlend = linearBlend ? 1u : 0u;
    return a;
}

// World colour 0xRRGGBB as stored bytes 0..1 (sRGB-encoded, like the picture).
static void WorldRgb(uint32_t rgb, float out[4])
{
    if (!out) return;
    out[0] = (float)((rgb >> 16) & 255) / 255.0f;
    out[1] = (float)((rgb >> 8) & 255) / 255.0f;
    out[2] = (float)(rgb & 255) / 255.0f;
    out[3] = 1.0f;
}

// Rotation matrix (rows) of a unit quaternion: world <- local.
static void QuatRows(const XrQuaternionf& q, float m[3][3])
{
    if (!m) return;
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}

// The curved pass's constants: each eye's position and view rotation expressed in
// the screen's own frame (origin at its middle, z towards the viewer), and its FOV.
static CurveConstants MakeCurveConstants(const Cylinder& cyl, const XrPosef& screenPose, const XrPosef eyePose[VIEWS],
                                         const XrFovf eyeFov[VIEWS], int ew, int eh, bool glowOn, float glowW, float glowH,
                                         uint32_t worldColor, bool linearBlend)
{
    CurveConstants c{};
    if (!eyePose || !eyeFov) return c;
    c.ew = (uint32_t)ew; c.eh = (uint32_t)eh;
    c.linearBlend = linearBlend ? 1u : 0u;
    c.radius = cyl.radius; c.halfWrap = cyl.halfWrap; c.halfWidth = cyl.halfWidth; c.halfHeight = cyl.halfHeight;
    c.glowOn = glowOn ? 1u : 0u;
    c.glowHalfW = glowW * 0.5f; c.glowHalfH = glowH * 0.5f;
    c.glowRadius = cyl.radius + kAmbiBehind;
    WorldRgb(worldColor, c.world);
    float S[3][3];
    QuatRows(screenPose.orientation, S);
    for (uint32_t e = 0; e < VIEWS; e++)
    {
        float E[3][3];
        QuatRows(eyePose[e].orientation, E);
        CurveEye& v = c.eye[e];
        float* rows[3] = { v.row0, v.row1, v.row2 };
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++)
                rows[r][k] = S[0][r] * E[0][k] + S[1][r] * E[1][k] + S[2][r] * E[2][k];     // S^T E
        const float rel[3] = { eyePose[e].position.x - screenPose.position.x, eyePose[e].position.y - screenPose.position.y,
                               eyePose[e].position.z - screenPose.position.z };
        for (int r = 0; r < 3; r++) v.origin[r] = S[0][r] * rel[0] + S[1][r] * rel[1] + S[2][r] * rel[2];
        v.tanL = tanf(eyeFov[e].angleLeft); v.tanR = tanf(eyeFov[e].angleRight);
        v.tanU = tanf(eyeFov[e].angleUp); v.tanD = tanf(eyeFov[e].angleDown);
    }
    return c;
}

// The world colour into the acquired world image (RENDER_TARGET at rest), from this
// frame slot's upload buffer - its previous copy finished before the slot was reused.
static void RecordWorldColour(App& app, int ring, uint32_t rgb, ID3D12Resource* img)
{
    if (!img || ring < 0 || ring >= RING || !app.worldMapped[ring]) return;

    for (uint32_t e = 0; e < VIEWS; e++)
        for (UINT y = 0; y < app.worldFp[e].Footprint.Height; y++)
        {
            unsigned char* row = app.worldMapped[ring] + app.worldFp[e].Offset + (size_t)y * app.worldFp[e].Footprint.RowPitch;
            for (UINT x = 0; x < app.worldFp[e].Footprint.Width; x++)
            {
                row[x * 4 + 0] = (unsigned char)((rgb >> 16) & 255);
                row[x * 4 + 1] = (unsigned char)((rgb >> 8) & 255);
                row[x * 4 + 2] = (unsigned char)(rgb & 255);
                row[x * 4 + 3] = 255;
            }
        }
    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    Transition(cl, img, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT e = 0; e < VIEWS; e++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = e; dst.pResource = img;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = app.worldFp[e]; src.pResource = app.worldUp[ring].Get();
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Transition(cl, img, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

// The room's light for this frame into its lightmap (UAV at rest): each emitter's
// radiance (EMIT), with mirrorH above 0 (the constants' mirrorH: Reflections are on) the
// mirror picture's first mirrorH rows (MIRROR, UAV at rest), then every lightmap texel
// (LIGHT). `geometry` (upload heap), when given, is the emitters' new geometry - it also
// clears their smoothing history, so that frame's constants must carry alpha 1.
// With a timeBase (not UINT_MAX) it writes timestamps T1 (after EMIT, MIRROR and their
// barrier) and T2 (after LIGHT and its barrier) at timeBase + 1 and + 2 of `timeHeap`
// (null: the frame loop's). Returns false, recording nothing, when the pass cannot run.
static bool RecordRoomLight(App& app, UINT srcDescIndex, D3D12_GPU_VIRTUAL_ADDRESS constants, ID3D12Resource* geometry, UINT emitters,
                            UINT mirrorH, UINT timeBase = UINT_MAX, ID3D12QueryHeap* timeHeap = nullptr)
{
    if (!app.roomEmitPso || !app.roomLightPso || !app.roomEmitters || !app.roomLight || !app.ambiTex) return false;
    if (constants == 0 || emitters == 0 || emitters > (UINT)kRoomMaxEmitters) return false;
    if (mirrorH > (UINT)kRoomMirrorMaxH || (mirrorH > 0 && (!app.roomMirrorPso || !app.roomMirror))) return false;
    ID3D12QueryHeap* heap = timeBase == UINT_MAX ? nullptr : (timeHeap ? timeHeap : app.timeHeap.Get());
    if (timeBase != UINT_MAX && !heap) return false;

    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    if (geometry)
    {
        Transition(cl, app.roomEmitters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyBufferRegion(app.roomEmitters.Get(), 0, geometry, 0, (UINT64)emitters * sizeof(RoomEmitter));
        Transition(cl, app.roomEmitters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    Transition(cl, app.ambiTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = { app.descHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(app.roomRootSig.Get());
    cl->SetComputeRootConstantBufferView(0, constants);
    cl->SetComputeRootDescriptorTable(1, GpuDesc(app, srcDescIndex));
    cl->SetComputeRootDescriptorTable(2, GpuDesc(app, DESC_ROOM));
    cl->SetPipelineState(app.roomEmitPso.Get());
    cl->Dispatch(emitters, 1, 1);
    if (mirrorH > 0)
    {
        cl->SetPipelineState(app.roomMirrorPso.Get());
        cl->Dispatch(kRoomMirrorW / 8, (mirrorH + 7) / 8, 1);
    }
    D3D12_RESOURCE_BARRIER emitted[2]{};
    emitted[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    emitted[0].UAV.pResource = app.roomEmitters.Get();
    emitted[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    emitted[1].UAV.pResource = app.roomMirror.Get();
    cl->ResourceBarrier(mirrorH > 0 ? 2 : 1, emitted);
    if (heap) cl->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, timeBase + 1);
    cl->SetPipelineState(app.roomLightPso.Get());
    cl->Dispatch(kRoomLightmap / 8, kRoomLightmap / 8, kRoomFaces);
    D3D12_RESOURCE_BARRIER lit{};
    lit.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    lit.UAV.pResource = app.roomLight.Get();
    cl->ResourceBarrier(1, &lit);
    if (heap) cl->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, timeBase + 2);
    Transition(cl, app.ambiTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

// The room's eye pass for these constants: the one with the v11 controls' code while any
// of them is on (RoomLookOn), else the plain one - Stage 1's pass exactly, which the
// controls' code, even when it is not run, would slow. Null if they are not built.
static ID3D12PipelineState* RoomEyePso(const App& app, const RoomConstants& rc)
{
    return RoomLookOn(rc) ? app.curveRoomLookPso.Get() : app.curveRoomPso.Get();
}

// Ray-cast the curved screen into `eyes` (UAV at rest) from `picture` (the warp's
// output, COPY_SOURCE as RecordWarp leaves it; left that way). With eyeImg, copy
// the result into the acquired swapchain image (RENDER_TARGET at rest).
// `roomConstants` non-zero: the room's variant, which also reads the room's lightmap and
// mirror picture (UAV at rest, shader resources for the pass) and shows the room round
// the screen.
// Only the c.ew x c.eh corner of the buffers is drawn and copied (a flat screen's
// room layer is drawn at half size).
// With a timeBase (not UINT_MAX) it writes timestamps T3 (after the pass and its
// transitions back) and T4 (after the copy; at once without eyeImg) at timeBase + 3 and
// + 4 of `timeHeap` (null: the frame loop's). `roomPso` (null: curveRoomPso) picks the
// room's eye pass - RoomEyePso's, or curveRoomV10Pso for the benchmark and --room-v10-eye. Returns
// false, recording nothing, when the pass cannot run.
static bool RecordCurvedScreen(App& app, UINT tableIndex, ID3D12Resource* picture, const CurveConstants& c,
                               ID3D12Resource* eyes, ID3D12Resource* eyeImg, D3D12_GPU_VIRTUAL_ADDRESS roomConstants = 0,
                               UINT timeBase = UINT_MAX, ID3D12QueryHeap* timeHeap = nullptr, ID3D12PipelineState* roomPso = nullptr)
{
    if (!picture || !eyes || !app.curvePso || !app.ambiTex) return false;
    if (c.ew == 0 || c.eh == 0) return false;
    const bool room = roomConstants != 0;
    ID3D12PipelineState* eyePso = room ? (roomPso ? roomPso : app.curveRoomPso.Get()) : app.curvePso.Get();
    if (room && (!eyePso || !app.roomLight || !app.roomMirror)) return false;
    ID3D12QueryHeap* heap = timeBase == UINT_MAX ? nullptr : (timeHeap ? timeHeap : app.timeHeap.Get());
    if (timeBase != UINT_MAX && !heap) return false;

    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    Transition(cl, picture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cl, app.ambiTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (room) Transition(cl, app.roomLight.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (room) Transition(cl, app.roomMirror.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = { app.descHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(room ? app.curveRoomRootSig.Get() : app.curveRootSig.Get());
    cl->SetPipelineState(eyePso);
    cl->SetComputeRoot32BitConstants(0, sizeof(CurveConstants) / 4, &c, 0);
    cl->SetComputeRootDescriptorTable(1, GpuDesc(app, tableIndex));
    if (room) cl->SetComputeRootConstantBufferView(2, roomConstants);
    cl->Dispatch((c.ew + 7) / 8, (c.eh + 7) / 8, VIEWS);
    if (room) Transition(cl, app.roomMirror.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (room) Transition(cl, app.roomLight.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cl, app.ambiTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cl, picture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (heap) cl->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, timeBase + 3);
    if (!eyeImg)
    {
        if (heap) cl->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, timeBase + 4);
        return true;
    }

    Transition(cl, eyes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cl, eyeImg, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_BOX box{ 0, 0, 0, c.ew, c.eh, 1 };
    for (UINT e = 0; e < VIEWS; e++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = e; dst.pResource = eyeImg;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = e; src.pResource = eyes;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    Transition(cl, eyeImg, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(cl, eyes, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (heap) cl->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, timeBase + 4);
    return true;
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

// A DirectML device on `device` (DirectML.dll ships next to the executable).
static bool CreateDmlDevice(ID3D12Device* device, ComPtr<IDMLDevice>& out)
{
    if (!device) { Log("CreateDmlDevice: FAIL no device"); return false; }
    HMODULE dmllib = LoadLibraryW(L"DirectML.dll");
    if (!dmllib) { Log("CreateDmlDevice: FAIL DirectML.dll"); return false; }
    auto createDmlDevice = (HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**))
        GetProcAddress(dmllib, "DMLCreateDevice");
    if (!createDmlDevice) { Log("CreateDmlDevice: FAIL DMLCreateDevice export"); return false; }
    if (FAILED(createDmlDevice(device, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&out)))) { Log("CreateDmlDevice: FAIL DMLCreateDevice"); return false; }
    return true;
}

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

    if (!CreateDmlDevice(app.inferDevice.Get(), app.dmlDevice)) return false;
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
    // DirectML runs the model on the GPU; ORT's CPU thread pool must not spin after
    // each pass, taking cores from the capture thread and steady/fuse maths.
    for (const char* key : { "session.intra_op.allow_spinning", "session.inter_op.allow_spinning" })
        if (OrtStatus* st = ort->AddSessionConfigEntry(so, key, "0")) { Fail("AddSessionConfigEntry", st); ort->ReleaseStatus(st); }
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

static void WaitXferFence(App& app, UINT64 value)
{
    if (value == 0 || !app.xferFence) return;
    if (app.xferFence->GetCompletedValue() >= value) return;

    app.xferFence->SetEventOnCompletion(value, app.xferFenceEvent);
    WaitForSingleObject(app.xferFenceEvent, INFINITE);
}

// Frame-transfer path: headset GPU copy engine sends the model-size frame `frame`
// (after `srcFence` reaches `srcValue`), the inference GPU unpacks it into modelIn;
// ORT's work follows on the inference queue. Returns the copy-engine fence value
// that marks when `frame` has been read.
static UINT64 SubmitFrameTransfer(App& app, ID3D12Resource* frame, ID3D12Fence* srcFence, UINT64 srcValue)
{
    if (!frame || !app.frameTransfer) return 0;
    const ModelSpec& spec = *app.opt.model;

    // The shared buffer must be free (previous unpack done) and the copy allocator idle.
    WaitInferFence(app, app.inferFenceVal);
    WaitXferFence(app, app.xferFenceVal);

    app.xferAlloc->Reset();
    app.xferList->Reset(app.xferAlloc.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = app.crossBuf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = app.xferFootprint;
    src.pResource = frame;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    app.xferList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    app.xferList->Close();
    if (srcFence) app.xferQueue->Wait(srcFence, srcValue);
    ID3D12CommandList* lists[] = { app.xferList.Get() };
    app.xferQueue->ExecuteCommandLists(1, lists);
    app.xferQueue->Signal(app.xferFence.Get(), ++app.xferFenceVal);
    app.xferQueue->Signal(app.crossFence.Get(), ++app.crossVal);

    app.inferCmdAlloc->Reset();
    app.inferCmdList->Reset(app.inferCmdAlloc.Get(), nullptr);
    ID3D12GraphicsCommandList* il = app.inferCmdList.Get();
    Transition(il, app.frameLocal.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    il->CopyBufferRegion(app.frameLocal.Get(), 0, app.crossBufInfer.Get(), 0, app.xferBytes);
    Transition(il, app.frameLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    UnpackConstants uc{};
    uc.sw = (uint32_t)app.xferW; uc.sh = (uint32_t)app.xferH; uc.pitch = app.xferFootprint.Footprint.RowPitch;
    uc.dw = (uint32_t)spec.inW; uc.dh = (uint32_t)spec.inH; uc.normalize = spec.imagenetNorm ? 1u : 0u;
    il->SetComputeRootSignature(app.unpackRootSig.Get());
    il->SetPipelineState(app.unpackPso.Get());
    il->SetComputeRoot32BitConstants(0, sizeof(uc) / 4, &uc, 0);
    il->SetComputeRootShaderResourceView(1, app.frameLocal->GetGPUVirtualAddress());
    il->SetComputeRootUnorderedAccessView(2, app.modelIn->GetGPUVirtualAddress());
    il->Dispatch((spec.inW + 7) / 8, (spec.inH + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = app.modelIn.Get();
    il->ResourceBarrier(1, &uav);
    Transition(il, app.frameLocal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    il->Close();
    app.inferQueue->Wait(app.crossFenceInfer.Get(), app.crossVal);
    ID3D12CommandList* inferLists[] = { il };
    app.inferQueue->ExecuteCommandLists(1, inferLists);
    app.inferQueue->Signal(app.inferFence.Get(), ++app.inferFenceVal);
    return app.xferFenceVal;
}

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
static void WaitXferFence(App& app, UINT64 value);

static void WaitDepthIdle(App& app)
{
    if (app.mlFence && app.mlFenceVal) WaitMlFence(app, app.mlFenceVal);
    WaitXferFence(app, app.xferFenceVal);
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

// ------------------------------------------------ steadying and model fusion
// See XMMODEL.md and depth_fusion.h. Both work on the published near map, before
// DilateNear, and both are off unless the desktop app (or --steady / --fuse) turns
// them on; with both off none of this runs.
static const double FUSE_MAX_AGE = 0.3;     // s: an older anchor only supplies the global scale
static const size_t FAST_HISTORY = 16;      // fast-model maps kept for fitting the anchor

static bool FuseActive(const App& app)
{
    return app.fuseEnabled.load() && app.opt.model == &kZipDepth && !app.anchorFailed.load();
}

// Grid image of the texture at descriptor `srcDesc` (srcW x srcH) on the ml queue,
// copied to the readback buffer. Returns the ml fence value that marks it complete
// (0 = not submitted).
static UINT64 SubmitGridDesc(App& app, UINT srcDesc, int srcW, int srcH, ID3D12Fence* srcFence, UINT64 srcValue)
{
    if (!app.gridPso || !app.gridList || srcW <= 0 || srcH <= 0) return 0;

    WaitMlFence(app, app.gridLast);                 // the allocator and readback must be idle
    PrepConstants pc{};
    pc.dw = (uint32_t)W; pc.dh = (uint32_t)H;
    pc.exactLoad = (srcW == W && srcH == H) ? 1u : 0u;
    pc.taps = (uint32_t)std::clamp(int(std::ceil((double)srcW / W)), 1, 4);

    app.gridAlloc->Reset();
    app.gridList->Reset(app.gridAlloc.Get(), nullptr);
    ID3D12GraphicsCommandList* cl = app.gridList.Get();
    ID3D12DescriptorHeap* heaps[] = { app.descHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(app.prepRootSig.Get());
    cl->SetPipelineState(app.gridPso.Get());
    cl->SetComputeRoot32BitConstants(0, sizeof(PrepConstants) / 4, &pc, 0);
    cl->SetComputeRootDescriptorTable(1, GpuDesc(app, srcDesc));
    cl->SetComputeRootUnorderedAccessView(2, app.gridBuf->GetGPUVirtualAddress());
    cl->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    Transition(cl, app.gridBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(app.gridReadback.Get(), 0, app.gridBuf.Get(), 0, (UINT64)W * H * sizeof(uint32_t));
    Transition(cl, app.gridBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->Close();

    if (srcFence) app.mlQueue->Wait(srcFence, srcValue);
    ID3D12CommandList* lists[] = { cl };
    app.mlQueue->ExecuteCommandLists(1, lists);
    app.mlQueue->Signal(app.mlFence.Get(), ++app.mlFenceVal);
    app.gridLast = app.mlFenceVal;
    return app.gridLast;
}

static UINT64 SubmitGrid(App& app, const SourceRef& src)
{
    if (!src) return 0;
    const UINT64 done = SubmitGridDesc(app, DESC_SRC0 + (UINT)src->index, app.srcW, app.srcH, SourceFence(app), src->value);
    if (done) app.sources.MarkRead(src, SourceFrames::Reader::Model, done);
    return done;
}

// The hardware motion estimator, created on first use on the headset GPU (where the
// captured frames are). False, logged once, when this GPU has none.
static bool EnsureMotion(App& app)
{
    if (app.motion.ok()) return true;
    if (app.motionTried) return false;
    app.motionTried = true;
    if (app.motion.Init(app.device.Get(), W, H, &Log)) return true;
    Log("EnsureMotion: no hardware motion estimation on this GPU; steady depth and model fusion have no effect");
    return false;
}

// Anchor thread: Depth Anything V2 on its own queue on the depth GPU, fed the newest
// grid image the depth worker hands over. Normal queue priority (the fast model's
// queue may be HIGH); without a second GPU it is capped at 10 passes per second so it
// does not take the game's GPU time.
static void AnchorMain(App* app)
{
    g_threadName = "anchor";
    if (!app) return;
    const ModelSpec& spec = kDepthAnythingV2;
    Log("AnchorMain: enter (%s on %ls%s)", spec.name, app->inferName.c_str(), app->secondGpu ? ", second GPU" : ", headset GPU, capped at 10 per second");
    if (spec.inW != W || spec.inH != H) { Log("AnchorMain: FAIL model input %dx%d is not the depth grid", spec.inW, spec.inH); app->anchorFailed = true; return; }

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDMLDevice> dml;
    OrtSession* session = nullptr;
    OrtMemoryInfo* cpu = nullptr;
    auto cleanup = [&]()
    {
        if (cpu) { ort->ReleaseMemoryInfo(cpu); cpu = nullptr; }
        if (session) { ort->ReleaseSession(session); session = nullptr; }
    };
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!app->inferDevice || FAILED(app->inferDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || !CreateDmlDevice(app->inferDevice.Get(), dml))
    { Log("AnchorMain: FAIL queue/DirectML; model fusion off"); app->anchorFailed = true; return; }
    OrtSessionOptions* so = nullptr;
    if (OrtStatus* st = ort->CreateSessionOptions(&so)) { Fail("CreateSessionOptions", st); ort->ReleaseStatus(st); app->anchorFailed = true; return; }
    // GPU model: one non-spinning CPU thread, so it never competes with the depth worker.
    OrtStatus* st = ort->SetIntraOpNumThreads(so, 1);
    for (const char* key : { "session.intra_op.allow_spinning", "session.inter_op.allow_spinning" })
        if (!st) st = ort->AddSessionConfigEntry(so, key, "0");
    if (!st) st = app->dmlApi->SessionOptionsAppendExecutionProvider_DML1(so, dml.Get(), queue.Get());
    if (!st) st = ort->CreateSession(app->env, ModelPath(spec.file).c_str(), so, &session);
    ort->ReleaseSessionOptions(so);
    if (!st) st = ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &cpu);
    if (st) { Fail("AnchorMain session", st); ort->ReleaseStatus(st); cleanup(); app->anchorFailed = true; return; }
    Log("AnchorMain: ready");

    std::vector<float> input((size_t)3 * W * H), raw((size_t)W * H);
    const int64_t dims[4] = { 1, 3, H, W };
    const float mean[3] = { 0.485f, 0.456f, 0.406f }, istd[3] = { 1 / 0.229f, 1 / 0.224f, 1 / 0.225f };
    RangeSmoother smoother;
    std::vector<double> passMs;
    double lastStart = 0, nextLog = NowSeconds() + 5.0;
    const double minInterval = app->secondGpu ? 0.0 : 0.1;
    uint64_t nextId = 1;
    int failures = 0;
    while (!app->stop.load())
    {
        const double wait = lastStart + minInterval - NowSeconds();
        if (wait > 0) Sleep((DWORD)(wait * 1000) + 1);
        App::AnchorJob job;
        {
            std::unique_lock<std::mutex> lock(app->anchorMutex);
            app->anchorCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return app->stop.load() || app->anchorJob.valid; });
            if (app->stop.load()) break;
            if (!app->anchorJob.valid) continue;
            job = std::move(app->anchorJob);
            app->anchorJob.valid = false;
        }
        if (!FuseActive(*app) || job.rgba.size() != (size_t)W * H) continue;
        lastStart = NowSeconds();

        const size_t plane = (size_t)W * H;
        for (size_t i = 0; i < plane; i++)
        {
            const uint32_t p = job.rgba[i];
            for (int c = 0; c < 3; c++) input[c * plane + i] = (((p >> (8 * c)) & 255) / 255.0f - mean[c]) * istd[c];
        }
        OrtValue* in = nullptr;
        OrtValue* out = nullptr;
        const char* inName = spec.inName;
        const char* outName = spec.outName;
        OrtStatus* rs = ort->CreateTensorWithDataAsOrtValue(cpu, input.data(), input.size() * sizeof(float), dims, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in);
        if (!rs) rs = ort->Run(session, nullptr, &inName, (const OrtValue* const*)&in, 1, &outName, 1, &out);
        float* dp = nullptr;
        size_t count = 0;
        if (!rs)
        {
            OrtTensorTypeAndShapeInfo* info = nullptr;
            rs = ort->GetTensorTypeAndShape(out, &info);
            if (!rs) { rs = ort->GetTensorShapeElementCount(info, &count); ort->ReleaseTensorTypeAndShapeInfo(info); }
            if (!rs) rs = ort->GetTensorMutableData(out, (void**)&dp);
        }
        const bool good = !rs && dp && count == plane && std::all_of(dp, dp + count, [](float v) { return std::isfinite(v); });
        if (good) memcpy(raw.data(), dp, plane * sizeof(float));
        if (rs) { Fail("AnchorMain run", rs); ort->ReleaseStatus(rs); }
        if (out) ort->ReleaseValue(out);
        if (in) ort->ReleaseValue(in);
        if (!good)
        {
            if (++failures >= 3) { Log("AnchorMain: 3 failures; model fusion off"); app->anchorFailed = true; break; }
            Log("AnchorMain: pass failed (%d/3)", failures);
            continue;
        }
        failures = 0;

        float lo = 0, hi = 1;
        SmoothRange(smoother, raw, NowSeconds(), app->opt.smooth, app->opt.smoothTau, &lo, &hi);
        const float inv = (hi - lo) > 1e-6f ? 1.0f / (hi - lo) : 0.0f;
        fusion::Image nearMap(W, H);
        for (size_t i = 0; i < plane; i++) nearMap.v[i] = std::clamp((raw[i] - lo) * inv, 0.0f, 1.0f);
        {
            std::lock_guard<std::mutex> lock(app->anchorMutex);
            app->anchorResult.nearMap = std::move(nearMap);
            app->anchorResult.luma = std::move(job.luma);
            app->anchorResult.seq = job.seq;
            app->anchorResult.time = job.time;
            app->anchorResult.id = nextId++;
        }
        app->anchorPasses++;
        passMs.push_back((NowSeconds() - lastStart) * 1000.0);
        if (NowSeconds() >= nextLog && !passMs.empty())
        {
            std::sort(passMs.begin(), passMs.end());
            Log("Anchor: %zu passes in 5 s | pass ms p50 %.1f p95 %.1f", passMs.size(), passMs[passMs.size() / 2], passMs[passMs.size() * 95 / 100]);
            passMs.clear();
            nextLog = NowSeconds() + 5.0;
        }
    }
    cleanup();
    Log("AnchorMain: exit");
}

static void EnsureAnchor(App& app)
{
    if (app.anchorThread.joinable() || app.anchorFailed.load()) return;
    app.anchorThread = std::thread(AnchorMain, &app);
}

// Steadying and/or fusion of this pass's near map (in place). Never fails the pass:
// anything unavailable leaves the map as it was. Stage times go to postStageMs:
// 0 grid wait + luma, 1 luma upload, 2 anchor hand-over + result, 3 motion estimates,
// 4 fusion maths, 5 steadying maths.
static void PostProcessDepth(App& app, const SourceRef& src, UINT64 gridDone, std::vector<float>& nearMap)
{
    if (!src || gridDone == 0 || nearMap.size() != (size_t)W * H) return;
    const bool steady = app.steadyEnabled.load();
    const bool fuse = FuseActive(app);
    if (!steady && !fuse) return;
    const auto t0 = std::chrono::steady_clock::now();
    auto stageStart = t0;
    auto stage = [&](int i)
    {
        const auto now = std::chrono::steady_clock::now();
        app.postStageMs[i] += std::chrono::duration<double, std::milli>(now - stageStart).count();
        stageStart = now;
    };

    WaitMlFence(app, gridDone);
    std::vector<uint32_t> rgba(app.gridMapped, app.gridMapped + (size_t)W * H);
    fusion::Image luma = fusion::LumaFromRgba(rgba.data(), W, H);
    if (!EnsureMotion(app)) return;
    if (app.postLayout != src->layout)
    {
        // A resized/moved source: nothing from before lines up any more.
        app.postLayout = src->layout;
        app.steadyHave = false;
        app.fastHistory.clear();
        app.anchorFit = false;
    }
    stage(0);
    const int cur = app.motionCurSlot;
    if (!app.motion.Upload(cur, luma.v.data())) { Log("PostProcessDepth: luma upload failed"); return; }
    stage(1);

    fusion::Image z(W, H);
    z.v = nearMap;

    // Fusion bookkeeping: hand this frame to the anchor, take its newest result.
    double anchorAge = 0;
    bool useAnchor = false, anchorSameFrame = false;
    if (fuse)
    {
        EnsureAnchor(app);
        {
            std::lock_guard<std::mutex> lock(app.anchorMutex);
            app.anchorJob.rgba = std::move(rgba);
            app.anchorJob.luma = luma;
            app.anchorJob.seq = src->seq;
            app.anchorJob.time = src->time;
            app.anchorJob.valid = true;
        }
        app.anchorCv.notify_one();
        app.fastHistory.push_back({ src->seq, z });
        while (app.fastHistory.size() > FAST_HISTORY) app.fastHistory.pop_front();

        bool fresh = false;
        {
            std::lock_guard<std::mutex> lock(app.anchorMutex);
            if (app.anchorResult.id != 0 && app.anchorResult.id != app.anchorUsedId)
            {
                app.anchorCurrent = app.anchorResult;
                fresh = true;
            }
        }
        if (fresh)
        {
            app.anchorUsedId = app.anchorCurrent.id;
            if (!app.motion.Upload(2, app.anchorCurrent.luma.v.data())) Log("PostProcessDepth: anchor luma upload failed");
            // Fit the anchor onto the fast model's map of the same frame: the global
            // mapping between the two models' scales.
            for (const auto& [seq, map] : app.fastHistory)
            {
                if (seq != app.anchorCurrent.seq) continue;
                fusion::GlobalFit(map, app.anchorCurrent.nearMap, app.anchorGa, app.anchorGb);
                app.anchorFit = true;
                break;
            }
        }
        anchorAge = src->time - app.anchorCurrent.time;
        useAnchor = app.anchorCurrent.id != 0 && app.anchorFit && anchorAge >= 0 && anchorAge <= FUSE_MAX_AGE;
        anchorSameFrame = useAnchor && app.anchorCurrent.seq == src->seq;
    }
    else
    {
        app.fastHistory.clear();
        app.anchorFit = false;
    }
    stage(2);

    // The motion estimates this pass needs, in one submission.
    MotionEstimator::Pair pairs[MotionEstimator::MAX_BATCH];
    std::vector<int16_t> vectors[MotionEstimator::MAX_BATCH];
    int count = 0, anchorIndex = -1, prevIndex = -1;
    if (useAnchor && !anchorSameFrame) { anchorIndex = count; pairs[count++] = { cur, 2 }; }
    if (steady && app.steadyHave && app.steadyPrevSeq != src->seq) { prevIndex = count; pairs[count++] = { cur, 1 - cur }; }
    const bool estimated = count > 0 && app.motion.EstimateMany(pairs, count, vectors);
    if (count > 0 && !estimated) { anchorIndex = prevIndex = -1; Log("PostProcessDepth: motion estimate failed"); }
    stage(3);
    const int bw = app.motion.blocksW(), bh = app.motion.blocksH();

    fusion::Image out = z;
    if (fuse && app.anchorCurrent.id != 0 && app.anchorFit)
    {
        fusion::Image fused;
        if (useAnchor && (anchorSameFrame || anchorIndex >= 0))
        {
            fusion::Image moved, trust;
            if (anchorSameFrame) moved = app.anchorCurrent.nearMap;
            else
            {
                const fusion::Motion m = fusion::MotionFromVectors(vectors[anchorIndex].data(), bw, bh, W, H);
                float trustMean = 0;
                trust = fusion::MotionTrust(luma, app.anchorCurrent.luma, m, &trustMean);
                moved = fusion::Remap(app.anchorCurrent.nearMap, m);
                app.postTrustSum += trustMean;
            }
            if (moved.valid()) fused = fusion::Fuse(z, moved, trust, app.anchorGa, app.anchorGb);
            if (fused.valid()) { app.postFused++; app.postAgeSum += anchorAge; }
        }
        if (!fused.valid())
        {
            // Anchor too old (or motion unavailable): keep the anchor's scale so the
            // depth does not jump between the two models' ranges.
            fused = fusion::Image(W, H);
            for (size_t i = 0; i < fused.v.size(); i++) fused.v[i] = std::clamp(app.anchorGa * z.v[i] + app.anchorGb, 0.0f, 1.0f);
        }
        out = std::move(fused);
    }
    stage(4);

    if (steady)
    {
        if (prevIndex >= 0)
        {
            const fusion::Motion m = fusion::MotionFromVectors(vectors[prevIndex].data(), bw, bh, W, H);
            const fusion::Image trust = fusion::MotionTrust(luma, app.steadyPrevLuma, m);
            out = fusion::Steady(out, fusion::Remap(app.steadyPrevOut, m), trust);
            app.postSteadied++;
        }
        app.steadyPrevOut = out;
        app.steadyPrevLuma = std::move(luma);
        app.steadyPrevSeq = src->seq;
        app.steadyHave = true;
        app.motionCurSlot = 1 - cur;
    }
    else
    {
        app.steadyHave = false;
    }
    stage(5);

    nearMap = std::move(out.v);
    app.postPasses++;
    app.postMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
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
    if (app.frameTransfer && app.smallTex[src->index])
    {
        const UINT64 sent = SubmitFrameTransfer(app, app.smallTex[src->index].Get(), SourceFence(app), src->value);
        app.sources.MarkRead(src, SourceFrames::Reader::Transfer, sent);
    }
    else
    {
        const UINT64 prepDone = SubmitPrep(app, DESC_SRC0 + (UINT)src->index, app.srcW, app.srcH, SourceFence(app), src->value);
        app.sources.MarkRead(src, SourceFrames::Reader::Model, prepDone);
    }
    // Steady/fuse need this frame on the depth grid; queued now so the GPU makes it
    // while the model runs.
    const bool post = app.steadyEnabled.load() || FuseActive(app);
    const UINT64 gridDone = post ? SubmitGrid(app, src) : 0;
    if (app.fuseEnabled.load() && app.opt.model != &kZipDepth && !app.fuseModelLogged)
    {
        Log("ComputeAndPublish: model fusion needs ZipDepth as the main model; off while %s is selected", app.opt.model->name);
        app.fuseModelLogged = true;
    }
    if (!RunModelRaw(app)) return false;
    double modelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m0).count();
    app.passMs.push_back(modelMs);                  // worker thread only; summarised by WorkerMain

    float lo = 0, hi = 1;
    SmoothRange(app.smoother, app.rawDepth, NowSeconds(), app.opt.smooth, app.opt.smoothTau, &lo, &hi);
    const float inv = (hi - lo) > 1e-6f ? 1.0f / (hi - lo) : 0.0f;
    nearScratch.resize(app.rawDepth.size());
    for (size_t i = 0; i < nearScratch.size(); i++)
        nearScratch[i] = std::min(1.0f, std::max(0.0f, (app.rawDepth[i] - lo) * inv));
    PostProcessDepth(app, src, gridDone, nearScratch);

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
    // Crops need the full-resolution frame, so they use the prep path.
    const UINT64 cropPrepDone = SubmitPrep(app, DESC_SRC0 + (UINT)src->index, app.srcW, app.srcH, SourceFence(app), src->value, crop);
    app.sources.MarkRead(src, SourceFrames::Reader::Model, cropPrepDone);
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
    double nextSummary = NowSeconds() + 2.0;
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
        if (NowSeconds() >= nextSummary && !app->passMs.empty())
        {
            // Latency per full depth pass: submit (prep or frame send) to the model's
            // output being on the CPU. Includes any wait for a turn on a busy GPU.
            auto& v = app->passMs;
            std::sort(v.begin(), v.end());
            auto pct = [&](double q) { return v[std::min(v.size() - 1, (size_t)(q * v.size()))]; };
            Log("Worker: %zu passes in 2 s | pass ms p50 %.1f p95 %.1f max %.1f | %s", v.size(), pct(.5), pct(.95), v.back(),
                !app->secondGpu ? "one GPU" : app->frameTransfer ? "second GPU, copy-engine frames" : "second GPU, prep on headset GPU");
            v.clear();
            if (app->postPasses > 0 && !app->postMs.empty())
            {
                // Steady/fuse: their own time after the model (grid wait, motion
                // estimates, CPU maths), how many passes each changed, anchor state.
                auto& p = app->postMs;
                std::sort(p.begin(), p.end());
                Log("Worker: steady %s, fuse %s | post ms p50 %.1f p95 %.1f | steadied %u, fused %u of %u | anchor passes %u, mean age %.0f ms, motion trust %.2f",
                    app->steadyEnabled.load() ? "on" : "off", FuseActive(*app) ? "on" : "off", p[p.size() / 2], p[std::min(p.size() - 1, p.size() * 95 / 100)],
                    app->postSteadied, app->postFused, app->postPasses, app->anchorPasses.exchange(0),
                    app->postFused ? app->postAgeSum / app->postFused * 1000.0 : 0.0, app->postFused ? app->postTrustSum / app->postFused : 0.0);
                p.clear();
                app->postSteadied = app->postFused = app->postPasses = 0;
                app->postAgeSum = app->postTrustSum = 0;
            }
            nextSummary = NowSeconds() + 2.0;
        }
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
    // STAGE: its origin is on the real floor, which the room's floor uses when it can.
    XrReferenceSpaceCreateInfo stageInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    stageInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    stageInfo.poseInReferenceSpace = { {0,0,0,1}, {0,0,0} };
    const XrResult stageResult = xrCreateReferenceSpace_(app.session, &stageInfo, &app.stage);
    if (XR_FAILED(stageResult)) { app.stage = XR_NULL_HANDLE; Log("InitXrSession: no STAGE space (%s) - the room will guess the floor", XRStr(stageResult)); }

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
    app.colorFormat = colorFmt;

    // The curved screen renders whole eye buffers at the runtime's recommended size.
    uint32_t viewCount = 0;
    XrViewConfigurationView vcv[VIEWS] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    const XrResult vr = xrEnumViewConfigs_(app.instance, app.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, VIEWS, &viewCount, vcv);
    if (XR_SUCCEEDED(vr) && viewCount == VIEWS)
    {
        app.eyeW = (int)std::min(std::min(vcv[0].recommendedImageRectWidth, vcv[0].maxImageRectWidth), 4096u);
        app.eyeH = (int)std::min(std::min(vcv[0].recommendedImageRectHeight, vcv[0].maxImageRectHeight), 4096u);
        Log("InitXrSession: recommended eye buffer %ux%u (max %ux%u), curved screen uses %dx%d",
            vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight, vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight,
            app.eyeW, app.eyeH);
    }
    else Log("InitXrSession: view configuration %s (%u views) - the screen cannot curve", XRStr(vr), viewCount);
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

    // Ambilight layer. Always created (the option applies live); a runtime that
    // refuses it simply plays without the glow.
    AmbiSizeFor(app.colorW, app.colorH, &app.ambiW, &app.ambiH);
    if (app.ambiW > 0 && app.ambiH > 0)
    {
        XrSwapchainCreateInfo gs{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        gs.sampleCount = 1; gs.width = (uint32_t)app.ambiW; gs.height = (uint32_t)app.ambiH;
        gs.faceCount = 1; gs.arraySize = 1; gs.mipCount = 1;
        gs.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        gs.format = colorFmt;
        const XrResult gr = xrCreateSwapchain_(app.session, &gs, &app.ambiSc);
        if (XR_FAILED(gr)) { Log("InitXrSession: ambilight swapchain %s - playing without the glow", XRStr(gr)); app.ambiSc = XR_NULL_HANDLE; }
        else
        {
            uint32_t gn = 0;
            xrEnumImages_(app.ambiSc, 0, &gn, nullptr);
            app.aimgs.assign(gn, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
            xrEnumImages_(app.ambiSc, gn, &gn, (XrSwapchainImageBaseHeader*)app.aimgs.data());
            if (gn == 0 || !app.aimgs[0].texture) { Log("InitXrSession: ambilight swapchain has no images - playing without the glow"); app.ambiSc = XR_NULL_HANDLE; }
            else Log("InitXrSession: ambilight swapchain %dx%d, %u images", app.ambiW, app.ambiH, gn);
        }
    }

    // World colour layer (behind a flat screen). Tiny and always made, so the colour
    // can be changed live; a runtime that refuses it simply keeps the world black.
    {
        XrSwapchainCreateInfo ws{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ws.sampleCount = 1; ws.width = WORLD_W; ws.height = WORLD_W; ws.faceCount = 1;
        ws.arraySize = VIEWS; ws.mipCount = 1;
        ws.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ws.format = colorFmt;
        const XrResult wr = xrCreateSwapchain_(app.session, &ws, &app.worldSc);
        uint32_t wn = 0;
        if (XR_SUCCEEDED(wr))
        {
            xrEnumImages_(app.worldSc, 0, &wn, nullptr);
            app.wimgs.assign(wn, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
            xrEnumImages_(app.worldSc, wn, &wn, (XrSwapchainImageBaseHeader*)app.wimgs.data());
        }
        bool worldOk = XR_SUCCEEDED(wr) && wn > 0 && app.wimgs[0].texture;
        if (worldOk)
        {
            D3D12_RESOURCE_DESC wd = app.wimgs[0].texture->GetDesc();
            UINT64 total = 0;
            app.device->GetCopyableFootprints(&wd, 0, VIEWS, 0, app.worldFp, nullptr, nullptr, &total);
            for (int i = 0; i < RING && worldOk; i++)
                worldOk = MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_GENERIC_READ, app.worldUp[i], (void**)&app.worldMapped[i]);
        }
        if (!worldOk)
        {
            Log("InitXrSession: world colour layer unavailable (%s) - the world stays black", XRStr(wr));
            if (app.worldSc) { xrDestroySwapchain_(app.worldSc); app.worldSc = XR_NULL_HANDLE; }
        }
        else Log("InitXrSession: world colour layer %dx%d, %u images", WORLD_W, WORLD_W, wn);
    }

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

// The curved screen's eye swapchain and buffers, made the first time a curve is
// asked for (most players never use it). On failure the screen stays flat.
static bool EnsureCurvedEyes(App& app)
{
    if (app.eyeSc != XR_NULL_HANDLE && app.eyeOut) return true;
    if (app.eyeFailed) return false;
    Log("EnsureCurvedEyes: enter (%dx%d per eye)", app.eyeW, app.eyeH);
    app.eyeFailed = true;                       // until everything below succeeds
    if (app.eyeW <= 0 || app.eyeH <= 0 || app.session == XR_NULL_HANDLE || !app.colorFormat)
    { Log("EnsureCurvedEyes: FAIL no eye size or session - the screen stays flat"); return false; }

    XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sc.sampleCount = 1; sc.width = (uint32_t)app.eyeW; sc.height = (uint32_t)app.eyeH; sc.faceCount = 1;
    sc.arraySize = VIEWS; sc.mipCount = 1;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = app.colorFormat;
    const XrResult r = xrCreateSwapchain_(app.session, &sc, &app.eyeSc);
    if (XR_FAILED(r)) { app.eyeSc = XR_NULL_HANDLE; Log("EnsureCurvedEyes: FAIL swapchain %s - the screen stays flat", XRStr(r)); return false; }
    uint32_t count = 0;
    xrEnumImages_(app.eyeSc, 0, &count, nullptr);
    app.eimgs.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumImages_(app.eyeSc, count, &count, (XrSwapchainImageBaseHeader*)app.eimgs.data());
    if (count == 0 || !app.eimgs[0].texture) { Log("EnsureCurvedEyes: FAIL no swapchain images - the screen stays flat"); return false; }

    if (!MakeTexture(app.device.Get(), app.eyeW, app.eyeH, DXGI_FORMAT_R8G8B8A8_TYPELESS, VIEWS,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, app.eyeOut))
    { Log("EnsureCurvedEyes: FAIL eye buffers - the screen stays flat"); return false; }
    MakeEyeUav(app, app.eyeOut.Get(), DESC_CURVE_MAIN + 2);
    app.eyeFailed = false;
    Log("EnsureCurvedEyes: exit ok (%u swapchain images, %.1f MB of eye buffers)", count,
        (double)app.eyeW * app.eyeH * 4 * VIEWS / (1024.0 * 1024.0));
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

// Steady/fuse building blocks on this GPU (--selftest only; a GPU without a motion
// estimator skips that part, as playback does):
//  1. grid shader vs the CPU: an exact W x H source must come back unchanged;
//  2. hardware motion estimation: a textured frame shifted by (6, 3) px must give
//     (-24, -12) quarter pels on nearly every interior block, and the per-pixel
//     motion must then move the reference onto the current frame;
//  3. steadying with that verified motion blends towards the moved previous map.
static bool SelfTestSteadyFuse(App& app, const std::vector<unsigned char>& scene)
{
    Log("SelfTestSteadyFuse: enter");
    if (scene.size() != (size_t)W * H * 3) { Log("SelfTestSteadyFuse: FAIL bad scene"); return false; }

    if (!UploadTestSource(app, scene)) { Log("SelfTestSteadyFuse: FAIL upload"); return false; }
    const UINT64 done = SubmitGridDesc(app, DESC_TEST_SRC, W, H, nullptr, 0);
    if (!done) { Log("SelfTestSteadyFuse: FAIL grid submit"); return false; }
    WaitMlFence(app, done);
    size_t gridBad = 0;
    for (size_t i = 0; i < (size_t)W * H; i++)
    {
        const uint32_t p = app.gridMapped[i];
        if ((p & 255) != scene[i * 3] || ((p >> 8) & 255) != scene[i * 3 + 1] || ((p >> 16) & 255) != scene[i * 3 + 2]) gridBad++;
    }
    const bool gridOk = gridBad == 0;
    Log("SelfTestSteadyFuse: grid image %s (%zu of %d pixels differ)", gridOk ? "PASS" : "FAIL", gridBad, W * H);

    if (!EnsureMotion(app))
    {
        Log("SelfTestSteadyFuse: exit %s (motion estimation unavailable: SKIPPED)", gridOk ? "PASS" : "FAIL");
        return gridOk;
    }
    // Smooth value noise on 4-px cells: texture everywhere for block matching.
    auto pattern = [](int x, int y) -> float
    {
        auto cell = [](int cx, int cy) { unsigned h = (unsigned)(cx * 73856093) ^ (unsigned)(cy * 19349663); h = (h ^ (h >> 13)) * 1274126177u; return (float)((h >> 8) & 255); };
        const int cx = (int)floorf(x / 4.0f), cy = (int)floorf(y / 4.0f);
        const float fx = x / 4.0f - cx, fy = y / 4.0f - cy;
        const float top = cell(cx, cy) * (1 - fx) + cell(cx + 1, cy) * fx, bot = cell(cx, cy + 1) * (1 - fx) + cell(cx + 1, cy + 1) * fx;
        return std::round(top * (1 - fy) + bot * fy);
    };
    const int dx = 6, dy = 3;
    fusion::Image ref(W, H), cur(W, H), refNear(W, H), curNear(W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            ref.at(x, y) = pattern(x, y);
            cur.at(x, y) = pattern(x - dx, y - dy);
            refNear.at(x, y) = (float)x / (W - 1);
            curNear.at(x, y) = 0.2f;              // far from the moved ramp, so the blend is measurable
        }
    std::vector<int16_t> vec;
    if (!app.motion.Upload(0, cur.v.data()) || !app.motion.Upload(1, ref.v.data()) || !app.motion.Estimate(0, 1, vec))
    { Log("SelfTestSteadyFuse: FAIL motion estimate"); return false; }
    const int bw = app.motion.blocksW(), bh = app.motion.blocksH();
    int right = 0, n = 0;
    for (int by = 1; by + 1 < bh; by++)
        for (int bx = 1; bx + 1 < bw; bx++, n++)
            if (vec[((size_t)by * bw + bx) * 2] == -4 * dx && vec[((size_t)by * bw + bx) * 2 + 1] == -4 * dy) right++;
    const bool motionOk = right >= n * 95 / 100;
    Log("SelfTestSteadyFuse: motion %s (%d of %d interior blocks exactly (%d, %d) quarter pels)", motionOk ? "PASS" : "FAIL", right, n, -4 * dx, -4 * dy);

    const fusion::Motion m = fusion::MotionFromVectors(vec.data(), bw, bh, W, H);
    float trustMean = 0;
    const fusion::Image trust = fusion::MotionTrust(cur, ref, m, &trustMean);
    const fusion::Image moved = fusion::Remap(refNear, m);
    // Interior pixel: its reference position is (x - dx), so the moved ramp there is (x - dx)/(W-1).
    const int px = W / 2, py = H / 2;
    const float movedWant = (float)(px - dx) / (W - 1);
    const fusion::Image steadied = fusion::Steady(curNear, moved, trust);
    const float steadyWant = 0.2f + fusion::STEADY_ALPHA * trust.at(px, py) * (movedWant - 0.2f);
    const bool mapOk = std::fabs(moved.at(px, py) - movedWant) < 1e-3f && trustMean > 0.9f &&
                       std::fabs(steadied.at(px, py) - steadyWant) < 1e-4f && std::fabs(steadied.at(px, py) - 0.2f) > 0.05f;
    Log("SelfTestSteadyFuse: steady %s (trust mean %.3f, moved %.4f want %.4f, steadied %.4f want %.4f)", mapOk ? "PASS" : "FAIL",
        trustMean, moved.at(px, py), movedWant, steadied.at(px, py), steadyWant);
    const bool ok = gridOk && motionOk && mapOk;
    Log("SelfTestSteadyFuse: exit %s", ok ? "PASS" : "FAIL");
    return ok;
}

// --selftest with --steady and/or --fuse: real depth passes on the live source for a
// few seconds with the options on (no VR session), then checks that the output stays
// finite and in range, that steadying/fusion ran, and that the anchor model produced
// results. Exercises the grid pass, motion estimator, anchor thread and CPU maths.
static bool FirstSourceFrame(App& app);

static bool SelfTestPipeline(App& app)
{
    if (!app.steadyEnabled.load() && !app.fuseEnabled.load()) return true;
    Log("SelfTestPipeline: enter (steady %d, fuse %d)", (int)app.steadyEnabled.load(), (int)app.fuseEnabled.load());
    if (!FirstSourceFrame(app)) { Log("SelfTestPipeline: FAIL no source frame"); return false; }

    std::vector<float> nearMap;
    unsigned passes = 0, bad = 0, steadied = 0, fused = 0;
    std::vector<double> postMs;
    const double end = NowSeconds() + 6.0;
    while (NowSeconds() < end)
    {
        SourceRef src = app.sources.Latest();
        if (!src) { Sleep(5); continue; }
        if (!ComputeAndPublish(app, src, nearMap)) { Log("SelfTestPipeline: FAIL depth pass"); return false; }
        passes++;
        for (float v : nearMap) if (!std::isfinite(v) || v < 0 || v > 1) { bad++; break; }
        steadied += app.postSteadied; fused += app.postFused;
        app.postSteadied = app.postFused = 0;
        postMs.insert(postMs.end(), app.postMs.begin(), app.postMs.end());
        app.postMs.clear();
        Sleep(8);
    }
    std::sort(postMs.begin(), postMs.end());
    const unsigned anchors = app.anchorPasses.exchange(0);
    Log("SelfTestPipeline: mean ms per pass - grid+luma %.2f, luma upload %.2f, anchor hand-over %.2f, motion estimates %.2f, fuse %.2f, steady %.2f",
        app.postStageMs[0] / passes, app.postStageMs[1] / passes, app.postStageMs[2] / passes, app.postStageMs[3] / passes,
        app.postStageMs[4] / passes, app.postStageMs[5] / passes);
    const bool fuseOk = !FuseActive(app) || (anchors > 0 && fused > 0);
    const bool ok = passes > 20 && bad == 0 && !postMs.empty() && fuseOk;
    Log("SelfTestPipeline: exit %s - %u passes, %u out of range, post ms p50 %.1f p95 %.1f, steadied %u, fused %u, anchor passes %u%s",
        ok ? "PASS" : "FAIL", passes, bad, postMs.empty() ? 0.0 : postMs[postMs.size() / 2],
        postMs.empty() ? 0.0 : postMs[std::min(postMs.size() - 1, postMs.size() * 95 / 100)], steadied, fused, anchors,
        app.motion.ok() ? "" : " (no motion estimator)");
    return ok;
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
    int worstColor = 0;
    for (UINT e = 0; e < VIEWS; e++)
    {
        WarpEyeFill(scene, near01, eyes[e], c.scaleFocal, 1.0f, c.invZNear, c.invZFar, c.nearZ, c.farZ, c.doWarp != 0, (int)c.fillMode,
                    refColor.data(), refDepth.data(), c.subpixel != 0);
        for (int y = 0; y < H; y++)
        {
            const unsigned char* grow = cp + cfp[e].Offset + (size_t)y * cfp[e].Footprint.RowPitch;
            const float* gdrow = (const float*)((const unsigned char*)dp + dfp[e].Offset + (size_t)y * dfp[e].Footprint.RowPitch);
            const unsigned char* rrow = refColor.data() + (size_t)y * ROW_PITCH;
            const float* rdrow = refDepth.data() + (size_t)y * (ROW_PITCH / 4);
            for (int x = 0; x < W; x++)
            {
                // Whole-pixel warp copies a source pixel, so it must match exactly.
                // Sub-pixel blends two of them, and the GPU's UNORM store rounds ties
                // to even where lroundf rounds away from zero: allow one last bit,
                // and report the worst difference so a real error still shows.
                if (c.subpixel == 0)
                {
                    if (memcmp(grow + x * 4, rrow + x * 4, 4) != 0) badColor++;
                }
                else
                {
                    int worst = 0;
                    for (int ch = 0; ch < 4; ch++) worst = std::max(worst, std::abs((int)grow[x * 4 + ch] - (int)rrow[x * 4 + ch]));
                    worstColor = std::max(worstColor, worst);
                    if (worst > 1) badColor++;
                }
                if (c.writeDepth && fabsf(gdrow[x] - rdrow[x]) > 1e-5f) badDepth++;
            }
        }
    }
    crb->Unmap(0, nullptr);
    drb->Unmap(0, nullptr);

    const size_t total = (size_t)W * H * VIEWS;
    const size_t tolerance = total / 2000;          // 0.05 % - rounding ties only
    bool ok = badColor <= tolerance && badDepth <= tolerance;
    Log("SelfTestWarp[%s]: exit %s - mismatches colour %zu depth %zu of %zu px (tolerance %zu)%s", name, ok ? "PASS" : "FAIL",
        badColor, badDepth, total, tolerance, c.subpixel ? (worstColor <= 1 ? " [sub-pixel: worst 1 bit]" : " [sub-pixel: worst > 1 bit]") : "");
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

// Frame-transfer path end to end: a known BGRA frame on the headset GPU goes
// through the copy engine, the cross-adapter buffer and the unpack shader; the model
// input on the inference GPU must match a CPU reference of the same resample and
// normalisation.
static bool SelfTestFrameTransfer(App& app)
{
    if (!app.frameTransfer) return true;
    const ModelSpec& spec = *app.opt.model;
    Log("SelfTestFrameTransfer: enter (%dx%d frame -> %dx%d %s input on %ls)", app.xferW, app.xferH, spec.inW, spec.inH,
        spec.name, app.inferName.c_str());

    ComPtr<ID3D12Resource> frame;
    if (!MakeSourceTexture(app, app.xferW, app.xferH, DXGI_FORMAT_B8G8R8A8_UNORM, false, frame)) return false;
    D3D12_RESOURCE_DESC fd = frame->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    app.device->GetCopyableFootprints(&fd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> up;
    unsigned char* mapped = nullptr;
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, up, (void**)&mapped)) return false;
    std::vector<uint32_t> pixels((size_t)app.xferW * app.xferH);
    uint32_t seed = 2024;
    for (auto& p : pixels) { seed = seed * 1664525u + 1013904223u; p = 0xFF000000u | (seed >> 8); }
    for (int y = 0; y < app.xferH; y++)
        memcpy(mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch, pixels.data() + (size_t)y * app.xferW, (size_t)app.xferW * 4);

    WaitFence(app, app.fenceVal);
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = frame.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);   // simultaneous access: back to COMMON afterwards
    app.cmdList->Close();
    WaitFence(app, SubmitAndSignal(app));

    const double t0 = NowSeconds();
    SubmitFrameTransfer(app, frame.Get(), nullptr, 0);
    WaitDepthIdle(app);
    const double sendMs = (NowSeconds() - t0) * 1000;

    const UINT64 bytes = (UINT64)3 * spec.inW * spec.inH * sizeof(float);
    ComPtr<ID3D12Resource> rb;
    if (!MakeBuffer(app.inferDevice.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb, nullptr)) return false;
    app.inferCmdAlloc->Reset();
    app.inferCmdList->Reset(app.inferCmdAlloc.Get(), nullptr);
    ID3D12GraphicsCommandList* il = app.inferCmdList.Get();
    Transition(il, app.modelIn.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    il->CopyBufferRegion(rb.Get(), 0, app.modelIn.Get(), 0, bytes);
    Transition(il, app.modelIn.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    il->Close();
    ID3D12CommandList* lists[] = { il };
    app.inferQueue->ExecuteCommandLists(1, lists);
    app.inferQueue->Signal(app.inferFence.Get(), ++app.inferFenceVal);
    WaitInferFence(app, app.inferFenceVal);

    const float* got = nullptr;
    if (FAILED(rb->Map(0, nullptr, (void**)&got)) || !got) { Log("SelfTestFrameTransfer: FAIL map readback"); return false; }
    const float mean[3] = { 0.485f, 0.456f, 0.406f };
    const float istd[3] = { 1.f / 0.229f, 1.f / 0.224f, 1.f / 0.225f };
    std::vector<float> channel(pixels.size());
    size_t bad = 0;
    float worst = 0;
    for (int c = 0; c < 3; c++)
    {
        const int shift = c == 0 ? 16 : c == 1 ? 8 : 0;                  // R, G, B from BGRA
        for (size_t i = 0; i < pixels.size(); i++) channel[i] = float((pixels[i] >> shift) & 255) / 255.f;
        for (int y = 0; y < spec.inH; y++)
            for (int x = 0; x < spec.inW; x++)
            {
                const float v = (spec.inW == app.xferW && spec.inH == app.xferH) ? channel[(size_t)y * app.xferW + x]
                    : SampleDepth(channel, app.xferW, app.xferH, (x + .5f) / spec.inW, (y + .5f) / spec.inH);
                const float ref = spec.imagenetNorm ? (v - mean[c]) * istd[c] : v;
                const float d = fabsf(got[((size_t)c * spec.inH + y) * spec.inW + x] - ref);
                worst = std::max(worst, d);
                if (d > 2e-4f) bad++;
            }
    }
    rb->Unmap(0, nullptr);
    const bool ok = bad == 0;
    Log("SelfTestFrameTransfer: exit %s - %zu of %zu input values differ from the CPU reference (worst %.2e; send + unpack %.2f ms, first run)",
        ok ? "PASS" : "FAIL", bad, (size_t)3 * spec.inW * spec.inH, worst, sendMs);
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

// GPU glow vs the CPU reference in ambilight.h, pixel for pixel. The taps are
// whole source pixels, so only the last bit of the store may differ.
static bool SelfTestAmbilight(App& app, const std::vector<unsigned char>& scene)
{
    if (!app.ambiTex || app.ambiW <= 0 || app.ambiH <= 0) { Log("SelfTestAmbilight: FAIL no glow texture"); return false; }
    if (scene.size() != (size_t)W * H * 3) { Log("SelfTestAmbilight: FAIL bad scene"); return false; }
    Log("SelfTestAmbilight: enter (glow %dx%d from a %dx%d picture)", app.ambiW, app.ambiH, W, H);

    if (!UploadTestSource(app, scene)) { Log("SelfTestAmbilight: FAIL upload"); return false; }

    const float width = 5.7f, height = width * (float)H / (float)W;
    const AmbiConstants a = MakeAmbiConstants(app, W, H, width, height, kAmbiDefaultStrength / 100.0f, true, true);
    if (!AmbiConstantsUsable(a)) { Log("SelfTestAmbilight: FAIL constants"); return false; }

    ID3D12Device* dev = app.device.Get();
    D3D12_RESOURCE_DESC gd = app.ambiTex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT gfp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&gd, 0, 1, 0, &gfp, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> rb;
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb, nullptr)) return false;

    WaitFence(app, app.fenceVal);
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    RecordAmbilight(app, DESC_TEST_SRC, a, nullptr);
    Transition(app.cmdList.Get(), app.ambiTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.pResource = rb.Get(); dst.PlacedFootprint = gfp;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = app.ambiTex.Get(); src.SubresourceIndex = 0;
    app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(app.cmdList.Get(), app.ambiTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    app.cmdList->Close();
    WaitFence(app, SubmitAndSignal(app));
    app.ambiHistory = false;            // playback starts the glow from nothing

    std::vector<unsigned char> rgba((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)W * H; i++)
    {
        rgba[i * 4 + 0] = scene[i * 3 + 0];
        rgba[i * 4 + 1] = scene[i * 3 + 1];
        rgba[i * 4 + 2] = scene[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    std::vector<unsigned char> ref((size_t)app.ambiW * app.ambiH * 4);
    if (!AmbilightReference(a, rgba.data(), W * 4, ref.data(), app.ambiW * 4)) { Log("SelfTestAmbilight: FAIL reference rejected the constants"); return false; }

    const unsigned char* got = nullptr;
    if (FAILED(rb->Map(0, nullptr, (void**)&got)) || !got) { Log("SelfTestAmbilight: FAIL map readback"); return false; }
    size_t bad = 0, lit = 0;
    int worst = 0;
    for (int y = 0; y < app.ambiH; y++)
        for (int x = 0; x < app.ambiW; x++)
        {
            const unsigned char* g = got + gfp.Offset + (size_t)y * gfp.Footprint.RowPitch + (size_t)x * 4;
            const unsigned char* r = ref.data() + ((size_t)y * app.ambiW + x) * 4;
            if (r[3] > 0) lit++;
            int diff = 0;
            for (int ch = 0; ch < 4; ch++) diff = std::max(diff, std::abs((int)g[ch] - (int)r[ch]));
            worst = std::max(worst, diff);
            if (diff > 1) bad++;
        }
    rb->Unmap(0, nullptr);

    const size_t pixels = (size_t)app.ambiW * app.ambiH;
    const bool ok = bad == 0 && lit > pixels / 10;
    Log("SelfTestAmbilight: exit %s - %zu of %zu glow pixels differ by more than one bit (worst %d), %zu lit",
        ok ? "PASS" : "FAIL", bad, pixels, worst, lit);
    return ok;
}

// Reads back a texture array (all slices) into tightly packed rows of `texelBytes`.
static bool ReadbackRgba(App& app, ID3D12Resource* tex, D3D12_RESOURCE_STATES state, std::vector<std::vector<unsigned char>>& out,
                         UINT texelBytes = 4)
{
    if (!tex || texelBytes == 0) return false;
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    const UINT slices = d.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(slices);
    UINT64 total = 0;
    app.device->GetCopyableFootprints(&d, 0, slices, 0, fp.data(), nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> rb;
    if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb, nullptr)) return false;
    WaitFence(app, app.fenceVal);
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) Transition(app.cmdList.Get(), tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i = 0; i < slices; i++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.pResource = rb.Get(); dst.PlacedFootprint = fp[i];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.pResource = tex; src.SubresourceIndex = i;
        app.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) Transition(app.cmdList.Get(), tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    app.cmdList->Close();
    WaitFence(app, SubmitAndSignal(app));
    const unsigned char* mapped = nullptr;
    if (FAILED(rb->Map(0, nullptr, (void**)&mapped)) || !mapped) return false;
    out.assign(slices, {});
    for (UINT i = 0; i < slices; i++)
    {
        out[i].resize((size_t)d.Width * d.Height * texelBytes);
        for (UINT y = 0; y < d.Height; y++)
            memcpy(out[i].data() + (size_t)y * d.Width * texelBytes, mapped + fp[i].Offset + (size_t)y * fp[i].Footprint.RowPitch,
                   (size_t)d.Width * texelBytes);
    }
    rb->Unmap(0, nullptr);
    return true;
}

// The curved screen, GPU vs the CPU reference (CurvedPixel): the test target's
// last warped pair on a fully curved cylinder with the glow behind it, seen by two
// eyes, one of them turned slightly, so that position, rotation, outline, glow and
// sampling are all exercised. The GPU filters with 8-bit weights, so a colour may
// differ by a couple of bits; a ray exactly on the outline may land either side.
static bool SelfTestCurve(App& app)
{
    if (!app.curvePso || !app.testEyeOut) { Log("SelfTestCurve: FAIL pass not built"); return false; }
    Log("SelfTestCurve: enter (%dx%d eyes)", TEST_EYE_W, TEST_EYE_H);

    const float width = 5.7f, height = width * (float)H / (float)W, distance = ScreenAnchor::distance;
    Cylinder cyl;
    if (!BuildCylinder(width, height, distance, 1.0f, cyl) || !cyl.curved) { Log("SelfTestCurve: FAIL cylinder"); return false; }
    const float marginM = kAmbiMargin * width;
    XrPosef screenPose{ { 0, 0, 0, 1 }, { 0.1f, 1.5f, -distance } };
    XrPosef eyes[VIEWS] = { { { 0, 0, 0, 1 }, { 0.1f - 0.032f, 1.55f, 0 } }, { { 0, 0.0262f, 0, 0.99966f }, { 0.1f + 0.032f, 1.55f, 0 } } };
    XrFovf fov[VIEWS] = { { -0.96f, 0.96f, 0.72f, -0.72f }, { -0.96f, 0.96f, 0.72f, -0.72f } };
    const CurveConstants c = MakeCurveConstants(cyl, screenPose, eyes, fov, TEST_EYE_W, TEST_EYE_H, true,
                                                width + 2 * marginM, height + 2 * marginM, 0x2A3441, true);

    WaitFence(app, app.fenceVal);
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    WarpTarget& t = app.testTarget;
    Transition(app.cmdList.Get(), t.colorOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    RecordCurvedScreen(app, DESC_CURVE_TEST, t.colorOut.Get(), c, app.testEyeOut.Get(), nullptr);
    Transition(app.cmdList.Get(), t.colorOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    app.cmdList->Close();
    WaitFence(app, SubmitAndSignal(app));

    std::vector<std::vector<unsigned char>> got, pictures, glowPx;
    if (!ReadbackRgba(app, app.testEyeOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, got) || got.size() != VIEWS ||
        !ReadbackRgba(app, t.colorOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pictures) || pictures.size() != VIEWS ||
        !ReadbackRgba(app, app.ambiTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, glowPx) || glowPx.size() != 1)
    { Log("SelfTestCurve: FAIL readback"); return false; }

    RgbaImage glowImg{ glowPx[0].data(), app.ambiW, app.ambiH, app.ambiW * 4 };
    size_t bad = 0, onScreen = 0, onGlow = 0;
    int worst = 0;
    for (uint32_t e = 0; e < VIEWS; e++)
    {
        RgbaImage pic{ pictures[e].data(), W, H, W * 4 };
        for (int y = 0; y < TEST_EYE_H; y++)
            for (int x = 0; x < TEST_EYE_W; x++)
            {
                float ref[3];
                if (!CurvedPixel(c, (int)e, x, y, cyl, pic, &glowImg, ref)) { Log("SelfTestCurve: FAIL reference at %d,%d", x, y); return false; }
                float o[3], d[3], tu, tv;
                CurveRay(c, (int)e, x + 0.5f, y + 0.5f, o, d);
                if (CylinderHit(cyl, o, d, &tu, &tv)) onScreen++;
                else if (GlowHit(c, o, d, &tu, &tv)) onGlow++;
                const unsigned char* g = got[e].data() + ((size_t)y * TEST_EYE_W + x) * 4;
                int diff = std::abs((int)g[3] - 255);
                for (int ch = 0; ch < 3; ch++)
                    diff = std::max(diff, std::abs((int)g[ch] - (int)lroundf(std::min(std::max(ref[ch], 0.0f), 1.0f) * 255.0f)));
                worst = std::max(worst, diff);
                if (diff > 2) bad++;
            }
    }
    if (app.opt.doDump)
    {
        // Both eyes side by side (left, right), to look at the shape.
        FILE* f = nullptr;
        if (!fopen_s(&f, "curve-eyes.ppm", "wb") && f)
        {
            fprintf(f, "P6\n%d %d\n255\n", TEST_EYE_W * 2, TEST_EYE_H);
            for (int y = 0; y < TEST_EYE_H; y++)
                for (uint32_t e = 0; e < VIEWS; e++)
                    for (int x = 0; x < TEST_EYE_W; x++) fwrite(got[e].data() + ((size_t)y * TEST_EYE_W + x) * 4, 1, 3, f);
            fclose(f);
            Log("SelfTestCurve: wrote curve-eyes.ppm");
        }
    }
    const size_t total = (size_t)TEST_EYE_W * TEST_EYE_H * VIEWS;
    const bool framed = onScreen > total / 4 && onGlow > total / 50;
    const bool ok = bad <= total / 200 && framed;
    Log("SelfTestCurve: exit %s - %zu of %zu px differ by more than 2 bits (worst %d; allowed %zu on the outline), %zu on the screen, %zu on the glow",
        ok ? "PASS" : "FAIL", bad, total, worst, total / 200, onScreen, onGlow);
    return ok;
}

// A head turned yawDeg to the right, then pitched pitchDeg up (q = yaw * pitch).
static XrQuaternionf YawPitch(float yawDeg, float pitchDeg)
{
    const float yaw = -yawDeg * kRoomPi / 180.0f, pitch = pitchDeg * kRoomPi / 180.0f;
    const float sy = std::sin(0.5f * yaw), cy = std::cos(0.5f * yaw), sp = std::sin(0.5f * pitch), cp = std::cos(0.5f * pitch);
    return XrQuaternionf{ cy * sp, sy * cp, -sy * sp, cy * cp };
}

// How far the GPU's eye buffers are from a CPU reference, per pixel: `reference`
// fills rgb (encoded, 0..1) for eye e at (x, y) and returns false on a failure. With
// `kinds` (the CPU reference's sample kind of every pixel, eye-major, w x h per eye;
// kEyeKindMixed where its four rays disagree), the pixels at an edge - mixed, or next to
// a pixel of another kind, where a GPU and a CPU ray can land either side - are also
// counted on their own. Away from any edge, only rounding separates them: a few levels
// at most, so a pixel more than kEyeFarOff levels off there is drawn wrong.
static const unsigned char kEyeKindMixed = 255;
static const int kEyeFarOff = 8;

struct EyeCompare
{
    size_t checked = 0, bad = 0;        // bad: a channel more than 2 bits off
    int worst = 0;
    size_t edgeChecked = 0, edgeBad = 0; // of those, the pixels at an edge (with `kinds`)
    size_t interiorFar = 0;             // away from any edge and more than kEyeFarOff levels off
    int interiorWorst = 0;              // the worst away from any edge
    size_t InteriorChecked() const { return checked - edgeChecked; }
    size_t InteriorBad() const { return bad - edgeBad; }
};

template <typename Reference>
static bool CompareEyes(const char* what, const std::vector<std::vector<unsigned char>>& got, int bufferW, int w, int h, int step, Reference reference,
                        const std::vector<unsigned char>* kinds, EyeCompare& out)
{
    if (!what) { Log("CompareEyes: FAIL no name"); return false; }
    if (got.size() != VIEWS || w <= 0 || h <= 0 || bufferW < w || step <= 0)
    { Log("CompareEyes: FAIL %s - %zu eyes, buffer %d wide, %dx%d, step %d", what, got.size(), bufferW, w, h, step); return false; }
    if (kinds && kinds->size() != (size_t)w * h * VIEWS) { Log("CompareEyes: FAIL %s - %zu kinds for %dx%d x %u eyes", what, kinds->size(), w, h, VIEWS); return false; }
    Log("CompareEyes: enter (%s, %dx%d x %u eyes, every %d px%s)", what, w, h, VIEWS, step, kinds ? ", edges apart" : "");
    out = EyeCompare();
    for (uint32_t e = 0; e < VIEWS; e++)
    {
        if (got[e].size() < (size_t)bufferW * h * 4) { Log("CompareEyes: FAIL %s - eye %u has %zu bytes", what, e, got[e].size()); return false; }
        const unsigned char* k = kinds ? kinds->data() + (size_t)e * w * h : nullptr;
        for (int y = step / 2; y < h; y += step)
            for (int x = step / 2; x < w; x += step)
            {
                float ref[3];
                if (!reference((int)e, x, y, ref)) { Log("CompareEyes: FAIL %s - no reference for eye %u at %d,%d", what, e, x, y); return false; }
                const unsigned char* gp = got[e].data() + ((size_t)y * bufferW + x) * 4;
                int diff = 0;
                for (int ch = 0; ch < 3; ch++)
                    diff = std::max(diff, std::abs((int)gp[ch] - (int)lroundf(std::min(std::max(ref[ch], 0.0f), 1.0f) * 255.0f)));
                out.worst = std::max(out.worst, diff);
                if (diff > 2) out.bad++;
                out.checked++;

                // At an edge: its own rays disagree, or a neighbour's kind differs.
                const unsigned char own = k ? k[(size_t)y * w + x] : 0;
                bool edge = k && own == kEyeKindMixed;
                for (int ny = std::max(0, y - 1); k && !edge && ny <= std::min(h - 1, y + 1); ny++)
                    for (int nx = std::max(0, x - 1); !edge && nx <= std::min(w - 1, x + 1); nx++)
                        edge = k[(size_t)ny * w + nx] != own;
                if (!edge)
                {
                    out.interiorWorst = std::max(out.interiorWorst, diff);
                    if (diff > kEyeFarOff) out.interiorFar++;
                    continue;
                }
                out.edgeChecked++;
                if (diff > 2) out.edgeBad++;
            }
    }
    Log("CompareEyes: exit - %s: %zu of %zu px more than 2 bits off (worst %d)%s", what, out.bad, out.checked, out.worst,
        kinds ? "; edges counted apart" : "");
    return true;
}

// The room, GPU vs room.h, for a flat and a fully curved screen, each with the room light
// off ("plain") and on ("light"): every emitter's radiance (to 1e-4; the room light's is
// the constants' exactly), every lightmap texel (half-float precision), and the eye pass
// seen by two eyes, compared with RoomPixel fed the GPU's own lightmap, glow and pictures.
// - plain: Room 40. One eye looks up at the screen, one is turned to a side wall and the
//   floor. The kept v10 eye pass (curveRoomV10Pso) is compared with room_v10::RoomPixel
//   likewise.
// - light: Room 40 with the room light at 50%, #FFB46B. One eye is turned round and looks
//   up at the panel; the other is turned left, to the left wall, the floor the light falls
//   on and (curved) the front's left wing. The floor under the panel must be lit by it.
// The GPU's mirror picture (RGBA16F, UAV at rest) as floats: its first h rows, w wide.
static bool ReadbackMirror(App& app, int w, int h, RoomMirror& out)
{
    Log("ReadbackMirror: enter (%d x %d)", w, h);
    if (!app.roomMirror || w <= 0 || h <= 0 || w > kRoomMirrorW || h > kRoomMirrorMaxH) { Log("ReadbackMirror: FAIL no mirror or a bad size"); return false; }
    std::vector<std::vector<unsigned char>> bytes;
    if (!ReadbackRgba(app, app.roomMirror.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, bytes, 8) || bytes.size() != 1 ||
        bytes[0].size() != (size_t)kRoomMirrorW * kRoomMirrorMaxH * 8)
    { Log("ReadbackMirror: FAIL readback"); return false; }
    out.w = w; out.h = h;
    out.texels.resize((size_t)w * h * 4);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
        {
            const uint16_t* hx = (const uint16_t*)(bytes[0].data() + ((size_t)j * kRoomMirrorW + i) * 8);
            for (int ch = 0; ch < 4; ch++) out.texels[((size_t)j * w + i) * 4 + ch] = RoomHalfToFloat(hx[ch]);
        }
    Log("ReadbackMirror: exit ok");
    return true;
}

static bool SelfTestRoom(App& app, const std::vector<unsigned char>& scene)
{
    if (!app.roomEmitPso || !app.roomMirrorPso || !app.roomMirror || !app.curveRoomPso || !app.curveRoomLookPso || !app.curveRoomV10Pso ||
        !app.testEyeOut || !app.roomCbMapped)
    { Log("SelfTestRoom: FAIL not built"); return false; }
    if (scene.size() != (size_t)W * H * 3) { Log("SelfTestRoom: FAIL bad scene"); return false; }
    Log("SelfTestRoom: enter");

    std::vector<std::vector<unsigned char>> glowPx, pictures;
    if (!ReadbackRgba(app, app.ambiTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, glowPx) || glowPx.size() != 1 ||
        !ReadbackRgba(app, app.testTarget.colorOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pictures) || pictures.size() != VIEWS)
    { Log("SelfTestRoom: FAIL readback of the glow and pictures"); return false; }
    std::vector<unsigned char> rgba((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)W * H; i++)
    {
        rgba[i * 4 + 0] = scene[i * 3 + 0]; rgba[i * 4 + 1] = scene[i * 3 + 1]; rgba[i * 4 + 2] = scene[i * 3 + 2]; rgba[i * 4 + 3] = 255;
    }
    float table[256];
    RoomDecodeTable(table);
    const float width = 5.7f, height = width * (float)H / (float)W;
    const uint32_t world = 0x2A3441;
    const int roomPercent = 40, lightPercent = 50, glassPercent = 60, reflectPercent = 40;
    bool ok = true;

    // Cases {flat, curved} x {plain, light, look}: plain has the v11 controls at 0; light the
    // room light alone; look Glass 60, Reflections 40 and the room light (the finish: frames,
    // tiles, glass, the reflections and the mirror picture).
    for (int ci = 0; ci < 6; ci++)
    {
        const bool curvedCase = ci >= 3, lightCase = ci % 3 == 1, lookCase = ci % 3 == 2;
        char name[24];
        snprintf(name, sizeof(name), "%s%s", curvedCase ? "curved" : "flat", lightCase ? "-light" : (lookCase ? "-look" : ""));
        Cylinder cyl;
        if (curvedCase && (!BuildCylinder(width, height, ScreenAnchor::distance, 1.0f, cyl) || !cyl.curved)) { Log("SelfTestRoom[%s]: FAIL cylinder", name); return false; }
        RoomInputs in;
        in.W = width; in.H = height; in.eye[1] = 0.05f; in.eye[2] = ScreenAnchor::distance; in.cyl = cyl;
        Room room;
        if (!BuildRoom(in, room)) { Log("SelfTestRoom[%s]: FAIL room", name); return false; }
        const float margin = kAmbiMargin * width;
        // The curved case uses 16-texel glow blocks, the path 4:3 and taller pictures take.
        const RoomEmitterLayout layout = RoomLayout(width, height, app.ambiW, app.ambiH, curvedCase ? 16 : kRoomGlowBlock);
        if (layout.count() == 0) { Log("SelfTestRoom[%s]: SKIPPED - no emitter layout fits a %dx%d glow", name, app.ambiW, app.ambiH); continue; }
        std::vector<RoomEmitter> em;
        if (!BuildRoomEmitters(room, cyl, width, height, 0.5f * width + margin, 0.5f * height + margin, layout, em)) { Log("SelfTestRoom[%s]: FAIL emitters", name); return false; }
        RoomView view;
        view.flatLayer = !curvedCase; view.W = width; view.H = height; view.glowOn = true;
        view.glowHalfW = 0.5f * width + margin; view.glowHalfH = 0.5f * height + margin; view.dither = true; view.cyl = cyl;
        RoomLook look;
        if (lightCase || lookCase) { look.light = lightPercent; look.lightRgb = kRoomLightDefault; }
        if (lookCase) { look.glass = glassPercent; look.reflect = reflectPercent; }
        const RoomShading shading = MakeRoomShading(room, roomPercent, world, width * height, look);
        const RoomConstants rc = MakeRoomConstants(room, shading, layout, view, W, H, 1.0f);
        if ((lightCase || lookCase) != ((rc.flags & kRoomFlagLight) != 0) || !room.lightValid)
        { Log("SelfTestRoom[%s]: FAIL the room light is %s (panel %s)", name, (rc.flags & kRoomFlagLight) ? "on" : "off", room.lightValid ? "fits" : "missing"); return false; }
        if (lookCase != ((rc.flags & kRoomFlagFinish) != 0))
        { Log("SelfTestRoom[%s]: FAIL the finish is %s", name, (rc.flags & kRoomFlagFinish) ? "on" : "off"); return false; }
        if (lookCase != ((rc.flags & kRoomFlagReflect) != 0) || (lookCase && (rc.mirrorW != (uint32_t)kRoomMirrorW || rc.mirrorH == 0)))
        { Log("SelfTestRoom[%s]: FAIL the reflections are %s (mirror %u x %u)", name, (rc.flags & kRoomFlagReflect) ? "on" : "off", rc.mirrorW, rc.mirrorH); return false; }
        if (lookCase)
            Log("SelfTestRoom[%s]: glass %d%%, reflections %d%% (mean coat reflectance %.4f, mirror picture %u x %u) - side walls %d bays of %.3f m, back "
                "wall %d of %.3f m, crossbar at %.3f m; bounce albedo %.4f", name, glassPercent, reflectPercent, shading.fbar, rc.mirrorW, rc.mirrorH,
                (int)std::lround((room.zB - room.zSide) / room.pitchSide), room.pitchSide, (int)std::lround(2.0f * room.X / room.pitchBack), room.pitchBack,
                room.transomY, shading.rhoBar);
        Log("SelfTestRoom[%s]: room light %d%% #%06X - panel x %.2f..%.2f, z %.2f..%.2f, emitted %.3f %.3f %.3f, seen %.3f %.3f %.3f", name, look.light,
            (unsigned)look.lightRgb, room.lightX0, room.lightX1, room.lightZ0, room.lightZ1, rc.lightL[0], rc.lightL[1], rc.lightL[2], rc.lightSeen[0],
            rc.lightSeen[1], rc.lightSeen[2]);
        memcpy(app.roomCbMapped + (size_t)RING * sizeof(RoomConstants), &rc, sizeof(rc));
        memcpy(app.roomGeomMapped[RING], em.data(), em.size() * sizeof(RoomEmitter));
        const D3D12_GPU_VIRTUAL_ADDRESS cb = app.roomCbUp->GetGPUVirtualAddress() + (UINT64)RING * sizeof(RoomConstants);

        // --- the light
        WaitFence(app, app.fenceVal);
        app.cmdAlloc[0]->Reset();
        app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
        RecordRoomLight(app, DESC_TEST_SRC, cb, app.roomGeomUp[RING].Get(), (UINT)layout.count(), rc.mirrorH);
        app.cmdList->Close();
        WaitFence(app, SubmitAndSignal(app));

        const UINT64 emBytes = (UINT64)layout.count() * sizeof(RoomEmitter);
        ComPtr<ID3D12Resource> rb;
        if (!MakeBuffer(app.device.Get(), D3D12_HEAP_TYPE_READBACK, emBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb, nullptr)) return false;
        app.cmdAlloc[0]->Reset();
        app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
        Transition(app.cmdList.Get(), app.roomEmitters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        app.cmdList->CopyBufferRegion(rb.Get(), 0, app.roomEmitters.Get(), 0, emBytes);
        Transition(app.cmdList.Get(), app.roomEmitters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        app.cmdList->Close();
        WaitFence(app, SubmitAndSignal(app));
        std::vector<RoomEmitter> gpuEm(em.size());
        {
            const unsigned char* m = nullptr;
            if (FAILED(rb->Map(0, nullptr, (void**)&m)) || !m) { Log("SelfTestRoom[%s]: FAIL map emitters", name); return false; }
            memcpy(gpuEm.data(), m, (size_t)emBytes);
            rb->Unmap(0, nullptr);
        }
        if (!RoomEmitRadiance(layout, rgba.data(), W, H, W * 4, glowPx[0].data(), app.ambiW * 4, true, 1.0f, table, em, rc.lightL))
        { Log("SelfTestRoom[%s]: FAIL CPU emitters", name); return false; }
        const size_t lamp = (size_t)layout.lampIndex();
        size_t badEm = 0, glowLit = 0;
        for (size_t i = 0; i < em.size(); i++)
            for (int ch = 0; ch < 3; ch++)
            {
                if (std::fabs(gpuEm[i].L[ch] - em[i].L[ch]) > 1e-4f * std::fabs(em[i].L[ch]) + 1e-6f) badEm++;
                if (i >= (size_t)layout.gridX * layout.gridY && i < lamp && ch == 0 && em[i].L[0] > 1e-4f) glowLit++;
            }
        // The room light: the last emitter, active (its panel fits) and exactly the constants' radiance.
        bool lampExact = layout.lights == kRoomLights && lamp + 1 == em.size() && gpuEm[lamp].n[3] == 1.0f && em[lamp].n[3] == 1.0f;
        for (int ch = 0; ch < 3; ch++) lampExact = lampExact && gpuEm[lamp].L[ch] == rc.lightL[ch] && em[lamp].L[ch] == rc.lightL[ch];

        std::vector<std::vector<unsigned char>> lmBytes;
        if (!ReadbackRgba(app, app.roomLight.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, lmBytes, 8) || lmBytes.size() != (size_t)kRoomFaces)
        { Log("SelfTestRoom[%s]: FAIL lightmap readback", name); return false; }
        RoomLightmap light;
        light.texels.resize((size_t)kRoomFaces * kRoomLightmap * kRoomLightmap * 4);
        size_t badTexel = 0;
        float worstRel = 0;
        bool facesLit[kRoomFaces] = {};
        for (int f = 0; f < kRoomFaces; f++)
            for (int j = 0; j < kRoomLightmap; j++)
                for (int i = 0; i < kRoomLightmap; i++)
                {
                    const uint16_t* hx = (const uint16_t*)(lmBytes[f].data() + ((size_t)j * kRoomLightmap + i) * 8);
                    float cpu[3];
                    RoomTexel(room, shading, em, f, i, j, cpu);
                    for (int ch = 0; ch < 4; ch++)
                        light.texels[(((size_t)f * kRoomLightmap + j) * kRoomLightmap + i) * 4 + ch] = RoomHalfToFloat(hx[ch]);
                    for (int ch = 0; ch < 3; ch++)
                    {
                        const float g = RoomHalfToFloat(hx[ch]);
                        const float diff = std::fabs(g - cpu[ch]);
                        if (diff > 2e-3f * std::fabs(cpu[ch]) + 1e-5f) badTexel++;
                        if (cpu[ch] > 1e-3f) worstRel = std::max(worstRel, diff / cpu[ch]);
                        // Lit by the screen or glow: above the face's house light alone.
                        const float shade = f == kFaceFloor ? kRoomShadeFloor : (f == kFaceCeiling ? kRoomShadeCeiling : 1.0f);
                        if (cpu[ch] - shade * shading.world[ch] > 1e-5f) facesLit[f] = true;
                    }
                }
        // The front wall of a flat room only gets the bounce (the screen faces away and
        // the glow lies in its own plane); every other face is lit by the screen.
        int lit = 0;
        for (int f = 1; f < kRoomFaces; f++) lit += facesLit[f];

        // The mirror picture (look): the GPU's first mirrorH rows against RoomMirrorPicture,
        // within half-float precision; the eye pass's reference then reads the GPU's own.
        RoomMirror mirror;
        size_t badMirror = 0;
        float worstMirror = 0;
        if (lookCase)
        {
            RoomMirror cpuMirror;
            if (!RoomMirrorPicture(rgba.data(), W, H, W * 4, table, cpuMirror) || cpuMirror.w != (int)rc.mirrorW || cpuMirror.h != (int)rc.mirrorH ||
                !ReadbackMirror(app, (int)rc.mirrorW, (int)rc.mirrorH, mirror))
            { Log("SelfTestRoom[%s]: FAIL mirror picture", name); return false; }
            for (size_t i = 0; i < cpuMirror.texels.size(); i++)
            {
                const float cpu = cpuMirror.texels[i], diff = std::fabs(mirror.texels[i] - cpu);
                if (diff > 2e-3f * std::fabs(cpu) + 1e-5f) badMirror++;
                if (cpu > 1e-3f) worstMirror = std::max(worstMirror, diff / cpu);
            }
            Log("SelfTestRoom[%s]: mirror picture %s - %d x %d of the %dx%d source, %zu of %zu values outside half precision (worst %.2e)", name,
                badMirror == 0 ? "PASS" : "FAIL", mirror.w, mirror.h, W, H, badMirror, cpuMirror.texels.size(), worstMirror);
        }

        // The room light on the floor under its panel's middle: the GPU's texel against the
        // CPU's with the lamp dark, at least 90% of the lamp's direct light (the rest is its
        // share of the bounce), and redder than blue for the warm white.
        bool floorLit = true;
        float floorRise[3] = { 0, 0, 0 }, floorDirect[3] = { 0, 0, 0 };
        if (lightCase)
        {
            const float cx = 0.5f * (room.lightX0 + room.lightX1), cz = 0.5f * (room.lightZ0 + room.lightZ1);
            const int fi = std::min(kRoomLightmap - 1, (int)((cx + room.X) / (2.0f * room.X) * kRoomLightmap));
            const int fj = std::min(kRoomLightmap - 1, (int)((cz + room.g) / (room.zB + room.g) * kRoomLightmap));
            std::vector<RoomEmitter> dark = em;
            dark[lamp].L[0] = dark[lamp].L[1] = dark[lamp].L[2] = 0;
            float without[3], p[3], n[3];
            RoomTexel(room, shading, dark, kFaceFloor, fi, fj, without);
            RoomLightPoint(room, kFaceFloor, fi, fj, p, n);
            const float G = RoomFormFactor(p, n, em[lamp]);
            for (int ch = 0; ch < 3; ch++)
            {
                floorRise[ch] = light.texels[(((size_t)kFaceFloor * kRoomLightmap + fj) * kRoomLightmap + fi) * 4 + ch] - without[ch];
                floorDirect[ch] = shading.rhoFloor / kRoomPi * G * rc.lightL[ch];
                floorLit = floorLit && floorRise[ch] >= 0.9f * floorDirect[ch];
            }
            floorLit = floorLit && floorDirect[0] > 0.01f && floorRise[0] > floorRise[2];
        }

        // --- the eye pass
        // plain: eye 0 looks up 20 degrees (the screen, front wall and ceiling); eye 1 turns
        // 80 degrees right and looks 20 degrees down (a side wall and the floor).
        // light: eye 0 turns round and looks up 60 degrees (the panel, the ceiling and the
        // back wall); eye 1 turns 80 degrees left and looks 20 degrees down (the left wall,
        // the floor and, curved, the front's left wing).
        // look, two dispatches: A - eye 0 looks down 25 degrees (the screen and the tiled
        // floor), eye 1 turns 72 degrees right (the right glass wall's frames, the ground
        // beyond it and the sky); B - eye 0 turns round and looks up 60 degrees (the panel,
        // the ceiling's beams and the back glass), eye 1 turns 100 degrees left and looks 10
        // degrees down (the left glass, the ground's grid and the horizon).
        const XrPosef screenPose{ { 0, 0, 0, 1 }, { 0, 1.5f, -ScreenAnchor::distance } };
        XrFovf fov[VIEWS] = { { -0.88f, 0.88f, 0.79f, -0.79f }, { -0.88f, 0.88f, 0.79f, -0.79f } };
        const size_t total = (size_t)TEST_EYE_W * TEST_EYE_H * VIEWS, onePercent = total / 100, oneEyePercent = onePercent / VIEWS;
        CurveConstants cc;
        std::vector<std::vector<unsigned char>> got;
        size_t bad = 0, kinds[9] = {}, eyeKinds[VIEWS][9] = {}, panelPx = 0, leftWingPx = 0, reflPx[VIEWS] = {};
        int worst = 0;
        bool lookCovered = true;
        RgbaImage glowImg{ glowPx[0].data(), app.ambiW, app.ambiH, app.ambiW * 4 };
        const int dispatches = lookCase ? 2 : 1;
        WarpTarget& t = app.testTarget;
        for (int di = 0; di < dispatches; di++)
        {
            XrPosef eyes[VIEWS] = { { YawPitch(0, 20), { 0, 1.55f, 0 } }, { YawPitch(80, -20), { 0.064f, 1.55f, 0 } } };
            if (lightCase)
            {
                eyes[0] = XrPosef{ YawPitch(180, 60), { 0, 1.55f, 0 } };
                eyes[1] = XrPosef{ YawPitch(-80, -20), { -0.064f, 1.55f, 0 } };
            }
            if (lookCase && di == 0)
            {
                eyes[0] = XrPosef{ YawPitch(0, -25), { 0, 1.55f, 0 } };
                eyes[1] = XrPosef{ YawPitch(72, 0), { 0.064f, 1.55f, 0 } };
            }
            if (lookCase && di == 1)
            {
                eyes[0] = XrPosef{ YawPitch(180, 60), { 0, 1.55f, 0 } };
                eyes[1] = XrPosef{ YawPitch(-100, -10), { -0.064f, 1.55f, 0 } };
            }
            cc = MakeCurveConstants(cyl, screenPose, eyes, fov, TEST_EYE_W, TEST_EYE_H, false, 2.0f * view.glowHalfW, 2.0f * view.glowHalfH, world, true);
            WaitFence(app, app.fenceVal);
            app.cmdAlloc[0]->Reset();
            app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
            Transition(app.cmdList.Get(), t.colorOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ID3D12PipelineState* eyePso = RoomEyePso(app, rc);
            if (eyePso != (lightCase || lookCase ? app.curveRoomLookPso.Get() : app.curveRoomPso.Get())) { Log("SelfTestRoom[%s]: FAIL the wrong eye pass for its constants", name); return false; }
            RecordCurvedScreen(app, DESC_CURVE_TEST, t.colorOut.Get(), cc, app.testEyeOut.Get(), nullptr, cb, UINT_MAX, nullptr, eyePso);
            Transition(app.cmdList.Get(), t.colorOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            app.cmdList->Close();
            WaitFence(app, SubmitAndSignal(app));
            got.clear();
            if (!ReadbackRgba(app, app.testEyeOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, got) || got.size() != VIEWS)
            { Log("SelfTestRoom[%s]: FAIL eye readback", name); return false; }

            // What the finish drew, from the CPU reference: frames (phi > 0.5), tiled floor,
            // the panel (kappa > 0.5), the ground and the sky beyond the glass.
            size_t framePx = 0, tilePx = 0, lookPanelPx = 0, groundPx = 0, skyPx = 0, horizonPx = 0;
            for (uint32_t e = 0; e < VIEWS; e++)
            {
                RgbaImage pic{ pictures[e].data(), W, H, W * 4 };
                RoomEyeInputs inputs;
                inputs.rc = &rc; inputs.picture = &pic; inputs.glow = &glowImg; inputs.light = &light; inputs.mirror = &mirror;
                float Dx[3], Dy[3];
                if (!RoomRayDiff(cc, (int)e, Dx, Dy)) { Log("SelfTestRoom[%s]: FAIL ray differentials", name); return false; }
                for (int y = 0; y < TEST_EYE_H; y++)
                    for (int x = 0; x < TEST_EYE_W; x++)
                    {
                        float ref[3];
                        RoomSurfaceInfo info;
                        if (!RoomPixel(cc, cyl, room, view, (int)e, x, y, inputs, ref, &info)) { Log("SelfTestRoom[%s]: FAIL reference at %d,%d", name, x, y); return false; }
                        framePx += info.phi > 0.5f;
                        tilePx += info.grout >= 0;
                        lookPanelPx += info.kappa > 0.5f;
                        groundPx += info.env == 3;
                        skyPx += info.env == 1;
                        horizonPx += info.env == 2;
                        if (di == 0) reflPx[e] += info.kappaS > 0 && info.fresnel > 0;
                        float o[3], d[3], u, v, gu, gv;
                        CurveRay(cc, (int)e, x + 0.5f, y + 0.5f, o, d);
                        const int kind = RoomClassify(cc, cyl, room, view, o, d, &u, &v, &gu, &gv);
                        kinds[kind]++;
                        eyeKinds[e][kind]++;
                        if (kind == 1 + kFaceCeiling && RoomCeilingPanel(rc, o, d, Dx, Dy) > 0.5f) panelPx++;
                        RoomHit hit;
                        if (kind == 1 + kFaceFront && RoomExit(room, o, d, hit) && hit.part == kRoomPartWingL) leftWingPx++;
                        const unsigned char* gp = got[e].data() + ((size_t)y * TEST_EYE_W + x) * 4;
                        int diff = 0;
                        for (int ch = 0; ch < 3; ch++)
                            diff = std::max(diff, std::abs((int)gp[ch] - (int)lroundf(std::min(std::max(ref[ch], 0.0f), 1.0f) * 255.0f)));
                        worst = std::max(worst, diff);
                        if (diff > 2) bad++;
                    }
            }
            if (lookCase)
            {
                const size_t frameMin = total / 500;
                const bool covered = di == 0 ? framePx > frameMin && tilePx > onePercent && reflPx[0] > oneEyePercent && reflPx[1] > oneEyePercent
                                             : lookPanelPx > onePercent && groundPx > onePercent && skyPx > onePercent;
                lookCovered = lookCovered && covered;
                char reflText[160] = "";
                if (di == 0)
                    snprintf(reflText, sizeof(reflText), "; the screen reflected (kappa_s > 0 and F > 0) in eye 0 %zu px, eye 1 %zu px (needed above %zu each)",
                             reflPx[0], reflPx[1], oneEyePercent);
                Log("SelfTestRoom[%s]: look %c %s - frames %zu px (phi > 0.5; needed above %zu), tiled floor %zu, panel %zu (kappa > 0.5), beyond the glass: "
                    "ground %zu, sky %zu, horizon %zu (needed above %zu each: %s)%s", name, 'A' + di, covered ? "covered" : "FAIL not covered", framePx, frameMin,
                    tilePx, lookPanelPx, groundPx, skyPx, horizonPx, onePercent, di == 0 ? "tiles" : "panel, ground and sky", reflText);
            }
            if (app.opt.doDump)
            {
                char file[64];
                if (lookCase) snprintf(file, sizeof(file), "room-eyes-%s-%c.ppm", name, 'a' + di);
                else snprintf(file, sizeof(file), "room-eyes-%s.ppm", name);
                FILE* f = nullptr;
                if (!fopen_s(&f, file, "wb") && f)
                {
                    fprintf(f, "P6\n%d %d\n255\n", TEST_EYE_W * 2, TEST_EYE_H);
                    for (int y = 0; y < TEST_EYE_H; y++)
                        for (uint32_t e = 0; e < VIEWS; e++)
                            for (int x = 0; x < TEST_EYE_W; x++) fwrite(got[e].data() + ((size_t)y * TEST_EYE_W + x) * 4, 1, 3, f);
                    fclose(f);
                    Log("SelfTestRoom[%s]: wrote %s", name, file);
                }
            }
        }
        bool framed = false;
        if (lookCase) framed = lookCovered;
        else if (!lightCase)
            framed = kinds[curvedCase ? 0 : kRoomKindFootprint] > onePercent && kinds[1 + kFaceFront] > onePercent &&
                     kinds[1 + kFaceFloor] > onePercent && kinds[1 + kFaceCeiling] > onePercent &&
                     kinds[1 + kFaceLeft] + kinds[1 + kFaceRight] > onePercent;
        else
            framed = panelPx > oneEyePercent && eyeKinds[0][1 + kFaceCeiling] > oneEyePercent && eyeKinds[0][1 + kFaceBack] > oneEyePercent &&
                     eyeKinds[1][1 + kFaceLeft] > oneEyePercent && eyeKinds[1][1 + kFaceFloor] > oneEyePercent &&
                     (!curvedCase || leftWingPx > oneEyePercent);

        // With the controls at 0, the look eye pass (its light code behind the flag) is the
        // plain one's reference too: within 2 bits of RoomPixel, like it. The two are
        // different shaders to the GPU's compiler, which may round a few edge rays the
        // other way, so they need not agree to the bit: at most 1 px in 1000 may differ.
        bool lookSame = true;
        if (!lightCase && !lookCase)
        {
            WaitFence(app, app.fenceVal);
            app.cmdAlloc[0]->Reset();
            app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
            Transition(app.cmdList.Get(), t.colorOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            const bool lookRecorded = RecordCurvedScreen(app, DESC_CURVE_TEST, t.colorOut.Get(), cc, app.testEyeOut.Get(), nullptr, cb, UINT_MAX, nullptr,
                                                         app.curveRoomLookPso.Get());
            Transition(app.cmdList.Get(), t.colorOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            app.cmdList->Close();
            WaitFence(app, SubmitAndSignal(app));
            std::vector<std::vector<unsigned char>> gotLook;
            if (!lookRecorded || !ReadbackRgba(app, app.testEyeOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, gotLook) || gotLook.size() != VIEWS)
            { Log("SelfTestRoom[%s]: FAIL look eye pass or its readback", name); return false; }
            size_t lookDiffer = 0;
            int lookWorst = 0;
            for (uint32_t e = 0; e < VIEWS; e++)
                for (size_t i = 0; i < (size_t)TEST_EYE_W * TEST_EYE_H; i++)
                {
                    if (memcmp(gotLook[e].data() + i * 4, got[e].data() + i * 4, 3) == 0) continue;
                    lookDiffer++;
                    for (int ch = 0; ch < 3; ch++) lookWorst = std::max(lookWorst, std::abs((int)gotLook[e][i * 4 + ch] - (int)got[e][i * 4 + ch]));
                }
            EyeCompare lookRef;
            char lookWhat[48];
            snprintf(lookWhat, sizeof(lookWhat), "SelfTestRoom[%s] look eye pass", name);
            RoomEyeInputs inputs;
            inputs.rc = &rc; inputs.glow = &glowImg; inputs.light = &light;
            const bool lookCompared = CompareEyes(lookWhat, gotLook, TEST_EYE_W, TEST_EYE_W, TEST_EYE_H, 1, [&](int e, int x, int y, float ref[3])
            {
                const RgbaImage pic{ pictures[e].data(), W, H, W * 4 };
                RoomEyeInputs eyeInputs = inputs;
                eyeInputs.picture = &pic;
                return RoomPixel(cc, cyl, room, view, e, x, y, eyeInputs, ref);
            }, nullptr, lookRef);
            if (!lookCompared) { Log("SelfTestRoom[%s]: FAIL look eye pass reference", name); return false; }
            lookSame = lookRef.checked == total && lookRef.bad <= total / 200 && lookDiffer <= total / 1000;
            Log("SelfTestRoom[%s]: look eye pass with the controls at 0 %s - %zu of %zu px differ from RoomPixel by more than 2 bits (worst %d; allowed %zu); "
                "%zu px differ from the plain eye pass (worst %d levels; allowed %zu)", name, lookSame ? "PASS" : "FAIL", lookRef.bad, lookRef.checked,
                lookRef.worst, total / 200, lookDiffer, lookWorst, total / 1000);
        }

        // The kept v10 eye pass on the same frame, against its own CPU copy; and how many
        // pixels it draws differently from the current pass (Stage 1's classification
        // differs from v10's only by float rounding and at exact edge ties, so a few
        // pixels may differ by a level). Only with the room light off: v10 has no panel.
        bool v10Ok = true;
        if (!lightCase && !lookCase)
        {
            WaitFence(app, app.fenceVal);
            app.cmdAlloc[0]->Reset();
            app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
            Transition(app.cmdList.Get(), t.colorOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            const bool v10Recorded = RecordCurvedScreen(app, DESC_CURVE_TEST, t.colorOut.Get(), cc, app.testEyeOut.Get(), nullptr, cb, UINT_MAX, nullptr,
                                                        app.curveRoomV10Pso.Get());
            Transition(app.cmdList.Get(), t.colorOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            app.cmdList->Close();
            WaitFence(app, SubmitAndSignal(app));
            std::vector<std::vector<unsigned char>> gotV10;
            if (!v10Recorded || !ReadbackRgba(app, app.testEyeOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, gotV10) || gotV10.size() != VIEWS)
            { Log("SelfTestRoom[%s]: FAIL v10 eye pass or its readback", name); return false; }
            EyeCompare v10;
            char v10What[48];
            snprintf(v10What, sizeof(v10What), "SelfTestRoom[%s] v10 eye pass", name);
            const bool v10Compared = CompareEyes(v10What, gotV10, TEST_EYE_W, TEST_EYE_W, TEST_EYE_H, 1, [&](int e, int x, int y, float ref[3])
            {
                const RgbaImage pic{ pictures[e].data(), W, H, W * 4 };
                return room_v10::RoomPixel(cc, cyl, room, view, e, x, y, pic, &glowImg, light, ref);
            }, nullptr, v10);
            if (!v10Compared) { Log("SelfTestRoom[%s]: FAIL v10 reference", name); return false; }
            size_t v10VsNew = 0;
            for (uint32_t e = 0; e < VIEWS; e++)
                for (size_t i = 0; i < (size_t)TEST_EYE_W * TEST_EYE_H; i++)
                    if (memcmp(gotV10[e].data() + i * 4, got[e].data() + i * 4, 3) != 0) v10VsNew++;
            v10Ok = v10.checked == total && v10.bad <= total / 200;
            Log("SelfTestRoom[%s]: v10 eye pass %s - %zu of %zu px differ from room_v10::RoomPixel by more than 2 bits (worst %d; allowed %zu); "
                "%zu px differ from the current eye pass", name, v10Ok ? "PASS" : "FAIL", v10.bad, v10.checked, v10.worst, total / 200, v10VsNew);
        }
        if (lightCase)
            Log("SelfTestRoom[%s]: room light %s - lamp emitter %s; the floor under the panel rises %.4f %.4f %.4f on the GPU (direct light %.4f %.4f %.4f); "
                "panel over %zu px of eye 0 (kappa > 0.5; needed above %zu), ceiling %zu, back %zu; eye 1: left wall %zu, floor %zu, the front's left wing %zu px",
                name, lampExact && floorLit ? "PASS" : "FAIL", lampExact ? "exact" : "WRONG", floorRise[0], floorRise[1], floorRise[2], floorDirect[0],
                floorDirect[1], floorDirect[2], panelPx, oneEyePercent, eyeKinds[0][1 + kFaceCeiling], eyeKinds[0][1 + kFaceBack],
                eyeKinds[1][1 + kFaceLeft], eyeKinds[1][1 + kFaceFloor], leftWingPx);

        const bool caseOk = badEm == 0 && lampExact && badTexel == 0 && lit == kRoomFaces - 1 && glowLit > 0 && bad <= total * dispatches / 200 && framed && v10Ok &&
                            floorLit && lookSame && badMirror == 0;
        Log("SelfTestRoom[%s]: %s - room %.2f x %.2f x %.2f m; %d emitters, %d px glow blocks (%zu differ, %zu glow blocks lit); lightmap %zu of %d texels "
            "outside half precision (worst %.2e), %d of 5 side, floor, ceiling and back faces lit; eyes %zu of %zu px differ by more than 2 bits (worst %d; "
            "allowed %zu); screen %zu, footprint %zu, front %zu, floor %zu, ceiling %zu, walls %zu, back %zu, outside %zu",
            name, caseOk ? "PASS" : "FAIL", 2 * room.X, room.yC - room.yF, room.zB + room.g, layout.count(), layout.block, badEm, glowLit, badTexel,
            kRoomFaces * kRoomLightmap * kRoomLightmap, worstRel, lit, bad, total * dispatches, worst, total * dispatches / 200, kinds[0],
            kinds[kRoomKindFootprint], kinds[1 + kFaceFront], kinds[1 + kFaceFloor], kinds[1 + kFaceCeiling], kinds[1 + kFaceLeft] + kinds[1 + kFaceRight],
            kinds[1 + kFaceBack], kinds[kRoomKindOutside]);
        ok = caseOk && ok;
    }
    Log("SelfTestRoom: exit %s", ok ? "PASS" : "FAIL");
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
    c.subpixel = app.opt.subpixel ? 1u : 0u;

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
    ok = SelfTestFrameTransfer(app) && ok;
    ok = SelfTestPrep(app, synth, DepthCrop{.17f,.23f,.5f}) && ok;
    // Steady/fuse parts only in --selftest (playback creates the estimator when used).
    if (app.opt.selfTestOnly) ok = SelfTestSteadyFuse(app, synth) && ok;
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
    ok = SelfTestAmbilight(app, synth) && ok;

    WarpConstants off = c;
    off.doWarp = 0;
    ok = SelfTestWarp(app, "no-warp", synth, truth, off) && ok;
    off.writeDepth = 0;
    std::vector<float> invalidDepth((size_t)W * H, std::numeric_limits<float>::quiet_NaN());
    ok = SelfTestWarp(app, "flat-fallback-invalid-depth", synth, invalidDepth, off) && ok;
    ok = SelfTestCurve(app) && ok;          // uses the last warped pair and the self-test glow
    ok = SelfTestRoom(app, synth) && ok;    // likewise, plus the self-test source

    Log("SelfTest: exit %s", ok ? "ALL PASS" : "FAILED");
    return ok;
}

// ------------------------------------------------------------ room benchmark
// --selftest --bench-room: the room's GPU passes timed offline, with no VR session, at
// the headset's eye size. The cases that draw the same screen - the curved one at each
// of kBenchCurves, or the flat one - form a group, and a group's cases are drawn in
// turn, frame by frame, the order rotating each round, so other work on the GPU falls
// on them alike. Every round also draws the probe: the plain curve pass at the first
// curve and yaw 0, the same work in every group, so its min shows whether the GPU was
// as free there as in the best group. Every case and view draws 30 frames to warm up,
// then 300 timed frames, each its own command list with five timestamps - T0, after
// EMIT, after LIGHT, after the eye pass, after the copy into a stand-in swapchain
// image - and a fence wait, as the frame loop's room frames are timed. Min / p50 / p95
// per pass go to the log and to room-bench.csv in the current directory.
//
// A row (a case at one curve, view and run) is CONTENDED when its eye pass's or its
// total's p50 is more than 10% above its min, or when the probe's min in the same rounds
// is more than 5% above its best: other work (SteamVR's compositor, a game) shared the
// GPU, and even the min may carry it - the probe catches the case where every frame of
// a round was slowed alike. The acceptance lines use steady rows only. (The probe's own
// p50 is not a test: short frames, like E's, can stay steady while the probe's longer
// ones are hit.)
//
// In the first run, every pixel of each case and view is classified by the CPU
// reference's own sample kinds (screen, room, mixed), so the eye pass's cost can be put
// per room pixel, and one extra frame is read back and checked against the CPU
// reference at every 16th pixel, so a broken frame is never timed as a fast one.
#pragma comment(lib, "advapi32.lib")

static const int BENCH_EYE_W = 2804, BENCH_EYE_H = 2860;     // SteamVR's recommended PSVR2 eye buffer (the headset log)
static const int BENCH_WARMUP = 30, BENCH_FRAMES = 300;
static const int BENCH_CHECK_STEP = 16;                      // the check: every 16th pixel each way
static const size_t BENCH_CHECK_ALLOWED = 1000;              // at most 1 in 1000 checked px more than 2 bits off...
static const size_t BENCH_CHECK_INTERIOR = 10000;            // ... and 1 in 10000 away from any edge more than kEyeFarOff off
static const float kBenchYaws[] = { 0.0f, 30.0f, 60.0f, 120.0f };
static const int BENCH_VIEWS = (int)(sizeof(kBenchYaws) / sizeof(kBenchYaws[0]));
static const float kBenchPitch = -15.0f;
static const float kBenchCurves[] = { 0.6f, 1.0f };          // the curved cases run at both; the probe at the first
static const int BENCH_CURVES = (int)(sizeof(kBenchCurves) / sizeof(kBenchCurves[0]));
static const int BENCH_GROUPS = BENCH_CURVES + 1;            // the curved screen at each curve, then the flat screen
static const int BENCH_ROOM = 45;                            // the Room slider, percent
static const float BENCH_HALF_IPD = 0.036f;
static const float BENCH_STAGE_FLOOR = -1.2f;                // SteamVR's floor, metres below the eyes
static const UINT BENCH_QUERIES = 64;
static const int BENCH_PASSES = (int)TIME_SLOT;              // emit, light, eye, copy, total
static const int BENCH_EYE_PASS = 2, BENCH_TOTAL = BENCH_PASSES - 1;
static const char* const kBenchPassNames[BENCH_PASSES] = { "emit", "light", "eye", "copy", "total" };
static const double BENCH_STEADY = 1.10, BENCH_STEADY_MS = 0.010;       // steady: eye and total p50 <= min * 1.10 + 0.01 ms
static const double BENCH_PROBE_SLOWER = 1.05, BENCH_PROBE_MS = 0.005;  // a free GPU: the probe's eye min <= its best * 1.05 + 0.005 ms

// The cases of the v11 spec (section 5.2). C and F have Glass, Reflections and the room
// light. A case is skipped until `ready`.
struct BenchCase
{
    char id;
    const char* what;
    bool room;                          // the room round the screen; else the plain curve pass
    bool curved;                        // the curved screen, at each of kBenchCurves; else the flat screen's half-size room layer
    bool v10;                           // the kept v10 eye pass (curveRoomV10Pso)
    int glass, reflect, light;          // v11's controls, percent
    bool ready;
};
static const BenchCase kBenchCases[] = {
    { 'A', "curve only, glow on", false, true, false, 0, 0, 0, true },
    { 'B', "curve + room, new eye pass, controls 0", true, true, false, 0, 0, 0, true },
    { 'C', "curve + room, new eye pass, glass 60 / reflections 40 / light 50", true, true, false, 60, 40, 50, true },
    { 'D', "curve + room, v10 eye pass", true, true, true, 0, 0, 0, true },
    { 'E', "flat room at half size, controls 0", true, false, false, 0, 0, 0, true },
    { 'F', "flat room at half size, glass 60 / reflections 40 / light 50", true, false, false, 60, 40, 50, true },
};
static const int BENCH_CASES = (int)(sizeof(kBenchCases) / sizeof(kBenchCases[0]));

struct BenchStats { double min = 0, p50 = 0, p95 = 0; };

// One case's (or the probe's) timings at one view: min / p50 / p95 per pass over the
// timed frames, and whether they were steady - the eye pass's and the total's p50
// within BENCH_STEADY of their min. Other work on the GPU pushes the p50 up.
struct BenchTimes
{
    BenchStats pass[BENCH_PASSES];
    size_t frames = 0;
    double eyeAbove = 0, totalAbove = 0;    // how far the eye pass's and the total's p50 lie above their min, percent
    bool steady = false;
};

static bool BenchTimesOf(const char* what, const std::vector<double> samples[BENCH_PASSES], BenchTimes& out)
{
    if (!what || !samples) { Log("BenchTimesOf: FAIL no samples"); return false; }
    Log("BenchTimesOf: enter (%s, %zu frames)", what, samples[0].size());
    out = BenchTimes();
    for (int p = 0; p < BENCH_PASSES; p++)
    {
        if (samples[p].empty()) { Log("BenchTimesOf: FAIL %s has no %s times", what, kBenchPassNames[p]); return false; }
        std::vector<double> v = samples[p];
        std::sort(v.begin(), v.end());
        out.pass[p].min = v.front();
        out.pass[p].p50 = v[v.size() / 2];
        out.pass[p].p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
    }
    out.frames = samples[0].size();
    const BenchStats& eye = out.pass[BENCH_EYE_PASS];
    const BenchStats& total = out.pass[BENCH_TOTAL];
    out.eyeAbove = eye.min > 0 ? 100.0 * (eye.p50 / eye.min - 1.0) : 0.0;
    out.totalAbove = total.min > 0 ? 100.0 * (total.p50 / total.min - 1.0) : 0.0;
    out.steady = eye.p50 <= eye.min * BENCH_STEADY + BENCH_STEADY_MS && total.p50 <= total.min * BENCH_STEADY + BENCH_STEADY_MS;
    Log("BenchTimesOf: exit - %s: eye %.3f ms min, p50 %+.1f%%; total %.3f ms min, p50 %+.1f%%: %s", what, eye.min, out.eyeAbove, total.min,
        out.totalAbove, out.steady ? "steady" : "unsteady");
    return true;
}

// Every pixel of one view by its four samples' kinds, as the eye pass casts them: all
// on the screen (or the flat screen's footprint), all on the room's faces, all on the
// glow or all on the world (case A), all outside the room (world), or mixed.
struct BenchClasses
{
    uint64_t screen = 0, room = 0, glow = 0, world = 0, mixed = 0;
    uint64_t Offscreen() const { return room + glow + world + mixed; }
};

// The CPU reference's classification of every pixel (room null: the plain curve pass),
// spread over the CPU's threads. With `kinds`, each pixel's kind too (eye-major, ew x eh
// per eye; kEyeKindMixed where its four rays disagree), for CompareEyes' edges.
static BenchClasses BenchClassify(const CurveConstants& cc, const Cylinder& cyl, const Room* room, const RoomView& view, bool v10,
                                  std::vector<unsigned char>* kinds)
{
    const int rows = (int)cc.eh * (int)VIEWS;
    const unsigned threads = std::max(1u, std::min(64u, std::thread::hardware_concurrency()));
    Log("BenchClassify: enter (%ux%u x %u eyes, %s, %u threads%s)", cc.ew, cc.eh, VIEWS, room ? (v10 ? "room, v10" : "room") : "curve only", threads,
        kinds ? ", with each pixel's kind" : "");
    if (kinds) kinds->assign((size_t)cc.ew * cc.eh * VIEWS, 0);
    std::vector<BenchClasses> part(threads);
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++)
        pool.emplace_back([&, t]()
        {
            BenchClasses& c = part[t];
            for (int row = (int)t; row < rows; row += (int)threads)
            {
                const int e = row / (int)cc.eh, y = row % (int)cc.eh;
                for (int x = 0; x < (int)cc.ew; x++)
                {
                    int kind[4];
                    for (int s = 0; s < 4; s++)
                    {
                        float o[3], d[3], u = 0, v = 0, gu = 0, gv = 0;
                        CurveRay(cc, e, (float)x + 0.5f + kCurveSubsamples[s][0], (float)y + 0.5f + kCurveSubsamples[s][1], o, d);
                        if (!room) kind[s] = CylinderHit(cyl, o, d, &u, &v) ? 0 : (GlowHit(cc, o, d, &u, &v) ? 1 : 2);
                        else if (v10) kind[s] = room_v10::RoomClassify(cc, cyl, *room, view, o, d, &u, &v, &gu, &gv);
                        else kind[s] = RoomClassify(cc, cyl, *room, view, o, d, &u, &v, &gu, &gv);
                    }
                    const bool mixed = kind[0] != kind[1] || kind[1] != kind[2] || kind[2] != kind[3];
                    if (kinds) (*kinds)[(size_t)row * cc.ew + x] = mixed ? kEyeKindMixed : (unsigned char)kind[0];
                    if (mixed) c.mixed++;
                    else if (!room) (kind[0] == 0 ? c.screen : (kind[0] == 1 ? c.glow : c.world))++;
                    else if (kind[0] == 0 || kind[0] == kRoomKindFootprint) c.screen++;
                    else if (kind[0] == kRoomKindOutside) c.world++;
                    else c.room++;
                }
            }
        });
    for (std::thread& th : pool) th.join();
    BenchClasses sum;
    for (const BenchClasses& c : part)
    {
        sum.screen += c.screen; sum.room += c.room; sum.glow += c.glow; sum.world += c.world; sum.mixed += c.mixed;
    }
    Log("BenchClassify: exit - screen %llu, room %llu, glow %llu, world %llu, mixed %llu px", (unsigned long long)sum.screen,
        (unsigned long long)sum.room, (unsigned long long)sum.glow, (unsigned long long)sum.world, (unsigned long long)sum.mixed);
    return sum;
}

// The benchmark's own GPU objects: the eye buffers (a UAV, as App::eyeOut), a stand-in
// for the acquired swapchain image (RENDER_TARGET at rest, so the copy is timed as in
// the headset), and its own timestamps.
struct BenchGpu
{
    ComPtr<ID3D12Resource> eyes, image, readback;
    ComPtr<ID3D12QueryHeap> heap;
    const UINT64* ts = nullptr;
    double freq = 0;
};

// One benchmark frame, drawn as the frame loop draws one: T0; the room's light (EMIT,
// MIRROR with mirrorH above 0, T1, LIGHT, T2) when roomCb is set, else T1 and T2 at once;
// the eye pass (T3) and its copy into the stand-in swapchain image (T4). It waits for the
// frame; `ms` gets emit (with the mirror), light, eye, copy and total. No logging: it runs
// 330 times per case and view.
static bool BenchFrame(App& app, BenchGpu& b, const CurveConstants& cc, D3D12_GPU_VIRTUAL_ADDRESS roomCb, UINT emitters, UINT mirrorH,
                       ID3D12Resource* geometry, ID3D12PipelineState* roomPso, double ms[BENCH_PASSES])
{
    if (!ms || !b.eyes || !b.image || !b.heap || !b.readback || !b.ts || !(b.freq > 0)) return false;
    if (roomCb != 0 && emitters == 0) return false;

    WaitFence(app, app.fenceVal);
    app.cmdAlloc[0]->Reset();
    app.cmdList->Reset(app.cmdAlloc[0].Get(), nullptr);
    ID3D12GraphicsCommandList* cl = app.cmdList.Get();
    WarpTarget& t = app.testTarget;
    Transition(cl, t.colorOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);    // as the warp leaves it
    cl->EndQuery(b.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    bool recorded = true;
    if (roomCb != 0) recorded = RecordRoomLight(app, DESC_TEST_SRC, roomCb, geometry, emitters, mirrorH, 0, b.heap.Get());
    else
    {
        cl->EndQuery(b.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        cl->EndQuery(b.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    }
    recorded = recorded && RecordCurvedScreen(app, DESC_CURVE_BENCH, t.colorOut.Get(), cc, b.eyes.Get(), b.image.Get(), roomCb, 0, b.heap.Get(), roomPso);
    cl->ResolveQueryData(b.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, TIME_SLOT, b.readback.Get(), 0);
    Transition(cl, t.colorOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->Close();
    if (!recorded) return false;
    WaitFence(app, SubmitAndSignal(app));

    const UINT64* ts = b.ts;
    for (UINT k = 1; k < TIME_SLOT; k++)
        if (ts[k] < ts[k - 1]) return false;
    for (UINT k = 1; k < TIME_SLOT; k++) ms[k - 1] = (double)(ts[k] - ts[k - 1]) * 1000.0 / b.freq;
    ms[BENCH_PASSES - 1] = (double)(ts[TIME_SLOT - 1] - ts[0]) * 1000.0 / b.freq;
    return true;
}

// Windows' Developer Mode, which SetStablePowerState needs (without it the call
// removes the device).
static bool DeveloperModeOn()
{
    DWORD value = 0, size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
                                        L"AllowDevelopmentWithoutDevLicense", RRF_RT_REG_DWORD, nullptr, &value, &size);
    const bool on = status == ERROR_SUCCESS && value != 0;
    Log("DeveloperModeOn: %s (registry status %ld, value %lu)", on ? "yes" : "no", (long)status, (unsigned long)value);
    return on;
}

// Whether SteamVR's compositor is running. It draws on the same GPU, so the benchmark's
// frames can share the GPU with its frames (and a game's).
static bool CompositorRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { Log("CompositorRunning: no process snapshot (%lu) - assumed not running", GetLastError()); return false; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"vrcompositor.exe") == 0;
    CloseHandle(snap);
    Log("CompositorRunning: SteamVR's compositor is %s", found ? "running - it shares the GPU" : "not running");
    return found;
}

// Locks the GPU's clocks for the benchmark: Release unlocks them, as does going out of
// scope on an early return.
struct BenchStablePower
{
    ID3D12Device* device = nullptr;
    void Release()
    {
        if (!device) return;
        const HRESULT hr = device->SetStablePowerState(FALSE);
        device = nullptr;
        Log("BenchRoom: GPU clocks unlocked again (0x%08X)", (unsigned)hr);
    }
    ~BenchStablePower() { Release(); }
};

// One row of results: a case at one curve (or the flat screen), one view and one run.
struct BenchRow
{
    int run = 0, caseIndex = 0, group = 0, curvePct = 0, view = 0;
    int ew = 0, eh = 0, emitters = 0;
    BenchClasses classes;
    EyeCompare check;
    BenchTimes times;                   // the case's
    BenchTimes probe;                   // the probe's, drawn in the same rounds
    bool contended = false;
    char why[112] = "";                 // what made it CONTENDED
};

// What every group draws: the screen, the eyes' fields of view, the self-test's pictures
// and glow (for the check), and the probe's constants.
struct BenchScene
{
    float width = 5.7f, height = 0, distance = 0, glowHalfW = 0, glowHalfH = 0;
    XrPosef screenPose{};
    XrFovf fov[VIEWS]{};
    uint32_t world = 0;                                 // the default world colour; its value costs nothing
    std::vector<std::vector<unsigned char>> pictures;   // the self-test's warped pair, W x H RGBA per eye
    std::vector<unsigned char> glowPx;                  // the self-test's glow, ambiW x ambiH RGBA
    int glowW = 0, glowH = 0;
    CurveConstants probe{};                             // the plain curve pass at kBenchCurves[0], yaw 0, full size
};

// One timed entry of a group at one view: a case, or the probe.
struct BenchEntry
{
    int caseIndex = -1;                 // -1: the probe
    CurveConstants cc{};
    RoomConstants rc{};                 // a room case's constants, which the check's reference reads too
    D3D12_GPU_VIRTUAL_ADDRESS roomCb = 0;
    ID3D12PipelineState* roomPso = nullptr;
    int ew = 0, eh = 0;
    std::vector<double> samples[BENCH_PASSES];
};

// The head at the origin, turned yawDeg and pitched kBenchPitch; the eyes either side.
static bool BenchEyes(float yawDeg, XrPosef eyes[VIEWS])
{
    if (!eyes) { Log("BenchEyes: FAIL no eyes"); return false; }
    const XrQuaternionf q = YawPitch(yawDeg, kBenchPitch);
    float R[3][3];
    QuatRows(q, R);
    for (uint32_t e = 0; e < VIEWS; e++)
    {
        const float side = e == 0 ? -BENCH_HALF_IPD : BENCH_HALF_IPD;
        eyes[e].orientation = q;
        eyes[e].position = { side * R[0][0], side * R[1][0], side * R[2][0] };
    }
    Log("BenchEyes: yaw %.0f, pitch %.0f - left eye at %.4f, %.4f, %.4f m", yawDeg, kBenchPitch, eyes[0].position.x, eyes[0].position.y,
        eyes[0].position.z);
    return true;
}

// One extra frame of a case, read back and checked against the CPU reference at every
// 16th pixel, with every pixel of the view classified by the CPU reference (for the
// pixel counts, and to tell the check's edge pixels from the rest). At most 1 in 1000
// of the checked pixels may be more than 2 bits off - at an edge a GPU and a CPU ray can
// land either side, and elsewhere rounding reaches 3 levels on a few - and away from any
// edge at most 1 in 10000 may be more than kEyeFarOff (8) levels off, which is a wrong
// surface, not rounding.
static bool BenchCheck(App& app, BenchGpu& b, const BenchScene& sc, const BenchEntry& en, const Cylinder& cyl, const Room* room,
                       const RoomView& view, UINT emitters, const char* what, BenchRow& row)
{
    if (!what) { Log("BenchCheck: FAIL no name"); return false; }
    if (en.caseIndex < 0 || en.caseIndex >= BENCH_CASES) { Log("BenchCheck: FAIL %s - case %d", what, en.caseIndex); return false; }
    const BenchCase& bc = kBenchCases[en.caseIndex];
    if (bc.room != (room != nullptr)) { Log("BenchCheck: FAIL %s - the room is %s", what, room ? "given to a case without one" : "missing"); return false; }
    if (sc.pictures.size() != VIEWS || sc.glowPx.empty()) { Log("BenchCheck: FAIL %s - no pictures or glow", what); return false; }
    Log("BenchCheck: enter (%s, %ux%u per eye, every %d px)", what, en.cc.ew, en.cc.eh, BENCH_CHECK_STEP);

    double ms[BENCH_PASSES];
    if (!BenchFrame(app, b, en.cc, en.roomCb, emitters, en.rc.mirrorH, nullptr, en.roomPso, ms)) { Log("BenchCheck: FAIL %s - the frame", what); return false; }
    RoomLightmap light;
    if (room)
    {
        std::vector<std::vector<unsigned char>> lm;
        if (!ReadbackRgba(app, app.roomLight.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, lm, 8) || lm.size() != (size_t)kRoomFaces)
        { Log("BenchCheck: FAIL %s - lightmap readback", what); return false; }
        light.texels.resize((size_t)kRoomFaces * kRoomLightmap * kRoomLightmap * 4);
        for (int f = 0; f < kRoomFaces; f++)
            for (size_t i = 0; i < (size_t)kRoomLightmap * kRoomLightmap * 4; i++)
                light.texels[(size_t)f * kRoomLightmap * kRoomLightmap * 4 + i] = RoomHalfToFloat(((const uint16_t*)lm[f].data())[i]);
    }
    RoomMirror mirror;
    if (room && en.rc.mirrorH > 0 && !ReadbackMirror(app, (int)en.rc.mirrorW, (int)en.rc.mirrorH, mirror))
    { Log("BenchCheck: FAIL %s - mirror picture readback", what); return false; }
    std::vector<std::vector<unsigned char>> got;
    if (!ReadbackRgba(app, b.eyes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, got) || got.size() != VIEWS)
    { Log("BenchCheck: FAIL %s - eye readback", what); return false; }

    std::vector<unsigned char> kinds;
    row.classes = BenchClassify(en.cc, cyl, room, view, bc.v10, &kinds);
    const RgbaImage glowImg{ sc.glowPx.data(), sc.glowW, sc.glowH, sc.glowW * 4 };
    const bool compared = CompareEyes(what, got, BENCH_EYE_W, (int)en.cc.ew, (int)en.cc.eh, BENCH_CHECK_STEP, [&](int e, int x, int y, float ref[3])
    {
        const RgbaImage pic{ sc.pictures[e].data(), W, H, W * 4 };
        if (!room) return CurvedPixel(en.cc, e, x, y, cyl, pic, &glowImg, ref);
        if (bc.v10) return room_v10::RoomPixel(en.cc, cyl, *room, view, e, x, y, pic, &glowImg, light, ref);
        RoomEyeInputs inputs;
        inputs.rc = &en.rc; inputs.picture = &pic; inputs.glow = &glowImg; inputs.light = &light; inputs.mirror = &mirror;
        return RoomPixel(en.cc, cyl, *room, view, e, x, y, inputs, ref);
    }, &kinds, row.check);
    if (!compared) { Log("BenchCheck: FAIL %s - the reference", what); return false; }

    const EyeCompare& k = row.check;
    const size_t farAllowed = k.InteriorChecked() / BENCH_CHECK_INTERIOR, allowed = k.checked / BENCH_CHECK_ALLOWED;
    const bool pass = k.interiorFar <= farAllowed && k.bad <= allowed;
    Log("BenchCheck: exit %s - %s: %zu of %zu px more than 2 bits off (allowed %zu, worst %d); away from edges %zu of %zu (worst %d), %zu more than "
        "%d levels off (allowed %zu); at edges %zu of %zu%s", pass ? "PASS" : "FAIL", what, k.bad, k.checked, allowed, k.worst, k.InteriorBad(),
        k.InteriorChecked(), k.interiorWorst, k.interiorFar, kEyeFarOff, farAllowed, k.edgeBad, k.edgeChecked,
        pass ? "" : ": the timed frame is not what the CPU reference draws");
    return pass;
}

// One group - the cases that draw the curved screen at kBenchCurves[g], or (g ==
// BENCH_CURVES) the flat screen - timed at every view, its cases and the probe drawn in
// turn. In the first run it also checks one extra frame of each case and view (and
// counts its pixel classes); later runs copy those from the first. Appends a row per
// case and view, each with the probe's times from the same rounds.
static bool BenchGroup(App& app, BenchGpu& b, const BenchScene& sc, int run, int g, std::vector<BenchRow>& rows)
{
    if (run < 0 || g < 0 || g >= BENCH_GROUPS) { Log("BenchGroup: FAIL run %d, group %d", run, g); return false; }
    const bool curved = g < BENCH_CURVES;
    const float curve = curved ? kBenchCurves[g] : 0.0f;
    const int curvePct = (int)std::lround(curve * 100.0f);
    char label[16];
    if (curved) snprintf(label, sizeof(label), "%d%%", curvePct);
    else snprintf(label, sizeof(label), "flat");

    // The group's cases: the ready ones that draw this screen.
    std::vector<int> members;
    int roomMembers = 0;
    for (int ci = 0; ci < BENCH_CASES; ci++)
    {
        const BenchCase& bc = kBenchCases[ci];
        if (!bc.ready || bc.curved != curved) continue;
        members.push_back(ci);
        if (bc.room) roomMembers++;
    }
    Log("BenchGroup: enter (run %d, %s screen, %zu cases and the probe, drawn in turn)", run + 1, label, members.size());
    if (members.empty()) { Log("BenchGroup: exit - the %s screen has no cases yet", label); return true; }
    if (roomMembers > RING + 1) { Log("BenchGroup: FAIL %d room cases, more than roomCbUp's %d constant slots", roomMembers, RING + 1); return false; }

    Cylinder cyl;
    if (curved && (!BuildCylinder(sc.width, sc.height, sc.distance, curve, cyl) || !cyl.curved)) { Log("BenchGroup: FAIL cylinder at %s", label); return false; }

    // The room, as the frame loop builds it round the recentre point: one for the group,
    // whose room cases differ only in their constants. Each case has its own slot of
    // roomCbUp; the frame loop's slots are free, as the self-test runs no frame loop.
    Room room;
    RoomEmitterLayout layout;
    RoomView view;
    if (roomMembers > 0)
    {
        RoomInputs in;
        in.W = sc.width; in.H = sc.height; in.eye[2] = sc.distance; in.floorY = BENCH_STAGE_FLOOR; in.cyl = cyl;
        std::vector<RoomEmitter> em;
        layout = RoomLayout(sc.width, sc.height, app.ambiW, app.ambiH);
        if (!BuildRoom(in, room) || layout.count() == 0 || !BuildRoomEmitters(room, cyl, sc.width, sc.height, sc.glowHalfW, sc.glowHalfH, layout, em))
        { Log("BenchGroup: FAIL room at %s", label); return false; }
        view.flatLayer = !curved; view.W = sc.width; view.H = sc.height; view.glowOn = true;
        view.glowHalfW = sc.glowHalfW; view.glowHalfH = sc.glowHalfH; view.dither = true; view.cyl = cyl;
        memcpy(app.roomGeomMapped[RING], em.data(), em.size() * sizeof(RoomEmitter));
        Log("BenchGroup: %s room %.2f x %.2f x %.2f m, floor %.2f m below the eye, %s front, %d emitters (glow blocks %d px; the room light's "
            "panel x %.2f..%.2f, z %.2f..%.2f)", label, 2 * room.X, room.yC - room.yF, room.zB + room.g, -room.yF, room.curved ? "curved" : "flat",
            layout.count(), layout.block, room.lightX0, room.lightX1, room.lightZ0, room.lightZ1);
    }

    // Each case's eye size, eye pass and constants; the probe last.
    std::vector<BenchEntry> entries(members.size() + 1);
    int slot = 0;
    for (size_t m = 0; m < members.size(); m++)
    {
        const BenchCase& bc = kBenchCases[members[m]];
        BenchEntry& en = entries[m];
        en.caseIndex = members[m];
        en.ew = bc.room && !bc.curved ? BENCH_EYE_W / 2 : BENCH_EYE_W;
        en.eh = bc.room && !bc.curved ? BENCH_EYE_H / 2 : BENCH_EYE_H;
        en.roomPso = bc.v10 ? app.curveRoomV10Pso.Get() : app.curveRoomPso.Get();
        if (!bc.room) continue;
        RoomLook look;
        look.glass = bc.glass; look.reflect = bc.reflect; look.light = bc.light;
        const RoomShading shading = MakeRoomShading(room, BENCH_ROOM, sc.world, sc.width * sc.height, look);
        en.rc = MakeRoomConstants(room, shading, layout, view, W, H, 1.0f);
        const RoomConstants& rc = en.rc;
        if (bc.light > 0 && (rc.flags & kRoomFlagLight) == 0) { Log("BenchGroup: FAIL case %c's room light is off (panel %s)", bc.id, room.lightValid ? "fits" : "missing"); return false; }
        if ((bc.glass > 0 || bc.reflect > 0) != ((rc.flags & kRoomFlagFinish) != 0)) { Log("BenchGroup: FAIL case %c's finish is %s", bc.id, (rc.flags & kRoomFlagFinish) ? "on" : "off"); return false; }
        if ((bc.reflect > 0) != ((rc.flags & kRoomFlagReflect) != 0)) { Log("BenchGroup: FAIL case %c's reflections are %s", bc.id, (rc.flags & kRoomFlagReflect) ? "on" : "off"); return false; }
        if (!bc.v10) en.roomPso = RoomEyePso(app, rc);
        memcpy(app.roomCbMapped + (size_t)slot * sizeof(RoomConstants), &rc, sizeof(rc));
        en.roomCb = app.roomCbUp->GetGPUVirtualAddress() + (UINT64)slot * sizeof(RoomConstants);
        slot++;
    }
    BenchEntry& probe = entries.back();
    probe.cc = sc.probe;
    probe.ew = BENCH_EYE_W;
    probe.eh = BENCH_EYE_H;

    bool geometryPending = roomMembers > 0;      // the group's first room frame copies in the emitters' geometry
    const size_t n = entries.size();
    for (int vi = 0; vi < BENCH_VIEWS; vi++)
    {
        XrPosef eyes[VIEWS];
        if (!BenchEyes(kBenchYaws[vi], eyes)) { Log("BenchGroup: FAIL eyes"); return false; }
        for (size_t m = 0; m + 1 < n; m++)
            entries[m].cc = MakeCurveConstants(curved ? cyl : Cylinder(), sc.screenPose, eyes, sc.fov, entries[m].ew, entries[m].eh, true,
                                               2.0f * sc.glowHalfW, 2.0f * sc.glowHalfH, sc.world, true);
        for (BenchEntry& en : entries)
            for (std::vector<double>& s : en.samples)
            {
                s.clear();
                s.reserve(BENCH_FRAMES);
            }

        // The rounds: every entry once a round, each round starting one further on.
        for (int i = 0; i < BENCH_WARMUP + BENCH_FRAMES; i++)
            for (size_t j = 0; j < n; j++)
            {
                BenchEntry& en = entries[((size_t)i + j) % n];
                const bool copyGeometry = geometryPending && en.roomCb != 0;
                double ms[BENCH_PASSES];
                if (!BenchFrame(app, b, en.cc, en.roomCb, (UINT)layout.count(), en.rc.mirrorH, copyGeometry ? app.roomGeomUp[RING].Get() : nullptr, en.roomPso, ms))
                {
                    Log("BenchGroup: FAIL %s yaw %.0f, round %d, %s (device %s)", label, kBenchYaws[vi], i,
                        en.caseIndex < 0 ? "the probe" : kBenchCases[en.caseIndex].what, FAILED(app.device->GetDeviceRemovedReason()) ? "removed" : "ok");
                    return false;
                }
                if (copyGeometry) geometryPending = false;
                if (i < BENCH_WARMUP) continue;
                for (int p = 0; p < BENCH_PASSES; p++) en.samples[p].push_back(ms[p]);
            }

        char what[48];
        snprintf(what, sizeof(what), "probe, %s yaw %.0f", label, kBenchYaws[vi]);
        BenchTimes probeTimes;
        if (!BenchTimesOf(what, probe.samples, probeTimes)) { Log("BenchGroup: FAIL the probe's times"); return false; }

        for (size_t m = 0; m + 1 < n; m++)
        {
            const BenchEntry& en = entries[m];
            const BenchCase& bc = kBenchCases[en.caseIndex];
            BenchRow row;
            row.run = run; row.caseIndex = en.caseIndex; row.group = g; row.curvePct = curvePct; row.view = vi;
            row.ew = en.ew; row.eh = en.eh; row.emitters = bc.room ? layout.count() : 0;
            row.probe = probeTimes;
            snprintf(what, sizeof(what), "%c %s yaw %.0f", bc.id, label, kBenchYaws[vi]);
            if (!BenchTimesOf(what, en.samples, row.times)) { Log("BenchGroup: FAIL the times of %s", what); return false; }
            if (run == 0 && !BenchCheck(app, b, sc, en, cyl, bc.room ? &room : nullptr, view, (UINT)layout.count(), what, row))
            { Log("BenchGroup: FAIL the check of %s", what); return false; }
            for (const BenchRow& first : rows)
            {
                if (run == 0 || first.run != 0 || first.caseIndex != row.caseIndex || first.group != g || first.view != vi) continue;
                row.classes = first.classes;
                row.check = first.check;
            }

            const BenchStats* s = row.times.pass;
            const double offscreenM = (double)row.classes.Offscreen() / 1e6;
            char pixels[160];
            if (bc.room)
                snprintf(pixels, sizeof(pixels), "screen %.2f M, room %.2f M, mixed %.3f M px", row.classes.screen / 1e6, row.classes.room / 1e6,
                         row.classes.mixed / 1e6);
            else
                snprintf(pixels, sizeof(pixels), "screen %.2f M, glow %.2f M, world %.2f M, mixed %.3f M px", row.classes.screen / 1e6,
                         row.classes.glow / 1e6, row.classes.world / 1e6, row.classes.mixed / 1e6);
            Log("BenchGroup[%s] run %d: total %.3f / %.3f / %.3f ms (min / p50 / p95); emit %.3f / %.3f / %.3f, light %.3f / %.3f / %.3f, "
                "eye %.3f / %.3f / %.3f, copy %.3f / %.3f / %.3f; %s; eye per M off-screen px %.4f ms (min), %.4f ms (p50); "
                "probe eye %.3f / %.3f ms (min / p50); check %zu of %zu px more than 2 bits off, %zu more than %d away from edges; %s",
                what, run + 1, s[4].min, s[4].p50, s[4].p95, s[0].min, s[0].p50, s[0].p95, s[1].min, s[1].p50, s[1].p95,
                s[2].min, s[2].p50, s[2].p95, s[3].min, s[3].p50, s[3].p95, pixels, offscreenM > 0 ? s[2].min / offscreenM : 0.0,
                offscreenM > 0 ? s[2].p50 / offscreenM : 0.0, probeTimes.pass[BENCH_EYE_PASS].min, probeTimes.pass[BENCH_EYE_PASS].p50,
                row.check.bad, row.check.checked, row.check.interiorFar, kEyeFarOff, row.times.steady ? "steady" : "unsteady");
            rows.push_back(row);
        }
    }
    Log("BenchGroup: exit - run %d, %s screen, %zu cases at %d views", run + 1, label, members.size(), BENCH_VIEWS);
    return true;
}

static bool BenchRoom(App& app)
{
    if (!app.device || !app.curveRoomPso || !app.curveRoomLookPso || !app.curveRoomV10Pso || !app.roomCbMapped || !app.roomGeomMapped[RING] ||
        !app.testTarget.colorOut || !app.ambiTex || !app.roomLight || !app.roomEmitters || !app.roomMirror || !app.roomMirrorPso)
    { Log("BenchRoom: FAIL the room's passes are not built"); return false; }
    Log("BenchRoom: enter (%dx%d per eye, %d warm-up + %d timed frames per case and view, the cases of a screen drawn in turn with a probe, "
        "Room %d%%, source %dx%d, glow %dx%d)", BENCH_EYE_W, BENCH_EYE_H, BENCH_WARMUP, BENCH_FRAMES, BENCH_ROOM, W, H, app.ambiW, app.ambiH);
    const double started = NowSeconds();
    ID3D12Device* dev = app.device.Get();

    // --- its own eye buffers, stand-in swapchain image, descriptors and timestamps
    BenchGpu b;
    if (!MakeTexture(dev, BENCH_EYE_W, BENCH_EYE_H, DXGI_FORMAT_R8G8B8A8_TYPELESS, VIEWS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, b.eyes) ||
        !MakeTexture(dev, BENCH_EYE_W, BENCH_EYE_H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, VIEWS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                     D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_RENDER_TARGET, b.image))
    { Log("BenchRoom: FAIL eye buffers"); return false; }
    MakePictureSrv(app, app.testTarget.colorOut.Get(), DESC_CURVE_BENCH + 0);
    D3D12_SHADER_RESOURCE_VIEW_DESC glowSrv{};
    glowSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    glowSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    glowSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    glowSrv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(app.ambiTex.Get(), &glowSrv, CpuDesc(app, DESC_CURVE_BENCH + 1));
    MakeEyeUav(app, b.eyes.Get(), DESC_CURVE_BENCH + 2);
    D3D12_SHADER_RESOURCE_VIEW_DESC lmSrv{};
    lmSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lmSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lmSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    lmSrv.Texture2DArray.MipLevels = 1;
    lmSrv.Texture2DArray.ArraySize = kRoomFaces;
    dev->CreateShaderResourceView(app.roomLight.Get(), &lmSrv, CpuDesc(app, DESC_CURVE_BENCH + 3));
    dev->CreateShaderResourceView(app.roomMirror.Get(), nullptr, CpuDesc(app, DESC_CURVE_BENCH + 4));
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = BENCH_QUERIES;
        UINT64 freq = 0;
        if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&b.heap))) || FAILED(app.gfxQueue->GetTimestampFrequency(&freq)) || freq == 0 ||
            !MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, (UINT64)BENCH_QUERIES * sizeof(UINT64), D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, b.readback, nullptr) ||
            FAILED(b.readback->Map(0, nullptr, (void**)&b.ts)) || !b.ts)
        { Log("BenchRoom: FAIL GPU timestamps unavailable"); return false; }
        b.freq = (double)freq;
    }
    Log("BenchRoom: %.1f MB of eye buffers and a stand-in swapchain image, timestamps at %.0f MHz", 2.0 * BENCH_EYE_W * BENCH_EYE_H * 4 * VIEWS / (1024.0 * 1024.0),
        b.freq / 1e6);

    // --- the scene: the default screen (5.7 m, 16:9) at 3 m, the eyes at its middle's
    // height; the self-test's warped pair and glow, which the check needs too
    BenchScene sc;
    sc.height = sc.width * 9.0f / 16.0f;
    sc.distance = ScreenAnchor::distance;
    const float margin = kAmbiMargin * sc.width;
    sc.glowHalfW = 0.5f * sc.width + margin;
    sc.glowHalfH = 0.5f * sc.height + margin;
    sc.screenPose = XrPosef{ { 0, 0, 0, 1 }, { 0, 0, -sc.distance } };
    const float d2r = kRoomPi / 180.0f;
    sc.fov[0] = XrFovf{ app.opt.benchFov[0] * d2r, app.opt.benchFov[1] * d2r, app.opt.benchFov[2] * d2r, app.opt.benchFov[3] * d2r };
    sc.fov[1] = XrFovf{ -sc.fov[0].angleRight, -sc.fov[0].angleLeft, sc.fov[0].angleUp, sc.fov[0].angleDown };
    std::vector<std::vector<unsigned char>> glowPx;
    if (!ReadbackRgba(app, app.testTarget.colorOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, sc.pictures) || sc.pictures.size() != VIEWS ||
        !ReadbackRgba(app, app.ambiTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, glowPx) || glowPx.size() != 1)
    { Log("BenchRoom: FAIL readback of the pictures and glow"); return false; }
    sc.glowPx = std::move(glowPx[0]);
    sc.glowW = app.ambiW;
    sc.glowH = app.ambiH;
    Cylinder probeCyl;
    XrPosef probeEyes[VIEWS];
    if (!BuildCylinder(sc.width, sc.height, sc.distance, kBenchCurves[0], probeCyl) || !probeCyl.curved || !BenchEyes(kBenchYaws[0], probeEyes))
    { Log("BenchRoom: FAIL the probe's cylinder"); return false; }
    sc.probe = MakeCurveConstants(probeCyl, sc.screenPose, probeEyes, sc.fov, BENCH_EYE_W, BENCH_EYE_H, true, 2.0f * sc.glowHalfW, 2.0f * sc.glowHalfH,
                                  sc.world, true);
    Log("BenchRoom: screen %.2f x %.2f m at %.1f m, eyes %.3f m apart, fov left eye %.1f / %.1f / %.1f / %.1f deg (right mirrored), views yaw 0/30/60/120 "
        "at pitch %.0f; the probe is (A) at %.0f%%, yaw %.0f", sc.width, sc.height, sc.distance, 2 * BENCH_HALF_IPD, app.opt.benchFov[0], app.opt.benchFov[1],
        app.opt.benchFov[2], app.opt.benchFov[3], kBenchPitch, kBenchCurves[0] * 100.0f, kBenchYaws[0]);

    // --- the clocks. Locking them (SetStablePowerState) holds the whole GPU at its base
    // clocks - SteamVR's and a game's frames too - so it is not done while SteamVR's
    // compositor runs, unless --bench-lock asks for it.
    const bool compositor = CompositorRunning();
    BenchStablePower power;
    if (app.opt.benchBoost) Log("BenchRoom: GPU clocks left unlocked (--bench-boost): 3 runs, for the spread");
    else if (compositor && !app.opt.benchLock)
        Log("BenchRoom: GPU clocks left unlocked, because SteamVR's compositor is running and locking them would slow the headset's frames too for the whole "
            "benchmark (--bench-lock locks them anyway): 3 runs, for the spread");
    else if (!DeveloperModeOn()) Log("BenchRoom: Developer Mode is off, so the GPU clocks cannot be locked (SetStablePowerState): 3 runs, for the spread");
    else
    {
        const HRESULT hr = dev->SetStablePowerState(TRUE);
        if (SUCCEEDED(hr)) power.device = dev;
        Log("BenchRoom: SetStablePowerState(TRUE) %s (0x%08X)%s%s", SUCCEEDED(hr) ? "- GPU clocks locked" : "failed", (unsigned)hr,
            SUCCEEDED(hr) ? "" : ": 3 runs, for the spread", SUCCEEDED(hr) && compositor ? " (--bench-lock: SteamVR's frames are slowed too)" : "");
        if (FAILED(dev->GetDeviceRemovedReason())) { Log("BenchRoom: FAIL the device was removed"); return false; }
    }
    const bool stable = power.device != nullptr;
    const int runs = stable ? 1 : 3;
    if (compositor) Log("BenchRoom: SteamVR's compositor is running on this GPU: its frames can fall in the timed ones, and rows where they did are marked CONTENDED");

    std::vector<BenchRow> rows;
    bool ok = true;
    for (int ci = 0; ci < BENCH_CASES; ci++)
    {
        const BenchCase& bc = kBenchCases[ci];
        if (!bc.ready) Log("BenchRoom[%c]: skipped - %s is not ready", bc.id, bc.what);
    }
    for (int run = 0; run < runs && ok; run++)
        for (int g = 0; g < BENCH_GROUPS && ok; g++)
            ok = BenchGroup(app, b, sc, run, g, rows);
    WaitFence(app, app.fenceVal);

    // --- contention. The probe's best eye min is what the GPU gives it when free; a row
    // that was unsteady itself, or whose rounds' probe min was more than 5% above that,
    // shared the GPU with other work.
    double bestProbe = -1;
    for (const BenchRow& r : rows)
        if (r.probe.frames > 0 && (bestProbe < 0 || r.probe.pass[BENCH_EYE_PASS].min < bestProbe)) bestProbe = r.probe.pass[BENCH_EYE_PASS].min;
    size_t contendedRows = 0;
    for (BenchRow& r : rows)
    {
        const BenchStats& probeEye = r.probe.pass[BENCH_EYE_PASS];
        const bool probeSlow = r.probe.frames > 0 && probeEye.min > bestProbe * BENCH_PROBE_SLOWER + BENCH_PROBE_MS;
        r.contended = !r.times.steady || probeSlow;
        if (!r.contended) continue;
        contendedRows++;
        char own[40] = "", slow[64] = "";
        if (!r.times.steady) snprintf(own, sizeof(own), "p50 +%.0f%% on its min; ", std::max(r.times.eyeAbove, r.times.totalAbove));
        if (probeSlow) snprintf(slow, sizeof(slow), "probe min %.3f ms, +%.0f%% on its best; ", probeEye.min, 100.0 * (probeEye.min / bestProbe - 1.0));
        snprintf(r.why, sizeof(r.why), "%s%s", own, slow);
        const size_t len = strlen(r.why);
        if (len >= 2) r.why[len - 2] = 0;
        const BenchCase& bc = kBenchCases[r.caseIndex];
        char where[16];
        if (bc.curved) snprintf(where, sizeof(where), "%d%%", r.curvePct);
        else snprintf(where, sizeof(where), "flat");
        Log("BenchRoom: [%c %s yaw %.0f] run %d CONTENDED - %s", bc.id, where, kBenchYaws[r.view], r.run + 1, r.why);
    }
    if (!rows.empty() && contendedRows > 0)
        Log("BenchRoom: CONTENDED - %zu of %zu rows shared the GPU with other work%s: their eye or total p50 was more than %.0f%% above its min, or the probe's "
            "min in the same rounds was more than %.0f%% above its best (%.3f ms). Their mins are not reliable, and the acceptance below uses steady rows "
            "only. For a clean baseline, run with nothing else drawing on the GPU (no game; SteamVR closed or idle)",
            contendedRows, rows.size(), compositor ? " (SteamVR's compositor is running)" : "", (BENCH_STEADY - 1.0) * 100.0,
            (BENCH_PROBE_SLOWER - 1.0) * 100.0, bestProbe);
    else if (!rows.empty())
        Log("BenchRoom: steady - in every row the eye and total p50 are within %.0f%% of their min, and the probe's min within %.0f%% of its best (%.3f ms)",
            (BENCH_STEADY - 1.0) * 100.0, (BENCH_PROBE_SLOWER - 1.0) * 100.0, bestProbe);

    // --- room-bench.csv: one row per run, case, curve and view
    FILE* csv = nullptr;
    if (fopen_s(&csv, "room-bench.csv", "w") || !csv) { Log("BenchRoom: FAIL cannot write room-bench.csv"); ok = false; }
    else
    {
        fprintf(csv, "run,case,what,curve_pct,yaw_deg,pitch_deg,eye_w,eye_h,emitters,clocks,compositor,contended,why,probe_eye_min_ms,probe_eye_p50_ms,"
                     "screen_px,room_px,glow_px,world_px,mixed_px,check_px,check_bad_px,check_edge_px,check_edge_bad_px,check_interior_far_px");
        for (int p = 0; p < BENCH_PASSES; p++) fprintf(csv, ",%s_min_ms,%s_p50_ms,%s_p95_ms", kBenchPassNames[p], kBenchPassNames[p], kBenchPassNames[p]);
        fprintf(csv, ",eye_min_ms_per_M_offscreen_px,eye_p50_ms_per_M_offscreen_px\n");
        for (const BenchRow& r : rows)
        {
            const BenchCase& bc = kBenchCases[r.caseIndex];
            const double offscreenM = (double)r.classes.Offscreen() / 1e6;
            fprintf(csv, "%d,%c,\"%s\",%d,%.0f,%.0f,%d,%d,%d,%s,%s,%s,\"%s\",%.4f,%.4f,%llu,%llu,%llu,%llu,%llu,%zu,%zu,%zu,%zu,%zu", r.run + 1, bc.id, bc.what,
                    r.curvePct, kBenchYaws[r.view], kBenchPitch, r.ew, r.eh, r.emitters, stable ? "locked" : "unlocked", compositor ? "running" : "off",
                    r.contended ? "yes" : "no", r.why, r.probe.pass[BENCH_EYE_PASS].min, r.probe.pass[BENCH_EYE_PASS].p50,
                    (unsigned long long)r.classes.screen, (unsigned long long)r.classes.room, (unsigned long long)r.classes.glow,
                    (unsigned long long)r.classes.world, (unsigned long long)r.classes.mixed, r.check.checked, r.check.bad, r.check.edgeChecked,
                    r.check.edgeBad, r.check.interiorFar);
            for (int p = 0; p < BENCH_PASSES; p++) fprintf(csv, ",%.4f,%.4f,%.4f", r.times.pass[p].min, r.times.pass[p].p50, r.times.pass[p].p95);
            fprintf(csv, ",%.5f,%.5f\n", offscreenM > 0 ? r.times.pass[BENCH_EYE_PASS].min / offscreenM : 0.0,
                    offscreenM > 0 ? r.times.pass[BENCH_EYE_PASS].p50 / offscreenM : 0.0);
        }
        fclose(csv);
        Log("BenchRoom: wrote room-bench.csv (%zu rows, %zu contended)", rows.size(), contendedRows);
    }

    // --- summary: for each case, the median over the runs of each view's min and p50
    // among its steady rows, and their spread; then what the spec's acceptance compares,
    // among the cases that exist so far - judged only where every figure has a steady row.
    auto statOf = [&](char id, int curvePct, int view, int pass, bool useMin, double* spread)
    {
        std::vector<double> v;
        for (const BenchRow& r : rows)
            if (!r.contended && kBenchCases[r.caseIndex].id == id && r.curvePct == curvePct && r.view == view)
                v.push_back(useMin ? r.times.pass[pass].min : r.times.pass[pass].p50);
        if (spread) *spread = v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());
        if (v.empty()) return -1.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    if (ok)
    {
        for (int ci = 0; ci < BENCH_CASES; ci++)
        {
            const BenchCase& bc = kBenchCases[ci];
            if (!bc.ready) continue;
            for (int k = 0; k < (bc.curved ? BENCH_CURVES : 1); k++)
            {
                const int curvePct = bc.curved ? (int)std::lround(kBenchCurves[k] * 100.0f) : 0;
                char line[400] = "";
                size_t used = 0;
                double worstSpread = 0;
                for (int vi = 0; vi < BENCH_VIEWS && used < sizeof(line); vi++)
                {
                    double spreadMin = 0, spreadP50 = 0;
                    const double totalMin = statOf(bc.id, curvePct, vi, BENCH_TOTAL, true, &spreadMin);
                    const double totalP50 = statOf(bc.id, curvePct, vi, BENCH_TOTAL, false, &spreadP50);
                    const double eyeMin = statOf(bc.id, curvePct, vi, BENCH_EYE_PASS, true, nullptr);
                    worstSpread = std::max(worstSpread, std::max(spreadMin, spreadP50));
                    if (totalMin < 0) used += (size_t)snprintf(line + used, sizeof(line) - used, "%syaw %.0f CONTENDED", vi ? ", " : "", kBenchYaws[vi]);
                    else
                        used += (size_t)snprintf(line + used, sizeof(line) - used, "%syaw %.0f %.3f / %.3f (eye min %.3f)", vi ? ", " : "", kBenchYaws[vi],
                                                 totalMin, totalP50, eyeMin);
                }
                char where[16];
                if (bc.curved) snprintf(where, sizeof(where), "%d%%", curvePct);
                else snprintf(where, sizeof(where), "flat");
                Log("BenchRoom: summary [%c %s] %s - total min / p50 over the steady runs: %s ms; spread over %d run%s at most %.3f ms", bc.id, where, bc.what,
                    line, runs, runs > 1 ? "s" : "", worstSpread);
            }
        }
        for (int k = 0; k < BENCH_CURVES; k++)
        {
            const int curvePct = (int)std::lround(kBenchCurves[k] * 100.0f);
            for (int useMin = 1; useMin >= 0; useMin--)
            {
                const bool m = useMin != 0;
                const double a0 = statOf('A', curvePct, 0, BENCH_EYE_PASS, m, nullptr), b0 = statOf('B', curvePct, 0, BENCH_EYE_PASS, m, nullptr);
                const double a60 = statOf('A', curvePct, 2, BENCH_EYE_PASS, m, nullptr), b60 = statOf('B', curvePct, 2, BENCH_EYE_PASS, m, nullptr);
                const double emit = statOf('B', curvePct, 0, 0, m, nullptr), light = statOf('B', curvePct, 0, 1, m, nullptr);
                const double d0 = statOf('D', curvePct, 0, BENCH_TOTAL, m, nullptr), bt0 = statOf('B', curvePct, 0, BENCH_TOTAL, m, nullptr);
                const double d60 = statOf('D', curvePct, 2, BENCH_TOTAL, m, nullptr), bt60 = statOf('B', curvePct, 2, BENCH_TOTAL, m, nullptr);
                const double ct0 = statOf('C', curvePct, 0, BENCH_TOTAL, m, nullptr), ct60 = statOf('C', curvePct, 2, BENCH_TOTAL, m, nullptr);
                if (a0 < 0 || b0 < 0 || a60 < 0 || b60 < 0 || emit < 0 || light < 0)
                {
                    Log("BenchRoom: acceptance at %d%% (%s) not judged - CONTENDED: (A) and (B) at yaw 0 and 60 each need a steady row, and some have none",
                        curvePct, m ? "min" : "p50");
                    continue;
                }
                const double gap0 = b0 - a0, gap60 = b60 - a60, emitLight = emit + light;
                // Stage 1's eye pass (B) against the kept v10 one (D): B's total must be below D's.
                auto belowD = [](double b, double d, char* out, size_t size)
                {
                    if (!out || size == 0) return;
                    if (d < 0 || b < 0) snprintf(out, size, "no steady row");
                    else snprintf(out, size, "%+.3f ms (%s)", b - d, b < d ? "meets" : "misses");
                };
                char bVsD0[48], bVsD60[48], cVsD0[48], cVsD60[48];
                belowD(bt0, d0, bVsD0, sizeof(bVsD0));
                belowD(bt60, d60, bVsD60, sizeof(bVsD60));
                // Every v11 control on (C: glass, reflections and the room light) against v10 (D).
                belowD(ct0, d0, cVsD0, sizeof(cVsD0));
                belowD(ct60, d60, cVsD60, sizeof(cVsD60));
                Log("BenchRoom: acceptance at %d%% (%s) - (B) eye minus (A) eye: %+.3f ms at yaw 0 (%s), %+.3f ms at yaw 60 (%s), target at most +0.25; "
                    "EMIT + LIGHT %.3f ms (%s, target at most 0.15); (B) total minus (D) total, target below 0: %s at yaw 0, %s at yaw 60; "
                    "(C) total minus (D) total, target below 0: %s at yaw 0, %s at yaw 60", curvePct, m ? "min" : "p50", gap0, gap0 <= 0.25 ? "meets" : "misses",
                    gap60, gap60 <= 0.25 ? "meets" : "misses", emitLight, emitLight <= 0.15 ? "meets" : "misses", bVsD0, bVsD60, cVsD0, cVsD60);
            }
        }
        // What the v11 look costs: C (and F, flat) draw Glass 60, Reflections 40 and the room light
        // at 50%, B (and E) the same room without them. Information, not a target.
        auto lampCost = [&](char with, char without, int curvePct, bool useMin, char* out, size_t size)
        {
            if (!out || size == 0) return;
            size_t used = 0;
            out[0] = 0;
            for (int vi = 0; vi < BENCH_VIEWS && used < size; vi++)
            {
                const double eyeWith = statOf(with, curvePct, vi, BENCH_EYE_PASS, useMin, nullptr), eyeWithout = statOf(without, curvePct, vi, BENCH_EYE_PASS, useMin, nullptr);
                const double lmWith = statOf(with, curvePct, vi, 0, useMin, nullptr) + statOf(with, curvePct, vi, 1, useMin, nullptr);
                const double lmWithout = statOf(without, curvePct, vi, 0, useMin, nullptr) + statOf(without, curvePct, vi, 1, useMin, nullptr);
                const double totalWith = statOf(with, curvePct, vi, BENCH_TOTAL, useMin, nullptr), totalWithout = statOf(without, curvePct, vi, BENCH_TOTAL, useMin, nullptr);
                if (eyeWith < 0 || eyeWithout < 0 || totalWith < 0 || totalWithout < 0)
                    used += (size_t)snprintf(out + used, size - used, "%syaw %.0f no steady row", vi ? ", " : "", kBenchYaws[vi]);
                else
                    used += (size_t)snprintf(out + used, size - used, "%syaw %.0f eye %+.3f, emit + light %+.3f, total %+.3f", vi ? ", " : "", kBenchYaws[vi],
                                             eyeWith - eyeWithout, lmWith - lmWithout, totalWith - totalWithout);
            }
        };
        for (int useMin = 1; useMin >= 0; useMin--)
        {
            char line[512];
            for (int k = 0; k < BENCH_CURVES; k++)
            {
                const int curvePct = (int)std::lround(kBenchCurves[k] * 100.0f);
                lampCost('C', 'B', curvePct, useMin != 0, line, sizeof(line));
                Log("BenchRoom: glass, reflections and the room light at %d%% (%s), (C) minus (B) in ms: %s", curvePct, useMin ? "min" : "p50", line);
            }
            lampCost('F', 'E', 0, useMin != 0, line, sizeof(line));
            Log("BenchRoom: glass, reflections and the room light, flat (%s), (F) minus (E) in ms: %s", useMin ? "min" : "p50", line);
        }
    }
    power.Release();
    Log("BenchRoom: exit %s (%.1f s, %s clocks, %d run%s; %zu of %zu rows contended)", ok ? "PASS" : "FAIL", NowSeconds() - started,
        stable ? "locked" : "unlocked", runs, runs > 1 ? "s" : "", contendedRows, rows.size());
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
    double repCpuMs = 0, repAgeMs = 0, repShownAgeMs = 0;
    std::vector<unsigned char> scene;
    // "Delayed to depth": recent frames (oldest first) and the measured depth delay.
    std::deque<SourceRef> history;
    std::vector<TimedFrame> historyTimes;
    DepthDelayEstimate depthDelay;
    const DepthSlot* cur = nullptr;
    const bool useDepthSc = app.opt.submitDepth;
    WarpTarget& target = app.mainTarget;
    // Curved screen (screen_curve.h): rebuilt only when the screen itself changes.
    Cylinder cylinder;
    float curveWidth = -1, curveHeight = -1, curveDistance = -1, curveFraction = -1;
    bool headLockedNoticeLogged = false;
    // The room (room.h): rebuilt only when the screen, the recentre point, the curve or
    // the floor change; its light is worked out every frame.
    Room room;
    RoomEmitterLayout roomLayout;
    std::vector<RoomEmitter> roomGeometry;
    bool roomGeometryDirty = false, roomFailedLogged = false;
    bool roomRejected = false;                              // the last build failed: wait for its inputs to change
    float roomKey[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    double roomLastTime = -1;
    float stageFloorY = NAN;                                // the real floor, LOCAL y
    bool stageLocated = false;
    XrTime stageLocateFrom = 0;                             // a pending reference space change takes effect then
    std::vector<double> passGpuMs;                          // screen/room passes, since the last report
    std::vector<double> passSplitMs[TIME_SLOT - 1];         // the room's EMIT, LIGHT, eye pass and copy, likewise
    bool roomV10Logged = false;                             // --room-v10-eye: said once when the room first draws with it

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
                if (next.version >= 7 && app.opt.subpixel != (next.subpixel != 0))
                {
                    app.opt.subpixel = next.subpixel != 0;
                    Log("Stereo warp: %s", app.opt.subpixel ? "sub-pixel (smooth depth)" : "whole pixels");
                }
                if (next.version >= 6 && app.opt.delayed != (next.delayed != 0))
                {
                    app.opt.delayed = next.delayed != 0;
                    Log("Game frame timing: %s", FrameTimingName(app.opt.paired ? FrameTiming::Matched : app.opt.delayed ? FrameTiming::Delayed : FrameTiming::Latest));
                }
                if (next.version >= 8)
                {
                    const float wantCurve = next.curve * 0.01f;
                    if (app.opt.curve != wantCurve)
                    {
                        app.opt.curve = wantCurve;
                        Log("Screen curve: %d%%", next.curve);
                    }
                    if (app.opt.ambilight != (next.ambilight != 0))
                    {
                        app.opt.ambilight = next.ambilight != 0;
                        Log("Ambilight: %s", app.opt.ambilight ? "enabled" : "disabled");
                    }
                }
                if (next.version >= 10 && app.opt.room != next.room)
                {
                    app.opt.room = next.room;
                    Log("Room: %d%%", next.room);
                }
                if (next.version >= 9)
                {
                    if (app.opt.ambiStrength != next.ambiStrength)
                    {
                        app.opt.ambiStrength = next.ambiStrength;
                        Log("Ambilight strength: %d%%", next.ambiStrength);
                    }
                    if (app.opt.worldColor != (uint32_t)next.worldColor)
                    {
                        app.opt.worldColor = (uint32_t)next.worldColor;
                        Log("World colour: #%06X", app.opt.worldColor);
                    }
                }
                if (next.version >= 5)
                {
                    if (app.steadyEnabled.exchange(next.steady != 0) != (next.steady != 0))
                        Log("Steady depth: %s", next.steady ? "enabled" : "disabled");
                    if (app.fuseEnabled.exchange(next.fuse != 0) != (next.fuse != 0))
                        Log("Model fusion: %s", next.fuse ? "enabled" : "disabled");
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
            else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
            {
                // e.g. SteamVR's "reset seated position": LOCAL moves, so the floor's
                // height in it has to be looked up again, once the change has taken effect.
                const auto* rc = (const XrEventDataReferenceSpaceChangePending*)&ev;
                stageLocated = false;
                stageLocateFrom = rc->changeTime;
                const XrReferenceSpaceType changed = rc->referenceSpaceType;
                Log("RunFrameLoop: %s space change pending - the room's floor will be looked up again",
                    changed == XR_REFERENCE_SPACE_TYPE_LOCAL ? "LOCAL" : changed == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "reference");
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

        const XrCompositionLayerBaseHeader* layers[VIEWS + 2] = {};
        uint32_t layerCount = 0;
        XrCompositionLayerProjection worldProj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        XrCompositionLayerProjectionView worldViews[VIEWS];
        XrCompositionLayerProjection roomProj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        XrCompositionLayerProjectionView roomViews[VIEWS];
        XrCompositionLayerQuad quads[VIEWS] = {};
        XrCompositionLayerQuad glowQuad{};
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

            // The room needs the screen fixed in the room, and keeps it level.
            const bool followsHead = !app.opt.controlPath.empty() && desktop.follow;
            const bool roomWanted = app.opt.room > 0 && !app.opt.headLocked && !followsHead;
            screen.level = roomWanted;
            if (!app.opt.headLocked)
            {
                if (followsHead) screen.pending = true;
                const bool placed = screen.Place(views[0].pose, views[1].pose, sharedRot, tanHalfX, float(target.ch) / target.cw);
                if (placed && !desktop.follow)
                    Log("RunFrameLoop: screen recentered using the current headset direction");
                if (placed) stageLocated = false;
                if (!app.opt.controlPath.empty()) screen.Adjust(desktop.width, desktop.distance,
                    desktop.height, desktop.horizontal, float(target.ch) / target.cw);
                // Keep size and disparity calibration stable after placement.
                focalPx = target.cw * (app.opt.controlPath.empty() ? ScreenAnchor::distance : desktop.distance) / screen.size.width;
            }

            // The curve needs a screen in the room. The desktop's "Screen follows my head"
            // still has one (re-placed every frame), so it curves; only the --head-locked
            // diagnostic makes the picture the whole view, with nothing to curve.
            const float curveScreenDistance = app.opt.controlPath.empty() ? ScreenAnchor::distance : desktop.distance;
            const float wantCurve = app.opt.headLocked ? 0.0f : app.opt.curve;
            if (app.opt.headLocked) cylinder = Cylinder();
            else if (curveWidth != screen.size.width || curveHeight != screen.size.height ||
                     curveDistance != curveScreenDistance || curveFraction != wantCurve)
            {
                if (!BuildCylinder(screen.size.width, screen.size.height, curveScreenDistance, wantCurve, cylinder))
                {
                    Log("RunFrameLoop: curve rejected (%.2f x %.2f m at %.2f m, %.0f%%) - flat screen",
                        screen.size.width, screen.size.height, curveScreenDistance, wantCurve * 100.0);
                    cylinder = Cylinder();
                }
                else if (cylinder.curved)
                    Log("RunFrameLoop: screen curve %.0f%% - wrap %.1f deg, radius %.2f m, edges %.2f m nearer (%.2f x %.2f m at %.2f m)",
                        wantCurve * 100.0, cylinder.halfWrap * 2 * 57.2958f, cylinder.radius,
                        CurveSag(screen.size.width, cylinder.halfWrap * 2), screen.size.width, screen.size.height, curveScreenDistance);
                else if (curveFraction >= 0)
                    Log("RunFrameLoop: screen curve off (flat screen)");
                curveWidth = screen.size.width; curveHeight = screen.size.height;
                curveDistance = curveScreenDistance; curveFraction = wantCurve;
            }
            const bool curvedScreen = cylinder.curved && EnsureCurvedEyes(app);

            // The room: built in the screen's own (level) frame round the recentre point.
            if (roomWanted && !stageLocated && fs.predictedDisplayTime >= stageLocateFrom)
            {
                stageLocated = true;
                stageFloorY = NAN;
                if (app.stage != XR_NULL_HANDLE)
                {
                    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
                    if (XR_SUCCEEDED(xrLocateSpace_(app.stage, app.space, fs.predictedDisplayTime, &loc)) &&
                        (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
                        stageFloorY = loc.pose.position.y;
                }
                Log("RunFrameLoop: room floor %s", std::isfinite(stageFloorY) ? "from STAGE" : "not tracked - seated guess");
            }
            bool roomOn = false;
            if (roomWanted)
            {
                float S[3][3];
                QuatRows(screen.pose.orientation, S);
                const float rel[3] = { screen.origin.position.x - screen.pose.position.x, screen.origin.position.y - screen.pose.position.y,
                                       screen.origin.position.z - screen.pose.position.z };
                RoomInputs in;
                in.W = screen.size.width; in.H = screen.size.height; in.cyl = cylinder;
                for (int r = 0; r < 3; r++) in.eye[r] = S[0][r] * rel[0] + S[1][r] * rel[1] + S[2][r] * rel[2];
                in.floorY = std::isfinite(stageFloorY) ? stageFloorY - screen.pose.position.y : NAN;
                const float key[8] = { in.W, in.H, in.eye[0], in.eye[1], in.eye[2], std::isfinite(in.floorY) ? in.floorY : -999.0f,
                                       cylinder.curved ? cylinder.radius : 0.0f, (float)app.ambiH };
                bool changed = !room.valid && !roomRejected;
                for (int k = 0; k < 8; k++) if (std::fabs(key[k] - roomKey[k]) > 1e-4f) changed = true;
                if (changed)
                {
                    memcpy(roomKey, key, sizeof(key));
                    const float margin = kAmbiMargin * in.W;
                    // A failed build is not retried until its inputs change.
                    roomRejected = true;
                    if (!BuildRoom(in, room))
                    {
                        room = Room();
                        if (!roomFailedLogged) Log("RunFrameLoop: no room (viewer at %.2f, %.2f, %.2f m from the screen)", in.eye[0], in.eye[1], in.eye[2]);
                        roomFailedLogged = true;
                    }
                    else
                    {
                        roomLayout = RoomLayout(in.W, in.H, app.ambiW, app.ambiH);
                        if (roomLayout.count() == 0 ||
                            !BuildRoomEmitters(room, cylinder, in.W, in.H, 0.5f * in.W + margin, 0.5f * in.H + margin, roomLayout, roomGeometry))
                        {
                            room = Room();
                            if (!roomFailedLogged) Log("RunFrameLoop: no room - its emitters do not fit (glow %dx%d)", app.ambiW, app.ambiH);
                            roomFailedLogged = true;
                        }
                        else
                        {
                            roomRejected = false;
                            roomFailedLogged = false;
                            roomGeometryDirty = true;
                            // Say which floor won: SteamVR's (or the seated guess), or the screen's
                            // bottom edge when the screen reaches below it.
                            char floorWhy[128];
                            const float wantedDrop = in.eye[1] - room.floorWanted, drop = in.eye[1] - room.yF;
                            if (drop > wantedDrop + 0.01f)
                                snprintf(floorWhy, sizeof(floorWhy), "%s %.2f m below - lowered %.2f m to stay under the screen",
                                         room.floorTracked ? "SteamVR's floor is" : "the seated guess is", wantedDrop, drop - wantedDrop);
                            else
                                snprintf(floorWhy, sizeof(floorWhy), "%s", room.floorTracked ? "SteamVR's floor" : "seated guess");
                            Log("RunFrameLoop: room %.2f x %.2f x %.2f m, floor %.2f m below the eye (%s), %s front%s, %d emitters (glow blocks %d px)",
                                2 * room.X, room.yC - room.yF, room.zB + room.g, drop, floorWhy, room.curved ? "curved" : "flat",
                                room.phiReduced ? " (arc shortened to keep the viewer inside)" : "", roomLayout.count(), roomLayout.block);
                            if (room.lightValid)
                                Log("RunFrameLoop: room light's panel %.2f x %.2f m at x %.2f..%.2f, z %.2f..%.2f (the eye at %.2f, %.2f), %.2f m above the eye",
                                    room.lightX1 - room.lightX0, room.lightZ1 - room.lightZ0, room.lightX0, room.lightX1, room.lightZ0, room.lightZ1,
                                    in.eye[0], in.eye[2], room.yC - in.eye[1]);
                            else
                                Log("RunFrameLoop: the room has no space for the room light's panel (%.1f x %.1f m, %.1f m clear of the walls)",
                                    kRoomLightMinSide, kRoomLightMinSide, kRoomLightInset);
                            Log("RunFrameLoop: room frames (with glass) - side walls %d bays of %.3f m, back wall %d bays of %.3f m, %s",
                                (int)std::lround((room.zB - room.zSide) / room.pitchSide), room.pitchSide, (int)std::lround(2.0f * room.X / room.pitchBack),
                                room.pitchBack, room.transomY >= room.yF ? "a crossbar 2.4 m above the floor" : "no crossbar (the room is too low)");
                        }
                    }
                }
                roomOn = room.valid && EnsureCurvedEyes(app);
            }
            else
            {
                if (room.valid) Log("RunFrameLoop: room off");
                room = Room();
                roomKey[0] = -1;
                roomLastTime = -1;
                roomRejected = false;
                roomFailedLogged = false;
            }
            const bool roomFlat = roomOn && !curvedScreen;
            if (app.opt.headLocked && (app.opt.curve > 0 || app.opt.ambilight || app.opt.worldColor != 0) && !headLockedNoticeLogged)
            {
                headLockedNoticeLogged = true;
                Log("RunFrameLoop: --head-locked shows the picture as the whole view: screen curve, ambilight and world colour are ignored");
            }

            if (!printedView)
            {
                printedView = true;
                Log("RunFrameLoop: shared fov %.1f x %.1f deg  ipd %.1f mm  focal %.1f px  warp scale %.2f  (%.1f px/deg)",
                    (sharedFov.angleRight - sharedFov.angleLeft) * 57.2958f, (sharedFov.angleUp - sharedFov.angleDown) * 57.2958f,
                    ipd * 1000.0f, focalPx, app.opt.warpScale, target.cw / ((sharedFov.angleRight - sharedFov.angleLeft) * 57.2958f));
                // Each eye's own field of view, as the runtime gives it: what the curved
                // screen and the room draw with, and what --bench-fov takes (left eye).
                Log("RunFrameLoop: eye fov left/right/up/down - left eye %.2f / %.2f / %.2f / %.2f deg, right eye %.2f / %.2f / %.2f / %.2f deg "
                    "(--bench-fov=%.2f,%.2f,%.2f,%.2f)",
                    views[0].fov.angleLeft * 57.2958f, views[0].fov.angleRight * 57.2958f, views[0].fov.angleUp * 57.2958f, views[0].fov.angleDown * 57.2958f,
                    views[1].fov.angleLeft * 57.2958f, views[1].fov.angleRight * 57.2958f, views[1].fov.angleUp * 57.2958f, views[1].fov.angleDown * 57.2958f,
                    views[0].fov.angleLeft * 57.2958f, views[0].fov.angleRight * 57.2958f, views[0].fov.angleUp * 57.2958f, views[0].fov.angleDown * 57.2958f);
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

            // A curved screen is drawn into its own eye buffers; a flat one is the
            // warped pair itself, placed by the compositor as quads.
            // With the room on, a flat screen stays the compositor's quads and the room is
            // a projection layer under them, drawn at half size into the eye buffers.
            XrReadyImage colorImage, depthImage, eyeImage;
            const bool needEyes = curvedScreen || roomFlat;
            bool ok = true;
            if (!curvedScreen) ok = colorImage.Acquire(app.colorSc, xrAcquireImage_, xrWaitImage_);
            if (ok && needEyes) ok = eyeImage.Acquire(app.eyeSc, xrAcquireImage_, xrWaitImage_);
            if (ok && useDepthSc && !curvedScreen) ok = depthImage.Acquire(app.depthSc, xrAcquireImage_, xrWaitImage_);
            if (!ok) { Log("RunFrameLoop: swapchain acquire/wait failed (%d, %d, %d)",
                (int)colorImage.result, (int)depthImage.result, (int)eyeImage.result); app.stop = true; }
            // The glow: its own layer behind a flat screen, or drawn into the eye
            // buffers behind a curved one.
            XrReadyImage glowImage;
            const bool glowWanted = app.opt.ambilight && !app.opt.headLocked;
            const bool glowLayer = glowWanted && !curvedScreen && !roomOn && app.ambiSc != XR_NULL_HANDLE;
            if (ok && glowLayer && !glowImage.Acquire(app.ambiSc, xrAcquireImage_, xrWaitImage_))
                Log("RunFrameLoop: ambilight image acquire failed (%d) - no glow this frame", (int)glowImage.result);
            if (!glowWanted) app.ambiHistory = false;
            // The world colour: behind a flat screen its own layer (none for black).
            XrReadyImage worldImage;
            const bool worldLayer = app.opt.worldColor != 0 && !app.opt.headLocked && !curvedScreen && !roomOn && app.worldSc != XR_NULL_HANDLE;
            if (ok && worldLayer && !worldImage.Acquire(app.worldSc, xrAcquireImage_, xrWaitImage_))
                Log("RunFrameLoop: world colour image acquire failed (%d) - black this frame", (int)worldImage.result);
            bool drewWorld = false;
            const uint32_t cIdx = colorImage.index, dIdx = depthImage.index;
            bool drewSource = false, stereo = false, drewGlow = false, drewRoom = false;
            float glowRectW = 0, glowRectH = 0;

            if (ok)
            {
                const int ring = (int)(drawn % RING);
                WaitFence(app, app.frameFence[ring]);
                if (app.timeCount[ring] && app.timeMapped)
                {
                    const UINT64* ts = app.timeMapped + TIME_SLOT * ring;
                    const double toMs = 1000.0 / app.timeFreq;
                    if (app.timeCount[ring] == TIME_SLOT)
                    {
                        // The room: the whole, and each pass (EMIT, LIGHT, eye, copy).
                        bool ordered = ts[TIME_SLOT - 1] > ts[0];
                        for (UINT k = 1; k < TIME_SLOT; k++) ordered = ordered && ts[k] >= ts[k - 1];
                        if (ordered)
                        {
                            passGpuMs.push_back((double)(ts[TIME_SLOT - 1] - ts[0]) * toMs);
                            for (UINT k = 1; k < TIME_SLOT; k++) passSplitMs[k - 1].push_back((double)(ts[k] - ts[k - 1]) * toMs);
                        }
                    }
                    else if (ts[1] > ts[0]) passGpuMs.push_back((double)(ts[1] - ts[0]) * toMs);
                    app.timeCount[ring] = 0;
                }
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
                    depthDelay.Update(cur->completeTime - cur->sceneTime);
                }

                // --- game frame timing (frame_timing.h)
                const FrameTiming timing = app.opt.paired ? FrameTiming::Matched : app.opt.delayed ? FrameTiming::Delayed : FrameTiming::Latest;
                if (timing == FrameTiming::Delayed && source && !newSrc)
                {
                    if (!history.empty() && history.back()->layout != source->layout) history.clear();
                    if (history.empty() || history.back()->seq != source->seq) history.push_back(source);
                    auto times = [&]()
                    {
                        historyTimes.clear();
                        for (const auto& f : history) historyTimes.push_back({ f->seq, f->time });
                    };
                    times();
                    for (size_t drop = DelayedHistoryExcess(historyTimes, t, depthDelay.value, DELAYED_HISTORY); drop > 0; drop--) history.pop_front();
                    times();
                    const int pick = PickDelayedFrame(historyTimes, t, depthDelay.value);
                    if (pick >= 0) source = history[pick];
                    // Never older than the depth: then show the depth's own frame (as matched).
                    if (cur && cur->source && source->layout == cur->source->layout && source->seq < cur->source->seq) source = cur->source;
                }
                else if (!history.empty())
                {
                    history.clear();
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
                    c.subpixel = app.opt.subpixel ? 1u : 0u;
                    c.indicator = (app.opt.abSeconds > 0.0) ? (depthOn ? 1u : 2u) : 0u;
                    c.depthLie = app.opt.depthLie ? 1u : 0u;
                    c.writeDepth = useDepthSc && stereo ? 1u : 0u;
                    c.exactLoad = (app.srcW == target.cw && app.srcH == target.ch) ? 1u : 0u;
                    c.fillMode = (uint32_t)app.opt.fillMode;
                    c.mirrorTol = MIRROR_TOL;
                    RecordWarp(app, target, DESC_SRC0 + (UINT)source->index, c);
                    if (!curvedScreen)
                        RecordCopyToSwapchain(app, target, app.cimgs[cIdx].texture, useDepthSc ? app.dimgs[dIdx].texture : nullptr);
                    drewSource = true;

                    if (glowWanted)
                    {
                        const AmbiConstants a = MakeAmbiConstants(app, app.srcW, app.srcH, screen.size.width, screen.size.height,
                                                                  app.opt.ambiStrength / 100.0f, !app.ambiHistory, LinearBlend(app));
                        const bool toLayer = glowLayer && glowImage.ready;
                        RecordAmbilight(app, DESC_SRC0 + (UINT)source->index, a, toLayer ? app.aimgs[glowImage.index].texture : nullptr);
                        app.ambiHistory = true;
                        drewGlow = toLayer;
                        glowRectW = a.rectW; glowRectH = a.rectH;
                    }
                    if (worldLayer && worldImage.ready)
                    {
                        RecordWorldColour(app, ring, app.opt.worldColor, app.wimgs[worldImage.index].texture);
                        drewWorld = true;
                    }
                    const bool timed = app.timeHeap && (curvedScreen || roomOn) && needEyes && eyeImage.ready;
                    // The room writes five timestamps (T0 here, T1-T4 in its passes); the
                    // curved screen alone writes today's two (T0 here, T1 at the end).
                    const UINT timeBase = TIME_SLOT * (UINT)ring;
                    const bool splitTimes = timed && roomOn;
                    bool splitDone = splitTimes;
                    if (timed) app.cmdList->EndQuery(app.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timeBase);
                    D3D12_GPU_VIRTUAL_ADDRESS roomCb = 0;
                    ID3D12PipelineState* roomEyePso = nullptr;
                    if (roomOn && eyeImage.ready)
                    {
                        const double tn = NowSeconds();
                        const bool reset = roomGeometryDirty || roomLastTime < 0;
                        const double dt = std::min(std::max(tn - roomLastTime, 0.0), 0.25);
                        const float alpha = reset ? 1.0f : (float)(1.0 - std::exp(-dt / kRoomLightTau));
                        roomLastTime = tn;
                        const float margin = kAmbiMargin * screen.size.width;
                        RoomView view;
                        view.flatLayer = roomFlat; view.W = screen.size.width; view.H = screen.size.height;
                        view.glowOn = glowWanted;
                        view.glowHalfW = 0.5f * screen.size.width + margin; view.glowHalfH = 0.5f * screen.size.height + margin;
                        view.dither = true;
                        view.cyl = roomFlat ? Cylinder() : cylinder;
                        RoomLook look;
                        look.glass = app.opt.roomGlass; look.reflect = app.opt.roomReflect; look.light = app.opt.roomLight;
                        look.lightRgb = app.opt.roomLightColor;
                        const RoomShading shading = MakeRoomShading(room, app.opt.room, app.opt.worldColor, screen.size.width * screen.size.height, look);
                        const RoomConstants rc = MakeRoomConstants(room, shading, roomLayout, view, app.srcW, app.srcH, alpha);
                        memcpy(app.roomCbMapped + (size_t)ring * sizeof(RoomConstants), &rc, sizeof(rc));
                        roomCb = app.roomCbUp->GetGPUVirtualAddress() + (UINT64)ring * sizeof(RoomConstants);
                        ID3D12Resource* geometry = nullptr;
                        if (roomGeometryDirty)
                        {
                            memcpy(app.roomGeomMapped[ring], roomGeometry.data(), roomGeometry.size() * sizeof(RoomEmitter));
                            geometry = app.roomGeomUp[ring].Get();
                            roomGeometryDirty = false;
                        }
                        if (!RecordRoomLight(app, DESC_SRC0 + (UINT)source->index, roomCb, geometry, (UINT)roomLayout.count(), rc.mirrorH,
                                             splitTimes ? timeBase : UINT_MAX))
                            splitDone = false;
                        roomEyePso = RoomEyePso(app, rc);
                    }
                    if (needEyes && eyeImage.ready)
                    {
                        XrPosef eyePose[VIEWS] = { views[0].pose, views[1].pose };
                        XrFovf eyeFov[VIEWS] = { views[0].fov, views[1].fov };
                        const int ew = roomFlat ? app.eyeW / 2 : app.eyeW, eh = roomFlat ? app.eyeH / 2 : app.eyeH;
                        const CurveConstants cc = MakeCurveConstants(roomFlat ? Cylinder() : cylinder, screen.pose, eyePose, eyeFov, ew, eh,
                                                                     glowWanted, glowRectW, glowRectH, app.opt.worldColor, LinearBlend(app));
                        const bool v10Eye = roomCb != 0 && app.opt.roomV10Eye;
                        if (v10Eye && !roomV10Logged)
                        {
                            roomV10Logged = true;
                            Log("RunFrameLoop: the room is drawn with the kept v10 eye pass (--room-v10-eye)%s",
                                app.opt.roomLight > 0 || app.opt.roomGlass > 0 || app.opt.roomReflect > 0
                                    ? " - it ignores the room light's panel, the glass and the reflections (the light is still in the lightmap)" : "");
                        }
                        if (!RecordCurvedScreen(app, DESC_CURVE_MAIN, target.colorOut.Get(), cc, app.eyeOut.Get(), app.eimgs[eyeImage.index].texture, roomCb,
                                                splitTimes ? timeBase : UINT_MAX, nullptr, v10Eye ? app.curveRoomV10Pso.Get() : roomEyePso))
                            splitDone = false;
                        drewRoom = roomFlat;
                    }
                    if (timed)
                    {
                        // A room frame whose passes could not all run falls back to begin
                        // and end, like the curved screen alone.
                        const UINT count = splitDone ? TIME_SLOT : 2;
                        if (!splitDone) app.cmdList->EndQuery(app.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timeBase + 1);
                        app.cmdList->ResolveQueryData(app.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timeBase, count, app.timeReadback.Get(),
                                                      (UINT64)timeBase * sizeof(UINT64));
                        app.timeCount[ring] = count;
                    }
                    RecordWarpOutputsBackToUav(app, target);
                }
                app.cmdList->Close();
                UINT64 fv = SubmitAndSignal(app);
                if (drewSource) app.sources.MarkRead(source, SourceFrames::Reader::Graphics, fv);
                app.frameFence[ring] = fv;
                if (tookSlot) app.slotFence = fv;
                if (newSrc) app.sources.Publish(newSrc, fv, t);

                if (drewSource) { drawn++; repDrawn++; if (source) repShownAgeMs += (t - source->time) * 1000.0; }
                if (cur) repAgeMs += (NowSeconds() - cur->completeTime) * 1000.0;
            }
            const bool releasedColor = colorImage.Release(xrReleaseImage_);
            const bool releasedDepth = depthImage.Release(xrReleaseImage_);
            const bool releasedEyes = eyeImage.Release(xrReleaseImage_);
            const bool releasedGlow = glowImage.Release(xrReleaseImage_);
            if (!worldImage.Release(xrReleaseImage_)) { Log("RunFrameLoop: world colour release failed - black this frame"); drewWorld = false; }
            if (!releasedColor || !releasedDepth || !releasedEyes) { Log("RunFrameLoop: swapchain release failed"); ok = false; app.stop = true; }
            if (!releasedGlow) { Log("RunFrameLoop: ambilight release failed - no glow this frame"); drewGlow = false; }

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
                if (app.opt.headLocked) layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
                else if (curvedScreen)
                {
                    // Each eye as it really was when the pass drew it, so the
                    // compositor reprojects it correctly; the glow is already in it.
                    for (uint32_t e = 0; e < VIEWS; e++)
                    {
                        pviews[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                        pviews[e].pose = views[e].pose;
                        pviews[e].fov = views[e].fov;
                        pviews[e].subImage.swapchain = app.eyeSc;
                        pviews[e].subImage.imageRect = { {0, 0}, {app.eyeW, app.eyeH} };
                        pviews[e].subImage.imageArrayIndex = e;
                    }
                    layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
                }
                else
                {
                    // The room (or else the world colour and the glow) first, then the
                    // screen: layers composite in submission order. The glow also sits a
                    // little behind, for any compositor that sorts layers by distance.
                    if (drewRoom)
                    {
                        for (uint32_t e = 0; e < VIEWS; e++)
                        {
                            roomViews[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                            roomViews[e].pose = views[e].pose;
                            roomViews[e].fov = views[e].fov;
                            roomViews[e].subImage.swapchain = app.eyeSc;
                            roomViews[e].subImage.imageRect = { {0, 0}, {app.eyeW / 2, app.eyeH / 2} };
                            roomViews[e].subImage.imageArrayIndex = e;
                        }
                        roomProj.space = app.space;
                        roomProj.viewCount = VIEWS;
                        roomProj.views = roomViews;
                        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&roomProj;
                    }
                    if (drewWorld)
                    {
                        for (uint32_t e = 0; e < VIEWS; e++)
                        {
                            worldViews[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                            worldViews[e].pose = views[e].pose;
                            worldViews[e].fov = views[e].fov;
                            worldViews[e].subImage.swapchain = app.worldSc;
                            worldViews[e].subImage.imageRect = { {0, 0}, {WORLD_W, WORLD_W} };
                            worldViews[e].subImage.imageArrayIndex = e;
                        }
                        worldProj.space = app.space;
                        worldProj.viewCount = VIEWS;
                        worldProj.views = worldViews;
                        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&worldProj;
                    }
                    if (drewGlow)
                    {
                        glowQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
                        glowQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        glowQuad.space = app.space;
                        glowQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        glowQuad.subImage.swapchain = app.ambiSc;
                        glowQuad.subImage.imageRect = { {0, 0}, {app.ambiW, app.ambiH} };
                        glowQuad.subImage.imageArrayIndex = 0;
                        float S[3][3];
                        QuatRows(screen.pose.orientation, S);
                        glowQuad.pose = screen.pose;
                        glowQuad.pose.position.x -= kAmbiBehind * S[0][2];
                        glowQuad.pose.position.y -= kAmbiBehind * S[1][2];
                        glowQuad.pose.position.z -= kAmbiBehind * S[2][2];
                        glowQuad.size = { glowRectW, glowRectH };
                        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&glowQuad;
                    }
                    for (uint32_t e = 0; e < VIEWS; ++e)
                    {
                        quads[e] = { XR_TYPE_COMPOSITION_LAYER_QUAD };
                        quads[e].space = app.space;
                        quads[e].eyeVisibility = e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                        quads[e].subImage = pviews[e].subImage;
                        quads[e].pose = screen.pose;
                        quads[e].size = screen.size;
                        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&quads[e];
                    }
                }
            }
            repCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
            } // valid tracking; otherwise xrEndFrame below submits zero layers
        }

        XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = layerCount;
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
            Log("render %.1f fps (drawn %.1f, cpu %.2f ms/frame) | capture %.1f fps | depth %.1f updates/s, model %.1f ms, age %.1f ms, range %.2f..%.2f | %s: game frame age %.1f ms, depth delay %.1f ms%s",
                repFrames / dt, repDrawn / dt, repDrawn ? repCpuMs / repDrawn : 0.0,
                (capFrames - lastCapFrames) / dt,
                repDepth / dt, cur ? cur->modelMs : 0.0, repDrawn ? repAgeMs / repDrawn : 0.0,
                cur ? cur->lo : 0.f, cur ? cur->hi : 0.f,
                FrameTimingName(app.opt.paired ? FrameTiming::Matched : app.opt.delayed ? FrameTiming::Delayed : FrameTiming::Latest),
                repDrawn ? repShownAgeMs / repDrawn : 0.0, depthDelay.value * 1000.0,
                app.opt.useTruth ? " [warp uses TRUTH]" : "");
            if (app.opt.source == SourceKind::Synthetic && cur)
                Log("       model near: back %.2f panel %.2f marker %.2f", cur->back, cur->panel, cur->marker);
            if (!passGpuMs.empty())
            {
                std::sort(passGpuMs.begin(), passGpuMs.end());
                // The room's frames also give each pass's p50 (a report can mix them with
                // frames of the curved screen alone, if the room was just switched).
                char split[128] = "";
                if (!passSplitMs[0].empty())
                {
                    double p50[TIME_SLOT - 1] = {};
                    for (UINT k = 0; k < TIME_SLOT - 1; k++)
                    {
                        std::sort(passSplitMs[k].begin(), passSplitMs[k].end());
                        p50[k] = passSplitMs[k][passSplitMs[k].size() / 2];
                    }
                    snprintf(split, sizeof(split), ": emit %.2f, light %.2f, eye %.2f, copy %.2f ms p50", p50[0], p50[1], p50[2], p50[3]);
                }
                Log("       %s GPU %.2f ms p50, %.2f ms p95 (%zu frames)%s", room.valid ? "room + screen" : "curved screen",
                    passGpuMs[passGpuMs.size() / 2], passGpuMs[std::min(passGpuMs.size() - 1, passGpuMs.size() * 95 / 100)], passGpuMs.size(), split);
                passGpuMs.clear();
                for (auto& v : passSplitMs) v.clear();
            }
            lastReport = now; repFrames = repDrawn = repDepth = 0; repCpuMs = repAgeMs = repShownAgeMs = 0;
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
    c.subpixel = app.opt.subpixel ? 1u : 0u;
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
    app.anchorCv.notify_all();
    if (app.anchorThread.joinable()) app.anchorThread.join();
    app.motion.Release();
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
    if (app.eyeSc) xrDestroySwapchain_(app.eyeSc);
    if (app.worldSc) xrDestroySwapchain_(app.worldSc);
    if (app.ambiSc) xrDestroySwapchain_(app.ambiSc);
    if (app.depthSc) xrDestroySwapchain_(app.depthSc);
    if (app.session) xrDestroySession_(app.session);
    if (app.instance) xrDestroyInstance_(app.instance);
    if (app.fenceEvent) CloseHandle(app.fenceEvent);
    if (app.mlFenceEvent) CloseHandle(app.mlFenceEvent);
    if (app.xferQueue || app.xferFenceEvent) ReleaseFrameTransfer(app);
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
    if (app.opt.listGpus) return ListGpus();
    app.testDepthFailuresLeft = app.opt.testDepthFailures;
    app.foregroundEnabled = app.opt.foreground && app.opt.doWarp;
    app.steadyEnabled = app.opt.steady;
    app.fuseEnabled = app.opt.fuse;

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
        if (initial.version >= 6) app.opt.delayed = initial.delayed != 0;
        if (initial.version >= 7) app.opt.subpixel = initial.subpixel != 0;
        if (initial.version >= 8)
        {
            app.opt.curve = initial.curve * 0.01f;
            app.opt.ambilight = initial.ambilight != 0;
        }
        if (initial.version >= 10) app.opt.room = initial.room;
        if (initial.version >= 9)
        {
            app.opt.ambiStrength = initial.ambiStrength;
            app.opt.worldColor = (uint32_t)initial.worldColor;
        }
        if (initial.version >= 5)
        {
            app.steadyEnabled = initial.steady != 0;
            app.fuseEnabled = initial.fuse != 0;
            Log("Desktop control: steady depth %s, model fusion %s", initial.steady ? "on" : "off", initial.fuse ? "on" : "off");
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
        if (app.opt.selfTestOnly)
        {
            const bool fg = SelfTestForegroundModel(app);
            bool passed = SelfTestPipeline(app) && fg;
            if (app.opt.benchRoom && !passed) Log("main: --bench-room skipped - the self-test failed, so its passes are not worth timing");
            if (app.opt.benchRoom && passed) passed = BenchRoom(app);
            if (passed) rc = 0;
            break;
        }

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
