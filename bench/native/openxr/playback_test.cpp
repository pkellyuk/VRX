#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture_scaler.h"
#include "xr_frame_guard.h"
#include "screen_anchor.h"
#include "capture_window.h"
#include "desktop_control.h"
#include "foreground_refinement.h"
#include "gpu_choice.h"
#include "depth_fusion.h"
#include "frame_timing.h"
#include "screen_curve.h"
#include "ambilight.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

static void Check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}
static void Hr(HRESULT hr) { Check(SUCCEEDED(hr), "D3D11 operation"); }

static int acquired = 0, waited = 0, released = 0;
static XrResult acquireResult = XR_SUCCESS, waitResult = XR_SUCCESS, releaseResult = XR_SUCCESS;
static XrResult XRAPI_CALL Acquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index)
{ ++acquired; *index = 7; return acquireResult; }
static XrResult XRAPI_CALL Wait(XrSwapchain, const XrSwapchainImageWaitInfo*)
{ ++waited; return waitResult; }
static XrResult XRAPI_CALL Release(XrSwapchain, const XrSwapchainImageReleaseInfo*)
{ ++released; return releaseResult; }

static void TestTrackingAndSwapchains()
{
    const auto flags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    Check(ValidStereoViews(XR_SUCCESS, 2, flags), "valid stereo tracking");
    Check(!ValidStereoViews(XR_ERROR_RUNTIME_FAILURE, 2, flags), "failed locate");
    Check(!ValidStereoViews(XR_SUCCESS, 1, flags), "missing eye");
    Check(!ValidStereoViews(XR_SUCCESS, 2, XR_VIEW_STATE_POSITION_VALID_BIT), "invalid orientation");
    Check(!ValidStereoViews(XR_SUCCESS, 2, XR_VIEW_STATE_ORIENTATION_VALID_BIT), "invalid position");
    Check(ValidStereoViews(XR_SUCCESS, 2, flags), "tracking can recover");
    XrReadyImage image;
    Check(image.Acquire(XR_NULL_HANDLE, Acquire, Wait) && image.index == 7, "acquire and wait");
    Check(image.Release(Release), "release ready image");
    Check(image.Release(Release) && released == 1, "release exactly once");
    acquireResult = XR_ERROR_RUNTIME_FAILURE;
    XrReadyImage failedAcquire;
    Check(!failedAcquire.Acquire(XR_NULL_HANDLE, Acquire, Wait), "acquire failure");
    failedAcquire.Release(Release);
    Check(waited == 1 && released == 1, "failed acquire must not wait or release");
    acquireResult = XR_SUCCESS;
    for (auto result : { XR_TIMEOUT_EXPIRED, XR_ERROR_RUNTIME_FAILURE })
    {
        waitResult = result;
        XrReadyImage failedWait;
        Check(!failedWait.Acquire(XR_NULL_HANDLE, Acquire, Wait), "timeout/error is not ready");
        failedWait.Release(Release);
        Check(released == 1, "unwaited image must not be released");
    }
    waitResult = XR_SUCCESS; releaseResult = XR_ERROR_RUNTIME_FAILURE;
    XrReadyImage failedRelease;
    Check(failedRelease.Acquire(XR_NULL_HANDLE, Acquire, Wait), "ready before release error");
    Check(!failedRelease.Release(Release), "release failure must propagate");
}

static void TestDepthPolicy()
{
    Check(UsableDepth(true, 1, 5000, 1, 1), "unchanged source does not expire");
    Check(UsableDepth(true, 10, 2.1, 9, 2.0), "short source lag may reuse depth");
    Check(!UsableDepth(true, 10, 2.5, 9, 2.0), "changing source with stale depth falls back");
    Check(!UsableDepth(false, 1, 1, 1, 1), "worker failure overrides even static pairing");
    Check(!UsableDepth(true, 1, 1, 0, 0), "missing depth falls back");
    Check(!UsableDepth(true, 2, std::numeric_limits<double>::quiet_NaN(), 1, 1), "invalid timestamp");
    Check(UsableDepth(true, 20, 3.0, 20, 3.0), "fresh depth restores stereo");
    Check(FitCapture(0, 0, 64, 48).width == 0, "minimized source has no valid rectangle");
}

static void TestCaptureResizePixels()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    CaptureScaler scaler;
    Hr(scaler.Init(device.Get()));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 64; td.Height = 48; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, readback;
    ComPtr<ID3D11RenderTargetView> rtv;
    Hr(device->CreateTexture2D(&td, nullptr, &target));
    Hr(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Hr(device->CreateTexture2D(&td, nullptr, &readback));
    const uint32_t colors[4] = { 0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF };
    struct Size { int w, h; };
    // Reuse one output across shrink/grow/aspect changes to expose stale pixels.
    for (auto size : { Size{64,48}, Size{32,24}, Size{96,24}, Size{12,48}, Size{96,72}, Size{64,48} })
    {
        std::vector<uint32_t> pixels(size_t(size.w) * size.h);
        for (int y = 0; y < size.h; ++y)
            for (int x = 0; x < size.w; ++x)
                pixels[size_t(y) * size.w + x] = colors[(y >= size.h / 2 ? 2 : 0) + (x >= size.w / 2 ? 1 : 0)];
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Width = UINT(size.w); sd.Height = UINT(size.h);
        sd.Usage = D3D11_USAGE_DEFAULT; sd.CPUAccessFlags = 0;
        D3D11_SUBRESOURCE_DATA data{ pixels.data(), UINT(size.w * 4), 0 };
        ComPtr<ID3D11Texture2D> input;
        Hr(device->CreateTexture2D(&sd, &data, &input));
        Hr(scaler.Copy(device.Get(), context.Get(), input.Get(), size.w, size.h,
            target.Get(), rtv.Get(), 64, 48));
        context->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Hr(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        auto pixel = [&](int x, int y) { return reinterpret_cast<const uint32_t*>(
            static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch)[x]; };
        auto rect = FitCapture(size.w, size.h, 64, 48);
        for (int q = 0; q < 4; ++q)
        {
            int x = int(rect.x + rect.width * (q % 2 ? 0.75f : 0.25f));
            int y = int(rect.y + rect.height * (q / 2 ? 0.75f : 0.25f));
            Check(pixel(x, y) == colors[q], "resized capture must retain all quadrants and orientation");
        }
        if (rect.x > 1) Check(pixel(0, 24) == 0xFF000000 && pixel(63, 24) == 0xFF000000, "side bars cleared");
        if (rect.y > 1) Check(pixel(32, 0) == 0xFF000000 && pixel(32, 47) == 0xFF000000, "top/bottom bars cleared");
        context->Unmap(readback.Get(), 0);
        Check(scaler.Copy(device.Get(), context.Get(), input.Get(), size.w + 1, size.h,
            target.Get(), rtv.Get(), 64, 48) == E_INVALIDARG, "reject clipped frame after pool resize");
    }
}

static void TestClientCrop()
{
    RECT crop{ -1, -1, -1, -1 };
    // Windowed game at 150%: 2880x1620 client inside a 2882x1668 frame
    // (1 px border each side, 47 px title bar + top border).
    RECT bounds{ 100, 200, 100 + 2882, 200 + 1668 };
    Check(ClientCropInFrame(bounds, POINT{ 101, 247 }, SIZE{ 2880, 1620 }, 2882, 1668, crop) &&
        crop.left == 1 && crop.top == 47 && crop.right == 2881 && crop.bottom == 1667, "windowed: frame and title bar cropped");
    // Borderless / fullscreen: client == frame, nothing cropped.
    RECT full{ 0, 0, 1920, 1080 };
    Check(ClientCropInFrame(full, POINT{ 0, 0 }, SIZE{ 1920, 1080 }, 1920, 1080, crop) &&
        crop.left == 0 && crop.top == 0 && crop.right == 1920 && crop.bottom == 1080, "borderless: whole frame");
    // Negative desktop coordinates (monitor left of the primary).
    RECT left{ -2000, -50, -2000 + 802, -50 + 632 };
    Check(ClientCropInFrame(left, POINT{ -1999, -19 }, SIZE{ 800, 600 }, 802, 632, crop) &&
        crop.left == 1 && crop.top == 31 && crop.right == 801 && crop.bottom == 631, "negative desktop coordinates");
    // Frame size disagrees with the bounds (resize or DPI change in flight): no crop.
    RECT before = crop;
    Check(!ClientCropInFrame(bounds, POINT{ 101, 247 }, SIZE{ 2880, 1620 }, 1920, 1080, crop) &&
        crop.left == before.left && crop.bottom == before.bottom, "inconsistent frame: whole frame, crop untouched");
    // Scaled (DPI-virtualised) coordinates would put the client mostly outside: rejected.
    // (origin 2600,1600 in a frame ending at 2982,1868: only 382x268 of 1920x1080 visible)
    Check(!ClientCropInFrame(bounds, POINT{ 2600, 1600 }, SIZE{ 1920, 1080 }, 2882, 1668, crop), "mostly off-frame client rejected");
    Check(!ClientCropInFrame(bounds, POINT{ 101, 247 }, SIZE{ 0, 0 }, 2882, 1668, crop), "empty client rejected");
    // DPI-unaware game stretched 150% by Windows: 1 px inset trims the blended edge ring.
    Check(ClientCropInFrame(bounds, POINT{ 101, 247 }, SIZE{ 2880, 1620 }, 2882, 1668, crop, 1) &&
        crop.left == 2 && crop.top == 48 && crop.right == 2880 && crop.bottom == 1666, "stretched window: edge ring trimmed");
    Check(ScaledWindowInset(144, 144) == 0 && ScaledWindowInset(96, 96) == 0, "DPI-aware window: no inset");
    Check(ScaledWindowInset(96, 144) == 1 && ScaledWindowInset(96, 192) == 1, "150% / 200% stretch: 1 px");
    Check(ScaledWindowInset(96, 240) == 2 && ScaledWindowInset(96, 480) == 3 && ScaledWindowInset(0, 144) == 0, "larger stretch, clamped");
    // Client slightly larger than the frame (rounding): clamped, still valid.
    Check(ClientCropInFrame(full, POINT{ 0, 0 }, SIZE{ 1921, 1081 }, 1920, 1080, crop) &&
        crop.right == 1920 && crop.bottom == 1080, "client clamped to frame");
}

// A captured window with a grey 1 px frame and title bar around a four-colour
// picture: cropping to the client region must leave no frame pixels in the
// output, on both the exact-size copy and the scaled path.
static void TestCaptureClientRegion()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    CaptureScaler scaler;
    Hr(scaler.Init(device.Get()));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 64; td.Height = 48; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, readback;
    ComPtr<ID3D11RenderTargetView> rtv;
    Hr(device->CreateTexture2D(&td, nullptr, &target));
    Hr(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Hr(device->CreateTexture2D(&td, nullptr, &readback));
    const uint32_t frameColour = 0xFF808080;
    const uint32_t colors[4] = { 0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFF00 };
    struct Case { int cw, ch; const char* name; };
    for (auto c : { Case{ 64, 48, "exact-size client" }, Case{ 32, 24, "scaled client" } })
    {
        const int border = 1, title = 9;
        const int fw = c.cw + 2 * border, fh = c.ch + border + title + border;
        std::vector<uint32_t> pixels(size_t(fw) * fh, frameColour);
        for (int y = 0; y < c.ch; ++y)
            for (int x = 0; x < c.cw; ++x)
                pixels[size_t(y + border + title) * fw + x + border] = colors[(y >= c.ch / 2 ? 2 : 0) + (x >= c.cw / 2 ? 1 : 0)];
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Width = UINT(fw); sd.Height = UINT(fh);
        sd.Usage = D3D11_USAGE_DEFAULT; sd.CPUAccessFlags = 0;
        D3D11_SUBRESOURCE_DATA data{ pixels.data(), UINT(fw * 4), 0 };
        ComPtr<ID3D11Texture2D> input;
        Hr(device->CreateTexture2D(&sd, &data, &input));

        const RECT region{ border, border + title, border + c.cw, border + title + c.ch };
        Hr(scaler.Copy(device.Get(), context.Get(), input.Get(), fw, fh, target.Get(), rtv.Get(), 64, 48, &region));
        context->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Hr(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        auto pixel = [&](int x, int y) { return reinterpret_cast<const uint32_t*>(
            static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch)[x]; };
        int framePixels = 0;
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x)
                if (pixel(x, y) == frameColour) ++framePixels;
        Check(framePixels == 0, "client crop leaves no window-frame pixels");
        Check(pixel(0, 0) == colors[0] && pixel(63, 0) == colors[1] && pixel(0, 47) == colors[2] && pixel(63, 47) == colors[3],
            "client crop keeps every corner of the game picture");
        context->Unmap(readback.Get(), 0);
        printf("  client region (%s): %dx%d frame -> 64x48, 0 frame pixels\n", c.name, fw, fh);
    }
    const RECT outside{ 0, 0, 200, 200 };
    Check(scaler.Copy(device.Get(), context.Get(), target.Get(), 64, 48, target.Get(), rtv.Get(), 64, 48, &outside) == E_INVALIDARG,
        "region outside the frame rejected");
}

// The capture thread's model-size box filter vs a CPU reference of the same math
// (taps x taps bilinear samples per output pixel, clamped edges). GPU bilinear
// weights are fixed point, so allow 2 levels in 255 per channel.
static void TestCaptureDownscale()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    CaptureScaler scaler;
    Hr(scaler.Init(device.Get()));

    const int sw = 700, sh = 400, dw = 160, dh = 96;          // taps = ceil(700/160) = 5 -> clamped to 4
    std::vector<uint32_t> src(size_t(sw) * sh);
    uint32_t seed = 12345;
    for (auto& p : src) { seed = seed * 1664525u + 1013904223u; p = 0xFF000000u | (seed >> 8); }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = sw; td.Height = sh; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{ src.data(), UINT(sw * 4), 0 };
    ComPtr<ID3D11Texture2D> input, output, readback;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    Hr(device->CreateTexture2D(&td, &data, &input));
    Hr(device->CreateShaderResourceView(input.Get(), nullptr, &srv));
    td.Width = dw; td.Height = dh; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    Hr(device->CreateTexture2D(&td, nullptr, &output));
    Hr(device->CreateRenderTargetView(output.Get(), nullptr, &rtv));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Hr(device->CreateTexture2D(&td, nullptr, &readback));

    Hr(scaler.Downscale(context.Get(), srv.Get(), sw, rtv.Get(), dw, dh));
    context->CopyResource(readback.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Hr(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));

    auto channel = [&](int x, int y, int c) { return float((src[size_t(y) * sw + x] >> (8 * c)) & 0xFF); };
    auto bilinear = [&](float u, float v, int c) {
        const float fx = std::min(std::max(u * sw - .5f, 0.f), float(sw - 1));
        const float fy = std::min(std::max(v * sh - .5f, 0.f), float(sh - 1));
        const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
        const float a = fx - x0, b = fy - y0;
        return (channel(x0, y0, c) * (1 - a) + channel(x1, y0, c) * a) * (1 - b) + (channel(x0, y1, c) * (1 - a) + channel(x1, y1, c) * a) * b;
    };
    const int taps = 4;
    int worst = 0;
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x)
        {
            const uint32_t got = reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch)[x];
            for (int c = 0; c < 3; ++c)
            {
                float sum = 0;
                for (int j = 0; j < taps; ++j)
                    for (int i = 0; i < taps; ++i)
                        sum += bilinear((x + (i + .5f) / taps) / dw, (y + (j + .5f) / taps) / dh, c);
                const int ref = int(sum / (taps * taps) + .5f);
                worst = std::max(worst, std::abs(int((got >> (8 * c)) & 0xFF) - ref));
            }
        }
    context->Unmap(readback.Get(), 0);
    printf("  capture downscale %dx%d -> %dx%d (4x4 taps): worst channel error %d/255\n", sw, sh, dw, dh, worst);
    Check(worst <= 2, "capture box-filter downscale matches the CPU reference");
    Check(scaler.Downscale(context.Get(), nullptr, sw, rtv.Get(), dw, dh) == E_INVALIDARG, "downscale rejects a missing picture");
}

static void TestGpuChoice()
{
    // RTX 3090 renders the headset; two identical 3060s; a software adapter.
    const std::vector<GpuEntry> gpus = {
        { L"NVIDIA GeForce RTX 3090", 24325, false, true },
        { L"NVIDIA GeForce RTX 3060", 12115, false, false },
        { L"NVIDIA GeForce RTX 3060", 12115, false, false },
        { L"Microsoft Basic Render Driver", 0, true, false },
    };
    GpuRequest r;
    std::wstring why;
    Check(ParseGpuRequest(L"same", r) && ChooseDepthAdapter(gpus, r, why) == -1, "same: headset GPU");
    Check(ParseGpuRequest(L"auto", r) && ChooseDepthAdapter(gpus, r, why) == 1, "auto: first largest other GPU");
    Check(ParseGpuRequest(L"name:NVIDIA GeForce RTX 3060#0", r) && ChooseDepthAdapter(gpus, r, why) == 1, "first of two identical cards");
    Check(ParseGpuRequest(L"name:NVIDIA GeForce RTX 3060#1", r) && ChooseDepthAdapter(gpus, r, why) == 2, "second of two identical cards");
    Check(ParseGpuRequest(L"name:nvidia geforce rtx 3060 #1", r) && ChooseDepthAdapter(gpus, r, why) == 2, "names match case- and space-insensitively");
    Check(ParseGpuRequest(L"name:NVIDIA GeForce RTX 3060#2", r) && ChooseDepthAdapter(gpus, r, why) == -1, "missing third card falls back");
    Check(ParseGpuRequest(L"name:NVIDIA GeForce RTX 3090#0", r) && ChooseDepthAdapter(gpus, r, why) == -1, "choosing the headset GPU means no offload");
    Check(ParseGpuRequest(L"name:Microsoft Basic Render Driver#0", r) && ChooseDepthAdapter(gpus, r, why) == -1, "software adapter refused");
    Check(ParseGpuRequest(L"name:AMD Radeon RX 7900 XTX#0", r) && ChooseDepthAdapter(gpus, r, why) == -1, "card no longer present falls back");
    Check(ParseGpuRequest(L"2", r) && ChooseDepthAdapter(gpus, r, why) == 2, "index for diagnostics");
    Check(ParseGpuRequest(L"0", r) && ChooseDepthAdapter(gpus, r, why) == -1, "index of headset GPU: no offload");
    Check(!ParseGpuRequest(L"name:#0", r) && !ParseGpuRequest(L"name:RTX", r) && !ParseGpuRequest(L"name:RTX#x", r) &&
        !ParseGpuRequest(L"third", r) && !ParseGpuRequest(L"", r), "malformed requests rejected");
    Check(NthOfName(gpus, 1) == 0 && NthOfName(gpus, 2) == 1 && NthOfName(gpus, 0) == 0, "identical cards are numbered in order");
    const std::vector<GpuEntry> single = { { L"NVIDIA GeForce RTX 3090", 24325, false, true } };
    Check(ParseGpuRequest(L"auto", r) && ChooseDepthAdapter(single, r, why) == -1, "auto with one GPU stays on it");
}

static void TestScreenAnchor()
{
    ScreenAnchor screen;
    XrPosef left{{0,0,0,1}, {-.032f,1.6f,0}}, right = left;
    right.position.x = .032f;
    Check(screen.Place(left, right, left.orientation, 1, .5625f), "first valid view places screen");
    Check(std::abs(screen.pose.position.z + 3) < .0001f && screen.pose.position.x == 0,
        "screen centered three metres ahead of eye midpoint");
    Check(screen.size.width == 6 && screen.size.height == 3.375f, "screen aspect and angular size");
    left.position.x += 1; right.position.x += 1;
    XrQuaternionf turn{0, std::sqrt(.5f), 0, std::sqrt(.5f)};
    Check(!screen.Place(left, right, turn, .5f, 1), "movement does not reposition or resize screen");
    Check(screen.pose.position.x == 0 && screen.size.width == 6, "anchor persists");
    screen.Key(true);
    Check(screen.pending, "recenter waits until valid tracking is supplied");
    Check(screen.Place(left, right, turn, 1, .5625f), "key recenters");
    Check(std::abs(screen.pose.position.x + 2) < .0001f && std::abs(screen.pose.position.z) < .0001f,
        "recenter follows rotated forward direction and translated head");
    screen.Key(true);
    Check(!screen.pending, "held key does not repeatedly recenter");
    screen.Key(false); screen.Key(true);
    Check(screen.pending, "released then pressed key recenters again");
    // Texture warp plus the physical quad must reproduce the original central
    // viewing disparity at strength 1, without counting the screen plane twice.
    for (float z : {1.2f, 3.f, 12.f})
    {
        const float plane = 362.f * .064f / ScreenAnchor::distance;
        const float warp = 362.f * .064f * (1/z - 1/ScreenAnchor::distance);
        Check(std::abs(plane + warp - 362.f * .064f / z) < .00001f, "stereo plane compensation");
    }
}

static void TestCaptureSelection()
{
    CaptureWindow window;
    window.pid = 42;
    window.executable = L"helldivers2.exe";
    window.title = L"HELLDIVERS 2";
    window.windowClass = L"GameWindow";
    Check(MatchesCaptureWindow(window, L"", L"HELLDIVERS2.EXE", 7), "executable match ignores case");
    Check(!MatchesCaptureWindow(window, L"", L"helldivers.exe", 7), "executable name must match exactly");
    Check(!MatchesCaptureWindow(window, L"", L"helldivers2.exe", 42), "exclude our own process");
    Check(!MatchesCaptureWindow(window, L"wrong title", L"helldivers2.exe", 7), "both filters must match");
    window.title.clear();
    Check(MatchesCaptureWindow(window, L"", L"helldivers2.exe", 7), "executable selection works without title");
    window.title = L"cmd.exe - xrapp5 1800 --window=HELLDIVERS";
    window.executable = L"conhost.exe";
    window.windowClass = L"ConsoleWindowClass";
    Check(!MatchesCaptureWindow(window, L"HELLDIVERS", L"", 7), "command line title cannot select console");
    window.executable.clear(); // Limited process query can fail; class still excludes the console.
    Check(!MatchesCaptureWindow(window, L"HELLDIVERS", L"", 7), "console excluded even without process query");
    window.executable = L"WindowsTerminal.exe";
    window.windowClass = L"CASCADIA_HOSTING_WINDOW_CLASS";
    Check(!MatchesCaptureWindow(window, L"HELLDIVERS", L"", 7), "exclude Windows Terminal");
    window.executable = L"some-launcher.exe"; window.windowClass = L"OtherWindow";
    Check(!MatchesCaptureWindow(window, L"", L"helldivers2.exe", 7), "title spoof cannot pass executable filter");
}

static void TestDesktopControl()
{
    DesktopSettings settings;
    Check(ParseDesktopSettings("VRX 1 6.25 3.5 0.2 0.4 0.8 0 1 1 187 120 4 7 0", settings), "valid complete desktop snapshot");
    Check(settings.width == 6.25f && settings.recenter == 4 && settings.menu == 7, "snapshot values and command sequence");
    Check(!ParseDesktopSettings("VRX 1 6.25 3.5", settings), "partial snapshot rejected");
    Check(!ParseDesktopSettings("VRX 8 6.25 3.5 0 0 1 0 1 1 187 120 0 0 0 1 0 0 0 0 0 1", settings), "unknown version rejected");
    Check(settings.foreground == 0, "v1 keeps foreground refinement off");
    Check(ParseDesktopSettings("VRX 2 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1", settings) && settings.foreground == 1, "v2 enables foreground refinement");
    Check(ParseDesktopSettings("VRX 2 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 0", settings) && settings.foreground == 0, "v2 disables foreground refinement");
    Check(!ParseDesktopSettings("VRX 2 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0", settings), "v2 missing feature flag rejected");
    Check(!ParseDesktopSettings("VRX 2 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 2", settings), "v2 invalid feature flag rejected");
    Check(settings.paired == 0, "older snapshots leave frame matching off");
    Check(ParseDesktopSettings("VRX 3 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 1", settings) && settings.paired == 1, "v3 enables matching");
    Check(!ParseDesktopSettings("VRX 3 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1", settings) && settings.paired == 1, "partial v3 rejected without changing settings");
    Check(ParseDesktopSettings("VRX 3 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0", settings) && settings.fastModel == 0, "v3 keeps the default model");
    Check(ParseDesktopSettings("VRX 4 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1", settings) && settings.fastModel == 1, "v4 selects the fast model");
    Check(ParseDesktopSettings("VRX 4 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 0", settings) && settings.fastModel == 0, "v4 selects the default model");
    Check(!ParseDesktopSettings("VRX 4 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0", settings), "v4 missing model flag rejected");
    Check(!ParseDesktopSettings("VRX 4 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 2", settings), "v4 invalid model flag rejected");
    Check(ParseDesktopSettings("VRX 4 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1", settings) && settings.steady == 0 && settings.fuse == 0, "v4 leaves steady/fuse off");
    Check(ParseDesktopSettings("VRX 5 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0", settings) && settings.steady == 1 && settings.fuse == 0, "v5 steady only");
    Check(ParseDesktopSettings("VRX 5 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 0 1", settings) && settings.steady == 0 && settings.fuse == 1, "v5 fuse only");
    Check(ParseDesktopSettings("VRX 5 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 1", settings) && settings.steady == 1 && settings.fuse == 1 && settings.fastModel == 1, "v5 both");
    Check(!ParseDesktopSettings("VRX 5 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1", settings), "v5 missing fuse flag rejected");
    Check(!ParseDesktopSettings("VRX 5 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 2 0", settings), "v5 invalid steady flag rejected");
    Check(!ParseDesktopSettings("VRX 5 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 0 1 0", settings), "v5 trailing value rejected");
    Check(ParseDesktopSettings("VRX 5 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0", settings) && settings.delayed == 0, "v5 leaves delayed timing off");
    Check(ParseDesktopSettings("VRX 6 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1", settings) && settings.delayed == 1 && settings.paired == 0, "v6 delayed timing");
    Check(ParseDesktopSettings("VRX 6 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 1 1 1 0 0", settings) && settings.delayed == 0 && settings.paired == 1, "v6 matched timing");
    Check(!ParseDesktopSettings("VRX 6 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0", settings), "v6 missing timing flag rejected");
    Check(!ParseDesktopSettings("VRX 6 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 2", settings), "v6 invalid timing flag rejected");
    Check(ParseDesktopSettings("VRX 6 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1", settings) && settings.subpixel == 1, "v6 keeps the sub-pixel warp on");
    Check(ParseDesktopSettings("VRX 7 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 0", settings) && settings.subpixel == 0, "v7 turns the sub-pixel warp off");
    Check(ParseDesktopSettings("VRX 7 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1", settings) && settings.subpixel == 1, "v7 sub-pixel warp on");
    Check(!ParseDesktopSettings("VRX 7 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1", settings), "v7 missing warp flag rejected");
    Check(!ParseDesktopSettings("VRX 7 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 2", settings), "v7 invalid warp flag rejected");
    Check(ParseDesktopSettings("VRX 7 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1", settings) && settings.curve == 0 && settings.ambilight == 0,
        "v7 has a flat screen and no glow");
    Check(ParseDesktopSettings("VRX 8 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 65 1", settings) && settings.curve == 65 && settings.ambilight == 1,
        "v8 curve percentage and ambilight");
    Check(ParseDesktopSettings("VRX 8 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0", settings) && settings.curve == 0 && settings.ambilight == 0,
        "v8 flat with no glow");
    Check(ParseDesktopSettings("VRX 8 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 100 0", settings) && settings.curve == 100,
        "v8 fully curved");
    Check(!ParseDesktopSettings("VRX 8 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 65", settings), "v8 missing ambilight flag rejected");
    Check(!ParseDesktopSettings("VRX 8 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 101 0", settings), "v8 curve over 100 rejected");
    Check(!ParseDesktopSettings("VRX 8 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 -1 0", settings), "v8 negative curve rejected");
    Check(!ParseDesktopSettings("VRX 8 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 2", settings), "v8 invalid ambilight flag rejected");
    Check(!ParseDesktopSettings("VRX 9 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0", settings), "a newer snapshot version is rejected");
    Check(!ParseDesktopSettings("VRX 3 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 2", settings), "invalid matching flag rejected");
    Check(ParseDesktopSettings("VRX 3 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0", settings) && settings.paired == 0, "v3 disables matching");
    Check(!ParseDesktopSettings("VRX 1 6.25 0 0 0 1 0 1 1 187 120 0 0 0", settings), "zero distance rejected");
    Check(!ParseDesktopSettings("VRX 1 6.25 3 0 0 1 0 1 1 120 120 0 0 0", settings), "conflicting shortcuts rejected");
    Check(!ParseDesktopSettings("VRX 1 nan 3 0 0 1 0 1 1 187 120 0 0 0", settings), "nonfinite value rejected");
    ScreenAnchor screen;
    XrPosef left{{0,0,0,1},{-.03f,1.6f,0}}, right{{0,0,0,1},{.03f,1.6f,0}};
    screen.Place(left, right, left.orientation, 1, .5f);
    screen.Adjust(4, 5, .5f, 1, .5f);
    Check(screen.pose.position.x == 1 && std::abs(screen.pose.position.y - 2.1f) < .00001f && screen.pose.position.z == -5,
        "live offsets use recentered origin");
    left.position.x = 8; right.position.x = 8;
    screen.Place(left, right, left.orientation, 1, .5f);
    screen.Adjust(6, 4, 0, 0, .5f);
    Check(screen.pose.position.x == 0 && screen.size.width == 6 && screen.size.height == 3, "slider preserves stationary origin after player movement");
}

// The fusion maths (depth_fusion.h); bench/native/xmmodel/fusion_golden checks it
// against the Python reference on real frames, these check its defining properties.
static void TestDepthFusion()
{
    const int w = 96, h = 64;
    fusion::Image z(w, h), ramp(w, h), luma(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            z.at(x, y) = 0.2f + 0.6f * (float)x / (w - 1);
            ramp.at(x, y) = (float)x;
            luma.at(x, y) = std::fmod(x * 37.0f + y * 91.0f, 256.0f);
        }

    // Blur keeps a constant image constant and preserves the mean of a ramp's middle.
    fusion::Image flat(w, h, 0.37f);
    const fusion::Image blurred = fusion::Blur(flat, 4.0f);
    Check(std::fabs(blurred.at(10, 10) - 0.37f) < 1e-5f && std::fabs(blurred.at(0, 0) - 0.37f) < 1e-5f, "blur keeps a constant");
    Check(std::fabs(fusion::Blur(ramp, 2.0f).at(48, 30) - 48.0f) < 1e-3f, "blur keeps a linear ramp in the middle");
    Check(fusion::GaussianKernel(4.0f).size() == 33 && fusion::GaussianKernel(16.0f).size() == 129, "kernel size as OpenCV (8 sigma + 1, odd)");

    // Global fit recovers an exact affine relation.
    fusion::Image d(w, h);
    for (size_t i = 0; i < d.v.size(); i++) d.v[i] = 0.7f * z.v[i] + 0.05f;
    float a = 0, b = 0;
    fusion::GlobalFit(z, d, a, b);
    Check(std::fabs(a - 0.7f) < 1e-4f && std::fabs(b - 0.05f) < 1e-4f, "global fit recovers the affine map");

    // The guided fit reproduces a target that is an affine map of the guide.
    const fusion::Image fitted = fusion::GuidedFit(z, d, 16.0f);
    float worst = 0;
    for (size_t i = 0; i < d.v.size(); i++) worst = std::max(worst, std::fabs(fitted.v[i] - d.v[i]));
    Check(worst < 1e-3f, "guided fit reproduces an affine target");

    // Zero motion vectors: identity maps, remap returns the image, trust is full.
    std::vector<int16_t> zero((size_t)((w + 7) / 8) * ((h + 7) / 8) * 2, 0);
    const fusion::Motion still = fusion::MotionFromVectors(zero.data(), (w + 7) / 8, (h + 7) / 8, w, h);
    Check(still.valid() && still.mapX.at(5, 7) == 5.0f && still.mapY.at(5, 7) == 7.0f, "zero vectors give identity maps");
    Check(fusion::Remap(ramp, still).at(40, 20) == 40.0f, "identity remap");
    float mean = 0;
    const fusion::Image full = fusion::MotionTrust(luma, luma, still, &mean);
    Check(full.valid() && mean > 0.999f && full.at(3, 3) == 1.0f, "identical pictures are fully trusted");

    // Uniform vectors of (-8, 0) quarter pels = 2 px: the moved ramp reads x - 2.
    std::vector<int16_t> left(zero.size());
    for (size_t i = 0; i < left.size(); i += 2) { left[i] = -8; left[i + 1] = 0; }
    const fusion::Motion shift = fusion::MotionFromVectors(left.data(), (w + 7) / 8, (h + 7) / 8, w, h);
    Check(std::fabs(fusion::Remap(ramp, shift).at(40, 20) - 38.0f) < 1e-4f, "vectors move content by quarter pels");

    // Pictures that do not match: trust collapses to zero everywhere (whole-frame cut).
    fusion::Image other(w, h);
    for (size_t i = 0; i < other.v.size(); i++) other.v[i] = 255.0f - luma.v[i];
    const fusion::Image none = fusion::MotionTrust(luma, other, still, &mean);
    Check(none.valid() && none.at(40, 20) == 0.0f && mean < fusion::VERIFY_CUT, "mismatched pictures are not trusted");

    // Fusion with no trust follows the fast model mapped globally; with full trust and
    // an anchor that is an affine map of the fast model, it reproduces the anchor.
    const fusion::Image noTrust = fusion::Fuse(z, fusion::Image(w, h, 0.9f), fusion::Image(w, h, 0.0f), 0.7f, 0.05f);
    worst = 0;
    for (size_t i = 0; i < z.v.size(); i++) worst = std::max(worst, std::fabs(noTrust.v[i] - d.v[i]));
    Check(worst < 1e-3f, "untrusted fusion falls back to the global mapping");
    const fusion::Image trusted = fusion::Fuse(z, d, fusion::Image(), 1.0f, 0.0f);
    worst = 0;
    for (size_t i = 0; i < z.v.size(); i++) worst = std::max(worst, std::fabs(trusted.v[i] - d.v[i]));
    Check(worst < 1e-3f, "trusted fusion follows the anchor");

    // Steadying: no trust leaves the frame alone; full trust blends by alpha.
    const fusion::Image prev(w, h, 0.8f), cur(w, h, 0.4f);
    Check(fusion::Steady(cur, prev, fusion::Image(w, h, 0.0f)).at(9, 9) == 0.4f, "untrusted steadying leaves the frame");
    Check(std::fabs(fusion::Steady(cur, prev, fusion::Image(w, h, 1.0f)).at(9, 9) - (0.4f + fusion::STEADY_ALPHA * 0.4f)) < 1e-6f, "trusted steadying blends by alpha");
    Check(fusion::Steady(cur, fusion::Image(), fusion::Image()).at(9, 9) == 0.4f, "steadying without history is a no-op");

    // Luma: BT.601 from packed RGBA (R in the low byte).
    const uint32_t px[2] = { 0xFF0000FFu, 0xFF00FF00u };
    const fusion::Image l = fusion::LumaFromRgba(px, 2, 1);
    Check(l.at(0, 0) == std::round(0.299f * 255) && l.at(1, 0) == std::round(0.587f * 255), "luma weights and channel order");
    std::printf("PASS: depth fusion maths\n");
}

// "Delayed to depth" frame choice (frame_timing.h).
static void TestFrameTiming()
{
    // 60 fps frames at t = 0.000, 0.0167, ... 0.1
    std::vector<TimedFrame> h;
    for (int i = 0; i <= 6; i++) h.push_back({ (uint64_t)(100 + i), i / 60.0 });
    const double now = 0.1;
    Check(PickDelayedFrame({}, now, 0.05) == -1, "no history, no frame");
    Check(PickDelayedFrame(h, now, 0.0) == 6, "no delay shows the newest frame");
    Check(PickDelayedFrame(h, now, 0.05) == 3, "50 ms delay shows the newest frame at or before now - 50 ms");
    Check(PickDelayedFrame(h, now, 0.5) == 0, "a delay longer than the history shows the oldest kept frame");
    Check(PickDelayedFrame(h, now, -1.0) == 6, "negative delay is treated as none");

    // History keeps exactly one frame at or before the target, and never exceeds maxKeep.
    Check(DelayedHistoryExcess(h, now, 0.05, 10) == 3, "frames older than the shown one are dropped");
    Check(DelayedHistoryExcess(h, now, 0.0, 10) == 6, "no delay keeps only the newest frame");
    Check(DelayedHistoryExcess(h, now, 0.5, 4) == 3, "at most maxKeep frames are kept");
    Check(DelayedHistoryExcess({ { 1, 0.0 } }, now, 0.0, 4) == 0, "a single frame is never dropped");

    DepthDelayEstimate d;
    d.Update(0.060);
    Check(d.have && std::fabs(d.value - 0.060) < 1e-9, "first delay sample taken as is");
    d.Update(0.160);
    Check(std::fabs(d.value - 0.080) < 1e-9, "delay estimate is smoothed");
    d.Update(-1.0); d.Update(5.0);
    Check(std::fabs(d.value - 0.080) < 1e-9, "invalid delay samples are ignored");
    Check(std::string(FrameTimingName(FrameTiming::Delayed)) == "delayed to depth", "timing names");
    std::printf("PASS: game frame timing\n");
}

static void TestForegroundRefinement()
{
    constexpr int w=196, h=112;
    std::vector<float> depth(w*h,.1f);
    for (int y=35; y<80; ++y) for (int x=50; x<88; ++x) depth[y*w+x]=.85f;
    ForegroundTracker tracker; DepthCrop crop;
    Check(!tracker.Update(depth,w,h,1,crop), "foreground requires persistence");
    Check(!tracker.Update(depth,w,h,1.15,crop), "foreground not acquired early");
    Check(tracker.Update(depth,w,h,1.3,crop), "persistent foreground gets crop");
    Check(crop.size<1 && crop.x>=0 && crop.y>=0 && crop.x+crop.size<=1 && crop.y+crop.size<=1, "crop is magnified and bounded");
    auto blank=std::vector<float>(w*h,.1f);
    Check(!tracker.Update(blank,w,h,1.4,crop), "disappearance clears foreground");
    Check(!tracker.Update(depth,w,h,1.5,crop), "reappearance must reacquire");
    Check(!tracker.Update(std::vector<float>(w*h,.9f),w,h,1.6,crop), "full screen near field is not a useful crop");
    tracker.Reset();
    Check(!tracker.Update(depth,w,h,2,crop), "explicit reset clears persistence");
    tracker.Update(depth,w,h,2.15,crop);
    Check(!tracker.Update(depth,w,h,3,crop), "long capture gap clears persistence");
    ForegroundBudget budget;
    Check(budget.CanRun(1,50,50) && !budget.CanRun(1,150,50), "crop budget rejects predicted stale result");
    budget.Complete(1,.050*1000);
    Check(!budget.CanRun(1.1,20,50) && budget.CanRun(1.3,20,50), "crop budget bounds extra inference frequency");
    budget.Complete(2,100);
    Check(!budget.CanRun(2.3,20,50) && budget.CanRun(2.5,20,50), "costlier crops have longer cooldown");
    const DepthCrop testCrop{.1f,.1f,.7f};
    std::vector<float> base(w*h), local(w*h);
    for (int y=0; y<h; ++y) for (int x=0; x<w; ++x)
    {
        base[y*w+x]=.1f+.8f*(x+.5f)/w;
        local[y*w+x]=3.f*(.1f+.8f*(testCrop.x+(x+.5f)/w*testCrop.size))+2.f;
    }
    const auto original=base;
    Check(FuseForeground(base,local,w,h,testCrop), "relative crop scale/offset aligns to global depth");
    for (size_t i=0;i<base.size();++i) Check(std::abs(base[i]-original[i])<.001f, "scale alignment preserves an agreeing scene");
    for (int y=45;y<55;++y) for (int x=135;x<145;++x) local[y*w+x]+=.2f;
    Check(FuseForeground(base,local,w,h,testCrop), "consistent crop may add localized detail");
    bool changed=false;
    for (size_t i=0;i<base.size();++i)
    {
        changed |= std::abs(base[i]-original[i])>.001f;
        Check(std::isfinite(base[i]) && base[i]>=0 && base[i]<=1 && std::abs(base[i]-original[i])<=.098f, "refinement changes bounded");
        if (original[i]<.35f) Check(base[i]==original[i], "far background remains unchanged");
    }
    Check(changed, "extra pass actually changes foreground detail");
    base=original;
    Check(!FuseForeground(base,std::vector<float>(w*h,1),w,h,testCrop) && base==original, "flat unalignable crop keeps base depth");
    local[0]=std::numeric_limits<float>::quiet_NaN();
    Check(!FuseForeground(base,local,w,h,testCrop) && base==original, "invalid crop does not corrupt base depth");
}

// Curved screen geometry (screen_curve.h): the cylinder and the ray casts that the
// curve shader and its CPU reference (CurvedPixel) share.
static void TestScreenCurve()
{
    Cylinder flat;
    Check(BuildCylinder(5.7f, 3.2f, 3.0f, 0.0f, flat) && !flat.curved, "no curve is a flat screen");
    Cylinder bad;
    Check(!BuildCylinder(0.0f, 3.2f, 3.0f, 1.0f, bad), "a screen with no width is rejected");
    Check(!BuildCylinder(5.7f, 0.0f, 3.0f, 1.0f, bad), "a screen with no height is rejected");
    Check(!BuildCylinder(5.7f, 3.2f, 0.0f, 1.0f, bad), "a screen at no distance is rejected");
    Check(!BuildCylinder(5.7f, 3.2f, 3.0f, -0.5f, bad), "a negative curve is rejected");
    Check(!BuildCylinder(std::numeric_limits<float>::quiet_NaN(), 3.2f, 3.0f, 1.0f, bad), "an invalid width is rejected");

    const float width = 5.7f, height = 3.2f, distance = 3.0f;
    Cylinder cyl;
    Check(BuildCylinder(width, height, distance, 1.0f, cyl) && cyl.curved, "a full curve builds");
    Check(std::fabs(cyl.halfWrap * 2 - kCurveMaxWrap) < 1e-5f, "a full curve uses the maximum wrap");
    Check(std::fabs(cyl.radius * cyl.halfWrap * 2 - width) < 1e-4f, "the arc keeps the width the player chose");

    const float viewer[3] = { 0, 0, distance };
    float tu = 0, tv = 0;
    const float ahead[3] = { 0, 0, -1 };
    Check(CylinderHit(cyl, viewer, ahead, &tu, &tv) && std::fabs(tu - 0.5f) < 1e-5f && std::fabs(tv - 0.5f) < 1e-5f,
        "straight ahead is the middle of the picture");
    Check(!CylinderHit(flat, viewer, ahead, &tu, &tv), "a flat screen is not ray-cast");
    Check(!CylinderHit(cyl, nullptr, ahead, &tu, &tv) && !CylinderHit(cyl, viewer, nullptr, &tu, &tv), "missing rays are rejected");

    // The real edge of the screen: where the arc ends, and nearer than the middle.
    const float edge[3] = { cyl.radius * std::sin(cyl.halfWrap * 0.999f), 0, cyl.radius * (1 - std::cos(cyl.halfWrap * 0.999f)) };
    Check(edge[2] > 0.5f, "the edges come well towards the viewer");
    const float toEdge[3] = { edge[0] - viewer[0], edge[1] - viewer[1], edge[2] - viewer[2] };
    Check(CylinderHit(cyl, viewer, toEdge, &tu, &tv) && std::fabs(tu - 0.9995f) < 1e-3f && std::fabs(tv - 0.5f) < 1e-4f,
        "aiming at the right-hand edge lands on the right-hand edge of the picture");
    const float pastEdge[3] = { cyl.radius * std::sin(cyl.halfWrap * 1.01f) - viewer[0], 0,
                                cyl.radius * (1 - std::cos(cyl.halfWrap * 1.01f)) - viewer[2] };
    Check(!CylinderHit(cyl, viewer, pastEdge, &tu, &tv), "just past the edge misses the screen");
    // Where a flat screen's edge was, a curved one still has picture: it wraps round.
    const float flatEdge[3] = { width * 0.5f, 0, -distance };
    Check(CylinderHit(cyl, viewer, flatEdge, &tu, &tv) && tu < 0.97f, "the curved screen reaches further round than the flat one");
    const float top[3] = { 0, height * 0.5f * 0.999f, -distance };
    Check(CylinderHit(cyl, viewer, top, &tu, &tv) && tv < 0.001f && tv >= 0, "aiming at the top of the middle lands on the top row");
    const float above[3] = { 0, height * 0.5f * 1.01f, -distance };
    Check(!CylinderHit(cyl, viewer, above, &tu, &tv), "above the screen misses it");
    // From one eye the middle of the picture is still where it should be.
    const float leftEye[3] = { -0.032f, 0, distance };
    const float toMiddle[3] = { 0.032f, 0, -distance };
    Check(CylinderHit(cyl, leftEye, toMiddle, &tu, &tv) && std::fabs(tu - 0.5f) < 1e-5f, "either eye sees the middle at the middle");

    // A very gentle curve has an enormous radius; the stable quadratic keeps it exact.
    Cylinder gentle;
    Check(BuildCylinder(width, height, distance, 0.01f, gentle) && gentle.curved && gentle.radius > 300, "a gentle curve has a huge radius");
    Check(CylinderHit(gentle, viewer, ahead, &tu, &tv) && std::fabs(tu - 0.5f) < 1e-4f && std::fabs(tv - 0.5f) < 1e-4f,
        "a gentle curve keeps its precision");
    const float nearFlatEdge[3] = { width * 0.5f * 0.99f, 0, -distance };
    Check(CylinderHit(gentle, viewer, nearFlatEdge, &tu, &tv) && std::fabs(tu - 0.995f) < 2e-3f, "a gentle curve is nearly flat");

    // A wide screen very close by would wrap past the viewer's head.
    Cylinder clamped;
    Check(BuildCylinder(10.0f, 5.6f, 1.0f, 1.0f, clamped) && clamped.curved, "a wide close screen still curves");
    Check(clamped.halfWrap * 2 < kCurveMaxWrap, "the wrap is reduced when the edges would come too close");
    Check(CurveSag(10.0f, clamped.halfWrap * 2) <= 1.0f * (1.0f - kCurveMinDepthFraction) + 1e-4f, "the edges stay in front of the viewer");

    // The pass: rays from an eye looking straight at the screen, and the glow behind.
    CurveConstants c{};
    c.ew = 64; c.eh = 48;
    c.radius = cyl.radius; c.halfWrap = cyl.halfWrap; c.halfWidth = cyl.halfWidth; c.halfHeight = cyl.halfHeight;
    c.glowOn = 1; c.glowHalfW = width; c.glowHalfH = height; c.glowZ = -kAmbiBehind;
    for (int e = 0; e < 2; e++)
    {
        CurveEye& v = c.eye[e];
        v.origin[0] = e == 0 ? -0.032f : 0.032f; v.origin[2] = distance;
        v.row0[0] = 1; v.row1[1] = 1; v.row2[2] = 1;
        v.tanL = -1.5f; v.tanR = 1.5f; v.tanU = 1.2f; v.tanD = -1.2f;
    }
    float o[3], d[3];
    CurveRay(c, 0, 32, 24, o, d);
    Check(std::fabs(d[0]) < 1e-6f && std::fabs(d[1]) < 1e-6f && d[2] == -1.0f && o[0] == -0.032f, "the middle pixel looks straight ahead from the eye");
    CurveRay(c, 0, 0, 0, o, d);
    Check(std::fabs(d[0] + 1.5f) < 1e-6f && std::fabs(d[1] - 1.2f) < 1e-6f, "the top-left pixel looks up and to the left");
    float gu = 0, gv = 0;
    const float corner[3] = { -width * 0.9f, height * 0.9f, -distance };
    Check(GlowHit(c, viewer, corner, &gu, &gv) && gu > 0 && gu < 0.2f && gv > 0 && gv < 0.2f, "past the screen's corner is the glow's corner");
    CurveConstants dark = c;
    dark.glowOn = 0;
    Check(!GlowHit(dark, viewer, corner, &gu, &gv), "no glow when the ambilight is off");

    std::vector<unsigned char> picture(8 * 8 * 4, 0), glowPixels(4 * 4 * 4, 0);
    for (size_t i = 0; i < picture.size(); i += 4) { picture[i] = 200; picture[i + 1] = 100; picture[i + 2] = 50; picture[i + 3] = 255; }
    for (size_t i = 0; i < glowPixels.size(); i += 4) { glowPixels[i + 2] = 80; glowPixels[i + 3] = 80; }
    RgbaImage pic{ picture.data(), 8, 8, 32 }, glowImg{ glowPixels.data(), 4, 4, 16 };
    float rgb[3];
    Check(CurvedPixel(c, 0, 32, 24, cyl, pic, &glowImg, rgb) && std::fabs(rgb[0] - 200 / 255.0f) < 1e-5f && std::fabs(rgb[2] - 50 / 255.0f) < 1e-5f,
        "the middle of the eye shows the picture");
    // Pixel (8, 8) looks above and left of the screen, into the glow.
    Check(CurvedPixel(c, 0, 8, 8, cyl, pic, &glowImg, rgb) && rgb[0] == 0 && std::fabs(rgb[2] - 80 / 255.0f) < 1e-5f,
        "above and beside the screen the eye sees the glow");
    Check(CurvedPixel(c, 0, 0, 0, cyl, pic, &glowImg, rgb) && rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0, "beyond the glow it is black");
    Check(CurvedPixel(dark, 0, 8, 8, cyl, pic, nullptr, rgb) && rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0, "with no glow the surround is black");
    Check(!CurvedPixel(c, 2, 0, 0, cyl, pic, &glowImg, rgb) && !CurvedPixel(c, 0, 64, 0, cyl, pic, &glowImg, rgb), "pixels outside the eyes are rejected");
    // Along the middle row, from the picture to the glow, there is an outline pixel
    // that mixes the two - the smooth edge the four samples are for.
    bool mixed = false;
    for (int x = 32; x < 64; x++)
        if (CurvedPixel(c, 0, x, 24, cyl, pic, &glowImg, rgb) && rgb[0] > 0.01f && rgb[0] < 200 / 255.0f - 0.01f) mixed = true;
    Check(mixed, "the screen's outline is smoothed");

    // Ambilight constants: the reference refuses anything it cannot compute.
    AmbiConstants a{};
    float rgba[4] = { 0, 0, 0, 0 };
    std::vector<unsigned char> source(16 * 16 * 4, 200);
    Check(!AmbilightPixel(a, source.data(), 16 * 4, 0, 0, rgba), "the glow needs a size");
    a.gw = 8; a.gh = 8; a.srcW = 16; a.srcH = 16;
    a.rectW = 2; a.rectH = 2; a.marginM = 0.25f; a.insetX = 0.125f; a.insetY = 0.125f;
    a.intensity = kAmbiIntensity; a.blurPx = 1; a.reset = 1;
    Check(!AmbilightPixel(a, nullptr, 16 * 4, 0, 0, rgba), "the glow needs a picture");
    Check(!AmbilightPixel(a, source.data(), 4, 0, 0, rgba), "a pitch shorter than the picture is rejected");
    Check(!AmbilightPixel(a, source.data(), 16 * 4, 8, 0, rgba), "a pixel outside the glow is rejected");
    Check(AmbilightPixel(a, source.data(), 16 * 4, 4, 4, rgba) && rgba[3] == 0, "the middle of the glow is hidden by the screen");
    Check(AmbilightPixel(a, source.data(), 16 * 4, 0, 4, rgba) && rgba[3] > 0 && rgba[3] <= kAmbiIntensity, "the side glows, never brighter than asked");
    Check(rgba[0] <= rgba[3] && rgba[1] <= rgba[3] && rgba[2] <= rgba[3], "the colour is premultiplied by its own alpha");
    // The bezel: dark right against the screen, full strength a little further out.
    AmbiConstants bezel = a;
    bezel.gw = 256; bezel.gh = 256; bezel.insetX = 0.25f; bezel.insetY = 0.25f; bezel.bezel = kAmbiBezel;
    float touching[4], beyond[4], unbezelled[4];
    Check(AmbilightPixel(bezel, source.data(), 16 * 4, 63, 128, touching) && AmbilightPixel(bezel, source.data(), 16 * 4, 56, 128, beyond),
        "glow either side of the bezel");
    AmbiConstants noBezel = bezel;
    noBezel.bezel = 0;
    Check(AmbilightPixel(noBezel, source.data(), 16 * 4, 63, 128, unbezelled), "glow without a bezel");
    Check(touching[3] < 0.2f * unbezelled[3], "right next to the screen the glow is dark");
    Check(beyond[3] > touching[3] * 3, "a little further out it is bright");
}

int main(int argc, char** argv)
{
    TestForegroundRefinement();
    TestDesktopControl();
    TestDepthFusion();
    TestFrameTiming();
    if (argc > 1)
    {
        const std::string path(argv[1]); DesktopSettings settings;
        Check(ReadDesktopSettings(std::wstring(path.begin(), path.end()), settings), "read C# emitted snapshot");
        Check(settings.width == 6.25f && settings.recenter == 4 && settings.menu == 7 && settings.menuKey == 120 && settings.foreground == 1 && settings.paired == 1,
            "C# to native control contract");
    }
    TestCaptureSelection();
    TestScreenAnchor();
    TestTrackingAndSwapchains();
    TestDepthPolicy();
    TestCaptureResizePixels();
    TestClientCrop();
    TestCaptureClientRegion();
    TestCaptureDownscale();
    TestGpuChoice();
    TestScreenCurve();
    std::puts("PASS: curved screen geometry, ambilight constants, game/terminal capture selection, stationary screen/recenter/stereo calibration, tracking validity, swapchain failures, depth fallback/recovery, D3D11 resize pixels and bars, window client-area crop, capture downscale, depth GPU choice");
}
