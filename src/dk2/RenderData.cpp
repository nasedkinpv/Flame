#include "dk2_functions.h"
#include "dk2_globals.h"
#include "dk2/RenderData.h"
#include "dk2/utils/Vec3f.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>


namespace {

template <typename T>
T &at(uintptr_t address) {
    return *reinterpret_cast<T *>(address);
}

bool hasSignBit(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x80000000u) != 0;
}

int encodeViewOffset(float coverage) {
    const float encoded = 12582912.0f - (0.499989986f - coverage * 65536.0f);
    uint32_t bits;
    std::memcpy(&bits, &encoded, sizeof(bits));
    return static_cast<int>((bits & 0x007FFFFFu) - 0x00400000u);
}

}


int __cdecl dk2::RenderData_addToArr(int index, Vec3f *input) {
    RenderData &output = RenderData_instance_arr[index];
    g_idxFlags[index] = 1;
    output.vec = *input;

    const float inverseZ = _mm_cvtss_f32(
            _mm_div_ss(_mm_set_ss(1.0f), _mm_set_ss(input->z)));
    const __m128 xy = _mm_set_ps(0.0f, 0.0f, input->y, input->x);
    const __m128 scale = _mm_set_ps(
            0.0f, 0.0f, at<float>(0x0078093C), at<float>(0x0077F4CC));
    const __m128 offset = _mm_set_ps(
            0.0f, 0.0f, at<float>(0x0077F930), at<float>(0x0077F4F0));
    const __m128 projected = _mm_add_ps(
            _mm_mul_ps(_mm_mul_ps(xy, _mm_set1_ps(inverseZ)), scale), offset);
    _mm_storel_pi(reinterpret_cast<__m64 *>(&output.xC), projected);

    if ((at<uint32_t>(0x0077F450) & 8u) != 0) {
        const float radial = input->z +
                             0.5f * (std::abs(input->x) + std::abs(input->y));
        const float coverage =
                (at<float>(0x0077F4E8) - radial) * at<float>(0x0077F4E0);
        if (hasSignBit(coverage)) {
            g_idxFlags[index] |= 0x10;
            output._viewOffsets = 0x20;
            output.f24 = 0;
        } else if (hasSignBit(coverage - 1.0f)) {
            g_idxFlags[index] |= 0x10;
            output.f24 = encodeViewOffset(coverage);
        }
    }

    output._viewOffsets = 0;
    const float depthDelta = 0.5f - input->z;
    if (hasSignBit(depthDelta)) {
        output.z14 = g_zAdd3_7793A0 - g_zMul3_77F934 * depthDelta;
    } else {
        output.z14 = g_zMul2_77F490 * input->z + g_zAdd2_77F4D0;
    }

    const float remainingDepth = 0.999938965f - output.z14;
    if (hasSignBit(remainingDepth)) {
        output.z14 = 0.999938965f;
    }
    output.f18 = remainingDepth;
    output._viewOffsets = 0;
    return index * static_cast<int>(sizeof(RenderData));
}
