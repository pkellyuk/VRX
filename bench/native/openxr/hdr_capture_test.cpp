#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture_scaler.h"
#include "hdr_capture.h"
#include "screen_anchor.h"
#include <DirectXPackedVector.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Hr(HRESULT hr) { Check(SUCCEEDED(hr), "D3D11 call failed"); }

static Microsoft::WRL::ComPtr<ID3D11Texture2D> FloatImage(ID3D11Device* device, int w, int h,
    const std::vector<std::array<float, 4>>& rgba)
{
    Check(rgba.size() == size_t(w) * h, "float test image has the requested shape");
    std::vector<uint16_t> half(rgba.size() * 4);
    for (size_t i = 0; i < rgba.size(); ++i) for (int c = 0; c < 4; ++c)
        half[i * 4 + c] = DirectX::PackedVector::XMConvertFloatToHalf(rgba[i][c]);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w; desc.Height = h; desc.ArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data{half.data(), UINT(w * 8), 0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> result;
    Hr(device->CreateTexture2D(&desc, &data, &result));
    return result;
}

static std::vector<unsigned char> ScaledPixels(CaptureScaler& scaler, ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* input, int sourceW, int sourceH, int targetW, int targetH, float white = 1)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = targetW; desc.Height = targetH; desc.ArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output, readback;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    Hr(device->CreateTexture2D(&desc, nullptr, &output));
    Hr(device->CreateRenderTargetView(output.Get(), nullptr, &target));
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Hr(device->CreateTexture2D(&desc, nullptr, &readback));
    Hr(scaler.Copy(device, context, input, sourceW, sourceH, output.Get(), target.Get(), targetW, targetH, nullptr, white));
    context->CopyResource(readback.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Hr(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<unsigned char> pixels(size_t(targetW) * targetH * 4);
    for (int y = 0; y < targetH; ++y)
        std::memcpy(pixels.data() + size_t(y) * targetW * 4,
            static_cast<const unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(targetW) * 4);
    context->Unmap(readback.Get(), 0);
    return pixels;
}

int main()
{
    float previous = -1;
    for (int i = 0; i <= 10000; ++i)
    {
        float r = i / 1000.0f, g = r, b = r;
        HdrToneMap(r, g, b);
        Check(r + 1e-6f >= previous && r >= 0 && r <= 1.00001f, "HDR shoulder is bounded and monotonic");
        previous = r;
    }
    float r = -1, g = std::numeric_limits<float>::infinity(), b = std::numeric_limits<float>::quiet_NaN();
    HdrToneMap(r, g, b);
    Check(r == 0 && g == 0 && b == 0, "invalid HDR input cannot contaminate model input");
    XrPosef origin{{0,0,0,1}, {1,2,3}};
    Check(PictureHeadOffset(origin, origin.orientation, origin.position, 1) == 0, "recenter has zero extra parallax");
    Check(PictureHeadOffset(origin, origin.orientation, {2,2,3}, 0) == 0, "disabled parallax is exact zero");
    Check(PictureHeadOffset(origin, origin.orientation, {2,2,3}, 1) <= 0.025f, "parallax is bounded");
    Check(PictureHeadOffset(origin, origin.orientation, {0,2,3}, 1) >= -0.025f, "negative parallax is bounded");
    const float half = std::sqrt(0.5f);
    Check(std::fabs(PictureHeadOffset(origin, {0,half,0,half}, {1.1f,2,3}, 1)) < 1e-6f, "screen local axis is used");
    Check(PictureHeadOffset(origin, {0,half,0,half}, {1,2,2.99f}, 1) > 0, "rotated screen parallax sign");

    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    CaptureScaler scaler; Hr(scaler.Init(device.Get()));
    const float values[8] = {0, 0.125f, 0.5f, 0.75f, 1, 2, 4, 16};
    std::vector<uint16_t> pixels(8*2*4);
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 8; ++x)
    {
        auto* p = pixels.data() + (y*8+x)*4;
        p[0] = DirectX::PackedVector::XMConvertFloatToHalf(values[x]);
        p[1] = DirectX::PackedVector::XMConvertFloatToHalf(values[x]*0.5f);
        p[2] = DirectX::PackedVector::XMConvertFloatToHalf(values[x]*0.25f);
        p[3] = DirectX::PackedVector::XMConvertFloatToHalf(1);
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 8; td.Height = 2; td.ArraySize = td.MipLevels = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), 8*8, 0};
    ComPtr<ID3D11Texture2D> input; Hr(device->CreateTexture2D(&td, &data, &input));
    for (float white : {1.0f, 2.5f}) for (bool crop : {false, true})
    {
        const int width = crop ? 4 : 8;
        td.Width = width; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET; td.CPUAccessFlags = 0;
        ComPtr<ID3D11Texture2D> output, rb;
        ComPtr<ID3D11RenderTargetView> rtv;
        Hr(device->CreateTexture2D(&td, nullptr, &output));
        Hr(device->CreateRenderTargetView(output.Get(), nullptr, &rtv));
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Hr(device->CreateTexture2D(&td, nullptr, &rb));
        RECT region{2,0,6,2};
        Hr(scaler.Copy(device.Get(), context.Get(), input.Get(), 8, 2, output.Get(), rtv.Get(), width, 2, crop ? &region : nullptr, white));
        context->CopyResource(rb.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{}; Hr(context->Map(rb.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        for (int y = 0; y < 2; ++y) for (int x = 0; x < width; ++x)
        {
            float rr = values[x+(crop ? 2 : 0)], gg = rr*0.5f, bb = rr*0.25f;
            HdrToneMap(rr, gg, bb, white);
            const unsigned char* p = static_cast<unsigned char*>(mapped.pData) + y*mapped.RowPitch + x*4;
            Check(std::abs(int(p[0])-int(std::lround(bb*255))) <= 1 &&
                  std::abs(int(p[1])-int(std::lround(gg*255))) <= 1 &&
                  std::abs(int(p[2])-int(std::lround(rr*255))) <= 1 && p[3] == 255,
                  "HDR GPU tone map matches float reference, including crop/white level");
        }
        context->Unmap(rb.Get(), 0);
    }
    // Exercise non-finite FP16 values through the actual GPU sampler and shader.
    // A valid colour channel must survive even when the other two are invalid.
    for (float red : {-1.0f, 0.5f})
    {
        auto invalid = FloatImage(device.Get(), 2, 2, std::vector<std::array<float, 4>>(4,
            {red, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 1}));
        const auto gpu = ScaledPixels(scaler, device.Get(), context.Get(), invalid.Get(), 2, 2, 2, 2);
        float expectedR = red, expectedG = std::numeric_limits<float>::infinity(), expectedB = std::numeric_limits<float>::quiet_NaN();
        HdrToneMap(expectedR, expectedG, expectedB);
        for (size_t i = 0; i < gpu.size(); i += 4)
            Check(gpu[i] == 0 && gpu[i + 1] == 0 && std::abs(int(gpu[i + 2]) - int(std::lround(expectedR * 255))) <= 1 && gpu[i + 3] == 255,
                "GPU HDR input rejects invalid channels without damaging valid channels");
    }

    // A resized HDR window must preserve the fitted area and black letterbox bars.
    // Each destination pixel averages an adjacent pair of source values in linear
    // scRGB before tone mapping: averaging already encoded values would be wrong.
    std::vector<std::array<float, 4>> wide(8 * 4);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x)
        wide[y * 8 + x] = {values[x], values[x] * 0.5f, values[x] * 0.25f, 1};
    auto wideImage = FloatImage(device.Get(), 8, 4, wide);
    const auto bars = ScaledPixels(scaler, device.Get(), context.Get(), wideImage.Get(), 8, 4, 4, 6, 2.5f);
    for (int y = 0; y < 6; ++y) for (int x = 0; x < 4; ++x)
    {
        const auto* pixel = bars.data() + (y * 4 + x) * 4;
        if (y < 2 || y >= 4)
            Check(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255, "HDR resized bars stay opaque black");
        else
        {
            float rr = (values[2 * x] + values[2 * x + 1]) * 0.5f, gg = rr * 0.5f, bb = rr * 0.25f;
            HdrToneMap(rr, gg, bb, 2.5f);
            Check(std::abs(int(pixel[0]) - int(std::lround(bb * 255))) <= 1 &&
                std::abs(int(pixel[1]) - int(std::lround(gg * 255))) <= 1 &&
                std::abs(int(pixel[2]) - int(std::lround(rr * 255))) <= 1 && pixel[3] == 255,
                "resized HDR samples stay linear until one calibrated tone map");
        }
    }

    // The same scaler can see a format change without a size change. Force the SDR
    // shader path by resizing too, so a stale FP16 staging texture cannot hide behind
    // the exact-size SDR CopySubresourceRegion shortcut.
    auto floating = FloatImage(device.Get(), 2, 2, std::vector<std::array<float, 4>>(4, {0.5f, 0.25f, 0.125f, 1}));
    std::array<unsigned char, 16> sdrPixels{17, 59, 201, 255, 17, 59, 201, 255, 17, 59, 201, 255, 17, 59, 201, 255};
    D3D11_TEXTURE2D_DESC sdrDesc{};
    sdrDesc.Width = sdrDesc.Height = 2; sdrDesc.ArraySize = sdrDesc.MipLevels = 1; sdrDesc.SampleDesc.Count = 1;
    sdrDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sdrDesc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA sdrData{sdrPixels.data(), 8, 0};
    ComPtr<ID3D11Texture2D> sdrImage;
    Hr(device->CreateTexture2D(&sdrDesc, &sdrData, &sdrImage));
    const auto hdrFirst = ScaledPixels(scaler, device.Get(), context.Get(), floating.Get(), 2, 2, 4, 4);
    const auto sdrMiddle = ScaledPixels(scaler, device.Get(), context.Get(), sdrImage.Get(), 2, 2, 4, 4);
    const auto hdrLast = ScaledPixels(scaler, device.Get(), context.Get(), floating.Get(), 2, 2, 4, 4);
    Check(hdrFirst == hdrLast, "HDR staging and tone mapping recover after an SDR format switch");
    for (size_t i = 0; i < sdrMiddle.size(); ++i)
        Check(sdrMiddle[i] == sdrPixels[i % 4], "same-size source format changes retain exact SDR colours");
    std::puts("PASS: HDR shader/reference, crop, SDR white calibration, linear resize and bars, GPU finite inputs, SDR/HDR format changes, head parallax");
}
