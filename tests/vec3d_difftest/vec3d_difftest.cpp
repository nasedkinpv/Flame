// Offline differential test for src/dk2/Vec3d.cpp (native replacement of
// DKII.EXE 0040F680 Vec3d::addVec3d). Pure integer leaf (fields are int).
//
// ABI from disasm: this=ecx, param1=output (written, returned), param2=right.
// output = this + right, two's-complement wraparound. Verifies result, return
// pointer, and that right is untouched when distinct.
//
// Build & run (Apple Silicon via Rosetta):
//   clang++ -arch x86_64 -O2 -std=c++17 -I tests/vec3d_difftest \
//       -o /tmp/vec3d_difftest tests/vec3d_difftest/vec3d_difftest.cpp
//   /tmp/vec3d_difftest
#include "../../src/dk2/Vec3d.cpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using dk2::Vec3d;

static bool eq(const Vec3d &a, const Vec3d &b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static Vec3d refAdd(const Vec3d &t, const Vec3d &r) {
    Vec3d o;
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
    const std::vector<int32_t> rvals = {0, 1, -1, 7, -8, 1000000, INT32_MIN, INT32_MAX};

    long n = 0;
    for (int32_t ax : tvals) for (int32_t ay : tvals) for (int32_t az : tvals)
    for (int32_t bx : rvals) for (int32_t by : rvals) for (int32_t bz : rvals) {
        const Vec3d t{ax, ay, az}, r{bx, by, bz};
        const Vec3d e = refAdd(t, r);

        { Vec3d out{9, 9, 9}; Vec3d tt = t, rr = r;
          Vec3d *ret = tt.addVec3d(&out, &rr);
          assert(ret == &out && eq(out, e) && eq(rr, r)); }
        { Vec3d tt = t, rr = r; Vec3d *ret = tt.addVec3d(&tt, &rr);
          assert(ret == &tt && eq(tt, e) && eq(rr, r)); }
        { Vec3d tt = t, rr = r; Vec3d *ret = tt.addVec3d(&rr, &rr);
          assert(ret == &rr && eq(rr, e)); }
        ++n;
    }
    printf("OK: %ld combinations, all bit-exact incl. overflow + aliasing\n", n);
    return 0;
}
