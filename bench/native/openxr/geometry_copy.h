// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Geometry pairs are already stereo. Copy each half into its own picture slice;
// never estimate depth or apply the AI stereo warp to these pixels.
#pragma once
#include <d3d12.h>

inline void GeometryBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    cl->ResourceBarrier(1, &b);
}

// Source starts at COMMON, or COPY_DEST immediately after a CPU source upload.
// Picture pair starts at UAV; mono lighting starts at COMMON. On return: source
// COMMON, pair COPY_SOURCE, lighting NON_PIXEL_SHADER_RESOURCE. The caller resets
// the latter two after presentation/lighting commands have been recorded.
inline bool RecordGeometryCopy(ID3D12GraphicsCommandList* cl, ID3D12Resource* source,
    ID3D12Resource* pictures, ID3D12Resource* lighting, UINT eyeWidth, UINT eyeHeight,
    bool stereo, bool uploaded)
{
    if (!cl || !source || !pictures || !lighting || !eyeWidth || !eyeHeight) return false;
    const auto s = source->GetDesc(), p = pictures->GetDesc(), l = lighting->GetDesc();
    if (s.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || p.Dimension != s.Dimension || l.Dimension != s.Dimension ||
        s.Width != UINT64(eyeWidth) * 2 || s.Height != eyeHeight || s.DepthOrArraySize != 1 ||
        p.Width != eyeWidth || p.Height != eyeHeight || p.DepthOrArraySize != 2 ||
        l.Width != eyeWidth || l.Height != eyeHeight || l.DepthOrArraySize != 1 ||
        s.MipLevels != 1 || p.MipLevels != 1 || l.MipLevels != 1 ||
        s.SampleDesc.Count != 1 || p.SampleDesc.Count != 1 || l.SampleDesc.Count != 1 ||
        s.Format != DXGI_FORMAT_R8G8B8A8_UNORM || p.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        l.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    GeometryBarrier(cl, source, uploaded ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    GeometryBarrier(cl, pictures, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    GeometryBarrier(cl, lighting, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = pictures; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    for (UINT eye = 0; eye < 2; ++eye)
    {
        dst.SubresourceIndex = eye;
        const UINT start = stereo ? eye * eyeWidth : 0;
        const D3D12_BOX box{start, 0, 0, start + eyeWidth, eyeHeight, 1};
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    dst.pResource = lighting; dst.SubresourceIndex = 0;
    const D3D12_BOX left{0, 0, 0, eyeWidth, eyeHeight, 1};
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &left);
    GeometryBarrier(cl, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    GeometryBarrier(cl, pictures, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    GeometryBarrier(cl, lighting, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}
