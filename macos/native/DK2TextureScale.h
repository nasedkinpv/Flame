// GPU-accelerated BGRA image scaling / HD atlas-page composition.
//
// Replaces the hand-rolled CPU nested-loop bilinear scaler that an Instruments
// Time Profiler trace of the live DK2Metal host showed as the dominant CPU
// cost (respack::scaleBilinearBgra: 78 of ~200 samples in a 12s window, well
// above actual Metal/AGX render encoding). All work runs on the GPU via
// MetalPerformanceShaders (MPSImageBilinearScale) plus a blit-encoder mip pass
// (generateMipmapsForTexture:), per Apple's "Optimizing texture data" guidance:
// touch pixel data on the CPU only at upload, let the GPU do every transform.
#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <vector>

namespace dk2scale {

// A single pack-art rect composited onto the HD page. `src` is a source
// texture (upload it with uploadBgra) sampled to fill the destination
// rectangle, given in HD-page (destination) pixels.
struct HDRect {
    id<MTLTexture> src;
    uint32_t dstX, dstY, dstW, dstH;
};

// Uploads a tight-or-pitched BGRA8 buffer into a fresh Shared, ShaderRead
// source texture ready to be handed to composeHDPage / used with MPS. Returns
// nil on allocation failure. The returned texture owns a private copy of the
// pixels, so the caller's buffer may be freed/evicted immediately after.
id<MTLTexture> uploadBgra(id<MTLDevice> device, const uint8_t *bgra,
                          uint32_t width, uint32_t height, uint32_t pitch);

// Composes a mipmapped HD atlas page entirely on the GPU: MPSImageBilinearScale
// upscales `base` to fill the whole hdW x hdH destination, then scales each
// rect's source into its destination sub-rectangle, then a blit encoder builds
// the mip chain. Returns a Private, ShaderRead, mipmapped BGRA8 texture (nil on
// failure). Synchronous: the command buffer is waited on before returning, so
// the result is safe to sample from any other queue (the renderer uses a
// separate MTL4 queue, so cross-queue hazard tracking does not apply).
id<MTLTexture> composeHDPage(id<MTLDevice> device, id<MTLTexture> base,
                             uint32_t hdW, uint32_t hdH,
                             const std::vector<HDRect> &rects, uint64_t label);

// Straight GPU bilinear resample of a BGRA8 buffer into a caller-provided BGRA8
// buffer. Used by the rare shadow-atlas promotion path; equal dimensions fall
// back to a row copy. Returns false on failure.
bool scaleBgra(id<MTLDevice> device, const uint8_t *src, uint32_t srcW,
               uint32_t srcH, uint32_t srcPitch, uint8_t *dst, uint32_t dstW,
               uint32_t dstH, uint32_t dstPitch);

}  // namespace dk2scale
