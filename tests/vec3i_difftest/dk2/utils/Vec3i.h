// Offline stub of the genapi-generated dk2/utils/Vec3i.h (struct id vec_xyz,
// sgmap: DKII_EXE_v170.sgmap). Layout must match the generated struct.
#pragma once
#include <cstdint>
namespace dk2 {
#pragma pack(push, 1)
struct Vec3i {
    int32_t x, y, z;
    Vec3i *add(Vec3i *right, Vec3i *output);
};
#pragma pack(pop)
static_assert(sizeof(Vec3i) == 0xC);
}
