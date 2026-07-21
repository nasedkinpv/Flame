#include "dk2/engine/primitive/2d/world/CEngineDynamicMesh.h"

#include "dk2/MeshVertEx.h"
#include "dk2/MyEntryBuf_MyStringHashMap_MyMeshResourceHolder_entry.h"
#include "dk2/MyMeshResourceHolder.h"
#include "dk2/MyScaledSurface.h"
#include "dk2/ShadowGpu.h"
#include "dk2/MyStringHashMap_MyMeshResourceHolder.h"
#include "dk2/MyStringHashMap_MyMeshResourceHolder_entry.h"
#include "dk2/SprsMeshHeader.h"
#include "dk2/Triangle.h"
#include "dk2/engine/primitive/resource/CPolyMeshResource.h"
#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <tools/flametal_config.h>

#include <cstdint>
#include <cstring>
#include <emmintrin.h>


// flametal:ShadowCache is registered in EngineAnimShadows.cpp (the hotter
// of the two shadow call sites -- see that file for the full investigation
// writeup of shadows_begin_ge23/shadows_end_58E470's surface lifecycle,
// which is identical for this dynamic-mesh path). Reused here rather than
// re-declared so both translation units agree on one flag.
extern flametal_config::define_flame_option<bool> o_flametal_shadowCache;


// NOTE ON shadows_process_58E080 (0058E080..0058E2C0):
// This symbol is already translated in src/dk2/Shadows.cpp, deliberately
// re-implemented (rather than a literal disassembly port) to clip the
// rasterizer to the actual 32x32 supersampled surface instead of trusting
// the original's implicit bounds assumption. Re-declaring/defining it here
// would collide with that translation unit, so this file only supplies the
// still-untranslated 0x005808E0 method.

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

// Layout of the object(s) pointed to by CEngineDynamicMesh::f58_pTrgObj when
// treated as a directional-light collection: see dk2::sceneLights() in
// Obj57BCB0.cpp (collection + 0x38 == array of light pointers). Only the
// light's Vec3f position (at +0x8) is needed here.
#pragma pack(push, 1)
struct ShadowLightRef {
    uint8_t unused_flags[8];
    dk2::Vec3f position;
};
#pragma pack(pop)

const float *floatAt(uintptr_t address) {
    return reinterpret_cast<const float *>(address);
}

// 00580943..00580B6B (part of shadow_sub_5808E0 itself, split out for
// readability): build the light-relative shadow-projection matrix (copied in
// the original, via `rep movsd`, into a scratch Mat3x3f on the caller's stack)
// used by the per-vertex projection loop below. Returns whatever the
// original left in EAX at that point (see caller for why it is effectively
// unused).
int buildShadowMatrix(
        dk2::CEngineDynamicMesh &mesh,
        dk2::CPolyMeshResource &resource,
        int lightIndex,
        dk2::Mat3x3f &shadowMatrix) {
    // 0058096E: shared setup for both branches.
    const float matrixDivisor = roundedDiv(
            *floatAt(0x0066FC44), roundedMul(resource.scale, mesh.field_54));

    if (dk2::g_shadowLevel >= 3) {
        // 00580974..00580B09: light-relative orientation.
        const auto *lightBase = reinterpret_cast<const uint8_t *>(
                static_cast<intptr_t>(mesh.f58_pTrgObj));
        const auto *const *lights = reinterpret_cast<const ShadowLightRef *const *>(
                lightBase + 0x38);
        const ShadowLightRef *light = lights[lightIndex];

        dk2::Vec3f transformedPos;
        mesh.f10_mat.multiplyVec(&transformedPos, &resource.pos);

        const float a = roundedAdd(transformedPos.x, mesh.field_4.x);
        const float b = roundedAdd(transformedPos.y, mesh.field_4.y);
        const float dx = roundedSub(a, light->position.x);
        const float dy = roundedSub(b, light->position.y);
        const float dz = roundedSub(transformedPos.z, a);
        const float dw = roundedSub(resource.pos.x, b);

        const float len1 = roundedSqrt(roundedAdd(roundedMul(dx, dx), roundedMul(dy, dy)));
        const float len2 = roundedSqrt(roundedAdd(roundedMul(dw, dw), roundedMul(dz, dz)));

        const float v0 = roundedMul(len1, *floatAt(0x0066FBF8));
        const float v1 = roundedDiv(*floatAt(0x0066FC28), len2);
        const float v2 = roundedMul(v0, *floatAt(0x0066FC48));
        // TODO(verify): original multiplies by a qword (double) constant
        // (00580A4F: fmul qword ptr [0x66FC50]); we widen-then-round once,
        // matching the repo's single-rounding-per-op convention rather than
        // modelling 80-bit-vs-64-bit operand width precisely.
        const float v3 = roundedMul(v2, static_cast<float>(*reinterpret_cast<const double *>(0x0066FC50)));
        const float angle = roundedDiv(v3, *floatAt(0x0066FBFC));

        // 00580A97: a second product, reused directly as matrix elements
        // below (not a dead store -- see matrixQ).
        const float p = roundedMul(v1, dw);

        dk2::Mat3x3f rotation;
        rotation.init_rotationMat(0, angle);

        // 00580A69..00580ACB: rather than a second init_rotationMat call, the
        // original hand-builds a second matrix directly from `p` and `b`
        // (row2 is the untouched identity row [0,0,1]).
        dk2::Mat3x3f matrixQ{};
        matrixQ.m[0][0] = p;    matrixQ.m[0][1] = b;    matrixQ.m[0][2] = 0.0f;
        matrixQ.m[1][0] = -p;   matrixQ.m[1][1] = p;    matrixQ.m[1][2] = 0.0f;
        matrixQ.m[2][0] = 0.0f; matrixQ.m[2][1] = 0.0f; matrixQ.m[2][2] = 1.0f;

        // 00580AD4..00580B55: shadowMatrix = ((rotation * matrixQ) * f10_mat) * matrixDivisor.
        // (Verified instruction-by-instruction via an automated push/pop
        // offset simulator against the disassembly; the chained
        // sub_594CB0/sub_594CB0/multiply calls share stack slots across the
        // two branches of this function.)
        dk2::Mat3x3f combined1;
        rotation.sub_594CB0(&combined1, &matrixQ);
        dk2::Mat3x3f combined2;
        combined1.sub_594CB0(&combined2, &mesh.f10_mat);
        combined2.multiply(&shadowMatrix, matrixDivisor);

        return *reinterpret_cast<const int32_t *>(&dx);
    }

    // 00580B10..00580B5A: fixed low-detail orientation (no light lookup).
    const float angle = roundedMul(*floatAt(0x0066FBF8), *floatAt(0x0066FC20));
    dk2::Mat3x3f rotation;
    rotation.init_rotationMat(0, angle);

    // CORRECTED (was TODO(verify)): this used to substitute a deterministic
    // 0.0f here on the theory that 00580B31's read of this stack slot was
    // uninitialized along this branch. Cross-checking against the sibling
    // CEngineAnimMesh::sub_5855E0 (0x5855E0..0x585AD0, see
    // src/dk2/EngineAnimShadows.cpp) resolved it: that function has the exact
    // same shared-preamble/low-detail-branch shape, and its own
    // instruction-level (push/pop-offset-normalized) trace proves the
    // corresponding slot is never anything but matrixDivisor -- there is zero
    // net stack growth between the store at 0058096A/00580663 and the reload
    // at 00580B31/00585842 in either function, on either branch. Using 0.0f
    // here would silently zero out low-detail shadow matrices, which cannot
    // be right, so this is now matrixDivisor as in the >=3 branch.
    dk2::Mat3x3f scaled;
    rotation.multiply(&scaled, matrixDivisor);
    dk2::Mat3x3f scaledTwice;
    scaled.multiply(&scaledTwice, mesh.field_54);
    shadowMatrix = scaledTwice;
    return 0;
}

// ---------------------------------------------------------------------------
// Shadow silhouette coverage cache (flametal:ShadowCache) -- dynamic-mesh
// side. See src/dk2/EngineAnimShadows.cpp (just above CEngineAnimMesh::
// sub_5855E0) for the full disassembly-backed writeup of why this caches
// the raw 32x32 coverage bitmap rather than the engine's rotating surface
// handle. Summary: shadows_begin_ge23() always clears the SAME global
// `shadows_surfaceData` scratch buffer and separately round-robins an
// unrelated 64-slot output-surface pool, so nothing about "the surface" can
// be retained across calls -- only the coverage bytes we copy out ourselves.
//
// Key here has no anim-frame component (CPolyMeshResource meshes are not
// posed/animated the way CAnimMeshResource creatures are): the coverage
// bitmap is a pure function of (resource pointer, LOD, the finished
// shadowMatrix), so an exact bit-match on the matrix is both correct and
// sufficient -- it already yields hits for a stationary or straight-line-
// moving object (the world-position terms cancel via `- resource->pos`
// before projection, same as the anim-mesh sibling) or for two placed
// instances of the same object type sharing a facing and light.
namespace shadow_cache {

#pragma pack(push, 1)
struct DynShadowCacheKey {
    const void *resource;
    int lod;
    uint32_t matrixBits[9];  // raw bits of the finished shadowMatrix
};
#pragma pack(pop)

struct DynShadowCacheEntry {
    bool valid = false;
    DynShadowCacheKey key{};
    uint8_t coverage[1024];
};

constexpr size_t kCacheSize = 256;
DynShadowCacheEntry g_cache[kCacheSize];
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

// Same session-keyed invalidation as the anim-mesh cache: dropped whole on
// a level transition so a freed-then-reused CPolyMeshResource pointer can
// never coincidentally hit a stale entry from the previous level.
DynShadowCacheEntry &lookup(const DynShadowCacheKey &key, bool &hit) {
    if (dk2::g_ddSceneSessionId != g_cacheSession) {
        for (auto &entry : g_cache) entry.valid = false;
        g_cacheSession = dk2::g_ddSceneSessionId;
    }
    const uint32_t slot = fnv1a(&key, sizeof(key)) % kCacheSize;
    DynShadowCacheEntry &entry = g_cache[slot];
    hit = entry.valid && std::memcmp(&entry.key, &key, sizeof(key)) == 0;
    return entry;
}

}  // namespace shadow_cache

}  // namespace


// 005808E0..00580DEA
int dk2::CEngineDynamicMesh::shadow_sub_5808E0(CPolyMeshResource *resource, int lightIndex) {
    // 005808F1..00580938: acquire the shadow surface handle. High shadow
    // detail (g_shadowLevel >= 3) uses the real silhouette rasterizer path
    // (see below); low detail resolves a cached/blob shadow surface by the
    // mesh's material name.
    // TODO(verify): 00580C7B reloads the LOD index from a stack slot rather
    // than keeping field_6C live in a register. Our instruction-level
    // (push/pop-offset) reconstruction of that slot resolves it, in the
    // g_shadowLevel>=3 branch, to the address of f10_mat rather than a small
    // LOD integer -- almost certainly a mistake in our offset bookkeeping
    // (that value cannot plausibly serve as a 0..15 array index into
    // SprsMeshHeader::triangleCount_list/pvertice_list without the shipped
    // game crashing). We use field_6C directly, consistent with its
    // confirmed meaning as the active LOD in
    // CEngineDynamicMesh::sub_581BE0 (see CEngineDynamicMesh.cpp); please
    // double-check against a live trace/debugger before relying on this.
    const int lod = field_6C;
    int shadowHandle;
    if (g_shadowLevel >= 3) {
        shadowHandle = shadows_begin_ge23();
    } else {
        const char *name = MyStringHashMap_MyMeshResourceHolder_instance
                .entries.buf[f34_pMyMeshResourceHolder->mapIdx].name;
        const int surfaceId = MyEntryBuf_MyScaledSurface_addFormatEnfineShadow(name, 0);
        shadowHandle = shadows_begin_lt23(surfaceId);
    }

    Mat3x3f shadowMatrix;
    const int setupResult = buildShadowMatrix(*this, *resource, lightIndex, shadowMatrix);
    (void) setupResult;  // buildShadowMatrix's return value is never stored back
                         // to the [esp+0x30] slot the real function returns from
                         // (only shadowHandle is written there); EAX from
                         // buildShadowMatrix is simply clobbered before the
                         // function's actual `mov eax,[esp+0x30]; ret 8` epilogue.

    // flametal:ShadowCache -- only meaningful on the shadows_begin_ge23()
    // branch above (g_shadowLevel >= 3): that is the only case where
    // shadows_lpSurface/shadows_dword_780A64 point at the rasterizable
    // shadows_surfaceData scratch buffer (see shadow_cache's writeup
    // above). The <3 branch is already cheap via its own material-name
    // blob cache and is left untouched.
    const bool cachingActive = g_shadowLevel >= 3 && o_flametal_shadowCache.get() &&
                               !dk2::shadowgpu::active();
    bool cacheHit = false;
    shadow_cache::DynShadowCacheEntry *cacheEntry = nullptr;
    if (cachingActive) {
        shadow_cache::DynShadowCacheKey key{};
        key.resource = resource;
        key.lod = lod;
        std::memcpy(key.matrixBits, &shadowMatrix, sizeof(key.matrixBits));
        cacheEntry = &shadow_cache::lookup(key, cacheHit);
        if (cacheHit) {
            std::memcpy(dk2::shadows_surfaceData, cacheEntry->coverage,
                        sizeof(cacheEntry->coverage));
        } else {
            cacheEntry->key = key;
        }
    }

    // 00580B7F..00580DD1: for every SprsMeshHeader LOD entry of the resource,
    // project its vertices through shadowMatrix into the dk2::g_vec_766A78
    // scratch array, then rasterize each triangle of the current LOD via
    // shadows_process_58E080. Skipped entirely on a cache hit: the coverage
    // buffer was just restored above, byte-for-byte identical to what this
    // loop would have produced.
    if (!cacheHit) {
    Vec3f *scratch = &g_vec_766A78;
    for (int i = 0; i < resource->sprsCount; ++i) {
        SprsMeshHeader &entry = resource->ptr[i];

        const MyScaledSurface *surface = MyEntryBuf_MyScaledSurface_getByIdx(entry.MyScaledSurface_idx);
        if (surface->drawFlags & 0x1023) continue;

        // 00580BB2..00580C75: project every extra ("Ex") vertex of this LOD
        // through shadowMatrix, relative to resource->pos.
        if (entry.numVertsEx != 0) {
            for (uint16_t v = 0; v < entry.numVertsEx; ++v) {
                // Only MeshVertEx::index is read here (the byte cursor in the
                // original steps by sizeof(MeshVertEx) == 0x14 per entry).
                const int32_t vertexIndex = entry.MeshVertEx_base[v].index;
                const Vec3f &geom = resource->geom_base[vertexIndex];

                Vec3f relative{
                        roundedSub(geom.x, resource->pos.x),
                        roundedSub(geom.y, resource->pos.y),
                        roundedSub(geom.z, resource->pos.z)};

                Vec3f projected;
                shadowMatrix.multiplyVec(&projected, &relative);

                const float bias = *floatAt(0x0066FC58);
                scratch[v] = Vec3f{
                        roundedSub(projected.x, bias),
                        roundedSub(projected.y, bias),
                        projected.z};
            }
        }

        // 00580C7F..00580DAB: rasterize every triangle of the current LOD.
        const uint8_t triangleCount = entry.triangleCount_list[lod];
        const Triangle *triangles = entry.pvertice_list[lod];
        for (uint8_t t = 0; t < triangleCount; ++t) {
            const Triangle &tri = triangles[t];

            // 00580C95..00580D96: convert each projected vertex's x/y into an
            // integer pixel coordinate via the classic "add 1.5 * 2^23, mask
            // the mantissa, subtract 0x400000" float-to-int trick, replicated
            // as two separate roundings (matching the two fsub instructions
            // per component in the original).
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
    // CORRECTED (was `return setupResult;`): 00580DDC reloads the return value
    // from the same stack slot shadowHandle was stored to earlier (verified
    // via instruction-level stack-offset normalization -- no net esp change
    // between the store and this reload on either shadowHandle-acquisition
    // branch), not from wherever buildShadowMatrix happened to leave EAX.
    return shadowHandle;
}
