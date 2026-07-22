// Offline differential test for src/dk2/Pos2i.cpp (native replacement of
// DKII.EXE 004D1EC0 Pos2i::shiftDiv). 16.16 fixed-point division:
//   output.c = ((int64)this->c << 16) / divisor   (truncate toward zero)
// ABI: param1=output (written, returned), param2=divisor.
//
// Range note: x86 idiv faults (#DE) on divisor==0 or when the 32-bit quotient
// overflows. With |v| <= 32767, |v<<16| <= 2147418112 < INT32_MAX, so quotients
// always fit for any |divisor| >= 1 — that range is tested exhaustively; 0 and
// overflow cases are excluded (both fault identically in orig and rewrite).
//
// Build & run (Apple Silicon via Rosetta):
//   clang++ -arch x86_64 -O2 -std=c++17 -I tests/pos2i_difftest \
//       -o /tmp/pos2i_difftest tests/pos2i_difftest/pos2i_difftest.cpp
//   /tmp/pos2i_difftest
#include "../../src/dk2/Pos2i.cpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using dk2::Pos2i;

static bool eq(const Pos2i &a, const Pos2i &b) { return a.x == b.x && a.y == b.y; }

static int refComp(int v, int divisor) {
    return static_cast<int>((static_cast<int64_t>(v) << 16) / divisor);
}

static Pos2i refDiv(const Pos2i &t, int divisor) {
    return Pos2i{refComp(t.x, divisor), refComp(t.y, divisor)};
}

int main() {
    const std::vector<int32_t> vvals = {
        0, 1, -1, 2, -2, 7, -7, 100, -100, 255, -256, 1000, -1000,
        32767, -32767, 32760, -32760, 12345, -12345};
    const std::vector<int32_t> divs = {
        1, -1, 2, -2, 3, -3, 7, -8, 16, 255, 256, -256, 65536, -65536, 1000};

    long n = 0;
    for (int32_t vx : vvals) for (int32_t vy : vvals)
    for (int32_t d : divs) {
        const Pos2i t{vx, vy};
        const Pos2i e = refDiv(t, d);

        // distinct output: result correct, this untouched, return == &out
        { Pos2i out{9, 9}; Pos2i tt = t;
          Pos2i *ret = tt.shiftDiv(&out, d);
          assert(ret == &out && eq(out, e) && eq(tt, t)); }
        // output == this
        { Pos2i tt = t; Pos2i *ret = tt.shiftDiv(&tt, d);
          assert(ret == &tt && eq(tt, e)); }
        ++n;
    }
    printf("OK: %ld combinations, all bit-exact (fixed-point div, trunc toward 0)\n", n);
    return 0;
}
