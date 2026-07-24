//
// dk2::static_createMeshes_p (0x005725D0..0x00572814, padded with nops to
// 0x00572820 where the next auto-declared function starts).
//
// Called once per frame from the camera pipeline (callsite 0x00575CCC, see
// dk2::installCameraPhaseProfiler() in CameraPhaseProfile.cpp, phase
// "meshes"), right before dk2::emitSceneCellPrimitives() (0x00572CF0).
//
// Walks the intrusive singly-linked list of "dirty" scene cells rooted at
// g_pNewObj571B3B_root (a NewObj571B3B::np chain), and for each dirty cell
// (a2,a3) refreshes/creates the meshes for a small block of cells centered
// on it: normally the full 7x7 block [a2-3..a2+3] x [a3-3..a3+3], but the
// block is clipped inward on whichever side its immediate (a2+-1,a3)/
// (a2,a3+-1) neighbour has flags bit 3 set -- i.e. the west/east/south/
// north extents collapse from +-3 down to the centre cell itself whenever
// that bit is set on the corresponding neighbour.
//
// flags bit 3 producer/consumer, traced by disassembly:
//   - SET at 0x00573784 (`or edx, 0x8` on the cell's flags, immediately
//     after zeroing its `np` link at 0x00573780) inside
//     MyDLVec2i_static_sub_5735A0 (0x005735A0, declared in dk2_functions.h
//     and already wired up as the camera pipeline's "BuildVisibleB" phase
//     in CameraPhaseProfile.cpp). That function enumerates the screen-space
//     cell rectangle for a visibility request, resolves/creates each cell
//     via NewObj571B3B_add (calling the same 0x00571B00 helper used by
//     findOrAddCell above), links them into the g_pNewObj571B3B_root chain
//     via `np`, and unconditionally sets bit 3 on every cell it enqueues --
//     i.e. bit 3 marks "this cell is currently enqueued in this frame's
//     dirty/rebuild list".
//   - CLEARED at 0x00572de2 (`and ecx, -0x9` on the cell's flags) inside
//     dk2::emitSceneCellPrimitives (0x00572CF0, the very next camera-phase
//     step after this function per the file-header note above). That
//     function walks the *same* g_pNewObj571B3B_root/np chain, releases
//     each cell's per-frame primitive lists (fields +0x2C/+0x30), and only
//     then clears bit 3 -- i.e. bit 3 flips back off once the cell has been
//     fully processed/emitted for the frame.
//   NewObj571B3B_add (0x00571B00) itself never touches bit 3 beyond the
//   generic `flags &= 1` reset on first allocation (0x00571B59) and the
//   `(flags & ~1) | ((param ^ flags) & 1)` bit-0-only recombine on lookup
//   (0x00571B99..0x00571BB2); it neither sets nor clears bit 3 for a reused
//   cell, so a neighbour's bit 3 here genuinely reflects whether that
//   neighbour is *itself* separately enqueued in the current dirty list --
//   confirming the "already handled independently, don't expand into it"
//   reading the clipping logic below already assumed. No logic change
//   needed; only this comment was previously unverified.
//
// See NewObj571B3B.h for the intrusive-list/hash-grid node layout (this is
// the same node type used by NewObj571B3B_add/NewObj571B3B_sub_5722A0/
// NewObj571B3B_sub_571F00 elsewhere, still largely untranslated).

#include "dk2/NewObj571B3B.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <cstdint>

// Shelf translation of the sceneEmit camera phase (0x00572CF0); see the block
// comment above its definition below. Declared here (the api models the
// original as `emitSceneCellPrimitives`; this `_p` symbol is the on-the-shelf
// re-implementation, mirroring static_createMeshes_p, and collides with
// nothing).
namespace dk2 {
void emitSceneCellPrimitives_p();
}

namespace {

// 0x00572616 / 0x00572663 / 0x005726bd / 0x005726fa / 0x005727b0: the
// hash-grid lookup shared by every neighbour test and by the inner block
// loop below. g_NewObj571B3B_hashTable is a 64x64 open grid keyed by
// (x & 0x3f, y & 0x3f); a slot is only trusted as a hit if it actually
// holds the cell (x,y) requested (a2/a3 match) *and* was last touched this
// scene session (f4 == g_ddSceneSessionId) -- otherwise NewObj571B3B_add()
// is called to either recycle the stale slot or allocate a fresh node.
dk2::NewObj571B3B *findOrAddCell(int x, int y) {
    // g_NewObj571B3B_hashTable (0x760B90) is declared in dk2_globals.h but not
    // exported by the delinked import library - address it directly.
    auto hashTable = reinterpret_cast<dk2::NewObj571B3B *(*)[64]>(0x00760B90);
    dk2::NewObj571B3B *bucket = hashTable[x & 0x3f][y & 0x3f];
    if (bucket != nullptr
            && bucket->a2 == static_cast<int16_t>(x)
            && bucket->a3 == static_cast<int16_t>(y)
            && bucket->f4 == dk2::g_ddSceneSessionId) {
        return bucket;
    }
    return dk2::NewObj571B3B_add(bucket, x, y);
}

}  // namespace

void dk2::static_createMeshes_p() {
    NewObj571B3B *node = dk2::g_pNewObj571B3B_root;
    if (node == nullptr) return;

    do {
        // 0x005725f0: side-effecting per-node update; the original discards
        // the return value (eax is immediately clobbered by the a2/a3 read
        // below).
        NewObj571B3B_sub_5722A0(node);

        const int a2 = node->a2;
        const int a3 = node->a3;

        // 0x0057260b..0x00572647: west neighbour (a2-1, a3).
        NewObj571B3B *west = findOrAddCell(a2 - 1, a3);
        // bit 3 = "already enqueued in the current dirty list" (set by
        // MyDLVec2i_static_sub_5735A0 @0x573784, cleared by
        // emitSceneCellPrimitives @0x572de2; see file-header note above).
        const bool westFlagBit3 = ((west->flags >> 3) & 1) != 0;

        // 0x0057264a..0x00572694: east neighbour (a2+1, a3).
        NewObj571B3B *east = findOrAddCell(a2 + 1, a3);
        const bool eastFlagBit3 = ((east->flags >> 3) & 1) != 0;

        // 0x00572697..0x005726ea: south neighbour (a2, a3-1).
        NewObj571B3B *south = findOrAddCell(a2, a3 - 1);
        const bool southFlagBit3 = ((south->flags >> 3) & 1) != 0;

        // 0x005726ed..0x00572736: north neighbour (a2, a3+1).
        NewObj571B3B *north = findOrAddCell(a2, a3 + 1);
        const bool northFlagBit3 = ((north->flags >> 3) & 1) != 0;

        // 0x00572739..0x0057277d: clip the 7x7 block inward on any side
        // whose neighbour has flags bit 3 set (verified meaning: neighbour
        // already enqueued in the current dirty list -- see comment above).
        const int minX = westFlagBit3 ? a2 : a2 - 3;
        const int maxX = eastFlagBit3 ? a2 : a2 + 3;
        const int minY = southFlagBit3 ? a3 : a3 - 3;
        const int maxY = northFlagBit3 ? a3 : a3 + 3;

        // 0x0057277f..0x005727fa: refresh every cell in the clipped block.
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                NewObj571B3B *cell = findOrAddCell(x, y);
                static_createMeshes(reinterpret_cast<int16_t *>(cell));
            }
        }

        // 0x005727fa: advance to the next dirty cell (NewObj571B3B::np).
        node = node->np;
    } while (node != nullptr);
}


//
// dk2::emitSceneCellPrimitives_p (0x00572CF0..0x00572DFA).
//
// The camera pipeline's "sceneEmit" phase, run once per frame immediately
// after static_createMeshes_p (see file header). It is the callee of the
// call at 0x00575D0A (dk2::installCameraPhaseProfiler()'s "scene emission"
// probe, CameraPhaseProfile.cpp). It walks the SAME g_pNewObj571B3B_root/np
// dirty-cell chain as static_createMeshes_p above and, per cell, re-emits
// that cell's two primitive lists into the scene, then clears the cell's
// flags bit 3 (0x00572de2, the "no longer enqueued" clear this file's header
// already traced).
//
// This is a faithful, address-verified re-implementation kept alongside its
// sibling static_createMeshes_p. It is NOT wired into the runtime: the port
// activates a native phase by call-site-patching the original x86 callsite
// (see the CallPatch table in installCameraPhaseProfiler), and no such patch
// redirects 0x00575D0A here yet -- so, exactly like static_createMeshes_p,
// defining this symbol changes NOTHING the running game does (every engine
// caller reaches 0x00572CF0 by baked-in address, not by this symbol). It is
// on the shelf, ready for a future session to activate + validate.
//
// Every offset/ABI fact below is cross-checked against the live DKII.EXE
// disassembly (0x00572CF0) and the generated struct headers:
//   - loop over g_pNewObj571B3B_root, advance via NewObj571B3B::np (+0x24).
//   - if g_rebuildSceneCellLists != 0: sub_572A70(cell, 1, 0)  [0x00572d1b].
//   - build a 20-byte scratch record (the stack struct at &local_14) passed
//     by pointer to every primitive's virtual method:
//       +0x00 a2 (cell->a2)         +0x04 a3 (cell->a3)
//       +0x08 &cell->fC4            +0x0c &cell->gap_44 or nullptr
//       +0x10 cell->sceneScale (+0x15c).
//   - flags bit 7 (0x00572d42 SHR 7 / TEST 1): when set, record[+0x0c] =
//     &cell->gap_44 and the Obj58EF60 at 0x781078 is (re)constructed with
//     (float(a2-1), float(a3-1), &cell->gap_44); when clear, record[+0x0c]
//     = nullptr and 0x781078 is left untouched.
//   - the Obj58EF60 at 0x781088 is ALWAYS (re)constructed with
//     (float(a2-1), float(a3-1), &cell->gap_44[0x40] == cell+0x84).
//     Obj58EF60::constructor (0x0058EF60) is a trivial 3-store leaf
//     (this->f0 = ptr; this->f4 = arg0; this->f8 = arg1 -- verified from its
//     own decompile); it is inlined here as the three stores, following this
//     session's established leaf-inlining discipline (no behaviour change).
//   - two primitive lists (heads cell+0x2c / cell+0x30, i.e.
//     pCEnginePrimitiveBase / pCEnginePrimitiveBase_last), each a singly
//     linked list threaded through the node's +0x04 word; for every node the
//     original calls virtual-table slot 2 (`call [vtbl+8]`, __thiscall,
//     this = node, single stack arg = &record) and ignores the result. The
//     concrete list element is a CEnginePrimitiveBase subclass whose slot-2
//     method is not modelled in the base header, so the call is issued
//     through the raw vtable exactly as the original does.
//   - finally clear cell->flags bit 3 (0x00572de2 AND 0xfffffff7).
//
void dk2::emitSceneCellPrimitives_p() {
    // The scratch record handed to every primitive's slot-2 method. Layout is
    // byte-identical to the original's stack struct at &local_14 (five 4-byte
    // fields, natural alignment -> 20 bytes, no padding on the 32-bit target).
    struct EmitRecord {
        int32_t a2;
        int32_t a3;
        void *pFC4;
        void *pGap44;
        float sceneScale;
    };

    for (NewObj571B3B *cell = dk2::g_pNewObj571B3B_root; cell != nullptr;
         cell = cell->np) {
        const int a2 = cell->a2;
        const int a3 = cell->a3;

        // 0x00572d13: rebuild this cell's primitive lists first when the
        // global rebuild flag is set (side-effecting; result discarded).
        if (dk2::g_rebuildSceneCellLists != 0) {
            sub_572A70(reinterpret_cast<int16_t *>(cell), 1, 0);
        }

        EmitRecord record;
        record.a2 = a2;                     // local_14
        record.a3 = a3;                     // local_10
        record.pFC4 = &cell->fC4;           // local_c  (cell+0xc4)
        record.sceneScale = cell->sceneScale;  // local_4 (cell+0x15c)

        const float fa2 = static_cast<float>(a2 - 1);
        const float fa3 = static_cast<float>(a3 - 1);

        // 0x00572d42: flags bit 7 gates both record[+0x0c] and the 0x781078
        // Obj58EF60 update.
        if (((static_cast<uint32_t>(cell->flags) >> 7) & 1) == 0) {
            record.pGap44 = nullptr;        // local_8
        } else {
            record.pGap44 = cell->gap_44;   // &cell->gap_44 (cell+0x44)
            // Inlined Obj58EF60::constructor(0x0058EF60) on g_..._781078.
            dk2::g_Obj58EF60_instance_781078.f0 =
                    static_cast<int>(reinterpret_cast<intptr_t>(record.pGap44));
            dk2::g_Obj58EF60_instance_781078.f4 = fa2;
            dk2::g_Obj58EF60_instance_781078.f8 = fa3;
        }

        // 0x00572d81: always (re)construct the 0x781088 Obj58EF60 from
        // cell+0x84 (== &cell->gap_44[0x40]). Inlined constructor, as above.
        dk2::g_Obj58EF60_instance_781088.f0 =
                static_cast<int>(reinterpret_cast<intptr_t>(&cell->gap_44[0x40]));
        dk2::g_Obj58EF60_instance_781088.f4 = fa2;
        dk2::g_Obj58EF60_instance_781088.f8 = fa3;

        // 0x00572dab / 0x00572dc5: walk each primitive list and invoke the
        // element's virtual-table slot 2 (`call [vtbl+8]`, __thiscall,
        // this = node, arg = &record). The next-link is the node's +0x04
        // word. Both loops are identical apart from their list head.
        for (void *prim = cell->pCEnginePrimitiveBase; prim != nullptr;
             prim = *reinterpret_cast<void **>(
                     reinterpret_cast<uint8_t *>(prim) + 4)) {
            void **vtbl = *reinterpret_cast<void ***>(prim);
            reinterpret_cast<void(__thiscall *)(void *, void *)>(vtbl[2])(
                    prim, &record);
        }
        for (void *prim = cell->pCEnginePrimitiveBase_last; prim != nullptr;
             prim = *reinterpret_cast<void **>(
                     reinterpret_cast<uint8_t *>(prim) + 4)) {
            void **vtbl = *reinterpret_cast<void ***>(prim);
            reinterpret_cast<void(__thiscall *)(void *, void *)>(vtbl[2])(
                    prim, &record);
        }

        // 0x00572de2: mark the cell no longer enqueued for this frame.
        cell->flags &= 0xfffffff7;
    }
}
