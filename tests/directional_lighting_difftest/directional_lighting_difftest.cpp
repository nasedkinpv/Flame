#include "dk2/math/directional_lighting.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>


namespace {

using dk2::lighting::DirectionalLight;
using dk2::lighting::DirectionalLightBuffer;

float rounded(float value) {
    volatile float result = value;
    return result;
}

void reference(
        const DirectionalLightBuffer &lights,
        float *accumulator,
        const float *position,
        const float *normal) {
    for (int i = 0; i < lights.count; ++i) {
        const DirectionalLight &light = lights.items[i];
        const float dx = rounded(light.position[0] - position[0]);
        const float dy = rounded(light.position[1] - position[1]);
        const float dz = rounded(light.position[2] - position[2]);
        const float nx = rounded(dx * normal[0]);
        const float ny = rounded(dy * normal[1]);
        const float nz = rounded(dz * normal[2]);
        const float dot = rounded(rounded(nx + nz) + ny);
        const float facing = rounded(dot * light.facingScale);
        if (dk2::lighting::floatBits(facing) >> 31) continue;
        const float factor = rounded(
                facing * dk2::lighting::floatFromBits(light.intensityBits));
        const float oneMinusFactor = rounded(1.0f - factor);
        const float applied = dk2::lighting::floatBits(oneMinusFactor) >> 31
                            ? 1.0f : factor;
        for (int channel = 0; channel < 3; ++channel) {
            accumulator[channel] = rounded(
                    accumulator[channel] + rounded(light.color[channel] * applied));
        }
    }
}

bool bitEqual(float left, float right) {
    return dk2::lighting::floatBits(left) == dk2::lighting::floatBits(right)
        || (std::isnan(left) && std::isnan(right));
}

}


int main() {
    std::mt19937 rng(0x57C190);
    std::uniform_real_distribution<float> value(-8.0f, 8.0f);
    std::uniform_real_distribution<float> positive(0.0f, 2.0f);
    for (int iteration = 0; iteration < 50000; ++iteration) {
        DirectionalLightBuffer lights{};
        lights.count = static_cast<int32_t>(rng() % 8);
        for (int i = 0; i < lights.count; ++i) {
            DirectionalLight &light = lights.items[i];
            for (float &component : light.position) component = value(rng);
            for (float &component : light.color) component = positive(rng);
            const float intensity = positive(rng);
            std::memcpy(&light.intensityBits, &intensity, sizeof(intensity));
            light.facingScale = value(rng);
        }
        float position[3]{value(rng), value(rng), value(rng)};
        float normal[3]{value(rng), value(rng), value(rng)};
        float expected[3]{value(rng), value(rng), value(rng)};
        float actual[3]{expected[0], expected[1], expected[2]};
        reference(lights, expected, position, normal);
        dk2::lighting::accumulateDirectional(lights, actual, position, normal);
        for (int channel = 0; channel < 3; ++channel) {
            assert(bitEqual(actual[channel], expected[channel]));
        }
    }
    std::puts("OK: 50000 directional-light cases are bit-exact");
}
