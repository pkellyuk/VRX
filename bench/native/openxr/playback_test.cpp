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
#include "srgb.h"
#include "room.h"
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
    Check(ParseDesktopSettings("VRX 8 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 1", settings) && settings.ambiStrength == 85 &&
        settings.worldColor == 0, "v8 has the default glow strength and a black world");
    Check(ParseDesktopSettings("VRX 9 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 30 1 40 2765889", settings) && settings.curve == 30 &&
        settings.ambilight == 1 && settings.ambiStrength == 40 && settings.worldColor == 0x2A3441, "v9 glow strength and world colour");
    Check(ParseDesktopSettings("VRX 9 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 100 16777215", settings) && settings.ambiStrength == 100 &&
        settings.worldColor == 0xFFFFFF, "v9 extremes");
    Check(!ParseDesktopSettings("VRX 9 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85", settings), "v9 missing world colour rejected");
    Check(!ParseDesktopSettings("VRX 9 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 101 0", settings), "v9 strength over 100 rejected");
    Check(!ParseDesktopSettings("VRX 9 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 16777216", settings), "v9 world colour out of range rejected");
    Check(!ParseDesktopSettings("VRX 9 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 -1", settings), "v9 negative world colour rejected");
    Check(ParseDesktopSettings("VRX 9 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 0", settings) && settings.room == 0, "v9 has no room");
    Check(ParseDesktopSettings("VRX 10 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 0 40", settings) && settings.room == 40, "v10 room level");
    Check(ParseDesktopSettings("VRX 10 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 0 0", settings) && settings.room == 0, "v10 room off");
    Check(ParseDesktopSettings("VRX 10 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 0 100", settings) && settings.room == 100, "v10 room at most");
    Check(!ParseDesktopSettings("VRX 10 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 0", settings), "v10 missing room rejected");
    Check(!ParseDesktopSettings("VRX 10 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 0 101", settings), "v10 room over 100 rejected");
    Check(!ParseDesktopSettings("VRX 10 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 0 -1", settings), "v10 negative room rejected");
    Check(!ParseDesktopSettings("VRX 10 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 0 40 7", settings), "v10 trailing value rejected");
    Check(!ParseDesktopSettings("VRX 11 6.25 3.5 0 0 1 0 1 1 187 120 4 7 0 1 0 1 1 0 1 1 0 0 85 0 40", settings), "a newer snapshot version is rejected");
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
    c.glowOn = 1; c.glowHalfW = width; c.glowHalfH = height; c.glowRadius = cyl.radius + kAmbiBehind;
    c.world[0] = 0.1f; c.world[1] = 0.2f; c.world[2] = 0.3f;
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

    // The glow wraps round with the screen: past the screen's edge, on a cylinder just
    // behind it, lined up in the screen's own arc metres.
    float gu = 0, gv = 0;
    const float beyond = cyl.halfWrap + 0.5f * (c.glowHalfW - cyl.halfWidth) / cyl.radius;     // halfway into the glow sideways
    const float glowPoint[3] = { c.glowRadius * std::sin(beyond), 0, cyl.radius - c.glowRadius * std::cos(beyond) };
    const float toGlow[3] = { glowPoint[0] - viewer[0], glowPoint[1] - viewer[1], glowPoint[2] - viewer[2] };
    Check(!CylinderHit(cyl, viewer, toGlow, &tu, &tv), "beside the curved screen is not the screen");
    Check(GlowHit(c, viewer, toGlow, &gu, &gv) && std::fabs(gu - (cyl.radius * beyond / (2 * c.glowHalfW) + 0.5f)) < 1e-4f &&
        std::fabs(gv - 0.5f) < 1e-4f, "beside the curved screen the glow follows the curve");
    Check(glowPoint[2] > 0.5f, "the glow beside the screen curves towards the viewer with it");
    const float straightBack[3] = { 0, 0, -1 };
    Check(GlowHit(c, viewer, straightBack, &gu, &gv) && std::fabs(gu - 0.5f) < 1e-5f, "the glow runs behind the whole screen");
    CurveConstants dark = c;
    dark.glowOn = 0;
    Check(!GlowHit(dark, viewer, toGlow, &gu, &gv), "no glow when the ambilight is off");

    std::vector<unsigned char> picture(8 * 8 * 4, 0), glowPixels(4 * 4 * 4, 0);
    for (size_t i = 0; i < picture.size(); i += 4) { picture[i] = 200; picture[i + 1] = 100; picture[i + 2] = 50; picture[i + 3] = 255; }
    for (size_t i = 0; i < glowPixels.size(); i += 4) { glowPixels[i + 2] = 80; glowPixels[i + 3] = 80; }
    RgbaImage pic{ picture.data(), 8, 8, 32 }, glowImg{ glowPixels.data(), 4, 4, 16 };
    float rgb[3];
    Check(CurvedPixel(c, 0, 32, 24, cyl, pic, &glowImg, rgb) && std::fabs(rgb[0] - 200 / 255.0f) < 1e-5f && std::fabs(rgb[2] - 50 / 255.0f) < 1e-5f,
        "the middle of the eye shows the picture");
    // Pixel (2, 24) looks well beside the screen, into the glow: premultiplied glow
    // over the world colour, blended in linear light as the compositor does.
    const float a80 = 80 / 255.0f;
    Check(CurvedPixel(c, 0, 2, 24, cyl, pic, &glowImg, rgb) &&
        std::fabs(rgb[0] - LinearToSrgb(SrgbToLinear(0.1f) * (1 - a80))) < 1e-5f &&
        std::fabs(rgb[2] - LinearToSrgb(SrgbToLinear(a80) + SrgbToLinear(0.3f) * (1 - a80))) < 1e-5f,
        "beside the screen the glow lies over the world colour, in linear light");
    CurveConstants stored = c;
    stored.linearBlend = 0;
    Check(CurvedPixel(stored, 0, 2, 24, cyl, pic, &glowImg, rgb) && std::fabs(rgb[0] - 0.1f * (1 - a80)) < 1e-5f &&
        std::fabs(rgb[2] - (a80 + 0.3f * (1 - a80))) < 1e-5f, "a plain UNORM swapchain blends the stored values");
    // A glow the colour of the world, premultiplied in linear light, disappears into
    // it - the review's case: premultiplied in encoded values it made a dark ring.
    {
        const float w = 74 / 255.0f, alpha = 128 / 255.0f;
        const unsigned char same = (unsigned char)lroundf(LinearToSrgb(SrgbToLinear(w) * alpha) * 255.0f);
        std::vector<unsigned char> sameGlow(4 * 4 * 4);
        for (size_t i = 0; i < sameGlow.size(); i += 4) { sameGlow[i] = sameGlow[i + 1] = sameGlow[i + 2] = same; sameGlow[i + 3] = 128; }
        RgbaImage sameImg{ sameGlow.data(), 4, 4, 16 };
        CurveConstants grey = c;
        grey.world[0] = grey.world[1] = grey.world[2] = w;
        float over[3];
        Check(CurveSampleColour(grey, 1, 0.5f, 0.5f, pic, &sameImg, over) && std::fabs(over[0] - w) < 2.0f / 255.0f,
            "a glow the colour of the world blends into it without a ring");
    }
    Check(CurvedPixel(c, 0, 32, 0, cyl, pic, &glowImg, rgb) && rgb[0] == 0.1f && rgb[1] == 0.2f && rgb[2] == 0.3f,
        "above the glow is the world colour");
    Check(CurvedPixel(dark, 0, 2, 24, cyl, pic, nullptr, rgb) && rgb[0] == 0.1f && rgb[2] == 0.3f, "with no glow the surround is the world colour");
    Check(!CurvedPixel(c, 2, 0, 0, cyl, pic, &glowImg, rgb) && !CurvedPixel(c, 0, 64, 0, cyl, pic, &glowImg, rgb), "pixels outside the eyes are rejected");
    // Along the middle row, from the picture to the glow, there is an outline pixel
    // that mixes the two - the smooth edge the four samples are for.
    bool mixed = false;
    for (int x = 32; x < 64; x++)
        if (CurvedPixel(c, 0, x, 24, cyl, pic, &glowImg, rgb) && rgb[0] > 0.1f && rgb[0] < 200 / 255.0f - 0.01f) mixed = true;
    Check(mixed, "the screen's outline is smoothed");

    // Ambilight: a picture whose left edge is red at the top and blue at the bottom,
    // and white elsewhere.
    const int sw = 64, sh = 36;
    std::vector<unsigned char> source((size_t)sw * sh * 4, 255);
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < 8; x++)
        {
            unsigned char* q = source.data() + ((size_t)y * sw + x) * 4;
            q[0] = y < sh / 2 ? 255 : 0; q[1] = 0; q[2] = y < sh / 2 ? 0 : 255;
        }
    AmbiConstants a{};
    AmbiRingPoint point;
    float rgba[4] = { 0, 0, 0, 0 };
    Check(!AmbiConstantsUsable(a) && !BuildRingPoint(a, source.data(), sw * 4, 0, point), "the glow needs a size");
    a.gw = 128; a.gh = 96; a.srcW = sw; a.srcH = sh;
    a.screenW = 4.0f; a.screenH = 2.25f; a.marginM = kAmbiMargin * a.screenW;
    a.rectW = a.screenW + 2 * a.marginM; a.rectH = a.screenH + 2 * a.marginM;
    a.intensity = 0.85f; a.soft = kAmbiSoft * a.screenW; a.bezel = kAmbiBezel; a.ringN = kAmbiRing;
    Check(AmbiConstantsUsable(a), "usable glow constants");
    Check(!BuildRingPoint(a, nullptr, sw * 4, 0, point) && !BuildRingPoint(a, source.data(), 4, 0, point) &&
          !BuildRingPoint(a, source.data(), sw * 4, kAmbiRing, point), "bad ring inputs are rejected");
    AmbiConstants tooMany = a;
    tooMany.ringN = kAmbiRingMax + 1;
    Check(!AmbiConstantsUsable(tooMany), "the ring cannot outgrow the shader's shared copy");
    std::vector<AmbiRingPoint> ring(kAmbiRing);
    for (int i = 0; i < kAmbiRing; i++) Check(BuildRingPoint(a, source.data(), sw * 4, (uint32_t)i, ring[i]), "ring point");
    Check(ring[0].y == 0.5f * a.screenH && ring[0].x < -0.5f * a.screenW + 0.1f, "the ring starts at the top-left corner");
    // Ring points on the left edge see red above the middle, blue below.
    bool leftRed = false, leftBlue = false;
    for (const auto& r : ring)
        if (r.x == -0.5f * a.screenW)
        {
            if (r.y > 0.3f && r.rgb[0] > 0.9f && r.rgb[2] < 0.1f) leftRed = true;
            if (r.y < -0.3f && r.rgb[2] > 0.9f && r.rgb[0] < 0.1f) leftBlue = true;
        }
    Check(leftRed && leftBlue, "the ring samples the picture just inside each edge");
    Check(!AmbilightPixel(a, nullptr, 0, 0, rgba) && !AmbilightPixel(a, ring.data(), 128, 0, rgba), "bad glow inputs are rejected");
    Check(AmbilightPixel(a, ring.data(), 64, 48, rgba) && rgba[3] == 0, "behind the screen there is no glow");
    // Left of the screen, above and below the middle: red and blue, and softer (more
    // mixed) further out than close in.
    auto glowAtWith = [&](const AmbiConstants& k, float mx, float my, float out[4])
    {
        const int gx = (int)((mx / k.rectW + 0.5f) * (float)k.gw), gy = (int)((0.5f - my / k.rectH) * (float)k.gh);
        return AmbilightPixel(k, ring.data(), gx, gy, out);
    };
    auto glowAt = [&](float mx, float my, float out[4]) { return glowAtWith(a, mx, my, out); };
    float nearTop[4], farTop[4], nearBottom[4];
    Check(glowAt(-0.5f * a.screenW - 0.15f, 0.7f, nearTop) && glowAt(-0.5f * a.screenW - 0.75f, 0.7f, farTop) &&
          glowAt(-0.5f * a.screenW - 0.15f, -0.7f, nearBottom), "glow beside the screen");
    Check(nearTop[0] > 2 * nearTop[2] && nearBottom[2] > 2 * nearBottom[0], "next to the edge the glow takes the local colour");
    // Colour = the stored value back in linear light, divided by alpha.
    auto hue = [](const float g[4], int ch) { return SrgbToLinear(g[ch]) / g[3]; };
    Check(hue(farTop, 0) < hue(nearTop, 0) && hue(farTop, 2) > hue(nearTop, 2), "further out it blends more of the border: softer");
    Check(nearTop[3] > farTop[3] && nearTop[3] <= 0.85f, "brightness falls off, never above the strength");
    for (int ch = 0; ch < 3; ch++)
        Check(SrgbToLinear(nearTop[ch]) <= nearTop[3] + 1e-6f, "the colour is premultiplied by its own alpha, in linear light");
    AmbiConstants encoded = a;
    encoded.linearBlend = 0;
    float plain[4];
    Check(glowAtWith(encoded, -0.5f * a.screenW - 0.15f, 0.7f, plain) &&
          std::fabs(plain[0] - LinearToSrgb(SrgbToLinear(plain[0] / plain[3]) * plain[3])) > 1e-3f &&
          std::fabs(LinearToSrgb(SrgbToLinear(plain[0] / plain[3]) * plain[3]) - nearTop[0]) < 1e-5f,
        "a plain UNORM swapchain premultiplies the stored colour, an sRGB one premultiplies in linear light");
    // Strength scales brightness only.
    AmbiConstants dim = a;
    dim.intensity = 0.3f;
    float dimTop[4];
    Check(AmbilightPixel(dim, ring.data(), (int)((((-0.5f * a.screenW - 0.15f) / a.rectW) + 0.5f) * (float)a.gw),
                         (int)((0.5f - 0.7f / a.rectH) * (float)a.gh), dimTop) &&
          std::fabs(dimTop[3] / nearTop[3] - 0.3f / 0.85f) < 1e-4f &&
          std::fabs(hue(dimTop, 0) - hue(nearTop, 0)) < 1e-4f, "the strength dims the glow without changing its colour");
    // The bezel: dark right against the screen, bright a little further out.
    float touching[4], clear[4];
    Check(glowAt(-0.5f * a.screenW - 0.01f, 0.0f, touching) && glowAt(-0.5f * a.screenW - 0.2f, 0.0f, clear), "glow either side of the bezel");
    Check(touching[3] < 0.25f * clear[3], "right next to the screen the glow is dark");
    std::vector<unsigned char> texture((size_t)a.gw * a.gh * 4);
    Check(AmbilightReference(a, source.data(), sw * 4, texture.data(), (int)a.gw * 4), "the whole glow");
    Check(!AmbilightReference(a, source.data(), sw * 4, texture.data(), 4), "a short output pitch is rejected");
}


// The form factor (times pi) from (p, n) to a quad, by brute force: the quad cut into
// k x k pieces, each cos cos A / r^2. The exact reference for RoomLambertQuad.
static double BruteG(const float p[3], const float n[3], const float c[4][4], const float nq[3], int k)
{
    double sum = 0;
    for (int a = 0; a < k; a++)
        for (int b = 0; b < k; b++)
        {
            const double fu = (a + 0.5) / k, fv = (b + 0.5) / k;
            double q[3];
            for (int i = 0; i < 3; i++)
                q[i] = c[0][i] + fu * (c[1][i] - c[0][i]) + fv * (c[3][i] - c[0][i]);
            const double v[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
            const double r2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2], r = std::sqrt(r2);
            const double cp = (n[0] * v[0] + n[1] * v[1] + n[2] * v[2]) / r, cq = -(nq[0] * v[0] + nq[1] * v[1] + nq[2] * v[2]) / r;
            if (cp <= 0 || cq <= 0) continue;
            double e1[3], e2[3];
            for (int i = 0; i < 3; i++) { e1[i] = c[1][i] - c[0][i]; e2[i] = c[3][i] - c[0][i]; }
            const double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
            const double dA = std::sqrt(cx * cx + cy * cy + cz * cz) / (k * k);
            sum += cp * cq * dA / r2;
        }
    return sum;
}

static void Quad(RoomEmitter& e, const float c[4][3], const float n[3]) { RoomSetQuad(e, c, n); }

// Point-to-rectangle form factor for a point on the axis of a parallel rectangle
// (closed form), times pi: the golden value for the back wall's middle.
static double ParallelG(double halfW, double halfH, double d)
{
    const double a = halfW / d, b = halfH / d;
    const double F = (2.0 / 3.14159265358979) * (a / std::sqrt(1 + a * a) * std::atan(b / std::sqrt(1 + a * a)) +
                                                  b / std::sqrt(1 + b * b) * std::atan(a / std::sqrt(1 + b * b)));
    return 3.14159265358979 * F;
}

static void TestRoom()
{
    // ---- geometry: the default screen, flat
    RoomInputs in;
    in.W = 5.7f; in.H = 5.7f * 9.0f / 16.0f; in.eye[2] = 3.0f;
    Room room;
    Check(BuildRoom(in, room) && room.valid && !room.curved, "the default room builds");
    Check(std::fabs(2 * room.X - 9.35f) < 0.01f && std::fabs(room.yC - room.yF - 4.66f) < 0.01f && std::fabs(room.zB + room.g - 4.52f) < 0.01f,
        "the default room is 9.35 x 4.66 x 4.52 m");
    Check(room.yF <= -0.5f * in.H - kRoomFloorBelowScreen + 1e-5f && room.yC >= 0.5f * in.H + room.margin - 1e-5f,
        "the floor is below the screen and the whole glow fits under the ceiling");
    Check(room.X - 0.5f * in.W >= room.margin, "the whole glow fits between the side walls");
    Check(RoomInside(room, in.eye) && room.X - std::fabs(in.eye[0]) >= kRoomSideClearance, "the viewer is inside, clear of the walls");
    Check(!room.floorTracked, "no tracked floor: the seated guess");
    RoomInputs tracked = in;
    tracked.floorY = -1.2f;
    Room withFloor;
    Check(BuildRoom(tracked, withFloor) && withFloor.floorTracked && withFloor.yF <= -1.2f, "a tracked floor is used, never above the screen's bottom");
    tracked.floorY = -5.0f;
    Check(BuildRoom(tracked, withFloor) && !withFloor.floorTracked, "an implausible tracked floor falls back to the seated guess");
    RoomInputs offset = in;
    offset.eye[0] = 3.0f;
    Room wide;
    Check(BuildRoom(offset, wide) && RoomInside(wide, offset.eye) && wide.X >= 4.0f, "a 3 m sideways offset keeps the viewer inside");
    RoomInputs behind = in;
    behind.eye[2] = -1.0f;
    Room none;
    Check(!BuildRoom(behind, none), "a viewer behind the screen has no room");
    RoomInputs bad = in;
    bad.W = 0;
    Check(!BuildRoom(bad, none), "a screen with no width has no room");

    // ---- geometry: the default screen at 100% curve
    RoomInputs curvedIn = in;
    Check(BuildCylinder(in.W, in.H, 3.0f, 1.0f, curvedIn.cyl), "cylinder");
    Room curved;
    Check(BuildRoom(curvedIn, curved) && curved.curved && !curved.phiReduced, "the curved room builds with the full arc");
    Check(std::fabs(curved.phiA - (0.5f * in.W + curved.margin) / curved.R) < 1e-5f, "the front's arc reaches as far as the glow");
    Check(std::fabs(FrontDepth(curved, 0.0f) + curved.g) < 1e-5f, "the front sits kAmbiBehind behind the screen's middle");
    {
        const float e = 1e-3f;
        const float left = FrontDepth(curved, curved.xa - e), at = FrontDepth(curved, curved.xa), right = FrontDepth(curved, curved.xa + e);
        Check(std::fabs(at - curved.za) < 1e-4f, "the arc ends where the wing starts");
        Check(std::fabs((at - left) / e - (right - at) / e) < 2e-2f, "the arc turns into the wing smoothly (C1)");
    }
    Check(std::fabs(FrontS(curved, curved.xa) - curved.R * curved.phiA) < 1e-3f, "the front's chart is the glow's arc metres");
    {
        float x, z, nx, nz;
        FrontPoint(curved, FrontS(curved, 2.0f), &x, &z, &nx, &nz);
        Check(std::fabs(x - 2.0f) < 1e-3f && std::fabs(z - FrontDepth(curved, 2.0f)) < 1e-3f, "FrontPoint inverts FrontS on the arc");
        FrontPoint(curved, FrontS(curved, 4.5f), &x, &z, &nx, &nz);
        Check(std::fabs(x - 4.5f) < 1e-3f && std::fabs(z - FrontDepth(curved, 4.5f)) < 1e-3f, "FrontPoint inverts FrontS on a wing");
    }
    RoomInputs smallIn;
    smallIn.W = 1.0f; smallIn.H = 0.5625f; smallIn.eye[0] = 3.0f; smallIn.eye[2] = 1.0f;
    Check(BuildCylinder(1.0f, 0.5625f, 1.0f, 1.0f, smallIn.cyl), "small cylinder");
    Room closeRoom;
    Check(BuildRoom(smallIn, closeRoom) && closeRoom.phiReduced && RoomInside(closeRoom, smallIn.eye), "a small, close, curved screen shortens the arc and keeps the viewer inside");
    Check(std::fmax(FrontDepth(closeRoom, 2.5f), FrontDepth(closeRoom, 3.5f)) <= 1.0f - kRoomFrontClearance + 1e-4f, "the curved front stays half a metre in front of the viewer");
    RoomInputs gentleIn = in;
    Check(BuildCylinder(in.W, in.H, 3.0f, 0.01f, gentleIn.cyl), "gentle cylinder");
    Room gentle;
    Check(BuildRoom(gentleIn, gentle) && std::fabs(FrontDepth(gentle, gentle.X) + gentle.g) < 0.05f, "a 1% curve is nearly flat");

    // ---- rays
    RoomHit hit;
    const float ahead[3] = { 0, 0, -1 }, down[3] = { 0, -1, 0 }, up[3] = { 0, 1, 0 }, leftd[3] = { -1, 0, 0 }, rightd[3] = { 1, 0, 0 }, back[3] = { 0, 0, 1 };
    Check(RoomExit(room, in.eye, ahead, hit) && hit.face == kFaceFront && std::fabs(hit.u - 0.5f) < 1e-5f && std::fabs(hit.s) < 1e-5f, "straight ahead is the front wall's middle");
    Check(RoomExit(room, in.eye, down, hit) && hit.face == kFaceFloor, "down is the floor");
    Check(RoomExit(room, in.eye, up, hit) && hit.face == kFaceCeiling, "up is the ceiling");
    Check(RoomExit(room, in.eye, leftd, hit) && hit.face == kFaceLeft, "left is the left wall");
    Check(RoomExit(room, in.eye, rightd, hit) && hit.face == kFaceRight, "right is the right wall");
    Check(RoomExit(room, in.eye, back, hit) && hit.face == kFaceBack && std::fabs(hit.t - (room.zB - in.eye[2])) < 1e-4f, "behind is the back wall");
    const float outside[3] = { 0, 0, 20 };
    Check(!RoomExit(room, outside, ahead, hit), "a ray from outside the room has no exit");
    const float toWing[3] = { 4.5f, 0, curved.za + (4.5f - curved.xa) * curved.sinA / curved.cosA - 3.0f };
    Check(RoomExit(curved, in.eye, toWing, hit) && hit.face == kFaceFront && hit.s > curved.R * curved.phiA, "towards a wing is the front wall, past the arc");
    Check(RoomExit(curved, in.eye, ahead, hit) && hit.face == kFaceFront && std::fabs(hit.s) < 1e-4f, "a curved room's front middle");
    {
        // Seeded random rays: every exit lies on its face and inside the room's box.
        uint32_t seed = 12345;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)((seed >> 8) & 0xFFFF) / 65535.0f * 2.0f - 1.0f; };
        int onFace = 0;
        const Room* rooms[2] = { &room, &curved };
        for (const Room* r : rooms)
            for (int i = 0; i < 1000; i++)
            {
                const float d[3] = { rnd(), rnd(), rnd() };
                if (!RoomExit(*r, in.eye, d, hit)) continue;
                const float q[3] = { in.eye[0] + hit.t * d[0], in.eye[1] + hit.t * d[1], in.eye[2] + hit.t * d[2] };
                float fp[3], fn[3];
                RoomFacePoint(*r, hit.face, hit.u, hit.v, fp, fn);
                const float err = std::fabs(fp[0] - q[0]) + std::fabs(fp[1] - q[1]) + std::fabs(fp[2] - q[2]);
                const float mid[3] = { 0.5f * (in.eye[0] + q[0]), 0.5f * (in.eye[1] + q[1]), 0.5f * (in.eye[2] + q[2]) };
                if (err < 2e-3f && hit.u >= -1e-4f && hit.u <= 1 + 1e-4f && hit.v >= -1e-4f && hit.v <= 1 + 1e-4f && RoomInside(*r, mid)) onFace++;
            }
        Check(onFace == 2000, "2,000 random rays each leave through a face, at its chart coordinates");
    }

    // ---- form factors
    {
        const float c[4][3] = { { -0.5f, 0.5f, 0 }, { 0.5f, 0.5f, 0 }, { 0.5f, -0.5f, 0 }, { -0.5f, -0.5f, 0 } };
        const float nq[3] = { 0, 0, 1 };
        RoomEmitter e;
        Quad(e, c, nq);
        Check(std::fabs(e.c[0][3] - 1.0f) < 1e-6f && std::fabs(e.c[1][3] - 2.0f) < 1e-6f, "a unit quad's area and squared diagonal");
        struct Case { float p[3], n[3]; const char* what; };
        const Case cases[] = {
            { { 0, 0, 1 }, { 0, 0, -1 }, "facing, 1 m" },
            { { 0.3f, -0.2f, 0.4f }, { 0, 0, -1 }, "facing, close and off-centre" },
            { { 0, -0.8f, 0.6f }, { 0, 1, 0 }, "perpendicular (a floor point under a wall)" },
            { { 0.9f, 0.1f, 0.3f }, { -1, 0, 0 }, "perpendicular, beside the corner" },
            { { 0.2f, 0.1f, 0.05f }, { 0, 0, -1 }, "very close" },
        };
        for (const Case& k : cases)
        {
            const double brute = BruteG(k.p, k.n, e.c, nq, 600);
            const float exact = RoomLambertQuad(k.p, k.n, e.c);
            Check(std::fabs(exact - brute) <= 0.005 * brute + 1e-6, k.what);
        }
        const float onAxis[3] = { 0, 0, 2.0f * std::sqrt(2.0f) }, facing[3] = { 0, 0, -1 };
        Check(std::fabs(RoomFormFactor(onAxis, facing, e) - RoomLambertQuad(onAxis, facing, e.c)) < 0.03f * RoomLambertQuad(onAxis, facing, e.c),
            "the disk formula is within 3% of the exact one where it takes over");
        const float behindIt[3] = { 0, 0, -1 }, sideways[3] = { 2, 0, 0 };
        Check(RoomFormFactor(behindIt, nq, e) == 0 && RoomFormFactor(sideways, facing, e) == 0, "nothing from behind, nothing edge-on");
        const float big[4][3] = { { -500, 500, 0 }, { 500, 500, 0 }, { 500, -500, 0 }, { -500, -500, 0 } };
        RoomEmitter huge;
        Quad(huge, big, nq);
        const float nearIt[3] = { 0, 0, 0.01f };
        Check(std::fabs(RoomFormFactor(nearIt, facing, huge) - kRoomPi) < 0.01f * kRoomPi, "a huge emitter gives pi (the whole hemisphere)");
        // Golden: the back wall's middle, 4.5 m from the default screen as one quad.
        const float hw = 2.85f, hh = 1.6031f;
        const float scr[4][3] = { { -hw, hh, 0 }, { hw, hh, 0 }, { hw, -hh, 0 }, { -hw, -hh, 0 } };
        RoomEmitter screen;
        Quad(screen, scr, nq);
        const float wall[3] = { 0, 0, 4.5f };
        Check(std::fabs(RoomLambertQuad(wall, facing, screen.c) - ParallelG(hw, hh, 4.5)) < 1e-4, "the back wall's middle matches the closed form");
        // Reciprocity: A1 G(1->2) = A2 G(2->1) for two small patches.
        const float a2[4][3] = { { 1, -0.8f, 0.6f }, { 1.1f, -0.8f, 0.6f }, { 1.1f, -0.8f, 0.7f }, { 1, -0.8f, 0.7f } };
        const float up2[3] = { 0, 1, 0 };
        RoomEmitter floorPatch;
        Quad(floorPatch, a2, up2);
        // Both small against their distance, so each centre stands for its patch.
        const float tinyQ[4][3] = { { -0.05f, 0.05f, 0 }, { 0.05f, 0.05f, 0 }, { 0.05f, -0.05f, 0 }, { -0.05f, -0.05f, 0 } };
        RoomEmitter tiny;
        Quad(tiny, tinyQ, nq);
        const float pc[3] = { 1.05f, -0.8f, 0.65f }, qc[3] = { 0, 0, 0 };
        const double g12 = floorPatch.c[0][3] * RoomLambertQuad(pc, up2, tiny.c);
        const double g21 = tiny.c[0][3] * RoomLambertQuad(qc, nq, floorPatch.c);
        Check(g12 > 0 && std::fabs(g12 - g21) < 0.03 * g12, "reciprocity");
    }

    // ---- emitters and light, the flat default room
    const int glowW = 256, glowH = 178;
    const float glowHalfW = 0.5f * in.W + room.margin, glowHalfH = 0.5f * in.H + room.margin;
    const RoomEmitterLayout layout = RoomLayout(in.W, in.H, glowW, glowH);
    Check(layout.gridX == 16 && layout.gridY == 9 && layout.count() <= kRoomMaxEmitters, "16 x 9 screen patches and the glow's blocks");
    std::vector<RoomEmitter> em;
    Check(BuildRoomEmitters(room, in.cyl, in.W, in.H, glowHalfW, glowHalfH, layout, em), "emitters build");
    float screenArea = 0;
    int glowActive = 0;
    for (int i = 0; i < layout.gridX * layout.gridY; i++) screenArea += em[i].c[0][3];
    for (size_t i = (size_t)layout.gridX * layout.gridY; i < em.size(); i++) glowActive += em[i].n[3] != 0;
    Check(std::fabs(screenArea - in.W * in.H) < 1e-2f, "the patches cover the screen exactly");
    Check(glowActive > 200 && glowActive < layout.blocksX * layout.blocksY, "glow blocks inside the screen or the floor are skipped");
    float table[256];
    RoomDecodeTable(table);
    Check(table[0] == 0 && std::fabs(table[255] - 1.0f) < 1e-6f && std::fabs(table[128] - SrgbToLinear(128 / 255.0f)) < 1e-7f, "the decode table");
    {
        float simple[3] = { 0, 0, 0 }, grouped[3];
        for (int k = 0; k < 1000; k++) simple[0] += (float)(k % 7);
        RoomGroupSum(1000, [](int k, float v[3]) { v[0] = (float)(k % 7); v[1] = v[2] = 0; }, grouped);
        Check(std::fabs(grouped[0] - simple[0]) < 1e-3f, "the group sum adds everything once");
    }
    // A white picture, glow off, black world: energy balance and the golden value.
    const int sw = 192, sh = 108;
    std::vector<unsigned char> white((size_t)sw * sh * 4, 255), black((size_t)sw * sh * 4, 0), glowTex((size_t)glowW * glowH * 4, 0);
    for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;
    std::vector<RoomEmitter> lit = em;
    Check(RoomEmitRadiance(layout, white.data(), sw, sh, sw * 4, glowTex.data(), glowW * 4, false, 1.0f, table, lit), "emit white");
    Check(std::fabs(lit[0].L[0] - 1.0f) < 1e-6f && lit[layout.gridX * layout.gridY].L[0] == 0, "a white patch is radiance 1; the glow is off");
    RoomShading shade = MakeRoomShading(room, 50, 0, in.W * in.H);
    Check(std::fabs(shade.rhoWall - 0.3f) < 1e-6f && std::fabs(shade.rhoFloor - 0.18f) < 1e-6f && shade.rhoBar > 0 && shade.rhoBar < 0.3f, "albedos at Room 50%");
    RoomShading noBounce = shade;
    noBounce.rhoBar = 0;
    {
        double received = 0, emitted = kRoomPi * in.W * in.H;
        for (int f = 0; f < kRoomFaces; f++)
        {
            const double texelArea = RoomArea(room, f) / (double)(kRoomLightmap * kRoomLightmap);
            for (int j = 0; j < kRoomLightmap; j++)
                for (int i = 0; i < kRoomLightmap; i++)
                {
                    float L[3];
                    RoomTexel(room, noBounce, lit, f, i, j, L);
                    const float rho = f == kFaceFloor ? noBounce.rhoFloor : (f == kFaceCeiling ? noBounce.rhoCeiling : noBounce.rhoWall);
                    received += (double)L[0] * kRoomPi / rho * texelArea;       // E = L pi / rho
                }
        }
        Check(std::fabs(received / emitted - 1.0) < 0.03, "energy: all the screen's light lands on the room (within 3%)");
        // The back wall's middle, on the screen's axis: the 144 patches together
        // against the closed form for the whole screen.
        const float onAxis[3] = { 0, 0, room.zB }, facing[3] = { 0, 0, -1 };
        double sumG = 0;
        for (int i = 0; i < layout.gridX * layout.gridY; i++) sumG += RoomFormFactor(onAxis, facing, lit[i]);
        const double golden = ParallelG(0.5 * in.W, 0.5 * in.H, room.zB);
        Check(std::fabs(sumG - golden) < 0.02 * golden, "the back wall's middle is lit as the closed form says");
    }
    {
        std::vector<RoomEmitter> dark = em;
        RoomEmitRadiance(layout, black.data(), sw, sh, sw * 4, glowTex.data(), glowW * 4, false, 1.0f, table, dark);
        float L[3];
        RoomTexel(room, MakeRoomShading(room, 50, 0, in.W * in.H), dark, kFaceFloor, 10, 10, L);
        Check(L[0] == 0 && L[1] == 0 && L[2] == 0, "a black picture and a black world light nothing");
        const RoomShading grey = MakeRoomShading(room, 50, 0x404040, in.W * in.H);
        RoomTexel(room, grey, dark, kFaceFloor, 10, 10, L);
        Check(std::fabs(L[0] - kRoomShadeFloor * SrgbToLinear(64 / 255.0f)) < 1e-6f, "with the screen dark the world colour is the house light");
        float c[3];
        RoomTexel(room, grey, dark, kFaceCeiling, 10, 10, c);
        Check(c[0] < L[0], "the ceiling's house light is dimmer than the floor's");
    }
    {
        // Red on the left half of the picture, blue on the right: the left wall is redder.
        std::vector<unsigned char> split = black;
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) { unsigned char* q = &split[((size_t)y * sw + x) * 4]; q[x < sw / 2 ? 0 : 2] = 255; }
        std::vector<RoomEmitter> rb = em, br = em;
        RoomEmitRadiance(layout, split.data(), sw, sh, sw * 4, glowTex.data(), glowW * 4, false, 1.0f, table, rb);
        float leftWall[3], rightWall[3];
        RoomTexel(room, shade, rb, kFaceLeft, 8, 32, leftWall);
        RoomTexel(room, shade, rb, kFaceRight, 8, 32, rightWall);
        Check(leftWall[0] > leftWall[2] && rightWall[2] > rightWall[0], "red on the left lights the left wall red, blue the right");
        // Mirror the picture: the lightmap mirrors (left <-> right, floor flipped).
        std::vector<unsigned char> mirrored = split;
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++)
                memcpy(&mirrored[((size_t)y * sw + x) * 4], &split[((size_t)y * sw + (sw - 1 - x)) * 4], 4);
        RoomEmitRadiance(layout, mirrored.data(), sw, sh, sw * 4, glowTex.data(), glowW * 4, false, 1.0f, table, br);
        float a[3], b[3], fa[3], fb[3];
        RoomTexel(room, shade, rb, kFaceLeft, 20, 30, a);
        RoomTexel(room, shade, br, kFaceRight, 20, 30, b);
        RoomTexel(room, shade, rb, kFaceFloor, 5, 7, fa);
        RoomTexel(room, shade, br, kFaceFloor, kRoomLightmap - 1 - 5, 7, fb);
        Check(std::fabs(a[0] - b[0]) < 1e-4f * (a[0] + 1e-3f) && std::fabs(a[2] - b[2]) < 1e-4f * (a[2] + 1e-3f) &&
              std::fabs(fa[0] - fb[0]) < 1e-4f * (fa[0] + 1e-3f), "a mirrored picture mirrors the room's light");
    }
    {
        // The glow's light: blocks with glow add light near the screen; with the
        // ambilight off they add none.
        std::vector<unsigned char> glowing((size_t)glowW * glowH * 4, 0);
        for (size_t i = 0; i < glowing.size(); i += 4) { glowing[i] = 200; glowing[i + 3] = 200; }
        std::vector<RoomEmitter> withGlow = em, without = em;
        RoomEmitRadiance(layout, black.data(), sw, sh, sw * 4, glowing.data(), glowW * 4, true, 1.0f, table, withGlow);
        RoomEmitRadiance(layout, black.data(), sw, sh, sw * 4, glowing.data(), glowW * 4, false, 1.0f, table, without);
        float gl[3], none3[3];
        RoomTexel(room, shade, withGlow, kFaceFloor, 32, 2, gl);
        RoomTexel(room, shade, without, kFaceFloor, 32, 2, none3);
        Check(gl[0] > 0 && none3[0] == 0, "the glow lights the floor in front of the screen, and nothing when it is off");
    }
    {
        // Temporal blend: alpha 1 takes the new picture; a smaller alpha moves part way.
        std::vector<RoomEmitter> hist = em;
        RoomEmitRadiance(layout, white.data(), sw, sh, sw * 4, glowTex.data(), glowW * 4, false, 1.0f, table, hist);
        RoomEmitRadiance(layout, black.data(), sw, sh, sw * 4, glowTex.data(), glowW * 4, false, 0.25f, table, hist);
        Check(std::fabs(hist[0].L[0] - 0.75f) < 1e-6f, "the screen's light blends towards a new picture");
    }

    // ---- the emitter budget: every picture shape fits, the glow's blocks growing as needed
    {
        struct Shape { int w, h; const char* what; };
        const Shape shapes[] = { { 2560, 1080, "21:9" }, { 1920, 1080, "16:9" }, { 1920, 1200, "16:10" }, { 1440, 1080, "4:3" },
                                 { 1280, 1024, "5:4" }, { 1080, 1080, "1:1" }, { 1080, 1920, "9:16" } };
        int fits = 0;
        for (const Shape& k : shapes)
        {
            int gw = 0, gh = 0;
            AmbiSizeFor(k.w, k.h, &gw, &gh);
            const RoomEmitterLayout l = RoomLayout((float)k.w, (float)k.h, gw, gh);
            const bool covers = l.blocksX * l.block >= gw && l.blocksY * l.block >= gh &&
                                (l.blocksX - 1) * l.block < gw && (l.blocksY - 1) * l.block < gh;
            if (l.count() > 0 && l.count() <= kRoomMaxEmitters && l.block >= kRoomGlowBlock && l.block <= kRoomMaxGlowBlock && covers) fits++;
            else std::printf("room layout for %s (glow %dx%d): %d emitters, %d px blocks\n", k.what, gw, gh, l.count(), l.block);
        }
        Check(fits == (int)(sizeof(shapes) / sizeof(shapes[0])), "every picture shape's emitters fit the budget, and its blocks cover the glow exactly");
        int gw = 0, gh = 0;
        AmbiSizeFor(1920, 1080, &gw, &gh);
        Check(gw == glowW && gh == glowH, "the tests' 16:9 glow is the real one");
        Check(RoomLayout(16, 9, gw, gh).block == kRoomGlowBlock && RoomLayout(16, 9, gw, gh).count() == 880, "a 16:9 picture keeps 8-texel glow blocks (880 emitters)");
        AmbiSizeFor(1440, 1080, &gw, &gh);
        Check(RoomLayout(4, 3, gw, gh).block == 16, "a 4:3 picture's glow blocks grow to 16 texels (at 8 they passed the budget)");
        Check(RoomLayout(16, 9, glowW, glowH, 16).block == 16, "larger blocks can be asked for");
        Check(RoomLayout(16, 9, 256, 100000).count() == 0, "a glow too tall for any block size has no layout");

        // 16-texel blocks, the partial ones at the edges too, average exactly their own texels.
        RoomInputs tallIn;
        tallIn.W = 4.0f; tallIn.H = 3.0f; tallIn.eye[2] = 3.0f;
        Room tallRoom;
        const RoomEmitterLayout tl = RoomLayout(tallIn.W, tallIn.H, gw, gh);
        std::vector<RoomEmitter> tem;
        Check(BuildRoom(tallIn, tallRoom) && BuildRoomEmitters(tallRoom, tallIn.cyl, tallIn.W, tallIn.H, 0.5f * tallIn.W + tallRoom.margin,
              0.5f * tallIn.H + tallRoom.margin, tl, tem) && (int)tem.size() == tl.count(), "a 4:3 picture's room and emitters build");
        std::vector<unsigned char> ramp((size_t)gw * gh * 4, 0), dark((size_t)64 * 48 * 4, 0);
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
            {
                unsigned char* q = &ramp[((size_t)y * gw + x) * 4];
                q[0] = (unsigned char)x; q[1] = (unsigned char)(y & 255); q[3] = 255;
            }
        for (size_t i = 3; i < dark.size(); i += 4) dark[i] = 255;
        Check(RoomEmitRadiance(tl, dark.data(), 64, 48, 64 * 4, ramp.data(), gw * 4, true, 1.0f, table, tem), "emit a 4:3 picture");
        int active = 0, right = 0;
        for (int by = 0; by < tl.blocksY; by++)
            for (int bx = 0; bx < tl.blocksX; bx++)
            {
                const RoomEmitter& e = tem[(size_t)tl.gridX * tl.gridY + (size_t)by * tl.blocksX + bx];
                if (e.n[3] == 0) continue;
                active++;
                double sx = 0, sy = 0;
                int count = 0;
                for (int y = by * tl.block; y < std::min((by + 1) * tl.block, gh); y++)
                    for (int x = bx * tl.block; x < std::min((bx + 1) * tl.block, gw); x++) { sx += table[x]; sy += table[y & 255]; count++; }
                if (std::fabs(e.L[0] - sx / count) < 1e-5 && std::fabs(e.L[1] - sy / count) < 1e-5) right++;
            }
        Check(active > 0 && right == active, "16-texel glow blocks, the edge ones too, take the mean of their own texels");
    }

    // ---- the curved room: its floor area, and no seam where the floor meets the front
    {
        const float fullArea = 2.0f * curved.X * (curved.zB + curved.g);
        double stripArea = 0;
        const int n = 2000;
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++)
            {
                const float x = -curved.X + ((float)i + 0.5f) * 2.0f * curved.X / n, z = -curved.g + ((float)j + 0.5f) * (curved.zB + curved.g) / n;
                if (z < FrontDepth(curved, x)) stripArea += 1;
            }
        stripArea *= (double)fullArea / ((double)n * n);
        Check(curved.frontFloorArea > 0 && std::fabs(curved.frontFloorArea - stripArea) < 0.01 * stripArea,
            "the floor between the screen's plane and a curved front is measured (within 1%)");
        Check(std::fabs(RoomArea(curved, kFaceFloor) - (fullArea - curved.frontFloorArea)) < 1e-4f &&
              std::fabs(RoomArea(curved, kFaceCeiling) - RoomArea(curved, kFaceFloor)) < 1e-6f && room.frontFloorArea == 0 &&
              std::fabs(RoomArea(room, kFaceFloor) - 2.0f * room.X * (room.zB + room.g)) < 1e-4f,
            "a curved room's floor and ceiling leave that strip out; a flat room's are whole");

        // Every lightmap texel is lit from inside the room.
        int outsideRoom = 0, nudged = 0;
        for (int f = 0; f < kRoomFaces; f++)
            for (int j = 0; j < kRoomLightmap; j++)
                for (int i = 0; i < kRoomLightmap; i++)
                {
                    float p[3], nn[3], raw[3];
                    RoomLightPoint(curved, f, i, j, p, nn);
                    RoomFacePoint(curved, f, ((float)i + 0.5f) / kRoomLightmap, ((float)j + 0.5f) / kRoomLightmap, raw, nn);
                    if (p[2] < FrontDepth(curved, p[0]) - 1e-4f) outsideRoom++;
                    if (p[2] != raw[2]) nudged++;
                }
        Check(outsideRoom == 0 && nudged > 0, "every curved-room lightmap texel is lit from inside the room");

        // The glow on the front wall lights the floor at its base. A texel behind the wall
        // was lit from out there - from behind the glow, so black - and bilinear sampling
        // blended it into the base of the wall as a dark sawtooth.
        const float cHalfW = 0.5f * in.W + curved.margin, cHalfH = 0.5f * in.H + curved.margin;
        std::vector<RoomEmitter> cem;
        Check(BuildRoomEmitters(curved, curvedIn.cyl, in.W, in.H, cHalfW, cHalfH, layout, cem), "curved emitters build");
        std::vector<unsigned char> glowing((size_t)glowW * glowH * 4, 0);
        for (size_t i = 0; i < glowing.size(); i += 4) { glowing[i] = 200; glowing[i + 3] = 200; }
        RoomEmitRadiance(layout, black.data(), sw, sh, sw * 4, glowing.data(), glowW * 4, true, 1.0f, table, cem);
        const RoomShading cshade = MakeRoomShading(curved, 50, 0, in.W * in.H);
        int pairs = 0, smooth = 0;
        float worst = 1e9f;
        for (int i = 0; i < kRoomLightmap; i++)
            for (int j = 0; j + 1 < kRoomLightmap; j++)
            {
                float a[3], b[3], na[3], nb[3];
                RoomFacePoint(curved, kFaceFloor, ((float)i + 0.5f) / kRoomLightmap, ((float)j + 0.5f) / kRoomLightmap, a, na);
                RoomFacePoint(curved, kFaceFloor, ((float)i + 0.5f) / kRoomLightmap, ((float)j + 1.5f) / kRoomLightmap, b, nb);
                if (!(a[2] < FrontDepth(curved, a[0])) || !(b[2] > FrontDepth(curved, b[0]))) continue;   // j behind, j+1 inside
                float La[3], Lb[3];
                RoomTexel(curved, cshade, cem, kFaceFloor, i, j, La);
                RoomTexel(curved, cshade, cem, kFaceFloor, i, j + 1, Lb);
                pairs++;
                if (Lb[0] > 0 && La[0] >= 0.5f * Lb[0]) smooth++;
                if (Lb[0] > 0) worst = std::min(worst, La[0] / Lb[0]);
            }
        if (smooth != pairs) std::printf("curved room seam: %d of %d floor columns smooth, worst ratio %.3f\n", smooth, pairs, worst);
        Check(pairs >= kRoomLightmap / 2 && smooth == pairs, "where the floor meets a curved front, the texel behind it is lit like its neighbour inside");

        // Energy, curved: all the screen's light lands on the room. Floor and ceiling
        // texels behind the front are outside the room and are left out.
        std::vector<RoomEmitter> clit = cem;
        RoomEmitRadiance(layout, white.data(), sw, sh, sw * 4, glowTex.data(), glowW * 4, false, 1.0f, table, clit);
        RoomShading cNoBounce = cshade;
        cNoBounce.rhoBar = 0;
        double received = 0, emitted = 0;
        for (int k = 0; k < layout.gridX * layout.gridY; k++) emitted += kRoomPi * clit[k].c[0][3];
        for (int f = 0; f < kRoomFaces; f++)
        {
            const bool strip = f == kFaceFloor || f == kFaceCeiling;
            const double texelArea = (strip ? fullArea : RoomArea(curved, f)) / (double)(kRoomLightmap * kRoomLightmap);
            for (int j = 0; j < kRoomLightmap; j++)
                for (int i = 0; i < kRoomLightmap; i++)
                {
                    float p[3], nn[3], L[3];
                    RoomFacePoint(curved, f, ((float)i + 0.5f) / kRoomLightmap, ((float)j + 0.5f) / kRoomLightmap, p, nn);
                    if (strip && p[2] < FrontDepth(curved, p[0])) continue;
                    RoomTexel(curved, cNoBounce, clit, f, i, j, L);
                    const float rho = f == kFaceFloor ? cNoBounce.rhoFloor : (f == kFaceCeiling ? cNoBounce.rhoCeiling : cNoBounce.rhoWall);
                    received += (double)L[0] * kRoomPi / rho * texelArea;
                }
        }
        if (std::fabs(received / emitted - 1.0) >= 0.03) std::printf("curved room energy: received / emitted = %.4f\n", received / emitted);
        Check(std::fabs(received / emitted - 1.0) < 0.03, "energy, curved: all the screen's light lands on the room (within 3%)");
    }

    // ---- dither, half floats, heading
    {
        double sum = 0;
        bool differ = false;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
            {
                sum += RoomDither(0, x, y);
                if (RoomDither(0, x, y) != RoomDither(1, x, y)) differ = true;
                Check(std::fabs(RoomDither(0, x, y)) < 0.5f / 255.0f, "the dither stays within half a step");
            }
        Check(std::fabs(sum) < 1e-6 && differ, "the dither averages to zero and differs between the eyes");
    }
    Check(RoomHalfToFloat(0x3C00) == 1.0f && RoomHalfToFloat(0x3800) == 0.5f && RoomHalfToFloat(0) == 0.0f &&
          std::fabs(RoomHalfToFloat(0x0001) - 5.96046448e-8f) < 1e-12f && RoomHalfToFloat(0xC000) == -2.0f, "half floats");
    {
        const float yaw = 0.7f;
        const XrQuaternionf pure{ 0, std::sin(0.5f * yaw), 0, std::cos(0.5f * yaw) };
        const XrQuaternionf keep = YawOnly(pure);
        Check(std::fabs(keep.y - pure.y) < 1e-5f && std::fabs(keep.w - pure.w) < 1e-5f, "a level heading is kept");
        // Pitch down 30 degrees after the yaw: q = yaw * pitch.
        const float pitch = -0.52f;
        const float sy = std::sin(0.5f * yaw), cy = std::cos(0.5f * yaw), sp = std::sin(0.5f * pitch), cp = std::cos(0.5f * pitch);
        const XrQuaternionf tilted{ cy * sp, sy * cp, -sy * sp, cy * cp };
        const XrQuaternionf level = YawOnly(tilted);
        Check(std::fabs(level.x) < 1e-6f && std::fabs(level.z) < 1e-6f && std::fabs(level.y - sy) < 1e-4f && std::fabs(level.w - cy) < 1e-4f,
            "pitch is removed, the heading kept");
        const XrQuaternionf straightDown{ -std::sin(0.785398f), 0, 0, std::cos(0.785398f) };
        const XrQuaternionf fromUp = YawOnly(straightDown);
        Check(std::fabs(fromUp.y) < 1e-5f && std::fabs(std::fabs(fromUp.w) - 1.0f) < 1e-5f, "looking straight down keeps the heading the head's top points at");
    }
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
    TestRoom();
    std::puts("PASS: room geometry, rays and light, curved screen geometry, ambilight constants, game/terminal capture selection, stationary screen/recenter/stereo calibration, tracking validity, swapchain failures, depth fallback/recovery, D3D11 resize pixels and bars, window client-area crop, capture downscale, depth GPU choice");
}
