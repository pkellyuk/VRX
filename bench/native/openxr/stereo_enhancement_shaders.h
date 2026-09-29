#pragma once
// Included after kWarpHlsl. All variants share precisely the same resource and
// constant declarations. The neutral warp is compiled separately so optional
// temporal matching cannot increase its register pressure.
static std::string WarpShaderText(bool enhanced)
{
    return std::string(enhanced ? "#define ENHANCED_WARP 1\n" : "#define ENHANCED_WARP 0\n") + kWarpHlsl;
}

static std::string StereoShaderPrefix()
{
    const std::string warp(kWarpHlsl);
    return warp.substr(0, warp.find("[numthreads(1, 8, 1)]"));
}

static std::string StereoPrepareShaderText()
{
    return std::string("#define ENHANCED_WARP 1\n#define STEREO_PREP 1\n") + StereoShaderPrefix() + R"HLSL(
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CW || id.y >= CH) return;
    int x = (int)id.x, y = (int)id.y;
    float4 c = SceneAt(x, y); // prepared is zero for this pass
    float n = (doWarp != 0 || writeDepth != 0) ? BaseNearAt(x, y) : 0.0;
    if (edgeRefinement != 0 && (doWarp != 0 || writeDepth != 0))
    {
        float2 f = (float2(x, y) + 0.5) * float2(DW, DH) / float2(CW, CH) - 0.5;
        int2 centre = (int2)floor(f + 0.5);
        float sum = 0.0, weights = 0.0, lo = 1.0, hi = 0.0;
        [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
        {
            int2 p = clamp(centre + int2(i, j), int2(0, 0), int2(DW - 1, DH - 1));
            float v = nearBuf[p.y * DW + p.x];
            float3 guide = scene.SampleLevel(samp, (float2(p) + 0.5) / float2(DW, DH), 0).rgb;
            float3 difference = c.rgb - guide;
            float2 d = float2(p) - f;
            float weight = exp2(-0.75 * dot(d, d) - 32.0 * dot(difference, difference));
            sum += weight * v; weights += weight;
            lo = min(lo, v); hi = max(hi, v);
        }
        if (hi - lo >= 0.025 && weights >= 0.000001) n += 0.85 * (sum / weights - n);
    }
    // Shape once per pixel rather than repeating a rational divide for every
    // warp/hole/motion-patch tap. History is invalidated if the curve changes.
    prepNear[y * CW + x] = ShapeNear(n);
    if (sharpness > 0.0)
    {
        float3 a = SceneAt(x - 1, y).rgb, b = SceneAt(x + 1, y).rgb;
        float3 d = SceneAt(x, y - 1).rgb, e = SceneAt(x, y + 1).rgb;
        float3 lo = min(c.rgb, min(min(a, b), min(d, e)));
        float3 hi = max(c.rgb, max(max(a, b), max(d, e)));
        float3 detail = clamp(c.rgb - (a + b + d + e) * 0.25, -0.12, 0.12);
        c.rgb = clamp(c.rgb + 0.35 * saturate(sharpness) * detail, lo, hi);
    }
    c.a = 1.0;
    prepColor[int2(x, y)] = c;
}
)HLSL";
}

static std::string StereoCutShaderText()
{
    return StereoShaderPrefix() + R"HLSL(
groupshared float4 differences[256];
[numthreads(256, 1, 1)]
void main(uint3 id : SV_GroupThreadID)
{
    uint i = id.x;
    int2 p = min(int2(((i % 16) + 0.5) * CW / 16.0, ((i / 16) + 0.5) * CH / 16.0), int2(CW - 1, CH - 1));
    float3 difference = historyValid != 0 ? preparedScene.Load(int3(p, 0)).rgb - oldScene.Load(int3(p, 0)).rgb : 0.0;
    differences[i] = float4(difference, dot(abs(difference), float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)));
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (i < stride) differences[i] += differences[i + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (i == 0)
    {
        float4 d = differences[0] / 256.0;
        // A broad cut/exposure check followed by strict local patch verification
        // in the warp. Motion need not have identical RGB at fixed coordinates.
        enhancementStats[0] = historyValid != 0 && d.w < 0.38 && max(abs(d.x), max(abs(d.y), abs(d.z))) < 0.15 ? 1 : 0;
        enhancementStats[1] = 0;
        enhancementStats[2] = 0;
        enhancementStats[3] = 0;
    }
}
)HLSL";
}
