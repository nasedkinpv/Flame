// Offline differential test for src/dk2/Vec3i.cpp (native replacement of
// DKII.EXE 00437FE0 Vec3i::add). Pure integer leaf.
//
// ABI from disasm (matches Vec3f::mulV): this=ecx, param1=output (written,
// returned), param2=right (read). output = this + right, two's-complement
// wraparound. The test verifies the result, the return pointer, AND that the
// right operand is left untouched when distinct — so a param-order swap fails.
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

// models the ORIGINAL semantics: out = t + r (wraparound), r unchanged
static Vec3i refAdd(const Vec3i &t, const Vec3i &r) {
    Vec3i o;
    o.x = static_cast<int32_t>(static_cast<uint32_t>(t.x) + static_cast<uint32_t>(r.x));
    o.y = static_cast<int32_t>(static_cast<uint32_t>(t.y) + static_cast<uint32_t>(r.y));
    o.z = static_cast<int32_t>(static_cast<uint32_t>(t.z) + static_cast<uint32_t>(r.z));
    return o;
}

int main() {
    const std::vector<int32_t> tvals = {
        0, 1, -1, 2, -7, 7, 1000, -1000, 255, -256,
        123456, -123456, INT32_MIN, INT32_MAX, 0x7fffffff,
        static_cast<int32_t>(0x80000000), 65535, -65536};
    const std::vector<int32_t> rvals = {
        0, 1, -1, 7, -8, 1000000, INT32_MIN, INT32_MAX};

    long n = 0;
    for (int32_t ax : tvals) for (int32_t ay : tvals) for (int32_t az : tvals)
    for (int32_t bx : rvals) for (int32_t by : rvals) for (int32_t bz : rvals) {
        const Vec3i t{ax, ay, az}, r{bx, by, bz};
        const Vec3i e = refAdd(t, r);

        // distinct output: out = t + r, r untouched, return == &out
        { Vec3i out{9, 9, 9}; Vec3i tt = t, rr = r;
          Vec3i *ret = tt.add(&out, &rr);
          assert(ret == &out && eq(out, e) && eq(rr, r)); }
        // output == this: tt = t + r
        { Vec3i tt = t, rr = r;
          Vec3i *ret = tt.add(&tt, &rr);
          assert(ret == &tt && eq(tt, e) && eq(rr, r)); }
        // output == right: rr = t + r (rr is both right and output)
        { Vec3i tt = t, rr = r;
          Vec3i *ret = tt.add(&rr, &rr);
          assert(ret == &rr && eq(rr, e)); }
        ++n;
    }
    printf("OK: %ld combinations, all bit-exact incl. overflow + aliasing\n", n);
    return 0;
}
