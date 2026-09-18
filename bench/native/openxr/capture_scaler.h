#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include "playback_policy.h"

// Keeps shared ring textures at a stable size. Resized input is fitted with black
// bars, without touching resources or descriptors used by the D3D12 readers.
class CaptureScaler
{
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Texture2D> input_;
    Ptr<ID3D11ShaderResourceView> srv_;
    Ptr<ID3D11VertexShader> vs_;
    Ptr<ID3D11PixelShader> ps_;
    Ptr<ID3D11SamplerState> sampler_;
    Ptr<ID3D11RasterizerState> raster_;
    int width_ = 0, height_ = 0;
public:
    HRESULT Init(ID3D11Device* device)
    {
        const char* shader = R"(
struct V { float4 p : SV_POSITION; float2 uv : TEXCOORD; };
V VS(uint id : SV_VertexID) {
    V v; v.uv = float2((id << 1) & 2, id & 2);
    v.p = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1); return v;
}
Texture2D picture : register(t0); SamplerState linearClamp : register(s0);
float4 PS(V v) : SV_TARGET { return float4(picture.Sample(linearClamp, v.uv).rgb, 1); }
)";
        Ptr<ID3DBlob> vs, ps, errors;
        HRESULT hr = D3DCompile(shader, std::strlen(shader), "capture-scaler", nullptr, nullptr,
            "VS", "vs_5_0", 0, 0, &vs, &errors);
        if (FAILED(hr)) return hr;
        hr = D3DCompile(shader, std::strlen(shader), "capture-scaler", nullptr, nullptr,
            "PS", "ps_5_0", 0, 0, &ps, &errors);
        if (FAILED(hr)) return hr;
        hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
        if (FAILED(hr)) return hr;
        hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_);
        if (FAILED(hr)) return hr;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sd, &sampler_);
        if (FAILED(hr)) return hr;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        return device->CreateRasterizerState(&rd, &raster_);
    }

    // w x h is the valid captured content; `region`, if given, selects part of it
    // (e.g. a window's client area, excluding its frame and title bar).
    HRESULT Copy(ID3D11Device* device, ID3D11DeviceContext* context,
        ID3D11Texture2D* captured, int w, int h, ID3D11Texture2D* target,
        ID3D11RenderTargetView* rtv, int targetW, int targetH, const RECT* region = nullptr)
    {
        D3D11_TEXTURE2D_DESC desc{}; captured->GetDesc(&desc);
        if (w <= 0 || h <= 0 || UINT(w) > desc.Width || UINT(h) > desc.Height) return E_INVALIDARG;
        D3D11_BOX box{ 0, 0, 0, UINT(w), UINT(h), 1 };
        if (region)
        {
            if (region->left < 0 || region->top < 0 || region->right > w || region->bottom > h ||
                region->left >= region->right || region->top >= region->bottom) return E_INVALIDARG;
            box = { UINT(region->left), UINT(region->top), 0, UINT(region->right), UINT(region->bottom), 1 };
            w = region->right - region->left;
            h = region->bottom - region->top;
        }
        if (w == targetW && h == targetH)
        {
            context->CopySubresourceRegion(target, 0, 0, 0, 0, captured, 0, &box);
            return S_OK;
        }
        if (w != width_ || h != height_)
        {
            srv_.Reset(); input_.Reset(); width_ = height_ = 0;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = UINT(w); td.Height = UINT(h); td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            HRESULT hr = device->CreateTexture2D(&td, nullptr, &input_);
            if (FAILED(hr)) return hr;
            hr = device->CreateShaderResourceView(input_.Get(), nullptr, &srv_);
            if (FAILED(hr)) return hr;
            width_ = w; height_ = h;
        }
        context->CopySubresourceRegion(input_.Get(), 0, 0, 0, 0, captured, 0, &box);
        const float black[4] = { 0, 0, 0, 1 };
        context->ClearRenderTargetView(rtv, black);
        const auto rect = FitCapture(w, h, targetW, targetH);
        D3D11_VIEWPORT vp{ rect.x, rect.y, rect.width, rect.height, 0, 1 };
        context->RSSetViewports(1, &vp);
        context->RSSetState(raster_.Get());
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs_.Get(), nullptr, 0);
        context->PSSetShader(ps_.Get(), nullptr, 0);
        auto* srv = srv_.Get(); auto* sampler = sampler_.Get();
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, &sampler);
        context->Draw(3, 0);
        // Unbind before the next copy and before handing the output to D3D12.
        srv = nullptr; context->PSSetShaderResources(0, 1, &srv);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        return S_OK;
    }
};
