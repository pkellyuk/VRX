// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace vrx::dx12::dxbc {
enum class Status { Patched, Unsupported, Invalid };
struct Result {
    Status status = Status::Invalid;
    std::vector<uint8_t> bytes;
    std::string reason;
};
// Signed clip-space offset, with oppositely signed convergence per eye:
// x += clamp(shift - convergence*w, -0.25|w|, 0.25|w|) (screen shift <= 0.25 NDC).
// |w - 1| <= 1e-4 is a HUD heuristic, configurable for orthographic geometry.
struct Options { float shift=0, convergence=0; bool skipUnitW=true; };
Result Rewrite(const void* bytes,size_t size,Options options);
bool ValidChecksum(const void* bytes,size_t size);
} // namespace vrx::dx12::dxbc
