#include "dk2/engine/primitive/2d/world/CEngineAnimMesh.h"

#include "dk2/MyEntryBuf_MyStringHashMap_MyMeshResourceHolder_entry.h"
#include "dk2/MyMeshResourceHolder.h"
#include "dk2/MyScaledSurface.h"
#include "dk2/MyStringHashMap_MyMeshResourceHolder.h"
#include "dk2/MyStringHashMap_MyMeshResourceHolder_entry.h"
#include "dk2/SprsAnimHeader.h"
#include "dk2/Triangle.h"
#include "dk2/engine/primitive/resource/CAnimMeshResource.h"
#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <tools/flametal_config.h>

#include <cstdint>
#include <cstring>
#include <emmintrin.h>


// flametal:ShadowCache -- see the cache implementation below (just above
// sub_5855E0) for the investigation writeup that justifies caching the raw
// coverage bitmap rather than the engine's surface handle. Declared here
// (rather than in EngineShadows.cpp, which also reads it) because this is
// the hotter of the two call sites: CEngineAnimMesh::sub_5855E0 runs twice
// per creature per frame, and creatures dominate the Shadows=3 cost.
flametal_config::define_flame_option<bool> o_flametal_shadowCache(
    "flametal:ShadowCache", flametal_config::OG_Config,
    "Cache rasterized creature/object shadow silhouettes at high shadow "
    "detail (Shadows>=3) instead of re-projecting and re-rasterizing them "
    "every frame",
    true);


// NOTE: this is the CEngineAnimMesh counterpart of
// dk2::CEngineDynamicMesh::shadow_sub_5808E0 (see src/dk2/EngineShadows.cpp,
// which documents the shared translation conventions -- roundedAdd/roundedMul
// etc, the "single rounding per x87 op" style, and the ShadowLightRef view of
// the light-collection pointer). Read that file first.
//
// It is called twice, back to back, from CEngineAnimMesh's scene-add
// (0x584900, still untranslated) exactly like shadow_sub_5808E0 is called
// twice from CEngineDynamicMesh's scene-add (0x580EC0).
//
// Despite the strong structural resemblance to shadow_sub_5808E0, this is NOT
// a byte-for-byte copy of that function: several details differ in ways that
// were verified against the raw disassembly (0x5855E0..0x585AD0) rather than
// assumed by analogy. See the inline notes below for each one.

namespace {

float roundedAdd(float a, float b) {
    return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a), _mm_set_ss(b)));
}

float roundedSub(float a, float b) {
    return _mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(a), _mm_set_ss(b)));
}

float roundedMul(float a, float b) {
    return _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(a), _mm_set_ss(b)));
}

float roundedDiv(float a, float b) {
    return _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(a), _mm_set_ss(b)));
}

float roundedSqrt(float a) {
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(a)));
}

// Same layout as EngineShadows.cpp's ShadowLightRef: only the light's Vec3f
// position (at +0x8) is read by this function, and only its x/y (never z, in
// either sibling function).
#pragma pack(push, 1)
struct AnimShadowLightRef {
    uint8_t unused_flags[8];
    dk2::Vec3f position;
};
#pragma pack(pop)

const float *floatAt(uintptr_t address) {
    return reinterpret_cast<const float *>(address);
}

// 0058566D..00585866 (part of sub_5855E0 itself, split out for readability):
// build the light-relative shadow-projection matrix into a scratch Mat3x3f
// that the original then `rep movsd`s into the caller's stack (see below).
void buildAnimShadowMatrix(
        dk2::CEngineAnimMesh &mesh,
        dk2::CAnimMeshResource &resource,
        int lightIndex,
        dk2::Mat3x3f &shadowMatrix) {
    // 005855F0..00585667: shared setup for both branches below. Note the
    // *scale* factor here is mesh.field_5C (offset 0x60), not field_54/0x58
    // -- confirmed by reading [esi+0x60] directly off the disassembly, cross
    // checked against CAnimMeshResource::cubeScale (offset 0xc) being the
    // other multiplicand, exactly mirroring shadow_sub_5808E0's
    // `resource.scale * mesh.field_54` shape but with a different field.
    const float matrixDivisor = roundedDiv(
            *floatAt(0x0066FC44), roundedMul(resource.cubeScale, mesh.field_5C));

    // NOTE: this threshold is a literal `cmp eax, 0x3` (0x58563f: 83 f8 03),
    // independently of the `cmp eax, 0x2` used above by the caller to decide
    // how to acquire the shadow surface handle (see sub_5855E0 below) --
    // unlike shadow_sub_5808E0, which reuses a single `g_shadowLevel >= 3`
    // check (a cached register compare) for both decisions. Verified by
    // reading the raw opcode bytes at both compare sites; this is a genuine
    // difference between the two functions, not a translation slip.
    if (dk2::g_shadowLevel >= 3) {
        // 0058566D..00585866: light-relative orientation.
        //
        // mesh.field_58 (offset 0x5C) plays exactly the role
        // CEngineDynamicMesh::f58_pTrgObj plays in shadow_sub_5808E0 (same
        // struct offset, same "+0x38 -> array of light pointers" shape) but
        // CEngineAnimMesh.h hasn't been given that name yet, so this is a
        // raw-offset access annotated the same way f58_pTrgObj originally was.
        const auto *lightBase = reinterpret_cast<const uint8_t *>(
                static_cast<intptr_t>(mesh.field_58));
        const auto *const *lights = reinterpret_cast<const AnimShadowLightRef *const *>(
                lightBase + 0x38);
        const AnimShadowLightRef *light = lights[lightIndex];

        dk2::Vec3f transformedPos;
        mesh.f10_matrix.multiplyVec(&transformedPos, &resource.pos);

        const float a = roundedAdd(transformedPos.x, mesh.field_4.x);
        const float b = roundedAdd(transformedPos.y, mesh.field_4.y);
        const float dx = roundedSub(a, light->position.x);
        const float dy = roundedSub(b, light->position.y);
        // ndx/e1 are literally -dx/-dy (re-fetched from light->position and
        // re-subtracted in the original rather than negated -- IEEE-754
        // subtraction is exact under sign flip, but we mirror the actual
        // instructions instead of relying on that identity).
        const float ndx = roundedSub(light->position.x, a);
        const float e1 = roundedSub(light->position.y, b);

        // Unlike shadow_sub_5808E0, where len2 comes from an independent pair
        // (dw = resource.pos.x - b, dz = transformedPos.z - a), here len2 is
        // built from ndx/e1 -- i.e. mathematically len2 == len1 (both equal
        // sqrt(dx^2 + dy^2)), just computed via a separate rounding
        // sequence. Confirmed by tracing the x87 stack instruction-by-
        // instruction; this isn't a mistake in the transcription, the two
        // sibling functions simply differ here (the animated-mesh shadow
        // projection has no z-tilt term).
        const float len1 = roundedSqrt(roundedAdd(roundedMul(dx, dx), roundedMul(dy, dy)));
        const float len2 = roundedSqrt(roundedAdd(roundedMul(ndx, ndx), roundedMul(e1, e1)));

        const float v0 = roundedMul(len1, *floatAt(0x0066FBF8));
        const float v1 = roundedDiv(*floatAt(0x0066FC28), len2);
        const float v2 = roundedMul(v0, *floatAt(0x0066FC48));
        // TODO(verify): as in shadow_sub_5808E0, the original multiplies by a
        // qword (double) constant here (0x585748: fmul qword ptr
        // [0x66FC50]); widened-then-rounded once to match this repo's
        // single-rounding-per-op convention rather than modelling 80-bit-vs-
        // 64-bit operand width precisely.
        const float v3 = roundedMul(v2, static_cast<float>(*reinterpret_cast<const double *>(0x0066FC50)));
        const float angle = roundedDiv(v3, *floatAt(0x0066FBFC));

        // 0058575A..005857AE: two products (not one, unlike
        // shadow_sub_5808E0's single `p`), reused directly as matrixQ
        // elements below.
        const float p = roundedMul(v1, ndx);
        const float q = roundedMul(v1, e1);

        dk2::Mat3x3f rotation;
        rotation.init_rotationMat(0, angle);

        // 00585762..005857D6: hand-built matrix, verified field-by-field via
        // the stack-offset normalizer (each of these 9 stores/immediates maps
        // to a distinct, non-overlapping slot of a single Mat3x3f-sized
        // region): row2 is the untouched identity row [0,0,1], as in
        // shadow_sub_5808E0, but row0/row1 here are [q,p,0]/[-p,q,0] -- a
        // clean 2x2 (q,p)/(-p,q) block, not shadow_sub_5808E0's [p,b,0]/
        // [-p,p,0] (which reused `b` directly rather than a second product).
        dk2::Mat3x3f matrixQ{};
        matrixQ.m[0][0] = q;    matrixQ.m[0][1] = p;    matrixQ.m[0][2] = 0.0f;
        matrixQ.m[1][0] = -p;   matrixQ.m[1][1] = q;    matrixQ.m[1][2] = 0.0f;
        matrixQ.m[2][0] = 0.0f; matrixQ.m[2][1] = 0.0f; matrixQ.m[2][2] = 1.0f;

        // 005857DD..00585866: shadowMatrix = ((rotation * matrixQ) * f10_matrix) * matrixDivisor.
        // Verified instruction-by-instruction via the same push/pop offset
        // simulator used for shadow_sub_5808E0 (fixed here to correctly
        // reset at branch targets instead of tracking straight-line/textual
        // order, which is what shadow_sub_5808E0's original write-up warns
        // about catching real errors with).
        dk2::Mat3x3f combined1;
        rotation.sub_594CB0(&combined1, &matrixQ);
        dk2::Mat3x3f combined2;
        combined1.sub_594CB0(&combined2, &mesh.f10_matrix);
        combined2.multiply(&shadowMatrix, matrixDivisor);
        return;
    }

    // 00585821..00585866: fixed low-detail orientation (no light lookup).
    // Same angle formula as shadow_sub_5808E0's low branch (identical
    // constants 0x66FBF8 * 0x66FC20).
    const float angle = roundedMul(*floatAt(0x0066FBF8), *floatAt(0x0066FC20));
    dk2::Mat3x3f rotation;
    rotation.init_rotationMat(0, angle);

    // Unlike shadow_sub_5808E0's low branch (which we now believe misread an
    // uninitialized-looking slot -- see the correction in
    // EngineShadows.cpp), this branch's own disassembly is unambiguous:
    // 00585842 reads the exact same slot matrixDivisor was stored to at
    // 00585663, with zero net stack growth in between (verified via the
    // offset normalizer). So the first multiply's scale is matrixDivisor,
    // not a placeholder.
    dk2::Mat3x3f scaled;
    rotation.multiply(&scaled, matrixDivisor);
    scaled.multiply(&shadowMatrix, mesh.field_5C);
}

// ---------------------------------------------------------------------------
// Shadow silhouette coverage cache (flametal:ShadowCache).
//
// Investigation (disassembly of 0x58E3E0 shadows_begin_ge23, 0x58E470
// shadows_end_58E470, and the 32x32-surface-pool init block at 0x58E330,
// cross-checked against the already-translated dk2::shadows_process_58E080
// in Shadows.cpp and its dk2_globals.h names):
//
//   * shadows_begin_ge23() ALWAYS points the global `shadows_lpSurface`
//     (0x78095C) at the SAME fixed 1024-byte scratch buffer,
//     `shadows_surfaceData` (0x780A68, a 32x32 8x-supersampled coverage
//     grid, stride `shadows_dword_780A64` == 0x20) -- and unconditionally
//     zeroes all 1024 bytes of it on every single call via `rep stosd`.
//     This is the buffer shadows_process_58E080 rasterizes triangles into.
//   * Separately (and this is easy to conflate with the above), it also
//     advances a 64-entry ring (`shad_dword_780E68` mod 0x40 indexing
//     `g_MyEntryBuf_MyScaledSurface_idxs[64]`) to choose which of 64
//     pre-created MyScaledSurface blit targets shadows_end_58E470() will
//     bake the *finished* coverage into for actual rendering. That pool is
//     initialized once (0x58E330) and never material/mesh-keyed -- slots
//     are handed out purely round-robin, one per shadows_begin_ge23() call.
//
//   Consequence: neither the coverage buffer nor the output surface is
//   pooled/retained per-mesh or per-frame in a way we could key on and
//   reuse across frames. The coverage buffer is single, global, and
//   destroyed (zeroed) on the very next call; the 64-slot output pool
//   wraps within a single frame once shadow-caster count exceeds 64 (this
//   game commonly has ~65 creatures alone, before dynamic-mesh casters),
//   so a slot's *identity* this frame has no relation to which creature
//   held it last frame. Retaining "the surface handle" is therefore unsafe.
//
//   What IS safe and sufficient: the coverage bitmap is a pure function of
//   (resource geometry at this LOD, the decoded per-frame vertex pose, and
//   the finished light/orientation shadowMatrix). Copy those 1024 bytes out
//   after a real rasterization pass, and splice them back into
//   shadows_surfaceData (between shadows_begin_ge23() and
//   shadows_end_58E470()) on a cache hit -- shadows_end_58E470() then bakes
//   an identical result into whichever pool slot it would have anyway,
//   completely unaware anything was skipped.
//
// Cache key: (resource pointer, LOD, the exact bit pattern of the finished
// shadowMatrix, and the creature's animation state). The matrix is compared
// bit-exact (no quantization): while a creature is idle or walking in a
// straight line it recomputes to the identical bits frame over frame (the
// world-position terms cancel via the `- resource->pos` subtraction before
// projection), so exact comparison already gets real hits without adding
// visible lag on top of the deliberate frame quantization below.
// field_60/field_64 (the resource->sub_57E5B0 blend inputs) are the only
// lossy part: field_60 is quantized to int(...) per the task's stated
// tolerance (pose may lag pose changes by at most one quantization step);
// field_64 is already a small integer and kept exact.
namespace shadow_cache {

#pragma pack(push, 1)
struct AnimShadowCacheKey {
    const void *resource;
    int lod;
    int quantFrame;          // int(field_60) -- see note above
    int field64;              // field_64, exact (already integer)
    uint32_t matrixBits[9];  // raw bits of the finished shadowMatrix
};
#pragma pack(pop)

struct AnimShadowCacheEntry {
    bool valid = false;
    AnimShadowCacheKey key{};
    uint8_t coverage[1024];
};

// Fixed-size, direct-mapped (hash-and-evict-on-collision): bounded memory,
// O(1) lookup, no bookkeeping beyond the entry itself. 256 slots comfortably
// covers the handful of distinct (mesh, pose, orientation) combinations
// active at once -- far more than the ~65-creature scene this is sized for.
constexpr size_t kCacheSize = 256;
AnimShadowCacheEntry g_cache[kCacheSize];
int g_cacheSession = -1;

uint32_t fnv1a(const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

// Returns the slot this key maps to, and whether it already holds a valid,
// matching entry. On a scene-session change (level load/unload -- tracked
// the same way CreateMeshes.cpp tracks g_ddSceneSessionId) the whole cache
// is dropped rather than trusting per-entry staleness: resource pointers
// can be freed and reused across a level transition, and a coincidental
// pointer match must never resurrect a stale silhouette for the wrong mesh.
AnimShadowCacheEntry &lookup(const AnimShadowCacheKey &key, bool &hit) {
    if (dk2::g_ddSceneSessionId != g_cacheSession) {
        for (auto &entry : g_cache) entry.valid = false;
        g_cacheSession = dk2::g_ddSceneSessionId;
    }
    const uint32_t slot = fnv1a(&key, sizeof(key)) % kCacheSize;
    AnimShadowCacheEntry &entry = g_cache[slot];
    hit = entry.valid && std::memcmp(&entry.key, &key, sizeof(key)) == 0;
    return entry;
}

}  // namespace shadow_cache

}  // namespace


// 005855E0..00585AD0
int dk2::CEngineAnimMesh::sub_5855E0(CAnimMeshResource *resource, int lightIndex) {
    // 005855E6..0058563A: acquire the shadow surface handle. This threshold
    // (`cmp eax, 0x2`, opcode bytes 83 f8 02 at 0x5855F0) is one level more
    // permissive than the `>= 3` used just below (and in shadow_sub_5808E0)
    // for the light-relative-matrix decision -- verified from the raw
    // opcodes, not assumed by analogy with the dynamic-mesh sibling.
    int shadowHandle;
    if (g_shadowLevel >= 2) {
        shadowHandle = shadows_begin_ge23();
    } else {
        const char *name = MyStringHashMap_MyMeshResourceHolder_instance
                .entries.buf[f50_pMeshHolder->mapIdx].name;
        const int surfaceId = MyEntryBuf_MyScaledSurface_addFormatEnfineShadow(name, 0);
        shadowHandle = shadows_begin_lt23(surfaceId);
    }

    // TODO(verify): field_78 (the active LOD, same semantic role as
    // shadow_sub_5808E0's field_6C -- see CEngineAnimMesh.cpp's
    // `const uint32_t lod = static_cast<uint32_t>(field_78);`) is read into a
    // register (ebx) before the two branches above run. The low-detail
    // (`g_shadowLevel < 2`) branch then unconditionally zeroes that same
    // register while building its MyEntryBuf_MyScaledSurface_addFormatEnfineShadow
    // argument, and -- confirmed by an exhaustive grep over every instruction
    // that touches ebx in this function -- it is never reloaded from field_78
    // afterwards. Unlike shadow_sub_5808E0's analogous spot (a stack slot,
    // genuinely ambiguous), this one is a hard register with a fully traced
    // lifetime, so there is no ambiguity: at low shadow detail this function
    // really does always rasterize LOD 0, regardless of the mesh's actual
    // active LOD.
    const int lod = (g_shadowLevel < 2) ? 0 : field_78;

    Mat3x3f shadowMatrix;
    buildAnimShadowMatrix(*this, *resource, lightIndex, shadowMatrix);

    // flametal:ShadowCache -- only meaningful on the shadows_begin_ge23()
    // branch above (g_shadowLevel >= 2): that is the only case where
    // shadows_lpSurface/shadows_dword_780A64 actually point at the
    // rasterizable shadows_surfaceData scratch buffer (see the cache
    // writeup above buildAnimShadowMatrix's definition). The <2 branch's
    // surface is unrelated and already cheap via its own material-name
    // cache, so it is left untouched.
    const bool cachingActive = g_shadowLevel >= 2 && o_flametal_shadowCache.get();
    bool cacheHit = false;
    shadow_cache::AnimShadowCacheEntry *cacheEntry = nullptr;
    if (cachingActive) {
        shadow_cache::AnimShadowCacheKey key{};
        key.resource = resource;
        key.lod = lod;
        key.quantFrame = static_cast<int>(field_60);
        key.field64 = field_64;
        std::memcpy(key.matrixBits, &shadowMatrix, sizeof(key.matrixBits));
        cacheEntry = &shadow_cache::lookup(key, cacheHit);
        if (cacheHit) {
            std::memcpy(dk2::shadows_surfaceData, cacheEntry->coverage,
                        sizeof(cacheEntry->coverage));
        } else {
            cacheEntry->key = key;
        }
    }

    // 00585872..00585AA6: for every SprsAnimHeader entry of the resource,
    // project its "Ex" vertices through shadowMatrix into the
    // dk2::g_vec_766A78 scratch array, then rasterize each triangle of the
    // current LOD via shadows_process_58E080. Skipped entirely on a cache
    // hit: the coverage buffer was just restored above, byte-for-byte
    // identical to what this loop would have produced.
    if (!cacheHit) {
    Vec3f *scratch = &g_vec_766A78;
    for (int i = 0; i < resource->sprsCount; ++i) {
        SprsAnimHeader &entry = resource->buf[i];

        // SprsAnimHeader.h declares MyScaledSurface_idx as int16_t, but
        // 00585897 zero-extends it (`xor eax,eax; mov ax,[edi+0x58]`) before
        // using it as the index, so we do the same rather than letting a
        // signed int16_t->int conversion sign-extend it.
        const MyScaledSurface *surface = MyEntryBuf_MyScaledSurface_getByIdx(
                static_cast<uint16_t>(entry.MyScaledSurface_idx));
        if (surface->drawFlags & 0x1023) continue;

        // 005858B3..0058594E: project every extra ("Ex") vertex of this entry
        // through shadowMatrix, relative to resource->pos.
        //
        // Unlike shadow_sub_5808E0 (which looks up a MeshVertEx::index
        // through an explicit per-vertex indices array before fetching raw
        // geometry), this loop passes the vertex loop counter straight
        // through to CAnimMeshResource::sub_57E5B0 as both `vertexIndex` and,
        // implicitly, `animation` = the current entry index `i` (this
        // matches CEngineAnimMesh::sub_5836A0's own use of sub_57E5B0, which
        // does the AnimVertEx_base[...].index indirection *inside* the
        // callee instead). sub_57E5B0 also folds in the frame interpolation,
        // so there's no separate "geom" fetch here.
        if (entry.numVertsEx != 0) {
            for (uint16_t v = 0; v < entry.numVertsEx; ++v) {
                Vec3f decoded;
                resource->sub_57E5B0(i, field_60, static_cast<uint32_t>(field_64),
                                     static_cast<int>(v), &decoded);

                Vec3f relative{
                        roundedSub(decoded.x, resource->pos.x),
                        roundedSub(decoded.y, resource->pos.y),
                        roundedSub(decoded.z, resource->pos.z)};

                // NOTE: Mat3x3f::sub_594E10, not multiplyVec -- confirmed by
                // both the (input, output) push order at the call site and by
                // src/dk2/Mat3x3f.cpp's existing implementation, which
                // already names its parameters (input, output) in that
                // order (multiplyVec is (output, input)). Functionally the
                // same row-combine as multiplyVec (see Mat3x3f.cpp), so this
                // doesn't change behaviour, but the call is reproduced as
                // written rather than silently normalized to multiplyVec.
                Vec3f projected;
                shadowMatrix.sub_594E10(&relative, &projected);

                const float bias = *floatAt(0x0066FC58);
                scratch[v] = Vec3f{
                        roundedSub(projected.x, bias),
                        roundedSub(projected.y, bias),
                        projected.z};
            }
        }

        // 00585954..00585A80: rasterize every triangle of the current LOD.
        // Same field-name caveat as shadow_sub_5808E0's SprsMeshHeader
        // access: here the byte count comes from entry.lod_list[lod]
        // (offset 0x4) and the pointer from entry.plod_list[lod] (offset
        // 0x14) -- SprsAnimHeader.h happens to name the count array
        // "lod_list" and the pointer array "plod_list", the reverse of
        // SprsMeshHeader's "triangleCount_list"/"pvertice_list" naming, but
        // the offsets read here match those declared fields exactly.
        const uint8_t triangleCount = static_cast<uint8_t>(entry.lod_list[lod]);
        const Triangle *triangles = entry.plod_list[lod];
        for (uint8_t t = 0; t < triangleCount; ++t) {
            const Triangle &tri = triangles[t];

            const auto toInt = [](float value) -> int32_t {
                const float biased = roundedSub(
                        roundedSub(value, *floatAt(0x0066FC3C)), *floatAt(0x0066FC40));
                int32_t bits;
                std::memcpy(&bits, &biased, sizeof(bits));
                return (bits & 0x7FFFFF) - 0x400000;
            };

            const int32_t x0 = toInt(scratch[tri.z].x);
            const int32_t y0 = toInt(scratch[tri.z].y);
            const int32_t x1 = toInt(scratch[tri.y].x);
            const int32_t y1 = toInt(scratch[tri.y].y);
            const int32_t x2 = toInt(scratch[tri.x].x);
            const int32_t y2 = toInt(scratch[tri.x].y);
            shadows_process_58E080(x0, y0, x1, y1, x2, y2);
        }
    }
    }  // !cacheHit

    if (cachingActive && !cacheHit) {
        std::memcpy(cacheEntry->coverage, dk2::shadows_surfaceData,
                    sizeof(cacheEntry->coverage));
        cacheEntry->valid = true;
    }

    shadows_end_58E470();
    // 00585ab1 reloads the return value from the same stack slot
    // shadowHandle was stored to (verified via the offset normalizer, same
    // pattern as shadow_sub_5808E0's corrected return -- see
    // EngineShadows.cpp).
    return shadowHandle;
}
