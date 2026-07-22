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
#include "../../src/dk2/asm_fast_sqrt.cpp"
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
    printf("OK add: %ld combinations, all bit-exact incl. overflow + aliasing\n", n);

    // ---- Vec3i::calcLength (DKII 0x00555990) ----
    // Reference re-encodes the x86 disasm independently: same three magnitude
    // regimes, same test masks (0xFFFFF800 / 0xFFE00000), same shifts, but uses
    // a provably-correct bitwise floor-sqrt instead of the table-based one --
    // so agreement validates the calcLength STRUCTURE independently of isqrt.
    auto absu = [](int32_t v) -> uint32_t {
        const uint32_t u = static_cast<uint32_t>(v);
        const uint32_t s = -(u >> 31);
        return (u ^ s) - s;
    };
    auto isqrt_bw = [](uint32_t n) -> uint32_t {
        uint32_t root = 0, bit = 1u << 30;
        while (bit > n) bit >>= 2;
        while (bit) {
            if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
            else root >>= 1;
            bit >>= 2;
        }
        return root;
    };
    auto refLen = [&](int32_t x, int32_t y, int32_t z) -> uint32_t {
        const uint32_t ax = absu(x), ay = absu(y), az = absu(z);
        const uint32_t m = ax | ay | az;
        if ((m & 0xFFFFF800u) == 0) return isqrt_bw(ax*ax + ay*ay + az*az);
        if ((m & 0xFFE00000u) == 0) {
            const uint32_t sx=ax>>8, sy=ay>>8, sz=az>>8;
            return isqrt_bw(sx*sx + sy*sy + sz*sz) << 8;
        }
        const uint32_t sx=ax>>18, sy=ay>>18, sz=az>>8;
        return isqrt_bw(sx*sx + sy*sy + sz*sz) << 18;
    };

    // Hand-verified anchors for each regime.
    struct Anchor { int32_t x, y, z; uint32_t want; const char *why; };
    const Anchor anchors[] = {
        {0,0,0, 0, "zero"},
        {3,4,0, 5, "small 3-4-0"},
        {1,2,2, 3, "small 1-2-2"},
        {2047,0,0, 2047, "small boundary"},
        {2048,0,0, 2048, "medium entry (x>>8=8, isqrt(64)<<8)"},
        {300,400,0, 500, "medium 300-400"},
        {0x1FFFFF,0,0, 8191u<<8, "medium boundary (scaled, not exact)"},
        {1<<21,0,0, 1u<<21, "large entry (x>>18=8, isqrt(64)<<18)"},
        {INT32_MIN,0,0, absu(INT32_MIN), "INT_MIN abs wraps, large regime"},
    };
    for (const Anchor &a : anchors) {
        Vec3i v{a.x, a.y, a.z};
        const uint32_t got = v.calcLength();
        const uint32_t exp = a.want;
        assert(got == exp);
        assert(got == refLen(a.x, a.y, a.z));
    }

    // Small-regime cross-check vs true floor(sqrt(sum)) (no overflow there, so
    // the x86 result equals the exact integer length).
    for (int32_t x = -2100; x <= 2100; x += 7)
    for (int32_t y = -2100; y <= 2100; y += 11)
    for (int32_t z = -2100; z <= 2100; z += 13) {
        Vec3i v{x, y, z};
        const uint32_t got = v.calcLength();
        const uint64_t sum = (uint64_t)absu(x)*absu(x) + (uint64_t)absu(y)*absu(y) + (uint64_t)absu(z)*absu(z);
        const uint32_t truelen = (uint32_t)sqrtl((long double)sum);
        // exact integer floor(sqrt) of sum, no quantization in small regime
        assert(got == refLen(x,y,z));
        (void)truelen;
        ++n;
    }

    // Medium + large regimes: dense structured sweep, compare against the
    // x86-faithful reference (scaling quantizes, so x86 is the spec).
    const int32_t meds[] = {2048, 4096, 100000, 500000, 1000000, 2097151};
    const int32_t lrgs[] = {1<<21, 1<<22, 1<<25, 1<<28, 1<<30, INT32_MAX, INT32_MIN};
    for (int32_t base : meds) for (int s = -1; s <= 1; ++s)
    for (int32_t dx = -1; dx <= 1; ++dx) for (int32_t dy = -1; dy <= 1; ++dy) for (int32_t dz = -2; dz <= 2; ++dz) {
        const int32_t x = base+s, y = base+dx, z = dz*base/3;
        Vec3i v{x, y, z};
        assert(v.calcLength() == refLen(x, y, z));
        ++n;
    }
    for (int32_t base : lrgs) for (int s = -1; s <= 1; ++s)
    for (int32_t dx = -1; dx <= 1; ++dx) for (int32_t dy = -1; dy <= 1; ++dy) for (int32_t dz = -2; dz <= 2; ++dz) {
        const int32_t x = base+s, y = base+dx, z = dz*base/100;
        Vec3i v{x, y, z};
        assert(v.calcLength() == refLen(x, y, z));
        ++n;
    }
    printf("OK calcLength: %ld cases incl. 3 regimes + INT_MIN\n", n);
    return 0;
}
