#include <metal_stdlib>
using namespace metal;

struct DK2MetalVertex {
    float4 position;
    float4 color;
    float2 texCoord;
    uint textureIndex;
    uint padding;
};

struct DK2RasterVertex {
    float4 position [[position]];
    float4 color;
    float2 texCoord;
    uint textureIndex [[flat]];
};

vertex DK2RasterVertex dk2_vertex(device const DK2MetalVertex *vertices [[buffer(0)]],
                                   uint vertexID [[vertex_id]]) {
    DK2RasterVertex result;
    result.position = vertices[vertexID].position;
    result.color = vertices[vertexID].color;
    result.texCoord = vertices[vertexID].texCoord;
    result.textureIndex = vertices[vertexID].textureIndex;
    return result;
}

fragment float4 dk2_fragment(DK2RasterVertex input [[stage_in]],
                             array<texture2d<float>, 128> textures [[texture(0)]],
                             sampler textureSampler [[sampler(0)]]) {
    return textures[input.textureIndex].sample(textureSampler, input.texCoord) * input.color;
}
