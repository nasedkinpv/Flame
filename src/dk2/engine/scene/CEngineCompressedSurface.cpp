//
// Created by Flametal contributor.
//
// dk2::CEngineCompressedSurface::copySurf (00590740) is the load/decode path
// for world/terrain/creature/room textures: MyTextures::loadCompressed(name)
// (00591070, still original/untranslated) looks `name` up in
// MyTextures::texNameToFileOffsetMap and returns a lazy CEngineCompressedSurface
// wrapping the still-compressed (JPEG/tqia) block read from EngineTextures.dat
// -- see MyCESurfHandle::resolveSurface() in MyCESurfHandle.cpp, which stores
// it into this->cesurf same as any other handle. Unlike a plain CEngineSurface,
// a CEngineCompressedSurface's v_hasBuf/v_lockBuf are stub no-ops (both bound
// to 00402AD0): you cannot read its pixels directly, only decompress it
// straight into some other surface's buffer via copySurf()/paintSurf().
//
// The actual call site is CEngineSurfaceBase::paintSurf (00590450, still
// original/untranslated), the "double dispatch" wrapper every
// `dst->paintSurf(src, x, y)` call in this codebase goes through: it first
// tries dst's own v_paintSurf (a no-op stub for plain surfaces), then
// `src->v_copySurf(dst, x, y)` -- which for a CEngineCompressedSurface is
// this function -- and only if that also does nothing does it fall back to a
// raw memcpy blit assuming `src` already has decoded pixels. Because
// CEngineSurfaceBase's vtable slot for v_copySurf is bound to this exact
// address (00590740), patching this one function is enough: the original,
// still-untranslated paintSurf() reaches it through the (unmodified) vtable
// regardless of whether the call was virtual or direct.
//
// Decode-wise this reimplements the original 1:1: tqia_init() configures the
// tqia JPEG decompressor's target pixel format from `dst`'s MyCEngineSurfDesc
// mask fields, then tqia_decompressJpeg() decodes this surface's compressed
// block (this->pixelBuf, cached raw bytes read from EngineTextures.dat by
// loadCompressed) directly into dst's locked buffer at (x, y). That write is
// the only point these pixels ever exist in decoded form before landing in
// the shared atlas/page surface, so the named texture dump hook (see
// TextureDump.h) has to sit right here, immediately after the decode and
// before v_unlockBuf. This function itself doesn't know its own resource
// name (CEngineCompressedSurface carries none) -- see
// patch::texture_dump::setCompositeSourceName(), which the already-translated
// callers (MyCESurfHandle::paint, MyCESurfHandle::loadPrescaled,
// SurfHashList::expandPut) set immediately before invoking paintSurf() on a
// handle whose name is known.

#include "dk2/CEngineCompressedSurface.h"
#include "dk2/MyCEngineSurfDesc.h"
#include "dk2/MySurface.h"
#include "dk2/TextureDump.h"
#include "dk2/utils/Size2i.h"
#include "dk2_functions.h"

#include <cstdint>
#include <cstring>

int dk2::CEngineCompressedSurface::copySurf(CEngineSurfaceBase *dst, int x, int y) {
    if (!dst->v_hasBuf()) return 0;

    void *locked = dst->v_lockBuf();
    auto *dstPtr = static_cast<uint8_t *>(locked)
            + static_cast<size_t>(y) * dst->lineWidth
            + static_cast<size_t>(x) * dst->fC_desc->bytesize;

    dk2::tqia_init(dst->fC_desc->_rmask, dst->fC_desc->_gmask, dst->fC_desc->_bmask, dst->fC_desc->_amask);
    dk2::tqia_decompressJpeg(
            (uint8_t *) this->pixelBuf,
            reinterpret_cast<uint16_t *>(dstPtr),
            dst->lineWidth,
            static_cast<uint32_t>(this->width),
            static_cast<uint32_t>(this->height));

    // Named texture dump hook (see TextureDump.h / patch::texture_dump).
    // No-op unless flametal:TextureDump is set.
    {
        Size2i sz{this->width, this->height};
        MySurfDesc desc;
        memcpy(&desc, reinterpret_cast<const uint8_t *>(dst->fC_desc) + 0x2D, sizeof(desc));
        MySurface local;  // the original never destroys it either (see MyCESurfHandle::paint)
        // Pass dst->lineWidth explicitly: this rect is a sub-region of a
        // shared atlas page, so its real row stride is the page's stride,
        // not width*bytesPerPixel (constructor(...,0) would derive the
        // latter and read the wrong rows for anything but a 1:1 page).
        local.constructor(&sz, &desc, dstPtr, dst->lineWidth);
        patch::texture_dump::onCompositedSurfaceDecoded(&local);
    }

    dst->v_unlockBuf((int) locked);
    return 1;
}
