#ifndef DK2_METAL_OVERLAY_UNMATTE_H
#define DK2_METAL_OVERLAY_UNMATTE_H

#include <cstdint>

#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define DK2M_OVERLAY_SSE2 1
#endif

namespace gog::metal_bridge {

inline void unmatteOverlayPixel(
        const uint8_t *black, const uint8_t *white, uint8_t *output) {
    const int d0 = static_cast<int>(white[0]) - black[0];
    const int d1 = static_cast<int>(white[1]) - black[1];
    const int d2 = static_cast<int>(white[2]) - black[2];
    const int minDiff = d0 < d1 ? (d0 < d2 ? d0 : d2) : (d1 < d2 ? d1 : d2);
    const int maxDiff = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
    const int transparency = d0 + d1 + d2 - minDiff - maxDiff;
    const int alpha = 255 - (transparency < 0 ? 0 : (transparency > 255 ? 255 : transparency));
    output[3] = static_cast<uint8_t>(alpha);
    if (alpha == 0) {
        output[0] = output[1] = output[2] = 0;
        return;
    }
    if (alpha == 255) {
        output[0] = black[0];
        output[1] = black[1];
        output[2] = black[2];
        return;
    }
    for (int channel = 0; channel < 3; ++channel) {
        const int straight = (black[channel] * 255 + alpha / 2) / alpha;
        output[channel] = static_cast<uint8_t>(straight > 255 ? 255 : straight);
    }
}

inline void unmatteOverlaySpan(const uint8_t *black, const uint8_t *white,
                               uint8_t *output, uint32_t pixelCount) {
    uint32_t pixel = 0;
#if DK2M_OVERLAY_SSE2
    const __m128i transparentBlack = _mm_set1_epi32(static_cast<int>(0xFF000000u));
    const __m128i transparentWhite = _mm_set1_epi32(-1);
    const __m128i zero = _mm_setzero_si128();
    for (; pixel + 4 <= pixelCount; pixel += 4) {
        const __m128i b = _mm_loadu_si128(
            reinterpret_cast<const __m128i *>(black + pixel * 4));
        const __m128i w = _mm_loadu_si128(
            reinterpret_cast<const __m128i *>(white + pixel * 4));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(b, transparentBlack)) == 0xFFFF &&
            _mm_movemask_epi8(_mm_cmpeq_epi8(w, transparentWhite)) == 0xFFFF) {
            _mm_storeu_si128(reinterpret_cast<__m128i *>(output + pixel * 4), zero);
            continue;
        }
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(b, w)) == 0xFFFF) {
            _mm_storeu_si128(reinterpret_cast<__m128i *>(output + pixel * 4), b);
            continue;
        }
        for (uint32_t lane = 0; lane < 4; ++lane) {
            unmatteOverlayPixel(black + (pixel + lane) * 4,
                                white + (pixel + lane) * 4,
                                output + (pixel + lane) * 4);
        }
    }
#endif
    for (; pixel < pixelCount; ++pixel) {
        unmatteOverlayPixel(black + pixel * 4, white + pixel * 4, output + pixel * 4);
    }
}

}

#undef DK2M_OVERLAY_SSE2

#endif
