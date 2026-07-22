// Offline differential test for src/dk2/asm_fast_sqrt.cpp (native replacement
// of DKII.EXE 0x0065FAE0 asm_fast_sqrt).
//
// Three independent implementations must agree bit-for-bit:
//   impl        — the real rewrite (table + bsr + unsigned-div Newton loop)
//   x86_ref     — faithful re-encoding of the x86 disasm (table extracted from
//                 the EXE independently, bsr via clz, same loop)
//   independent — provably-correct bitwise floor-sqrt (no table, no division)
//
// If impl == independent over the whole 32-bit input space, impl computes
// floor(sqrt(n)). If x86_ref == independent too, the x86 algorithm ALSO
// computes floor(sqrt(n)), so impl is bit-exact with x86.
//
// Coverage: exhaustive over [0, 2^24]; every perfect-square boundary
// k*k-2 .. k*k+2 for all k in [0, 65535] (every point where floor(sqrt)
// changes); plus 200M pseudo-random values across the full 32-bit range.
//
// Build & run (Apple Silicon via Rosetta):
//   clang++ -arch x86_64 -O3 -std=c++17 \
//       -o /tmp/isqrt_difftest tests/isqrt_difftest/isqrt_difftest.cpp
//   /tmp/isqrt_difftest
#include "../../src/dk2/asm_fast_sqrt.cpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

using dk2::asm_fast_sqrt;

// Reference 1: x86-faithful re-encoding. Table copied straight from the EXE
// bytes at 0x0065FB14 (independent of the impl's copy, so a transcription typo
// in either is caught by disagreement with `independent`).
static uint32_t x86_ref(uint32_t n) {
    static const uint16_t tbl[32] = {
            1, 2, 2, 4, 5, 8, 11, 16, 22, 32, 45, 64, 90, 128, 181, 256,
            362, 512, 724, 1024, 1448, 2048, 2896, 4096, 5792, 8192, 11585,
            16384, 23170, 32768, 46340, 65535,
    };
    if (n == 0) return 0;
    int bsr = 31 - __builtin_clz(n);
    uint32_t x = tbl[bsr];
    for (;;) {
        uint32_t q = n / x;
        if (q >= x) break;
        x = (x + q) >> 1;
    }
    return x;
}

// Reference 2: provably-correct bitwise integer sqrt (classic digit-by-digit,
// no lookup table, no division). Ground truth for floor(sqrt(n)).
static uint32_t independent(uint32_t n) {
    uint32_t root = 0;
    uint32_t bit = 1u << 30;  // largest power of four <= 2^32
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static uint64_t failures = 0;
static uint64_t checked = 0;

static void check(uint32_t n) {
    ++checked;
    const uint32_t a = asm_fast_sqrt(n);
    const uint32_t b = x86_ref(n);
    const uint32_t c = independent(n);
    if (a != b || a != c) {
        if (failures < 20) {
            printf("MISMATCH n=%u (0x%X): impl=%u x86_ref=%u independent=%u\n",
                   n, n, a, b, c);
        }
        ++failures;
    }
}

int main() {
    // 1. Exhaustive over [0, 2^24): sqrt <= 4095, dense on the low range.
    const uint32_t lo = 1u << 24;
    for (uint32_t n = 0; n < lo; ++n) check(n);
    printf("  [0, 2^24) exhaustive done: %llu checked\n",
           (unsigned long long)checked);

    // 2. Every perfect-square boundary k*k-2 .. k*k+2 for all k. These are the
    //    only points where floor(sqrt(n)) transitions, so they are the regions
    //    an off-by-one (wrong convergence / wrong truncation) would surface.
    for (uint64_t k = 0; k <= 65535; ++k) {
        const uint64_t ksq = k * k;
        for (int64_t d = -2; d <= 2; ++d) {
            const int64_t v = static_cast<int64_t>(ksq) + d;
            if (v < 0) continue;
            if (v > 0xFFFFFFFFull) continue;
            check(static_cast<uint32_t>(v));
        }
    }
    printf("  perfect-square boundaries done: %llu checked\n",
           (unsigned long long)checked);

    // 3. Pseudo-random across the full 32-bit range (LCG, deterministic).
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (uint64_t i = 0; i < 200000000ull; ++i) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        check(static_cast<uint32_t>(s >> 32));
    }
    printf("  200M random done: %llu checked\n",
           (unsigned long long)checked);

    // 4. Powers of two and neighbors (bsr transition points).
    for (int b = 0; b < 32; ++b) {
        const uint32_t base = 1u << b;
        for (int d = -2; d <= 2; ++d) {
            uint64_t v = static_cast<uint64_t>(base) + d;
            if (v > 0xFFFFFFFFull) continue;
            check(static_cast<uint32_t>(v));
        }
    }

    if (failures) {
        printf("FAIL: %llu mismatch(es) across %llu cases\n",
               (unsigned long long)failures, (unsigned long long)checked);
        return 1;
    }
    printf("OK: %llu cases, asm_fast_sqrt == x86_ref == independent floor(sqrt)\n",
           (unsigned long long)checked);
    return 0;
}
