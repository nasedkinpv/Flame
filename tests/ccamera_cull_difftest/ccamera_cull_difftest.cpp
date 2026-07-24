// Difftest for dk2::Vec3f_static_sub_575D70 / Vec3f_static_sub_575F10
// (DKII 0x00575D70 / 0x00575F10), the per-static-object frustum-sphere cull
// test used by CEngineStaticMeshAdd.cpp/CEngineAnimMeshAdd.cpp. Investigated
// 2026-07-23 as the prime suspect for a "black void that grows with camera
// rotation" regression report - reference model below was derived from a
// full manual trace of the x87 disassembly (every FPU stack slot tracked
// instruction-by-instruction), not guessed. This difftest exists to prove
// (or disprove) that trace exhaustively rather than trust it by eye.
//
// Reference algorithm for 575D70 (point, radius, &fullyInside):
//   dot_i = point . plane_i, for planes A=760B70, B=760B38, C=760B18, D=760B28
//     (dot_A, dot_B computed in (X*n.x + Z*n.z) + Y*n.y order;
//      dot_C, dot_D computed in (X*n.x + Y*n.y) + Z*n.z order -
//      matches the original's specific x87 addition order per plane, and the
//      current SSE2 impl's "upperLanes" split, which encode exactly this)
//   if any (radius + dot_i) < 0: return 0                      // culled
//   *fullyInside = 0
//   if (point.z - radius) < 0: return 1                         // near clip
//   if any (dot_i - radius) < 0: return 1
//   *fullyInside = 1; return 1
//
// Reference for 575F10 (point, radius, &projected, &scaleOut):
//   if (point.z - radius) < 0: projected = (camState.trg.x, camState.trg.y, 0),
//     *scaleOut = bit-pattern 0x7149F2CA (sentinel), return projected
//   scale = camState.ww240 / point.z
//   *scaleOut = scale * radius
//   projected = (point.x*scale + trg.x, point.y*scale + trg.y, point.z)
//
// Build & run (BOTH arches must pass bit-exactly -- the arm64 run is the
// cross-arch-determinism + no-SIMD gate for the dk2_core relocation):
//   for A in x86_64 arm64; do \
//     clang++ -arch $A -O2 -std=c++17 -ffp-contract=off \
//       -I tests/ccamera_cull_difftest -I src/shared \
//       -o /tmp/ccamera_cull_difftest_$A \
//       tests/ccamera_cull_difftest/ccamera_cull_difftest.cpp && \
//     /tmp/ccamera_cull_difftest_$A; done

#include "dk2_globals.h"

namespace dk2 {
Vec3f g_vec_760B70{};
Vec3f g_vec_760B38{};
Vec3f g_vec_760B18{};
Vec3f g_vec_760B28{};
uint32_t g_drawSceneCount_76520C = 0;
CamState g_camState{};
}  // namespace dk2

// The math now lives in the portable dk2_core relocation; CCamera.cpp's
// Vec3f_static_sub_575D70/_575F10 are thin wrappers over it. Include both so
// this difftest exercises the guest wrapper AND the shared core, and (crucially)
// compiles on BOTH -arch x86_64 and -arch arm64 -- the core is pure scalar C++
// with no <emmintrin.h>/__m128, which is what unblocked the arm64 half.
#include "../../src/shared/dk2_core/sub_575D70.cpp"
#include "../../src/dk2/CCamera.cpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>

namespace {

uint32_t bitsOf(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

// nan-aware bit-exact compare (matches project convention: bit patterns must
// match exactly, including which NaN payload, since the original carries
// raw FPU garbage through untouched paths).
bool bitEq(float a, float b) { return bitsOf(a) == bitsOf(b); }

float mulf(float a, float b) { return a * b; }
float addf(float a, float b) { return a + b; }
float subf(float a, float b) { return a - b; }

// The original tests raw sign bits (x87 `test reg,0x80000000`; the SSE2
// impl's `_mm_movemask_ps`), not ordered comparisons - matters for -0.0,
// whose sign bit is set even though `-0.0f < 0.0f` is false. Caught by this
// difftest (a mismatch was otherwise indistinguishable from a real bug).
bool negSign(float x) { return (bitsOf(x) & 0x80000000u) != 0; }

// Reference dot products, replicating the EXACT per-plane addition order
// traced from the original x87 disassembly (see file header).
float refDotXZY(const dk2::Vec3f &p, const dk2::Vec3f &n) {
    return addf(addf(mulf(p.x, n.x), mulf(n.z, p.z)), mulf(n.y, p.y));
}
float refDotXYZ(const dk2::Vec3f &p, const dk2::Vec3f &n) {
    return addf(addf(mulf(n.x, p.x), mulf(n.y, p.y)), mulf(n.z, p.z));
}

int refCull(const dk2::Vec3f &point, float radius, uint32_t *fullyInside,
            const dk2::Vec3f &A, const dk2::Vec3f &B,
            const dk2::Vec3f &C, const dk2::Vec3f &D) {
    const float dotA = refDotXZY(point, A);
    const float dotB = refDotXZY(point, B);
    const float dotC = refDotXYZ(point, C);
    const float dotD = refDotXYZ(point, D);

    // The original sets *fullyInside=0 unconditionally right after computing
    // the dots (before any of the 4 cull checks below), not after them -
    // verified via disasm: `mov dword ptr [edx],0` at 0x575e42 executes
    // before the first je/fallthrough decision. Caught by this difftest on
    // its first mismatching case.
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

void refProject(const dk2::Vec3f &point, float radius,
                dk2::Vec3f *projected, float *scaleOut) {
    if (negSign(subf(point.z, radius))) {
        projected->x = dk2::g_camState.trg.x;
        projected->y = dk2::g_camState.trg.y;
        projected->z = 0.0f;
        const uint32_t sentinel = 0x7149F2CA;
        std::memcpy(scaleOut, &sentinel, sizeof(sentinel));
        return;
    }
    const float scale = dk2::g_camState.ww240 / point.z;
    *scaleOut = mulf(scale, radius);
    projected->x = addf(mulf(point.x, scale), dk2::g_camState.trg.x);
    projected->y = addf(mulf(point.y, scale), dk2::g_camState.trg.y);
    projected->z = point.z;
}

int g_checked = 0;

void checkCull(const dk2::Vec3f &point, float radius,
               const dk2::Vec3f &A, const dk2::Vec3f &B,
               const dk2::Vec3f &C, const dk2::Vec3f &D) {
    dk2::g_vec_760B70 = A;
    dk2::g_vec_760B38 = B;
    dk2::g_vec_760B18 = C;
    dk2::g_vec_760B28 = D;
    dk2::g_drawSceneCount_76520C++;  // force cullingPlanes() to recompute

    dk2::Vec3f p = point;
    uint32_t implFullyInside = 0xDEADBEEF;
    const int implResult = dk2::Vec3f_static_sub_575D70(&p, radius, &implFullyInside);

    uint32_t refFullyInside = 0xDEADBEEF;
    const int refResult = refCull(point, radius, &refFullyInside, A, B, C, D);

    if (implResult != refResult || implFullyInside != refFullyInside) {
        std::printf("MISMATCH cull: point=(%g,%g,%g) radius=%g "
                    "A=(%g,%g,%g) B=(%g,%g,%g) C=(%g,%g,%g) D=(%g,%g,%g)\n"
                    "  impl: result=%d fullyInside=%u\n"
                    "  ref:  result=%d fullyInside=%u\n",
                    point.x, point.y, point.z, radius,
                    A.x, A.y, A.z, B.x, B.y, B.z, C.x, C.y, C.z, D.x, D.y, D.z,
                    implResult, implFullyInside, refResult, refFullyInside);
        std::exit(1);
    }
    ++g_checked;
}

void checkProject(const dk2::Vec3f &point, float radius,
                  float trgX, float trgY, float ww240) {
    dk2::g_camState.trg.x = trgX;
    dk2::g_camState.trg.y = trgY;
    dk2::g_camState.ww240 = ww240;

    dk2::Vec3f p = point;
    dk2::Vec3f implProjected{};
    float implScale = 0.0f;
    dk2::Vec3f *ret = dk2::Vec3f_static_sub_575F10(&p, radius, &implProjected, &implScale);

    dk2::Vec3f refProjected{};
    float refScale = 0.0f;
    refProject(point, radius, &refProjected, &refScale);

    if (ret != &implProjected ||
        !bitEq(implProjected.x, refProjected.x) ||
        !bitEq(implProjected.y, refProjected.y) ||
        !bitEq(implProjected.z, refProjected.z) ||
        !bitEq(implScale, refScale)) {
        std::printf("MISMATCH project: point=(%g,%g,%g) radius=%g trg=(%g,%g) ww240=%g\n"
                    "  impl: proj=(%g,%g,%g) scale=%g\n"
                    "  ref:  proj=(%g,%g,%g) scale=%g\n",
                    point.x, point.y, point.z, radius, trgX, trgY, ww240,
                    implProjected.x, implProjected.y, implProjected.z, implScale,
                    refProjected.x, refProjected.y, refProjected.z, refScale);
        std::exit(1);
    }
    ++g_checked;
}

}  // namespace

int main() {
    // Fixed planes resembling a real view frustum (roughly unit normals,
    // negative-ish offsets) plus randomized planes to stress arbitrary
    // sign/magnitude combinations - the original code makes no assumption
    // planes are normalized.
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> coordDist(-2000.0f, 2000.0f);
    std::uniform_real_distribution<float> smallDist(-4.0f, 4.0f);
    std::uniform_real_distribution<float> radiusDist(0.0f, 300.0f);

    auto randVec = [&](std::uniform_real_distribution<float> &d) {
        return dk2::Vec3f{d(rng), d(rng), d(rng)};
    };

    // Exhaustive-ish grid over sign/magnitude combinations for the 4 dot
    // products via small integer plane/point components (catches off-by-sign
    // and lane-order bugs deterministically), plus a large randomized sweep
    // for realistic float coordinates.
    const float small[] = {-3.f, -1.f, -0.001f, 0.f, 0.001f, 1.f, 3.f};
    for (float ax : small) for (float ay : small) for (float az : small) {
        dk2::Vec3f A{ax, ay, az};
        dk2::Vec3f B{az, ax, ay};      // permuted, still covers sign combos
        dk2::Vec3f C{ay, az, ax};
        dk2::Vec3f D{-ax, -ay, -az};
        for (float px : small) {
            dk2::Vec3f point{px, -px, px * 0.5f};
            checkCull(point, 1.5f, A, B, C, D);
            checkCull(point, 0.0f, A, B, C, D);
        }
    }
    std::printf("grid: OK %d combinations\n", g_checked);

    const int gridChecked = g_checked;
    for (int i = 0; i < 2'000'000; ++i) {
        const dk2::Vec3f point = randVec(coordDist);
        const dk2::Vec3f A = randVec(smallDist);
        const dk2::Vec3f B = randVec(smallDist);
        const dk2::Vec3f C = randVec(smallDist);
        const dk2::Vec3f D = randVec(smallDist);
        const float radius = radiusDist(rng);
        checkCull(point, radius, A, B, C, D);
    }
    std::printf("random cull: OK %d combinations\n", g_checked - gridChecked);

    const int cullChecked = g_checked;
    std::uniform_real_distribution<float> depthDist(-50.0f, 2000.0f);
    std::uniform_real_distribution<float> ww240Dist(1.0f, 4000.0f);
    for (int i = 0; i < 500000; ++i) {
        dk2::Vec3f point = randVec(coordDist);
        point.z = depthDist(rng);
        const float radius = radiusDist(rng);
        const float trgX = coordDist(rng);
        const float trgY = coordDist(rng);
        const float ww240 = ww240Dist(rng);
        checkProject(point, radius, trgX, trgY, ww240);
    }
    // z exactly at the radius boundary and z==0 (division edge)
    for (float z : {0.0f, 1.0f, -1.0f}) {
        dk2::Vec3f point{10.0f, -20.0f, z};
        checkProject(point, 1.0f, 5.0f, -5.0f, 100.0f);
    }
    std::printf("project: OK %d combinations\n", g_checked - cullChecked);

    std::printf("OK: %d total combinations\n", g_checked);
    return 0;
}
