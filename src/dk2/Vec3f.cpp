#include "dk2/utils/Vec3f.h"

#include <emmintrin.h>

#if defined(_M_IX86)
#define DK2_THISCALL __thiscall
#else
#define DK2_THISCALL
#endif


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


namespace dk2 {

float *DK2_THISCALL sub_59E6E0(float *self, float *right) {
    auto *leftValue = reinterpret_cast<Vec3f *>(self);
    const auto *rightValue = reinterpret_cast<const Vec3f *>(right);
    storeVec3(leftValue, _mm_add_ps(loadVec3(leftValue), loadVec3(rightValue)));
    return self;
}

}

#undef DK2_THISCALL
