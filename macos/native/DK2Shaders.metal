#include <metal_stdlib>
using namespace metal;

struct DK2Vertex1C {
    float x;
    float y;
    float z;
    float rhw;
    uint diffuse;
    float u;
    float v;
};

struct DK2Vertex2C {
    float x;
    float y;
    float z;
    float rhw;
    uint diffuse;
    float texCoord[3][2];
};

struct DK2DrawUniform {
    float screenWidth;
    float screenHeight;
    uint textureIndex;
    uint colorOp;
    uint colorArg1;
    uint colorArg2;
    uint alphaOp;
    uint alphaArg1;
    uint alphaArg2;
    uint textureFactor;
    // Stage 1 (e.g. water/lava env or lightmap combine). colorOp/alphaOp
    // default to D3DTOP_DISABLE (1) whenever the game never sets stage-1
    // state, which dk2_color_op/dk2_alpha_op already treat as "pass `current`
    // through unchanged" - so untouched draws are bit-for-bit identical to
    // the single-stage path.
    uint textureIndex1;
    uint colorOp1;
    uint colorArg1_1;
    uint colorArg2_1;
    uint alphaOp1;
    uint alphaArg1_1;
    uint alphaArg2_1;
    // Stage 2. Same default-disabled convention as stage 1.
    uint textureIndex2;
    uint colorOp2;
    uint colorArg1_2;
    uint colorArg2_2;
    uint alphaOp2;
    uint alphaArg1_2;
    uint alphaArg2_2;
    // D3DTOP_BUMPENVMAP / D3DTOP_BUMPENVMAPLUMINANCE parameters, one set per
    // stage that can carry that op (stage 2 is always last, so it never
    // perturbs a further stage). Classic DX7 environment-bump-mapped water:
    // stage N samples a signed bump texture and uses these to offset stage
    // N+1's texture coordinates instead of contributing a colour itself.
    float bumpEnvMat0_00;
    float bumpEnvMat0_01;
    float bumpEnvMat0_10;
    float bumpEnvMat0_11;
    float bumpEnvLScale0;
    float bumpEnvLOffset0;
    float bumpEnvMat1_00;
    float bumpEnvMat1_01;
    float bumpEnvMat1_10;
    float bumpEnvMat1_11;
    float bumpEnvLScale1;
    float bumpEnvLOffset1;
    // Metal shadows: 1 when D3DRS_ZENABLE was on for this draw (depth-tested
    // world geometry, eligible for shadow-coverage darkening in
    // dk2_fragment). Mirrors kWorldGeometryShadowBit's meaning for the mesh
    // path - see DK2Metal.mm.
    uint worldGeometry;
};

// Metal shadows: host-only bit (never a wire-protocol bit) stitched into
// DK2RasterVertex.meshFlags for BOTH the legacy (via draw.worldGeometry) and
// mesh (via MeshDrawUniform.flags) vertex stages, so dk2_fragment can gate
// the darkening pass with one check regardless of which produced the
// fragment. Must match kWorldGeometryShadowBit in DK2Metal.mm.
constant uint kWorldGeometryShadowBit = 1u << 5;

struct DK2RasterVertex {
    float4 position [[position]];
    float4 color;
    float2 texCoord;
    float2 texCoord1;
    float2 texCoord2;
    uint textureIndex [[flat]];
    uint colorOp [[flat]];
    uint colorArg1 [[flat]];
    uint colorArg2 [[flat]];
    uint alphaOp [[flat]];
    uint alphaArg1 [[flat]];
    uint alphaArg2 [[flat]];
    uint textureFactor [[flat]];
    uint textureIndex1 [[flat]];
    uint colorOp1 [[flat]];
    uint colorArg1_1 [[flat]];
    uint colorArg2_1 [[flat]];
    uint alphaOp1 [[flat]];
    uint alphaArg1_1 [[flat]];
    uint alphaArg2_1 [[flat]];
    uint textureIndex2 [[flat]];
    uint colorOp2 [[flat]];
    uint colorArg1_2 [[flat]];
    uint colorArg2_2 [[flat]];
    uint alphaOp2 [[flat]];
    uint alphaArg1_2 [[flat]];
    uint alphaArg2_2 [[flat]];
    float bumpEnvMat0_00 [[flat]];
    float bumpEnvMat0_01 [[flat]];
    float bumpEnvMat0_10 [[flat]];
    float bumpEnvMat0_11 [[flat]];
    float bumpEnvLScale0 [[flat]];
    float bumpEnvLOffset0 [[flat]];
    float bumpEnvMat1_00 [[flat]];
    float bumpEnvMat1_01 [[flat]];
    float bumpEnvMat1_10 [[flat]];
    float bumpEnvMat1_11 [[flat]];
    float bumpEnvLScale1 [[flat]];
    float bumpEnvLOffset1 [[flat]];
    uint meshFlags [[flat]];   // DK2M_DRAW_MESH_* for mesh-path draws, else 0
};

DK2RasterVertex dk2_make_vertex(float x, float y, float z, float rhw, uint diffuse,
                                float2 texCoord, float2 texCoord1, float2 texCoord2,
                                thread const DK2DrawUniform &draw) {
    const float reciprocalW = abs(rhw) > 0.000001f ? rhw : 1.0f;
    const float clipW = 1.0f / reciprocalW;
    DK2RasterVertex result;
    result.position = float4((x * 2.0f / draw.screenWidth - 1.0f) * clipW,
                             (1.0f - y * 2.0f / draw.screenHeight) * clipW,
                             z * clipW, clipW);
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
    result.meshFlags = draw.worldGeometry ? kWorldGeometryShadowBit : 0u;
    return result;
}

vertex DK2RasterVertex dk2_vertex_1c(device const DK2Vertex1C *vertices [[buffer(0)]],
                                     device const DK2DrawUniform *draws [[buffer(1)]],
                                     uint vertexID [[vertex_id]],
                                     uint drawID [[instance_id]]) {
    const DK2Vertex1C inputVertex = vertices[vertexID];
    const DK2DrawUniform draw = draws[drawID];
    const float2 uv = float2(inputVertex.u, inputVertex.v);
    return dk2_make_vertex(inputVertex.x, inputVertex.y, inputVertex.z, inputVertex.rhw,
                           inputVertex.diffuse, uv, uv, uv, draw);
}

vertex DK2RasterVertex dk2_vertex_2c(device const DK2Vertex2C *vertices [[buffer(0)]],
                                     device const DK2DrawUniform *draws [[buffer(1)]],
                                     uint vertexID [[vertex_id]],
                                     uint drawID [[instance_id]]) {
    const DK2Vertex2C inputVertex = vertices[vertexID];
    const DK2DrawUniform draw = draws[drawID];
    return dk2_make_vertex(inputVertex.x, inputVertex.y, inputVertex.z, inputVertex.rhw,
                           inputVertex.diffuse,
                           float2(inputVertex.texCoord[0][0], inputVertex.texCoord[0][1]),
                           float2(inputVertex.texCoord[1][0], inputVertex.texCoord[1][1]),
                           float2(inputVertex.texCoord[2][0], inputVertex.texCoord[2][1]),
                           draw);
}

float4 dk2_unpack_color(uint value) {
    return float4(float((value >> 16) & 0xFF), float((value >> 8) & 0xFF),
                  float(value & 0xFF), float((value >> 24) & 0xFF)) / 255.0;
}

// --- world-space mesh pipeline (protocol v9) ---
// The game registers object-space meshes once; per frame it sends camera,
// lights and per-instance world transforms, and this path does the
// transform+lighting the original engine did per-vertex on the CPU
// (see Obj57BCB0.cpp::calculateLighting for the model being ported).

struct DK2MeshVertexIn {   // matches DK2MMeshVertex (36 bytes, packed)
    packed_float3 position;
    packed_float3 normal;
    float u;
    float v;
    uint baseColor;
};

struct DK2MeshDrawUniform {
    float4 world0;   // rows of the 3x4 world transform
    float4 world1;
    float4 world2;
    float4 ambient;  // additive per-draw ambient (rgb), w unused
    uint textureIndex;
    uint tint;
    uint flags;      // DK2M_DRAW_MESH_LIT etc.
    uint pad;
};

struct DK2MeshCamera {
    float4x4 viewProj;   // column-major world -> clip
    float zMul2, zAdd2;      // near-branch linear depth
    float zAdd3, zMul3F;     // far-branch hyperbolic depth
    float farThreshold;      // branch switch (view z above -> far branch)
    float depthCap;          // maximum depth value
    float pad0, pad1;
};

struct DK2MeshLightsHeader {
    uint count;
    float ambientR;
    float ambientG;
    float ambientB;
    float lut[256];      // the engine's falloff LUT captured from the game
};

struct DK2MeshLight {    // matches DK2MLight (48 bytes)
    packed_float3 position;
    packed_float3 color;
    float distSqLimit;
    float attenScale;
    float facingScale;
    float pad0, pad1, pad2;
};

float3 dk2_mesh_accumulate_lights(float3 positionWorld, float3 normalWorld, float3 base,
                                  constant DK2MeshLightsHeader &header,
                                  device const DK2MeshLight *lights) {
    float3 color = base + float3(header.ambientR, header.ambientG, header.ambientB);
    for (uint i = 0; i < header.count; ++i) {
        const device DK2MeshLight &light = lights[i];
        const float3 d = positionWorld - float3(light.position);
        const float d2 = dot(d, d);
        if (!(d2 < light.distSqLimit)) continue;
        // The engine's float-bit LUT index trick (add 12582912, read mantissa)
        // is just round-to-nearest of (16*d2 - 0.49999) == floor(16*d2).
        // Metal's fast-math FMA fusion breaks the exact bit pattern, so
        // compute the index arithmetically instead.
        const uint index = static_cast<uint>(16.0f * d2);
        if (index >= 256u) continue;
        const float atten = (light.distSqLimit - d2) * header.lut[index] * light.attenScale;
        const float facing = dot(normalWorld, -d) * light.facingScale;
        if (facing < 0.0f) continue;
        const float factor = atten * facing;
        color += factor > 1.0f ? float3(light.color) : float3(light.color) * factor;
    }
    return color;
}

vertex DK2RasterVertex dk2_vertex_mesh(device const DK2MeshVertexIn *vertices [[buffer(2)]],
                                       constant DK2MeshCamera &camera [[buffer(3)]],
                                       constant DK2MeshLightsHeader &lightsHeader [[buffer(4)]],
                                       device const DK2MeshLight *lights [[buffer(5)]],
                                       device const DK2MeshDrawUniform *draws [[buffer(6)]],
                                       uint vertexID [[vertex_id]],
                                       uint drawID [[instance_id]]) {
    const DK2MeshVertexIn inputVertex = vertices[vertexID];
    const DK2MeshDrawUniform draw = draws[drawID];
    const float4 p = float4(float3(inputVertex.position), 1.0f);
    const float3 positionWorld = float3(dot(draw.world0, p), dot(draw.world1, p), dot(draw.world2, p));
    const float3 n = float3(inputVertex.normal);
    const float3 normalWorld = float3(dot(draw.world0.xyz, n), dot(draw.world1.xyz, n),
                                      dot(draw.world2.xyz, n));
    const float4 base = dk2_unpack_color(inputVertex.baseColor);
    const float4 tint = dk2_unpack_color(draw.tint);
    float3 lit = base.rgb + draw.ambient.rgb;
    if ((draw.flags & 1u) != 0u) {  // DK2M_DRAW_MESH_LIT
        lit = dk2_mesh_accumulate_lights(positionWorld, normalWorld,
                                         base.rgb + draw.ambient.rgb,
                                         lightsHeader, lights);
    }
    DK2RasterVertex result;
    result.position = camera.viewProj * float4(positionWorld, 1.0f);
    // Replicate the engine's piecewise depth exactly: the matrix row only
    // covers the far (hyperbolic) branch, near geometry uses the linear one.
    {
        const float viewZ = result.position.w;
        float depth = camera.farThreshold < viewZ
            ? camera.zAdd3 - camera.zMul3F / viewZ
            : camera.zMul2 * viewZ + camera.zAdd2;
        depth = min(depth, camera.depthCap);
        result.position.z = depth * viewZ;
        // Behind-camera guard: with viewZ < 0 both z and w go negative and
        // z/w re-enters [0,1], so triangles crossing the camera plane smear
        // into giant screen-covering shapes (the legacy CPU path clipped by
        // frustum outcode before projecting). Push such vertices far outside
        // the z clip volume so the hardware clips the crossing triangles.
        if (viewZ < 1e-3f) {
            result.position.z = -2.0f * abs(result.position.w) - 1.0f;
        }
    }
    result.color = float4(saturate(lit) * tint.rgb, base.a * tint.a);
    result.texCoord = float2(inputVertex.u, inputVertex.v);
    result.texCoord1 = result.texCoord;
    result.texCoord2 = result.texCoord;
    result.textureIndex = draw.textureIndex;
    result.colorOp = 4;   // MODULATE texture x lit diffuse
    result.colorArg1 = 2; // TEXTURE
    result.colorArg2 = 0; // DIFFUSE
    result.alphaOp = 4;
    result.alphaArg1 = 2;
    result.alphaArg2 = 0;
    result.textureFactor = 0xFFFFFFFFu;
    result.textureIndex1 = 0;
    result.colorOp1 = 1;  // DISABLE
    result.colorArg1_1 = 2;
    result.colorArg2_1 = 1;
    result.alphaOp1 = 1;
    result.alphaArg1_1 = 2;
    result.alphaArg2_1 = 1;
    result.textureIndex2 = 0;
    result.colorOp2 = 1;
    result.colorArg1_2 = 2;
    result.colorArg2_2 = 1;
    result.alphaOp2 = 1;
    result.alphaArg1_2 = 2;
    result.alphaArg2_2 = 1;
    result.bumpEnvMat0_00 = 1.0f;
    result.bumpEnvMat0_01 = 0.0f;
    result.bumpEnvMat0_10 = 0.0f;
    result.bumpEnvMat0_11 = 1.0f;
    result.bumpEnvLScale0 = 1.0f;
    result.bumpEnvLOffset0 = 0.0f;
    result.bumpEnvMat1_00 = 1.0f;
    result.bumpEnvMat1_01 = 0.0f;
    result.bumpEnvMat1_10 = 0.0f;
    result.bumpEnvMat1_11 = 1.0f;
    result.bumpEnvLScale1 = 1.0f;
    result.bumpEnvLOffset1 = 0.0f;
    result.meshFlags = draw.flags;
    return result;
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

// --- Metal shadows: GPU shadow-coverage map for mesh-path casters ---
// See DK2M_DRAW_MESH_SHADOW_CASTER in DK2BridgeProtocol.h and the session
// report for the full derivation. Mirrors ShadowGlobalUniform in
// DK2Metal.mm exactly (bound at buffer(7) in every argument table bank,
// alongside the coverage texture at texture(128)).
struct DK2ShadowGlobalUniform {
    float4x4 invReconstruct;  // (ndcX*viewZ, ndcY*viewZ, viewZ, 1) -> world xyz1
    float screenWidth;
    float screenHeight;
    float casterMinZ;
    float casterMaxZ;
    float upSign;             // +1: caster base = casterMinZ, -1: = casterMaxZ
    float epsilon;
    float darkenStrength;
    uint active;               // 1 iff shadows enabled AND a camera AND casters arrived this frame
    float shadowCenterX, shadowCenterY;
    float shadowHalfExtentX, shadowHalfExtentY;
    uint pad0, pad1, pad2, pad3;
};

// Reusing the caster vertex layout for the shadow pass's own vertex buffer -
// identical bytes to DK2MeshVertexIn (36-byte DK2MMeshVertex), only position
// is read here (casters arrive as world-space vertices with implicit
// identity world transform, same as any other DRAW_MESH_INLINE).
vertex float4 dk2_shadow_vertex(device const DK2MeshVertexIn *vertices [[buffer(0)]],
                                constant DK2ShadowGlobalUniform &shadow [[buffer(1)]],
                                uint vertexID [[vertex_id]]) {
    const float3 p = float3(vertices[vertexID].position);
    const float ndcX = (p.x - shadow.shadowCenterX) / max(shadow.shadowHalfExtentX, 0.0001f);
    // Flipped so the UV formula used when SAMPLING the coverage map back in
    // dk2_fragment (u,v = (world - center)/(2*halfExtent) + 0.5, standard
    // Metal top-left-origin texture convention) lines up with this pass's
    // NDC -> render-target-row mapping without a separate y-flip constant.
    const float ndcY = -(p.y - shadow.shadowCenterY) / max(shadow.shadowHalfExtentY, 0.0001f);
    return float4(ndcX, ndcY, 0.5, 1.0);
}

fragment float4 dk2_shadow_fragment() {
    // Binary coverage: any caster triangle covering this texel writes full
    // white (R8Unorm; only the red channel is read back). No blending, no
    // depth test - overlapping casters are idempotent here.
    return float4(1.0, 0.0, 0.0, 0.0);
}

// Cheap 5-tap cross box filter (see the task's "bilinear + poisson" note) -
// softens the coverage map's hard silhouette edges without a separate blur
// pass. Weighted 4:1:1:1:1 (center:N/S/E/W) so a fully-covered neighbourhood
// still saturates to ~1.0.
float dk2_sample_shadow_coverage(texture2d<float> tex, sampler smp, float2 uv) {
    const float2 texel = float2(1.0 / float(tex.get_width()), 1.0 / float(tex.get_height()));
    float sum = tex.sample(smp, uv).r * 4.0;
    sum += tex.sample(smp, uv + float2(texel.x, 0.0)).r;
    sum += tex.sample(smp, uv - float2(texel.x, 0.0)).r;
    sum += tex.sample(smp, uv + float2(0.0, texel.y)).r;
    sum += tex.sample(smp, uv - float2(0.0, texel.y)).r;
    return saturate(sum / 8.0);
}

fragment float4 dk2_fragment(DK2RasterVertex input [[stage_in]],
                             array<texture2d<float>, 127> textures [[texture(0)]],
                             texture2d<float> shadowCoverage [[texture(127)]],
                             constant DK2ShadowGlobalUniform &shadowGlobal [[buffer(7)]],
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

    // Metal shadows: darken depth-tested world geometry (input.meshFlags has
    // kWorldGeometryShadowBit - see DK2Metal.mm) that lies at/below the
    // shadow casters' ground-contact height, using this frame's coverage map.
    // Skipped entirely (one flag test) when disabled, no camera arrived, or
    // no casters this frame (shadowGlobal.active == 0).
    if ((input.meshFlags & kWorldGeometryShadowBit) != 0u && shadowGlobal.active != 0u) {
        // input.position (fragment-stage [[position]]) is window-space: xy in
        // pixels, w = 1/clipW. clipW equals viewZ for both vertex stages
        // (dk2_make_vertex sets w=clipW=1/rhw=viewZ; dk2_vertex_mesh's
        // position.w is camera.viewProj's w row, also viewZ) - see the
        // session report.
        const float clipW = 1.0 / input.position.w;
        if (clipW > 1e-4) {
            const float ndcX = (input.position.x / shadowGlobal.screenWidth) * 2.0 - 1.0;
            const float ndcY = 1.0 - (input.position.y / shadowGlobal.screenHeight) * 2.0;
            // Reconstruct the ORIGINAL clip-space vector (pre perspective
            // divide) from screen xy + viewZ alone, deliberately NOT using
            // the piecewise-remapped clip.z (see invertMatrix4x4 call site in
            // DK2Metal.mm for why that row is excluded from the inverse).
            const float4 reconVec = float4(ndcX * clipW, ndcY * clipW, clipW, 1.0);
            const float3 worldPos = (shadowGlobal.invReconstruct * reconVec).xyz;
            const float base = shadowGlobal.upSign > 0.0
                ? shadowGlobal.casterMinZ : shadowGlobal.casterMaxZ;
            const bool below = shadowGlobal.upSign > 0.0
                ? worldPos.z <= base + shadowGlobal.epsilon
                : worldPos.z >= base - shadowGlobal.epsilon;
            if (below) {
                const float u = (worldPos.x - shadowGlobal.shadowCenterX) /
                    (2.0 * shadowGlobal.shadowHalfExtentX) + 0.5;
                const float v = (worldPos.y - shadowGlobal.shadowCenterY) /
                    (2.0 * shadowGlobal.shadowHalfExtentY) + 0.5;
                if (u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0) {
                    const float coverage = dk2_sample_shadow_coverage(shadowCoverage, textureSampler,
                                                                      float2(u, v));
                    current.rgb *= (1.0 - shadowGlobal.darkenStrength * coverage);
                }
            }
        }
    }

    return current;
}

// --- bloom (see dk2BloomEnabled in DK2Metal.mm) ---
// A conservative, low-intensity glow for lava/fire/torches: threshold-extract
// bright pixels, blur at half resolution with a separable fixed kernel, then
// add back onto the resolved scene. All four passes below are fullscreen
// triangles driven purely by vertex_id - no vertex/index buffers needed.

struct DK2BloomVaryings {
    float4 position [[position]];
    float2 uv;
};

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

constant float kDK2BloomThreshold = 0.7;

fragment float4 dk2_bloom_threshold(DK2BloomVaryings in [[stage_in]],
                                    texture2d<float> scene [[texture(0)]],
                                    sampler smp [[sampler(0)]]) {
    const float4 color = scene.sample(smp, in.uv);
    const float luminance = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    // Soft knee: 0 below threshold, ramps to full contribution by
    // luminance == 1 so only genuinely bright pixels (lava, fire, torches)
    // feed the glow.
    const float contribution = saturate((luminance - kDK2BloomThreshold) /
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
