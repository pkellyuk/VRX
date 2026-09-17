// M3: AI depth into OpenXR, as a projection layer with depth attached.
//
// Why a projection layer: XrCompositionLayerDepthInfoKHR extends
// XrCompositionLayerProjectionView - a quad layer cannot carry depth.
//
// Pipeline per frame:
//   1. synthesise a scene (686x392) with obvious depth structure
//   2. feed it to Depth-Anything-V2 (392x686) on the SAME D3D12 device as the
//      OpenXR session, via a zero-copy GPU input buffer
//   3. normalise the model's relative depth, write it into a D32_FLOAT depth
//      swapchain
//   4. warp the colour per eye with a horizontal offset proportional to depth
//      (a CPU implementation of the stereo warp, so there is a visible 2D->3D
//      effect to check; a shader is the performant follow-up)
//   5. submit a projection layer with XrCompositionLayerDepthInfoKHR per view
//
// usage: xrapp3 [seconds] [--no-warp]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// The model's fixed geometry, and therefore the render size too: a projection
// view's colour and depth sub-image rects must match, so matching the model
// output avoids a resize shader entirely.
static const int W = 686, H = 392;
static const UINT32 ROW_BYTES = (UINT32)W * 4;          // 2744
static const UINT32 ROW_PITCH = 2816;                   // <= must be 256-aligned for buffer->texture copy

static const OrtApi* ort = nullptr;

static int Fail(const char* what, OrtStatus* st)
{
    printf("FAIL: %s -> %s\n", what, st ? ort->GetErrorMessage(st) : "(null)");
    return 1;
}

static const char* XRStr(XrResult r)
{
    switch (r)
    {
    case XR_SUCCESS: return "XR_SUCCESS";
    case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
    case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
    case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
    case XR_ERROR_PATH_UNSUPPORTED: return "XR_ERROR_PATH_UNSUPPORTED";
    case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
    case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
    default: return "(other)";
    }
}

// ------------------------------------------------------------------ xr glue
static PFN_xrGetInstanceProcAddr g_getProc = nullptr;

#define XRFN(t, n) static t n = nullptr
XRFN(PFN_xrEnumerateInstanceExtensionProperties, xrEnumExt_);
XRFN(PFN_xrCreateInstance, xrCreateInstance_);
XRFN(PFN_xrDestroyInstance, xrDestroyInstance_);
XRFN(PFN_xrGetSystem, xrGetSystem_);
XRFN(PFN_xrCreateSession, xrCreateSession_);
XRFN(PFN_xrDestroySession, xrDestroySession_);
XRFN(PFN_xrBeginSession, xrBeginSession_);
XRFN(PFN_xrEndSession, xrEndSession_);
XRFN(PFN_xrPollEvent, xrPollEvent_);
XRFN(PFN_xrCreateReferenceSpace, xrCreateReferenceSpace_);
XRFN(PFN_xrLocateViews, xrLocateViews_);
XRFN(PFN_xrEnumerateViewConfigurationViews, xrEnumViewConfigs_);
XRFN(PFN_xrEnumerateSwapchainFormats, xrEnumFormats_);
XRFN(PFN_xrCreateSwapchain, xrCreateSwapchain_);
XRFN(PFN_xrDestroySwapchain, xrDestroySwapchain_);
XRFN(PFN_xrEnumerateSwapchainImages, xrEnumImages_);
XRFN(PFN_xrAcquireSwapchainImage, xrAcquireImage_);
XRFN(PFN_xrWaitSwapchainImage, xrWaitImage_);
XRFN(PFN_xrReleaseSwapchainImage, xrReleaseImage_);
XRFN(PFN_xrWaitFrame, xrWaitFrame_);
XRFN(PFN_xrBeginFrame, xrBeginFrame_);
XRFN(PFN_xrEndFrame, xrEndFrame_);
XRFN(PFN_xrGetD3D12GraphicsRequirementsKHR, xrD3D12Reqs_);

static bool ResolveFns(XrInstance inst)
{
    const char* names[] = {
        "xrDestroyInstance", "xrGetSystem", "xrCreateSession", "xrDestroySession",
        "xrBeginSession", "xrEndSession", "xrPollEvent", "xrCreateReferenceSpace",
        "xrLocateViews", "xrEnumerateViewConfigurationViews", "xrEnumerateSwapchainFormats",
        "xrCreateSwapchain", "xrDestroySwapchain", "xrEnumerateSwapchainImages",
        "xrAcquireSwapchainImage", "xrWaitSwapchainImage", "xrReleaseSwapchainImage",
        "xrWaitFrame", "xrBeginFrame", "xrEndFrame", "xrGetD3D12GraphicsRequirementsKHR",
    };
    PFN_xrVoidFunction* slots[] = {
        (PFN_xrVoidFunction*)&xrDestroyInstance_, (PFN_xrVoidFunction*)&xrGetSystem_,
        (PFN_xrVoidFunction*)&xrCreateSession_, (PFN_xrVoidFunction*)&xrDestroySession_,
        (PFN_xrVoidFunction*)&xrBeginSession_, (PFN_xrVoidFunction*)&xrEndSession_,
        (PFN_xrVoidFunction*)&xrPollEvent_, (PFN_xrVoidFunction*)&xrCreateReferenceSpace_,
        (PFN_xrVoidFunction*)&xrLocateViews_, (PFN_xrVoidFunction*)&xrEnumViewConfigs_,
        (PFN_xrVoidFunction*)&xrEnumFormats_, (PFN_xrVoidFunction*)&xrCreateSwapchain_,
        (PFN_xrVoidFunction*)&xrDestroySwapchain_, (PFN_xrVoidFunction*)&xrEnumImages_,
        (PFN_xrVoidFunction*)&xrAcquireImage_, (PFN_xrVoidFunction*)&xrWaitImage_,
        (PFN_xrVoidFunction*)&xrReleaseImage_, (PFN_xrVoidFunction*)&xrWaitFrame_,
        (PFN_xrVoidFunction*)&xrBeginFrame_, (PFN_xrVoidFunction*)&xrEndFrame_,
        (PFN_xrVoidFunction*)&xrD3D12Reqs_,
    };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
    {
        if (XR_FAILED(g_getProc(inst, names[i], slots[i])) || *slots[i] == nullptr)
        { printf("FAIL: resolve %s\n", names[i]); return false; }
    }
    return true;
}

// ------------------------------------------------------------------- scene
// A synthetic scene with unambiguous depth structure: far gradient backdrop,
// a mid-depth panel, and a near marker that moves so the parallax is obvious.
static void MakeScene(std::vector<unsigned char>& rgb, double t)
{
    rgb.resize((size_t)W * H * 3);
    int markerX = (int)((0.5 + 0.35 * sin(t * 0.8)) * (W - 120));

    for (int y = 0; y < H; y++)
    {
        for (int x = 0; x < W; x++)
        {
            unsigned char r, g, b;
            // backdrop: dim blue-grey gradient (far)
            float k = 0.35f + 0.25f * ((float)x / W);
            r = (unsigned char)(60 * k); g = (unsigned char)(70 * k); b = (unsigned char)(110 * k);

            // mid panel
            if (x > W * 0.18 && x < W * 0.52 && y > H * 0.22 && y < H * 0.78)
            { r = 190; g = 150; b = 60; }

            // near marker (moves)
            if (x >= markerX && x < markerX + 120 && y > H * 0.35 && y < H * 0.65)
            { r = 235; g = 235; b = 235; }

            size_t i = ((size_t)y * W + x) * 3;
            rgb[i + 0] = r; rgb[i + 1] = g; rgb[i + 2] = b;
        }
    }
}

// Ground-truth "nearness" (0 = far, 1 = near) for MakeScene. Flat-coloured
// rectangles carry no monocular depth cues, so the model's answer for that scene
// is arbitrary; --truth substitutes this so the warp can be judged on its own.
static void MakeTruth(std::vector<float>& near01, double t)
{
    near01.assign((size_t)W * H, 0.0f);
    int markerX = (int)((0.5 + 0.35 * sin(t * 0.8)) * (W - 120));

    for (int y = 0; y < H; y++)
    {
        for (int x = 0; x < W; x++)
        {
            float n = 0.0f;
            if (x > W * 0.18 && x < W * 0.52 && y > H * 0.22 && y < H * 0.78) n = 0.5f;
            if (x >= markerX && x < markerX + 120 && y > H * 0.35 && y < H * 0.65) n = 1.0f;
            near01[(size_t)y * W + x] = n;
        }
    }
}

// Mean nearness inside each region of MakeScene - tells us in the log what the
// model actually thinks of the synthetic scene.
static void RegionMeans(const std::vector<float>& near01, double t, float& back, float& panel, float& marker)
{
    std::vector<float> truth;
    MakeTruth(truth, t);
    double s[3] = { 0, 0, 0 }; size_t n[3] = { 0, 0, 0 };
    for (size_t i = 0; i < truth.size(); i++)
    {
        int k = truth[i] > 0.75f ? 2 : (truth[i] > 0.25f ? 1 : 0);
        s[k] += near01[i]; n[k]++;
    }
    back = n[0] ? (float)(s[0] / n[0]) : 0;
    panel = n[1] ? (float)(s[1] / n[1]) : 0;
    marker = n[2] ? (float)(s[2] / n[2]) : 0;
}

// Load any WIC-decodable image (png/jpg/bmp), scaled to WxH, as packed RGB.
static bool LoadImageWIC(const wchar_t* path, std::vector<unsigned char>& rgb)
{
    if (!path) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler))) return false;
    if (FAILED(scaler->Initialize(frame.Get(), W, H, WICBitmapInterpolationModeFant))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    rgb.resize((size_t)W * H * 3);
    return SUCCEEDED(conv->CopyPixels(nullptr, W * 3, (UINT)rgb.size(), rgb.data()));
}

static void WritePNM(const char* path, const char* magic, const unsigned char* data, size_t bytes)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) { printf("dump: cannot write %s\n", path); return; }
    fprintf(f, "%s\n%d %d\n255\n", magic, W, H);
    fwrite(data, 1, bytes, f);
    fclose(f);
    printf("dump: wrote %s\n", path);
}

// Forward stereo warp for one eye. Every SOURCE pixel is moved by its OWN
// disparity and z-tested at the destination, so near content really shifts and
// occludes what is behind it. (The earlier backward warp looked up disparity at
// the destination pixel, which merely trims the edges of a flat-coloured object
// instead of moving it - and lets far content overwrite near content.)
// Disocclusion holes are filled from the FARTHER neighbour, i.e. background.
//   eyeOffset : eye's lateral offset in metres (left negative)
//   outRGBA   : ROW_PITCH bytes per row;  outDepth : ROW_PITCH/4 floats per row,
//               written as 0 = near .. 1 = far
static void WarpEye(const std::vector<unsigned char>& scene, const std::vector<float>& near01,
                    float eyeOffset, float focalPx, float scale, float invZNear, float invZFar,
                    bool doWarp, unsigned char* outRGBA, float* outDepth)
{
    std::vector<int> src(W);
    for (int y = 0; y < H; y++)
    {
        const float* nrow = near01.data() + (size_t)y * W;
        std::fill(src.begin(), src.end(), -1);

        for (int x = 0; x < W; x++)
        {
            int dx = x;
            if (doWarp)
            {
                // Content at distance Z sits at -focal*E/Z in the eye's image
                // relative to the cyclopean image (left eye: shifted right).
                float invZ = invZFar + nrow[x] * (invZNear - invZFar);
                dx = x - (int)lroundf(scale * focalPx * eyeOffset * invZ);
            }
            if (dx < 0 || dx >= W) continue;
            if (src[dx] < 0 || nrow[x] > nrow[src[dx]]) src[dx] = x;
        }

        // hole fill: take the farther of the nearest valid neighbours
        int lastValid = -1;
        for (int x = 0; x < W; x++)
        {
            if (src[x] >= 0) { lastValid = src[x]; continue; }
            int r = x + 1;
            while (r < W && src[r] < 0) r++;
            int rightValid = r < W ? src[r] : -1;
            int pick = lastValid;
            if (pick < 0 || (rightValid >= 0 && nrow[rightValid] < nrow[pick])) pick = rightValid;
            if (pick < 0) pick = x;
            for (int h = x; h < r; h++) src[h] = -2 - pick;      // mark as filled, keep lastValid intact
            x = r - 1;
        }

        unsigned char* crow = outRGBA + (size_t)y * ROW_PITCH;
        float* drow = outDepth + (size_t)y * (ROW_PITCH / 4);
        for (int x = 0; x < W; x++)
        {
            int s = src[x] >= 0 ? src[x] : -2 - src[x];
            size_t si = ((size_t)y * W + s) * 3;
            crow[x * 4 + 0] = scene[si + 0];
            crow[x * 4 + 1] = scene[si + 1];
            crow[x * 4 + 2] = scene[si + 2];
            crow[x * 4 + 3] = 255;
            drow[x] = 1.0f - nrow[s];
        }
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    double runSeconds = 75.0;
    bool doWarp = true;
    float warpScale = 1.0f;
    bool useTruth = false, doDump = false;
    std::wstring imagePath;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--no-warp")) { doWarp = false; continue; }
        if (!strncmp(argv[i], "--scale=", 8)) { warpScale = (float)atof(argv[i] + 8); continue; }
        if (!strncmp(argv[i], "--near=", 7)) { continue; }
        if (!strcmp(argv[i], "--truth")) { useTruth = true; continue; }
        if (!strcmp(argv[i], "--dump")) { doDump = true; continue; }
        if (!strncmp(argv[i], "--image=", 8))
        {
            std::string p(argv[i] + 8);
            imagePath.assign(p.begin(), p.end());
            continue;
        }
        runSeconds = atof(argv[i]);
    }

    std::vector<unsigned char> imageRgb;
    if (!imagePath.empty())
    {
        if (!LoadImageWIC(imagePath.c_str(), imageRgb)) { printf("FAIL: cannot load image %ls\n", imagePath.c_str()); return 1; }
        printf("image: %ls -> %dx%d\n", imagePath.c_str(), W, H);
        if (useTruth) { printf("note: --truth ignored with --image (no ground truth for a photo)\n"); useTruth = false; }
    }

    // ---- ONNX Runtime + DML on a device we create -------------------------
    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    const OrtDmlApi* dmlApi = nullptr;
    if (OrtStatus* st = ort->GetExecutionProviderApi("DML", ORT_API_VERSION, (const void**)&dmlApi))
        return Fail("GetExecutionProviderApi(DML)", st);

    OrtEnv* env = nullptr;
    ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "xrapp3", &env);
    OrtSessionOptions* so = nullptr;
    ort->CreateSessionOptions(&so);

    ComPtr<IDXGIFactory4> fac;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    ComPtr<IDXGIAdapter1> ad;
    fac->EnumAdapters1(0, &ad);
    { DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d); wprintf(L"adapter: %s\n", d.Description); }

    // ---- OpenXR instance first, so we can create our device on the adapter
    //      the runtime demands (its LUID) rather than assuming adapter 0.
    HMODULE loader = LoadLibraryW(
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll");
    if (!loader) loader = LoadLibraryW(L"openxr_loader.dll");
    if (!loader) { printf("FAIL: openxr_loader.dll\n"); return 1; }
    g_getProc = (PFN_xrGetInstanceProcAddr)GetProcAddress(loader, "xrGetInstanceProcAddr");
    xrEnumExt_ = (PFN_xrEnumerateInstanceExtensionProperties)GetProcAddress(loader, "xrEnumerateInstanceExtensionProperties");
    xrCreateInstance_ = (PFN_xrCreateInstance)GetProcAddress(loader, "xrCreateInstance");

    const char* wantExt[] = { "XR_KHR_D3D12_enable", "XR_KHR_composition_layer_depth" };
    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    strcpy_s(ci.applicationInfo.applicationName, "VRX xrapp3");
    ci.enabledExtensionCount = 2;
    ci.enabledExtensionNames = wantExt;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult r = xrCreateInstance_(&ci, &instance);
    printf("xrCreateInstance: %s\n", XRStr(r));
    if (XR_FAILED(r)) return 1;
    if (!ResolveFns(instance)) return 1;

    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    if (XR_FAILED(xrGetSystem_(instance, &sgi, &systemId))) { printf("FAIL: xrGetSystem\n"); return 1; }

    XrGraphicsRequirementsD3D12KHR reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    r = xrD3D12Reqs_(instance, systemId, &reqs);
    if (XR_FAILED(r)) { printf("FAIL: graphics requirements: %s\n", XRStr(r)); return 1; }

    ComPtr<IDXGIAdapter1> rtAdapter;
    if (FAILED(fac->EnumAdapterByLuid(reqs.adapterLuid, IID_PPV_ARGS(&rtAdapter))))
    { printf("FAIL: EnumAdapterByLuid\n"); return 1; }

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(rtAdapter.Get(), reqs.minFeatureLevel, IID_PPV_ARGS(&device))))
    { printf("FAIL: D3D12CreateDevice\n"); return 1; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
    cmdList->Close();

    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fenceVal = 0;
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto gpuWait = [&]() {
        queue->Signal(fence.Get(), ++fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    };

    // ---- hand our device + queue to ORT ----------------------------------
    HMODULE dmllib = LoadLibraryW(L"DirectML.dll");
    if (!dmllib) { printf("FAIL: DirectML.dll\n"); return 1; }
    auto createDmlDevice = (HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**))
        GetProcAddress(dmllib, "DMLCreateDevice");
    ComPtr<IDMLDevice> dmlDevice;
    if (FAILED(createDmlDevice(device.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dmlDevice))))
    { printf("FAIL: DMLCreateDevice\n"); return 1; }
    if (OrtStatus* st = dmlApi->SessionOptionsAppendExecutionProvider_DML1(so, dmlDevice.Get(), queue.Get()))
        return Fail("AppendExecutionProvider_DML1", st);

    OrtSession* session = nullptr;
    std::wstring modelPath =
        L"C:\\Users\\paulj\\dev\\VRX\\bench\\models\\model_fixed_686x392.onnx";
    if (OrtStatus* st = ort->CreateSession(env, modelPath.c_str(), so, &session)) return Fail("CreateSession", st);

    const OrtMemoryInfo* inInfos[1] = { nullptr };
    if (OrtStatus* st = ort->SessionGetMemoryInfoForInputs(session, inInfos, 1)) return Fail("SessionGetMemoryInfoForInputs", st);

    // zero-copy model input on our device
    const size_t modelElems = (size_t)3 * H * W;
    const UINT64 modelBytes = (UINT64)modelElems * sizeof(float);
    int64_t modelDims[4] = { 1, 3, H, W };

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = modelBytes;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> modelIn;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&modelIn))))
    { printf("FAIL: model input resource\n"); return 1; }
    float* modelInMapped = nullptr;
    { D3D12_RANGE nr{ 0, 0 }; modelIn->Map(0, &nr, (void**)&modelInMapped); }

    void* dmlAlloc = nullptr;
    if (OrtStatus* st = dmlApi->CreateGPUAllocationFromD3DResource(modelIn.Get(), &dmlAlloc))
        return Fail("CreateGPUAllocationFromD3DResource", st);

    OrtValue* modelInValue = nullptr;
    if (OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(const_cast<OrtMemoryInfo*>(inInfos[0]), dmlAlloc,
                                                            modelBytes, modelDims, 4,
                                                            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &modelInValue))
        return Fail("CreateTensorWithDataAsOrtValue", st);

    printf("model : 392x686 on the shared device (zero-copy input)\n");

    // ---- OpenXR session on the same device -------------------------------
    XrGraphicsBindingD3D12KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    binding.device = device.Get();
    binding.queue = queue.Get();
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = systemId;
    XrSession sessionXr = XR_NULL_HANDLE;
    r = xrCreateSession_(instance, &sci, &sessionXr);
    printf("xrCreateSession: %s\n", XRStr(r));
    if (XR_FAILED(r)) return 1;

    XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace = { {0,0,0,1}, {0,0,0} };
    XrSpace space = XR_NULL_HANDLE;
    xrCreateReferenceSpace_(sessionXr, &rsci, &space);

    // ---- swapchains -------------------------------------------------------
    uint32_t fmtCount = 0;
    xrEnumFormats_(sessionXr, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumFormats_(sessionXr, fmtCount, &fmtCount, formats.data());

    int64_t colorFmt = 0;
    for (auto f : formats)
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM) { colorFmt = f; break; }
    if (!colorFmt) for (auto f : formats)
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { colorFmt = f; break; }
    if (!colorFmt) { printf("FAIL: no colour format\n"); return 1; }

    int64_t depthFmt = 0;
    for (auto f : formats)
        if (f == DXGI_FORMAT_D32_FLOAT) { depthFmt = f; break; }
    if (!depthFmt) for (auto f : formats)
        if (f == DXGI_FORMAT_D16_UNORM) { depthFmt = f; break; }
    printf("formats: colour %lld, depth %lld (of %u)\n", (long long)colorFmt, (long long)depthFmt, fmtCount);
    if (!depthFmt) { printf("FAIL: runtime offers no depth format\n"); return 1; }

    const uint32_t VIEWS = 2;

    XrSwapchain colorSc = XR_NULL_HANDLE, depthSc = XR_NULL_HANDLE;
    XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sc.sampleCount = 1; sc.width = W; sc.height = H; sc.faceCount = 1;
    sc.arraySize = VIEWS; sc.mipCount = 1;

    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = colorFmt;
    r = xrCreateSwapchain_(sessionXr, &sc, &colorSc);
    printf("colour swapchain: %s\n", XRStr(r));
    if (XR_FAILED(r)) return 1;

    sc.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = depthFmt;
    r = xrCreateSwapchain_(sessionXr, &sc, &depthSc);
    printf("depth  swapchain: %s\n", XRStr(r));
    if (XR_FAILED(r)) return 1;

    uint32_t ci2 = 0, di2 = 0;
    xrEnumImages_(colorSc, 0, &ci2, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> cimgs(ci2, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumImages_(colorSc, ci2, &ci2, (XrSwapchainImageBaseHeader*)cimgs.data());
    xrEnumImages_(depthSc, 0, &di2, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> dimgs(di2, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumImages_(depthSc, di2, &di2, (XrSwapchainImageBaseHeader*)dimgs.data());
    printf("swapchain images: colour %u, depth %u\n", ci2, di2);

    // ---- upload staging buffers ------------------------------------------
    auto makeUpload = [&](UINT64 bytes, ComPtr<ID3D12Resource>& out, void** mapped) {
        D3D12_HEAP_PROPERTIES uh{};
        uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        uh.CreationNodeMask = 1; uh.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC ud{};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = bytes;
        ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.SampleDesc.Count = 1;
        ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &ud,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&out)))) return false;
        D3D12_RANGE nr{ 0, 0 };
        return SUCCEEDED(out->Map(0, &nr, mapped));
    };

    const UINT64 sliceBytes = (UINT64)ROW_PITCH * H;
    ComPtr<ID3D12Resource> colorUp, depthUp;
    void* colorMapped = nullptr; void* depthMappedRaw = nullptr;
    if (!makeUpload(sliceBytes * VIEWS, colorUp, &colorMapped)) { printf("FAIL: colour upload\n"); return 1; }
    if (!makeUpload(sliceBytes * VIEWS, depthUp, &depthMappedRaw)) { printf("FAIL: depth upload\n"); return 1; }
    unsigned char* colorUpBase = (unsigned char*)colorMapped;
    float* depthUpF = (float*)depthMappedRaw;

    // ---- frame loop -------------------------------------------------------
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, exitLoop = false;
    auto start = std::chrono::steady_clock::now();
    uint64_t frames = 0, drawn = 0;
    double sumModel = 0, sumWarp = 0, sumUpload = 0;
    float lastDepthMin = 0, lastDepthMax = 0;

    std::vector<unsigned char> scene;
    std::vector<float> depth(H * W);
    std::vector<float> near01;
    float modelBack = 0, modelPanel = 0, modelMarker = 0;

    // scene (packed RGB) -> model -> nearness in [0,1] (0 = far, 1 = near).
    // The model gives relative disparity-like values, larger = nearer.
    auto runModel = [&](const std::vector<unsigned char>& rgb, std::vector<float>& outNear) -> bool {
        if (rgb.size() != (size_t)W * H * 3) { printf("runModel: bad scene size %zu\n", rgb.size()); return false; }

        const float mean[3] = { 0.485f, 0.456f, 0.406f };
        const float istd[3] = { 1.f / 0.229f, 1.f / 0.224f, 1.f / 0.225f };
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                size_t si = ((size_t)y * W + x) * 3;
                for (int c = 0; c < 3; c++)
                {
                    float v = rgb[si + c] / 255.0f;
                    modelInMapped[((size_t)c * H + y) * W + x] = (v - mean[c]) * istd[c];
                }
            }

        OrtValue* out = nullptr;
        const char* inName = "pixel_values";
        const char* outName = "predicted_depth";
        if (OrtStatus* st = ort->Run(session, nullptr, &inName,
                                     (const OrtValue* const*)&modelInValue, 1,
                                     &outName, 1, &out))
        { Fail("Run", st); return false; }
        float* dp = nullptr;
        ort->GetTensorMutableData(out, (void**)&dp);
        memcpy(depth.data(), dp, depth.size() * sizeof(float));
        ort->ReleaseValue(out);
        gpuWait();

        float dmin = depth[0], dmax = depth[0];
        for (float v : depth) { dmin = std::min(dmin, v); dmax = std::max(dmax, v); }
        lastDepthMin = dmin; lastDepthMax = dmax;
        float inv = (dmax - dmin) > 1e-6f ? 1.0f / (dmax - dmin) : 0.0f;
        outNear.resize(depth.size());
        for (size_t i = 0; i < depth.size(); i++) outNear[i] = (depth[i] - dmin) * inv;
        return true;
    };

    // ---- headset-independent model check ---------------------------------
    // Runs once before the frame loop so the model's opinion of the input is
    // in the log (and on disk with --dump) even if the HMD never wakes.
    {
        if (imageRgb.empty()) MakeScene(scene, 0.0); else scene = imageRgb;
        if (!runModel(scene, near01)) return 1;
        printf("model raw range: %.3f .. %.3f\n", lastDepthMin, lastDepthMax);
        if (imageRgb.empty())
        {
            RegionMeans(near01, 0.0, modelBack, modelPanel, modelMarker);
            printf("model nearness (0 far..1 near): backdrop %.2f  panel %.2f  marker %.2f   [truth: 0.00 / 0.50 / 1.00]\n",
                   modelBack, modelPanel, modelMarker);
        }
        if (doDump)
        {
            std::vector<unsigned char> g(near01.size());
            for (size_t i = 0; i < g.size(); i++) g[i] = (unsigned char)(near01[i] * 255.0f + 0.5f);
            WritePNM("xrapp3_scene.ppm", "P6", scene.data(), scene.size());
            WritePNM("xrapp3_near.pgm", "P5", g.data(), g.size());
        }
    }

    printf("\nentering frame loop (%.0fs)%s\n", runSeconds, doWarp ? "" : " [warp disabled]");

    while (!exitLoop)
    {
        XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent_(instance, &ev) == XR_SUCCESS)
        {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                auto* sc2 = (XrEventDataSessionStateChanged*)&ev;
                if (sc2->state != state)
                {
                    static const char* nm[] = { "UNKNOWN","IDLE","READY","SYNCHRONIZED","VISIBLE","FOCUSED","STOPPING","LOST","EXITING" };
                    printf("  state %d -> %s\n", (int)state,
                           ((int)sc2->state >= 0 && (int)sc2->state <= 8) ? nm[(int)sc2->state] : "?");
                }
                state = sc2->state;
                if (state == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = XR_SUCCEEDED(xrBeginSession_(sessionXr, &bi));
                    printf("  xrBeginSession: %s\n", running ? "OK" : "failed");
                }
                else if (state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED)
                {
                    printf("\n  >>> HMD ACTIVE - drawing the depth-attached projection layer now <<<\n"
                           "  >>> look for the moving white square and the amber panel;          <<<\n"
                           "  >>> the white square should sit *nearer* than the amber panel.      <<<\n\n");
                }
                else if (state == XR_SESSION_STATE_STOPPING) { xrEndSession_(sessionXr); running = false; }
                else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) exitLoop = true;
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
        if (exitLoop) break;

        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        if (XR_FAILED(xrWaitFrame_(sessionXr, nullptr, &fs))) break;
        xrBeginFrame_(sessionXr, nullptr);
        frames++;

        const XrCompositionLayerBaseHeader* layers[1] = { nullptr };
        XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        std::vector<XrCompositionLayerProjectionView> pviews(VIEWS);
        std::vector<XrCompositionLayerDepthInfoKHR> dinfo(VIEWS);

        if (running && fs.shouldRender)
        {
            // Locate the views FIRST: the stereo warp needs each eye's measured
            // offset and the focal length implied by the FOV. Guessing the eye
            // sign is how you accidentally produce *divergent* disparity, which
            // cannot be fused at all.
            XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = space;
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            uint32_t vc = VIEWS;
            std::vector<XrView> views(VIEWS, { XR_TYPE_VIEW });
            XrResult vr = xrLocateViews_(sessionXr, &vli, &vs, VIEWS, &vc, views.data());
            if (XR_FAILED(vr)) printf("xrLocateViews: %s\n", XRStr(vr));

            // Eye offset from the measured IPD, not from position.x: position.x
            // is in the reference space and is only the eye's lateral offset
            // while the head happens to face -Z.
            float ex = views[1].pose.position.x - views[0].pose.position.x;
            float ey = views[1].pose.position.y - views[0].pose.position.y;
            float ez = views[1].pose.position.z - views[0].pose.position.z;
            float ipd = sqrtf(ex * ex + ey * ey + ez * ez);
            float eyeX[VIEWS] = { -0.5f * ipd, 0.5f * ipd };

            // Both eyes are fed the SAME image, so both must declare the SAME
            // symmetric FOV and the SAME orientation. The runtime's per-eye
            // FOVs are asymmetric (wider outward) and mirrored between eyes;
            // declaring them puts the image centre left-of-ahead in the left
            // eye and right-of-ahead in the right eye - divergent disparity of
            // several degrees, which cannot be fused. With a shared symmetric
            // FOV the unwarped image sits at infinity and the warp alone adds
            // (convergent) disparity. The compositor reprojects the declared
            // FOV onto the real display, so this is legal.
            float tanHalfX = 1e9f;
            for (uint32_t e = 0; e < VIEWS; e++)
            {
                tanHalfX = std::min(tanHalfX, tanf(fabsf(views[e].fov.angleLeft)));
                tanHalfX = std::min(tanHalfX, tanf(fabsf(views[e].fov.angleRight)));
            }
            if (!(tanHalfX > 0.01f && tanHalfX < 100.0f)) tanHalfX = 1.0f;
            float tanHalfY = tanHalfX * (float)H / (float)W;   // square pixels
            XrFovf sharedFov{ -atanf(tanHalfX), atanf(tanHalfX), atanf(tanHalfY), -atanf(tanHalfY) };
            float fovx = sharedFov.angleRight - sharedFov.angleLeft;
            float focalPx = (float)(W * 0.5) / tanHalfX;

            // Shared orientation: nlerp of the two eyes (they differ on canted
            // displays).
            XrQuaternionf q0 = views[0].pose.orientation, q1 = views[1].pose.orientation;
            float qd = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
            float qs = qd < 0 ? -1.0f : 1.0f;
            XrQuaternionf sharedRot{ q0.x + qs * q1.x, q0.y + qs * q1.y, q0.z + qs * q1.z, q0.w + qs * q1.w };
            float qn = sqrtf(sharedRot.x * sharedRot.x + sharedRot.y * sharedRot.y +
                             sharedRot.z * sharedRot.z + sharedRot.w * sharedRot.w);
            if (qn > 1e-6f) { sharedRot.x /= qn; sharedRot.y /= qn; sharedRot.z /= qn; sharedRot.w /= qn; }
            else sharedRot = q0;

            static bool printed = false;
            if (!printed)
            {
                printed = true;
                for (uint32_t e = 0; e < VIEWS; e++)
                    printf("runtime fov eye %u: left %.1f  right %.1f  up %.1f  down %.1f deg\n", e,
                           views[e].fov.angleLeft * 57.2958f, views[e].fov.angleRight * 57.2958f,
                           views[e].fov.angleUp * 57.2958f, views[e].fov.angleDown * 57.2958f);
                printf("view (shared symmetric): fov_x %.1f deg   ipd %.1f mm   focal %.1f px\n",
                       fovx * 57.2958f, (eyeX[1] - eyeX[0]) * 1000.0f, focalPx);
                printf("warp: scale %.2f  (use --scale=N to adjust)\n", warpScale);
            }

            // Depth -> inverse distance. A modest range keeps the disparity
            // inside what the eyes can actually fuse.
            const float invZNear = 1.0f / 1.2f;   // nearest content ~1.2 m
            const float invZFar = 1.0f / 12.0f;   // furthest content ~12 m

            uint32_t cIdx = 0, dIdx = 0;
            XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            bool gotColor = XR_SUCCEEDED(xrAcquireImage_(colorSc, &ai, &cIdx));
            bool gotDepth = XR_SUCCEEDED(xrAcquireImage_(depthSc, &ai, &dIdx));

            if (gotColor && gotDepth)
            {
                XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi.timeout = XR_INFINITE_DURATION;
                bool ok = XR_SUCCEEDED(xrWaitImage_(colorSc, &wi)) && XR_SUCCEEDED(xrWaitImage_(depthSc, &wi));

                if (ok)
                {
                    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    if (imageRgb.empty()) MakeScene(scene, t); else scene = imageRgb;

                    auto m0 = std::chrono::steady_clock::now();
                    if (!runModel(scene, near01)) exitLoop = true;
                    auto m1 = std::chrono::steady_clock::now();
                    sumModel += std::chrono::duration<double, std::milli>(m1 - m0).count();

                    if (imageRgb.empty()) RegionMeans(near01, t, modelBack, modelPanel, modelMarker);
                    if (useTruth) MakeTruth(near01, t);

                    // --- forward stereo warp of colour AND depth, per eye,
                    //     straight into the staging buffers ------------------
                    auto w0 = std::chrono::steady_clock::now();
                    for (int e = 0; e < VIEWS; e++)
                    {
                        WarpEye(scene, near01, eyeX[e], focalPx, warpScale, invZNear, invZFar, doWarp,
                                colorUpBase + (size_t)e * sliceBytes,
                                depthUpF + ((size_t)e * sliceBytes) / sizeof(float));
                    }
                    auto w1 = std::chrono::steady_clock::now();
                    sumWarp += std::chrono::duration<double, std::milli>(w1 - w0).count();

                    // --- copy into the swapchain images ---------------------
                    cmdAlloc->Reset();
                    cmdList->Reset(cmdAlloc.Get(), nullptr);

                    D3D12_RESOURCE_BARRIER bars[4]{};
                    for (int i = 0; i < 2; i++)
                    {
                        bars[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        bars[i].Transition.pResource = i == 0 ? cimgs[cIdx].texture : dimgs[dIdx].texture;
                        bars[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        bars[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                        bars[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    }
                    cmdList->ResourceBarrier(2, bars);

                    for (UINT32 e = 0; e < VIEWS; e++)
                    {
                        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                        dst.pResource = cimgs[cIdx].texture;
                        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        dst.SubresourceIndex = e;
                        src.pResource = colorUp.Get();
                        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        src.PlacedFootprint.Offset = (UINT64)e * sliceBytes;
                        src.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)colorFmt;
                        src.PlacedFootprint.Footprint.Width = W;
                        src.PlacedFootprint.Footprint.Height = H;
                        src.PlacedFootprint.Footprint.Depth = 1;
                        src.PlacedFootprint.Footprint.RowPitch = ROW_PITCH;
                        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                        dst.pResource = dimgs[dIdx].texture;
                        dst.SubresourceIndex = e;
                        src.pResource = depthUp.Get();
                        src.PlacedFootprint.Offset = (UINT64)e * sliceBytes;
                        src.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)depthFmt;
                        src.PlacedFootprint.Footprint.RowPitch = ROW_PITCH;
                        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    }

                    for (int i = 0; i < 2; i++)
                    {
                        bars[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                        bars[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                    }
                    cmdList->ResourceBarrier(2, bars);

                    cmdList->Close();
                    ID3D12CommandList* lists[] = { cmdList.Get() };
                    queue->ExecuteCommandLists(1, lists);
                    gpuWait();

                    xrReleaseImage_(colorSc, &(XrSwapchainImageReleaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO }));
                    xrReleaseImage_(depthSc, &(XrSwapchainImageReleaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO }));
                    drawn++;
                }
            }

            for (uint32_t e = 0; e < VIEWS; e++)
            {
                pviews[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                pviews[e].pose = views[e].pose;
                pviews[e].pose.orientation = sharedRot;
                pviews[e].fov = sharedFov;
                pviews[e].subImage.swapchain = colorSc;
                pviews[e].subImage.imageRect = { {0, 0}, {W, H} };
                pviews[e].subImage.imageArrayIndex = e;

                dinfo[e] = { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
                dinfo[e].subImage.swapchain = depthSc;
                dinfo[e].subImage.imageRect = { {0, 0}, {W, H} };
                dinfo[e].subImage.imageArrayIndex = e;
                dinfo[e].minDepth = 0.0f;      // depth image value 0 ...
                dinfo[e].maxDepth = 1.0f;      // ... to 1
                dinfo[e].nearZ = 0.10f;
                dinfo[e].farZ = 30.0f;
                pviews[e].next = &dinfo[e];
            }

            proj.space = space;
            proj.viewCount = VIEWS;
            proj.views = pviews.data();
            layers[0] = (XrCompositionLayerBaseHeader*)&proj;
        }

        XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = layers[0] ? 1 : 0;
        fei.layers = layers;
        XrResult er = xrEndFrame_(sessionXr, &fei);
        if (XR_FAILED(er)) printf("xrEndFrame: %s\n", XRStr(er));

        if (frames % 100 == 0 && drawn > 0)
        {
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            printf("frame %5llu drawn %5llu  %.1f fps | model %.1f ms  warp+pack %.1f ms | depth %.3f..%.3f | model near: back %.2f panel %.2f marker %.2f%s\n",
                   (unsigned long long)frames, (unsigned long long)drawn, frames / (el > 0 ? el : 1),
                   sumModel / drawn, sumWarp / drawn, lastDepthMin, lastDepthMax,
                   modelBack, modelPanel, modelMarker, useTruth ? " [warp uses TRUTH]" : "");
        }

        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > runSeconds)
            exitLoop = true;
    }

    printf("\n--- summary ---\n");
    printf("frames %llu  drawn %llu\n", (unsigned long long)frames, (unsigned long long)drawn);
    if (drawn)
    {
        printf("model      : %.2f ms/frame\n", sumModel / drawn);
        printf("warp+pack  : %.2f ms/frame (CPU)\n", sumWarp / drawn);
        printf("depth range: %.3f .. %.3f\n", lastDepthMin, lastDepthMax);
    }

    if (modelInValue) ort->ReleaseValue(modelInValue);
    if (session) ort->ReleaseSession(session);
    xrDestroySwapchain_(colorSc);
    xrDestroySwapchain_(depthSc);
    xrDestroySession_(sessionXr);
    xrDestroyInstance_(instance);
    return 0;
}
