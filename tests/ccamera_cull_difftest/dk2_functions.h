#pragma once
#include <cstdint>
#include "dk2/utils/Vec3f.h"

// C++ requires these forward-declared in namespace dk2 before CCamera.cpp's
// out-of-line `dk2::Vec3f_static_sub_575D70(...)` definitions are valid -
// matches the real auto-generated dk2_functions.h entries verbatim.
namespace dk2 {
int __cdecl Vec3f_static_sub_575D70(Vec3f *, float, uint32_t *);
Vec3f *__cdecl Vec3f_static_sub_575F10(Vec3f *, float, Vec3f *, float *);
}
