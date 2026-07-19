#pragma once

#include <cstdint>
#include <cstring>
#include <xmmintrin.h>


namespace dk2::lighting {

#pragma pack(push, 1)
struct DirectionalLight {
    float position[3];
    float unusedC;
    float unused10;
    uint32_t intensityBits;
    float facingScale;
    float color[3];
};

struct DirectionalLightBuffer {
    int32_t count;
    DirectionalLight items[24];
};
#pragma pack(pop)

static_assert(sizeof(DirectionalLight) == 0x28);
static_assert(sizeof(DirectionalLightBuffer) == 0x3C4);

inline uint32_t floatBits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float floatFromBits(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Exact SSE equivalent of DKII.EXE 0057C190. The original x87 control word
// uses 24-bit precision, so every arithmetic operation rounds like float.
inline void accumulateDirectional(
        const DirectionalLightBuffer &lights,
        float *accumulator,
        const float *position,
        const float *normal) {
    if (lights.count <= 0) return;

    const __m128 p = _mm_set_ps(0.0f, position[2], position[1], position[0]);
    const __m128 n = _mm_set_ps(0.0f, normal[2], normal[1], normal[0]);
    for (int32_t i = 0; i < lights.count; ++i) {
        const DirectionalLight &light = lights.items[i];
        const __m128 lightPosition = _mm_set_ps(
                0.0f, light.position[2], light.position[1], light.position[0]);
        const __m128 products = _mm_mul_ps(_mm_sub_ps(lightPosition, p), n);

        // The x87 stack adds X+Z first, then Y. Keep that order so the packed
        // diffuse colour remains bit-identical at triangle boundaries.
        const __m128 xPlusZ = _mm_add_ss(
                products,
                _mm_shuffle_ps(products, products, _MM_SHUFFLE(2, 2, 2, 2)));
        const __m128 dot = _mm_add_ss(
                xPlusZ,
                _mm_shuffle_ps(products, products, _MM_SHUFFLE(1, 1, 1, 1)));
        const float facing = _mm_cvtss_f32(
                _mm_mul_ss(dot, _mm_set_ss(light.facingScale)));
        if (floatBits(facing) >> 31) continue;

        const float intensity = floatFromBits(light.intensityBits);
        const float factor = _mm_cvtss_f32(
                _mm_mul_ss(_mm_set_ss(facing), _mm_set_ss(intensity)));
        const float oneMinusFactor = _mm_cvtss_f32(
                _mm_sub_ss(_mm_set_ss(1.0f), _mm_set_ss(factor)));
        const float appliedFactor = (floatBits(oneMinusFactor) >> 31) ? 1.0f : factor;

        const __m128 colour = _mm_mul_ps(
                _mm_set_ps(0.0f, light.color[2], light.color[1], light.color[0]),
                _mm_set1_ps(appliedFactor));
        const __m128 accumulated = _mm_add_ps(
                _mm_set_ps(0.0f, accumulator[2], accumulator[1], accumulator[0]),
                colour);
        _mm_storel_pi(reinterpret_cast<__m64 *>(accumulator), accumulated);
        _mm_store_ss(
                accumulator + 2,
                _mm_shuffle_ps(accumulated, accumulated, _MM_SHUFFLE(2, 2, 2, 2)));
    }
}

}  // namespace dk2::lighting
