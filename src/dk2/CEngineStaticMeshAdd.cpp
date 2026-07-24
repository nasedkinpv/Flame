#include "dk2/engine/primitive/CEngineStaticMesh.h"

#include "dk2/MyCESurfHandle.h"
#include "dk2/MyCESurfScale.h"
#include "dk2/MyScaledSurface.h"
#include "dk2/Obj57AD20.h"
#include "dk2/SceneObject2E.h"
#include "dk2/SceneObject2EList.h"
#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/MyCameraState.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"
#include "patches/logging.h"

#include <cstdint>
#include <cstring>
#include <emmintrin.h>

// dk2::CEngineStaticMesh::appendToSceneObject2EList, 0x00586190..0x00586A6C
// (next symbol: the free function static_appendToSceneObject2EList at
// 0x00586A70, called from inside this function -- see below; the body ends
// at the `ret 4`/nop padding at 0x00586A69/6C, confirmed by objdump).
//
// This is the static-mesh sibling of
// dk2::CEngineDynamicMesh::appendToSceneObject2EList (0x00580EC0, still
// untranslated) -- per-frame scene registration of a static mesh: pick a
// LOD/reduction level for each of the mesh's Obj57AD20 "sub-parts", resolve
// their MyScaledSurface -> MyCESurfHandle, add the handle(s) to the global
// hash list, and append one (or two) dk2::SceneObject2E entries describing
// the draw. Read src/dk2/EngineShadows.cpp and src/dk2/EngineAnimShadows.cpp
// first for this repo's translation conventions (roundedX() single-rounding
// helpers, the stack-offset normalizer approach, TODO(verify) usage); this
// file follows the same conventions and, like
// src/dk2/CEngineAnimMeshBlend.cpp, keeps a couple of genuinely unresolved
// micro-details flagged rather than guessed silently (see part 5 below).
//
// High-level structure (verified via objdump + a manual stack-offset
// normalizer over the whole 0x586190..0x586A6C range):
//
//  1. 0x586190..0x5861D4: three early-out guards on `a5_flags` bits
//     0x8/0x10/0x2000 against three undocumented globals
//     (0x00760B60/0x00760B84/0x00764BB8) -- TODO(verify): these three
//     addresses have no name yet anywhere in libs/dkii_exe/api (checked
//     dk2_globals.h; they fall in unnamed gaps between g_sc_renderLeft/
//     g_scMax, g_vec_760B70/g_meshLast_760B80, and g_sc_renderTop
//     respectively), so they are read as raw addresses. They read like
//     "is this optional debug/rendering feature enabled" toggles gating a
//     hard bail-out of the whole function.
//
//  2. 0x5861D4..0x586259: copies 5 fields out of the request argument
//     (actually a pointer, despite the auto header's `int` parameter type
//     -- exactly the same situation as CEngineDynamicMesh::f58_pTrgObj,
//     kept un-renamed there for the same reason: no struct name exists yet)
//     into `this`, then computes camPos-relative position
//     (`Vec3f::substractAssign`, 0x0041C4C0) and transforms it into camera
//     space (`Mat3x3f::sub_594E10`, 0x00594E10, this=g_camState.m) exactly
//     like src/dk2/CEngineAnimMeshBlend.cpp's `g_camState.m.sub_594E10(&relative,
//     &projected)` -- confirmed here too via 0x00760AB8/0x00760AC4 =
//     g_camState.v3f / g_camState.m per dk2/utils/MyCameraState.h. Unless
//     the request says "ignore distance cull" (field at request+0xC != 0),
//     the camera-space position is passed to
//     `Vec3f_static_sub_575D70` as a cull test; failing it bails out.
//
//  3. 0x586259..0x586274: `Vec3f_static_sub_575F10` computes an
//     LOD/reduction scale factor from `pObj57AD20->f20`; only the float*
//     output is used later (the two Vec3f* outputs are genuine scratch --
//     never read again, mirrored here as unused locals for call-signature
//     fidelity, same pattern as the "TODO(verify)" outputs in
//     CEngineAnimMeshBlend.cpp).
//
//  4. 0x586274..0x5863A7: for `pObj57AD20->sprsCount` "sub-part" 20-byte
//     records at `pObj57AD20->f4[byteOffset]` (struct not named yet
//     anywhere in the API -- modelled locally as SubmeshRecord), inlines
//     the tiny, currently-undeclared helper `Obj57AD20::sub_57AC10`
//     (0x0057AC10, 0x27 bytes, no header entry exists for it anywhere
//     under libs/dkii_exe/api -- confirmed absent by grep) which just does
//     `pObj57AD20->f2C = sub_57BBF0(collection, nullptr, vec.x, vec.y,
//     vec.z, f20, 1) | pObj57AD20->f28` (sub_57BBF0 is already translated
//     in src/dk2/Obj57AD20.cpp); resolves the sub-part's MyScaledSurface via
//     `MyEntryBuf_MyScaledSurface_getByIdx`; combines draw flags via
//     `transformFlags`; picks a coarse LOD level 0..3 via three `fcom`s
//     against undocumented float constants 0x0066FC00/08/10; picks a fine
//     sub-index via a mantissa-bit-extraction trick (same shape as
//     0x00581B80's `sub_581B80`, just inlined here because *this* call
//     site indexes MyScaledSurface::scaledSurfArr directly rather than
//     going through that helper) and adds the resulting MyCESurfHandle to
//     the global hash list via `MyCESurfHandle_static_addToHashList_flagsOr400`.
//
//  5. 0x5863A7..0x586A16: if `a5_flags & 0x300`, resolves a *second*
//     MyScaledSurface (index `field_20`) the same way but via the already-
//     declared helpers `sub_57F030`/`sub_581B80` (this repo's dynamic-mesh
//     sibling calls these too, per the task notes) and adds its handle to
//     the hash list too; otherwise jumps straight to emitting a single
//     SceneObject2E via the free function `static_appendToSceneObject2EList`
//     (0x00586A70, already declared in dk2_functions.h, not implemented by
//     this file -- it is a separate translation unit's responsibility).
//     Stack-argument split confirmed by direct listing trace at
//     0x5863A7..0x5863E5: `CALL FUN_0057F030` at 0x5863D5 takes exactly 3
//     args (surf2, reductionFactor, record.mmFactor; `ADD ESP,0xC` after
//     confirms 3), then `MOV ECX,EDI` at 0x5863DD sets ECX (this) back to
//     surf2 for the following thiscall, `PUSH EAX` at 0x5863DF pushes
//     sub_57F030's return (the picked LOD level) as the 1st stack arg, and
//     the 2nd/3rd stack args are two values pushed *before* the
//     sub_57F030 call and left unconsumed by its cdecl cleanup (the
//     "leftover stack reuse between two cdecl calls" pattern already
//     documented in CEngineAnimMeshAdd.cpp for this exact helper pair):
//     `this->field_24` bit-reinterpreted as float (loaded via
//     `FLD [ESI+0x28]` at 0x5863B6) and a literal 0. So the call is
//     `surf2->sub_581B80(lodLevel, *(float*)&field_24, 0)` -- matching the
//     sibling's `surf2->sub_581B80(lod2, field_74, 0)` shape exactly.
//
//  6. 0x586416..0x586A16: reloads `a5_flags`/the combined draw flags and
//     picks one of three ways to append SceneObject2E entries. Verified by
//     register trace (0x586426 `test ah,0x2` reads a5_flags; 0x58642f
//     `test bh,0x2` reads ebx, which has held combined-flags-1 unbroken
//     since 0x5862eb -- every intervening callee (sub_589140, sub_57c780,
//     sub_57f030, sub_581b80, sub_57f090) preserves ebx per standard
//     calling convention, confirmed by disassembling each):
//       a) `a5_flags & 0x200` clear: appends ONE entry using the *second*
//          surface's handle only (simplest; "branch C" below).
//       b) `a5_flags & 0x200` set, combined-flags-1 & 0x200 clear:
//          "branch B" -- appends ONE entry that packs both surfh pointers
//          into surfh_x4[0..1]. Resolved by direct disassembly of
//          0x586808..0x5868be: the compiler stages two per-slot "count"
//          locals hardcoded to 1 (unconditionally, no null test anywhere
//          on handle1/handle2), sums them into surfhCount (=2, always),
//          copies them into numTextureSamplers_x2[0]/[1], then a second,
//          simple loop (0x586896..0x5868b6) copies handle1 (stack home
//          esp+0x54) into surfh_x4[0] and handle2 (esp+0x58) into
//          surfh_x4[1]. So `handle1`/`handle2` in that order was in fact
//          the correct fill; the only actual bugs were (1) surfhCount and
//          numTextureSamplers_x2 not being written at all in the earlier
//          translation, and (2) this branch condition testing
//          combined-flags-2 instead of combined-flags-1. Both are fixed
//          below. Every other field of this entry (mesh/lod/numVerts/
//          renMode/propsCount/zeroOrM1, and the *omission* of
//          drawFlags_x2[0] -- genuinely never written by this branch, its
//          staged locals at esp+0x40/0x44 are never read back) remains
//          verified exactly.
//       c) both set: "branch A" -- appends up to TWO entries (one using
//          `handle1` with combined-flags-1, gated on the sub-part's own
//          `baseTriCountLo != 0`; one using `handle2` with combined-
//          flags-2, gated on the same field re-read), matching the "two
//          separate SceneObject2E for base+detail" idea suggested by the
//          task notes.
//     Every field write in a)/c) (and the non-loop fields of b) was
//     verified against the raw opcode bytes and the SceneObject2E.h /
//     SceneObject2EList.h layouts (idx*0x2E stride, arr/maxCount/count at
//     0x7820A8/AC/B0/C4).
//
// Callee census over 0x586190..0x586A6C: Vec3f::substractAssign (0x41C4C0)
// x1, Mat3x3f::sub_594E10 (0x594E10) x1, Vec3f_static_sub_575D70 (0x575D70)
// x1, Vec3f_static_sub_575F10 (0x575F10) x1, sub_57BBF0 (already translated
// in Obj57AD20.cpp, inlined per-call here since its caller
// Obj57AD20::sub_57AC10 has no header entry) x1,
// MyEntryBuf_MyScaledSurface_getByIdx (0x57C780) x2, transformFlags
// (0x57F090) x2, MyCESurfHandle_static_addToHashList_flagsOr400 (0x589140)
// x2, sub_57F030 (0x57F030) x1, MyScaledSurface::sub_581B80 (0x581B80) x1,
// SceneObject2EList::objects2EToDraw_enlarge (0x579FD0) x as-needed,
// static_appendToSceneObject2EList (0x586A70) x0-1 (only taken when
// `a5_flags & 0x300 == 0`).

namespace {

float roundedSub(float a, float b) {
    return _mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(a), _mm_set_ss(b)));
}

float roundedMul(float a, float b) {
    return _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(a), _mm_set_ss(b)));
}

float roundedDiv(float a, float b) {
    return _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(a), _mm_set_ss(b)));
}

const float *floatAt(uintptr_t address) {
    return reinterpret_cast<const float *>(address);
}

const double *doubleAt(uintptr_t address) {
    return reinterpret_cast<const double *>(address);
}

// The "int" argument to appendToSceneObject2EList is really a pointer to a
// small per-call request struct -- same situation as
// CEngineDynamicMesh::f58_pTrgObj (no struct name exists anywhere in the
// API yet). Field offsets verified against the raw `mov`s at
// 0x5861DC..0x5861FC.
#pragma pack(push, 1)
struct SceneAddRequest {
    uint16_t word0;       // +0x00 -> this->gap_14 word at +4 (TODO(verify) meaning)
    uint16_t unused2;
    uint16_t word4;       // +0x04 -> this->gap_14 word at +6 (TODO(verify) meaning)
    uint16_t unused6;
    int32_t field8;       // +0x08 -> this->gap_C
    int32_t fieldC;       // +0x0C -> this->gap_14 dword at +0; also the
                           //          "ignore distance cull" flag (nonzero
                           //          skips the Vec3f_static_sub_575D70 test)
    float customScale;    // +0x10 -> this->f2C, only read when a5_flags & 0x2000
};
#pragma pack(pop)

// pObj57AD20->f4 points at an array of `pObj57AD20->sprsCount` of these
// 20-byte (0x14) records; no struct name exists anywhere in the API for it
// (it is not SprsMeshHeader, which is 0x5C bytes). Field offsets verified
// against 0x5862AF (float at +0), 0x5862B0-ish (int at +4, the
// MyScaledSurface index), and 0x586444/0x58671B (bytes at +0x10/+0x11).
#pragma pack(push, 1)
struct SubmeshRecord {
    float mmFactor;              // +0x00
    int32_t surfIdx;             // +0x04
    uint8_t gap8[8];             // +0x08 .. +0x10, TODO(verify): unused here
    uint8_t baseTriCountLo;      // +0x10 -> SceneObject2E::lod__triangleCount
    uint8_t baseTriCountHi;      // +0x11 -> SceneObject2E::numVertsEx
    uint8_t gap12[2];            // +0x12 .. +0x14
};
#pragma pack(pop)
static_assert(sizeof(SubmeshRecord) == 0x14);

// wip: instrumentation for the "checkerboard holes in unclaimed rock"
// investigation (2026-07-24) -- same rationale as the heightfield sibling's
// g_hfStats. Remove once the cause is confirmed.
struct StaticMeshStats {
    uint32_t calls = 0, parts = 0, nullSurf = 0, nullScaledSurfArr = 0,
             nullBaseHandle = 0, nullHandle1 = 0, nullSurf2 = 0, nullHandle2 = 0,
             appended = 0;
};
StaticMeshStats g_smStats;

}  // namespace

int dk2::CEngineStaticMesh::appendToSceneObject2EList(int requestArg) {
    ++g_smStats.calls;
    if (g_smStats.calls % 256 == 0) {
        patch::log::dbg("staticmesh append stats: calls=%u parts=%u nullSurf=%u "
                        "nullScaledSurfArr=%u nullBaseHandle=%u nullHandle1=%u "
                        "nullSurf2=%u nullHandle2=%u appended=%u maxCount=%d count=%u",
                        g_smStats.calls, g_smStats.parts, g_smStats.nullSurf,
                        g_smStats.nullScaledSurfArr, g_smStats.nullBaseHandle,
                        g_smStats.nullHandle1, g_smStats.nullSurf2, g_smStats.nullHandle2,
                        g_smStats.appended, SceneObject2EList_instance.maxCount,
                        SceneObject2E_count);
    }
    // 0x586190..0x5861D4: early-out guards. TODO(verify): 0x760B60/0x760B84/
    // 0x764BB8 have no names anywhere in the API; treated as raw globals.
    if ((a5_flags & 0x8) != 0 && *reinterpret_cast<const int32_t *>(0x00760B60) == 0) {
        return 0;
    }
    if ((a5_flags & 0x10) != 0 && *reinterpret_cast<const int32_t *>(0x00760B84) == 0) {
        return 0;
    }
    if ((a5_flags & 0x2000) != 0 &&
        (*reinterpret_cast<const uint8_t *>(0x00764BB8) & 0x41) != 0) {
        return 0;
    }

    const auto *request = reinterpret_cast<const SceneAddRequest *>(
            static_cast<intptr_t>(requestArg));

    // 0x5861D4..0x586208: copy request fields into `this`.
    *reinterpret_cast<int32_t *>(gap_C) = request->field8;
    auto *gap14Dword = reinterpret_cast<int32_t *>(gap_14);
    auto *gap14Words = reinterpret_cast<uint16_t *>(gap_14 + 4);
    gap14Dword[0] = request->fieldC;
    gap14Words[0] = request->word0;
    gap14Words[1] = request->word4;
    if ((a5_flags & 0x2000) != 0) {
        f2C = request->customScale;
    } else {
        f2C = 1.0f;
    }

    // 0x586208..0x586257: camPos-relative position, transformed into camera
    // space, exactly like CEngineAnimMeshBlend.cpp's
    // `g_camState.m.sub_594E10(&relative, &projected)`.
    Vec3f relative{};
    pObj57AD20->vec.substractAssign(&relative, &dk2::g_camState.v3f);
    Vec3f camSpacePos{};
    dk2::g_camState.m.sub_594E10(&relative, &camSpacePos);

    // 0x586257..0x586259: unless the request says "ignore distance cull",
    // bail out if the camera-space position fails the cull test.
    if (request->fieldC == 0) {
        uint32_t cullScratch = 0;
        if (Vec3f_static_sub_575D70(&camSpacePos, pObj57AD20->f20, &cullScratch) == 0) {
            return 0;
        }
    }

    // 0x58625F..0x586274: reduction/LOD scale factor.
    //
    // Bug found 2026-07-24 (terrain-HD investigation, same call in the
    // heightfield sibling): this passed a zero-initialized scratch Vec3f as
    // the first argument instead of camSpacePos. The original disassembly
    // passes the SAME camera-space position to both
    // Vec3f_static_sub_575D70 (cull test, above) and this call. With a zero
    // input, reductionFactor comes out as a constant, wildly-out-of-range
    // value regardless of true camera distance, making the LOD metric
    // meaningless. camSpacePos isn't read again after this call, so reusing
    // it here is safe even if the callee treats it as in/out. The second
    // Vec3f* output (reductionOtherScratch) genuinely is never read again
    // (verified: its stack slot has no further reader) -- kept as unused
    // scratch for call-signature fidelity, same as the "TODO(verify)"
    // outputs in CEngineAnimMeshBlend.cpp.
    Vec3f reductionOtherScratch{};
    float reductionFactor = 0.0f;
    Vec3f_static_sub_575F10(&camSpacePos, pObj57AD20->f20,
                             &reductionOtherScratch, &reductionFactor);

    // 0x586274..0x58629C: nothing to submit if there are no sub-parts.
    const int32_t sprsCount = static_cast<int32_t>(pObj57AD20->sprsCount);
    if (sprsCount <= 0) {
        return 0;
    }

    auto appendEntry = [&]() -> SceneObject2E & {
        ++g_smStats.appended;
        if (SceneObject2E_count >= static_cast<uint32_t>(SceneObject2EList_instance.maxCount)) {
            SceneObject2EList_instance.objects2EToDraw_enlarge(SceneObject2E_count);
        }
        SceneObject2E &entry = SceneObject2EList_instance.arr[SceneObject2E_count];
        ++SceneObject2E_count;
        return entry;
    };

    const auto *records = reinterpret_cast<const SubmeshRecord *>(pObj57AD20->f4);
    for (int32_t part = 0; part < sprsCount; ++part) {
        ++g_smStats.parts;
        const SubmeshRecord &record = records[part];

        // 0x5862AC..0x5862BB: resolve this sub-part's base MyScaledSurface.
        MyScaledSurface *surf = MyEntryBuf_MyScaledSurface_getByIdx(record.surfIdx);
        // wip: defensive null-guard (same rationale as the heightfield
        // sibling CEngineStaticHeightFieldAdd.cpp -- not present in the
        // original decompile, but this function was never actually live
        // until now and an unchecked deref here would crash instead of
        // relying on Windows SEH like the original x86 does). Skip just
        // this sub-part rather than aborting the whole append.
        if (surf == nullptr) { ++g_smStats.nullSurf; continue; }

        // 0x5862BE..0x5862F3: combine draw flags.
        uint32_t andMask = 0xFFFFFFFFu;
        uint32_t orMask = 0;
        transformFlags(static_cast<int16_t>(a5_flags), &andMask, &orMask);
        const uint32_t combinedFlags1 = (andMask & surf->drawFlags) | orMask;

        // 0x5862F3..0x586341: inlined dk2::Obj57AD20::sub_57AC10 (0x0057AC10,
        // no header entry exists for it anywhere in libs/dkii_exe/api --
        // confirmed by grep). Original body: copies `pObj57AD20->vec` onto
        // the stack and calls the already-translated `sub_57BBF0` (see
        // src/dk2/Obj57AD20.cpp) with a mask of 1, ORing pObj57AD20->f28
        // into the result.
        pObj57AD20->f2C = dk2::sub_57BBF0(
                                   reinterpret_cast<int32_t *>(
                                           static_cast<intptr_t>(request->field8)),
                                   nullptr, pObj57AD20->vec.x, pObj57AD20->vec.y,
                                   pObj57AD20->vec.z, pObj57AD20->f20, 1) |
                           pObj57AD20->f28;

        // 0x586341..0x58638A: LOD metric = record.mmFactor * reductionFactor
        // / baseHandle.surfWidth8, compared against 3 undocumented
        // thresholds to pick a coarse level 0..3.
        //
        // Bug found 2026-07-24 (independent audit, terrain-HD investigation):
        // these three thresholds at 0x0066FC00/08/10 are 8 bytes apart --
        // they're 64-bit doubles (the mantissa-extraction constants further
        // below at 0x0066FC38/3C/40 are 4 bytes apart, genuinely floats, and
        // are unaffected). Reading them via `floatAt` took only the low
        // 32 bits of each double; for the values 1.0/0.5/0.25 (matching the
        // heightfield sibling's literal thresholds) that low dword is
        // 0x00000000 for all three, i.e. every comparison silently read 0.0.
        // Combined with the wrong `>=` direction (should be `<`, matching
        // the heightfield sibling and the confirmed original disassembly
        // sense `test ah,1` / ST<src), `metric >= 0.0` was always true,
        // forcing lodLevel to 3 (worst) for every static-mesh object
        // regardless of distance -- a likely major contributor to the
        // broad "texture quality dropped everywhere" regression, since this
        // function only went live this session.
        if (surf->scaledSurfArr == nullptr) { ++g_smStats.nullScaledSurfArr; continue; }
        MyCESurfHandle *baseHandle = surf->scaledSurfArr->surfScaledArr[0];
        if (baseHandle == nullptr) { ++g_smStats.nullBaseHandle; continue; }
        const float metric = roundedDiv(
                roundedMul(record.mmFactor, reductionFactor),
                static_cast<float>(baseHandle->surfWidth8));
        int lodLevel = 0;
        if (metric < static_cast<float>(*doubleAt(0x0066FC00))) lodLevel = 1;
        if (metric < static_cast<float>(*doubleAt(0x0066FC08))) lodLevel = 2;
        if (metric < static_cast<float>(*doubleAt(0x0066FC10))) lodLevel = 3;

        // 0x58634C..0x586390: fine sub-index via mantissa-bit extraction
        // (same shape as MyScaledSurface::sub_581B80, inlined here because
        // this call site indexes scaledSurfArr directly).
        const int32_t probHeight = static_cast<int32_t>(surf->prob_height);
        const float mantissaRaw = roundedSub(
                roundedSub(
                        roundedSub(roundedMul(field_18_float_scale,
                                               static_cast<float>(probHeight)),
                                   *floatAt(0x0066FC38)),
                        *floatAt(0x0066FC3C)),
                *floatAt(0x0066FC40));
        int32_t bits;
        std::memcpy(&bits, &mantissaRaw, sizeof(bits));
        int32_t fineIndex = (bits & 0x7FFFFF) - 0x400000;
        if (fineIndex < 0) fineIndex = 0;
        if (fineIndex >= probHeight) fineIndex = probHeight - 1;

        const int32_t widthStep = static_cast<uint8_t>(field_2D);
        const int32_t flatIndex = lodLevel + 4 * (widthStep * probHeight + fineIndex);
        MyCESurfHandle *handle1 =
                reinterpret_cast<MyCESurfHandle *const *>(surf->scaledSurfArr)[flatIndex];
        if (handle1 == nullptr) { ++g_smStats.nullHandle1; continue; }

        MyCESurfHandle_static_addToHashList_flagsOr400(
                handle1, static_cast<int16_t>(combinedFlags1));

        // 0x586398..0x5863A1: reload a5_flags after the hash-add call.
        const int32_t a5FlagsNow = a5_flags;

        MyCESurfHandle *handle2 = nullptr;
        uint32_t combinedFlags2 = 0;
        bool hasSecondSurface = (a5FlagsNow & 0x300) != 0;
        if (hasSecondSurface) {
            // 0x5863A7..0x58641B: second MyScaledSurface (this->field_20),
            // via the already-declared reduction-pick / handle-pick helpers.
            // TODO(verify): the precise stack-argument mapping for these two
            // calls (see file-header note 5) -- reductionFactor/record.mmFactor
            // are the best-supported candidates for sub_57F030's two float
            // parameters, and the picked LOD level is the best-supported
            // candidate for sub_581B80's declared first parameter.
            MyScaledSurface *surf2 = MyEntryBuf_MyScaledSurface_getByIdx(field_20);
            if (surf2 == nullptr) { ++g_smStats.nullSurf2; continue; }
            const int lodLevel2 = sub_57F030(surf2, reductionFactor, record.mmFactor);
            // Verified against the raw listing at 0x5863dd..0x5863e0: ECX (this)
            // is surf2 (MOV ECX,EDI), and the 2nd stack arg is `this->field_24`
            // bit-reinterpreted as float (FLD [ESI+0x28] at 0x5863b6, staged
            // into the arg slot the same way as sub_57F030's own args -- the
            // classic "leftover stack push reused by the next cdecl call"
            // pattern already documented in CEngineAnimMeshAdd.cpp for this
            // exact helper pair). Previously hardcoded to 0.0f here, which
            // doesn't match this->field_24 -- same call shape as the sibling's
            // `surf2->sub_581B80(lod2, field_74, 0)`.
            const float field24AsFloat = *reinterpret_cast<const float *>(&field_24);
            handle2 = surf2->sub_581B80(lodLevel2, field24AsFloat, 0);
            if (handle2 == nullptr) { ++g_smStats.nullHandle2; continue; }

            uint32_t andMask2 = 0xFFFFFFFFu;
            uint32_t orMask2 = 0;
            transformFlags(static_cast<int16_t>(a5_flags), &andMask2, &orMask2);
            combinedFlags2 = (andMask2 & surf2->drawFlags) | orMask2;

            MyCESurfHandle_static_addToHashList_flagsOr400(
                    handle2, static_cast<int16_t>(combinedFlags2));
        } else {
            // 0x586a16..0x586a41: emit via the free function (declared in
            // dk2_functions.h, implemented elsewhere) instead of appending
            // an entry directly.
            //
            // Bug found 2026-07-24 while chasing the "checkerboard holes /
            // missing menu columns" regression: the 4th argument (-> the
            // appended entry's f2C_) was wired to `bits` (the LOD
            // mantissa-extraction scratch, effectively noise here), but the
            // full decompile of this call site
            // (FUN_00586a70(iVar6,uVar10,param_1,iStack_5c,...)) shows the
            // 4th arg is `iStack_5c` -- the per-part LOOP COUNTER, exactly
            // like every other branch's `entry.f2C_ = static_cast<int16_t>(
            // request->word0 or i)`. f2C_ later doubles as the shadow-decal
            // classifier in draw_functions.cpp (`f2C_ >= 0x7D0` => treated as
            // a shadow decal, drawn on a completely different path) --
            // feeding it near-random mantissa bits misclassified a large
            // fraction of static-mesh objects (e.g. menu columns) as shadow
            // decals, which is exactly the kind of bug that would make them
            // vanish from the normal draw path.
            static_appendToSceneObject2EList(
                    handle1, static_cast<int>(combinedFlags1), this,
                    static_cast<int16_t>(part), record.baseTriCountLo,
                    static_cast<int16_t>(record.baseTriCountHi), 0);
            continue;
        }

        // 0x586420..0x586432: pick which of the three emission shapes to use.
        // Verified by register trace: `test ah,0x2` at 0x586426 reads
        // a5_flags (eax reloaded from this->field_10 just above), but
        // `test bh,0x2` at 0x58642f reads ebx, which has held combinedFlags1
        // unbroken since 0x5862eb (`or ebx,eax`) -- every intervening call
        // (sub_589140, sub_57c780, sub_57f030, sub_581b80, sub_57f090) is
        // confirmed by disassembly to never touch ebx (sub_57f090 even
        // explicitly push/pop-saves it). So the second test is against
        // combinedFlags1, not combinedFlags2.
        const int32_t a5FlagsAfterSecondAdd = a5_flags;
        const bool useSimpleSecondOnly = (a5FlagsAfterSecondAdd & 0x200) == 0;
        const bool useTwoLayerSingleEntry =
                !useSimpleSecondOnly && (combinedFlags1 & 0x200) == 0;

        if (useSimpleSecondOnly) {
            // Branch C (0x5868C3..0x586A0E): single entry, second surface
            // handle only.
            if (record.baseTriCountLo != 0) {
                SceneObject2E &entry = appendEntry();
                entry.mesh = this;
                entry.f2C_ = static_cast<int16_t>(request->word0);
                entry.lod__triangleCount = record.baseTriCountLo;
                entry.numVertsEx = record.baseTriCountHi;
                entry.drawFlags_x2[0] = combinedFlags2;
                entry.renMode = g_renMode_7820A0;
                entry.surfhCount = 1;
                entry.propsCount = 1;
                entry.numTextureSamplers_x2[0] = 1;
                entry.surfh_x4[0] = handle2;
                entry.zeroOrM1 = 0;
            }
        } else if (useTwoLayerSingleEntry) {
            // Branch B (0x5866EE..0x586A0E): single entry packing both
            // surfh pointers. Verified by direct disassembly of
            // 0x586808..0x5868be: the compiler stages two per-slot "count"
            // locals hardcoded to 1 (never null-checked), sums them
            // unconditionally into surfhCount (=2, at 0x58688c), copies
            // them byte-wise into numTextureSamplers_x2[0]/[1] (loop at
            // 0x586822..0x586874, writes to entry offset 0x1F+i), and then
            // a second, simpler loop (0x586896..0x5868b6) copies the two
            // handle pointers straight from their stack homes into
            // surfh_x4[0]/[1]:
            //   ecx=1: eax = [esp+0x54] (handle1) -> surfh_x4[0]
            //   ecx=2: eax = [esp+0x58] (handle2) -> surfh_x4[1]
            // There is no pointer-null test anywhere in this path -- both
            // slots and numTextureSamplers_x2[0]/[1] are always written,
            // and surfhCount is always 2. (The deliberate *omission* of
            // drawFlags_x2[0], which the original genuinely never writes
            // on this path -- esp+0x40/0x44 are staged but never read
            // back -- remains verified as before.)
            if (record.baseTriCountLo != 0) {
                SceneObject2E &entry = appendEntry();
                entry.mesh = this;
                entry.f2C_ = static_cast<int16_t>(request->word0);
                entry.lod__triangleCount = record.baseTriCountLo;
                entry.numVertsEx = record.baseTriCountHi;
                entry.renMode = g_renMode_7820A0;
                entry.propsCount = 2;
                entry.zeroOrM1 = 0;
                entry.surfhCount = 2;
                entry.numTextureSamplers_x2[0] = 1;
                entry.numTextureSamplers_x2[1] = 1;
                entry.surfh_x4[0] = handle1;
                entry.surfh_x4[1] = handle2;
            }
        } else {
            // Branch A (0x586438..0x5866E9): up to two separate entries.
            if (record.baseTriCountLo != 0) {
                SceneObject2E &entry1 = appendEntry();
                entry1.mesh = this;
                entry1.f2C_ = static_cast<int16_t>(request->word0);
                entry1.lod__triangleCount = record.baseTriCountLo;
                entry1.numVertsEx = record.baseTriCountHi;
                entry1.drawFlags_x2[0] = combinedFlags1;
                entry1.renMode = g_renMode_7820A0;
                entry1.surfhCount = 1;
                entry1.propsCount = 1;
                entry1.numTextureSamplers_x2[0] = 1;
                entry1.surfh_x4[0] = handle1;
                entry1.zeroOrM1 = 0;
            }
            if (record.baseTriCountLo != 0) {
                SceneObject2E &entry2 = appendEntry();
                entry2.mesh = this;
                entry2.f2C_ = static_cast<int16_t>(request->word0);
                entry2.lod__triangleCount = record.baseTriCountLo;
                entry2.numVertsEx = record.baseTriCountHi;
                entry2.drawFlags_x2[0] = combinedFlags2;
                entry2.renMode = g_renMode_7820A0;
                entry2.surfhCount = 1;
                entry2.propsCount = 1;
                entry2.numTextureSamplers_x2[0] = 1;
                entry2.surfh_x4[0] = handle2;
                entry2.zeroOrM1 = static_cast<char>(0xFF);
            }
        }
    }

    return 0;
}
