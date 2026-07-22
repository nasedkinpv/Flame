// DKII 0x0065FAE0: asm_fast_sqrt — integer floor-sqrt via Newton-Raphson.
//
// Clean leaf (no calls, no globals read): bsr(n) indexes a 32-entry uint16
// initial-guess table, then an unsigned-divide Newton loop refines to
// floor(sqrt(n)). 61 call sites in DKII.EXE; it is also the sole dependency of
// Vec3i::calcLength (0x00555990), which becomes a translatable leaf once this
// is in place.
//
// Bit-exact with the x86: same table (extracted verbatim from
// g_fastSqrtByHighestBit_arr at 0x0065FB14), same unsigned division, same
// convergence test (stop when n/x >= x), same truncated Newton step ((x+q)>>1).
// bsr(n>0) == 31 - clz(n); guarded n==0 == 0 like the original.
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace dk2 {

namespace {

int bsr32(uint32_t n) {
    // n != 0 on entry (caller guards); x86 bsr is undefined for 0.
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanReverse(&i, n);
    return static_cast<int>(i);
#else
    return 31 - __builtin_clz(n);
#endif
}

// Initial guess indexed by the bit-index of n's highest set bit (== x86 bsr).
// Replicated verbatim from g_fastSqrtByHighestBit_arr (DKII 0x0065FB14, 32 x
// uint16). Embedding the constant keeps this a self-contained leaf.
constexpr uint16_t fastSqrtByHighestBit[32] = {
        1, 2, 2, 4, 5, 8, 11, 16, 22, 32, 45, 64, 90, 128, 181, 256,
        362, 512, 724, 1024, 1448, 2048, 2896, 4096, 5792, 8192, 11585,
        16384, 23170, 32768, 46340, 65535,
};

}  // namespace

uint32_t asm_fast_sqrt(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = fastSqrtByHighestBit[bsr32(n)];
    for (;;) {
        const uint32_t q = n / x;  // unsigned (edx:eax / ebx on x86)
        if (q >= x) break;         // x^2 <= n: converged
        x = (x + q) >> 1;          // Newton step, truncated toward zero
    }
    return x;
}

}  // namespace dk2
