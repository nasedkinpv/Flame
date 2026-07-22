// DKII 0x00437FE0: Vec3i::add — integer componentwise add. Leaf, no globals;
// output may alias this/right. Two's-complement wraparound via uint32 (matches
// x86 `add`; well-defined under -fsanitize). Returns output.
#include "dk2/utils/Vec3i.h"

#include <cstdint>

dk2::Vec3i *dk2::Vec3i::add(Vec3i *right, Vec3i *output) {
    output->x = static_cast<int32_t>(static_cast<uint32_t>(x) + static_cast<uint32_t>(right->x));
    output->y = static_cast<int32_t>(static_cast<uint32_t>(y) + static_cast<uint32_t>(right->y));
    output->z = static_cast<int32_t>(static_cast<uint32_t>(z) + static_cast<uint32_t>(right->z));
    return output;
}
