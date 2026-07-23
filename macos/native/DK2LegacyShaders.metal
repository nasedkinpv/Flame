#include "DK2ShaderTypes.h"

// --- Legacy (fixed-function TL-vertex) pipeline ---
// DK2Vertex1C/2C are already screen-space (transformed+lit), matching the
// original D3D fixed-function path this emulates.

DK2RasterVertex dk2_make_vertex(float x, float y, float z, float rhw, uint diffuse,
                                float2 texCoord, float2 texCoord1, float2 texCoord2,
                                thread const DK2DrawUniform &draw,
                                constant DK2MeshCamera &camera) {
    const float reciprocalW = abs(rhw) > 0.000001f ? rhw : 1.0f;
    const float clipW = 1.0f / reciprocalW;
    DK2RasterVertex result;
    result.position = float4((x * 2.0f / draw.screenWidth - 1.0f) * clipW,
                             (1.0f - y * 2.0f / draw.screenHeight) * clipW,
                             z * clipW, clipW);
    result.clipDistance = 1.0f;
    result.color = float4(float((diffuse >> 16) & 0xFF),
                          float((diffuse >> 8) & 0xFF),
                          float(diffuse & 0xFF),
                          float((diffuse >> 24) & 0xFF)) / 255.0f;
    result.texCoord = texCoord;
    result.texCoord1 = texCoord1;
    result.texCoord2 = texCoord2;
    result.textureIndex = draw.textureIndex;
    result.colorOp = draw.colorOp;
    result.colorArg1 = draw.colorArg1;
    result.colorArg2 = draw.colorArg2;
    result.alphaOp = draw.alphaOp;
    result.alphaArg1 = draw.alphaArg1;
    result.alphaArg2 = draw.alphaArg2;
    result.textureFactor = draw.textureFactor;
    result.textureIndex1 = draw.textureIndex1;
    result.colorOp1 = draw.colorOp1;
    result.colorArg1_1 = draw.colorArg1_1;
    result.colorArg2_1 = draw.colorArg2_1;
    result.alphaOp1 = draw.alphaOp1;
    result.alphaArg1_1 = draw.alphaArg1_1;
    result.alphaArg2_1 = draw.alphaArg2_1;
    result.textureIndex2 = draw.textureIndex2;
    result.colorOp2 = draw.colorOp2;
    result.colorArg1_2 = draw.colorArg1_2;
    result.colorArg2_2 = draw.colorArg2_2;
    result.alphaOp2 = draw.alphaOp2;
    result.alphaArg1_2 = draw.alphaArg1_2;
    result.alphaArg2_2 = draw.alphaArg2_2;
    result.bumpEnvMat0_00 = draw.bumpEnvMat0_00;
    result.bumpEnvMat0_01 = draw.bumpEnvMat0_01;
    result.bumpEnvMat0_10 = draw.bumpEnvMat0_10;
    result.bumpEnvMat0_11 = draw.bumpEnvMat0_11;
    result.bumpEnvLScale0 = draw.bumpEnvLScale0;
    result.bumpEnvLOffset0 = draw.bumpEnvLOffset0;
    result.bumpEnvMat1_00 = draw.bumpEnvMat1_00;
    result.bumpEnvMat1_01 = draw.bumpEnvMat1_01;
    result.bumpEnvMat1_10 = draw.bumpEnvMat1_10;
    result.bumpEnvMat1_11 = draw.bumpEnvMat1_11;
    result.bumpEnvLScale1 = draw.bumpEnvLScale1;
    result.bumpEnvLOffset1 = draw.bumpEnvLOffset1;
    result.meshFlags = 0u;
    result.materialFlags = draw.materialFlags;
    result.waterTime = draw.waterTime;
    // World XY for water/lava draws only (the 2x2 solve is wasted on
    // everything else). See dk2_unproject_ground (DK2WaterShader.metal) for
    // the ground-plane assumption and its caveats.
    result.worldPos = draw.materialFlags != 0u
        ? dk2_unproject_ground(camera,
              float2(x * 2.0f / draw.screenWidth - 1.0f,
                     1.0f - y * 2.0f / draw.screenHeight))
        : float2(0.0f);
    return result;
}

vertex DK2RasterVertex dk2_vertex_1c(device const DK2Vertex1C *vertices [[buffer(0)]],
                                     device const DK2DrawUniform *draws [[buffer(1)]],
                                     constant DK2MeshCamera &camera [[buffer(3)]],
                                     uint vertexID [[vertex_id]],
                                     uint drawID [[instance_id]]) {
    const DK2Vertex1C inputVertex = vertices[vertexID];
    const DK2DrawUniform draw = draws[drawID];
    const float2 uv = float2(inputVertex.u, inputVertex.v);
    return dk2_make_vertex(inputVertex.x, inputVertex.y, inputVertex.z, inputVertex.rhw,
                           inputVertex.diffuse, uv, uv, uv, draw, camera);
}

vertex DK2RasterVertex dk2_vertex_2c(device const DK2Vertex2C *vertices [[buffer(0)]],
                                     device const DK2DrawUniform *draws [[buffer(1)]],
                                     constant DK2MeshCamera &camera [[buffer(3)]],
                                     uint vertexID [[vertex_id]],
                                     uint drawID [[instance_id]]) {
    const DK2Vertex2C inputVertex = vertices[vertexID];
    const DK2DrawUniform draw = draws[drawID];
    return dk2_make_vertex(inputVertex.x, inputVertex.y, inputVertex.z, inputVertex.rhw,
                           inputVertex.diffuse,
                           float2(inputVertex.texCoord[0][0], inputVertex.texCoord[0][1]),
                           float2(inputVertex.texCoord[1][0], inputVertex.texCoord[1][1]),
                           float2(inputVertex.texCoord[2][0], inputVertex.texCoord[2][1]),
                           draw, camera);
}

float4 dk2_texture_arg(uint selector, float4 textureColor, float4 current,
                       float4 diffuse, float4 factor) {
    float4 value;
    switch (selector & 0xF) {
        case 0: value = diffuse; break;
        case 1: value = current; break;
        case 2: value = textureColor; break;
        case 3: value = factor; break;
        default: value = float4(1.0); break;
    }
    if ((selector & 0x10) != 0) value = 1.0 - value;
    if ((selector & 0x20) != 0) value = value.aaaa;
    return value;
}

float3 dk2_color_op(uint op, float4 a, float4 b, float4 diffuse,
                    float4 textureColor, float4 factor, float4 current) {
    switch (op) {
        case 1: return current.rgb;
        case 2: return a.rgb;
        case 3: return b.rgb;
        case 4: return a.rgb * b.rgb;
        case 5: return saturate(a.rgb * b.rgb * 2.0);
        case 6: return saturate(a.rgb * b.rgb * 4.0);
        case 7: return saturate(a.rgb + b.rgb);
        case 8: return saturate(a.rgb + b.rgb - 0.5);
        case 9: return saturate((a.rgb + b.rgb - 0.5) * 2.0);
        case 10: return saturate(a.rgb - b.rgb);
        case 11: return saturate(a.rgb + b.rgb * (1.0 - a.rgb));
        case 12: return mix(b.rgb, a.rgb, diffuse.a);
        case 13: return mix(b.rgb, a.rgb, textureColor.a);
        case 14: return mix(b.rgb, a.rgb, factor.a);
        case 15: return saturate(a.rgb + b.rgb * (1.0 - textureColor.a));
        case 16: return mix(b.rgb, a.rgb, current.a);
        case 18: return saturate(a.rgb + b.rgb * a.a);
        case 19: return saturate(a.rgb * b.rgb + a.aaa);
        case 20: return saturate(a.rgb + b.rgb * (1.0 - a.a));
        case 21: return saturate(a.rgb * (1.0 - b.rgb) + a.aaa);
        case 24: return saturate(float3(dot(a.rgb * 2.0 - 1.0, b.rgb * 2.0 - 1.0)));
        default: return a.rgb * b.rgb;
    }
}

float dk2_alpha_op(uint op, float4 a, float4 b, float4 diffuse,
                   float4 textureColor, float4 factor, float4 current) {
    switch (op) {
        case 1: return current.a;
        case 2: return a.a;
        case 3: return b.a;
        case 4: return a.a * b.a;
        case 5: return saturate(a.a * b.a * 2.0);
        case 6: return saturate(a.a * b.a * 4.0);
        case 7: return saturate(a.a + b.a);
        case 8: return saturate(a.a + b.a - 0.5);
        case 9: return saturate((a.a + b.a - 0.5) * 2.0);
        case 10: return saturate(a.a - b.a);
        case 11: return saturate(a.a + b.a * (1.0 - a.a));
        case 12: return mix(b.a, a.a, diffuse.a);
        case 13: return mix(b.a, a.a, textureColor.a);
        case 14: return mix(b.a, a.a, factor.a);
        case 15: return saturate(a.a + b.a * (1.0 - textureColor.a));
        case 16: return mix(b.a, a.a, current.a);
        default: return a.a * b.a;
    }
}

// D3DTOP_BUMPENVMAP (22) / D3DTOP_BUMPENVMAPLUMINANCE (23): this stage's own
// texture is a signed bump map (Du, Dv in its r/g channels, already stored
// unsigned-biased as 2*x-1 so the usual [0,1] sampling recovers [-1,1]). It
// contributes no colour of its own - instead it offsets the NEXT stage's
// texture coordinates through the 2x2 BUMPENVMAT, and (LUMINANCE variant
// only) scales the next stage's result by its own blue/"L" channel. Returns
// true if `op` was actually a bump op (caller should skip the normal combine
// for this stage in that case).
bool dk2_apply_bump_env(uint op, float4 bumpTexColor,
                        float m00, float m01, float m10, float m11,
                        float lscale, float loffset,
                        thread float2 &nextTexCoord, thread float &pendingLuminance) {
    if (op != 22 && op != 23) return false;
    const float2 duv = bumpTexColor.rg * 2.0 - 1.0;
    nextTexCoord += float2(m00 * duv.x + m10 * duv.y, m01 * duv.x + m11 * duv.y);
    if (op == 23) pendingLuminance = saturate(bumpTexColor.b * lscale + loffset);
    return true;
}

fragment float4 dk2_fragment(DK2RasterVertex input [[stage_in]],
                             array<texture2d<float>, 128> textures [[texture(0)]],
                             sampler textureSampler [[sampler(0)]]) {
    const float4 factor = dk2_unpack_color(input.textureFactor);
    float4 current = input.color;
    float2 texCoord1 = input.texCoord1;
    float2 texCoord2 = input.texCoord2;
    float pendingLuminance = -1.0;  // < 0 means "no pending luminance factor"

    // Stage 0.
    {
        const float4 textureColor = textures[input.textureIndex].sample(textureSampler, input.texCoord);
        if (!dk2_apply_bump_env(input.colorOp, textureColor,
                                input.bumpEnvMat0_00, input.bumpEnvMat0_01,
                                input.bumpEnvMat0_10, input.bumpEnvMat0_11,
                                input.bumpEnvLScale0, input.bumpEnvLOffset0,
                                texCoord1, pendingLuminance)) {
            const float4 colorArg1 = dk2_texture_arg(input.colorArg1, textureColor, current, input.color, factor);
            const float4 colorArg2 = dk2_texture_arg(input.colorArg2, textureColor, current, input.color, factor);
            const float4 alphaArg1 = dk2_texture_arg(input.alphaArg1, textureColor, current, input.color, factor);
            const float4 alphaArg2 = dk2_texture_arg(input.alphaArg2, textureColor, current, input.color, factor);
            current = float4(
                dk2_color_op(input.colorOp, colorArg1, colorArg2,
                             input.color, textureColor, factor, current),
                dk2_alpha_op(input.alphaOp, alphaArg1, alphaArg2,
                             input.color, textureColor, factor, current));
        }
    }

    // Stage 1: colorOp1/alphaOp1 default to D3DTOP_DISABLE (1), which just
    // returns `current` unchanged below, so a draw that never sets stage-1
    // state (the overwhelming majority) costs one extra texture sample
    // through a texture the game never bound (index 0 = the shared white
    // texture) and is otherwise a no-op.
    {
        // mesh-path alpha test: original cutouts (bars, wall-top holes) kill
        // texels below the reference and draw the rest fully opaque
        if ((input.meshFlags & 8u) != 0u) {
            if (current.a < 0.5f) discard_fragment();
            current.a = 1.0f;
        }
        const float4 textureColor1 = textures[input.textureIndex1].sample(textureSampler, texCoord1);
        if (!dk2_apply_bump_env(input.colorOp1, textureColor1,
                                input.bumpEnvMat1_00, input.bumpEnvMat1_01,
                                input.bumpEnvMat1_10, input.bumpEnvMat1_11,
                                input.bumpEnvLScale1, input.bumpEnvLOffset1,
                                texCoord2, pendingLuminance)) {
            const float4 colorArg1_1 = dk2_texture_arg(input.colorArg1_1, textureColor1, current, input.color, factor);
            const float4 colorArg2_1 = dk2_texture_arg(input.colorArg2_1, textureColor1, current, input.color, factor);
            const float4 alphaArg1_1 = dk2_texture_arg(input.alphaArg1_1, textureColor1, current, input.color, factor);
            const float4 alphaArg2_1 = dk2_texture_arg(input.alphaArg2_1, textureColor1, current, input.color, factor);
            current = float4(
                dk2_color_op(input.colorOp1, colorArg1_1, colorArg2_1,
                             input.color, textureColor1, factor, current),
                dk2_alpha_op(input.alphaOp1, alphaArg1_1, alphaArg2_1,
                             input.color, textureColor1, factor, current));
            if (pendingLuminance >= 0.0) {
                current.rgb *= pendingLuminance;
                pendingLuminance = -1.0;
            }
        }
    }

    // Stage 2: same default-disabled convention. Always terminal (no stage 3
    // to perturb), so D3DTOP_BUMPENVMAP here would be a no-op in hardware too.
    {
        const float4 textureColor2 = textures[input.textureIndex2].sample(textureSampler, texCoord2);
        const float4 colorArg1_2 = dk2_texture_arg(input.colorArg1_2, textureColor2, current, input.color, factor);
        const float4 colorArg2_2 = dk2_texture_arg(input.colorArg2_2, textureColor2, current, input.color, factor);
        const float4 alphaArg1_2 = dk2_texture_arg(input.alphaArg1_2, textureColor2, current, input.color, factor);
        const float4 alphaArg2_2 = dk2_texture_arg(input.alphaArg2_2, textureColor2, current, input.color, factor);
        current = float4(
            dk2_color_op(input.colorOp2, colorArg1_2, colorArg2_2,
                         input.color, textureColor2, factor, current),
            dk2_alpha_op(input.alphaOp2, alphaArg1_2, alphaArg2_2,
                         input.color, textureColor2, factor, current));
        if (pendingLuminance >= 0.0) current.rgb *= pendingLuminance;
    }

    // Modern water/lava overlay (DK2WaterShader.metal). materialFlags is set
    // host-side for draws sampling a water-/lava-named atlas region.
    if ((input.materialFlags & 3u) != 0u) {
        current = dk2_water_overlay(current, input.worldPos,
                                    input.waterTime,
                                    (input.materialFlags & 2u) != 0u);
    }

    return current;
}
