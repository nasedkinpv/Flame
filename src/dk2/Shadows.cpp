#include "dk2_functions.h"
#include "dk2_globals.h"

#include <algorithm>
#include <cmath>
#include <cstdint>


namespace {

struct Point2i {
    int x;
    int y;
};

void addCoverage(uint8_t *pixel, int subpixels) {
    const int value = static_cast<int>(*pixel) + subpixels * 4;
    *pixel = static_cast<uint8_t>(value > 255 ? 255 : value);
}

void rasterizeSubpixelRow(
        uint8_t *row,
        const Point2i (&points)[3],
        int subpixelY) {
    const double sampleY = static_cast<double>(subpixelY) + 0.5;
    double intersections[3];
    int count = 0;
    for (int edge = 0; edge < 3; ++edge) {
        const Point2i &a = points[edge];
        const Point2i &b = points[(edge + 1) % 3];
        const double low = std::min(a.y, b.y);
        const double high = std::max(a.y, b.y);
        if (!(sampleY >= low && sampleY < high)) continue;

        const double t = (sampleY - static_cast<double>(a.y))
                       / static_cast<double>(b.y - a.y);
        intersections[count++] = static_cast<double>(a.x)
                               + t * static_cast<double>(b.x - a.x);
    }
    if (count < 2) return;

    std::sort(intersections, intersections + count);
    int first = static_cast<int>(std::ceil(intersections[0] - 0.5));
    int last = static_cast<int>(std::ceil(intersections[count - 1] - 0.5)) - 1;
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
    for (int pixel = firstPixel + 1; pixel < lastPixel; ++pixel) {
        addCoverage(row + pixel, 8);
    }
    addCoverage(row + lastPixel, (last & 7) + 1);
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

    const Point2i points[3]{{x0, y0}, {x1, y1}, {x2, y2}};
    const int minY = std::min({y0, y1, y2});
    const int maxY = std::max({y0, y1, y2});
    int firstRow = static_cast<int>(std::ceil(static_cast<double>(minY) - 0.5));
    int lastRow = static_cast<int>(std::ceil(static_cast<double>(maxY) - 0.5)) - 1;
    firstRow = std::max(firstRow, 0);
    lastRow = std::min(lastRow, 255);
    for (int subpixelY = firstRow; subpixelY <= lastRow; ++subpixelY) {
        rasterizeSubpixelRow(
                surface + (subpixelY >> 3) * stride,
                points,
                subpixelY);
    }
    return 0;
}
