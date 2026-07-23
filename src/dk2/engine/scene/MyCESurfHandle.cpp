//
// Created by DiaLight on 10/9/2025.
//

#include "dk2/MyCESurfHandle.h"
#include "dk2/MyStringHashMap_MyCESurfHandle_entry.h"
#include "dk2/MyStringHashMap_entry.h"
#include <metal_bridge/MetalBridgeProducer.h>
#include "dk2/CEngineDDSurface.h"

// The producer keys texture ids by IDirectDrawSurface4* for legacy pages
// (SetTexture path) but by the engine surface pointer for GPU-mesh pages
// (ensureBufferTexture) - report atlas rects under whichever key the page
// will upload with.
static const void *atlasPageKey(dk2::CEngineSurfaceBase *page) {
    if (page && *(void **) page == (void *) 0x6703C4) {
        IDirectDrawSurface4 *dd = static_cast<dk2::CEngineDDSurface *>(page)->ddSurf;
        if (dd) return dd;
    }
    return page;
}
#include "dk2/CEngineSurface.h"
#include "dk2/CEngineSurfaceScaler.h"
#include "dk2/MySurface.h"
#include "dk2/MySurfDesc.h"
#include "dk2/SurfaceHolder.h"
#include "dk2/MyDirectDraw.h"
#include "dk2/TextureDump.h"
#include "dk2/ShadowGpu.h"
#include <cstdint>
#include <cstring>
#include "dk2_functions.h"
#include "dk2_globals.h"
#include "patches/logging.h"


void dk2::MyCESurfHandle::resolveSurface() {
    if (this->cesurf != nullptr) return;
    if ((this->reductionLevel_andFlags & 0x80) != 0) {
        this->create();
        return;
    }
    char *f0_name = MyStringHashMap_MyCESurfHandle_instance.entries.buf[this->mapIdx].name;
    char texName[256];
    sprintf(texName, "%sMM%d", f0_name, this->reductionLevel_andFlags & 7);
    int f24_reductionLevel = MyDirectDraw_instance_devTexture.reductionLevel;
    if (this->surfWidth8) {
        if (MyDirectDraw_instance_devTexture.reductionLevel <= (this->reductionLevel_andFlags & 7) || !this->nextByReduction) {
            this->cesurf = (CEngineSurface*) MyTextures_instance.loadCompressed(texName);
            if (!this->cesurf) {
                int EntryIdx = ((MyStringHashMap *)&MyStringHashMap_MyCESurfHandle_instance)->getEntryIdx(f0_name);
                MyStringHashMap_MyCESurfHandle_instance.entries.buf[EntryIdx].value->loadPrescaled();
            }
        }
        return;
    }
    int v5 = MyTextures_instance.texNameToFileOffsetMap.getEntryIdx(texName);
    if ( v5 < 0 ) {
        this->loadPrescaled();
        return;
    }
    dk2::_fseek(
        MyTextures_instance.fileHandle,
        (int) MyTextures_instance.texNameToFileOffsetMap.entries.buf[v5].value,
        0);
    int width = -1;
    int height = -1;
    readFromFile(&width, 4u, 1u, MyTextures_instance.fileHandle);
    readFromFile(&height, 4u, 1u, MyTextures_instance.fileHandle);

    BOOL needsRescale = width < 16;
    unsigned __int8 height_ = height;
    if ( height < 16 ) needsRescale = 1;
    if ( width > 128 ) needsRescale = 1;
    if ( height > 128 ) needsRescale = 1;
    if ( needsRescale ) {
        this->loadPrescaled();
        return;
    }
    this->surfWidth8 = width;
    this->surfHeight8 = height_;
    this->createReduction();
    if (!f24_reductionLevel || !this->nextByReduction) {
        this->cesurf = (CEngineSurface*) MyTextures_instance.loadCompressed(texName);
    }
}


void dk2::MyCESurfHandle::loadPrescaled() {
    char surfName[2048];
    sprintf(surfName, MyStringHashMap_MyCESurfHandle_instance.entries.buf[this->mapIdx].name);
    char isPrescaled = 0;
    char *prescaledPos = strstr(surfName, "PRESCALED_TO");
    if (prescaledPos) {
        *prescaledPos = 0;
        isPrescaled = 1;
    }
    if (isPrescaled) {
        MyCESurfHandle *f4_value = MyStringHashMap_MyCESurfHandle_instance.entries.buf[
                ((MyStringHashMap *) &MyStringHashMap_MyCESurfHandle_instance)->getEntryIdx(surfName)
        ].value;

        f4_value->reductionLevel_andFlags |= 0x200u;
        CEngineSurface *f0_cesurf = f4_value->cesurf;
        if (f4_value->cesurf == NULL) {
            f4_value->resolveSurface();
            for (f0_cesurf = f4_value->cesurf; !f4_value->cesurf; f0_cesurf = f4_value->cesurf) {
                f4_value->reductionLevel_andFlags &= ~0x200;
                f4_value = f4_value->nextByReduction;
                f0_cesurf = f4_value->cesurf;
                f4_value->reductionLevel_andFlags |= 0x200u;
                if (f4_value->cesurf != NULL) break;
                f4_value->resolveSurface();
            }
        }
        CEngineSurfaceScaler_instance.prescaleWigth = f0_cesurf->width;
        CEngineSurfaceScaler_instance.prescaleHeight = f0_cesurf->height;
        // f0_cesurf may be a CEngineCompressedSurface (world/terrain/creature
        // texture) that only gets decoded here, inside copySurf(); give it
        // the name of the handle it came from (see TextureDump.h).
        patch::texture_dump::setCompositeSourceName(surfName);
        ((CEngineSurfaceBase *) CEngineSurfaceScaler_instance.orig_128x128_8a8r8g8b)->paintSurf(f0_cesurf, 0, 0);
        patch::texture_dump::setCompositeSourceName(nullptr);
        CEngineSurfaceScaler_instance.isScaled = 0;
        f4_value->reductionLevel_andFlags &= ~0x200u;
    } else {
        MySurface *Png = static_CBridge_loadPng(surfName);
        CEngineSurfaceScaler_instance.convertCopyFrom(Png);
        this->surfWidth8 = Png->size.w;
        this->surfHeight8 = Png->size.h;
        this->createReduction();
    }

    int f24_reductionLevel = MyDirectDraw_instance_devTexture.reductionLevel;
    char surfType;
    if ((this->flags & 8) != 0) {
        if (!g_isSupports_16bit) return;
        surfType = 2;
    } else {
        surfType = (this->reductionLevel_andFlags & 0x80) == 0;
    }

    if (this->surfWidth8 != CEngineSurfaceScaler_instance.prescaleWigth
        || this->surfHeight8 != CEngineSurfaceScaler_instance.prescaleHeight) {
        scaleImg(
                CEngineSurfaceScaler_instance.orig_128x128_8a8r8g8b->v_lockBuf(), CEngineSurfaceScaler_instance.prescaleWigth, CEngineSurfaceScaler_instance.prescaleHeight, 128,
                CEngineSurfaceScaler_instance.scaled_128x128_8a8r8g8b->v_lockBuf(), this->surfWidth8, this->surfHeight8, 128
        );
        CEngineSurfaceScaler_instance.isScaled = 1;
    }
    this->cesurf = (CEngineSurface *) CEngineSurfaceScaler_instance.copyScaledWithType(
            surfType, this->surfWidth8, this->surfHeight8);
    char v32[1024];
    sprintf(
            v32,
            "%sMM%d",
            MyStringHashMap_MyCESurfHandle_instance.entries.buf[this->mapIdx].name,
            this->reductionLevel_andFlags & 7);
    if (f24_reductionLevel > 0) {
        if (this->nextByReduction) {
            if (this->cesurf) this->cesurf->v_scalar_destructor(1u);
            this->cesurf = NULL;
        }
    }

    int dstWidth = this->surfWidth8 >> 1;
    int dstHeight = this->surfHeight8 >> 1;
    int v17_reductionLevel = f24_reductionLevel - 1;
    for (MyCESurfHandle *f18_nextByReduction = this->nextByReduction; f18_nextByReduction; f18_nextByReduction = f18_nextByReduction->nextByReduction) {
        if (dstWidth != CEngineSurfaceScaler_instance.prescaleWigth
            || dstHeight != CEngineSurfaceScaler_instance.prescaleHeight) {
            void *dstBuf = CEngineSurfaceScaler_instance.scaled_128x128_8a8r8g8b->v_lockBuf();
            int srcHeight = CEngineSurfaceScaler_instance.prescaleHeight;
            int srcWidth = CEngineSurfaceScaler_instance.prescaleWigth;
            void *srcBuf = CEngineSurfaceScaler_instance.orig_128x128_8a8r8g8b->v_lockBuf();
            scaleImg(srcBuf, srcWidth, srcHeight, 128, dstBuf, dstWidth, dstHeight, 128);
            CEngineSurfaceScaler_instance.isScaled = 1;
        }
        CEngineSurfaceBase *v21 = CEngineSurfaceScaler_instance.copyScaledWithType(surfType, dstWidth, dstHeight);
        unsigned int v22 = f18_nextByReduction->reductionLevel_andFlags;
        f18_nextByReduction->cesurf = (CEngineSurface *) v21;
        sprintf(
                v32,
                "%sMM%d",
                MyStringHashMap_MyCESurfHandle_instance.entries.buf[f18_nextByReduction->mapIdx].name,
                v22 & 7);
        if (v17_reductionLevel > 0) {
            if (f18_nextByReduction->nextByReduction) {
                if (f18_nextByReduction->cesurf) f18_nextByReduction->cesurf->v_scalar_destructor(1u);
                f18_nextByReduction->cesurf = NULL;
            }
        }
        --v17_reductionLevel;
        dstWidth >>= 1;
        dstHeight >>= 1;
    }
}



// 00590C30 MyCESurfHandle::paint
// The hot part on animated surfaces (torches, dungeon heart) is the CRC-16
// change-detector over the source pixels (init 0xFBEA, table g_crc_tab16,
// state = (state >> 8) ^ tab[(state ^ byte) & 0xFF]). CRC is XOR-linear, so a
// slicing-by-4 evaluation with tables derived from g_crc_tab16 is bit-exact
// while consuming 4 bytes per step.
namespace {

uint32_t g_crcSliced[4][256];
bool g_crcSlicedReady = false;

void ensureCrcSliced() {
    if (g_crcSlicedReady) return;
    for (int i = 0; i < 256; ++i) g_crcSliced[0][i] = (uint32_t) dk2::g_crc_tab16[i];
    for (int k = 1; k < 4; ++k) {
        for (int i = 0; i < 256; ++i) {
            const uint32_t prev = g_crcSliced[k - 1][i];
            g_crcSliced[k][i] = (prev >> 8) ^ (uint32_t) dk2::g_crc_tab16[prev & 0xFF];
        }
    }
    g_crcSlicedReady = true;
}

}

dk2::MyCESurfHandle *dk2::MyCESurfHandle::paint(MySurface *surf, char computeCrc) {
    // wip: terrain-HD-on-zoom investigation (2026-07-24) -- confirm whether
    // terrain-looking handles ever reach paint() at all, and whether they
    // have a holder_parent assigned by the time they get here. Remove once
    // resolved.
    {
        static int terrainPaintLogsLeft = 20;
        if (terrainPaintLogsLeft > 0) {
            const char *dbgName =
                    MyStringHashMap_MyCESurfHandle_instance.entries.buf[this->mapIdx].name;
            if (std::strstr(dbgName, "Rock") || std::strstr(dbgName, "rock") ||
                std::strstr(dbgName, "T_") || std::strstr(dbgName, "Path")) {
                --terrainPaintLogsLeft;
                patch::log::dbg("MyCESurfHandle::paint terrain-name: \"%s\" holder=%p",
                                dbgName, (void *) this->holder_parent);
            }
        }
    }
    if (!this->cesurf) this->create();
    MySurfDesc desc;
    memcpy(&desc, reinterpret_cast<const uint8_t *>(this->cesurf->fC_desc) + 0x2D, sizeof(desc));
    // Flush the current projected shadow before CRC can skip an unchanged
    // CPU surface. Metal deliberately leaves that scratch blank, so putting
    // this below the early return loses batches and can later associate stale
    // triangles with a different round-robin shadow slot.
    dk2::shadowgpu::finishIfCurrent(this, surf);
    if (computeCrc) {
        ensureCrcSliced();
        const uint8_t *row = static_cast<const uint8_t *>(surf->lpSurface);
        const int bytesPerRow = (int) (surf->size.w * surf->desc.dwRGBBitCount) >> 3;
        const int rows = surf->size.h;
        uint32_t crc = 0xFBEA;
        for (int i = 0; i < rows; ++i, row += surf->lPitch) {
            const uint8_t *p = row;
            int n = bytesPerRow;
            for (; n >= 4; n -= 4, p += 4) {
                crc = g_crcSliced[3][(crc ^ p[0]) & 0xFF]
                    ^ g_crcSliced[2][((crc >> 8) ^ p[1]) & 0xFF]
                    ^ g_crcSliced[1][p[2]]
                    ^ g_crcSliced[0][p[3]];
            }
            for (; n > 0; --n, ++p) crc = (crc >> 8) ^ (uint32_t) g_crc_tab16[(crc ^ *p) & 0xFF];
        }
        if ((int) crc == this->crc16Hash) return this;  // unchanged - skip repaint
        this->crc16Hash = (int) crc;
    }
    // Named texture dump hook: `surf` is the decoded-but-not-yet-composited
    // source surface for this handle's resource name, the narrowest point
    // where both are available together (see TextureDump.cpp for why here).
    // No-op unless flametal:TextureDump is set.
    const char *name = MyStringHashMap_MyCESurfHandle_instance.entries.buf[this->mapIdx].name;
    patch::texture_dump::onDecodedSurface(name, surf);

    void *pixels = this->cesurf->v_lockBuf();
    MySurface local;  // the original never destroys it either
    local.constructor(&surf->size, &desc, pixels, 0);
    MySurface_blend(&local, surf);
    this->cesurf->v_unlockBuf((int) pixels);
    SurfaceHolder *holder = this->holder_parent;
    if (holder) {
        if (MyDirectDraw_instance.flags & 1) {
            // this->cesurf itself may be a CEngineCompressedSurface for
            // world/terrain/creature/room handles: it only gets decoded
            // inside copySurf(), triggered by this call.
            patch::texture_dump::setCompositeSourceName(name);
            holder->surf->paintSurf(this->cesurf, this->x8, this->y8);
            patch::texture_dump::setCompositeSourceName(nullptr);
            // named-atlas map for the host's HD resource pack (see
            // reportAtlasRect: no-op when the bridge is disabled)
            if (this->cesurf) {
                gog::metal_bridge::reportAtlasRect(
                        atlasPageKey(holder->surf), name, this->x8, this->y8,
                        static_cast<uint32_t>(this->cesurf->width),
                        static_cast<uint32_t>(this->cesurf->height));
            }
        } else {
            // unlink this handle from the holder's list
            MyCESurfHandle *prev = nullptr;
            for (MyCESurfHandle *cur = holder->surfh_first; cur; prev = cur, cur = cur->nextByHolder) {
                if (cur != this) continue;
                if (prev) prev->nextByHolder = cur->nextByHolder;
                else holder->surfh_first = cur->nextByHolder;
                this->nextByHolder = nullptr;
                this->holder_parent = nullptr;
                break;
            }
        }
    }
    return this;
}
