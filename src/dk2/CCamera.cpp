//
// Created by DiaLight on 10.10.2024.
//
#include "dk2/CCamera.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"
#include "patches/micro_patches.h"

#include "dk2_core/dk2_cull.h"

#include <cstdint>
#include <cstring>


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


// Thin wrappers over the portable, dependency-free cores in
// src/shared/dk2_core/sub_575D70.cpp. The math (and its exact x87/SSE2 sign-bit
// and per-plane addition-order semantics) now lives once, difftested on both
// x86_64 and arm64, so the native Metal host can recompute a BIT-IDENTICAL cull
// (native scene mirror, Phase 2). These wrappers only marshal the guest's
// dk2::Vec3f + camera globals into the core's plain-POD inputs; behaviour is
// byte-identical to the previous SSE2 implementation (proven by the difftest).
int __cdecl dk2::Vec3f_static_sub_575D70(
        Vec3f *point, float radius, uint32_t *fullyInside) {
    // Four camera-space frustum-side plane normals, read directly from the
    // guest globals each call (equivalent to the old per-frame cache: the
    // planes are constant within a draw-scene). A=760B70, B=760B38, C=760B18,
    // D=760B28 -- the exact order and dot-add split the core replicates.
    const core::CullVec3 p{point->x, point->y, point->z};
    const core::CullVec3 A{g_vec_760B70.x, g_vec_760B70.y, g_vec_760B70.z};
    const core::CullVec3 B{g_vec_760B38.x, g_vec_760B38.y, g_vec_760B38.z};
    const core::CullVec3 C{g_vec_760B18.x, g_vec_760B18.y, g_vec_760B18.z};
    const core::CullVec3 D{g_vec_760B28.x, g_vec_760B28.y, g_vec_760B28.z};
    return core::cullSphere575D70(p, radius, fullyInside, A, B, C, D);
}


dk2::Vec3f *__cdecl dk2::Vec3f_static_sub_575F10(
        Vec3f *point, float radius, Vec3f *projected, float *scaleOut) {
    const core::CullVec3 p{point->x, point->y, point->z};
    const core::ProjectResult r = core::projectSphere575F10(
            p, radius, g_camState.trg.x, g_camState.trg.y, g_camState.ww240);
    projected->x = r.x;
    projected->y = r.y;
    projected->z = r.z;
    // r.scale is a plain float; assignment is bit-preserving, including the
    // 0x7149F2CA near-clip sentinel the core carries in that field.
    *scaleOut = r.scale;
    return projected;
}
