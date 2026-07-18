#include "dk2/utils/Mat3x3f.h"

#include <emmintrin.h>


dk2::Vec3f *dk2::Mat3x3f::sub_594E10(Vec3f *input, Vec3f *output) {
    const float x = input->x;
    const float y = input->y;
    const float z = input->z;

    const __m128 xyzMask = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
    const __m128 row0 = _mm_and_ps(_mm_loadu_ps(&m[0][0]), xyzMask);
    const __m128 row1 = _mm_and_ps(_mm_loadu_ps(&m[1][0]), xyzMask);
    const __m128 row2 = _mm_set_ps(0.0f, m[2][2], m[2][1], m[2][0]);
    const __m128 result = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(row0, _mm_set1_ps(x)),
                       _mm_mul_ps(row1, _mm_set1_ps(y))),
            _mm_mul_ps(row2, _mm_set1_ps(z)));

    _mm_storel_pi(reinterpret_cast<__m64 *>(output), result);
    _mm_store_ss(&output->z,
                 _mm_shuffle_ps(result, result, _MM_SHUFFLE(2, 2, 2, 2)));
    return output;
}
