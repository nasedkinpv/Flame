#include "gog_patch_dll/metal_bridge/OverlayUnmatte.h"

#include <cassert>
#include <array>
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

    std::array<uint8_t, 64> black = {}, white = {}, scalar = {}, span = {};
    for (size_t pixel = 0; pixel < black.size() / 4; ++pixel) {
        const auto &color = colors[pixel % (sizeof(colors) / sizeof(colors[0]))];
        const uint8_t alpha = alphas[pixel % (sizeof(alphas) / sizeof(alphas[0]))];
        for (int channel = 0; channel < 3; ++channel) {
            black[pixel * 4 + channel] = composite(color[channel], alpha, 0);
            white[pixel * 4 + channel] = composite(color[channel], alpha, 255);
        }
        black[pixel * 4 + 3] = white[pixel * 4 + 3] = 255;
        gog::metal_bridge::unmatteOverlayPixel(
            black.data() + pixel * 4, white.data() + pixel * 4,
            scalar.data() + pixel * 4);
    }
    gog::metal_bridge::unmatteOverlaySpan(
        black.data(), white.data(), span.data(), black.size() / 4);
    assert(span == scalar);
    std::puts("OK: two-matte overlay recovery");
}
