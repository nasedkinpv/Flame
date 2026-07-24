#ifndef DK2_CORE_DK2_CULL_H
#define DK2_CORE_DK2_CULL_H

// Portable scalar cores of the DKII per-static-object frustum-sphere cull
// (0x00575D70) and reduction-factor / projection (0x00575F10) functions.
//
// These are the SHARED, dependency-free relocation of the math that used to
// live only as a guest-side SSE2 translation in src/dk2/CCamera.cpp. Both the
// guest DLL (x86 MSVC) and the native ARM64 Metal host compile the SAME source
// (src/shared/dk2_core/sub_575D70.cpp), so the host can recompute a cull
// decision that is BIT-IDENTICAL to the guest's (native scene mirror, Phase 2).
//
// Determinism rules (see src/shared/dk2_core/README.md): no OS/Windows/x86
// types, no <emmintrin.h>/__m128/SSE, plain scalar C++ only, and both builds
// use -ffp-contract=off so mul/add never fuse. The dual-arch difftest
// (tests/ccamera_cull_difftest/, x86_64 AND arm64) is the iron proof of parity.

#include <cstdint>

namespace dk2 {
namespace core {

// Plain POD float3 -- deliberately NOT dk2::Vec3f (that header pulls in
// <ddraw.h>/<d3d.h> via the auto-generated struct tree and is unavailable on
// the host). Callers copy their own vector's x/y/z into this at the boundary.
struct CullVec3 {
    float x;
    float y;
    float z;
};

// Portable scalar core of DKII 0x00575D70 (frustum-sphere cull test).
//
// `point` is the object's bounding-sphere centre ALREADY transformed into
// camera/view space (the caller does world->view via g_camState). `radius` is
// the sphere radius. planeA..planeD are the four camera-space frustum-side
// plane normals (guest globals g_vec_760B70 / _760B38 / _760B18 / _760B28, in
// that order -- A/B are dotted in (x*nx + z*nz) + y*ny order, C/D in
// (x*nx + y*ny) + z*nz order, matching the original x87 per-plane add order).
//
// Returns 0 = culled (outside the frustum), 1 = visible. *fullyInside is set to
// 1 iff the whole sphere is inside every plane and past the near clip, else 0.
// The sign tests use raw IEEE-754 sign bits (not ordered <), matching the
// original's `test reg,0x80000000` -- so -0.0 counts as negative.
int cullSphere575D70(const CullVec3& point, float radius, uint32_t* fullyInside,
                     const CullVec3& planeA, const CullVec3& planeB,
                     const CullVec3& planeC, const CullVec3& planeD);

// Portable scalar core of DKII 0x00575F10 (reduction-factor / projection).
//
// If the sphere crosses the near clip (point.z - radius < 0), returns a
// sentinel: projected = (trgX, trgY, 0) and scale carries the exact bit pattern
// 0x7149F2CA (the original's untouched-FPU-garbage marker). Otherwise
// scale = (ww240 / point.z) * radius and projected = (point.x*s + trgX,
// point.y*s + trgY, point.z), where s = ww240 / point.z.
struct ProjectResult {
    float x;
    float y;
    float z;
    float scale;
};
ProjectResult projectSphere575F10(const CullVec3& point, float radius,
                                  float trgX, float trgY, float ww240);

}  // namespace core
}  // namespace dk2

#endif  // DK2_CORE_DK2_CULL_H
