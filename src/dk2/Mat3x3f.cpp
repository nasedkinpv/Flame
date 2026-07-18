#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/Vec3f.h"

#include <emmintrin.h>


namespace {

__m128 combineRows(const float (&m)[3][3], float x, float y, float z) {
    const __m128 xyzMask = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
    const __m128 row0 = _mm_and_ps(_mm_loadu_ps(&m[0][0]), xyzMask);
    const __m128 row1 = _mm_and_ps(_mm_loadu_ps(&m[1][0]), xyzMask);
    const __m128 row2 = _mm_set_ps(0.0f, m[2][2], m[2][1], m[2][0]);
    return _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(row0, _mm_set1_ps(x)),
                       _mm_mul_ps(row1, _mm_set1_ps(y))),
            _mm_mul_ps(row2, _mm_set1_ps(z)));
}

__m128 combineColumns(const float (&m)[3][3], float x, float y, float z) {
    const __m128 column0 = _mm_set_ps(0.0f, m[2][0], m[1][0], m[0][0]);
    const __m128 column1 = _mm_set_ps(0.0f, m[2][1], m[1][1], m[0][1]);
    const __m128 column2 = _mm_set_ps(0.0f, m[2][2], m[1][2], m[0][2]);
    return _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(column0, _mm_set1_ps(x)),
                       _mm_mul_ps(column1, _mm_set1_ps(y))),
            _mm_mul_ps(column2, _mm_set1_ps(z)));
}

void storeVec3(dk2::Vec3f *output, __m128 value) {
    _mm_storel_pi(reinterpret_cast<__m64 *>(output), value);
    _mm_store_ss(&output->z,
                 _mm_shuffle_ps(value, value, _MM_SHUFFLE(2, 2, 2, 2)));
}

}


dk2::Vec3f *dk2::Mat3x3f::multiplyVec(Vec3f *output, Vec3f *input) {
    const float x = input->x;
    const float y = input->y;
    const float z = input->z;
    storeVec3(output, combineRows(m, x, y, z));
    return output;
}


dk2::Vec3f *dk2::Mat3x3f::sub_594E10(Vec3f *input, Vec3f *output) {
    const float x = input->x;
    const float y = input->y;
    const float z = input->z;
    storeVec3(output, combineRows(m, x, y, z));
    return output;
}


dk2::Vec3f *dk2::Mat3x3f::fun_594E70(Vec3f *input, Vec3f *output) {
    const float x = input->x;
    const float y = input->y;
    const float z = input->z;
    storeVec3(output, combineColumns(m, x, y, z));
    return output;
}
