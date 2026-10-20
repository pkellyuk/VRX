// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <cstdint>

namespace vrx::dx12 {
struct Counts {
    uint32_t bytes = sizeof(Counts), version = 4;
    uint64_t devices = 0, queues = 0, commandLists = 0, resources = 0, pipelines = 0;
    uint64_t closes = 0, resets = 0, draws = 0, indexedDraws = 0, bundles = 0, submissions = 0;
    uint64_t hookFailures = 0;
    uint64_t mirrorReplays=0,mirrorStops=0,mirrorResources=0,mirrorPipelines=0,mirrorLiveObjects=0;
    uint64_t swapchains=0,presentations=0,publications=0,presentDrops=0,resizes=0;
    uint64_t streamPipelines=0,memoryHeaps=0,placedResources=0,modernResources=0;
    char reason[256]{};
};
// Only called after the real D3D12CreateDevice succeeds, never in DllMain.
void Attach(ID3D12Device* device) noexcept;
void Snapshot(Counts& output) noexcept;
void DiagnosticScope(bool enter) noexcept;
bool ApiOuter() noexcept;
void Report(bool force=false) noexcept;
}
