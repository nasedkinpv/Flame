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
//     Verified 2026-07-24 against the shipped binary (objdump): the original
//     shares the sibling's mantissa-fine-index shape, but its fine-index
//     *input* is the hardcoded magic constant 0x4B400000 (mov dword ptr
//     [esp+0x50], 0x4B400000 at 0x58720C), whose mantissa extraction
//     (`and eax,0x7fffff; sub eax,0x400000`) is exactly 0 -- so the fine
//     index is always 0. The following `cmp eax,prob_height; jl .+; lea
//     eax,[prob_height-1]` (0x587226-0x58722D) therefore only fires when
//     0 >= prob_height, i.e. the `prob_height < 1` clamp mirrored below; it
//     is genuinely unreachable for a real surface (prob_height >= 1) and is
//     kept as-is per this project's no-silent-fixes convention.
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
    // Both guards must pass (single combined condition, unlike the
    // static-mesh sibling's two sequential early returns).
    if ((a8 & 0x8) != 0 && *reinterpret_cast<const int32_t *>(0x00760B60) == 0) {
        ++g_hfStats.guard1Bail;
        return 0;
    }
    if ((a8 & 0x10) != 0 && *reinterpret_cast<const int32_t *>(0x00760B84) == 0) {
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

    // Bug found 2026-07-24 (terrain-HD investigation): this call was passing
    // a zero-initialized scratch Vec3f as the first argument. The original
    // disassembly (FUN_00587060) passes the SAME camSpacePos (auStack_24) to
    // BOTH Vec3f_static_sub_575D70 (cull test, above) and this call -- not a
    // separate zero vector. Confirmed live: with the wrong zero input,
    // reductionFactor came out as a constant, wildly-out-of-range value
    // (~1e30) every time regardless of true camera distance, making the LOD
    // metric meaningless (either always-best or always-worst by accident of
    // floating point, not by actual distance). camSpacePos isn't read again
    // after this call, so reusing it here (rather than a fresh scratch) is
    // safe even if the callee treats it as in/out.
    Vec3f reductionOtherScratch{};
    float reductionFactor = 0.0f;
    Vec3f_static_sub_575F10(&camSpacePos, pObj57AD20->f20,
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

    // Defensive null-guards (menu/world-load crash investigation) --
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

    // Diagnostic (Bug B, 2026-07-24): "selected tile renders as a solid black
    // rectangular hole." Two live theories remain after the earlier color/tint
    // pass was exhausted-and-negative and the 0c0f44d decal-misclassification
    // mechanism was DISPROVEN for terrain (f2C_ is hardcoded 0 here -- verified
    // against the shipped binary at 0x587298: `mov word[edx+2*ecx+0x2c],0` --
    // so a heightfield tile is NEVER routed through the >=0x7D0 shadow-decal
    // path in draw_functions.cpp; the black hole is not a decal):
    //   (A) missing geometry -- one of the two guards above bails for the
    //       selected tile (watch guard1Bail/guard2Bail in the stats log spike
    //       during a drag-select), OR
    //   (B) blend shading -- the selected tile is emitted with a blend bit in
    //       its OWN drawFlags. metalMeshFlags (Obj57AD20.cpp) maps 0x1000->
    //       MULTIPLY, 0x20->ALPHA_BLEND, 0x1->ADDITIVE. A MULTIPLY tile over a
    //       bright texture reads as solid black. No prior probe logged the
    //       heightfield tile's own combinedFlags at the emit site, so this
    //       pins whether selection actually flips these bits on terrain.
    // orMask 0x220 (=0x200|0x20 -> ALPHA_BLEND) and 0x201 (=0x200|0x1 ->
    // ADDITIVE) are the selection-driven a8&0x2/0x2001 contributions, verified
    // faithful at 0x58717c/0x58719b. If the log shows 0x1000 set here, the
    // MULTIPLY comes from surf->drawFlags, not the selection bits.
    // Gated behind [flametal:logging:debug]; throttled; remove once confirmed.
    if ((combinedFlags & 0x1021u) != 0) {  // any blend selector (0x1000|0x20|0x1)
        static uint32_t g_hfBlendSeq = 0;
        if ((g_hfBlendSeq++ % 64) == 0) {
            patch::log::dbg(
                "heightfield blend emit: a8=0x%X surfDrawFlags=0x%X andMask=0x%X "
                "orMask=0x%X combinedFlags=0x%X (MULTIPLY=%d ALPHA=%d ADD=%d)",
                a8, surf->drawFlags, andMask, orMask, combinedFlags,
                (combinedFlags & 0x1000u) ? 1 : 0, (combinedFlags & 0x20u) ? 1 : 0,
                (combinedFlags & 0x1u) ? 1 : 0);
        }
    }

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

    // Defensive index adjustment for prob_height < 1, mirrored as-is from the
    // original. Verified 2026-07-24 against the shipped binary at
    // 0x58720C-0x587230 (objdump -d -M intel): the fine-index input is the
    // hardcoded constant 0x4B400000 whose mantissa-extraction yields exactly 0
    // (`and eax,0x7fffff; sub eax,0x400000` -> 0), so eax is 0 going into the
    // `cmp eax,prob_height (edi+0x15); jl .+; lea eax,[edi-1]` clamp -- which
    // only takes the `prob_height-1` branch when 0 >= prob_height, i.e. the
    // `prob_height < 1` test below. handle = scaledSurfArr[lodLevel + 4*eax]
    // matches `mov ebx,[ecx+4*edx]` with edx = lodLevel + 4*eax at 0x587234.
    // Unreachable for a real surface (prob_height >= 1); not "corrected" here
    // per this project's no-silent-fixes convention.
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
