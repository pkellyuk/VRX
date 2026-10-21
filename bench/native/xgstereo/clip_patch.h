// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Parallel camera translation in clip space: baked offset or a live BDA float.
// Written for VRX; no implementation code is copied from vrdual.
#pragma once
#include <spirv/unified1/spirv.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace xg {
struct PatchResult {
    bool changed = false;
    std::string reason;
    uint32_t stage = 0; // PatchStage bit that was rewritten (when changed)
};

// Shader stages PatchClipX may rewrite (bit mask). The rewrite belongs in the
// LAST pre-rasterisation stage of a pipeline: geometry if present, otherwise
// tessellation evaluation, otherwise vertex. Patching an earlier stage as well
// would shift twice when the later stage passes Position through.
enum PatchStage : uint32_t {
    PatchStageVertex = 1u,
    PatchStageTessellationEvaluation = 2u,
    PatchStageGeometry = 4u,
    PatchStagePreRasterization = 7u,
};

// Clip-space W within this distance of 1 is treated as an unprojected HUD,
// overlay or full-screen pass and left unshifted. An exact comparison missed
// W values such as 0.99999994 produced by a matrix multiply.
constexpr float HudWTolerance = 1.0e-4f;
// The per-eye offset is x += shift - convergence * w, i.e. a screen (NDC)
// shift of shift / w - convergence. It is clamped to +/- MaxScreenShift NDC
// units (one eighth of the eye's width), so geometry with W near zero can no
// longer be thrown arbitrarily far. 0.25 also equals the largest accepted
// convergence, so the far-field (convergence-only) shift is never clipped.
constexpr float MaxScreenShift = 0.25f;

inline uint32_t FloatWord(float value)
{
    uint32_t word;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

inline void Emit(std::vector<uint32_t>& words, spv::Op op, std::initializer_list<uint32_t> args)
{
    words.push_back((uint32_t(args.size() + 1) << 16) | uint32_t(op));
    words.insert(words.end(), args);
}

// Edits only on success. Unsupported modules remain byte-for-byte unchanged.
// Supports one entry point whose execution model is in `stages` and a scalar
// Position output (direct BuiltIn variable or member of an output block).
// Vertex and tessellation-evaluation shaders are rewritten before each return
// of the entry point, geometry shaders before every vertex emission.
inline PatchResult PatchClipX(std::vector<uint32_t>& words, float shift, uint64_t liveAddress = 0, bool liveConvergence = false,
    uint32_t stages = PatchStageVertex)
{
    auto reject = [](const char* reason) { return PatchResult{false, reason}; };
    if (!std::isfinite(shift)) return reject("non-finite eye shift");
    if (words.size() < 5 || words[0] != spv::MagicNumber || words[4] != 0)
        return reject("invalid SPIR-V header");
    if (shift == 0 && !liveAddress) return reject("zero eye shift");
    if (liveAddress && (liveAddress & 3)) return reject("unaligned live eye address");
    if (liveConvergence && (!liveAddress || liveAddress > std::numeric_limits<uint64_t>::max() - 4)) return reject("invalid convergence address");
    if (words[3] == 0 || words[3] > std::numeric_limits<uint32_t>::max() - 32)
        return reject("invalid id bound");

    struct Instruction { size_t offset; uint32_t count; spv::Op op; };
    std::vector<Instruction> instructions;
    std::unordered_map<uint32_t, uint32_t> outputPointers, outputVariables;
    std::unordered_map<uint32_t, std::vector<uint32_t>> structs;
    std::vector<uint32_t> interfaces, positionVariables;
    std::vector<std::pair<uint32_t, uint32_t>> positionMembers;
    uint32_t main = 0, floatType = 0, vectorType = 0;
    uint32_t boolType = 0, uintType = 0, entryCount = 0;
    bool geometry = false, evaluationStage = false;
    for (size_t at = 5; at < words.size();)
    {
        const uint32_t count = words[at] >> 16;
        if (!count || count > words.size() - at) return reject("truncated instruction");
        const auto op = spv::Op(words[at] & 0xffff);
        const uint32_t* p = words.data() + at;
        instructions.push_back({at, count, op});
        switch (op)
        {
        case spv::OpEntryPoint:
        {
            if (count < 4) return reject("invalid entry point");
            ++entryCount;
            const bool vertex = p[1] == spv::ExecutionModelVertex && (stages & PatchStageVertex);
            const bool evaluation = p[1] == spv::ExecutionModelTessellationEvaluation && (stages & PatchStageTessellationEvaluation);
            const bool primitive = p[1] == spv::ExecutionModelGeometry && (stages & PatchStageGeometry);
            if (!vertex && !evaluation && !primitive) break;
            geometry = primitive; evaluationStage = evaluation;
            main = p[2];
            uint32_t nameEnd = 3;
            for (; nameEnd < count; ++nameEnd)
            {
                const uint32_t name = p[nameEnd];
                if (!(name & 0xff) || !(name & 0xff00) || !(name & 0xff0000) || !(name & 0xff000000)) break;
            }
            if (nameEnd == count) return reject("unterminated entry point name");
            interfaces.assign(p + nameEnd + 1, p + count);
            break;
        }
        case spv::OpTypeFloat:
            if (count != 3) return reject("invalid float type");
            if (p[2] == 32) floatType = p[1];
            break;
        case spv::OpTypeBool:
            if (count != 2) return reject("invalid bool type");
            boolType = p[1]; break;
        case spv::OpTypeInt:
            if (count != 4) return reject("invalid integer type");
            if (p[2] == 32 && p[3] == 0) uintType = p[1];
            break;
        case spv::OpTypePointer:
            if (count != 4) return reject("invalid pointer type");
            if (p[2] == spv::StorageClassOutput) outputPointers[p[1]] = p[3];
            break;
        case spv::OpVariable:
            if (count < 4) return reject("invalid variable");
            if (p[3] == spv::StorageClassOutput) outputVariables[p[2]] = p[1];
            break;
        case spv::OpTypeStruct:
            if (count < 2) return reject("invalid structure");
            structs[p[1]] = std::vector<uint32_t>(p + 2, p + count);
            break;
        case spv::OpDecorate:
            if (count < 3) return reject("invalid decoration");
            if (p[2] == spv::DecorationBuiltIn && count == 4 && p[3] == spv::BuiltInPosition)
                positionVariables.push_back(p[1]);
            break;
        case spv::OpMemberDecorate:
            if (count < 4) return reject("invalid member decoration");
            if (p[3] == spv::DecorationBuiltIn && count == 5 && p[4] == spv::BuiltInPosition)
                positionMembers.emplace_back(p[1], p[2]);
            break;
        default: break;
        }
        at += count;
    }
    if (!main || entryCount != 1) return reject("requires a single supported pre-rasterisation entry point");
    if (!floatType) return reject("no float32 type");
    for (const auto& i : instructions)
        if (i.op == spv::OpTypeVector && i.count == 4 && words[i.offset + 2] == floatType && words[i.offset + 3] == 4)
            vectorType = words[i.offset + 1];
    if (!vectorType) return reject("no float32 vec4 type");

    uint32_t position = 0, member = 0;
    bool isBlock = false;
    for (const auto id : interfaces)
    {
        const auto var = outputVariables.find(id);
        if (var == outputVariables.end()) continue;
        const auto pointer = outputPointers.find(var->second);
        if (pointer == outputPointers.end()) continue;
        for (const auto direct : positionVariables)
            if (direct == id && pointer->second == vectorType)
            {
                if (position) return reject("ambiguous Position output");
                position = id;
            }
        for (const auto& decorated : positionMembers)
            if (decorated.first == pointer->second)
            {
                const auto type = structs.find(decorated.first);
                if (type == structs.end() || decorated.second >= type->second.size() || type->second[decorated.second] != vectorType)
                    return reject("unsupported Position member");
                if (position) return reject("ambiguous Position output");
                position = id; member = decorated.second; isBlock = true;
            }
    }
    if (!position) return reject("no supported Position output");
    bool physicalCapability = false, physicalExtension = false;
    uint32_t physicalFloatPointer = 0, uint2Type = 0;
    for (const auto& i : instructions)
    {
        const auto* p = words.data() + i.offset;
        if (i.op == spv::OpCapability && i.count == 2 && p[1] == spv::CapabilityPhysicalStorageBufferAddresses) physicalCapability = true;
        if (i.op == spv::OpExtension && i.count > 1)
        {
            const auto* name = reinterpret_cast<const char*>(p + 1);
            const size_t bytes = (i.count - 1) * 4;
            constexpr char extension[] = "SPV_KHR_physical_storage_buffer";
            if (bytes >= sizeof(extension) && !std::memcmp(name, extension, sizeof(extension))) physicalExtension = true;
        }
        if (i.op == spv::OpMemoryModel && liveAddress &&
            (i.count != 3 || (p[1] != spv::AddressingModelLogical && p[1] != spv::AddressingModelPhysicalStorageBuffer64)))
            return reject("unsupported addressing model");
        if (i.op == spv::OpTypePointer && i.count == 4 && p[2] == spv::StorageClassPhysicalStorageBuffer && p[3] == floatType)
            physicalFloatPointer = p[1];
        if (i.op == spv::OpTypeVector && i.count == 4 && p[2] == uintType && p[3] == 2) uint2Type = p[1];
    }

    uint32_t nextId = words[3];
    auto id = [&]() { return nextId++; };
    std::vector<uint32_t> globals;
    if (!boolType) { boolType = id(); Emit(globals, spv::OpTypeBool, {boolType}); }
    if ((isBlock || liveAddress) && !uintType) { uintType = id(); Emit(globals, spv::OpTypeInt, {uintType, 32, 0}); }
    uint32_t offset = 0, liveBits = 0, convergenceBits = 0;
    const uint32_t one = id(), zero = id(), tolerance = id(), limitScale = id();
    if (liveAddress)
    {
        if (!uint2Type) { uint2Type = id(); Emit(globals, spv::OpTypeVector, {uint2Type, uintType, 2}); }
        if (!physicalFloatPointer)
        { physicalFloatPointer = id(); Emit(globals, spv::OpTypePointer, {physicalFloatPointer, spv::StorageClassPhysicalStorageBuffer, floatType}); }
        const uint32_t lo = id(), hi = id(); liveBits = id();
        Emit(globals, spv::OpConstant, {uintType, lo, uint32_t(liveAddress)});
        Emit(globals, spv::OpConstant, {uintType, hi, uint32_t(liveAddress >> 32)});
        Emit(globals, spv::OpConstantComposite, {uint2Type, liveBits, lo, hi});
        if (liveConvergence)
        {
            const uint32_t cl = id(), ch = id(); convergenceBits = id();
            Emit(globals, spv::OpConstant, {uintType, cl, uint32_t(liveAddress + 4)});
            Emit(globals, spv::OpConstant, {uintType, ch, uint32_t((liveAddress + 4) >> 32)});
            Emit(globals, spv::OpConstantComposite, {uint2Type, convergenceBits, cl, ch});
        }
    }
    else { offset = id(); Emit(globals, spv::OpConstant, {floatType, offset, FloatWord(shift)}); }
    Emit(globals, spv::OpConstant, {floatType, one, FloatWord(1)});
    Emit(globals, spv::OpConstant, {floatType, zero, FloatWord(0)});
    Emit(globals, spv::OpConstant, {floatType, tolerance, FloatWord(HudWTolerance)});
    Emit(globals, spv::OpConstant, {floatType, limitScale, FloatWord(MaxScreenShift)});
    uint32_t memberIndex = 0, vecPointer = 0;
    if (isBlock)
    {
        // SPIR-V forbids duplicate non-aggregate type declarations.
        for (const auto& pointer : outputPointers)
            if (pointer.second == vectorType) { vecPointer = pointer.first; break; }
        if (!vecPointer)
        {
            vecPointer = id();
            Emit(globals, spv::OpTypePointer, {vecPointer, spv::StorageClassOutput, vectorType});
        }
        memberIndex = id();
        Emit(globals, spv::OpConstant, {uintType, memberIndex, member});
    }
    std::vector<uint32_t> result(words.begin(), words.begin() + 5);
    if (liveAddress)
    {
        if (!physicalCapability) Emit(result, spv::OpCapability, {spv::CapabilityPhysicalStorageBufferAddresses});
        // Extensions follow all capabilities; insert in the loop below.
    }
    uint32_t function = 0, returns = 0;
    bool insertedGlobals = false, insertedExtension = false;
    for (const auto& i : instructions)
    {
        const uint32_t* p = words.data() + i.offset;
        if (liveAddress && !insertedExtension && i.op != spv::OpCapability)
        {
            if (!physicalExtension)
            {
                constexpr char text[] = "SPV_KHR_physical_storage_buffer";
                constexpr size_t stringWords = (sizeof(text) + 3) / 4;
                result.push_back((uint32_t(stringWords + 1) << 16) | spv::OpExtension);
                const size_t start = result.size(); result.resize(start + stringWords, 0);
                std::memcpy(result.data() + start, text, sizeof(text));
            }
            insertedExtension = true;
        }
        if (liveAddress && i.op == spv::OpMemoryModel)
        {
            Emit(result, spv::OpMemoryModel, {spv::AddressingModelPhysicalStorageBuffer64, p[2]});
            continue;
        }
        if (i.op == spv::OpFunction)
        {
            if (i.count != 5 || function) return reject("invalid function");
            function = p[2];
            if (!insertedGlobals) { result.insert(result.end(), globals.begin(), globals.end()); insertedGlobals = true; }
        }
        const bool emits = i.op == spv::OpEmitVertex || i.op == spv::OpEmitStreamVertex;
        if (geometry ? (emits && function) : (i.op == spv::OpReturn && function == main))
        {
            if (!geometry && i.count != 1) return reject("invalid return");
            // Bound growth is checked again below (there can be many returns).
            if (nextId > std::numeric_limits<uint32_t>::max() - 64) return reject("id bound overflow");
            uint32_t address = position;
            if (isBlock) { address = id(); Emit(result, spv::OpAccessChain, {vecPointer, address, position, memberIndex}); }
            if (liveAddress)
            {
                const uint32_t pointer = id(); offset = id();
                Emit(result, spv::OpBitcast, {physicalFloatPointer, pointer, liveBits});
                Emit(result, spv::OpLoad, {floatType, offset, pointer, spv::MemoryAccessAlignedMask, 4});
            }
            const uint32_t v = id(), x = id(), w = id(), shifted = id(), hud = id(), selected = id(), out = id();
            Emit(result, spv::OpLoad, {vectorType, v, address});
            Emit(result, spv::OpCompositeExtract, {floatType, x, v, 0});
            Emit(result, spv::OpCompositeExtract, {floatType, w, v, 3});
            uint32_t effectiveOffset = offset;
            if (liveConvergence)
            {
                const uint32_t pointer = id(), convergence = id(), scaled = id(); effectiveOffset = id();
                Emit(result, spv::OpBitcast, {physicalFloatPointer, pointer, convergenceBits});
                Emit(result, spv::OpLoad, {floatType, convergence, pointer, spv::MemoryAccessAlignedMask, 4});
                Emit(result, spv::OpFMul, {floatType, scaled, convergence, w});
                Emit(result, spv::OpFSub, {floatType, effectiveOffset, offset, scaled});
            }
            // Clamp the clip-space offset to MaxScreenShift * |w| so the NDC
            // shift (offset / w) stays bounded as w approaches zero.
            const uint32_t negW = id(), wNegative = id(), absW = id(), limit = id(), negLimit = id();
            const uint32_t over = id(), upper = id(), under = id(), clamped = id();
            Emit(result, spv::OpFNegate, {floatType, negW, w});
            Emit(result, spv::OpFOrdLessThan, {boolType, wNegative, w, zero});
            Emit(result, spv::OpSelect, {floatType, absW, wNegative, negW, w});
            Emit(result, spv::OpFMul, {floatType, limit, absW, limitScale});
            Emit(result, spv::OpFNegate, {floatType, negLimit, limit});
            Emit(result, spv::OpFOrdGreaterThan, {boolType, over, effectiveOffset, limit});
            Emit(result, spv::OpSelect, {floatType, upper, over, limit, effectiveOffset});
            Emit(result, spv::OpFOrdLessThan, {boolType, under, upper, negLimit});
            Emit(result, spv::OpSelect, {floatType, clamped, under, negLimit, upper});
            Emit(result, spv::OpFAdd, {floatType, shifted, x, clamped});
            // HUD heuristic: |w - 1| <= HudWTolerance keeps x unchanged.
            const uint32_t distance = id(), negDistance = id(), below = id(), absDistance = id();
            Emit(result, spv::OpFSub, {floatType, distance, w, one});
            Emit(result, spv::OpFNegate, {floatType, negDistance, distance});
            Emit(result, spv::OpFOrdLessThan, {boolType, below, distance, zero});
            Emit(result, spv::OpSelect, {floatType, absDistance, below, negDistance, distance});
            Emit(result, spv::OpFOrdLessThanEqual, {boolType, hud, absDistance, tolerance});
            Emit(result, spv::OpSelect, {floatType, selected, hud, x, shifted});
            Emit(result, spv::OpCompositeInsert, {vectorType, out, selected, v, 0});
            Emit(result, spv::OpStore, {address, out});
            ++returns;
        }
        result.insert(result.end(), p, p + i.count);
        if (i.op == spv::OpFunctionEnd)
        {
            if (i.count != 1 || !function) return reject("invalid function end");
            function = 0;
        }
    }
    if (function) return reject("unterminated function");
    if (!returns) return reject(geometry ? "geometry shader emits no vertex" : "entry point has no return");
    result[3] = nextId;
    words.swap(result);
    PatchResult patched{true, {}};
    patched.stage = geometry ? PatchStageGeometry : (evaluationStage ? PatchStageTessellationEvaluation : PatchStageVertex);
    return patched;
}
} // namespace xg
