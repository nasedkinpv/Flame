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
