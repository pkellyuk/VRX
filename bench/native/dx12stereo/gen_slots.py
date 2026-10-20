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
    for match in re.finditer(r"\b(ID3D12\w+)\s*:\s*public\s+(\w+)\s*\{\s*public:(.*?)\n\s*\};", source, re.S):
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
    selected = {
        "ID3D12Device": ["CreateCommandQueue", "CreateCommandAllocator", "CreateCommandList", "CreateGraphicsPipelineState", "CreateCommittedResource", "CreateDescriptorHeap", "CreateRenderTargetView"],
        "ID3D12CommandAllocator": ["Reset"],
        "ID3D12Device4": ["CreateCommandList1"],
        "ID3D12CommandQueue": ["ExecuteCommandLists"],
        "ID3D12GraphicsCommandList": ["Close", "Reset", "DrawInstanced", "DrawIndexedInstanced", "ExecuteBundle", "ClearState", "SetPipelineState", "RSSetViewports", "RSSetScissorRects", "IASetPrimitiveTopology", "OMSetRenderTargets", "ClearRenderTargetView", "ResourceBarrier", "SetGraphicsRootSignature", "SetGraphicsRoot32BitConstant", "SetGraphicsRoot32BitConstants", "IASetIndexBuffer", "IASetVertexBuffers", "CopyResource", "CopyBufferRegion", "CopyTextureRegion", "SetDescriptorHeaps", "Dispatch"],
    }
    rows = ["// Generated from Microsoft's installed d3d12.h; do not edit.", "#pragma once", "namespace vrx::dx12::slot {"]
    for interface, methods in selected.items():
        table = slots(source, interface)
        for method in methods:
            rows.append(f"constexpr unsigned {interface}_{method} = {table[method]};")
    rows.append("}")
    args.output.write_text("\n".join(rows) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
