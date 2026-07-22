// Offline differential test for src/dk2/AABB.cpp (native replacements of
// DKII.EXE 0052D3A0 contains, 005B7050 isIntersects, 005B6FD0 intersection,
// 005B7090 getOuter). All pure integer, signed compares.
//
// References model the disasm exactly. intersection clamps max up to min per
// axis (disjoint → zero-size boundary box); getOuter is plain union.
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
    return t.minX <= o.minX && t.minY <= o.minY
        && t.maxX >= o.maxX && t.maxY >= o.maxY;
}
static BOOL refIntersects(const AABB &t, const AABB &o) {
    return t.minX <= o.maxX && t.maxX >= o.minX
        && t.minY <= o.maxY && t.maxY >= o.minY;
}
static AABB refIntersection(const AABB &t, const AABB &o) {
    int32_t mnX = std::max(t.minX, o.minX);
    int32_t mnY = std::max(t.minY, o.minY);
    int32_t mxX = std::min(t.maxX, o.maxX);
    int32_t mxY = std::min(t.maxY, o.maxY);
    mxX = std::max(mxX, mnX);
    mxY = std::max(mxY, mnY);
    return {mnX, mnY, mxX, mxY};
}
static AABB refOuter(const AABB &t, const AABB &o) {
    return {std::min(t.minX, o.minX), std::min(t.minY, o.minY),
            std::max(t.maxX, o.maxX), std::max(t.maxY, o.maxY)};
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
    for (int32_t axl : coords) for (int32_t axh : coords)
    for (int32_t ayl : coords) for (int32_t ayh : coords)
    for (int32_t bxl : coords) for (int32_t bxh : coords)
    for (int32_t byl : coords) for (int32_t byh : coords) {
        const AABB a = box(axl, axh, ayl, ayh);
        const AABB b = box(bxl, bxh, byl, byh);

        // predicates
        { AABB ta = a; assert(ta.contains(const_cast<AABB *>(&b)) == refContains(a, b)); }
        { AABB ta = a; assert(ta.isIntersects(const_cast<AABB *>(&b)) == refIntersects(a, b)); }

        // intersection: distinct / output==this / output==other
        const AABB ei = refIntersection(a, b);
        { AABB out{9,9,9,9}, tt = a, oo = b; AABB *r = tt.intersection(&out, &oo);
          assert(r == &out && eq(out, ei) && eq(oo, b) && eq(tt, a)); }
        { AABB tt = a, oo = b; AABB *r = tt.intersection(&tt, &oo);
          assert(r == &tt && eq(tt, ei) && eq(oo, b)); }
        { AABB tt = a, oo = b; AABB *r = tt.intersection(&oo, &oo);
          assert(r == &oo && eq(oo, refIntersection(a, b))); }

        // getOuter: distinct / output==this / output==other
        const AABB eo = refOuter(a, b);
        { AABB out{9,9,9,9}, tt = a, oo = b; AABB *r = tt.getOuter(&out, &oo);
          assert(r == &out && eq(out, eo) && eq(oo, b) && eq(tt, a)); }
        { AABB tt = a, oo = b; AABB *r = tt.getOuter(&tt, &oo);
          assert(r == &tt && eq(tt, eo) && eq(oo, b)); }
        { AABB tt = a, oo = b; AABB *r = tt.getOuter(&oo, &oo);
          assert(r == &oo && eq(oo, refOuter(a, b))); }
        ++n;
    }
    printf("OK: %ld box-pairs, contains/isIntersects/intersection/getOuter bit-exact\n", n);
    return 0;
}
