#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <emmintrin.h>


namespace {

// world transform: tmp = g_mat_77F3A8 * v + g_vec_77F4C0
dk2::Vec3f worldTransform(dk2::Vec3f *v) {
    dk2::Vec3f tmp;
    dk2::g_mat_77F3A8.sub_594E10(v, &tmp);
    const __m128 sum = _mm_add_ps(
            _mm_set_ps(0.0f, tmp.z, tmp.y, tmp.x),
            _mm_set_ps(0.0f, dk2::g_vec_77F4C0.z, dk2::g_vec_77F4C0.y, dk2::g_vec_77F4C0.x));
    _mm_storel_pi(reinterpret_cast<__m64 *>(&tmp), sum);
    _mm_store_ss(&tmp.z, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 2, 2, 2)));
    return tmp;
}

}


int dk2::sub_58ACB0(int a1, Vec3f *v) {
    Vec3f tmp = worldTransform(v);
    return sub_58AF70(a1, &tmp.x);
}


int dk2::sub_58AD10(int a1, Vec3f *v) {
    Vec3f tmp = worldTransform(v);
    return RenderData_addToArr(a1, &tmp);
}


int dk2::sub_58AD70(int a1, float *v) {
    // original scales by two unnamed floats right after g_mat_77F498 and before g_zAdd3_7793A0
    const float sx = (&g_vec_77F4C0.x)[-1];                          // 0077F4BC
    const float sy = (&g_zAdd3_7793A0)[-1];  // 0077939C
    Vec3f tmp{v[0] * sx, v[1] * sy, v[2]};
    return sub_58AF70(a1, &tmp.x);
}
