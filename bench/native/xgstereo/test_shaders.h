// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Minimal real SPIR-V modules for deterministic tests, independent of a shader SDK.
#pragma once
#include "clip_patch.h"

namespace xg::test {
inline void Name(std::vector<uint32_t>& w, spv::ExecutionModel model, uint32_t main,
    std::initializer_list<uint32_t> interfaces)
{
    w.push_back((uint32_t(5 + interfaces.size()) << 16) | spv::OpEntryPoint);
    w.insert(w.end(), {uint32_t(model), main, 0x6e69616d, 0}); // "main\0"
    w.insert(w.end(), interfaces);
}

inline std::vector<uint32_t> Vertex(bool block = false)
{
    // IDs: void=1, function type=2, float=3, vec4=4, input ptr=5,
    // input=6, output vec4 ptr=7, output=8, main=9, label=10,
    // value=11, block=12, block ptr=13, uint=14, zero=15, address=16.
    std::vector<uint32_t> w{spv::MagicNumber, 0x00010000, 0, 17, 0};
    Emit(w, spv::OpCapability, {spv::CapabilityShader});
    Emit(w, spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    Name(w, spv::ExecutionModelVertex, 9, {6, 8});
    Emit(w, spv::OpDecorate, {6, spv::DecorationLocation, 0});
    if (block)
    {
        Emit(w, spv::OpDecorate, {12, spv::DecorationBlock});
        Emit(w, spv::OpMemberDecorate, {12, 0, spv::DecorationBuiltIn, spv::BuiltInPosition});
    }
    else Emit(w, spv::OpDecorate, {8, spv::DecorationBuiltIn, spv::BuiltInPosition});
    Emit(w, spv::OpTypeVoid, {1});
    Emit(w, spv::OpTypeFunction, {2, 1});
    Emit(w, spv::OpTypeFloat, {3, 32});
    Emit(w, spv::OpTypeVector, {4, 3, 4});
    Emit(w, spv::OpTypePointer, {5, spv::StorageClassInput, 4});
    Emit(w, spv::OpVariable, {5, 6, spv::StorageClassInput});
    Emit(w, spv::OpTypePointer, {7, spv::StorageClassOutput, 4});
    if (block)
    {
        Emit(w, spv::OpTypeStruct, {12, 4});
        Emit(w, spv::OpTypePointer, {13, spv::StorageClassOutput, 12});
        Emit(w, spv::OpTypeInt, {14, 32, 0});
        Emit(w, spv::OpConstant, {14, 15, 0});
        Emit(w, spv::OpVariable, {13, 8, spv::StorageClassOutput});
    }
    else Emit(w, spv::OpVariable, {7, 8, spv::StorageClassOutput});
    Emit(w, spv::OpFunction, {1, 9, spv::FunctionControlMaskNone, 2});
    Emit(w, spv::OpLabel, {10});
    Emit(w, spv::OpLoad, {4, 11, 6});
    if (block) Emit(w, spv::OpAccessChain, {7, 16, 8, 15});
    Emit(w, spv::OpStore, {block ? 16u : 8u, 11});
    Emit(w, spv::OpReturn, {});
    Emit(w, spv::OpFunctionEnd, {});
    return w;
}

inline std::vector<uint32_t> MultipleVertexEntries()
{
    auto words = Vertex(true);
    for (size_t at = 5; at < words.size(); at += words[at] >> 16)
        if ((words[at] & 0xffff) == uint32_t(spv::OpEntryPoint))
        {
            const size_t count = words[at] >> 16;
            std::vector<uint32_t> entry(words.begin() + at, words.begin() + at + count);
            entry[2] = 17;
            entry[4] = 0x32; // second name: "main2\0", independent vertex function
            words.insert(words.begin() + at + count, entry.begin(), entry.end());
            break;
        }
    Emit(words, spv::OpFunction, {1, 17, spv::FunctionControlMaskNone, 2});
    Emit(words, spv::OpLabel, {18});
    Emit(words, spv::OpLoad, {4, 19, 6});
    Emit(words, spv::OpAccessChain, {7, 20, 8, 15});
    Emit(words, spv::OpStore, {20, 19});
    Emit(words, spv::OpReturn, {});
    Emit(words, spv::OpFunctionEnd, {});
    words[3] = 21;
    return words;
}

// A later pre-rasterisation stage writing a constant Position: tessellation
// evaluation (returns) or geometry (one point per EmitVertex).
inline std::vector<uint32_t> LateStage(bool geometry)
{
    // IDs: void=1, function type=2, float=3, vec4=4, output ptr=5, output=6,
    // one=7, zero=8, position=9, main=10, label=11.
    std::vector<uint32_t> w{spv::MagicNumber, 0x00010000, 0, 12, 0};
    Emit(w, spv::OpCapability, {spv::CapabilityShader});
    Emit(w, spv::OpCapability, {geometry ? uint32_t(spv::CapabilityGeometry) : uint32_t(spv::CapabilityTessellation)});
    Emit(w, spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    Name(w, geometry ? spv::ExecutionModelGeometry : spv::ExecutionModelTessellationEvaluation, 10, {6});
    if (geometry)
    {
        Emit(w, spv::OpExecutionMode, {10, spv::ExecutionModeInputPoints});
        Emit(w, spv::OpExecutionMode, {10, spv::ExecutionModeInvocations, 1});
        Emit(w, spv::OpExecutionMode, {10, spv::ExecutionModeOutputPoints});
        Emit(w, spv::OpExecutionMode, {10, spv::ExecutionModeOutputVertices, 1});
    }
    else Emit(w, spv::OpExecutionMode, {10, spv::ExecutionModeTriangles});
    Emit(w, spv::OpDecorate, {6, spv::DecorationBuiltIn, spv::BuiltInPosition});
    Emit(w, spv::OpTypeVoid, {1});
    Emit(w, spv::OpTypeFunction, {2, 1});
    Emit(w, spv::OpTypeFloat, {3, 32});
    Emit(w, spv::OpTypeVector, {4, 3, 4});
    Emit(w, spv::OpTypePointer, {5, spv::StorageClassOutput, 4});
    Emit(w, spv::OpVariable, {5, 6, spv::StorageClassOutput});
    Emit(w, spv::OpConstant, {3, 7, FloatWord(2)});
    Emit(w, spv::OpConstant, {3, 8, FloatWord(0)});
    Emit(w, spv::OpConstantComposite, {4, 9, 8, 8, 8, 7});
    Emit(w, spv::OpFunction, {1, 10, spv::FunctionControlMaskNone, 2});
    Emit(w, spv::OpLabel, {11});
    Emit(w, spv::OpStore, {6, 9});
    if (geometry) Emit(w, spv::OpEmitVertex, {});
    Emit(w, spv::OpReturn, {});
    Emit(w, spv::OpFunctionEnd, {});
    return w;
}

inline std::vector<uint32_t> Fragment()
{
    std::vector<uint32_t> w{spv::MagicNumber, 0x00010000, 0, 13, 0};
    Emit(w, spv::OpCapability, {spv::CapabilityShader});
    Emit(w, spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    Name(w, spv::ExecutionModelFragment, 10, {6});
    Emit(w, spv::OpExecutionMode, {10, spv::ExecutionModeOriginUpperLeft});
    Emit(w, spv::OpDecorate, {6, spv::DecorationLocation, 0});
    Emit(w, spv::OpTypeVoid, {1});
    Emit(w, spv::OpTypeFunction, {2, 1});
    Emit(w, spv::OpTypeFloat, {3, 32});
    Emit(w, spv::OpTypeVector, {4, 3, 4});
    Emit(w, spv::OpTypePointer, {5, spv::StorageClassOutput, 4});
    Emit(w, spv::OpVariable, {5, 6, spv::StorageClassOutput});
    Emit(w, spv::OpConstant, {3, 7, FloatWord(1)});
    Emit(w, spv::OpConstant, {3, 8, FloatWord(0)});
    Emit(w, spv::OpConstantComposite, {4, 9, 7, 8, 8, 7});
    Emit(w, spv::OpFunction, {1, 10, spv::FunctionControlMaskNone, 2});
    Emit(w, spv::OpLabel, {11});
    Emit(w, spv::OpStore, {6, 9});
    Emit(w, spv::OpReturn, {});
    Emit(w, spv::OpFunctionEnd, {});
    return w;
}
} // namespace xg::test
