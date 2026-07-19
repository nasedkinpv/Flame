#include "dk2/Obj57BCB0.h"
#include "dk2/math/directional_lighting.h"
#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/Vec3f.h"
#include "patches/logging.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>


extern "C" int __fastcall sub_57BD70_fastcall(
        dk2::Obj57BCB0 *, void *, int32_t *, int,
        dk2::Mat3x3f, dk2::Vec3f);

// The sgmap describes 0057BD70 as a namespace-level __thiscall function.
// MSVC cannot spell that in C++, so give the generated export its ABI-equivalent
// fastcall implementation (ECX=this, EDX=unused, the remaining 56 bytes on stack).
#pragma comment(linker, "/alternatename:?sub_57BD70@dk2@@YEHPAHPAIHMHHHHHHHHMMM@Z=@sub_57BD70_fastcall@64")


namespace {

struct LightUsageProfile {
    uint64_t lightEntries = 0;
    uint32_t activeCalls = 0;
    uint32_t calls = 0;

    void add(uint32_t count, const char *name) {
        lightEntries += count;
        activeCalls += count != 0;
        if (++calls != 65536) return;
        patch::log::dbg(
                "PERF %s lighting: calls=%u active=%u lights=%llu avg_x100=%llu",
                name, calls, activeCalls, lightEntries,
                lightEntries * 100u / calls);
        *this = {};
    }
};

LightUsageProfile g_staticLightProfile;
LightUsageProfile g_directionalLightProfile;

#pragma pack(push, 1)
struct SceneLight {
    uint32_t unused;
    uint32_t flags;
    dk2::Vec3f position;
    dk2::Vec3f color;
    float queryRadius;
    float distanceSquaredLimit;
    float attenuationScale;
    uint8_t padding[8];
    float facingScale;
};
#pragma pack(pop)

static_assert(offsetof(SceneLight, flags) == 0x04);
static_assert(offsetof(SceneLight, position) == 0x08);
static_assert(offsetof(SceneLight, color) == 0x14);
static_assert(offsetof(SceneLight, queryRadius) == 0x20);
static_assert(offsetof(SceneLight, distanceSquaredLimit) == 0x24);
static_assert(offsetof(SceneLight, attenuationScale) == 0x28);
static_assert(offsetof(SceneLight, facingScale) == 0x34);
static_assert(sizeof(SceneLight) == 0x38);

const SceneLight *const *sceneLights(const int32_t *collection) {
    return reinterpret_cast<const SceneLight *const *>(
            reinterpret_cast<const uint8_t *>(collection) + 0x38);
}

float roundedMul(float left, float right) {
    return _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(left), _mm_set_ss(right)));
}

float roundedAdd(float left, float right) {
    return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(left), _mm_set_ss(right)));
}

dk2::Vec3f transformDirection(const dk2::Mat3x3f &matrix, const dk2::Vec3f &value) {
    dk2::Vec3f output;
    for (int row = 0; row < 3; ++row) {
        const float xy = roundedAdd(
                roundedMul(matrix.m[row][0], value.x),
                roundedMul(matrix.m[row][1], value.y));
        (&output.x)[row] = roundedAdd(xy, roundedMul(matrix.m[row][2], value.z));
    }
    return output;
}

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


dk2::Obj57BCB0 *dk2::Obj57BCB0::constructor(uint32_t *opaqueCollection, int mask) {
    const auto *collection = reinterpret_cast<const int32_t *>(opaqueCollection);
    const int32_t total = collection[0] + collection[1];
    const SceneLight *const *lights = sceneLights(collection);
    uint32_t outputCount = 0;
    uint32_t bit = 1;
    for (int32_t i = 0; i < total; ++i, bit <<= 1) {
        if ((static_cast<uint32_t>(mask) & bit) == 0) continue;
        const SceneLight &source = *lights[i];
        Obj57BCB0_item &destination = items[outputCount++];
        destination.vec = source.position;
        destination.fC = source.distanceSquaredLimit;
        destination.f10 = source.attenuationScale;
        destination.f18 = source.facingScale;
        destination.vec_1C = source.color;
    }
    count = outputCount;
    return this;
}


int __fastcall sub_57BD70_fastcall(
        dk2::Obj57BCB0 *self, void *,
        int32_t *opaqueCollection, int mask,
        dk2::Mat3x3f matrix, dk2::Vec3f position) {
    const int32_t total = opaqueCollection[0] + opaqueCollection[1];
    const SceneLight *const *lights = sceneLights(opaqueCollection);
    const float multiplier = *reinterpret_cast<const float *>(0x0066FB74);
    const float firstBias = *reinterpret_cast<const float *>(0x0066FB78);
    const float secondBias = *reinterpret_cast<const float *>(0x0066FB7C);
    const float zero = *reinterpret_cast<const float *>(0x0066FB70);
    const auto *distanceTable = reinterpret_cast<const float *>(0x007818A0);
    uint32_t outputCount = 0;
    uint32_t bit = 1;

    for (int32_t i = 0; i < total; ++i, bit <<= 1) {
        if ((static_cast<uint32_t>(mask) & bit) == 0) continue;
        const SceneLight &source = *lights[i];
        const float dx = _mm_cvtss_f32(_mm_sub_ss(
                _mm_set_ss(position.x), _mm_set_ss(source.position.x)));
        const float dy = _mm_cvtss_f32(_mm_sub_ss(
                _mm_set_ss(position.y), _mm_set_ss(source.position.y)));
        const float dz = _mm_cvtss_f32(_mm_sub_ss(
                _mm_set_ss(position.z), _mm_set_ss(source.position.z)));
        const float xy = roundedAdd(roundedMul(dx, dx), roundedMul(dy, dy));
        const float distanceSquared = roundedAdd(xy, roundedMul(dz, dz));
        if (!(distanceSquared < source.distanceSquaredLimit)) continue;

        const float encoded = _mm_cvtss_f32(_mm_sub_ss(
                _mm_set_ss(secondBias),
                _mm_sub_ss(_mm_set_ss(firstBias),
                           _mm_mul_ss(_mm_set_ss(distanceSquared), _mm_set_ss(multiplier)))));
        uint32_t encodedBits;
        std::memcpy(&encodedBits, &encoded, sizeof(encodedBits));
        const uint32_t tableIndex = (encodedBits & 0x007FFFFF) - 0x00400000;
        const float attenuation = roundedMul(
                roundedMul(
                        _mm_cvtss_f32(_mm_sub_ss(
                                _mm_set_ss(source.distanceSquaredLimit),
                                _mm_set_ss(distanceSquared))),
                        distanceTable[tableIndex]),
                source.attenuationScale);
        if (!(attenuation > zero)) continue;

        dk2::Obj57BCB0_item &destination = self->items[outputCount++];
        const dk2::Vec3f fromLight{
                source.position.x - position.x,
                source.position.y - position.y,
                source.position.z - position.z};
        destination.vec = transformDirection(matrix, fromLight);
        destination.f14 = static_cast<int>(dk2::lighting::floatBits(attenuation));
        destination.f18 = source.facingScale;
        destination.vec_1C = source.color;
    }
    self->count = outputCount;
    return static_cast<int>(outputCount);
}


float *dk2::Obj57BCB0::sub_57BF00(
        float *accumulator, float *position, float *normal) {
    g_staticLightProfile.add(count, "static");
    calculateLighting(*this, accumulator, position, normal);
    return accumulator;
}


float *dk2::Obj57BCB0::sub_57C190(
        float *accumulator, float *position, float *normal) {
    g_directionalLightProfile.add(count, "directional");
    static_assert(sizeof(Obj57BCB0) == sizeof(lighting::DirectionalLightBuffer));
    const auto &lights = reinterpret_cast<const lighting::DirectionalLightBuffer &>(*this);
    lighting::accumulateDirectional(lights, accumulator, position, normal);
    return accumulator;  // all original callers ignore EAX
}
