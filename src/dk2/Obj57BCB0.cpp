#include "dk2/Obj57BCB0.h"

#include <Windows.h>
#include <cstdio>


namespace {

using OriginalSub57BF00 = float *(__thiscall *)(
        dk2::Obj57BCB0 *, float *, float *, float *);

const auto originalSub57BF00 =
        reinterpret_cast<OriginalSub57BF00>(0x0057BF00);

volatile LONG loggedLightingSample = 0;

}


float *dk2::Obj57BCB0::sub_57BF00(
        float *accumulator, float *position, float *normal) {
    const float before[3] = {
        accumulator[0], accumulator[1], accumulator[2]
    };
    float *result = originalSub57BF00(this, accumulator, position, normal);

    const bool changed = accumulator[0] != before[0]
        || accumulator[1] != before[1]
        || accumulator[2] != before[2];
    if (changed && InterlockedCompareExchange(&loggedLightingSample, 1, 0) == 0) {
        std::printf(
            "[dk2:perf] sub_57BF00 count=%u pos=(%.9g,%.9g,%.9g) "
            "normal=(%.9g,%.9g,%.9g) before=(%.9g,%.9g,%.9g) "
            "after=(%.9g,%.9g,%.9g)\n",
            count,
            position[0], position[1], position[2],
            normal[0], normal[1], normal[2],
            before[0], before[1], before[2],
            accumulator[0], accumulator[1], accumulator[2]);
        for (uint32_t i = 0; i < count; ++i) {
            const auto &item = items[i];
            std::printf(
                "[dk2:perf] light[%u] vec=(%.9g,%.9g,%.9g) "
                "fC=%.9g f10=%.9g f14=%d f18=%.9g "
                "color=(%.9g,%.9g,%.9g)\n",
                i,
                item.vec.x, item.vec.y, item.vec.z,
                item.fC, item.f10, item.f14, item.f18,
                item.vec_1C.x, item.vec_1C.y, item.vec_1C.z);
        }
        std::fflush(stdout);
    }
    return result;
}
