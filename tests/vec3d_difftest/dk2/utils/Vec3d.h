// Offline stub of the genapi-generated dk2/utils/Vec3d.h (struct id vecd_xyz).
// NB: fields are int (sizeof 0xC), not double. Layout matches the generated struct.
#pragma once
#include <cstdint>
namespace dk2 {
#pragma pack(push, 1)
struct Vec3d {
    int32_t x, y, z;
    Vec3d *addVec3d(Vec3d *output, Vec3d *right);
};
#pragma pack(pop)
static_assert(sizeof(Vec3d) == 0xC);
}
