#include "DK2TextureScale.h"

#import <MetalPerformanceShaders/MetalPerformanceShaders.h>

#include <cstring>

namespace dk2scale {
namespace {

// Per-device singleton: a dedicated command queue and a reusable
// MPSImageBilinearScale filter. The renderer draws on a separate MTL4 queue, so
// this classic-Metal queue never contends with it; correctness across the two
// queues is guaranteed by waiting on each compose command buffer before its
// result texture is handed back (see composeHDPage / scaleBgra).
struct Scaler {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    MPSImageBilinearScale *filter = nil;
};

Scaler &scaler(id<MTLDevice> device) {
    static Scaler s;
    if (s.device != device) {
        s.device = device;
        s.queue = [device newCommandQueue];
        s.queue.label = @"DK2 texture scale";
        s.filter = [[MPSImageBilinearScale alloc] initWithDevice:device];
    }
    return s;
}

}  // namespace

id<MTLTexture> uploadBgra(id<MTLDevice> device, const uint8_t *bgra,
                          uint32_t width, uint32_t height, uint32_t pitch) {
    if (!device || !bgra || !width || !height) return nil;
    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:width
                                    height:height
                                 mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
    if (!tex) return nil;
    [tex replaceRegion:MTLRegionMake2D(0, 0, width, height)
           mipmapLevel:0
             withBytes:bgra
           bytesPerRow:pitch];
    return tex;
}

id<MTLTexture> composeHDPage(id<MTLDevice> device, id<MTLTexture> base,
                             uint32_t hdW, uint32_t hdH,
                             const std::vector<HDRect> &rects, uint64_t label) {
    if (!device || !base || !hdW || !hdH) return nil;
    Scaler &s = scaler(device);
    if (!s.queue || !s.filter) return nil;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:hdW
                                    height:hdH
                                 mipmapped:YES];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> dst = [device newTextureWithDescriptor:desc];
    if (!dst) return nil;
    dst.label = [NSString stringWithFormat:@"DK2 pack HD %016llx", label];

    id<MTLCommandBuffer> cb = [s.queue commandBuffer];

    // Full-page upscale: no transform + no clip => MPS scales `base` to cover
    // the entire destination (hdW x hdH == baseW*kScale x baseH*kScale).
    s.filter.scaleTransform = NULL;
    s.filter.clipRect = MPSRectNoClip;
    [s.filter encodeToCommandBuffer:cb sourceTexture:base destinationTexture:dst];

    // Each pack rect: scale its source to fill (dstW x dstH) at (dstX, dstY).
    // MPS maps destination(x,y) = scale*source(x,y) + translate, and clipRect
    // restricts the written region to exactly this rect.
    for (const auto &r : rects) {
        if (!r.src || !r.dstW || !r.dstH) continue;
        MPSScaleTransform tf;
        tf.scaleX = (double)r.dstW / (double)r.src.width;
        tf.scaleY = (double)r.dstH / (double)r.src.height;
        tf.translateX = (double)r.dstX;
        tf.translateY = (double)r.dstY;
        s.filter.scaleTransform = &tf;
        s.filter.clipRect = (MTLRegion){{r.dstX, r.dstY, 0}, {r.dstW, r.dstH, 1}};
        [s.filter encodeToCommandBuffer:cb sourceTexture:r.src destinationTexture:dst];
    }
    s.filter.scaleTransform = NULL;

    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit generateMipmapsForTexture:dst];
    [blit endEncoding];

    [cb commit];
    [cb waitUntilCompleted];  // result is sampled from the renderer's MTL4 queue
    return dst;
}

bool scaleBgra(id<MTLDevice> device, const uint8_t *src, uint32_t srcW,
               uint32_t srcH, uint32_t srcPitch, uint8_t *dst, uint32_t dstW,
               uint32_t dstH, uint32_t dstPitch) {
    if (!device || !src || !dst || !srcW || !srcH || !dstW || !dstH) return false;
    if (srcW == dstW && srcH == dstH) {
        for (uint32_t y = 0; y < dstH; ++y) {
            std::memcpy(dst + (size_t)y * dstPitch, src + (size_t)y * srcPitch,
                        (size_t)dstW * 4);
        }
        return true;
    }
    Scaler &s = scaler(device);
    if (!s.queue || !s.filter) return false;

    id<MTLTexture> srcTex = uploadBgra(device, src, srcW, srcH, srcPitch);
    if (!srcTex) return false;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:dstW
                                    height:dstH
                                 mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;  // read back with getBytes
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> dstTex = [device newTextureWithDescriptor:desc];
    if (!dstTex) return false;

    id<MTLCommandBuffer> cb = [s.queue commandBuffer];
    s.filter.scaleTransform = NULL;
    s.filter.clipRect = MPSRectNoClip;
    [s.filter encodeToCommandBuffer:cb sourceTexture:srcTex destinationTexture:dstTex];
    [cb commit];
    [cb waitUntilCompleted];

    [dstTex getBytes:dst
         bytesPerRow:dstPitch
          fromRegion:MTLRegionMake2D(0, 0, dstW, dstH)
         mipmapLevel:0];
    return true;
}

}  // namespace dk2scale
