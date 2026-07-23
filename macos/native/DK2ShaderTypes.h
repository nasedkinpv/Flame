#pragma once

#include <metal_stdlib>
using namespace metal;

// Shared types for the DK2 Metal shader library. Each .metal file in this
// library includes this header; functions actually shared ACROSS files are
// declared here (defined once, non-static, linked by `metallib`); functions
// used within only one file stay local to that file.

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
    // Reconstructed world XY on the level's Z=0 floor plane (see
    // dk2_unproject_ground in DK2WaterShader.metal) - the water/lava noise
    // domain. Zero for non-liquid draws. NOT [[flat]]: perspective-correct
    // interpolation across the triangle is required here.
    float2 worldPos;
};

// Column-major world->clip camera. Shared by the world-space mesh pipeline
// (DK2MeshShader.metal) and, for the legacy screen-space path, so it can
// unproject fragments back to world space for the water effect
// (DK2WaterShader.metal / DK2LegacyShaders.metal).
struct DK2MeshCamera {
    float4x4 viewProj;
    float zMul2, zAdd2;      // near-branch linear depth
    float zAdd3, zMul3F;     // far-branch hyperbolic depth
    float farThreshold;      // branch switch (view z above -> far branch)
    float depthCap;          // maximum depth value
    float pad0, pad1;
};

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

struct DK2ShadowMaskVertex {
    float2 position;
};

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

struct DK2BloomVaryings {
    float4 position [[position]];
    float2 uv;
};

// Tiny, pure, used from more than one file - cheaper to duplicate per
// translation unit than to link.
static inline float4 dk2_unpack_color(uint value) {
    return float4(float((value >> 16) & 0xFF), float((value >> 8) & 0xFF),
                  float(value & 0xFF), float((value >> 24) & 0xFF)) / 255.0;
}

// --- Cross-file entry points ---
// Defined once in DK2WaterShader.metal (non-static -> external linkage),
// linked into the single DK2Shaders.metallib by `metallib` alongside every
// other .air module. Declared here so DK2LegacyShaders.metal can call them.

// Solve for the world XY that projects to a given NDC on the level's ground
// plane (world Z=0; DK2 world space has X/Y horizontal, Z as height).
float2 dk2_unproject_ground(constant DK2MeshCamera &camera, float2 ndc);

// Simple, tasteful top-down water/lava overlay. `worldXY` is the
// world-anchored ripple domain (see dk2_unproject_ground); `base` is the
// engine-shaded fragment so far (only base.a - tile/shore shape - survives
// into the result, the rest is replaced).
float4 dk2_water_overlay(float4 base, float2 worldXY, float t, bool lava);
