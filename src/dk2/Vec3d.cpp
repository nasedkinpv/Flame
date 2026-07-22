// DKII 0x0040F680: Vec3d::addVec3d — integer componentwise add. Despite the
// name, Vec3d holds int x,y,z (sizeof 0xC), not double. Leaf, no globals.
// ABI (param1 = output written/returned, param2 = right read) matches Vec3i::add.
// Two's-complement wraparound via uint32 (matches x86 `add`).
#include "dk2/utils/Vec3d.h"

#include <cstdint>

dk2::Vec3d *dk2::Vec3d::addVec3d(Vec3d *output, Vec3d *right) {
    output->x = static_cast<int32_t>(static_cast<uint32_t>(x) + static_cast<uint32_t>(right->x));
    output->y = static_cast<int32_t>(static_cast<uint32_t>(y) + static_cast<uint32_t>(right->y));
    output->z = static_cast<int32_t>(static_cast<uint32_t>(z) + static_cast<uint32_t>(right->z));
    return output;
}
