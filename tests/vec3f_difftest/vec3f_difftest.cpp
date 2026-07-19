// Offline differential test for src/dk2/Vec3f.cpp (SSE2 replacements of
// DKII.EXE 0041C480 mulV / 0041C4C0 substractAssign / 0044E7B0 sumVec3f).
//
// The originals are x87 leaf functions that perform one arithmetic op per
// component and store to m32, which rounds identically to SSE single-precision,
// so bit-exact equality is required — including denormals, inf, nan, and every
// aliasing combination (output == this, output == right, this == right).
//
// Build & run (works on Apple Silicon via Rosetta — real SSE2 is executed):
//   clang++ -arch x86_64 -O2 -std=c++17 -I tests/vec3f_difftest \
//       -o /tmp/vec3f_difftest tests/vec3f_difftest/vec3f_difftest.cpp
//   /tmp/vec3f_difftest
#include "../../src/dk2/Vec3f.cpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using dk2::Vec3f;

static bool bitEq(float a, float b) {
    unsigned ia, ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    return ia == ib || (std::isnan(a) && std::isnan(b));
}
static void refMul(const Vec3f *t, Vec3f *o, float s) { o->x = t->x * s; o->y = t->y * s; o->z = t->z * s; }
static void refSub(const Vec3f *t, Vec3f *o, const Vec3f *r) { o->x = t->x - r->x; o->y = t->y - r->y; o->z = t->z - r->z; }
static void refAdd(const Vec3f *t, Vec3f *o, const Vec3f *r) { o->x = t->x + r->x; o->y = t->y + r->y; o->z = t->z + r->z; }

int main() {
    const std::vector<float> vals = {
        0.f, -0.f, 1.f, -1.f, 0.5f, 3.14159265f, -123456.78f,
        1e-38f, -1e-38f, 1e-45f /*denormal*/, 3.4e38f, -3.4e38f,
        INFINITY, -INFINITY, NAN, 1e20f, -1e-20f};
    int n = 0;
    for (float a : vals) for (float b : vals) for (float c : vals) for (float s : vals) {
        const Vec3f t{a, b, c}, r{c, s, a};
        { Vec3f o{9,9,9}, e; refMul(&t, &e, s); Vec3f tt = t; Vec3f *ret = tt.mulV(&o, s);
          assert(ret == &o && bitEq(o.x,e.x) && bitEq(o.y,e.y) && bitEq(o.z,e.z)); }
        { Vec3f tt = t, e; refMul(&t, &e, s); tt.mulV(&tt, s);
          assert(bitEq(tt.x,e.x) && bitEq(tt.y,e.y) && bitEq(tt.z,e.z)); }
        { Vec3f o{9,9,9}, e; refSub(&t, &e, &r); Vec3f tt = t, rr = r; Vec3f *ret = tt.substractAssign(&o, &rr);
          assert(ret == &o && bitEq(o.x,e.x) && bitEq(o.y,e.y) && bitEq(o.z,e.z)); }
        { Vec3f tt = t, rr = r, e; refSub(&t, &e, &r); tt.substractAssign(&tt, &rr);
          assert(bitEq(tt.x,e.x) && bitEq(tt.y,e.y) && bitEq(tt.z,e.z)); }
        { Vec3f tt = t, rr = r, e; refSub(&t, &e, &r); tt.substractAssign(&rr, &rr);
          assert(bitEq(rr.x,e.x) && bitEq(rr.y,e.y) && bitEq(rr.z,e.z)); }
        { Vec3f o{9,9,9}, e; refAdd(&t, &e, &r); Vec3f tt = t, rr = r; Vec3f *ret = tt.sumVec3f(&o, &rr);
          assert(ret == &o && bitEq(o.x,e.x) && bitEq(o.y,e.y) && bitEq(o.z,e.z)); }
        { Vec3f tt = t, rr = r, e; refAdd(&t, &e, &r); tt.sumVec3f(&tt, &rr);
          assert(bitEq(tt.x,e.x) && bitEq(tt.y,e.y) && bitEq(tt.z,e.z)); }
        { Vec3f tt = t, e; refAdd(&t, &e, &t); tt.sumVec3f(&tt, &tt);
          assert(bitEq(tt.x,e.x) && bitEq(tt.y,e.y) && bitEq(tt.z,e.z)); }
        { Vec3f tt = t, rr = r, e; refAdd(&t, &e, &r); float *ret = tt.sub_59E6E0(&rr.x);
          // original returns eax = its argument, not `this` (see 0059E6E0 disasm)
          assert(ret == &rr.x && bitEq(tt.x,e.x) && bitEq(tt.y,e.y) && bitEq(tt.z,e.z)); }
        { Vec3f tt = t, e; refAdd(&t, &e, &t); tt.sub_59E6E0(&tt.x);
          assert(bitEq(tt.x,e.x) && bitEq(tt.y,e.y) && bitEq(tt.z,e.z)); }
        // 0041C500 normalize: dot = (y*y + z*z) + x*x, inv = 1/sqrt
        { Vec3f tt = t, o{9,9,9};
          float dot = (tt.y*tt.y + tt.z*tt.z) + tt.x*tt.x;
          float inv = 1.0f / sqrtf(dot);
          Vec3f e{tt.x*inv, tt.y*inv, tt.z*inv};
          float *ret = tt.sub_41C500(&o.x);
          assert(ret == &o.x && bitEq(o.x,e.x) && bitEq(o.y,e.y) && bitEq(o.z,e.z)); }
        { Vec3f tt = t;
          float dot = (tt.y*tt.y + tt.z*tt.z) + tt.x*tt.x;
          float inv = 1.0f / sqrtf(dot);
          Vec3f e{tt.x*inv, tt.y*inv, tt.z*inv};
          tt.sub_41C500(&tt.x);  // output == this
          assert(bitEq(tt.x,e.x) && bitEq(tt.y,e.y) && bitEq(tt.z,e.z)); }
        // 0059E700 / 0059EC90 axis adders, all four flag combos
        for (int fa = 0; fa < 2; ++fa) for (int fb = 0; fb < 2; ++fb) {
            Vec3f v1 = t, e1 = t;
            if (fa != fb) { e1.y += fa ? -a : a; e1.z += b; }
            float *ret = dk2::sub_59E700(&v1.x, a, b, fa, fb);
            assert(ret == &v1.x && bitEq(v1.x,e1.x) && bitEq(v1.y,e1.y) && bitEq(v1.z,e1.z));
            Vec3f v2 = t, e2 = t;
            if (fa != fb) { e2.x += fa ? -a : a; e2.z += b; }
            dk2::sub_59EC90(&v2.x, a, b, fa, fb);
            assert(bitEq(v2.x,e2.x) && bitEq(v2.y,e2.y) && bitEq(v2.z,e2.z));
        }
        n++;
    }
    // Vec3f is 12 bytes; a heap allocation of exactly 12 bytes catches a
    // 16-byte overread of the source struct under ASan (-fsanitize=address).
    { Vec3f *t = (Vec3f *) malloc(sizeof(Vec3f)); *t = {1, 2, 3}; Vec3f o;
      t->mulV(&o, 2.f); assert(o.x == 2 && o.y == 4 && o.z == 6); free(t); }
    printf("OK: %d combinations, all bit-exact incl. aliasing\n", n);
    return 0;
}
