#include "dk2/MyDirectDraw.h"
#include "dk2/MyEntryBuf_Triangle24.h"
#include "dk2/MyEntryBuf_uint16.h"
#include "dk2/SurfaceHolder.h"
#include "dk2/Triangle24.h"
#include "dk2/VerticesData.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

// 005898F0..0058990F: draw_tex_to_buf().
//
// This is a two-instruction dispatcher, not a body of its own: it tests bit 0
// of MyDirectDraw_instance_triangles.flags (offset 0x28 of MyDirectDraw, the
// global at 0x0076C2B8 + 0x28 == 0x0076C2E0) and tail-calls (`jmp`, not
// `call`) one of two large sibling functions declared in dk2_functions.h:
//   - draw_tex_to_buf_impl1 (0x00588F90) when the flag bit is set,
//   - draw_tex_to_buf_impl2 (0x00588D00) otherwise.
// Both siblings remain untranslated (auto-generated declarations only); this
// file only supplies the dispatcher itself, calling through to whichever one
// is live in the original binary via the declared prototypes.
//
// Because the original used `jmp` rather than `call`, EAX at return is
// whatever the callee left there -- reproduced here by simply returning the
// callee's result through the same reinterpret_cast the auto header implies
// (the callees are declared as returning `int`, the dispatcher as `int *`).

namespace dk2 {

int *draw_tex_to_buf() {
    if ((MyDirectDraw_instance_triangles.flags & 1) != 0) {
        return reinterpret_cast<int *>(draw_tex_to_buf_impl1());
    }
    return reinterpret_cast<int *>(draw_tex_to_buf_impl2());
}

}  // namespace dk2


// 00588D00..00588F8D: draw_tex_to_buf_impl2() -- the hardware-renderer
// branch (taken when MyDirectDraw_instance_triangles.flags & 1 == 0).
//
// This flushes the queued 2D-primitive triangle pool (built up elsewhere by
// whatever pushes entries into sceneObj2E_f21_to_triangleIndices / the
// Triangle24 pool / g_flexibleVertices) into hardware draw calls.
//
// sceneObj2E_f21_to_triangleIndices[1024] is a hash-bucket table: each slot
// either holds -1 (empty) or the index of the *last* Triangle24 queued for
// that bucket; Triangle24::prevTriangleIdx chains backwards to earlier
// triangles queued into the same bucket (again -1-terminated). Buckets are
// consumed by scanning top-down (1023..0); a consumed bucket's slot is reset
// to -1 so the next frame starts empty.
// TODO(verify): dk2_globals.h declares this array as `int[1023]`, but the
// original scans (and, on a fully-populated frame, touches) index 1023 too
// (1024 slots total) -- see the loop below. Left as-is (indexing through the
// declared symbol) since this is a pre-existing header sizing quirk, not
// something to silently "fix" in a translation.
//
// Triangles are grouped into batches: consecutive (bucket-chain-order)
// Triangle24 entries are coalesced into one DrawTriangleList() call as long
// as they share props_flags (-> DirectDraw_prepareTexture argument),
// holders[] (-> renderer_setSurfaceHolder per stage), and the batch stays
// under the fixed 0x100-triangle capacity (TODO(verify): exact origin of
// this 256 cap wasn't traced further, presumably matches a fixed-size
// vertex/triangle scratch buffer elsewhere). A batch can legitimately span
// a bucket boundary if the next bucket's head happens to match state.
//
// Each triangle's 3 Triangle24::vertIdx{0,1,2} are indices into the
// "flexible vertex" pool (g_flexibleVertices, stride g_flexibleVertex_size);
// MyEntryBuf_uint16_indices_instance.buf[] is a per-flexible-vertex-index
// cache (-1 == "not yet emitted this batch") mapping to the compacted
// per-batch vertex index written into g_vertices[0].verticies1C and
// referenced from DrawTriangleList_lpwIndices. The cache entries touched by
// a batch are reset back to -1 right before that batch is flushed (via
// drawnTrianglesArr[], which records, for each triangle accumulated into
// the *current* batch, which Triangle24 produced it).
namespace dk2 {

int draw_tex_to_buf_impl2() {
    int *bucketHeads = sceneObj2E_f21_to_triangleIndices;

    Triangle24 *node = nullptr;
    int bucket = 1024;               // [esp+0x28]; one-past-end sentinel
    int batchTriangleCount = 0;      // [esp+0x14]
    int batchVertexCount = 0;        // [esp+0x18]
    int batchSurfhCount = 0;         // [esp+0x1c]
    int batchDrawFlags = 0;          // [esp+0x20]
    // TODO(verify): the original's stack prologue zero-inits this slot (not
    // to DrawTriangleList_lpwIndices' base -- that assignment only happens
    // once a batch is actually opened, below). Mirrored as nullptr rather
    // than "helpfully" pre-pointing it, since a node matching the all-zero
    // initial batch state (flags==0 && surfhCount==0) on the very first
    // iteration would dereference this while still null in the original
    // too; believed unreachable in practice (a real queued triangle always
    // has at least one texture holder), not "fixed" here.
    uint16_t *lpwIndicesCursor = nullptr;  // [esp+0x24]
    SurfaceHolder *holderSnapshot[4] = {};  // [esp+0x30..0x3c]

    for (;;) {
        if (!node) {
            // Scan buckets top-down for the next non-empty chain head,
            // clearing each consumed bucket back to -1 as we move past it
            // (the original defers this clear to the next time this scan
            // runs -- see loop shape -- which is externally unobservable
            // since nothing else touches the table during this call).
            for (;;) {
                if (bucket < 1024) bucketHeads[bucket] = -1;
                --bucket;
                if (bucket < 0) break;
                int headIdx = bucketHeads[bucket];
                if (headIdx >= 0) {
                    // sizeof(Triangle24) == 0x24 == 36, matching the
                    // original's `buf + 36*headIdx` byte address exactly,
                    // so plain pointer arithmetic reproduces it.
                    node = MyEntryBuf_Triangle24_instance.buf + headIdx;
                    if (node) break;
                }
            }
        }

        bool mustFlush;
        if (!node) {
            mustFlush = true;
        } else if (node->props_flags != static_cast<uint32_t>(batchDrawFlags) ||
                   static_cast<int>(node->surfhCount) != batchSurfhCount ||
                   batchTriangleCount >= 0x100) {
            mustFlush = true;
        } else {
            mustFlush = false;
            for (int i = 0; i < batchSurfhCount; ++i) {
                if (node->holders[i] != holderSnapshot[i]) {
                    mustFlush = true;
                    break;
                }
            }
        }

        if (mustFlush) {
            if (batchTriangleCount != 0) {
                // Release this batch's vertex-remap cache entries.
                for (int t = 0; t < batchTriangleCount; ++t) {
                    Triangle24 *drawn = drawnTrianglesArr[t];
                    const int *vertIdx = &drawn->vertIdx0;
                    for (int k = 0; k < 3; ++k) {
                        MyEntryBuf_uint16_indices_instance.buf[vertIdx[k]] = 0xFFFF;
                    }
                }
                DirectDraw_prepareTexture(batchDrawFlags);
                for (int i = 0; i < batchSurfhCount; ++i) {
                    renderer_setSurfaceHolder(holderSnapshot[i], i);
                }
                DrawTriangleList(0, batchTriangleCount, batchVertexCount);
                batchTriangleCount = 0;
                batchVertexCount = 0;
            }
            if (!node) {
                // End of frame: reset the 3d-scene submission accumulators
                // for the next frame (mirrors the tail of the original).
                g_totalTrianglesCount = 0;
                g_totalVerticesCount = 0;
                Triangle34_count = 0;
                return 0;  // TODO(verify): original leaves whatever was last in EAX; unused by callers.
            }
            batchSurfhCount = static_cast<int>(node->surfhCount);
            lpwIndicesCursor = reinterpret_cast<uint16_t *>(DrawTriangleList_lpwIndices);
            if (batchSurfhCount > 0) {
                std::memcpy(holderSnapshot, node->holders, sizeof(SurfaceHolder *) * batchSurfhCount);
            }
            batchDrawFlags = static_cast<int>(node->props_flags);
        }

        // Accumulate `node`'s triangle into the current batch, remapping
        // its 3 flexible-vertex indices into the compacted output buffer.
        const int *srcVertIdx = &node->vertIdx0;
        for (int k = 0; k < 3; ++k) {
            int srcIdx = srcVertIdx[k];
            int16_t mapped;
            std::memcpy(&mapped, &MyEntryBuf_uint16_indices_instance.buf[srcIdx], sizeof(mapped));
            if (mapped < 0) {
                mapped = static_cast<int16_t>(batchVertexCount);
                MyEntryBuf_uint16_indices_instance.buf[srcIdx] = static_cast<uint16_t>(mapped);
                int stride = g_flexibleVertex_size;
                char *dst = reinterpret_cast<char *>(g_vertices[0].verticies1C) +
                        static_cast<ptrdiff_t>(mapped) * stride;
                const char *src = reinterpret_cast<const char *>(g_flexibleVertices) +
                        static_cast<ptrdiff_t>(srcIdx) * stride;
                // TODO(verify): original skips this copy entirely (leaving
                // dst untouched) when stride<=0, but still counts the vertex
                // -- reproduced here since the loop below-runs 0 times.
                for (int b = 0; b < stride; ++b) dst[b] = src[b];
                ++batchVertexCount;
            }
            uint16_t mappedU;
            std::memcpy(&mappedU, &mapped, sizeof(mappedU));
            *lpwIndicesCursor++ = mappedU;
        }

        drawnTrianglesArr[batchTriangleCount] = node;
        int prevIdx = static_cast<int>(node->prevTriangleIdx);
        if (prevIdx < 0) {
            node = nullptr;
        } else {
            node = MyEntryBuf_Triangle24_instance.buf + prevIdx;
        }
        ++batchTriangleCount;
    }
}

}  // namespace dk2
