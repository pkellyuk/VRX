// Shared by xrapp3 / xrapp4: OpenXR loader glue, the test scene, image loading,
// and the CPU reference stereo warp. Include AFTER windows/d3d12/openxr/ORT/wincodec.
#pragma once


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

// Forward stereo warp for one eye. Every SOURCE pixel is moved by its OWN
// disparity and z-tested at the destination, so near content really shifts and
// occludes what is behind it. (The earlier backward warp looked up disparity at
// the destination pixel, which merely trims the edges of a flat-coloured object
// instead of moving it - and lets far content overwrite near content.)
// Disocclusion holes are filled from the FARTHER neighbour, i.e. background.
//   eyeOffset : eye's lateral offset in metres (left negative)
//   outRGBA   : ROW_PITCH bytes per row;  outDepth : ROW_PITCH/4 floats per row.
//   Depth is written as a real D3D projective depth for the distance the warp
//   used, d = farZ/(farZ-nearZ) * (1 - nearZ/Z), so that the nearZ/farZ declared in
//   XrCompositionLayerDepthInfoKHR decode it back to the same metres. (Writing
//   "1 - nearness" linearly, as the first version did, decodes to 0.1-1 m for
//   almost the whole range - a depth image that contradicts the stereo disparity.)
// Hole-fill modes for disocclusions (the strip of background a near object uncovers
// when it shifts). Both fill from the FARTHER neighbour, i.e. the background side.
//   FILL_STRETCH : repeat that one background pixel across the hole. Cheap, but a
//                  hole up to ~30 px wide becomes a flat horizontal streak, and if
//                  the edge pixel is tinted by the foreground the streak is too.
//   FILL_MIRROR  : reflect the background texture outward about the hole's edge
//                  (dest anchor+k takes source anchorSrc-/+k), so the hole carries
//                  real texture that continues across the seam. A guard stops the
//                  reflection from pulling in anything nearer than the anchor
//                  (another foreground object): it holds the last good pixel.
static const int FILL_STRETCH = 0;
static const int FILL_MIRROR = 1;
static const float MIRROR_TOL = 0.08f;      // nearness units (0 far .. 1 near)

// Fills dest hole [x, r) of one row. src[] holds scatter results (>= 0) and is
// written with -2 - source for filled pixels. leftSrc/rightSrc are the sources of
// dest x-1 and dest r (-1 if outside the row); holes are maximal runs, so both
// neighbours are scatter hits, never fills.
static void FillHole(std::vector<int>& src, const float* nrow, int w, int x, int r,
                     int leftSrc, int rightSrc, int fillMode)
{
    if (!nrow) return;
    if (x < 0 || r > w || x >= r) return;

    if (leftSrc < 0 && rightSrc < 0)
    {
        for (int h = x; h < r; h++) src[h] = -2 - x;     // whole row empty: as the original fill
        return;
    }

    const bool useLeft = leftSrc >= 0 && !(rightSrc >= 0 && nrow[rightSrc] < nrow[leftSrc]);
    const int anchor = useLeft ? leftSrc : rightSrc;

    if (fillMode != FILL_MIRROR)
    {
        for (int h = x; h < r; h++) src[h] = -2 - anchor;
        return;
    }

    // walk outward from the anchor so the guard's "last good" propagates correctly
    const float anchorNear = nrow[anchor];
    int good = anchor;
    const int n = r - x;
    for (int k = 1; k <= n; k++)
    {
        const int h = useLeft ? (x - 1 + k) : (r - k);
        int cand = useLeft ? anchor - k : anchor + k;
        cand = cand < 0 ? 0 : (cand >= w ? w - 1 : cand);
        if (nrow[cand] <= anchorNear + MIRROR_TOL) good = cand;
        src[h] = -2 - good;
    }
}

// subpixel: keep the fractional part of each shift. Whole-pixel shifts quantise a
// smoothly receding surface into ~25 flat bands with a 1 px step between them, which
// reads as ridges ("ploughed field") on uniform texture; sampling the colour at the
// fractional source position that lands on this destination removes them.
static void WarpEyeFill(const std::vector<unsigned char>& scene, const std::vector<float>& near01,
                        float eyeOffset, float focalPx, float scale, float invZNear, float invZFar,
                        float nearZ, float farZ, bool doWarp, int fillMode,
                        unsigned char* outRGBA, float* outDepth, bool subpixel = false)
{
    if (!outRGBA || !outDepth) return;
    if (scene.size() != (size_t)W * H * 3 || near01.size() != (size_t)W * H) return;

    std::vector<int> src(W);
    std::vector<float> srcDest(W);            // where the chosen source pixel really lands
    for (int y = 0; y < H; y++)
    {
        const float* nrow = near01.data() + (size_t)y * W;
        std::fill(src.begin(), src.end(), -1);

        for (int x = 0; x < W; x++)
        {
            int dx = x;
            float destF = (float)x;
            if (doWarp)
            {
                // Content at distance Z sits at -focal*E/Z in the eye's image
                // relative to the cyclopean image (left eye: shifted right).
                float invZ = invZFar + nrow[x] * (invZNear - invZFar);
                const float s = scale * focalPx * eyeOffset * invZ;
                destF = (float)x - s;
                dx = x - (int)lroundf(s);
            }
            if (dx < 0 || dx >= W) continue;
            if (src[dx] < 0 || nrow[x] > nrow[src[dx]]) { src[dx] = x; srcDest[dx] = destF; }
        }

        // holes: maximal runs of unwritten dest pixels
        int lastValid = -1;
        for (int x = 0; x < W; x++)
        {
            if (src[x] >= 0) { lastValid = src[x]; continue; }
            int r = x + 1;
            while (r < W && src[r] < 0) r++;
            int rightValid = r < W ? src[r] : -1;
            FillHole(src, nrow, W, x, r, lastValid, rightValid, fillMode);
            x = r - 1;
        }

        unsigned char* crow = outRGBA + (size_t)y * ROW_PITCH;
        float* drow = outDepth + (size_t)y * (ROW_PITCH / 4);
        for (int x = 0; x < W; x++)
        {
            const bool filled = src[x] < 0;
            int s = filled ? -2 - src[x] : src[x];
            size_t si = ((size_t)y * W + s) * 3;
            // Filled pixels have no continuous mapping, so they stay whole-pixel.
            // srcDest is indexed by DESTINATION: where the pixel written here landed.
            const float pos = (subpixel && !filled) ?
                std::min(std::max((float)s + ((float)x - srcDest[x]), 0.0f), (float)(W - 1)) : (float)s;
            const int i0 = (int)pos, i1 = i0 + 1 < W ? i0 + 1 : W - 1;
            const float fr = pos - i0;
            const size_t a = ((size_t)y * W + i0) * 3, b = ((size_t)y * W + i1) * 3;
            // Same arithmetic as the shader: UNORM values, lerp as c0 + f*(c1-c0),
            // then the UNORM store's round-to-nearest.
            for (int ch = 0; ch < 3; ch++)
            {
                const float c0 = scene[a + ch] / 255.0f, c1 = scene[b + ch] / 255.0f;
                crow[x * 4 + ch] = (unsigned char)lroundf((c0 + fr * (c1 - c0)) * 255.0f);
            }
            crow[x * 4 + 3] = 255;
            float invZ = invZFar + nrow[s] * (invZNear - invZFar);
            drow[x] = (farZ / (farZ - nearZ)) * (1.0f - nearZ * invZ);
        }
    }
}

// The original stretch-fill warp (xrapp3 / xrapp4 and their shader).
static void WarpEye(const std::vector<unsigned char>& scene, const std::vector<float>& near01,
                    float eyeOffset, float focalPx, float scale, float invZNear, float invZFar,
                    float nearZ, float farZ,
                    bool doWarp, unsigned char* outRGBA, float* outDepth)
{
    WarpEyeFill(scene, near01, eyeOffset, focalPx, scale, invZNear, invZFar, nearZ, farZ, doWarp,
                FILL_STRETCH, outRGBA, outDepth);
}

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

