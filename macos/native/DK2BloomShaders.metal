#include "DK2ShaderTypes.h"

// --- bloom (see dk2BloomEnabled in DK2Metal.mm) ---
// A conservative, low-intensity glow for lava/fire/torches: threshold-extract
// bright pixels, blur at half resolution with a separable fixed kernel, then
// add back onto the resolved scene. All four passes below are fullscreen
// triangles driven purely by vertex_id - no vertex/index buffers needed.

vertex DK2BloomVaryings dk2_bloom_vertex(uint vertexID [[vertex_id]]) {
    // Emits (-1,-1), (3,-1), (-1,3) in clip space: a single triangle that
    // covers the viewport with no seam, cheaper than a quad + index buffer.
    const float2 corner = float2((vertexID << 1) & 2, vertexID & 2);
    const float2 ndc = corner * 2.0 - 1.0;
    DK2BloomVaryings out;
    out.position = float4(ndc, 0.0, 1.0);
    // Matches the engine's top-left-origin texture convention used elsewhere
    // in this file (clip y=+1 is the top row).
    out.uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return out;
}

constant float kDK2BloomThreshold = 0.65;

fragment float4 dk2_bloom_threshold(DK2BloomVaryings in [[stage_in]],
                                    texture2d<float> scene [[texture(0)]],
                                    sampler smp [[sampler(0)]]) {
    const float4 color = scene.sample(smp, in.uv);
    // DK2's emissive-looking fire/lava is authored as saturated SDR color,
    // not HDR values above 1. Rec.709 luminance therefore rejects even a
    // full red texel. Peak channel brightness preserves those hot colors.
    const float brightness = max(color.r, max(color.g, color.b));
    // Soft knee: 0 below threshold, ramps to full contribution at 1.
    const float contribution = saturate((brightness - kDK2BloomThreshold) /
                                        max(1.0 - kDK2BloomThreshold, 0.0001));
    return float4(color.rgb * contribution, 1.0);
}

// 9-tap fixed gaussian, split into two separable passes.
constant float kDK2BloomWeights[5] = {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216};

fragment float4 dk2_bloom_blur_horizontal(DK2BloomVaryings in [[stage_in]],
                                          texture2d<float> src [[texture(0)]],
                                          sampler smp [[sampler(0)]]) {
    const float2 texel = float2(1.0 / float(src.get_width()), 0.0);
    float3 result = src.sample(smp, in.uv).rgb * kDK2BloomWeights[0];
    for (int i = 1; i < 5; ++i) {
        const float2 offset = texel * float(i);
        result += src.sample(smp, in.uv + offset).rgb * kDK2BloomWeights[i];
        result += src.sample(smp, in.uv - offset).rgb * kDK2BloomWeights[i];
    }
    return float4(result, 1.0);
}

fragment float4 dk2_bloom_blur_vertical(DK2BloomVaryings in [[stage_in]],
                                        texture2d<float> src [[texture(0)]],
                                        sampler smp [[sampler(0)]]) {
    const float2 texel = float2(0.0, 1.0 / float(src.get_height()));
    float3 result = src.sample(smp, in.uv).rgb * kDK2BloomWeights[0];
    for (int i = 1; i < 5; ++i) {
        const float2 offset = texel * float(i);
        result += src.sample(smp, in.uv + offset).rgb * kDK2BloomWeights[i];
        result += src.sample(smp, in.uv - offset).rgb * kDK2BloomWeights[i];
    }
    return float4(result, 1.0);
}

constant float kDK2BloomIntensity = 0.35;

fragment float4 dk2_bloom_composite(DK2BloomVaryings in [[stage_in]],
                                    texture2d<float> scene [[texture(0)]],
                                    texture2d<float> bloom [[texture(1)]],
                                    sampler smp [[sampler(0)]]) {
    const float4 color = scene.sample(smp, in.uv);
    const float3 glow = bloom.sample(smp, in.uv).rgb;
    return float4(color.rgb + glow * kDK2BloomIntensity, color.a);
}
