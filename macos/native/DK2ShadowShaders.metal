#include "DK2ShaderTypes.h"

// --- Original DK2 shadow silhouettes, GPU rasterized ---
// The game keeps light selection, projection and decal rendering. These
// stages reproduce only its 256x256-subpixel accumulator and 8x8 reduction.

vertex float4 dk2_shadow_mask_vertex(
    device const DK2ShadowMaskVertex *vertices [[buffer(0)]],
    uint vertexID [[vertex_id]]) {
    const float2 p = vertices[vertexID].position;
    return float4(p.x / 128.0 - 1.0, 1.0 - p.y / 128.0, 0.0, 1.0);
}

fragment float4 dk2_shadow_mask_fragment() {
    return float4(1.0 / 255.0, 0.0, 0.0, 0.0);
}

vertex DK2ShadowResolveVaryings dk2_shadow_resolve_vertex(
    uint vertexID [[vertex_id]], uint instanceID [[instance_id]]) {
    constexpr float2 positions[] = {
        float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    DK2ShadowResolveVaryings result;
    result.position = float4(positions[vertexID], 0.0, 1.0);
    result.maskIndex = instanceID;
    return result;
}

fragment float4 dk2_shadow_resolve_fragment(
    DK2ShadowResolveVaryings input [[stage_in]],
    texture2d<float, access::read> scratch [[texture(0)]],
    device const DK2ShadowResolveUniform *uniforms [[buffer(0)]]) {
    const DK2ShadowResolveUniform uniform = uniforms[input.maskIndex];
    const uint2 destination = uint2(input.position.xy);
    const uint2 local = destination - uint2(uniform.targetX, uniform.targetY);
    const uint2 source = uint2(uniform.scratchX, uniform.scratchY) + local * 8u;
    float coverage = 0.0;
    for (uint y = 0; y < 8; ++y) {
        for (uint x = 0; x < 8; ++x) {
            coverage += scratch.read(source + uint2(x, y)).r * 4.0;
        }
    }
    coverage = saturate(coverage);
    return uniform.mode == 0u
        ? float4(0.0, 0.0, 0.0, coverage)
        : float4(coverage, coverage, coverage, 1.0);
}
