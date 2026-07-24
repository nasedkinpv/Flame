// DKII 0x00575D70 / 0x00575F10 -- per-static-object frustum-sphere cull test
// and reduction-factor / projection, as PORTABLE SCALAR cores shared by the
// guest DLL (x86 MSVC) and the native ARM64 Metal host (native scene mirror,
// Phase 2). See dk2_cull.h for the contract and src/shared/dk2_core/README.md
// for the determinism rules (no SIMD, no globals, -ffp-contract=off).
//
// This is a faithful RELOCATION, not a re-translation: the algorithm below is
// the exact golden scalar reference model that tests/ccamera_cull_difftest/
// difftested (bit-for-bit, millions of cases) against the original guest SSE2
// translation that used to live in src/dk2/CCamera.cpp. The guest now calls
// these cores through thin wrappers in CCamera.cpp, so its output is unchanged;
// the host calls them directly with its own camera-reconstructed inputs.
//
// The original tested raw sign bits (x87 `test reg,0x80000000`), NOT ordered
// comparisons -- this matters for -0.0 (sign bit set although -0.0f < 0.0f is
// false) and for signed NaNs carried through untouched paths. The helpers
// below reproduce that exactly. Every + - * is a plain scalar op; with
// -ffp-contract=off (both builds) SSE2 (guest) and ARM64 scalar (host) yield
// identical IEEE-754 results.

#include "dk2_core/dk2_cull.h"

#include <cstdint>
#include <cstring>

namespace dk2 {
namespace core {

namespace {

uint32_t bitsOf(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

float fromBits(uint32_t b) {
    float f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

// Raw IEEE-754 sign-bit test (matches the original's `test reg,0x80000000`).
bool negSign(float x) { return (bitsOf(x) & 0x80000000u) != 0; }

float mulf(float a, float b) { return a * b; }
float addf(float a, float b) { return a + b; }
float subf(float a, float b) { return a - b; }

// Per-plane dot products replicating the EXACT x87 addition order traced from
// the original disassembly: A/B use (x*nx + z*nz) + y*ny; C/D use
// (x*nx + y*ny) + z*nz. Verified by the difftest against the SSE2 impl's
// "upperLanes" split, which encodes exactly this.
float dotXZY(const CullVec3& p, const CullVec3& n) {
    return addf(addf(mulf(p.x, n.x), mulf(n.z, p.z)), mulf(n.y, p.y));
}
float dotXYZ(const CullVec3& p, const CullVec3& n) {
    return addf(addf(mulf(n.x, p.x), mulf(n.y, p.y)), mulf(n.z, p.z));
}

}  // namespace

int cullSphere575D70(const CullVec3& point, float radius, uint32_t* fullyInside,
                     const CullVec3& planeA, const CullVec3& planeB,
                     const CullVec3& planeC, const CullVec3& planeD) {
    const float dotA = dotXZY(point, planeA);
    const float dotB = dotXZY(point, planeB);
    const float dotC = dotXYZ(point, planeC);
    const float dotD = dotXYZ(point, planeD);

    // The original sets *fullyInside=0 unconditionally right after the dots,
    // BEFORE any of the 4 cull checks (disasm: `mov dword ptr [edx],0` at
    // 0x575e42 executes before the first branch).
    *fullyInside = 0;
    if (negSign(addf(radius, dotA))) return 0;
    if (negSign(addf(radius, dotB))) return 0;
    if (negSign(addf(radius, dotC))) return 0;
    if (negSign(addf(radius, dotD))) return 0;

    if (negSign(subf(point.z, radius))) return 1;
    if (negSign(subf(dotA, radius))) return 1;
    if (negSign(subf(dotB, radius))) return 1;
    if (negSign(subf(dotC, radius))) return 1;
    if (negSign(subf(dotD, radius))) return 1;

    *fullyInside = 1;
    return 1;
}

ProjectResult projectSphere575F10(const CullVec3& point, float radius,
                                  float trgX, float trgY, float ww240) {
    ProjectResult out;
    if (negSign(subf(point.z, radius))) {
        out.x = trgX;
        out.y = trgY;
        out.z = 0.0f;
        // The original leaves *scaleOut holding raw FPU garbage on this path;
        // the difftest pinned that to this exact bit pattern.
        out.scale = fromBits(0x7149F2CAu);
        return out;
    }
    const float scale = ww240 / point.z;
    out.scale = mulf(scale, radius);
    out.x = addf(mulf(point.x, scale), trgX);
    out.y = addf(mulf(point.y, scale), trgY);
    out.z = point.z;
    return out;
}

}  // namespace core
}  // namespace dk2
