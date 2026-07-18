#include "dk2/Obj57BCB0.h"

#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>


namespace {

using OriginalSub57BF00 = float *(__thiscall *)(
        dk2::Obj57BCB0 *, float *, float *, float *);

const auto originalSub57BF00 =
        reinterpret_cast<OriginalSub57BF00>(0x0057BF00);

constexpr uint32_t kValidationCalls = 10000;

struct LightingValidation {
    uint32_t calls = 0;
    uint32_t nonExact = 0;
    uint32_t overOneHundredth = 0;
    uint32_t invalidTableIndex = 0;
    float maxAbsoluteError = 0.0f;
};

LightingValidation lightingValidation;

void calculateLighting(
        const dk2::Obj57BCB0 &lights,
        float *accumulator,
        const float *position,
        const float *normal,
        uint32_t &invalidTableIndex) {
    const auto *distanceTable = reinterpret_cast<const float *>(0x007818A0);

    for (uint32_t i = 0; i < lights.count; ++i) {
        const auto &light = lights.items[i];
        const float dx = position[0] - light.vec.x;
        const float dy = position[1] - light.vec.y;
        const float dz = position[2] - light.vec.z;
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        if (!(distanceSquared < light.fC)) {
            continue;
        }

        const float encodedIndex =
                12582912.0f - (0.499989986f - 16.0f * distanceSquared);
        uint32_t indexBits;
        std::memcpy(&indexBits, &encodedIndex, sizeof(indexBits));
        const uint32_t index = (indexBits & 0x007FFFFF) - 0x00400000;
        if (index >= 256) {
            ++invalidTableIndex;
            continue;
        }

        const float attenuation =
                (light.fC - distanceSquared) * distanceTable[index] * light.f10;
        const float facing = (
                normal[0] * -dx
                + normal[1] * -dy
                + normal[2] * -dz) * light.f18;
        if (facing < 0.0f) {
            continue;
        }

        const float factor = attenuation * facing;
        if (factor > 1.0f) {
            accumulator[0] += light.vec_1C.x;
            accumulator[1] += light.vec_1C.y;
            accumulator[2] += light.vec_1C.z;
        } else {
            accumulator[0] += light.vec_1C.x * factor;
            accumulator[1] += light.vec_1C.y * factor;
            accumulator[2] += light.vec_1C.z * factor;
        }
    }
}

}


float *dk2::Obj57BCB0::sub_57BF00(
        float *accumulator, float *position, float *normal) {
    float candidate[3] = {accumulator[0], accumulator[1], accumulator[2]};
    const bool validate = lightingValidation.calls < kValidationCalls;
    if (validate) {
        calculateLighting(
            *this, candidate, position, normal,
            lightingValidation.invalidTableIndex);
    }

    float *result = originalSub57BF00(this, accumulator, position, normal);

    if (!validate) {
        return result;
    }

    float callMaxError = 0.0f;
    bool exact = true;
    for (uint32_t i = 0; i < 3; ++i) {
        if (candidate[i] != accumulator[i]) {
            exact = false;
        }
        const float error = std::fabs(candidate[i] - accumulator[i]);
        if (error > callMaxError) {
            callMaxError = error;
        }
    }
    if (!exact) {
        ++lightingValidation.nonExact;
    }
    if (callMaxError > 0.01f) {
        ++lightingValidation.overOneHundredth;
    }
    if (callMaxError > lightingValidation.maxAbsoluteError) {
        lightingValidation.maxAbsoluteError = callMaxError;
    }

    ++lightingValidation.calls;
    if (lightingValidation.calls == kValidationCalls) {
        std::printf(
            "[dk2:perf] sub_57BF00 validation calls=%u nonExact=%u "
            "over0.01=%u invalidIndex=%u maxAbsError=%.9g\n",
            lightingValidation.calls,
            lightingValidation.nonExact,
            lightingValidation.overOneHundredth,
            lightingValidation.invalidTableIndex,
            lightingValidation.maxAbsoluteError);
        std::fflush(stdout);
    }
    return result;
}
