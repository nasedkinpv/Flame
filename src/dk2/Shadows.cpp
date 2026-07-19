#include "dk2_functions.h"
#include "dk2_globals.h"

#include <algorithm>
#include <cstdint>
#include <emmintrin.h>
#include <limits>


namespace {

struct Point2i {
    int x;
    int y;
};

void addCoverage(uint8_t *pixel, int subpixels) {
    const int value = static_cast<int>(*pixel) + subpixels * 4;
    *pixel = static_cast<uint8_t>(value > 255 ? 255 : value);
}

int ceilToInt(double value) {
    if (value <= std::numeric_limits<int>::min()) {
        return std::numeric_limits<int>::min();
    }
    if (value >= std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    const int truncated = static_cast<int>(value);
    return truncated + (static_cast<double>(truncated) < value ? 1 : 0);
}

void addFullPixels(uint8_t *first, uint8_t *last) {
    const __m128i increment = _mm_set1_epi8(32);
    while (first + 16 <= last) {
        const __m128i pixels = _mm_loadu_si128(
                reinterpret_cast<const __m128i *>(first));
        _mm_storeu_si128(
                reinterpret_cast<__m128i *>(first),
                _mm_adds_epu8(pixels, increment));
        first += 16;
    }
    while (first != last) {
        addCoverage(first++, 8);
    }
}

void rasterizeSpan(uint8_t *row, double firstX, double secondX) {
    const double left = std::min(firstX, secondX);
    const double right = std::max(firstX, secondX);
    if (right < -0.5 || left > 255.5) return;

    int first = ceilToInt(left - 0.5);
    int last = ceilToInt(right - 0.5) - 1;
    first = std::max(first, 0);
    last = std::min(last, 255);
    if (first > last) return;

    const int firstPixel = first >> 3;
    const int lastPixel = last >> 3;
    if (firstPixel == lastPixel) {
        addCoverage(row + firstPixel, last - first + 1);
        return;
    }

    addCoverage(row + firstPixel, 8 - (first & 7));
    addFullPixels(row + firstPixel + 1, row + lastPixel);
    addCoverage(row + lastPixel, (last & 7) + 1);
}

void rasterizeSegment(
        uint8_t *surface,
        int stride,
        const Point2i &top,
        const Point2i &bottom,
        const Point2i &longTop,
        const Point2i &longBottom) {
    if (top.y == bottom.y || longTop.y == longBottom.y) return;
    const int firstRow = std::max(top.y, 0);
    const int lastRow = std::min(bottom.y - 1, 255);
    if (firstRow > lastRow) return;

    const double shortStep = static_cast<double>(bottom.x - top.x) /
                             static_cast<double>(bottom.y - top.y);
    const double longStep = static_cast<double>(longBottom.x - longTop.x) /
                            static_cast<double>(longBottom.y - longTop.y);
    const double firstSampleY = static_cast<double>(firstRow) + 0.5;
    double shortX = static_cast<double>(top.x) +
            (firstSampleY - static_cast<double>(top.y)) * shortStep;
    double longX = static_cast<double>(longTop.x) +
            (firstSampleY - static_cast<double>(longTop.y)) * longStep;
    for (int subpixelY = firstRow; subpixelY <= lastRow; ++subpixelY) {
        rasterizeSpan(surface + (subpixelY >> 3) * stride, shortX, longX);
        shortX += shortStep;
        longX += longStep;
    }
}

}  // namespace


// DK2's original shadow rasterizer assumes every projected coordinate stays
// inside its 32x32, 8x-supersampled target.  At the maximum shadow detail the
// high-resolution camera can violate that assumption: the reciprocal-table
// lookup runs out of bounds, and a later span can overwrite the surface index
// immediately following shadows_surfaceData.  Rasterize the same coverage
// representation while clipping every read and write to the actual target.
int __cdecl dk2::shadows_process_58E080(
        int x0, int y0,
        int x1, int y1,
        int x2, int y2) {
    auto *surface = static_cast<uint8_t *>(shadows_lpSurface);
    const int stride = shadows_dword_780A64;
    if (surface == nullptr || stride < 32) return 0;

    Point2i points[3]{{x0, y0}, {x1, y1}, {x2, y2}};
    std::sort(points, points + 3, [](const Point2i &left, const Point2i &right) {
        return left.y < right.y;
    });
    rasterizeSegment(surface, stride, points[0], points[1], points[0], points[2]);
    rasterizeSegment(surface, stride, points[1], points[2], points[0], points[2]);
    return 0;
}
