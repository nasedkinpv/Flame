// Offline differential test for src/dk2/AABB.cpp (native replacements of
// DKII.EXE AABB methods: 0052D3A0 contains, 005B7050 isIntersects,
// 005B6FD0 intersection, 005B7090 getOuter, 00556590 appendPoint,
// 005DC2D0 move). All pure integer, signed compares / modular add/sub.
//
// Build & run (Apple Silicon via Rosetta):
//   clang++ -arch x86_64 -O2 -std=c++17 -I tests/aabb_difftest \
//       -o /tmp/aabb_difftest tests/aabb_difftest/aabb_difftest.cpp
//   /tmp/aabb_difftest
#include "../../src/dk2/AABB.cpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using dk2::AABB;

static BOOL refContains(const AABB &t, const AABB &o) {
    return t.minX <= o.minX && t.minY <= o.minY && t.maxX >= o.maxX && t.maxY >= o.maxY;
}
static BOOL refIntersects(const AABB &t, const AABB &o) {
    return t.minX <= o.maxX && t.maxX >= o.minX && t.minY <= o.maxY && t.maxY >= o.minY;
}
static AABB refIntersection(const AABB &t, const AABB &o) {
    int32_t mnX = std::max(t.minX, o.minX), mnY = std::max(t.minY, o.minY);
    int32_t mxX = std::min(t.maxX, o.maxX), mxY = std::min(t.maxY, o.maxY);
    return {mnX, mnY, std::max(mxX, mnX), std::max(mxY, mnY)};
}
static AABB refOuter(const AABB &t, const AABB &o) {
    return {std::min(t.minX, o.minX), std::min(t.minY, o.minY),
            std::max(t.maxX, o.maxX), std::max(t.maxY, o.maxY)};
}
// appendPoint: translate this by point, then copy this -> output
static AABB refAppend(const AABB &t, int32_t px, int32_t py) {
    return {static_cast<int32_t>(static_cast<uint32_t>(t.minX) + static_cast<uint32_t>(px)),
            static_cast<int32_t>(static_cast<uint32_t>(t.minY) + static_cast<uint32_t>(py)),
            static_cast<int32_t>(static_cast<uint32_t>(t.maxX) + static_cast<uint32_t>(px)),
            static_cast<int32_t>(static_cast<uint32_t>(t.maxY) + static_cast<uint32_t>(py))};
}
// move: minX->newX, minY->newY preserving size
static AABB refMove(const AABB &t, int32_t nx, int32_t ny) {
    AABB r;
    r.maxX = static_cast<int32_t>(static_cast<uint32_t>(t.maxX) + static_cast<uint32_t>(nx) - static_cast<uint32_t>(t.minX));
    r.maxY = static_cast<int32_t>(static_cast<uint32_t>(t.maxY) + static_cast<uint32_t>(ny) - static_cast<uint32_t>(t.minY));
    r.minX = nx;
    r.minY = ny;
    return r;
}
static bool eq(const AABB &a, const AABB &b) {
    return a.minX == b.minX && a.minY == b.minY && a.maxX == b.maxX && a.maxY == b.maxY;
}

int main() {
    const std::vector<int32_t> coords = {-1000, -1, 0, 5, 1000};
    long n = 0;
    auto box = [](int32_t xlo, int32_t xhi, int32_t ylo, int32_t yhi) {
        return AABB{xlo, ylo, xhi, yhi};
    };

    // --- contains / isIntersects / intersection / getOuter (two-box grid) ---
    for (int32_t axl : coords) for (int32_t axh : coords)
    for (int32_t ayl : coords) for (int32_t ayh : coords)
    for (int32_t bxl : coords) for (int32_t bxh : coords)
    for (int32_t byl : coords) for (int32_t byh : coords) {
        const AABB a = box(axl, axh, ayl, ayh);
        const AABB b = box(bxl, bxh, byl, byh);
        { AABB ta = a; assert(ta.contains(const_cast<AABB *>(&b)) == refContains(a, b)); }
        { AABB ta = a; assert(ta.isIntersects(const_cast<AABB *>(&b)) == refIntersects(a, b)); }
        const AABB ei = refIntersection(a, b);
        { AABB out{9,9,9,9}, tt = a, oo = b; AABB *r = tt.intersection(&out, &oo);
          assert(r == &out && eq(out, ei) && eq(oo, b) && eq(tt, a)); }
        { AABB tt = a, oo = b; tt.intersection(&tt, &oo); assert(eq(tt, ei) && eq(oo, b)); }
        { AABB tt = a, oo = b; tt.intersection(&oo, &oo); assert(eq(oo, refIntersection(a, b))); }
        const AABB eo = refOuter(a, b);
        { AABB out{9,9,9,9}, tt = a, oo = b; AABB *r = tt.getOuter(&out, &oo);
          assert(r == &out && eq(out, eo) && eq(oo, b) && eq(tt, a)); }
        { AABB tt = a, oo = b; tt.getOuter(&tt, &oo); assert(eq(tt, eo) && eq(oo, b)); }
        { AABB tt = a, oo = b; tt.getOuter(&oo, &oo); assert(eq(oo, refOuter(a, b))); }
        ++n;
    }

    // --- appendPoint (this mutated + copied to output) ---
    const std::vector<int32_t> pvals = {INT32_MIN, -1, 0, 1, 7, -8, 1000, -1000, INT32_MAX};
    for (int32_t xl : coords) for (int32_t xh : coords)
    for (int32_t yl : coords) for (int32_t yh : coords)
    for (int32_t px : pvals) for (int32_t py : pvals) {
        const AABB orig = box(xl, xh, yl, yh);
        const AABB e = refAppend(orig, px, py);
        // distinct output: this==e, output==e, point untouched, return==&out
        { AABB out{9,9,9,9}, tt = orig; tagPOINT pt{px, py};
          AABB *r = tt.appendPoint(&out, &pt);
          assert(r == &out && eq(tt, e) && eq(out, e) && pt.x == px && pt.y == py); }
        // output == this
        { AABB tt = orig; tagPOINT pt{px, py}; AABB *r = tt.appendPoint(&tt, &pt);
          assert(r == &tt && eq(tt, e)); }
        ++n;
    }

    // --- move (this mutated in place, returns newX) ---
    for (int32_t xl : coords) for (int32_t xh : coords)
    for (int32_t yl : coords) for (int32_t yh : coords)
    for (int32_t nx : pvals) for (int32_t ny : pvals) {
        const AABB orig = box(xl, xh, yl, yh);
        const AABB e = refMove(orig, nx, ny);
        AABB tt = orig;
        int ret = tt.move(nx, ny);
        assert(ret == nx && eq(tt, e));
        ++n;
    }

    printf("OK: %ld cases, all 6 AABB methods bit-exact\n", n);
    return 0;
}
