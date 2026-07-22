// Offline differential test for src/dk2/Vec3i.cpp (native replacement of
// DKII.EXE 00437FE0 Vec3i::add). The original is a pure integer leaf:
// output = this + right per component, two's-complement wraparound (x86 add),
// returns output. No FP, so bit-exact equality over the full int32 range,
// including overflow and every aliasing combination.
//
// Build & run (Apple Silicon via Rosetta):
//   clang++ -arch x86_64 -O2 -std=c++17 -I tests/vec3i_difftest \
//       -o /tmp/vec3i_difftest tests/vec3i_difftest/vec3i_difftest.cpp
//   /tmp/vec3i_difftest
#include "../../src/dk2/Vec3i.cpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using dk2::Vec3i;

static bool eq(const Vec3i &a, const Vec3i &b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static Vec3i refAdd(const Vec3i &l, const Vec3i &r) {
    Vec3i o;
    o.x = static_cast<int32_t>(static_cast<uint32_t>(l.x) + static_cast<uint32_t>(r.x));
    o.y = static_cast<int32_t>(static_cast<uint32_t>(l.y) + static_cast<uint32_t>(r.y));
    o.z = static_cast<int32_t>(static_cast<uint32_t>(l.z) + static_cast<uint32_t>(r.z));
    return o;
}

int main() {
    const std::vector<int32_t> tvals = {
        0, 1, -1, 2, -7, 7, 1000, -1000, 255, -256,
        123456, -123456, INT32_MIN, INT32_MAX, 0x7fffffff,
        static_cast<int32_t>(0x80000000), 65535, -65536};
    // smaller curated set for the right operand (covers sign + overflow paths)
    const std::vector<int32_t> rvals = {
        0, 1, -1, 7, -8, 1000000, INT32_MIN, INT32_MAX};

    long n = 0;
    for (int32_t ax : tvals) for (int32_t ay : tvals) for (int32_t az : tvals)
    for (int32_t bx : rvals) for (int32_t by : rvals) for (int32_t bz : rvals) {
        const Vec3i t{ax, ay, az}, r{bx, by, bz};
        const Vec3i e = refAdd(t, r);

        { Vec3i o{9, 9, 9}; Vec3i tt = t;
          Vec3i *ret = tt.add(const_cast<Vec3i *>(&r), &o);
          assert(ret == &o && eq(o, e)); }  // distinct output
        { Vec3i tt = t; tt.add(const_cast<Vec3i *>(&r), &tt);
          assert(eq(tt, e)); }              // output == this
        { Vec3i rr = r; Vec3i tt = t; tt.add(&rr, &rr);
          assert(eq(rr, refAdd(t, r))); }   // output == right (this stays t)
        ++n;
    }
    printf("OK: %ld combinations, all bit-exact incl. overflow + aliasing\n", n);
    return 0;
}
