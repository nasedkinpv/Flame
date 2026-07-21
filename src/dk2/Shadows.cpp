#include "dk2/ShadowGpu.h"
#include "dk2/CEngineDDSurface.h"
#include "dk2/MyCESurfHandle.h"
#include "dk2/MyCESurfScale.h"
#include "dk2/MyScaledSurface.h"
#include "dk2/MySurface.h"
#include "dk2/SurfaceHolder.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <metal_bridge/DK2BridgeProtocol.h>
#include <metal_bridge/MetalBridgeProducer.h>

#include <algorithm>
#include <cstdint>
#include <emmintrin.h>
#include <limits>
#include <vector>


namespace {

struct Point2i {
    int x;
    int y;
};

struct GpuShadowBatch {
    int surfaceIndex = -1;
    bool started = false;
    bool gpu = false;
    std::vector<DK2MShadowTriangle> triangles;
};

GpuShadowBatch g_gpuBatch;

dk2::MyCESurfHandle *currentShadowHandle() {
    const int index = dk2::shadows_dword_780E6C;
    if (index < 0 || index >= dk2::MyEntryBuf_MyScaledSurface_instance.count) return nullptr;
    dk2::MyScaledSurface *surface = dk2::MyEntryBuf_MyScaledSurface_getByIdx(index);
    if (!surface || !surface->scaledSurfArr) return nullptr;
    return surface->scaledSurfArr->surfScaledArr[0];
}

bool usableGpuTarget(dk2::MyCESurfHandle *handle) {
    if (!handle || handle->surfWidth8 != 32 || handle->surfHeight8 != 32) return false;
    dk2::SurfaceHolder *holder = handle->holder_parent;
    if (!holder || !holder->a3 || !holder->surf) return false;
    auto *page = reinterpret_cast<dk2::CEngineDDSurface *>(holder->surf);
    return page->ddSurf != nullptr;
}

void beginGpuBatchIfNeeded() {
    const int surfaceIndex = dk2::shadows_dword_780E6C;
    if (g_gpuBatch.started && g_gpuBatch.surfaceIndex == surfaceIndex) return;
    g_gpuBatch.surfaceIndex = surfaceIndex;
    g_gpuBatch.started = true;
    g_gpuBatch.gpu = gog::metal_bridge::metalShadowsEnabled() &&
                     usableGpuTarget(currentShadowHandle());
    g_gpuBatch.triangles.clear();
}

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

void rasterizeCpuTriangle(const DK2MShadowTriangle &triangle) {
    auto *surface = static_cast<uint8_t *>(dk2::shadows_lpSurface);
    const int stride = dk2::shadows_dword_780A64;
    if (surface == nullptr || stride < 32) return;

    Point2i points[3]{{triangle.x0, triangle.y0},
                      {triangle.x1, triangle.y1},
                      {triangle.x2, triangle.y2}};
    std::sort(points, points + 3, [](const Point2i &left, const Point2i &right) {
        return left.y < right.y;
    });
    rasterizeSegment(surface, stride, points[0], points[1], points[0], points[2]);
    rasterizeSegment(surface, stride, points[1], points[2], points[0], points[2]);
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
    beginGpuBatchIfNeeded();
    const DK2MShadowTriangle triangle{x0, y0, x1, y1, x2, y2};
    if (g_gpuBatch.gpu) {
        g_gpuBatch.triangles.push_back(triangle);
        return 0;
    }
    rasterizeCpuTriangle(triangle);
    return 0;
}

bool dk2::shadowgpu::active() {
    return gog::metal_bridge::metalShadowsEnabled();
}

bool dk2::shadowgpu::finishIfCurrent(MyCESurfHandle *handle, const MySurface *source) {
    if (!handle || currentShadowHandle() != handle) return false;

    const int surfaceIndex = dk2::shadows_dword_780E6C;
    bool gpu = g_gpuBatch.started && g_gpuBatch.surfaceIndex == surfaceIndex
        ? g_gpuBatch.gpu
        : active() && usableGpuTarget(handle);
    // A live off (or host heartbeat loss) can land between triangle capture
    // and this paint. Rebuild the blank CPU scratch from the queued geometry
    // so that exact frame falls back instead of briefly dropping the shadow.
    if (gpu && !active()) {
        for (const DK2MShadowTriangle &triangle : g_gpuBatch.triangles) {
            rasterizeCpuTriangle(triangle);
        }
        gpu = false;
    }
    if (gpu) {
        const uint32_t mode = source && source->desc.dwRGBBitCount == 8 &&
                                      source->desc.dwRGBAlphaBitMask == 0xFF
            ? DK2M_SHADOW_MASK_ALPHA
            : DK2M_SHADOW_MASK_GRAYSCALE;
        const DK2MShadowTriangle *triangles = g_gpuBatch.triangles.empty()
            ? nullptr : g_gpuBatch.triangles.data();
        gog::metal_bridge::shadowMask(
            handle, triangles, static_cast<uint32_t>(g_gpuBatch.triangles.size()), mode);
    }
    g_gpuBatch.started = false;
    g_gpuBatch.surfaceIndex = -1;
    g_gpuBatch.gpu = false;
    g_gpuBatch.triangles.clear();
    return gpu;
}

bool dk2::shadowgpu::resolveTarget(const void *handleKey, const void *boundSurface,
                                  TargetRegion *out) {
    if (!handleKey || !boundSurface || !out) return false;
    __try {
        auto *handle = reinterpret_cast<MyCESurfHandle *>(
            const_cast<void *>(handleKey));
        SurfaceHolder *holder = handle->holder_parent;
        if (!holder || !holder->a3 || !holder->surf) return false;
        auto *page = reinterpret_cast<CEngineDDSurface *>(holder->surf);
        if (page->ddSurf != boundSurface || handle->surfWidth8 == 0 ||
            handle->surfHeight8 == 0) return false;
        const uint32_t x = handle->x8;
        const uint32_t y = handle->y8;
        const uint32_t width = handle->surfWidth8;
        const uint32_t height = handle->surfHeight8;
        if (x + width > static_cast<uint32_t>(holder->surf->width) ||
            y + height > static_cast<uint32_t>(holder->surf->height)) return false;
        *out = {x, y, width, height};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
