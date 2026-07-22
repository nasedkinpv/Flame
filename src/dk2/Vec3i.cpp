// DKII 0x00437FE0: Vec3i::add — integer componentwise add. Leaf, no globals.
// ABI (verified from disasm, matches Vec3f::mulV convention): param1 = output
// (written, returned), param2 = right (read). output may alias this/right.
// Two's-complement wraparound via uint32 (matches x86 `add`).
#include "dk2/utils/Vec3i.h"

#include <cstdint>

// Defined in asm_fast_sqrt.cpp (DKII 0x0065FAE0); native integer floor-sqrt.
namespace dk2 {
uint32_t asm_fast_sqrt(uint32_t);
}

dk2::Vec3i *dk2::Vec3i::add(Vec3i *output, Vec3i *right) {
    output->x = static_cast<int32_t>(static_cast<uint32_t>(x) + static_cast<uint32_t>(right->x));
    output->y = static_cast<int32_t>(static_cast<uint32_t>(y) + static_cast<uint32_t>(right->y));
    output->z = static_cast<int32_t>(static_cast<uint32_t>(z) + static_cast<uint32_t>(right->z));
    return output;
}

// DKII 0x00555990: Vec3i::calcLength -- integer vector length via isqrt with
// magnitude-scaled regimes that keep the sum-of-squares in range. abs() uses
// the x86 xor/sub sign trick (so abs(INT_MIN) == 0x80000000, UB-free via
// uint32); squares are low-32-bit imul (uint32 wraparound). The regime is
// chosen by the OR of all three abs components (x86 `test` masks verbatim):
//   small  (all <= 0x7FF):     isqrt(x^2 + y^2 + z^2)
//   medium (all <= 0x1FFFFF):  isqrt((x>>8)^2 + (y>>8)^2 + (z>>8)^2) << 8
//   large  otherwise:          isqrt((x>>18)^2 + (y>>18)^2 + (z>>8)^2) << 18
// The large regime scales z by 8, not 18 -- replicated from the disasm.
uint32_t dk2::Vec3i::calcLength() {
    auto absu = [](int32_t v) -> uint32_t {
        const uint32_t u = static_cast<uint32_t>(v);
        const uint32_t sgn = -(u >> 31);   // 0 or 0xFFFFFFFF
        return (u ^ sgn) - sgn;           // x86: xor edx; sub edx
    };
    const uint32_t ax = absu(x), ay = absu(y), az = absu(z);
    const uint32_t m = ax | ay | az;
    if ((m & 0xFFFFF800u) == 0) {
        return asm_fast_sqrt(ax * ax + ay * ay + az * az);
    }
    if ((m & 0xFFE00000u) == 0) {
        const uint32_t sx = ax >> 8, sy = ay >> 8, sz = az >> 8;
        return asm_fast_sqrt(sx * sx + sy * sy + sz * sz) << 8;
    }
    const uint32_t sx = ax >> 18, sy = ay >> 18, sz = az >> 8;
    return asm_fast_sqrt(sx * sx + sy * sy + sz * sz) << 18;
}
