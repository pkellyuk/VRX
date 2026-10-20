#!/usr/bin/env python3
# Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
"""Derive COM method indices from the installed Microsoft SDK declarations.

No driver layout guesses or copied hook tables. Repeated declarations under
SDK ABI conditionals describe one COM slot and are counted once.
"""
import argparse
import re
from pathlib import Path


def slots(source, interface):
    declarations = {}
    for match in re.finditer(r"\b((?:ID3D12|IDXGI)\w+)\s*:\s*public\s+(\w+)\s*\{\s*public:(.*?)\n\s*\};", source, re.S):
        name, parent, body = match.groups()
        methods = list(dict.fromkeys(re.findall(r"virtual\s+[^;]*?STDMETHODCALLTYPE\s+(\w+)\s*\(", body)))
        declarations[name] = (parent, methods)

    def inherit(name):
        if name == "IUnknown":
            return ["QueryInterface", "AddRef", "Release"]
        parent, methods = declarations[name]
        return inherit(parent) + methods

    return {method: i for i, method in enumerate(inherit(interface))}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("header", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.header.read_text(encoding="utf-8-sig")
    shared = args.header.parent.parent / "shared"
    source += "\n".join(p.read_text(encoding="utf-8-sig") for p in sorted(shared.glob("dxgi*.h")))
    selected = {
        "ID3D12Device": ["CreateCommandQueue", "CreateCommandAllocator", "CreateCommandList", "CreateGraphicsPipelineState", "CreateCommittedResource", "CreateDescriptorHeap", "CreateRenderTargetView", "CreateDepthStencilView", "CreateConstantBufferView", "CreateShaderResourceView", "CreateUnorderedAccessView", "CreateSampler", "CopyDescriptors", "CopyDescriptorsSimple", "CreateComputePipelineState", "CreateHeap", "CreatePlacedResource", "CreateReservedResource"],
        "ID3D12CommandAllocator": ["Reset"],
        "ID3D12Device2": ["CreatePipelineState"],
        "ID3D12Device4": ["CreateCommandList1", "CreateCommittedResource1", "CreateHeap1"],
        "ID3D12Device8": ["CreateCommittedResource2", "CreatePlacedResource1"],
        "ID3D12CommandQueue": ["ExecuteCommandLists"],
        "IDXGIFactory": ["CreateSwapChain"],
        "IDXGIFactory2": ["CreateSwapChainForHwnd", "CreateSwapChainForCoreWindow", "CreateSwapChainForComposition"],
        "IDXGISwapChain": ["Present", "GetBuffer", "ResizeBuffers"],
        "IDXGISwapChain1": ["Present1"],
        "IDXGISwapChain3": ["ResizeBuffers1"],
        "ID3D12GraphicsCommandList": ["Close", "Reset", "DrawInstanced", "DrawIndexedInstanced", "ExecuteBundle", "ClearState", "SetPipelineState", "RSSetViewports", "RSSetScissorRects", "IASetPrimitiveTopology", "OMSetRenderTargets", "ClearRenderTargetView", "ResourceBarrier", "SetGraphicsRootSignature", "SetGraphicsRoot32BitConstant", "SetGraphicsRoot32BitConstants", "IASetIndexBuffer", "IASetVertexBuffers", "CopyResource", "CopyBufferRegion", "CopyTextureRegion", "SetDescriptorHeaps", "Dispatch", "ClearDepthStencilView", "SetGraphicsRootDescriptorTable", "SetGraphicsRootConstantBufferView", "SetGraphicsRootShaderResourceView", "SetGraphicsRootUnorderedAccessView", "SetPredication", "SetComputeRootSignature", "SetComputeRoot32BitConstant", "SetComputeRoot32BitConstants", "SetComputeRootDescriptorTable", "SetComputeRootConstantBufferView", "SetComputeRootShaderResourceView", "SetComputeRootUnorderedAccessView"],
    }
    selected["ID3D12CommandQueue"] += ["UpdateTileMappings", "CopyTileMappings"]
    selected["ID3D12GraphicsCommandList"] += ["CopyTiles"]
    selected["ID3D12Device4"] += ["CreateReservedResource1"]
    selected["IDXGISwapChain3"] += ["SetColorSpace1"]
    selected["ID3D12Device"] += ["CreateQueryHeap"]
    selected["ID3D12GraphicsCommandList"] += ["ResolveSubresource", "OMSetBlendFactor", "OMSetStencilRef", "SOSetTargets", "ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat", "DiscardResource", "BeginQuery", "EndQuery", "ResolveQueryData", "ExecuteIndirect"]
    rows = ["// Generated from Microsoft's installed d3d12.h; do not edit.", "#pragma once", "namespace vrx::dx12::slot {"]
    for interface, methods in selected.items():
        table = slots(source, interface)
        if interface == "ID3D12GraphicsCommandList":
            passive = {"QueryInterface", "AddRef", "Release", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "SetName", "GetDevice", "GetType", "SetMarker", "BeginEvent", "EndEvent"}
            missing = table.keys() - set(methods) - passive
            if missing:
                raise RuntimeError(f"Base command-list calls need replay or explicit guards: {sorted(missing)}")
        for method in methods:
            rows.append(f"constexpr unsigned {interface}_{method} = {table[method]};")
    rows.append("}")
    args.output.write_text("\n".join(rows) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
