#include <metal_stdlib>
using namespace metal;

struct DK2MetalVertex {
    float4 position;
    float4 color;
    float2 texCoord;
    uint textureIndex;
    uint colorOp;
    uint colorArg1;
    uint colorArg2;
    uint alphaOp;
    uint alphaArg1;
    uint alphaArg2;
    uint textureFactor;
    uint padding;
    uint padding2;
};

struct DK2RasterVertex {
    float4 position [[position]];
    float4 color;
    float2 texCoord;
    uint textureIndex [[flat]];
    uint colorOp [[flat]];
    uint colorArg1 [[flat]];
    uint colorArg2 [[flat]];
    uint alphaOp [[flat]];
    uint alphaArg1 [[flat]];
    uint alphaArg2 [[flat]];
    uint textureFactor [[flat]];
};

vertex DK2RasterVertex dk2_vertex(device const DK2MetalVertex *vertices [[buffer(0)]],
                                   uint vertexID [[vertex_id]]) {
    DK2RasterVertex result;
    result.position = vertices[vertexID].position;
    result.color = vertices[vertexID].color;
    result.texCoord = vertices[vertexID].texCoord;
    result.textureIndex = vertices[vertexID].textureIndex;
    result.colorOp = vertices[vertexID].colorOp;
    result.colorArg1 = vertices[vertexID].colorArg1;
    result.colorArg2 = vertices[vertexID].colorArg2;
    result.alphaOp = vertices[vertexID].alphaOp;
    result.alphaArg1 = vertices[vertexID].alphaArg1;
    result.alphaArg2 = vertices[vertexID].alphaArg2;
    result.textureFactor = vertices[vertexID].textureFactor;
    return result;
}

float4 dk2_unpack_color(uint value) {
    return float4(float((value >> 16) & 0xFF), float((value >> 8) & 0xFF),
                  float(value & 0xFF), float((value >> 24) & 0xFF)) / 255.0;
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

fragment float4 dk2_fragment(DK2RasterVertex input [[stage_in]],
                             array<texture2d<float>, 128> textures [[texture(0)]],
                             sampler textureSampler [[sampler(0)]]) {
    const float4 textureColor = textures[input.textureIndex].sample(textureSampler, input.texCoord);
    const float4 factor = dk2_unpack_color(input.textureFactor);
    const float4 current = input.color;
    const float4 colorArg1 = dk2_texture_arg(input.colorArg1, textureColor, current, input.color, factor);
    const float4 colorArg2 = dk2_texture_arg(input.colorArg2, textureColor, current, input.color, factor);
    const float4 alphaArg1 = dk2_texture_arg(input.alphaArg1, textureColor, current, input.color, factor);
    const float4 alphaArg2 = dk2_texture_arg(input.alphaArg2, textureColor, current, input.color, factor);
    return float4(
        dk2_color_op(input.colorOp, colorArg1, colorArg2,
                     input.color, textureColor, factor, current),
        dk2_alpha_op(input.alphaOp, alphaArg1, alphaArg2,
                     input.color, textureColor, factor, current));
}
