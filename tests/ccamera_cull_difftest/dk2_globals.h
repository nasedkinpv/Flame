#pragma once
#include <cstdint>
#include "dk2/utils/Vec3f.h"

namespace dk2 {

extern Vec3f g_vec_760B70;
extern Vec3f g_vec_760B38;
extern Vec3f g_vec_760B18;
extern Vec3f g_vec_760B28;
extern uint32_t g_drawSceneCount_76520C;

struct CamStateTarget { float x, y; };
struct CamState {
    CamStateTarget trg;
    float ww240;
};
extern CamState g_camState;

}  // namespace dk2
