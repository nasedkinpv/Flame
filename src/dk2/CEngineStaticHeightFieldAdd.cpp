#include "dk2/engine/primitive/CEngineStaticHeightField.h"

#include "dk2/MyCESurfHandle.h"
#include "dk2/MyCESurfScale.h"
#include "dk2/MyScaledSurface.h"
#include "dk2/Obj57AD20.h"
#include "dk2/SceneObject2E.h"
#include "dk2/SceneObject2EList.h"
#include "dk2/utils/MyCameraState.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"
#include "patches/logging.h"

#include <cstdint>
#include <cstring>
#include <emmintrin.h>

// dk2::CEngineStaticHeightField::appendToSceneObject2EList, 0x00587060.
//
// Traced via Ghidra (headless decompile + GUI, since the auto-analyzer never
// created a function here at all -- 0x587060 is reached only through the
// vtable at 0x66FDB4, and Ghidra's headless call-graph does not resolve
// virtual dispatch; had to manually Disassemble+Create Function in the GUI).
//
// This is the terrain/heightfield sibling of the already-translated
// dk2::CEngineStaticMesh::appendToSceneObject2EList (0x586190, see
// CEngineStaticMeshAdd.cpp) -- same shape (guards, camera-space cull test,
// light-selection mask, LOD pick, SceneObject2E append), but simpler: a
// heightfield chunk has exactly one MyScaledSurface (this->field_10, a plain
// index), not a list of per-part SubmeshRecords, so there is no per-part
// loop and no "two surfaces / three emission shapes" branching.
//
// High-level structure (cross-referenced field-for-field against the
// generated CEngineStaticHeightField.h -- a8/gap_C/pObj57AD20/field_10/
// field_14/field_18/field_1C at +8/+C/+10/+14/+18/+1C/+20 line up exactly
// with the request-copy + cull + LOD logic below):
//
//  1. Two early-out guards on `a8` bits 0x8/0x10 against the same
//     undocumented globals 0x00760B60/0x00760B84 as the static-mesh sibling
//     (see that file's header comment -- TODO(verify): still no name for
//     these anywhere in the API). Unlike the static-mesh sibling these are
//     combined into one `&&` (both must pass, not two sequential early
//     returns) -- confirmed by the decompiled boolean structure, not just
//     assumed identical.
//
//  2. Copies the request (this->fun_582CE0-style opaque struct pointer,
//     same per-call convention as the static-mesh sibling's SceneAddRequest)
//     into `this`: `gap_C` <- request->collection (word[4]), `field_18` <-
//     request->fieldC/"ignore distance cull" flag (word[6]), `field_1C`
//     <- request->word0/word4 packed as two uint16.
//
//  3. camPos-relative position -> camera space, exactly like the static-mesh
//     sibling (`Vec3f::substractAssign` + `Mat3x3f::sub_594E10` against
//     g_camState), read directly off `pObj57AD20->vec`/`f20` (this-> holds a
//     pointer to the same Obj57AD20 the static-mesh sibling holds by value).
//     Cull test via `Vec3f_static_sub_575D70`, skippable via the "ignore
//     distance cull" flag exactly like the sibling.
//
//  4. `Vec3f_static_sub_575F10` for the reduction/LOD scale factor (float
//     output only, same as the sibling).
//
//  5. Inlined `dk2::Obj57AD20::sub_57AC10` light-selection mask (the same
//     already-translated helper the static-mesh sibling inlines) --
//     `pObj57AD20->sub_57AC10(gap_C)`. TODO(verify): the decompiler shows
//     this as a bare call with one argument (no visible `this` load), so the
//     implicit-ECX receiver is inferred from `sub_57AC10`'s already-known
//     real signature (`int Obj57AD20::sub_57AC10(int *)`, fixed earlier this
//     session) and from `pObj57AD20` being the only Obj57AD20 in scope here
//     -- not confirmed against the raw `mov ecx,...` byte.
//
//  6. Combines draw flags directly from `a8` (NOT via the shared
//     `transformFlags` helper the static-mesh sibling calls -- this is a
//     smaller, inlined bit-mask tailored to heightfield-specific `a8` bits
//     0x2/0x4/0x20/0x2000/0x2001, verified against the raw AND/OR sequence):
//       a8 & 0x2001 -> andMask &= 0xFFFFFF7E, orMask |= 0x220
//       a8 & 0x2000 && *0x764BB8 & 0x40 -> orMask |= 0x4000
//       a8 & 0x2   -> andMask &= 0xFFFFFF1F, orMask |= 0x201
//       a8 & 0x20  -> andMask &= 0xFFFFFBFF
//       a8 & 0x4   -> andMask &= 0xFFFFFFBF
//     combined = (andMask & surf->drawFlags) | orMask.
//
//  7. LOD pick: metric = (pObj57AD20->f20 * reductionFactor) /
//     baseHandle->surfWidth8 (same formula as the static-mesh sibling, just
//     compared against fixed 1.0/0.5/0.25 thresholds here instead of three
//     undocumented float constants). `surf->scaledSurfArr[lodLevel]` is the
//     handle -- heightfield has no per-part width/height probability grid,
//     so there is no mantissa-bit fine-index like the static-mesh sibling.
//     TODO(verify): a defensive `prob_height < 1` branch subtracts 1 from
//     the index base; this looks unreachable in practice (a real heightfield
//     surface should always have prob_height >= 1) and is mirrored as-is
//     rather than "fixed" -- see the equivalent caution in
//     Obj57AD20.cpp about not silently correcting the original's behavior.
//
//  8. `MyCESurfHandle_static_addToHashList_flagsOr400` on the picked handle,
//     then a single SceneObject2E append (heightfield never needs the
//     static-mesh sibling's two-surface/three-branch logic): `lod__triangleCount`
//     and `numVertsEx` are computed from `field_14` (a grid-width-like value)
//     as `field_14^2 * 2` and `(field_14+1)^2` respectively, not read from a
//     per-part record -- verified against the raw `imul`/`shl` sequence.
//
// Callee census: Vec3f::substractAssign (0x41C4C0) x1, Mat3x3f::sub_594E10
// (0x594E10) x1, Vec3f_static_sub_575D70 (0x575D70) x1,
// Vec3f_static_sub_575F10 (0x575F10) x1, Obj57AD20::sub_57AC10 (0x57AC10,
// already translated in Obj57AD20.cpp) x1, MyEntryBuf_MyScaledSurface_getByIdx
// (0x57C780) x1, MyCESurfHandle_static_addToHashList_flagsOr400 (0x589140)
// x1, SceneObject2EList::objects2EToDraw_enlarge (0x579FD0) x-as-needed.

namespace {

float roundedMul(float a, float b) {
    return _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(a), _mm_set_ss(b)));
}

float roundedDiv(float a, float b) {
    return _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(a), _mm_set_ss(b)));
}

// The "int" argument is really a pointer to the same small per-call request
// struct the static-mesh sibling calls SceneAddRequest -- same field offsets
// (verified against the raw movs copying into gap_C/field_18/field_1C here).
#pragma pack(push, 1)
struct SceneAddRequest {
    uint16_t word0;       // +0x00 -> this->field_1C low word
    uint16_t unused2;
    uint16_t word4;       // +0x04 -> this->field_1C high word
    uint16_t unused6;
    int32_t collection;   // +0x08 -> this->gap_C (light/sub_57AC10 arg)
    int32_t ignoreCull;    // +0x0C -> this->field_18 ("ignore distance cull")
};
#pragma pack(pop)

// wip: instrumentation for the "checkerboard holes in unclaimed rock"
// investigation (2026-07-24) -- logs every ~256 calls so we can see, live,
// how often each bail-out actually fires relative to total calls/appends.
// Remove once the cause is confirmed.
struct HeightFieldStats {
    uint32_t calls = 0, guard1Bail = 0, guard2Bail = 0, cullFail = 0, nullSurf = 0,
             nullBaseHandle = 0, nullHandle = 0, zeroTriangles = 0, appended = 0;
};
HeightFieldStats g_hfStats;

}  // namespace

int dk2::CEngineStaticHeightField::appendToSceneObject2EList(int requestArg) {
    ++g_hfStats.calls;
    if (g_hfStats.calls % 256 == 0) {
        patch::log::dbg("heightfield append stats: calls=%u guard1Bail=%u guard2Bail=%u "
                        "cullFail=%u nullSurf=%u nullBaseHandle=%u nullHandle=%u "
                        "zeroTriangles=%u appended=%u maxCount=%d count=%u",
                        g_hfStats.calls, g_hfStats.guard1Bail, g_hfStats.guard2Bail,
                        g_hfStats.cullFail, g_hfStats.nullSurf, g_hfStats.nullBaseHandle,
                        g_hfStats.nullHandle, g_hfStats.zeroTriangles, g_hfStats.appended,
                        SceneObject2EList_instance.maxCount, SceneObject2E_count);
    }
    // wip: temporary re-bypass (2026-07-24i), JUST to get the metric log
    // below to actually execute (guard1 blocks ~100% of live calls) -- not
    // proposing this as the fix, purely for data collection. Remove after.
    constexpr bool wipBypassGuards = true;
    // Both guards must pass (single combined condition, unlike the
    // static-mesh sibling's two sequential early returns).
    if (!wipBypassGuards && (a8 & 0x8) != 0 && *reinterpret_cast<const int32_t *>(0x00760B60) == 0) {
        ++g_hfStats.guard1Bail;
        return 0;
    }
    if (!wipBypassGuards && (a8 & 0x10) != 0 && *reinterpret_cast<const int32_t *>(0x00760B84) == 0) {
        ++g_hfStats.guard2Bail;
        return 0;
    }

    const auto *request = reinterpret_cast<const SceneAddRequest *>(
            static_cast<intptr_t>(requestArg));

    std::memcpy(gap_C, &request->collection, sizeof(request->collection));
    field_18 = request->ignoreCull;
    field_1C = (static_cast<int32_t>(request->word4) << 16) | request->word0;

    // camPos-relative position, transformed into camera space -- identical
    // to the static-mesh sibling.
    Vec3f relative{};
    pObj57AD20->vec.substractAssign(&relative, &dk2::g_camState.v3f);
    Vec3f camSpacePos{};
    dk2::g_camState.m.sub_594E10(&relative, &camSpacePos);

    if (request->ignoreCull == 0) {
        uint32_t cullScratch = 0;
        if (Vec3f_static_sub_575D70(&camSpacePos, pObj57AD20->f20, &cullScratch) == 0) {
            ++g_hfStats.cullFail;
            return 0;
        }
    }

    Vec3f reductionCenterScratch{};
    Vec3f reductionOtherScratch{};
    float reductionFactor = 0.0f;
    Vec3f_static_sub_575F10(&reductionCenterScratch, pObj57AD20->f20,
                             &reductionOtherScratch, &reductionFactor);

    // Inlined Obj57AD20::sub_57AC10 -- light-selection mask.
    pObj57AD20->f2C = pObj57AD20->sub_57AC10(
            reinterpret_cast<int32_t *>(static_cast<intptr_t>(request->collection)));

    // Draw-flags combine, inlined from `a8` (heightfield-specific bit set,
    // not the shared transformFlags() helper).
    uint32_t andMask = 0xFFFFFFFFu;
    uint32_t orMask = 0;
    if ((a8 & 0x2001) != 0) {
        andMask &= 0xFFFFFF7Eu;
        orMask |= 0x220u;
    }
    if ((a8 & 0x2000) != 0 &&
        (*reinterpret_cast<const uint8_t *>(0x00764BB8) & 0x40) != 0) {
        orMask |= 0x4000u;
    }
    if ((a8 & 0x2) != 0) {
        andMask &= 0xFFFFFF1Fu;
        orMask |= 0x201u;
    }
    if ((a8 & 0x20) != 0) {
        andMask &= 0xFFFFFBFFu;
    }
    if ((a8 & 0x4) != 0) {
        andMask &= 0xFFFFFFBFu;
    }

    // wip: defensive null-guards (menu/world-load crash investigation) --
    // NOT present in the original decompile (mirrored elsewhere in this file
    // per this project's no-silent-fixes convention), but this function is
    // now confirmed to crash intermittently on world load, and the original
    // x86 relies on Windows SEH to survive a bad pointer here where our
    // translation would just segfault -- same rationale as the SEH-guarded
    // resolveBridgeTextureIdGuarded negative-cache pattern in Obj57AD20.cpp.
    // Bail out (append nothing) rather than crash if the surface isn't
    // ready yet -- plausible during level-load before resources finish
    // decompressing, given the intermittent (not deterministic) crash.
    MyScaledSurface *surf = MyEntryBuf_MyScaledSurface_getByIdx(field_10);
    if (surf == nullptr || surf->scaledSurfArr == nullptr) {
        ++g_hfStats.nullSurf;
        return 0;
    }
    const uint32_t combinedFlags = (andMask & surf->drawFlags) | orMask;

    // LOD pick: metric = radius * reductionFactor / baseHandle->surfWidth8,
    // against fixed thresholds (the static-mesh sibling uses three
    // undocumented float constants instead -- this one is a plain literal
    // compare, confirmed via the raw `fcmp`s against 1.0/0.5/0.25).
    MyCESurfHandle *baseHandle = surf->scaledSurfArr->surfScaledArr[0];
    if (baseHandle == nullptr) {
        ++g_hfStats.nullBaseHandle;
        return 0;
    }
    const float metric = roundedDiv(
            roundedMul(pObj57AD20->f20, reductionFactor),
            static_cast<float>(baseHandle->surfWidth8));
    int lodLevel = (metric < 1.0f) ? 1 : 0;
    if (metric < 0.5f) lodLevel = 2;
    if (metric < 0.25f) lodLevel = 3;

    // wip: terrain-HD investigation (2026-07-24h) -- the actual computed LOD
    // metric/level for this heightfield chunk, to see what's really driving
    // the tag=3/16px handles observed downstream in SurfHashList2.
    {
        static int wipLeft = 40;
        if (wipLeft > 0) {
            --wipLeft;
            patch::log::dbg("heightfield LOD pick: field_10=%u f20=%f reductionFactor=%f "
                            "baseHandleW=%u metric=%f lodLevel=%d",
                            field_10, pObj57AD20->f20, reductionFactor,
                            (unsigned) baseHandle->surfWidth8, metric, lodLevel);
        }
    }

    // TODO(verify): defensive index adjustment for prob_height < 1 --
    // mirrored as-is from the decompile; looks unreachable for a real
    // heightfield surface (prob_height should always be >= 1), not
    // "corrected" here per this project's no-silent-fixes convention.
    int indexAdjust = 0;
    if (static_cast<int32_t>(surf->prob_height) < 1) {
        indexAdjust = static_cast<int32_t>(surf->prob_height) - 1;
    }
    MyCESurfHandle *handle = reinterpret_cast<MyCESurfHandle *const *>(
            surf->scaledSurfArr)[lodLevel + indexAdjust * 4];
    if (handle == nullptr) {
        ++g_hfStats.nullHandle;
        return 0;
    }

    MyCESurfHandle_static_addToHashList_flagsOr400(
            handle, static_cast<int16_t>(combinedFlags));

    // Grid-derived triangle/vertex counts (no per-part record here, unlike
    // the static-mesh sibling -- verified against the raw imul/shl sequence).
    const int32_t gridWidth = field_14;
    const int16_t vertsPerSide = static_cast<int16_t>(gridWidth + 1);
    const int32_t triangleCountX2 = gridWidth * gridWidth * 2;
    if (triangleCountX2 == 0) {
        ++g_hfStats.zeroTriangles;
    } else {
        ++g_hfStats.appended;
        if (SceneObject2E_count >= static_cast<uint32_t>(SceneObject2EList_instance.maxCount)) {
            SceneObject2EList_instance.objects2EToDraw_enlarge(SceneObject2E_count);
        }
        SceneObject2E &entry = SceneObject2EList_instance.arr[SceneObject2E_count];
        ++SceneObject2E_count;

        entry.mesh = this;
        entry.f2C_ = 0;
        entry.lod__triangleCount = static_cast<uint16_t>(triangleCountX2);
        entry.numVertsEx = static_cast<uint16_t>(vertsPerSide * vertsPerSide);
        entry.drawFlags_x2[0] = combinedFlags;
        entry.renMode = static_cast<uint8_t>(g_renMode_7820A0);
        entry.surfhCount = 1;
        entry.propsCount = 1;
        entry.numTextureSamplers_x2[0] = 1;
        entry.surfh_x4[0] = handle;
        entry.zeroOrM1 = 0;
    }

    return 0;
}
