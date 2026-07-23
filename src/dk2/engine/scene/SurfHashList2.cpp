//
// Created by DiaLight on 1/24/2026.
//
#include "dk2_functions.h"
#include "dk2_globals.h"
#include "dk2/SurfHashList2.h"
#include "dk2/MyCESurfHandle.h"
#include "dk2/SurfaceHolder.h"
#include "dk2/CEngineSurfaceBase.h"
#include "dk2/CEngineDDSurface.h"
#include "dk2/MyStringHashMap_MyCESurfHandle_entry.h"
#include "dk2/MyCEngineSurfDesc.h"
#include "dk2/SurfHashListItem.h"
#include <metal_bridge/MetalBridgeProducer.h>

namespace {

const void *atlasPageKey(dk2::CEngineSurfaceBase *page) {
    if (page && *(void **) page == (void *) 0x6703C4) {
        IDirectDrawSurface4 *dd = static_cast<dk2::CEngineDDSurface *>(page)->ddSurf;
        if (dd) return dd;
    }
    return page;
}

void reportHardwareAtlasRect(dk2::MyCESurfHandle *handle) {
    if (!handle || !handle->holder_parent || !handle->surfWidth8 || !handle->surfHeight8) return;
    const char *name =
        dk2::MyStringHashMap_MyCESurfHandle_instance.entries.buf[handle->mapIdx].name;
    gog::metal_bridge::reportAtlasRect(
        atlasPageKey(handle->holder_parent->surf), name, handle->x8, handle->y8,
        handle->surfWidth8, handle->surfHeight8);
}

// FUN_00591da0 (still original/untranslated): allocates one SurfaceHolder
// (0x20 bytes) wrapping a fresh hardware CEngineDDSurface of size
// (holderSize x holderSize), built from `desc`. cdecl, no header entry
// exists for it anywhere in libs/dkii_exe/api (confirmed absent by grep --
// same situation as Obj57AD20::sub_57AC10 documented elsewhere in this
// project), so called through a raw function pointer to its original
// address like the other genapi-missed helpers.
using CreateHolderFn = dk2::SurfaceHolder *(__cdecl *)(int, dk2::MyCEngineSurfDesc *, int);
constexpr auto createHolder = reinterpret_cast<CreateHolderFn>(0x00591DA0);

}  // namespace

// dk2::SurfHashList2::constructor (0x592BD0). Allocates `targetHolders`
// hardware holder pages of holder_size x holder_size each (bailing early if
// an allocation fails), then requires at least `minHolders` to have
// succeeded -- otherwise tears everything down and reports failure.
//
// holder_size bumped 0x80 (128) -> 0x100 (256) (2026-07-24): the original's
// hardcoded 128 packs many small, unrelated named textures (terrain rock/
// floor/wall variants, ~15-30 per page observed live) into one tiny shared
// atlas page, so the HD resource-pack art (verified separately reaching
// composePage successfully) gets scaled down into a correspondingly tiny
// sub-rect and reads as blurry -- not a registration bug, a page-size
// ceiling. 256 is the max safe bump: MyCESurfHandle::x8/y8 (the position of
// a handle's rect within its holder) are uint8_t, so positions must stay in
// 0..255, meaning holder_size can be at most 256. Untouched:
// createHolder/sub_593880's own math (they already read holder_size back
// from `this`, not a hardcoded local), and FUN_00591da0 itself (unchanged,
// original address) -- it already computes everything (including
// SurfaceHolder::_1divSize = 1/holder_size) from the size it's given.
char dk2::SurfHashList2::constructor(MyCEngineSurfDesc *desc, int minHolders, int targetHolders) {
    this->surfDesc = desc;
    this->holder_size = 0x100;
    this->holder_count = 0;

    int allocated = 0;
    if (targetHolders > 0) {
        do {
            SurfaceHolder *newHolder = createHolder(this->holder_size, this->surfDesc, 1);
            if (!newHolder) break;
            ++this->holder_count;
            if (this->holder_first != nullptr) {
                this->holder_first->next_ = newHolder;
            }
            newHolder->next_ = nullptr;
            ++allocated;
            newHolder->prev_ = this->holder_first;
            this->holder_first = newHolder;
        } while (allocated < targetHolders);
    }

    if (this->holder_count < minHolders) {
        // Not enough holders could be allocated: reverse-walk the list
        // (built via prev_ above) and tear every holder down, matching the
        // original's cleanup exactly.
        SurfaceHolder *cur = this->holder_first;
        SurfaceHolder *reversed = nullptr;
        while (cur != nullptr) {
            SurfaceHolder *walkNext = cur->prev_;
            cur->prev_ = reversed;
            reversed = cur;
            cur = walkNext;
        }
        this->holder_first = nullptr;
        cur = reversed;
        while (cur != nullptr) {
            SurfaceHolder *next = cur->prev_;
            if (cur->surf) {
                cur->surf->v_scalar_destructor(1);
            }
            if (cur->hashItem_link) {
                // Unreachable in practice: freshly-allocated holders here
                // have never been hashed into, so hashItem_link is always
                // null -- mirrored for fidelity rather than assumed away.
                using FreeItemFn = void (__thiscall *)(SurfHashListItem *);
                reinterpret_cast<FreeItemFn>(0x00592D40)(cur->hashItem_link);
            }
            MyHeap_free(cur);
            cur = next;
        }
        if (this->ddsurf) {
            this->ddsurf->v_scalar_destructor(1);
        }
        this->ddsurf = nullptr;
        return 0;
    }

    auto *newDdsurf = static_cast<CEngineDDSurface *>(MyHeap_alloc(sizeof(CEngineDDSurface)));
    if (!newDdsurf) {
        this->ddsurf = nullptr;
        return 0;
    }
    this->ddsurf = newDdsurf->constructor(this->holder_size, this->holder_size, this->surfDesc, 0);
    return 1;
}

int dk2::SurfHashList2::_probablySort() {
    int handleCount = this->calcHandleCountToFitHolder();
    int handleCount_ = handleCount;
    for (MyCESurfHandle *f4_surfh_first = this->surfh_first; f4_surfh_first; f4_surfh_first = f4_surfh_first->nextByHashList) {
        MyCESurfHandle *curReducted = f4_surfh_first;
        for (int handleLeft = handleCount_;
             (handleLeft > 0 || (curReducted->reductionLevel_andFlags & 7) < MyDirectDraw_instance_devTexture.reductionLevel)
             && curReducted->nextByReduction;
             --handleLeft) {
            curReducted = curReducted->nextByReduction;
        }
        f4_surfh_first->curReduction = curReducted;
        double f28_padNorm_width = curReducted->padNorm_width;
        curReducted->reductionLevel_andFlags |= 8;
        if (f28_padNorm_width == 0.0)
            curReducted->holder_parent = 0;
        if (!curReducted->holder_parent)
            this->deleteHolder(curReducted);
    }
    SurfaceHolder *v9 = NULL;
    SurfaceHolder *v31 = NULL;
    while (this->f8count) {
        int surfCount = 0;
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                for (MyCESurfHandle *i = this->arr5x5_surfh[x][y]; i; i = i->nextByHolder) ++surfCount;
            }
        }
        SurfaceHolder *fD8_holder_first = this->holder_first;
        int v17_minValue = 0x10000;
        SurfaceHolder *v16_minValueItem = this->holder_first;
        if (fD8_holder_first) {
            while (true) {
                int v18_value = fD8_holder_first->calcWeight();
                if (!v18_value) break;
                if (v18_value < v17_minValue) {
                    v17_minValue = v18_value;
                    v16_minValueItem = fD8_holder_first;
                }
                fD8_holder_first = fD8_holder_first->prev_;
                if (!fD8_holder_first) {
                    fD8_holder_first = v16_minValueItem;
                    break;
                }
            }
        } else {
            fD8_holder_first = v16_minValueItem;
        }
        if (!fD8_holder_first) {
            this->sub_593E90(v31);
            break;
        }

        if (SurfaceHolder *f10_prev = fD8_holder_first->next_)
            f10_prev->prev_ = fD8_holder_first->prev_;
        else
            this->holder_first = fD8_holder_first->prev_;

        if (SurfaceHolder *f14_next = fD8_holder_first->prev_)
            f14_next->next_ = fD8_holder_first->next_;
        fD8_holder_first->next_ = NULL;
        fD8_holder_first->prev_ = NULL;
        for (MyCESurfHandle *j = fD8_holder_first->surfh_first; j; j = fD8_holder_first->surfh_first) {
            fD8_holder_first->surfh_first = j->nextByHolder;
            bool v22 = (j->reductionLevel_andFlags & 8) == 0;
            j->nextByHolder = NULL;
            j->holder_parent = NULL;
            if (!v22)
                this->deleteHolder(j);
        }
        // The page is recomposited from scratch: invalidate its atlas map on
        // both bridge sides before the new placements arrive, or historical
        // rects keep getting composed into the reused page.
        gog::metal_bridge::atlasPageReset(atlasPageKey(fD8_holder_first->surf));
        this->sub_593880(fD8_holder_first, 0, 0, this->holder_size, this->holder_size, 3);
        if (auto *fD0_ddsurf = this->ddsurf)
            fD8_holder_first->surf->paintSurf((CEngineSurface *) fD0_ddsurf, 0, 0);
        SurfaceHolder *v24 = v31;
        v31 = fD8_holder_first;
        fD8_holder_first->prev_ = v24;
    }
    v9 = v31;
    if (v9) {
        while (true) {
            SurfaceHolder *v25 = this->holder_first;
            SurfaceHolder *v26 = v9->prev_;
            if (v25)
                v25->next_ = v9;
            SurfaceHolder *v27 = this->holder_first;
            v9->next_ = NULL;
            v9->prev_ = v27;
            this->holder_first = v9;
            if (!v26)
                break;
            v9 = v26;
        }
    }
    if (this->surfh_first) {
        // Hardware/devTexture atlases are packed by SurfHashList2's original
        // helpers (deleteHolder/sub_593880), not SurfHashList::expandPut.
        // At this point every selected reduction has its final page+x/y;
        // report exactly the handles considered by this sort before the list
        // is cleared below. Producer-side dedupe makes recurring use cheap.
        for (MyCESurfHandle *handle = this->surfh_first; handle;
             handle = handle->nextByHashList) {
            reportHardwareAtlasRect(handle->curReduction);
        }
        MyCESurfHandle *f10_nextByHashList;
        do {
            MyCESurfHandle *v28 = this->surfh_first;
            v28->reductionLevel_andFlags &= ~0x10u;
            v28->curReduction->reductionLevel_andFlags &= ~8u;
            f10_nextByHashList = v28->nextByHashList;
            this->surfh_first = f10_nextByHashList;
        } while (f10_nextByHashList);
    }
    return ++SurfHashList_sortTick;
}
