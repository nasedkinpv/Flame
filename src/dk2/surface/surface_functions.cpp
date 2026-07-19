//
// Created by DiaLight on 3/30/2025.
//
#include <dk2_globals.h>
#include <dk2_functions.h>
#include <dk2/dk2_memory.h>
#include <dk2/MySurface.h>
#include <patches/logging.h>
#include <patches/micro_patches.h>

#include <cstdint>
#include <cstring>
#include <emmintrin.h>


namespace {

struct PairPackSpec {
    uint32_t replicatedScale;
    uint32_t rotate;
};

PairPackSpec makePairPackSpec(uint32_t mask) {
    uint32_t shift = 8;
    uint32_t scale = 0;
    while (mask && !(mask & 1)) {
        mask >>= 1;
        ++shift;
    }
    while (mask) {
        scale = (scale >> 1) + 0x80;
        mask >>= 1;
        ++shift;
    }
    return {scale | (scale << 16), shift & 31};
}

uint32_t rotateLeft(uint32_t value, uint32_t shift) {
    if (!shift) return value;
    return (value << shift) | (value >> (32 - shift));
}

uint32_t packPair(uint8_t first, uint8_t second, const PairPackSpec &spec) {
    const uint32_t pair = (static_cast<uint32_t>(first) << 16) | second;
    return rotateLeft(pair & spec.replicatedScale, spec.rotate);
}

void storeU32(void *destination, uint32_t value) {
    std::memcpy(destination, &value, sizeof(value));
}

__m128i loadRgb4(const uint8_t *source) {
    uint32_t first;
    uint32_t second;
    uint32_t third;
    std::memcpy(&first, source, sizeof(first));
    std::memcpy(&second, source + 4, sizeof(second));
    std::memcpy(&third, source + 8, sizeof(third));
    return _mm_set_epi32(
            static_cast<int>((third >> 8) | 0xFF000000u),
            static_cast<int>((second >> 16) | ((third & 0xFFu) << 16) | 0xFF000000u),
            static_cast<int>((first >> 24) | ((second & 0xFFFFu) << 8) | 0xFF000000u),
            static_cast<int>((first & 0x00FFFFFFu) | 0xFF000000u));
}

__m128i packUnsigned16(__m128i low, __m128i high) {
    const __m128i bias32 = _mm_set1_epi32(0x8000);
    const __m128i bias16 = _mm_set1_epi16(-32768);
    return _mm_xor_si128(
            _mm_packs_epi32(_mm_sub_epi32(low, bias32), _mm_sub_epi32(high, bias32)),
            bias16);
}

enum class Common16Format {
    none,
    rgb565,
    argb1555,
    argb4444,
};

Common16Format common16Format(const dk2::MySurfDesc &desc) {
    if (desc.dwRBitMask == 0xF800 && desc.dwGBitMask == 0x07E0 &&
        desc.dwBBitMask == 0x001F && desc.dwRGBAlphaBitMask == 0) {
        return Common16Format::rgb565;
    }
    if (desc.dwRBitMask == 0x7C00 && desc.dwGBitMask == 0x03E0 &&
        desc.dwBBitMask == 0x001F && desc.dwRGBAlphaBitMask == 0x8000) {
        return Common16Format::argb1555;
    }
    if (desc.dwRBitMask == 0x0F00 && desc.dwGBitMask == 0x00F0 &&
        desc.dwBBitMask == 0x000F && desc.dwRGBAlphaBitMask == 0xF000) {
        return Common16Format::argb4444;
    }
    return Common16Format::none;
}

__m128i packCommon16(
        __m128i low, __m128i high, Common16Format format, bool sourceHasAlpha) {
    __m128i packedLow;
    __m128i packedHigh;
    switch (format) {
    case Common16Format::rgb565:
        packedLow = _mm_or_si128(
                _mm_or_si128(
                        _mm_slli_epi32(_mm_and_si128(low, _mm_set1_epi32(0x000000F8)), 8),
                        _mm_srli_epi32(_mm_and_si128(low, _mm_set1_epi32(0x0000FC00)), 5)),
                _mm_srli_epi32(_mm_and_si128(low, _mm_set1_epi32(0x00F80000)), 19));
        packedHigh = _mm_or_si128(
                _mm_or_si128(
                        _mm_slli_epi32(_mm_and_si128(high, _mm_set1_epi32(0x000000F8)), 8),
                        _mm_srli_epi32(_mm_and_si128(high, _mm_set1_epi32(0x0000FC00)), 5)),
                _mm_srli_epi32(_mm_and_si128(high, _mm_set1_epi32(0x00F80000)), 19));
        break;
    case Common16Format::argb1555: {
        const __m128i alphaLow = sourceHasAlpha
                ? _mm_srli_epi32(_mm_and_si128(low, _mm_set1_epi32(0x80000000u)), 16)
                : _mm_set1_epi32(0x8000);
        const __m128i alphaHigh = sourceHasAlpha
                ? _mm_srli_epi32(_mm_and_si128(high, _mm_set1_epi32(0x80000000u)), 16)
                : _mm_set1_epi32(0x8000);
        packedLow = _mm_or_si128(
                _mm_or_si128(
                        _mm_or_si128(
                                _mm_slli_epi32(_mm_and_si128(low, _mm_set1_epi32(0x000000F8)), 7),
                                _mm_srli_epi32(_mm_and_si128(low, _mm_set1_epi32(0x0000F800)), 6)),
                        _mm_srli_epi32(_mm_and_si128(low, _mm_set1_epi32(0x00F80000)), 19)),
                alphaLow);
        packedHigh = _mm_or_si128(
                _mm_or_si128(
                        _mm_or_si128(
                                _mm_slli_epi32(_mm_and_si128(high, _mm_set1_epi32(0x000000F8)), 7),
                                _mm_srli_epi32(_mm_and_si128(high, _mm_set1_epi32(0x0000F800)), 6)),
                        _mm_srli_epi32(_mm_and_si128(high, _mm_set1_epi32(0x00F80000)), 19)),
                alphaHigh);
        break;
    }
    case Common16Format::argb4444: {
        const __m128i alphaLow = sourceHasAlpha
                ? _mm_srli_epi32(_mm_and_si128(low, _mm_set1_epi32(0xF0000000u)), 16)
                : _mm_set1_epi32(0xF000);
        const __m128i alphaHigh = sourceHasAlpha
                ? _mm_srli_epi32(_mm_and_si128(high, _mm_set1_epi32(0xF0000000u)), 16)
                : _mm_set1_epi32(0xF000);
        packedLow = _mm_or_si128(
                _mm_or_si128(
                        _mm_or_si128(
                                _mm_slli_epi32(_mm_and_si128(low, _mm_set1_epi32(0x000000F0)), 4),
                                _mm_srli_epi32(_mm_and_si128(low, _mm_set1_epi32(0x0000F000)), 8)),
                        _mm_srli_epi32(_mm_and_si128(low, _mm_set1_epi32(0x00F00000)), 20)),
                alphaLow);
        packedHigh = _mm_or_si128(
                _mm_or_si128(
                        _mm_or_si128(
                                _mm_slli_epi32(_mm_and_si128(high, _mm_set1_epi32(0x000000F0)), 4),
                                _mm_srli_epi32(_mm_and_si128(high, _mm_set1_epi32(0x0000F000)), 8)),
                        _mm_srli_epi32(_mm_and_si128(high, _mm_set1_epi32(0x00F00000)), 20)),
                alphaHigh);
        break;
    }
    default:
        return _mm_setzero_si128();
    }
    return packUnsigned16(packedLow, packedHigh);
}

void copyRows(dk2::MySurface *destination, const dk2::MySurface *source, uint32_t bytesPerPixel) {
    if (source->size.w <= 0 || source->size.h <= 0) return;
    const size_t rowBytes = static_cast<size_t>(source->size.w) * bytesPerPixel;
    auto *destinationRow = static_cast<uint8_t *>(destination->lpSurface);
    const auto *sourceRow = static_cast<const uint8_t *>(source->lpSurface);
    for (int row = 0; row < source->size.h; ++row) {
        std::memcpy(destinationRow, sourceRow, rowBytes);
        destinationRow += destination->lPitch;
        sourceRow += source->lPitch;
    }
}

void rgb24ToRgba32Tight(dk2::MySurface *destination, const dk2::MySurface *source) {
    if (source->size.w <= 0 || source->size.h <= 0) return;
    size_t pixels = static_cast<size_t>(source->size.w) * source->size.h;
    auto *out = static_cast<uint8_t *>(destination->lpSurface);
    const auto *in = static_cast<const uint8_t *>(source->lpSurface);
    while (pixels >= 4) {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(out), loadRgb4(in));
        in += 12;
        out += 16;
        pixels -= 4;
    }
    while (pixels--) {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        out[3] = 0xFF;
        in += 3;
        out += 4;
    }
}

void alpha8ToRgba32(dk2::MySurface *destination, const dk2::MySurface *source) {
    if (source->size.w <= 0 || source->size.h <= 0) return;
    auto *destinationRow = static_cast<uint8_t *>(destination->lpSurface);
    const auto *sourceRow = static_cast<const uint8_t *>(source->lpSurface);
    const __m128i zero = _mm_setzero_si128();
    for (int row = 0; row < source->size.h; ++row) {
        auto *out = reinterpret_cast<uint32_t *>(destinationRow);
        uint32_t column = 0;
        const uint32_t width = static_cast<uint32_t>(source->size.w);
        for (; column + 16 <= width; column += 16) {
            const __m128i bytes = _mm_loadu_si128(
                    reinterpret_cast<const __m128i *>(sourceRow + column));
            const __m128i lowWords = _mm_unpacklo_epi8(bytes, zero);
            const __m128i highWords = _mm_unpackhi_epi8(bytes, zero);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(out + column),
                    _mm_slli_epi32(_mm_unpacklo_epi16(lowWords, zero), 24));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(out + column + 4),
                    _mm_slli_epi32(_mm_unpackhi_epi16(lowWords, zero), 24));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(out + column + 8),
                    _mm_slli_epi32(_mm_unpacklo_epi16(highWords, zero), 24));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(out + column + 12),
                    _mm_slli_epi32(_mm_unpackhi_epi16(highWords, zero), 24));
        }
        for (; column < width; ++column) out[column] = static_cast<uint32_t>(sourceRow[column]) << 24;
        destinationRow += destination->lPitch;
        sourceRow += source->lPitch;
    }
}

}  // namespace


dk2::MySurface *__cdecl dk2::static_CBridge_loadPng(const char *name) {
    char Buffer[1024];
    if (!MyResources_instance.gameCfg.EnableArtPatching) {
        sprintf(Buffer, "Attempt To Load PNG without artpatch : %s", name);
        patch::log::dbg("%s", Buffer);
    }
    MySurface *surf = g_pCBridge->v_loadPng(name);
    if (patch::null_surf_fix::enabled) {
        if (surf == NULL) {
            surf = &patch::null_surf_fix::emptySurf;
            patch::log::dbg("[fix] tried to load \"%s\" PNG but NULL returned. replace with empty texture", name);
        }
    }
    return surf;
}


dk2::MySurface *__cdecl dk2::MySurface_blend24bit(MySurface *destination, MySurface *source) {
    if (source->size.w <= 0 || source->size.h <= 0) return destination;
    const uint32_t width = static_cast<uint32_t>(source->size.w) & ~1u;
    const Common16Format format = common16Format(destination->desc);
    const PairPackSpec red = makePairPackSpec(destination->desc.dwRBitMask);
    const PairPackSpec green = makePairPackSpec(destination->desc.dwGBitMask);
    const PairPackSpec blue = makePairPackSpec(destination->desc.dwBBitMask);
    const uint32_t opaqueAlpha = destination->desc.dwRGBAlphaBitMask |
            (destination->desc.dwRGBAlphaBitMask << 16);
    auto *destinationRow = static_cast<uint8_t *>(destination->lpSurface);
    const auto *sourceRow = static_cast<const uint8_t *>(source->lpSurface);
    for (int row = 0; row < source->size.h; ++row) {
        uint32_t column = 0;
        if (format != Common16Format::none) {
            for (; column + 8 <= width; column += 8) {
                const __m128i first = loadRgb4(sourceRow + column * 3);
                const __m128i second = loadRgb4(sourceRow + column * 3 + 12);
                _mm_storeu_si128(reinterpret_cast<__m128i *>(destinationRow + column * 2),
                        packCommon16(first, second, format, false));
            }
        }
        for (; column < width; column += 2) {
            const uint8_t *pixels = sourceRow + column * 3;
            const uint32_t packed = opaqueAlpha |
                    packPair(pixels[0], pixels[3], red) |
                    packPair(pixels[1], pixels[4], green) |
                    packPair(pixels[2], pixels[5], blue);
            storeU32(destinationRow + column * 2, packed);
        }
        destinationRow += destination->lPitch;
        sourceRow += source->lPitch;
    }
    return destination;
}


dk2::MySurface *__cdecl dk2::MySurface_blend8bit(MySurface *destination, MySurface *source) {
    if (source->size.w <= 0 || source->size.h <= 0) return destination;
    const uint32_t width = static_cast<uint32_t>(source->size.w) & ~1u;
    const PairPackSpec alpha = makePairPackSpec(destination->desc.dwRGBAlphaBitMask);
    auto *destinationRow = static_cast<uint8_t *>(destination->lpSurface);
    const auto *sourceRow = static_cast<const uint8_t *>(source->lpSurface);
    for (int row = 0; row < source->size.h; ++row) {
        for (uint32_t column = 0; column < width; column += 2) {
            storeU32(destinationRow + column * 2,
                    packPair(sourceRow[column], sourceRow[column + 1], alpha));
        }
        destinationRow += destination->lPitch;
        sourceRow += source->lPitch;
    }
    return destination;
}


dk2::MySurface *__cdecl dk2::MySurface_blend32bit(MySurface *destination, MySurface *source) {
    if (source->size.w <= 0 || source->size.h <= 0) return destination;
    const uint32_t width = static_cast<uint32_t>(source->size.w) & ~1u;
    const Common16Format format = common16Format(destination->desc);
    const PairPackSpec red = makePairPackSpec(destination->desc.dwRBitMask);
    const PairPackSpec green = makePairPackSpec(destination->desc.dwGBitMask);
    const PairPackSpec blue = makePairPackSpec(destination->desc.dwBBitMask);
    const PairPackSpec alpha = makePairPackSpec(destination->desc.dwRGBAlphaBitMask);
    auto *destinationRow = static_cast<uint8_t *>(destination->lpSurface);
    const auto *sourceRow = static_cast<const uint8_t *>(source->lpSurface);
    for (int row = 0; row < source->size.h; ++row) {
        uint32_t column = 0;
        if (format != Common16Format::none) {
            for (; column + 8 <= width; column += 8) {
                const __m128i first = _mm_loadu_si128(
                        reinterpret_cast<const __m128i *>(sourceRow + column * 4));
                const __m128i second = _mm_loadu_si128(
                        reinterpret_cast<const __m128i *>(sourceRow + column * 4 + 16));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(destinationRow + column * 2),
                        packCommon16(first, second, format, true));
            }
        }
        for (; column < width; column += 2) {
            const uint8_t *pixels = sourceRow + column * 4;
            const uint32_t packed =
                    packPair(pixels[0], pixels[4], red) |
                    packPair(pixels[1], pixels[5], green) |
                    packPair(pixels[2], pixels[6], blue) |
                    packPair(pixels[3], pixels[7], alpha);
            storeU32(destinationRow + column * 2, packed);
        }
        destinationRow += destination->lPitch;
        sourceRow += source->lPitch;
    }
    return destination;
}


dk2::MySurface *__cdecl dk2::MySurface_blend(MySurface *destination, MySurface *source) {
    if (destination->desc.dwRGBBitCount == 32) {
        switch (source->desc.dwRGBBitCount) {
        case 32:
            copyRows(destination, source, 4);
            break;
        case 24:
            rgb24ToRgba32Tight(destination, source);
            break;
        case 8:
            if (source->desc.dwRGBAlphaBitMask == 0xFF) alpha8ToRgba32(destination, source);
            break;
        default:
            break;
        }
        return destination;
    }

    switch (source->desc.dwRGBBitCount) {
    case 16:
        copyRows(destination, source, 2);
        break;
    case 24:
        MySurface_blend24bit(destination, source);
        break;
    case 32:
        MySurface_blend32bit(destination, source);
        break;
    case 8:
        if (source->desc.dwRGBAlphaBitMask == 0xFF) MySurface_blend8bit(destination, source);
        break;
    default:
        break;
    }
    return destination;
}
