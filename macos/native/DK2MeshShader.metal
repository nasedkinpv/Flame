#include "DK2ShaderTypes.h"

// --- world-space mesh pipeline (introduced in v9; retained/deformed v13) ---
// The game registers object-space meshes once; per frame it sends camera,
// lights and per-instance world transforms, and this path does the
// transform+lighting the original engine did per-vertex on the CPU
// (see Obj57BCB0.cpp::calculateLighting for the model being ported).

static float3 dk2_mesh_accumulate_lights(float3 positionWorld, float3 normalWorld, float3 base,
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
    result.worldPos = float2(0.0f);
    result.meshFlags = draw.flags;
    return result;
}
