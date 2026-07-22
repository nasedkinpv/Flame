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

#include "patches/logging.h"

#include <algorithm>
#include <cstdint>
#include <emmintrin.h>
#include <limits>
#include <unordered_map>
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

// Retained per-handle mask geometry, mirroring the engine's own design: the
// original keeps baked coverage in the handle's scratch surface and re-blits
// it on every repack repaint, so mask content survives layout changes and
// generation bumps without a rebake. The GPU path leaves the scratch blank,
// so retain the triangles here and RE-EMIT them (at the handle's current,
// already-updated placement -> fresh generation) whenever the engine
// repaints a handle we baked. Bounded by the ring + blob cache population.
// Lookups happen only with the handle as `this` inside paint(), so a stored
// pointer is never dereferenced stale.
struct RetainedMask {
    uint32_t mode = 0;
    std::vector<DK2MShadowTriangle> triangles;
};
std::unordered_map<dk2::MyCESurfHandle *, RetainedMask> g_retainedMasks;
int g_retainedPoolCount = -1;  // MyEntryBuf count; change = pool churn, drop all
uint32_t g_maskReemits = 0;

void retainedPoolGuard() {
    const int count = dk2::MyEntryBuf_MyScaledSurface_instance.count;
    if (count != g_retainedPoolCount) {
        g_retainedPoolCount = count;
        g_retainedMasks.clear();
    }
}

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
    // GPU shadows on = GPU-only by design: never rasterize CPU coverage into
    // the shared atlas page. A shadow that is not GPU-eligible (not 32x32, or
    // its page has no ddSurf) is simply absent for now - far better than the
    // hybrid, where CPU coverage written here into a ring-reused scratch got
    // re-blitted and decals sampled another object's silhouette ("shadows
    // from other objects"). Making the GPU path cover every shadow, and fixing
    // the decal redirect, is the remaining work; correctness (no stale
    // artifacts) comes first. Off (metal_shadows=false) keeps the full CPU path.
    if (gog::metal_bridge::metalShadowsEnabled()) return 0;
    rasterizeCpuTriangle(triangle);
    return 0;
}

bool dk2::shadowgpu::active() {
    return gog::metal_bridge::metalShadowsEnabled();
}

namespace {

void clearGpuBatch() {
    g_gpuBatch.started = false;
    g_gpuBatch.surfaceIndex = -1;
    g_gpuBatch.gpu = false;
    g_gpuBatch.triangles.clear();
}

}  // namespace

bool dk2::shadowgpu::finishIfCurrent(MyCESurfHandle *handle, const MySurface *source) {
    if (!handle || currentShadowHandle() != handle) {
        // Unrelated paints interleave with an in-flight bake (any surface
        // composition calls into here), so only drop the batch when the ring
        // has demonstrably moved past it - a stale batch would otherwise be
        // attributed to the NEXT bake that reuses its ring index.
        if (g_gpuBatch.started &&
            g_gpuBatch.surfaceIndex != dk2::shadows_dword_780E6C) {
            clearGpuBatch();
        }
        // Repack repaint of a handle we baked: the engine re-blits the
        // handle's scratch into its (possibly new) placement, but the GPU
        // path keeps that scratch blank. Re-emit the retained triangles at
        // the CURRENT placement - which also stamps the page's fresh
        // post-repack generation. Without this, every bake whose page
        // repacks later in the same frame is dropped as a stale generation
        // (measured 80% of masks on Level1), and the decals draw against an
        // empty twin: the residual shadow flicker.
        if (handle && active() && !g_retainedMasks.empty()) {
            const auto retained = g_retainedMasks.find(handle);
            if (retained != g_retainedMasks.end() && usableGpuTarget(handle)) {
                SurfaceHolder *holder = handle->holder_parent;
                auto *page = reinterpret_cast<CEngineDDSurface *>(holder->surf);
                const uint32_t x = handle->x8;
                const uint32_t y = handle->y8;
                const uint32_t width = handle->surfWidth8;
                const uint32_t height = handle->surfHeight8;
                if (x + width <= static_cast<uint32_t>(holder->surf->width) &&
                    y + height <= static_cast<uint32_t>(holder->surf->height)) {
                    const RetainedMask &mask = retained->second;
                    gog::metal_bridge::shadowMaskCaptured(
                        page->ddSurf, x, y, width, height,
                        mask.triangles.empty() ? nullptr : mask.triangles.data(),
                        static_cast<uint32_t>(mask.triangles.size()), mask.mode);
                    if ((++g_maskReemits % 500) == 1) {
                        patch::log::dbg("shadowgpu: repaint re-emits=%u retained=%u",
                                        g_maskReemits,
                                        (unsigned) g_retainedMasks.size());
                    }
                    return true;  // mask delivered; the blank CPU blit may proceed
                }
            }
        }
        return false;
    }

    const int surfaceIndex = dk2::shadows_dword_780E6C;
    const bool batchMatches =
        g_gpuBatch.started && g_gpuBatch.surfaceIndex == surfaceIndex;
    const std::vector<DK2MShadowTriangle> *captured =
        batchMatches ? &g_gpuBatch.triangles : nullptr;
    bool gpu = batchMatches ? g_gpuBatch.gpu : active() && usableGpuTarget(handle);
    // A live off (or host heartbeat loss) can land between triangle capture
    // and this paint. Rebuild the blank CPU scratch from the queued geometry
    // so that exact frame falls back instead of briefly dropping the shadow.
    if (gpu && !active()) {
        if (captured) {
            for (const DK2MShadowTriangle &triangle : *captured) {
                rasterizeCpuTriangle(triangle);
            }
        }
        gpu = false;
    }
    if (gpu) {
        // Immediate-mode contract: the target region is resolved HERE, while
        // the handle's placement is exactly the one this bake rasterized
        // for. Nothing downstream may re-resolve it (the ring recycles both
        // handles and placements, which is what glued stale silhouettes
        // under unrelated decals in the retained design).
        SurfaceHolder *holder = handle->holder_parent;
        CEngineDDSurface *page = holder && holder->a3
            ? reinterpret_cast<CEngineDDSurface *>(holder->surf) : nullptr;
        const uint32_t x = handle->x8;
        const uint32_t y = handle->y8;
        const uint32_t width = handle->surfWidth8;
        const uint32_t height = handle->surfHeight8;
        if (page && page->ddSurf && width && height &&
            x + width <= static_cast<uint32_t>(holder->surf->width) &&
            y + height <= static_cast<uint32_t>(holder->surf->height)) {
            const uint32_t mode = source && source->desc.dwRGBBitCount == 8 &&
                                          source->desc.dwRGBAlphaBitMask == 0xFF
                ? DK2M_SHADOW_MASK_ALPHA
                : DK2M_SHADOW_MASK_GRAYSCALE;
            const DK2MShadowTriangle *triangles = captured && !captured->empty()
                ? captured->data() : nullptr;
            const uint32_t triangleCount = captured
                ? static_cast<uint32_t>(captured->size()) : 0;
            gog::metal_bridge::shadowMaskCaptured(
                page->ddSurf, x, y, width, height, triangles, triangleCount, mode);
            // Retain for repack repaints (see the early-return path above).
            retainedPoolGuard();
            if (g_retainedMasks.size() > 256) g_retainedMasks.clear();
            RetainedMask &retained = g_retainedMasks[handle];
            retained.mode = mode;
            if (triangles) {
                retained.triangles.assign(triangles, triangles + triangleCount);
            } else {
                retained.triangles.clear();
            }
        } else {
            // No resolvable target this bake: fall back to the CPU raster so
            // the shadow is not silently dropped.
            if (captured) {
                for (const DK2MShadowTriangle &triangle : *captured) {
                    rasterizeCpuTriangle(triangle);
                }
            }
            gpu = false;
        }
    }
    clearGpuBatch();
    return gpu;
}
