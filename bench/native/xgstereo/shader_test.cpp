#include "test_shaders.h"
#include "live_tuning.h"
#include <cstdio>
#include <cstdlib>
#ifdef XG_VALIDATE_SPIRV
#include <spirv-tools/libspirv.hpp>
#endif

static void Check(bool condition, const char* description)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}

static size_t Count(const std::vector<uint32_t>& w, spv::Op op)
{
    size_t n = 0;
    for (size_t at = 5; at < w.size(); at += w[at] >> 16)
    {
        Check((w[at] >> 16) && (w[at] >> 16) <= w.size() - at, "valid output instruction lengths");
        if ((w[at] & 0xffff) == uint32_t(op)) ++n;
    }
    return n;
}

static void Unchanged(std::vector<uint32_t> words, float shift, const char* description)
{
    const auto original = words;
    const auto result = xg::PatchClipX(words, shift);
    Check(!result.changed && !result.reason.empty() && words == original, description);
}

int main()
{
#ifdef XG_VALIDATE_SPIRV
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_0);
    validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
    { std::fprintf(stderr, "SPIR-V validation at instruction %zu: %s\n", position.index, message); });
    Check(validator.Validate(xg::test::Fragment()), "valid fragment fixture");
    Check(validator.Validate(xg::test::MultipleVertexEntries()), "valid unsupported multiple-entry fixture");
#endif
    for (const bool block : {false, true})
    {
        auto words = xg::test::Vertex(block);
        const auto before = words;
#ifdef XG_VALIDATE_SPIRV
        Check(validator.Validate(before), "valid vertex fixture");
#endif
        Check(xg::PatchClipX(words, 0.125f).changed, "patch direct and block Position");
#ifdef XG_VALIDATE_SPIRV
        Check(validator.Validate(words), "patched shader satisfies Vulkan SPIR-V rules");
        auto live = before;
        spvtools::SpirvTools liveValidator(SPV_ENV_VULKAN_1_2);
        liveValidator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* message)
        { std::fprintf(stderr, "live SPIR-V: %s\n", message); });
        Check(xg::PatchClipX(live, 0, 0x1234567800ull).changed && liveValidator.Validate(live), "valid live eye load without shaderInt64");
        auto converged = before;
        Check(xg::PatchClipX(converged, 0, 0x1234567800ull, true).changed && liveValidator.Validate(converged), "valid live separation and convergence loads");
        auto negative = before;
        Check(xg::PatchClipX(negative, -0.125f).changed && validator.Validate(negative), "valid opposite eye shader");
#endif
        Check(words[3] > before[3], "increase id bound");
        Check(Count(words, spv::OpFAdd) == 1 && Count(words, spv::OpSelect) == 1, "one translation and HUD selection");
        Check(Count(words, spv::OpStore) == Count(before, spv::OpStore) + 1, "write translated position at return");
        Check(Count(words, spv::OpAccessChain) == (block ? 2u : 0u), "correct direct and block addressing");
    }
    Unchanged(xg::test::Fragment(), .125f, "fragment shader untouched");
    float shift = 9, convergence = 9;
    Check(xg::ParseTuning("0.8 0.002\r\n", shift, convergence) && shift == .8f && convergence == .002f, "live tuning parses invariant values");
    for (const char* text : {"", "0.8", "nan 0", "0.8 inf", "-1 0", "65 0", "1 .5", "0 .01", "1 0 extra", "0,8 0"})
    {
        shift = 9; convergence = 9;
        Check(!xg::ParseTuning(text, shift, convergence) && shift == 9 && convergence == 9, "invalid/partial tuning preserves current values");
    }
    Unchanged(xg::test::MultipleVertexEntries(), .125f, "multiple entry points unchanged");
    Unchanged(xg::test::Vertex(), 0, "zero shift untouched");
    Unchanged(xg::test::Vertex(), std::numeric_limits<float>::infinity(), "non-finite shift rejected");
    Unchanged({}, .1f, "empty module rejected");
    auto invalid = xg::test::Vertex(); invalid[5] = 0;
    Unchanged(invalid, .1f, "zero-length instruction rejected");
    invalid = xg::test::Vertex(); invalid.pop_back(); invalid.back() = (8 << 16) | spv::OpReturn;
    Unchanged(invalid, .1f, "truncated instruction rejected");
    invalid = xg::test::Vertex(); invalid[3] = UINT32_MAX;
    Unchanged(invalid, .1f, "id bound overflow rejected");
    invalid = xg::test::Vertex();
    // Remove the Position decoration, retaining a valid instruction stream.
    for (size_t at = 5; at < invalid.size(); at += invalid[at] >> 16)
        if ((invalid[at] & 0xffff) == spv::OpDecorate && invalid[at + 2] == spv::DecorationBuiltIn)
        { invalid[at + 3] = spv::BuiltInPointSize; break; }
    Unchanged(invalid, .1f, "unrecognised output rejected");
    invalid = xg::test::Vertex();
    // A helper function must retain its own return without a Position store.
    xg::Emit(invalid, spv::OpFunction, {1, 17, spv::FunctionControlMaskNone, 2});
    xg::Emit(invalid, spv::OpLabel, {18});
    xg::Emit(invalid, spv::OpReturn, {});
    xg::Emit(invalid, spv::OpFunctionEnd, {});
    invalid[3] = 19;
    Check(xg::PatchClipX(invalid, .125f).changed && Count(invalid, spv::OpStore) == 2 && Count(invalid, spv::OpReturn) == 2,
        "helper function returns stay untouched");
    // Malformed shader data must never be partially rewritten or read out of bounds.
    const auto fixture = xg::test::Vertex(true);
    for (size_t length = 0; length < fixture.size(); ++length)
    {
        std::vector<uint32_t> prefix(fixture.begin(), fixture.begin() + length);
        auto saved = prefix;
        auto result = xg::PatchClipX(prefix, .125f);
        Check(!result.changed && prefix == saved, "all truncated modules rejected unchanged");
    }
    std::puts("PASS: direct/block Position, shader isolation, HUD selection, helper returns, malformed input");
    return 0;
}
