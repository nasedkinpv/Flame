// Offline differential test for src/dk2/sub_57BBF0.cpp (native replacement of
// DKII.EXE 0x0057BBF0). Batched sphere-cull bitmask leaf on the
// appendToSceneObject2EList render hot path.
//
// Builds objects with sphere-entry arrays, sweeps (X,Y,Z,W,mask) + entry
// configs (centers/radii/flags), and asserts the impl bitmask == an independent
// double-precision reference. The reference uses double so its sign is
// ground-truth; the impl uses float (x87->SSE2). They agree for all realistic
// inputs (output is a discrete bitmask of sign tests); divergence is possible
// only within ~1e-7 of a sphere surface, which the sweep avoids by design.
//
// Build & run (Apple Silicon via Rosetta):
//   clang++ -arch x86_64 -O2 -std=c++17 -ffp-contract=off \
//       -o /tmp/sub57bbf0_difftest tests/sub_57BBF0_difftest/sub_57BBF0_difftest.cpp
//   /tmp/sub57bbf0_difftest
#include "../../src/dk2/sub_57BBF0.cpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using dk2::sub_57BBF0;

// ---- minimal object/entry builders matching the DKII layout ----
struct Entry {            // packed offsets: flags@4, cx@8, cy@0xC, cz@0x10, r@0x20
    uint32_t pad0;        // +0
    uint32_t flags;       // +4
    float cx, cy, cz;     // +8, +0xC, +0x10
    uint32_t pad1[3];     // +0x14..+0x1F  (12 bytes -> +0x20)
    float radius;         // +0x20
};
static_assert(offsetof(Entry, flags) == 0x04);
static_assert(offsetof(Entry, cx) == 0x08);
static_assert(offsetof(Entry, cz) == 0x10);
static_assert(offsetof(Entry, radius) == 0x20);

struct Obj {              // field0@0, field4@4, entry-ptr array @ +0x38
    int32_t base;
    int32_t count;
    uint32_t pad[12];     // +8..+0x37 (48 bytes)
    Entry *slots[64];     // +0x38 ...
};
static_assert(offsetof(Obj, slots) == 0x38);

// Independent reference: exact range/filter/bit logic, double-precision sign.
static int ref(const Obj *o, float X, float Y, float Z, float W, int mask) {
    const int32_t base = o->base, count = o->count;
    const bool selRange = (mask & 1u) != 0;
    const bool clampEmpty = (mask & 0x20u) != 0;
    const int32_t start = selRange ? base : 0;
    const int32_t end = clampEmpty ? base : (base + count);
    if (start >= end) return 0;
    const uint32_t bit0 = selRange ? static_cast<uint32_t>(base & 31) : 0u;
    uint32_t result = 0, bit = 1u << bit0;
    for (int32_t i = start; i < end; ++i) {
        const Entry *e = o->slots[i];
        if ((static_cast<int32_t>(e->flags) & mask) == mask) {
            const double dx = X - e->cx, dy = Y - e->cy, dz = Z - e->cz;
            const double rW = (double)e->radius + (double)W;
            const double s = rW * rW - (dx * dx + dy * dy + dz * dz);
            if (s < 0.0) result |= bit;
        }
        bit <<= 1;
    }
    return static_cast<int32_t>(result);
}

int main() {
    long n = 0, mism = 0;

    // Hand-verified anchors: single-sphere objects, point clearly in/out.
    // NOTE: mask bits 0 and 5 select range -- bit5 (0x20) clamps the range
    // empty, so use a mask WITHOUT bit5 to actually test a sphere.
    {
        Entry e{0, 0xffffffff, 0.f, 0.f, 0.f, {0,0,0}, 10.f};  // r=10 at origin
        Obj o{0, 1, {}, {&e, nullptr}};
        // mask=0x1: selRange (bit0) -> start=base=0, bit5 clear -> end=0+1=1, bit0=0
        assert(sub_57BBF0((int*)&o, nullptr, 0,0,0, 0, 0x1) == 0);   // inside -> bit0 clear
        assert(sub_57BBF0((int*)&o, nullptr, 100,0,0, 0, 0x1) == 1); // outside -> bit0 set
        assert(sub_57BBF0((int*)&o, nullptr, 100,0,0, 0, 0x1) == ref(&o,100,0,0,0,0x1));
        assert(sub_57BBF0((int*)&o, nullptr, 0,0,0, 0, 0x1) == ref(&o,0,0,0,0,0x1));
        // inflation by W: point at dist 12 is outside r=10 but inside r=10+W=15
        assert(sub_57BBF0((int*)&o, nullptr, 12,0,0, 5, 0x1) == 0);  // 12<15 inside
        assert(sub_57BBF0((int*)&o, nullptr, 12,0,0, 5, 0x1) == ref(&o,12,0,0,5,0x1));
        // mask=0xffffffff (bit0 AND bit5 set) -> clampEmpty -> empty range -> 0
        assert(sub_57BBF0((int*)&o, nullptr, 100,0,0, 0, static_cast<int>(0xffffffffu)) == 0);
        ++n;
    }

    // Sweep: multi-sphere objects, varied flags/centers/radii, realistic points.
    std::vector<float> coords = {-50,-12.5f,-3,0,0.5f,3,7,12.5f,50,200};
    std::vector<float> radii  = {0.25f,1,3.5f,10,40,100};
    std::vector<uint32_t> fvals = {0xffffffffu, 0x00000001u, 0x00000021u, 0x00000010u, 0xdeadbeefu};
    std::vector<int> masks = {-1, 0x1, 0x20, 0x21, 0x10, 0x100, 0xbeef};

    for (uint32_t fmask : fvals)
    for (int mask : masks)
    for (float cx : coords) for (float cy : coords) for (float cz : coords)
    for (float r : radii) {
        // build a 5-sphere object; sphere 0 uses fmask, others fixed flags
        Entry es[5];
        es[0] = {0, fmask, cx, cy, cz, {0,0,0}, r};
        es[1] = {0, 0xffffffffu, 10.f, 0, 0, {0,0,0}, 5.f};
        es[2] = {0, 0xffffffffu, -10.f, 0, 0, {0,0,0}, 5.f};
        es[3] = {0, 0x00000001u, 0, 30.f, 0, {0,0,0}, 8.f};
        es[4] = {0, 0x00000000u, 0, 0, 30.f, {0,0,0}, 8.f};  // never matches
        Obj o{0, 5, {}, {&es[0],&es[1],&es[2],&es[3],&es[4],nullptr}};
        // query points across the field
        for (float X : {-60.f,-12.f,0.f,3.f,10.f,15.f,60.f})
        for (float Y : {-60.f,0.f,30.f,60.f})
        for (float Z : {-60.f,0.f,30.f,60.f})
        for (float W : {0.f, 1.f, 5.f, 20.f}) {
            const int got = sub_57BBF0((int*)&o, nullptr, X,Y,Z, W, mask);
            const int exp = ref(&o, X,Y,Z, W, mask);
            if (got != exp) {
                if (mism < 10)
                    printf("MISMATCH fmask=%x mask=%x cx=%g cy=%g cz=%g r=%g "
                           "X=%g Y=%g Z=%g W=%g: got=%x exp=%x\n",
                           fmask, mask, cx, cy, cz, r, X, Y, Z, W, got, exp);
                ++mism;
            }
            ++n;
        }
    }

    // Range-selection cases: mask bit0 (start=base) and bit5 (end=base).
    // Hand-pinned values are error-prone here (bit0/5 interact), so validate
    // impl==ref across masks/points; pin only the one provably-empty case.
    {
        Entry es[6];
        for (int i = 0; i < 6; ++i) es[i] = {0, 0xffffffffu, (float)(i*20), 0, 0, {0,0,0}, 5.f};
        Obj o{2, 4, {}, {&es[0],&es[1],&es[2],&es[3],&es[4],&es[5],nullptr}}; // base=2,count=4
        for (int mask : {0x0, 0x1, 0x20, 0x21, 0x10, 0x11})
        for (float X : {-5.f, 0.f, 40.f, 60.f, 1000.f})
        for (float W : {0.f, 3.f, 50.f}) {
            const int Y=0,Z=0;
            assert(sub_57BBF0((int*)&o, nullptr, X,Y,Z, W, mask) == ref(&o, X,Y,Z, W, mask));
        }
        // mask=0x21 (bit0 AND bit5): start=base=2, end=base=2 -> empty -> 0
        assert(sub_57BBF0((int*)&o, nullptr, 1000,0,0, 0, 0x21) == 0);
        ++n;
    }

    if (mism) {
        printf("FAIL: %ld / %ld mismatched\n", mism, n);
        return 1;
    }
    printf("OK: %ld cases, sub_57BBF0 bitmask == double-precision reference "
           "(range/bit0/bit5 selection + flags filter)\n", n);
    return 0;
}
