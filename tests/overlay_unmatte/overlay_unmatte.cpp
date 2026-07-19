#include "gog_patch_dll/metal_bridge/OverlayUnmatte.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

uint8_t composite(uint8_t foreground, uint8_t alpha, uint8_t background) {
    return static_cast<uint8_t>(
        (foreground * alpha + background * (255 - alpha) + 127) / 255);
}

}

int main() {
    const uint8_t colors[][3] = {{0, 0, 0}, {255, 255, 255}, {24, 96, 220}, {255, 48, 192}};
    const uint8_t alphas[] = {0, 32, 127, 224, 255};
    for (const auto &color : colors) {
        for (uint8_t alpha : alphas) {
            uint8_t black[4] = {}, white[4] = {};
            for (int channel = 0; channel < 3; ++channel) {
                black[channel] = composite(color[channel], alpha, 0);
                white[channel] = composite(color[channel], alpha, 255);
            }
            uint8_t actual[4] = {};
            gog::metal_bridge::unmatteOverlayPixel(black, white, actual);
            assert(actual[3] >= alpha - (alpha != 0));
            assert(actual[3] <= alpha + (alpha != 255));
            if (alpha >= 32) {
                for (int channel = 0; channel < 3; ++channel) {
                    const int delta = static_cast<int>(actual[channel]) - color[channel];
                    assert(delta >= -4 && delta <= 4);
                }
            }
        }
    }
    std::puts("OK: two-matte overlay recovery");
}
