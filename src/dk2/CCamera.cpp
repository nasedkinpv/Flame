//
// Created by DiaLight on 10.10.2024.
//
#include "dk2/CCamera.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"
#include "patches/micro_patches.h"

#include <emmintrin.h>
#include <cstring>


namespace {

__m128 cullingPlaneDots(const dk2::Vec3f &point) {
    // Lane order is 760B70, 760B38, 760B18, 760B28.  The two pairs keep the
    // original x87 addition order so boundary decisions remain unchanged.
    const __m128 planeX = _mm_set_ps(
            dk2::g_vec_760B28.x, dk2::g_vec_760B18.x,
            dk2::g_vec_760B38.x, dk2::g_vec_760B70.x);
    const __m128 planeY = _mm_set_ps(
            dk2::g_vec_760B28.y, dk2::g_vec_760B18.y,
            dk2::g_vec_760B38.y, dk2::g_vec_760B70.y);
    const __m128 planeZ = _mm_set_ps(
            dk2::g_vec_760B28.z, dk2::g_vec_760B18.z,
            dk2::g_vec_760B38.z, dk2::g_vec_760B70.z);
    const __m128 productsX = _mm_mul_ps(_mm_set1_ps(point.x), planeX);
    const __m128 productsY = _mm_mul_ps(_mm_set1_ps(point.y), planeY);
    const __m128 productsZ = _mm_mul_ps(_mm_set1_ps(point.z), planeZ);
    const __m128 upperLanes = _mm_castsi128_ps(
            _mm_set_epi32(-1, -1, 0, 0));
    const __m128 firstSum = _mm_or_ps(
            _mm_andnot_ps(upperLanes, _mm_add_ps(productsX, productsZ)),
            _mm_and_ps(upperLanes, _mm_add_ps(productsX, productsY)));
    const __m128 lastTerm = _mm_or_ps(
            _mm_andnot_ps(upperLanes, productsY),
            _mm_and_ps(upperLanes, productsZ));
    return _mm_add_ps(firstSum, lastTerm);
}

}  // namespace


void dk2::CCamera::zoomRel_449CA0(int delta) {
    if ((this->fD92 & 8) != 0 || this->endTime) return;
    int min = this->minZoomLevel;
    int max = this->maxZoomLevel - 1;
    if(patch::increase_zoom_level::enabled) {
        max += 50000;
        if(min > 2000) min -= 2000;
    }
    int newCur = delta + this->curZoomLevel;
    this->curZoomLevel = newCur;
    if (newCur <= min)
        newCur = min;
    if (newCur >= max) {
        newCur = max;
    } else if (newCur <= min) {
        newCur = min;
    }
    this->curZoomLevel = newCur;
}


int __cdecl dk2::Vec3f_static_sub_575D70(
        Vec3f *point, float radius, uint32_t *fullyInside) {
    const __m128 dots = cullingPlaneDots(*point);
    const __m128 radii = _mm_set1_ps(radius);
    const __m128 signBits = _mm_castsi128_ps(_mm_set1_epi32(0x80000000u));
    const __m128 negativeDots = _mm_xor_ps(dots, signBits);
    *fullyInside = 0;

    // The original tests raw sign bits instead of ordered comparisons.  Keep
    // that behavior for negative zero and signed NaNs as well as normal input.
    if (_mm_movemask_ps(_mm_sub_ps(radii, negativeDots)) != 0) return 0;
    const __m128 depthMinusRadius = _mm_sub_ss(
            _mm_set_ss(point->z), _mm_set_ss(radius));
    if (_mm_movemask_ps(depthMinusRadius) != 0) return 1;
    const __m128 negativeRadii = _mm_xor_ps(radii, signBits);
    if (_mm_movemask_ps(_mm_sub_ps(negativeRadii, negativeDots)) != 0) return 1;

    *fullyInside = 1;
    return 1;
}


dk2::Vec3f *__cdecl dk2::Vec3f_static_sub_575F10(
        Vec3f *point, float radius, Vec3f *projected, float *scaleOut) {
    const __m128 depthMinusRadius = _mm_sub_ss(
            _mm_set_ss(point->z), _mm_set_ss(radius));
    if (_mm_movemask_ps(depthMinusRadius) != 0) {
        projected->x = g_camState.trg.x;
        projected->y = g_camState.trg.y;
        projected->z = 0.0f;
        const uint32_t sentinel = 0x7149F2CA;
        std::memcpy(scaleOut, &sentinel, sizeof(sentinel));
        return projected;
    }

    const float scale = g_camState.ww240 / point->z;
    *scaleOut = scale * radius;
    const __m128 xy = _mm_add_ps(
            _mm_mul_ps(
                    _mm_set_ps(0.0f, 0.0f, point->y, point->x),
                    _mm_set1_ps(scale)),
            _mm_set_ps(0.0f, 0.0f, g_camState.trg.y, g_camState.trg.x));
    alignas(16) float values[4];
    _mm_store_ps(values, xy);
    projected->x = values[0];
    projected->y = values[1];
    projected->z = point->z;
    return projected;
}
