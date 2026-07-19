#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"

#include <emmintrin.h>

namespace {

__m128 loadVec3(const dk2::Vec3f *value) {
    return _mm_set_ps(0.0f, value->z, value->y, value->x);
}

void storeVec3(dk2::Vec3f *output, __m128 value) {
    _mm_storel_pi(reinterpret_cast<__m64 *>(output), value);
    _mm_store_ss(&output->z,
                 _mm_shuffle_ps(value, value, _MM_SHUFFLE(2, 2, 2, 2)));
}

}


dk2::Vec3f *dk2::Vec3f::mulV(Vec3f *output, float scalar) {
    storeVec3(output, _mm_mul_ps(loadVec3(this), _mm_set1_ps(scalar)));
    return output;
}


dk2::Vec3f *dk2::Vec3f::substractAssign(Vec3f *output, Vec3f *right) {
    const __m128 leftValue = loadVec3(this);
    const __m128 rightValue = loadVec3(right);
    storeVec3(output, _mm_sub_ps(leftValue, rightValue));
    return output;
}


dk2::Vec3f *dk2::Vec3f::sumVec3f(Vec3f *output, Vec3f *right) {
    const __m128 leftValue = loadVec3(this);
    const __m128 rightValue = loadVec3(right);
    storeVec3(output, _mm_add_ps(leftValue, rightValue));
    return output;
}


float *dk2::Vec3f::sub_59E6E0(float *right) {
    const auto *rightValue = reinterpret_cast<const Vec3f *>(right);
    storeVec3(this, _mm_add_ps(loadVec3(this), loadVec3(rightValue)));
    // original returns its argument (eax is never reloaded), not `this`;
    // no caller reads it, but keep the ABI byte-exact
    return right;
}


// 0041C500: output = this / |this| (dot summed as (y*y + z*z) + x*x, then
// 1.0f / sqrt, one rounding per operation like x87 with 24-bit precision)
float *dk2::Vec3f::sub_41C500(float *output) {
    const float vx = x, vy = y, vz = z;
    const float dot = (vy * vy + vz * vz) + vx * vx;
    float len;
    _mm_store_ss(&len, _mm_sqrt_ss(_mm_set_ss(dot)));
    const float inv = 1.0f / len;
    output[0] = vx * inv;
    output[1] = vy * inv;
    output[2] = vz * inv;
    return output;
}


// 0059E700: conditionally add (+-a, b) to v->y / v->z depending on which of
// the two flags is set; no-op when both or neither is set
float *__cdecl dk2::sub_59E700(float *v, float a, float b, int flagNeg, int flagPos) {
    if (flagNeg != flagPos) {  // the original compares values (xor), not truthiness
        v[1] += flagNeg ? -a : a;
        v[2] += b;
    }
    return v;
}


// 0059EC90: same for v->x / v->z
int __cdecl dk2::sub_59EC90(float *v, float a, float b, int flagNeg, int flagPos) {
    if (flagNeg != flagPos) {  // the original compares values (xor), not truthiness
        v[0] += flagNeg ? -a : a;
        v[2] += b;
    }
    return 0;  // eax is garbage in the original; no caller reads it
}
