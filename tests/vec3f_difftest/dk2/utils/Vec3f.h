// Offline stub of the genapi-generated dk2/utils/Vec3f.h (struct id vecf_xyz,
// sgmap: DKII_EXE_v170.sgmap). Layout and method list must match the generated one.
#pragma once
namespace dk2 {
#pragma pack(push, 1)
struct Vec3f {
    float x, y, z;
    Vec3f *mulV(Vec3f *, float);
    Vec3f *substractAssign(Vec3f *, Vec3f *);
    Vec3f *sumVec3f(Vec3f *, Vec3f *);
    float *sub_59E6E0(float *);
    float *sub_41C500(float *);
};
#pragma pack(pop)
static_assert(sizeof(Vec3f) == 0xC);
}
