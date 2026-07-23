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
    // Host bookkeeping kept in the legacy uniform layout.
    uint worldGeometry;
    // Material class + animation clock for host-side effect overlays (water,
    // lava). materialFlags bit 0 = water, bit 1 = lava; waterTime = seconds.
    uint materialFlags;
    float waterTime;
};

struct DK2RasterVertex {
    float4 position [[position]];
    float clipDistance [[clip_distance]];
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
    uint materialFlags [[flat]];  // 1=water 2=lava (host effect overlays)
    float waterTime [[flat]];
    float2 worldPos;   // reconstructed world XZ on the Y=0 plane (water domain)
};

// Column-major world->clip camera, shared with the mesh path (defined again
// below for the mesh shaders). Declared here so the legacy vertex path can
// unproject screen fragments back to world space for the water effect.
struct DK2MeshCamera {
    float4x4 viewProj;
    float zMul2, zAdd2;
    float zAdd3, zMul3F;
    float farThreshold;
    float depthCap;
    float pad0, pad1;
};

// Solve for the world XY that projects to a given NDC on the ground plane Z=0.
// In DK2 world space X/Y are the horizontal map axes and Z is height, so the
// floor is a Z=const plane; the near-top-down camera never sees it edge-on, so
// there is no horizon singularity. World X (col 0) and Y (col 1) are the
// unknowns; Z (col 2) is pinned to the plane height. Only clip rows x/y/w are
// used (row z is the piecewise-depth far branch, unreliable). Two equations,
// two unknowns.
static float2 dk2_unproject_ground(constant DK2MeshCamera &camera, float2 ndc) {
    const float kPlaneZ = 0.0f;               // water/floor height in world Z
    const float4 A = camera.viewProj[0];      // world x column
    const float4 B = camera.viewProj[1];      // world y column
    const float4 C = camera.viewProj[2];      // world z column (pinned)
    const float4 D = camera.viewProj[3];      // translation column
    const float4 K = C * kPlaneZ + D;         // constant term at z = kPlaneZ
    const float a00 = A.x - ndc.x * A.w, a01 = B.x - ndc.x * B.w;
    const float a10 = A.y - ndc.y * A.w, a11 = B.y - ndc.y * B.w;
    const float b0 = ndc.x * K.w - K.x;
    const float b1 = ndc.y * K.w - K.y;
    const float det = a00 * a11 - a01 * a10;
    if (abs(det) < 1e-12f) return float2(0.0f);
    const float inv = 1.0f / det;
    return float2((b0 * a11 - a01 * b1) * inv, (a00 * b1 - b0 * a10) * inv);
}

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
    // World XZ for water/lava draws only (2x2 solve is wasted on everything
    // else). The ground-plane assumption pans correctly with the camera.
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

float4 dk2_unpack_color(uint value) {
    return float4(float((value >> 16) & 0xFF), float((value >> 8) & 0xFF),
                  float(value & 0xFF), float((value >> 24) & 0xFF)) / 255.0;
}

// --- world-space mesh pipeline (introduced in v9; retained/deformed v13) ---
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
    float4 uvTransform;  // scaleU, scaleV, offsetU, offsetV
    float4 ambient;  // additive per-draw ambient (rgb), w unused
    uint textureIndex;
    uint tint;
    uint flags;      // DK2M_DRAW_MESH_LIT etc.
    uint pad;
    uint lightCount;
    ushort lightIndices[24];
    uint lightPad0, lightPad1, lightPad2;
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
                                  thread const DK2MeshDrawUniform &draw,
                                  constant DK2MeshLightsHeader &header,
                                  device const DK2MeshLight *lights) {
    float3 color = base + float3(header.ambientR, header.ambientG, header.ambientB);
    for (uint selection = 0; selection < draw.lightCount; ++selection) {
        const uint index = draw.lightIndices[selection];
        if (index >= header.count) continue;
        const device DK2MeshLight &light = lights[index];
        const float3 d = positionWorld - float3(light.position);
        const float d2 = dot(d, d);
        if (!(d2 < light.distSqLimit)) continue;
        // The engine's float-bit LUT index trick (add 12582912, read mantissa)
        // is just round-to-nearest of (16*d2 - 0.49999) == floor(16*d2).
        // Metal's fast-math FMA fusion breaks the exact bit pattern, so
        // compute the index arithmetically instead.
        const uint lutIndex = static_cast<uint>(16.0f * d2);
        if (lutIndex >= 256u) continue;
        const float atten = (light.distSqLimit - d2) * header.lut[lutIndex] * light.attenScale;
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
                                      dot(draw.world2.xyz, n)) /
                               max(length(draw.world0.xyz), 1e-8f);
    const float4 base = dk2_unpack_color(inputVertex.baseColor);
    const float4 tint = dk2_unpack_color(draw.tint);
    float3 lit = base.rgb + draw.ambient.rgb;
    if ((draw.flags & 1u) != 0u) {  // DK2M_DRAW_MESH_LIT
        lit = dk2_mesh_accumulate_lights(positionWorld, normalWorld,
                                         base.rgb + draw.ambient.rgb, draw,
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
        // The legacy CPU path clipped triangles before projection. A real
        // clip distance preserves the camera-plane intersection; moving only
        // a behind-camera vertex's z still leaves a stretched triangle.
        result.clipDistance = viewZ - 1e-3f;
    }
    result.color = float4(saturate(lit) * tint.rgb, base.a * tint.a);
    result.texCoord = float2(
        inputVertex.u * draw.uvTransform.x + draw.uvTransform.z,
        inputVertex.v * draw.uvTransform.y + draw.uvTransform.w);
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
    result.materialFlags = 0u;   // world-mesh path is not water/lava tagged
    result.waterTime = 0.0f;
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

// --- Original DK2 shadow silhouettes, GPU rasterized ---
// The game keeps light selection, projection and decal rendering. These
// stages reproduce only its 256x256-subpixel accumulator and 8x8 reduction.
struct DK2ShadowMaskVertex {
    float2 position;
};

vertex float4 dk2_shadow_mask_vertex(
    device const DK2ShadowMaskVertex *vertices [[buffer(0)]],
    uint vertexID [[vertex_id]]) {
    const float2 p = vertices[vertexID].position;
    return float4(p.x / 128.0 - 1.0, 1.0 - p.y / 128.0, 0.0, 1.0);
}

fragment float4 dk2_shadow_mask_fragment() {
    return float4(1.0 / 255.0, 0.0, 0.0, 0.0);
}

struct DK2ShadowResolveUniform {
    uint scratchX;
    uint scratchY;
    uint targetX;
    uint targetY;
    uint mode;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct DK2ShadowResolveVaryings {
    float4 position [[position]];
    uint maskIndex [[flat]];
};

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

// Value noise + fbm for caustics/glints (cheap, tileable enough for water).
static inline float dk2_hash21(float2 p) {
    p = fract(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return fract(p.x * p.y);
}
static inline float dk2_vnoise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    float a = dk2_hash21(i);
    float b = dk2_hash21(i + float2(1.0f, 0.0f));
    float c = dk2_hash21(i + float2(0.0f, 1.0f));
    float d = dk2_hash21(i + float2(1.0f, 1.0f));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Four-octave fbm over the value noise above.
static inline float dk2_fbm(float2 p) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sum += amp * dk2_vnoise(p);
        p *= 2.03f;
        amp *= 0.5f;
    }
    return sum;
}

// Simple, tasteful top-down water/lava. Standard practice: two slow scrolling
// fbm layers give a height field; its gradient is the surface normal; a fixed
// sun gives one soft broad specular; a gentle grazing (fresnel-ish) term lifts
// the rim. Calm palette, no high-frequency sparkle spam. The domain is the
// world XY reconstructed on the floor plane, so it is physically fixed to the
// level. Only base.a (tile/shore shape) is kept from the engine texture.
static inline float4 dk2_water_overlay(float4 base, float2 worldXY,
                                       float t, bool lava) {
    const float kScale = 0.020f;              // world units -> ripple size (tune)
    const float2 p = worldXY * kScale;
    const float flow = lava ? 0.35f : 1.0f;   // lava creeps, water flows
    const float2 a = p        + float2(0.030f, 0.021f) * t * flow;
    const float2 b = p * 1.7f - float2(0.019f, 0.034f) * t * flow;

    const float e = 0.15f;
    const float h  = dk2_fbm(a)            * 0.6f + dk2_fbm(b)            * 0.4f;
    const float hx = dk2_fbm(a + float2(e, 0)) * 0.6f + dk2_fbm(b + float2(e, 0)) * 0.4f;
    const float hy = dk2_fbm(a + float2(0, e)) * 0.6f + dk2_fbm(b + float2(0, e)) * 0.4f;
    const float slope = lava ? 1.6f : 2.4f;
    const float3 N = normalize(float3((h - hx) * slope / e, (h - hy) * slope / e, 1.0f));

    const float3 V = float3(0.0f, 0.0f, 1.0f);               // near top-down
    const float3 L = normalize(float3(0.4f, 0.5f, 0.75f));   // fixed sun
    const float3 H = normalize(L + V);
    const float spec = pow(saturate(dot(N, H)), lava ? 30.0f : 60.0f);  // broad, soft
    const float fres = pow(1.0f - saturate(N.z), 3.0f);      // grazing lift

    float3 col;
    if (lava) {
        const float3 deep = float3(0.30f, 0.045f, 0.02f);
        const float3 hot  = float3(1.7f, 0.62f, 0.14f);
        col = mix(deep, hot, saturate(h + 0.15f));
        col += spec * float3(2.0f, 1.1f, 0.45f);            // molten glint
        col += fres * float3(0.5f, 0.16f, 0.05f);           // heat rim
    } else {
        const float3 deep    = float3(0.03f, 0.15f, 0.22f);
        const float3 shallow = float3(0.10f, 0.34f, 0.42f);
        col = mix(deep, shallow, saturate(h + 0.2f));
        col += spec * float3(1.0f, 0.97f, 0.9f) * 0.9f;     // soft sun
        col = mix(col, float3(0.35f, 0.5f, 0.6f), fres * 0.25f); // sky rim
    }
    return float4(col, base.a);
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

    // Modern water/lava overlay (see dk2_water_overlay). materialFlags is set
    // host-side for draws sampling a water-/lava-named atlas region.
    if ((input.materialFlags & 3u) != 0u) {
        current = dk2_water_overlay(current, input.worldPos,
                                    input.waterTime,
                                    (input.materialFlags & 2u) != 0u);
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
