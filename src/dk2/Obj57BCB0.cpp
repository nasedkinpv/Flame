#include "dk2/Obj57BCB0.h"
#include "dk2/math/directional_lighting.h"

#include <cstdint>
#include <cstring>


namespace {

void calculateLighting(
        const dk2::Obj57BCB0 &lights,
        float *accumulator,
        const float *position,
        const float *normal) {
    const auto *distanceTable = reinterpret_cast<const float *>(0x007818A0);
    const int32_t lightCount = static_cast<int32_t>(lights.count);
    if (lightCount <= 0) {
        return;
    }

    for (int32_t i = 0; i < lightCount; ++i) {
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
    calculateLighting(*this, accumulator, position, normal);
    return accumulator;
}


float *dk2::Obj57BCB0::sub_57C190(
        float *accumulator, float *position, float *normal) {
    static_assert(sizeof(Obj57BCB0) == sizeof(lighting::DirectionalLightBuffer));
    const auto &lights = reinterpret_cast<const lighting::DirectionalLightBuffer &>(*this);
    lighting::accumulateDirectional(lights, accumulator, position, normal);
    return accumulator;  // all original callers ignore EAX
}
