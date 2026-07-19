#include "dk2/Obj58EF60.h"
#include "patches/logging.h"

#include <cstdint>
#include <emmintrin.h>
#include <windows.h>


namespace dk2 {
    bool installSpatialSamplerHotCallsite();
}


namespace {

int quantizeCell(float coordinate) {
    const __m128 value = _mm_set_ss(coordinate);
    const __m128 doubled = _mm_add_ss(value, value);
    const __m128 shifted = _mm_add_ss(doubled, _mm_set_ss(1.0f));
    const __m128 below = _mm_sub_ss(_mm_set_ss(0.49998998641967773f), shifted);
    const __m128 encoded = _mm_sub_ss(_mm_set_ss(12582912.0f), below);
    const uint32_t bits = static_cast<uint32_t>(
            _mm_cvtsi128_si32(_mm_castps_si128(encoded)));
    return static_cast<int>(bits & 0x007FFFFFu) - 0x00400001;
}

float cellFraction(float coordinate, int cell) {
    const __m128 value = _mm_set_ss(coordinate);
    return _mm_cvtss_f32(_mm_sub_ss(
            _mm_add_ss(value, value),
            _mm_cvtepi32_ps(_mm_cvtsi32_si128(cell))));
}

float roundedSubtract(float left, float right) {
    return _mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(left), _mm_set_ss(right)));
}

float roundedMultiply(float left, float right) {
    return _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(left), _mm_set_ss(right)));
}

__m128 loadVec3(const float *value) {
    return _mm_set_ps(0.0f, value[2], value[1], value[0]);
}

void storeVec3(float *output, __m128 value) {
    _mm_store_ss(output, value);
    _mm_store_ss(output + 1,
                 _mm_shuffle_ps(value, value, _MM_SHUFFLE(1, 1, 1, 1)));
    _mm_store_ss(output + 2,
                 _mm_shuffle_ps(value, value, _MM_SHUFFLE(2, 2, 2, 2)));
}

float *__fastcall sampleHotCallsite(
        dk2::Obj58EF60 *self, void *,
        float x, float y, float z, float *output) {
    static uint32_t calls = 0;
    if (++calls == 65536) {
        patch::log::dbg("spatial sampler: 65536 accelerated calls completed");
        calls = 0;
    }
    return self->sub_58F030(x, y, z, output);
}

}


float *dk2::Obj58EF60::sub_58F030(float x, float y, float z, float *output) {
    const float sx = roundedSubtract(x, f4);
    const float sy = roundedSubtract(y, f8);
    const float sz = roundedSubtract(z, -2.5f);
    const int ix = quantizeCell(sx);
    const int iy = quantizeCell(sy);
    const int iz = quantizeCell(sz);
    if (ix < 0 || ix > 4 || iy < 0 || iy > 4 || iz < 0 || iz > 3) {
        output[0] = x;
        output[1] = y;
        output[2] = z;
        return output;
    }

    const float fx = cellFraction(sx, ix);
    const float fy = cellFraction(sy, iy);
    const float fz = cellFraction(sz, iz);
    const float nx = roundedSubtract(1.0f, fx);
    const float ny = roundedSubtract(1.0f, fy);
    const float nz = roundedSubtract(1.0f, fz);

    // The original 512-byte table only maps the logical 6x6x5 field to its
    // physical 2x2x5 blocks.  Compute that mapping directly so the hot path
    // has no dependent table loads.
    const int blockIndexes[4]{
            (iy >> 1) + 4 * (ix >> 1),
            (iy >> 1) + 4 * ((ix + 1) >> 1),
            ((iy + 1) >> 1) + 4 * (ix >> 1),
            ((iy + 1) >> 1) + 4 * ((ix + 1) >> 1)};
    const int localIndexes[4]{
            5 * ((iy & 1) + 2 * (ix & 1)) + iz,
            5 * ((iy & 1) + 2 * ((ix + 1) & 1)) + iz,
            5 * (((iy + 1) & 1) + 2 * (ix & 1)) + iz,
            5 * (((iy + 1) & 1) + 2 * ((ix + 1) & 1)) + iz};
    const auto blocks = reinterpret_cast<const float *const *>(
            static_cast<uintptr_t>(static_cast<uint32_t>(f0)));
    const float *c000 = blocks[blockIndexes[0]] + localIndexes[0] * 3;
    const float *c001 = c000 + 3;
    const float *c100 = blocks[blockIndexes[1]] + localIndexes[1] * 3;
    const float *c101 = c100 + 3;
    const float *c010 = blocks[blockIndexes[2]] + localIndexes[2] * 3;
    const float *c011 = c010 + 3;
    const float *c110 = blocks[blockIndexes[3]] + localIndexes[3] * 3;
    const float *c111 = c110 + 3;

    const float nxny = roundedMultiply(nx, ny);
    const float fxny = roundedMultiply(fx, ny);
    const float nxfy = roundedMultiply(nx, fy);
    const float fxfy = roundedMultiply(fx, fy);
    const float weights[8]{
            roundedMultiply(nxny, nz), roundedMultiply(nxny, fz),
            roundedMultiply(fxny, nz), roundedMultiply(fxny, fz),
            roundedMultiply(nxfy, nz), roundedMultiply(nxfy, fz),
            roundedMultiply(fxfy, nz), roundedMultiply(fxfy, fz)};
    const float *corners[8]{
            c000, c001, c100, c101, c010, c011, c110, c111};

    __m128 result = _mm_mul_ps(loadVec3(corners[0]), _mm_set1_ps(weights[0]));
    for (int i = 1; i < 8; ++i) {
        result = _mm_add_ps(result,
                _mm_mul_ps(loadVec3(corners[i]), _mm_set1_ps(weights[i])));
    }
    storeVec3(output, result);
    return output;
}


bool dk2::installSpatialSamplerHotCallsite() {
    // Replacing all seven references previously exposed an unrelated early
    // startup path in Wine.  The measured renderer hotspot is this one call in
    // Obj57AD20::constructor, so patch only that proven site.
    constexpr uintptr_t callAddress = 0x0057AE48;
    constexpr uintptr_t originalTarget = 0x0058F030;
    auto *call = reinterpret_cast<uint8_t *>(callAddress);
    if (call[0] != 0xE8) {
        patch::log::err("spatial sampler: %08X is not a relative call", callAddress);
        return false;
    }

    const int32_t oldDisplacement = *reinterpret_cast<const int32_t *>(call + 1);
    const uintptr_t currentTarget = callAddress + 5 + oldDisplacement;
    if (currentTarget != originalTarget) {
        patch::log::err(
                "spatial sampler: unexpected target %08X at %08X",
                currentTarget, callAddress);
        return false;
    }

    const uintptr_t replacement = reinterpret_cast<uintptr_t>(&sampleHotCallsite);
    const int32_t displacement = static_cast<int32_t>(replacement - (callAddress + 5));
    DWORD oldProtection = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        patch::log::err("spatial sampler: VirtualProtect failed: %08X", GetLastError());
        return false;
    }
    *reinterpret_cast<int32_t *>(call + 1) = displacement;
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    DWORD ignored = 0;
    VirtualProtect(call, 5, oldProtection, &ignored);
    patch::log::dbg("spatial sampler: accelerated callsite %08X", callAddress);
    return true;
}
