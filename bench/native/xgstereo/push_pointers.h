// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Reflect typed physical pointers and proven integer address sources in push
// constants; arbitrary floats/integers are never scanned as addresses.
#pragma once
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <functional>
#include "../common/spirv_push_addresses.h"
namespace xg {
struct PushPointers { bool supported = true; std::vector<uint32_t> offsets; };
inline PushPointers ReflectPushPointers(const uint32_t* words, size_t count) {
    PushPointers result;
    if (count < 5 || words[0] != spv::MagicNumber) { result.supported = false; return result; }
    struct Type { uint32_t op = 0, storage = 0, element = 0, length = 0; std::vector<uint32_t> members; };
    std::map<uint32_t, Type> types;
    std::map<std::pair<uint32_t,uint32_t>, uint32_t> offsets;
    std::map<uint32_t,uint32_t> strides, constants;
    std::vector<uint32_t> roots;
    for (size_t at = 5; at < count;) {
        const auto* w = words + at; const uint32_t n = w[0] >> 16, op = w[0] & 65535;
        if (!n || n > count - at) { result.supported = false; return result; }
        if (op == spv::OpTypePointer && n == 4) types[w[1]] = {op, w[2], w[3], 0, {}};
        if (op == spv::OpTypeStruct && n >= 2) { Type t; t.op = op; t.members.assign(w+2,w+n); types[w[1]] = std::move(t); }
        if (op == spv::OpTypeArray && n == 4) types[w[1]] = {op, 0, w[2], w[3], {}};
        if (op == spv::OpConstant && n == 4) constants[w[2]] = w[3];
        if (op == spv::OpMemberDecorate && n == 5 && w[3] == spv::DecorationOffset) offsets[{w[1],w[2]}] = w[4];
        if (op == spv::OpDecorate && n == 4 && w[2] == spv::DecorationArrayStride) strides[w[1]] = w[3];
        if (op == spv::OpVariable && n >= 4 && w[3] == spv::StorageClassPushConstant) roots.push_back(w[1]);
        at += n;
    }
    std::function<void(uint32_t,uint32_t,uint32_t)> walk = [&](uint32_t id, uint32_t base, uint32_t depth) {
        if (depth > 32 || base > 4096) { result.supported = false; return; }
        const auto it = types.find(id); if (it == types.end()) return;
        const auto& t = it->second;
        if (t.op == spv::OpTypePointer) {
            if (t.storage == spv::StorageClassPhysicalStorageBuffer) result.offsets.push_back(base);
            else if (t.storage == spv::StorageClassPushConstant) walk(t.element, base, depth+1);
        } else if (t.op == spv::OpTypeStruct) {
            for (uint32_t i = 0; i < t.members.size(); ++i) {
                auto off = offsets.find({id,i}); if (off == offsets.end()) { result.supported = false; return; }
                walk(t.members[i], base+off->second, depth+1);
            }
        } else if (t.op == spv::OpTypeArray) {
            auto n = constants.find(t.length), stride = strides.find(id);
            if (n == constants.end() || stride == strides.end() || n->second > 512) { result.supported = false; return; }
            for (uint32_t i = 0; i < n->second; ++i) walk(t.element, base+i*stride->second, depth+1);
        }
    };
    for (auto root : roots) walk(root,0,0);
    const auto integers=vrx::spirv::IntegerPushAddresses(words,count);
    result.supported=result.supported && integers.supported;
    result.offsets.insert(result.offsets.end(),integers.offsets.begin(),integers.offsets.end());
    std::sort(result.offsets.begin(),result.offsets.end());
    result.offsets.erase(std::unique(result.offsets.begin(),result.offsets.end()),result.offsets.end());
    return result;
}
} // namespace xg
