// Offline differential test for src/dk2/AABB.cpp (native replacements of
// DKII.EXE 0052D3A0 AABB::contains and 005B7050 AABB::isIntersects).
// Pure integer predicates (signed compares), BOOL (0/1) return.
//
// References model the disasm exactly. Cross-products of two boxes cover the
// fully-bounded, partial, disjoint, touching, and degenerate (min>max) cases.
//
// Build & run (Apple Silicon via Rosetta):
//   clang++ -arch x86_64 -O2 -std=c++17 -I tests/aabb_difftest \
//       -o /tmp/aabb_difftest tests/aabb_difftest/aabb_difftest.cpp
//   /tmp/aabb_difftest
#include "../../src/dk2/AABB.cpp"

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

int main() {
    // moderate grid (independent min/max picks → includes degenerate min>max)
    const std::vector<int32_t> coords = {-1000, -1, 0, 5, 1000};
    long n = 0;
    auto makeBox = [&](int32_t xlo, int32_t xhi, int32_t ylo, int32_t yhi) {
        return AABB{xlo, ylo, xhi, yhi};
    };
    for (int32_t axl : coords) for (int32_t axh : coords)
    for (int32_t ayl : coords) for (int32_t ayh : coords)
    for (int32_t bxl : coords) for (int32_t bxh : coords)
    for (int32_t byl : coords) for (int32_t byh : coords) {
        const AABB a = makeBox(axl, axh, ayl, ayh);
        const AABB b = makeBox(bxl, bxh, byl, byh);
        AABB ta = a;
        assert(ta.contains(const_cast<AABB *>(&b)) == refContains(a, b));
        assert(ta.isIntersects(const_cast<AABB *>(&b)) == refIntersects(a, b));
        ++n;
    }

    // edge cases: extremes, equality, self-intersect, degenerate
    const std::vector<AABB> edges = {
        {0,0,0,0}, {INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX},
        {INT32_MAX,INT32_MAX,INT32_MIN,INT32_MIN},  // fully degenerate/inverted
        {INT32_MIN,INT32_MIN,INT32_MIN,INT32_MIN},
        {INT32_MAX,INT32_MAX,INT32_MAX,INT32_MAX},
        {-5,-5,5,5}, {5,5,-5,-5}, {-10,5,-10,5}, {5,-10,5,-10}};
    for (const AABB &a : edges) for (const AABB &b : edges) {
        AABB ta = a;
        assert(ta.contains(const_cast<AABB *>(&b)) == refContains(a, b));
        assert(ta.isIntersects(const_cast<AABB *>(&b)) == refIntersects(a, b));
        // self: a with itself — contains(self) and intersects(self) both reflect
        // the box's own min<=max / min<=max conditions
        AABB sa = a;
        BOOL sc = refContains(a, a), si = refIntersects(a, a);
        assert(sa.contains(&sa) == sc && sa.isIntersects(&sa) == si);
        ++n;
    }
    printf("OK: %ld box-pairs, contains + isIntersects bit-exact\n", n);
    return 0;
}
