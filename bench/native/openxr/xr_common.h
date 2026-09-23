// Shared by xrapp3 / xrapp4: OpenXR loader glue, the test scene, image loading,
// and the CPU reference stereo warp. Include AFTER windows/d3d12/openxr/ORT/wincodec.
#pragma once
#include "synthetic_scene.h"
#include "stereo_warp.h"


// The model's fixed geometry, and therefore the render size too: a projection
// view's colour and depth sub-image rects must match, so matching the model
// output avoids a resize shader entirely.
static const int W = vrx::kSyntheticWidth, H = vrx::kSyntheticHeight;
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
XRFN(PFN_xrLocateSpace, xrLocateSpace_);

static bool ResolveFns(XrInstance inst)
{
    const char* names[] = {
        "xrDestroyInstance", "xrGetSystem", "xrCreateSession", "xrDestroySession",
        "xrBeginSession", "xrEndSession", "xrPollEvent", "xrCreateReferenceSpace",
        "xrLocateViews", "xrEnumerateViewConfigurationViews", "xrEnumerateSwapchainFormats",
        "xrCreateSwapchain", "xrDestroySwapchain", "xrEnumerateSwapchainImages",
        "xrAcquireSwapchainImage", "xrWaitSwapchainImage", "xrReleaseSwapchainImage",
        "xrWaitFrame", "xrBeginFrame", "xrEndFrame", "xrGetD3D12GraphicsRequirementsKHR",
        "xrLocateSpace",
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
        (PFN_xrVoidFunction*)&xrD3D12Reqs_, (PFN_xrVoidFunction*)&xrLocateSpace_,
    };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
    {
        if (XR_FAILED(g_getProc(inst, names[i], slots[i])) || *slots[i] == nullptr)
        { printf("FAIL: resolve %s\n", names[i]); return false; }
    }
    return true;
}

// The same reference scene is used by the Linux Vulkan renderer.
using vrx::MakeScene;
using vrx::MakeTruth;
using vrx::RegionMeans;

// Load any WIC-decodable image (png/jpg/bmp) as packed RGB.
//   forceW/forceH > 0 : scale to exactly that size.
//   otherwise         : keep the aspect ratio, shrink so width <= maxW, even dims.
static bool LoadImageWICSized(const wchar_t* path, int forceW, int forceH, int maxW,
                              int* outW, int* outH, std::vector<unsigned char>& rgb)
{
    if (!path || !outW || !outH) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;

    UINT sw = 0, sh = 0;
    if (FAILED(frame->GetSize(&sw, &sh)) || sw == 0 || sh == 0) return false;
    int w = forceW, h = forceH;
    if (w <= 0 || h <= 0)
    {
        if (maxW <= 0) return false;
        double k = sw > (UINT)maxW ? (double)maxW / sw : 1.0;
        w = std::max(2, (int)(sw * k + 0.5) & ~1);
        h = std::max(2, (int)(sh * k + 0.5) & ~1);
    }

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler))) return false;
    if (FAILED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    rgb.resize((size_t)w * h * 3);
    if (FAILED(conv->CopyPixels(nullptr, w * 3, (UINT)rgb.size(), rgb.data()))) return false;
    *outW = w; *outH = h;
    return true;
}

// Scaled to the model geometry WxH (xrapp3 / xrapp4).
static bool LoadImageWIC(const wchar_t* path, std::vector<unsigned char>& rgb)
{
    int w = 0, h = 0;
    return LoadImageWICSized(path, W, H, 0, &w, &h, rgb);
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

// Portable reference warp shared with the Linux renderer.
static const int FILL_STRETCH = vrx::kFillStretch;
static const int FILL_MIRROR = vrx::kFillMirror;
static const float MIRROR_TOL = vrx::kMirrorTol;
using vrx::WarpEyeFill;
using vrx::WarpEye;

// Widens the near (foreground) regions by rx depth pixels horizontally and ry
// vertically with a separable running max. Depth is ~2.8x coarser than colour and
// the model's edges are soft, so a depth edge lands a few colour pixels off the
// colour edge, and foreground-coloured pixels there receive background depth.
//  * Left/right edges: those pixels stay behind when the object shifts and leave a
//    ghost sliver beside the hole.
//  * Top/bottom edges: whole rows of the object move with the background instead
//    (outward in each eye when the background is behind the screen plane), so the
//    object's top edge visibly skews - e.g. the top of a third-person character's
//    helmet leaning left in the left eye and right in the right eye.
// Dilating the near field in both directions makes these pixels travel with their
// object. (An earlier version dilated horizontally only, reasoning that disparity
// is horizontal; that misses the top/bottom case.)
static void DilateNear(std::vector<float>& near01, int w, int h, int rx, int ry)
{
    if (rx <= 0 && ry <= 0) return;
    if (w <= 0 || h <= 0 || near01.size() != (size_t)w * h) return;

    if (rx > 0)
    {
        std::vector<float> row(w);
        for (int y = 0; y < h; y++)
        {
            float* p = near01.data() + (size_t)y * w;
            std::copy(p, p + w, row.begin());
            for (int x = 0; x < w; x++)
            {
                int a = x - rx < 0 ? 0 : x - rx;
                int b = x + rx >= w ? w - 1 : x + rx;
                float m = row[a];
                for (int i = a + 1; i <= b; i++) m = row[i] > m ? row[i] : m;
                p[x] = m;
            }
        }
    }
    if (ry > 0)
    {
        const std::vector<float> src = near01;
        for (int y = 0; y < h; y++)
        {
            int a = y - ry < 0 ? 0 : y - ry;
            int b = y + ry >= h ? h - 1 : y + ry;
            float* p = near01.data() + (size_t)y * w;
            for (int x = 0; x < w; x++)
            {
                float m = src[(size_t)a * w + x];
                for (int i = a + 1; i <= b; i++)
                {
                    const float v = src[(size_t)i * w + x];
                    m = v > m ? v : m;
                }
                p[x] = m;
            }
        }
    }
}

static void DilateNearHorizontal(std::vector<float>& near01, int w, int h, int radius)
{
    DilateNear(near01, w, h, radius, 0);
}

