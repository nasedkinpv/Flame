#include <metal_stdlib>
using namespace metal;

struct DK2MetalVertex {
    float4 position;
    float4 color;
};

struct DK2RasterVertex {
    float4 position [[position]];
    float4 color;
};

vertex DK2RasterVertex dk2_vertex(device const DK2MetalVertex *vertices [[buffer(0)]],
                                   uint vertexID [[vertex_id]]) {
    DK2RasterVertex result;
    result.position = vertices[vertexID].position;
    result.color = vertices[vertexID].color;
    return result;
}

fragment float4 dk2_fragment(DK2RasterVertex input [[stage_in]]) {
    return input.color;
}
