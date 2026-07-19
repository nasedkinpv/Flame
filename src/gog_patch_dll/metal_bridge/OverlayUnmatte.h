#ifndef DK2_METAL_OVERLAY_UNMATTE_H
#define DK2_METAL_OVERLAY_UNMATTE_H

#include <cstdint>

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
    for (int channel = 0; channel < 3; ++channel) {
        const int straight = alpha ? (black[channel] * 255 + alpha / 2) / alpha : 0;
        output[channel] = static_cast<uint8_t>(straight > 255 ? 255 : straight);
    }
}

}

#endif
