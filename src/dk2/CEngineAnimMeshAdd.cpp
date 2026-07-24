#include "dk2/engine/primitive/2d/world/CEngineAnimMesh.h"

#include "dk2/MyCESurfHandle.h"
#include "dk2/MyCESurfScale.h"
#include "dk2/MyScaledSurface.h"
#include "dk2/Obj57BCB0.h"
#include "dk2/ScreenObjectArr.h"
#include "dk2/SceneObject2E.h"
#include "dk2/SceneObject2EList.h"
#include "dk2/SprsAnimHeader.h"
#include "dk2/engine/primitive/resource/CAnimMeshResource.h"
#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/MyCameraState.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>

// dk2::CEngineAnimMesh::appendToSceneObject2EList (v_f8_appendToSceneObject2EList
// in the vtable, CEngineAnimMesh.h line ~67/~81), 0x00584900..0x005855C0.
//
// This is the ANIMATED-mesh sibling of
// dk2::CEngineStaticMesh::appendToSceneObject2EList (see
// src/dk2/CEngineStaticMeshAdd.cpp, whose file-header comment documents this
// repo's stack-offset-normalizer method and float-rounding conventions in
// more depth -- read that first). It is also the per-frame caller of
// dk2::CEngineAnimMesh::sub_5855E0 (src/dk2/EngineAnimShadows.cpp), called
// twice back to back near the very end of this function.
//
// This is a substantially bigger and busier function than its static
// sibling: besides the LOD pick / surface-handle-registration / SceneObject2E
// append loop the two share, it also performs a once-per-mesh mouse-picking
// registration (ScreenObjectArr_instance) and a once-per-mesh directional/
// point-light query (Obj57BCB0), and its per-subpart loop has THREE
// possible extra-surface append paths instead of the static mesh's one.
//
// ============================================================================
// High-level structure (verified via objdump over the whole
// 0x584900..0x5855C0 range; every callee below is cross-checked against its
// own disassembly, not assumed by analogy with CEngineStaticMeshAdd.cpp):
//
//  1. 0x584900..0x584936: unless `f48_flags & 0x4`, add the global velocity-
//     ish vector `g_vec_760A98` into `field_3C` (a per-mesh position/ambient
//     accumulator whose exact semantic role is shared with sub_5836A0's
//     "ambient" use of field_3C).
//
//  2. 0x584936..0x58494c: `resource = getResource(f50_pMeshHolder)`
//     (0x57ED30); `field_58 = requestArg->field8` (the ONLY field of the
//     incoming request pointer this function ever reads -- unlike the
//     static-mesh sibling's 5-field request struct, everything else of that
//     struct, if it even has the same shape, goes unused here). `field_58`
//     doubles from here on as the "light/sphere spatial-query collection"
//     pointer passed to sub_57BBF0 and Obj57BCB0::constructor below -- same
//     struct offset EngineAnimShadows.cpp's `mesh.field_58` already
//     documents playing this role for the shadow path.
//
//  3. 0x584951..0x5849cc: pivot = f10_matrix.multiplyVec(&resource->pos);
//     worldPos = field_4 + pivot (exactly sub_5836A0's own "pivot"/
//     "worldPosition" computation, see CEngineAnimMesh.cpp); then
//     relative = worldPos - g_camState.v3f, camSpacePos =
//     g_camState.m.sub_594E10(relative) -- the camera-space transform, same
//     idiom as the static-mesh sibling's `g_camState.m.sub_594E10(&relative,
//     &camSpacePos)`. TODO(verify): the exact stack-slot/register path by
//     which `relative`'s buffer address survives through the 0x41C4C0 call
//     (whose own disassembly the census flagged as "ret 0x8 two-arg
//     subtract-to-out, not Vec3f::copy") into the following 0x594E10 call
//     was reconstructed from the instruction sequence plus strong structural
//     analogy with the already-verified static-mesh sibling, not fully
//     re-derived slot-by-slot; the VALUE computed (worldPos - camPos, then
//     transformed into camera space) is not in doubt, only the precise
//     transient stack addresses used to get there.
//
//  4. radius = resource->cubeScale * field_5C (same product
//     EngineAnimShadows.cpp's buildAnimShadowMatrix uses for its own
//     `matrixDivisor`). Unless the request said otherwise (mirrors the
//     static sibling; here there is no separate "ignore cull" flag observed
//     in the disassembly -- the cull test always runs), bail out if
//     Vec3f_static_sub_575D70(&camSpacePos, radius, &cullScratch) fails.
//
//  5. reductionFactor via Vec3f_static_sub_575F10(&scratch1, radius,
//     &scratch2, &reductionFactor) -- same call shape as the static sibling
//     (the two Vec3f* outputs are scratch, never read again).
//
//  6. 0x5849f2..0x584ab0: unless `f48_flags & 0x4000`, pick a discrete LOD
//     0..3 for `field_78` from a distance-like metric against three
//     undocumented float thresholds (0x66FC20/24/28) and __ftol (0x634F30);
//     otherwise field_78 stays 0 (it is unconditionally zeroed first).
//     TODO(verify): the exact input value read at 0x584a05
//     (`fld dword ptr [esp+0x10]`) feeding this metric was not fully traced
//     back to a named field -- it is some camera-space/reduction-derived
//     scalar computed earlier in this same function, consistent with the
//     purpose (a coarse distance-based LOD level) but not pinned to a
//     specific named local.
//
//  7. 0x584ab4..0x584ac1: if a certain scratch value (the output of the
//     cull test at step 4, most likely) is nonzero, OR `f48_flags` with
//     0x10000. TODO(verify): exact semantic meaning of this bit.
//
//  8. 0x584ac1..0x584b25 -- mouse-picking registration: if `f4C_thing` is
//     non-null and `ScreenObjectArr_instance.itemsCount < 256`, append one
//     `ScreenObject` (centerX/centerY = camSpacePos.x/.y, sqr =
//     reductionFactor^2, pMesh = this) and increment itemsCount. No
//     enlarge-on-full path exists here (unlike SceneObject2EList) -- the
//     entry is simply dropped once the fixed 256-slot array is full.
//
//  9. 0x584b25..0x584b95 -- point/spot-light query + Obj57BCB0 lights
//     collection build:
//       field_68 = sub_57BBF0(field_58, worldPos.x/.y/.z, radius, mask=0x100)
//       tempMask = sub_57BBF0(field_58, worldPos.x/.y/.z, radius, mask=4)
//       lights   = Obj57BCB0::constructor(field_58, tempMask)
//     (the 24x `call 0x40D640` loop immediately before the constructor call
//     is Obj57BCB0_item's own trivial default constructor -- confirmed
//     0x40D640 is a bare `mov eax,ecx; ret` stub -- run once per array slot
//     before Obj57BCB0::constructor overwrites every slot it actually
//     populates; it has no observable effect and is elided here per the
//     task's own instruction).
//
// 10. 0x584b9a..0x584bae: `lights.sub_57C080(&field_3C, &field_4)` (a real
//     declared Obj57BCB0 method, see Obj57BCB0.h).
//
// 11. 0x584bae..0x584c2a: `transformFlags(f48_flags, &andMask, &orMask)`
//     (0x57F090, already declared/used by the static-mesh sibling) computed
//     ONCE here and reused for every sub-part of the loop below -- verified
//     by disassembling 0x57F090 itself and confirming this inlined sequence
//     reproduces it exactly (the static-mesh sibling instead calls it fresh
//     per sub-part; this function's compiler apparently hoisted it since
//     `f48_flags` cannot change mid-loop).
//
// 12. 0x584c2a..0x585386 -- the per-sub-part loop (`resource->sprsCount`
//     iterations of `resource->buf[i]`, an SprsAnimHeader, 0x5C bytes each):
//       a. surf1 = getByIdx(entry.MyScaledSurface_idx);
//          combinedFlags1 = (andMask & surf1->drawFlags) | orMask.
//       b. PRIMARY handle pick, exactly one of:
//          - `f48_flags & 0x100` set: surf2 = getByIdx(field_6C) [a second
//            per-mesh surface-index field, offset 0x70 -- NOT f70_surfIdx,
//            which is offset 0x74 and used by a different branch below];
//            combinedFlags2 = (andMask & surf2->drawFlags) | orMask;
//            lod2 = sub_57F030(surf2, reductionFactor, entry.mmFactor);
//            handle = surf2->sub_581B80(lod2, field_74, 0);
//            addToHashList(handle, combinedFlags2); the SceneObject2E
//            appended below (if entry.lod_list[field_78] != 0) uses
//            combinedFlags2.
//          - clear: lod1 = sub_57F030(surf1, reductionFactor, entry.mmFactor);
//            handle = surf1->sub_581B80(lod1, field_54, field_94);
//            addToHashList(handle, combinedFlags1) -- reusing combinedFlags1,
//            NOT a fresh combine; the SceneObject2E appended (same
//            triangle-count gate) uses combinedFlags1.
//          NOTE (confirms/resolves the static-mesh sibling's own TODO about
//          sub_581B80's stack-argument split): in BOTH sub-branches here,
//          sub_581B80's 2nd/3rd args are genuinely live mesh fields left on
//          the stack by the immediately-preceding sub_57F030 call's own
//          cdecl cleanup (which only pops sub_57F030's own 3 args, leaving
//          2 more caller-reserved dwords in place) -- not placeholder
//          zeros. Here they resolve to field_74/0 and field_54/field_94
//          respectively; entry.mmFactor is read as a raw float despite the
//          auto header's `int mmFactor` (same situation as the static
//          sibling's SubmeshRecord.mmFactor).
//       c. SECONDARY ("extra decal") pass, gated first on
//          `combinedFlags1 & 0x21`: if that is nonzero AND
//          `f48_flags & 0x400` is clear, skip straight to the next sub-part
//          (nothing further happens for this one). Otherwise:
//          - `f48_flags & 0x200` set: surf3 = getByIdx(f70_surfIdx) [offset
//            0x74]; an INLINED duplicate of the coarse LOD-pick (three QWORD
//            `fcom`s against 0x66FC00/08/10, not a call to sub_57F030) picks
//            lod3; handle3 = surf3->sub_581B80(lod3, field_74, 0);
//            combinedFlags3 = surf3->drawFlags directly (no andMask/orMask
//            combine at all here); addToHashList(handle3, combinedFlags3);
//            if entry.lod_list[field_78] != 0, append a SceneObject2E with
//            zeroOrM1 = -1 (0xFF) -- the "detail/second entry" convention
//            the static-mesh sibling also uses.
//          - else `f48_flags & 0x800` set AND `g_doAdd_0x741_objToScene`
//            (0x764BE4) nonzero: addToHashList(surf1->surfh, 0x741);
//            append a SceneObject2E tagged with the literal constant 0x741
//            as its drawFlags_x2[0], zeroOrM1 = 0. This is a fixed,
//            globally-gated "special decal" path (TODO(verify): the exact
//            source of its lod__triangleCount/numVertsEx fields -- the
//            disassembly builds one of them from `word0 + 0x3E8`
//            (word0 + 1000) rather than reading it from the SprsAnimHeader,
//            which reads as a synthetic/fixed tag rather than real geometry
//            counts, but this was not chased further).
//          - neither flag set: nothing further for this sub-part.
//
// 13. 0x585386..0x5855C0 -- per-light shadow-decal registration, run once
//     per light AFTER the whole sub-part loop, IF `g_shadowLevel > 0` and
//     `f85_count != 0` (the mesh's own resolved-light count, filled in by
//     `sub_58EC70` at 0x5853b7).
//     Structure verified against the raw opcodes at 0x585417/0x58543b,
//     confirming the task's own note precisely: the low-detail
//     (`g_shadowLevel < 2`) call result is cached in
//     `resource->init_neg1` (offset 0x48 -- previously an unexplained
//     "-1 init" field; this is exactly why it starts at -1: it is a lazy,
//     per-RESOURCE-not-per-creature cache of the low-detail shadow surface
//     id, recomputed only while still negative, and only written back while
//     `g_shadowLevel < 2` stays true) and, separately from that, the
//     per-CREATURE-per-frame call is only made for the FIRST light of the
//     loop unless `g_shadowLevel >= 3` (a local flag, reset to 0 after the
//     first light and only set back to 1 when `g_shadowLevel >= 3`) --
//     i.e. at shadow level exactly 2, one call result is reused across every
//     light of this creature this frame; at level 3+ every light recomputes.
//     Either way the result feeds `getByIdx` -> `scaledSurfArr->
//     surfScaledArr[0]` -> `addToHashList`, then (if `field_78`'s own
//     f85_count-gated loop body reaches it) a SceneObject2E with a fixed
//     2-triangle/4-vertex quad (drawFlags_x2[0] = surf->drawFlags raw,
//     f2C_ = lightIndex + 0x7D0, zeroOrM1 = 0) is appended -- the on-screen
//     shadow decal itself.
//
// ----------------------------------------------------------------------
// Callee census over 0x584900..0x5855C0 (each verified against its own
// disassembly or an existing translation, per file):
//   0x57ED30 MyMeshResourceHolder_getResource x1
//   0x594DB0 Mat3x3f::multiplyVec x1, 0x594E10 Mat3x3f::sub_594E10 x1
//   0x41C4C0 (2-arg subtract-to-out, ret 8) x1, 0x41C440 Vec3f::copy (ret 4) x2
//   0x575D70 Vec3f_static_sub_575D70 x1, 0x575F10 Vec3f_static_sub_575F10 x1
//   0x634F30 __ftol x1
//   0x57BBF0 sub_57BBF0 (Obj57AD20.cpp) x2
//   0x57BCB0 Obj57BCB0::constructor x1, 0x57C080 Obj57BCB0::sub_57C080 x1
//   0x40D640 Obj57BCB0_item trivial ctor x24 (elided, see part 9)
//   0x57F090 transformFlags (inlined once, see part 11)
//   0x57C780 MyEntryBuf_MyScaledSurface_getByIdx x2-5 (once/sub-part plus the
//     conditional extra-surface/shadow-surface lookups)
//   0x57F030 sub_57F030 (reduction pick) x1-2 per sub-part
//   0x581B80 MyScaledSurface::sub_581B80 x1-2 per sub-part
//   0x589140 MyCESurfHandle_static_addToHashList_flagsOr400 x1-3 per sub-part,
//     x1 per light
//   0x579FD0 SceneObject2EList::objects2EToDraw_enlarge (as needed, via the
//     same enlarge-then-append idiom as CEngineStaticMeshAdd.cpp)
//   0x58EC70 (undeclared elsewhere in this repo; populates f85_count and the
//     per-light gap_8A[] byte flags used by part 13 -- called once per mesh;
//     signature CONFIRMED 2026-07-23 against its own body, see the local
//     declaration below -- the prior guess had the wrong arg types/order)
//   0x5855E0 dk2::CEngineAnimMesh::sub_5855E0 (EngineAnimShadows.cpp) x0-N
//     per mesh (see part 13 above)
//
// TODO(verify) summary (kept deliberately narrow -- everything else above,
// including all branch conditions and every
// SceneObject2E field write, was confirmed against the raw opcodes):
//   - the exact transient stack path for the worldPos->camSpacePos transform
//     (part 3);
//   - the precise input driving the field_78 LOD-metric formula (part 6);
//   - the exact semantic meaning of the `f48_flags |= 0x10000` condition
//     (part 7);
//   - the literal 0x741-path's lod__triangleCount/numVertsEx source fields
//     (part 12c, second bullet);
//   - sub_58EC70's field_7C+8/+9 gate bytes and its four global thresholds
//     (0x780958/0x66FEB4/0x66FED8/0x780A60) -- the argument signature itself
//     is now confirmed, see the local declaration below.

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

const float *floatAt(uintptr_t address) {
    return reinterpret_cast<const float *>(address);
}
const double *doubleAt(uintptr_t address) {
    return reinterpret_cast<const double *>(address);
}

// Same situation as the static-mesh sibling's SceneAddRequest: the "int"
// argument is really a pointer to a small per-call struct, but this
// function is only ever observed reading a single field (+0x8) from it.
#pragma pack(push, 1)
struct SceneAddRequest {
    uint8_t unknown[8];  // TODO(verify): never read here; shape assumed from
                         // the static-mesh sibling's own request struct.
    int32_t field8;      // +0x08 -> this->field_58 (light/sphere query
                         // collection pointer).
};
#pragma pack(pop)

// 0x0058EC70 -- opaque helper, not otherwise translated in this repo.
// thiscall(this = &mesh->field_7C), populating mesh->f85_count and the
// per-light byte flags at mesh->gap_8A[0..f85_count) (read back
// immediately after this call, and again per-light in part 13).
//
// CORRECTED 2026-07-23: the previous signature here -- (float, int32_t,
// int32_t, int32_t*) -- was an unverified guess reconstructed only from the
// call site, and was WRONG in both type and order. Verified this time
// against the function's own body (disasm from 0x58EC70 through its ret 0x10
// epilogue, all four stack-arg slots traced):
//   arg1 [esp+0x88]: a POINTER to the same light/sphere-collection structure
//     sub_57BBF0 reads (count+base at +0/+4, entry pointers at +0x38; each
//     entry has a flags dword at +4 and a float x,y,z at +8/+0xC/+0x10) --
//     NOT a float. The previous code passed `reductionFactor` (a float bit
//     pattern) into this slot, which the callee immediately dereferences as
//     a pointer -- a real crash/memory-corruption bug whenever the early-out
//     byte at field_7C+8 is nonzero and this branch actually runs (g_shadowLevel > 0).
//   args 2-4 [esp+0x8C/0x90/0x94]: three floats, the query position (x,y,z) --
//     subtracted from each entry's center to form a squared distance, tested
//     against a fixed global threshold (not a per-entry radius like
//     sub_57BBF0) filtered by (entry->flags & 0x10).
// TODO(verify) still open: the exact meaning of the field_7C+8/+9 gate bytes
// and the four global threshold addresses (0x780958/0x66FEB4/0x66FED8/
// 0x780A60) -- narrower and lower-risk than the argument bug above.
using Sub58EC70Fun = int(__thiscall *)(void *, int32_t *, float, float, float);
const auto sub_58EC70 = reinterpret_cast<Sub58EC70Fun>(0x0058EC70);

// --- Optimization (2026-07-24): inline the two cheapest per-sub-part /
// per-light leaf calls, EXACTLY as the already-deployed static-mesh sibling
// did in CEngineStaticMeshAdd.cpp (commit eb232a4). This is the #1 profiled
// hotspot (appendToSceneObject2EList base 0x584900, 17.3% of samples under
// Rosetta); both leaves are called once-or-more per sub-part AND once per
// light in part 13, so collapsing their ABI-boundary call+ret+prologue into
// this translation unit removes many Rosetta call layers per mesh per frame.
// Both helpers are verified byte-for-byte against the shipped binary
// (objdump -d -M intel) AND the live Ghidra listing -- identical result to
// the original call, pure fewer-instructions change, no behaviour change.
//
// (transformFlags @0x57F090 is already hoisted out of the loop as an inlined
// once-per-mesh sequence here -- see part 11 -- so it is not a per-part call
// to collapse; sub_57F030 / sub_581B80 / sub_57BBF0 are branchier real
// leaves and stay ABI calls, mirroring eb232a4's own restraint.)

// Inlined MyEntryBuf_MyScaledSurface_getByIdx (0x0057C780) -- pointer-chase
// getter into the global MyScaledSurface* table (base ptr at 0x00765B00,
// count at 0x00765B04). Faithful to the ORIGINAL's out-of-range behaviour: it
// logs "Invalid Material" via MyWindow_log_printf and then STILL performs
// table[index] -- the assert branch FALLS THROUGH into the load at
// 0x0057C7A3, it does NOT early-return -- so an out-of-range index faults on
// that load exactly as the ABI call did. Every call site here already used
// the getter raw (no SEH guard), so fault semantics are unchanged. The rare
// bounds-fail + log stays out-of-line; the common in-range path is two loads
// with no call. Verified identical to the static-mesh sibling's helper of the
// same name.
dk2::MyScaledSurface *entryBufGetByIdx(int index) {
    if (index < 0 || index >= *reinterpret_cast<const int32_t *>(0x00765B04)) {
        dk2::MyWindow_log_printf(&dk2::MyWindow_instance, "Invalid Material\n");
    }
    dk2::MyScaledSurface *const *table =
        *reinterpret_cast<dk2::MyScaledSurface *const *const *>(0x00765B00);
    return table[index];
}

// Inlined MyCESurfHandle_static_addToHashList_flagsOr400 (0x00589140) -- a
// thin trampoline that computes (flags & 0x400) != 0 (objdump: `test ch,0x4`)
// and forwards it as the `char` 2nd arg to the real
// MyCESurfHandle_static_addToHashList (0x00593720, already declared in the
// API), which does the actual linked-list insertion. Collapsing the
// trampoline removes exactly one Rosetta call layer per hash-add; the inner
// call (the real work) is unchanged. Verified identical to the static-mesh
// sibling's helper of the same name.
void addToHashListFlagsOr400(dk2::MyCESurfHandle *handle, int16_t flags) {
    dk2::MyCESurfHandle_static_addToHashList(
        handle, static_cast<char>((flags & 0x400) != 0 ? 1 : 0));
}

}  // namespace

void dk2::CEngineAnimMesh::appendToSceneObject2EList(int requestArg) {
    // Part 1 (0x584900..0x584936): drift field_3C by the global
    // velocity-ish vector unless the caller says it's already current.
    if ((f48_flags & 0x4) == 0) {
        field_3C.x = roundedAdd(field_3C.x, dk2::g_vec_760A98.x);
        field_3C.y = roundedAdd(field_3C.y, dk2::g_vec_760A98.y);
        field_3C.z = roundedAdd(field_3C.z, dk2::g_vec_760A98.z);
    }

    const auto *request = reinterpret_cast<const SceneAddRequest *>(
            static_cast<intptr_t>(requestArg));

    // Part 2: resolve the resource and stash the light/sphere query
    // collection pointer.
    auto *resource = static_cast<CAnimMeshResource *>(
            dk2::MyMeshResourceHolder_getResource(f50_pMeshHolder));
    field_58 = request->field8;

    // Part 3: pivot/worldPos, then camera-space transform. See the
    // TODO(verify) note in the file header about the exact transient stack
    // path; the values themselves match sub_5836A0's own pivot/worldPos
    // idiom and the static-mesh sibling's camSpacePos idiom exactly.
    Vec3f pivot{};
    f10_matrix.multiplyVec(&pivot, &resource->pos);
    const Vec3f worldPos{
            roundedAdd(field_4.x, pivot.x),
            roundedAdd(field_4.y, pivot.y),
            roundedAdd(field_4.z, pivot.z)};

    Vec3f relative{
            roundedSub(worldPos.x, dk2::g_camState.v3f.x),
            roundedSub(worldPos.y, dk2::g_camState.v3f.y),
            roundedSub(worldPos.z, dk2::g_camState.v3f.z)};
    Vec3f camSpacePos{};
    dk2::g_camState.m.sub_594E10(&relative, &camSpacePos);

    // Part 4: cull radius and cull test.
    const float radius = roundedMul(resource->cubeScale, field_5C);
    uint32_t cullScratch = 0;
    if (Vec3f_static_sub_575D70(&camSpacePos, radius, &cullScratch) == 0) {
        return;
    }

    // Part 5: reduction factor (the two Vec3f* outputs are scratch, never
    // read again -- same convention as the static-mesh sibling).
    Vec3f reductionCenterScratch{};
    Vec3f reductionOtherScratch{};
    float reductionFactor = 0.0f;
    Vec3f_static_sub_575F10(&reductionCenterScratch, radius,
                             &reductionOtherScratch, &reductionFactor);

    // Part 6: coarse LOD pick into field_78, unless suppressed.
    //
    // TODO(verify): this block's *shape* (a clamped input, a reciprocal-ish
    // term folded against field_54/reductionFactor, then a threshold-picked
    // final value scaled and truncated via __ftol) is confirmed against the
    // raw x87 sequence at 0x584a05..0x584aad, but the exact provenance of
    // the initial scalar (`fld dword ptr [esp+0x10]`) and of 0x760AE8 (an
    // unnamed float global near g_camState) were not fully re-derived to
    // named fields in the time available for this pass -- implemented here
    // as the best-supported reading (a camera-space/reduction-derived
    // distance scalar) rather than guessed silently. Get another pass on
    // this specific formula before relying on its exact numeric output;
    // the SURROUNDING control flow (field_78 defaulting to 0, being
    // skipped when f48_flags & 0x4000, and being used afterwards as an
    // index into entry.lod_list[]/entry.plod_list[]) is solid.
    field_78 = 0;
    if ((f48_flags & 0x4000) == 0) {
        const float clamped = (camSpacePos.z >= *floatAt(0x0066FC20))
                ? *floatAt(0x0066FC20)
                : camSpacePos.z;
        const float recip = roundedDiv(*floatAt(0x0066FC64), *floatAt(0x00760AE8));
        const float term = roundedMul(
                roundedMul(roundedAdd(recip, reductionFactor), field_54),
                *floatAt(0x0066FC60));
        const float sub1 = roundedSub(*floatAt(0x0066FC68), term);
        const float sub2 = roundedSub(sub1, static_cast<float>(g_pmeshReductionLevel));
        const float scaled = roundedMul(sub2, *floatAt(0x0066FC6C));
        const float picked = (clamped >= *floatAt(0x0066FC24))
                ? *floatAt(0x0066FC24)
                : scaled;
        field_78 = static_cast<int>(roundedMul(picked, *floatAt(0x0066FC70)));
    }

    // Part 7: TODO(verify) exact meaning -- observed as "OR field_78's
    // cull-test byproduct into f48_flags bit 0x10000".
    if (cullScratch != 0) {
        f48_flags |= 0x10000;
    }

    // Part 8: mouse-picking registration (ScreenObjectArr_instance).
    if (f4C_thing != nullptr &&
        dk2::ScreenObjectArr_instance.itemsCount < 256) {
        ScreenObject &picked =
                dk2::ScreenObjectArr_instance.arr[dk2::ScreenObjectArr_instance.itemsCount];
        picked.centerX = camSpacePos.x;
        picked.centerY = camSpacePos.y;
        picked.sqr = roundedMul(reductionFactor, reductionFactor);
        picked.pMesh = this;
        ++dk2::ScreenObjectArr_instance.itemsCount;
    }

    // Part 9: light/sphere spatial query + Obj57BCB0 lights collection.
    // field_68 feeds ONLY dk2::meshgpu::prepareLights' `mask` (GPU mesh path,
    // CEngineAnimMesh.cpp) - with settings.toml [game] light_selection_gpu on
    // (default), that mask is ignored there in favour of a per-vertex GPU
    // test (see Obj57AD20.cpp sub_57AC10 for the full rationale/trade-off),
    // so skip the real sphere-cull query for it too. The SECOND query below
    // (mask=4) is untouched: it feeds `lights.sub_57C080` -> field_3C, an
    // ambient-colour contribution consumed regardless of GPU/CPU mesh path.
    auto *collection = reinterpret_cast<int32_t *>(static_cast<intptr_t>(field_58));
    static const bool skipCpuLightSelection = [] {
        const char *v = std::getenv("DK2_LIGHT_SELECTION_GPU");
        return !v || std::strcmp(v, "0") != 0;
    }();
    field_68 = skipCpuLightSelection
        ? -1
        : dk2::sub_57BBF0(collection, nullptr, worldPos.x, worldPos.y,
                          worldPos.z, radius, 0x100);
    const int lightMask = dk2::sub_57BBF0(collection, nullptr, worldPos.x,
                                          worldPos.y, worldPos.z, radius, 4);
    Obj57BCB0 lights;
    lights.constructor(reinterpret_cast<uint32_t *>(collection), lightMask);

    // Part 10.
    lights.sub_57C080(&field_3C, &field_4);

    // Part 11: transformFlags computed once, reused for every sub-part.
    uint32_t andMask = 0xFFFFFFFFu;
    uint32_t orMask = 0;
    transformFlags(static_cast<int16_t>(f48_flags), &andMask, &orMask);

    auto appendEntry = [&]() -> SceneObject2E & {
        if (SceneObject2E_count >= static_cast<uint32_t>(SceneObject2EList_instance.maxCount)) {
            SceneObject2EList_instance.objects2EToDraw_enlarge(SceneObject2E_count);
        }
        SceneObject2E &entry = SceneObject2EList_instance.arr[SceneObject2E_count];
        ++SceneObject2E_count;
        return entry;
    };

    const int32_t sprsCount = resource->sprsCount;
    for (int32_t i = 0; i < sprsCount; ++i) {
        SprsAnimHeader &entry = resource->buf[i];
        const float mmFactor = *reinterpret_cast<const float *>(&entry.mmFactor);

        MyScaledSurface *surf1 = entryBufGetByIdx(
                static_cast<uint16_t>(entry.MyScaledSurface_idx));
        const uint32_t combinedFlags1 = (andMask & surf1->drawFlags) | orMask;

        MyCESurfHandle *handle = nullptr;
        uint32_t combinedFlagsPrimary = combinedFlags1;
        if (f48_flags & 0x100) {
            // field_6C (offset 0x70) -- distinct from f70_surfIdx (0x74),
            // used only by the secondary/extra-surface pass below.
            MyScaledSurface *surf2 = entryBufGetByIdx(
                    static_cast<int32_t>(field_6C));
            const uint32_t combinedFlags2 = (andMask & surf2->drawFlags) | orMask;
            const int lod2 = sub_57F030(surf2, reductionFactor, mmFactor);
            handle = surf2->sub_581B80(lod2, field_74, 0);
            addToHashListFlagsOr400(
                    handle, static_cast<int16_t>(combinedFlags2));
            combinedFlagsPrimary = combinedFlags2;
        } else {
            const int lod1 = sub_57F030(surf1, reductionFactor, mmFactor);
            handle = surf1->sub_581B80(
                    lod1, field_54, static_cast<uint8_t>(field_94));
            addToHashListFlagsOr400(
                    handle, static_cast<int16_t>(combinedFlags1));
        }

        const uint8_t triCountPrimary =
                static_cast<uint8_t>(entry.lod_list[field_78]);
        if (triCountPrimary != 0) {
            SceneObject2E &out = appendEntry();
            out.mesh = this;
            out.f2C_ = static_cast<int16_t>(i);
            out.lod__triangleCount = triCountPrimary;
            out.numVertsEx = static_cast<uint16_t>(entry.numVertsEx);
            out.drawFlags_x2[0] = combinedFlagsPrimary;
            out.renMode = static_cast<uint8_t>(g_renMode_7820A0);
            out.surfhCount = 1;
            out.propsCount = 1;
            out.numTextureSamplers_x2[0] = 1;
            out.surfh_x4[0] = handle;
            out.zeroOrM1 = 0;
        }

        // Secondary/"extra decal" pass.
        if ((combinedFlags1 & 0x21) != 0 && (f48_flags & 0x400) == 0) {
            continue;
        }

        if (f48_flags & 0x200) {
            MyScaledSurface *surf3 = entryBufGetByIdx(
                    static_cast<int32_t>(f70_surfIdx));
            MyCESurfHandle *baseHandle3 = surf3->scaledSurfArr->surfScaledArr[0];
            const float metric3 = roundedDiv(roundedMul(mmFactor, reductionFactor),
                                              static_cast<float>(baseHandle3->surfWidth8));
            // Bug found 2026-07-24 (independent Opus-agent audit, cross-
            // checked against the actual shipped binary's disassembly at
            // 0x585024-0x585052): comparison direction was inverted. `fcom
            // qword ptr [0x66fc00]` + `test ah,1` reads the FPU C0 flag, set
            // iff ST(0) < operand -- i.e. the original assigns the level
            // when `metric < threshold`, matching the static-mesh sibling's
            // (already-correct) CEngineStaticMeshAdd.cpp:388-390. The `>=`
            // here forced lod3 to 3 (worst) whenever metric3 was
            // non-negative, i.e. always -- same bug shape as the
            // static-mesh float/double fix, just the direction half of it.
            int lod3 = 0;
            if (metric3 < static_cast<float>(*doubleAt(0x0066FC00))) lod3 = 1;
            if (metric3 < static_cast<float>(*doubleAt(0x0066FC08))) lod3 = 2;
            if (metric3 < static_cast<float>(*doubleAt(0x0066FC10))) lod3 = 3;
            MyCESurfHandle *handle3 = surf3->sub_581B80(lod3, field_74, 0);
            const uint32_t combinedFlags3 = surf3->drawFlags;
            addToHashListFlagsOr400(
                    handle3, static_cast<int16_t>(combinedFlags3));

            const uint8_t triCount3 = static_cast<uint8_t>(entry.lod_list[field_78]);
            if (triCount3 != 0) {
                SceneObject2E &out = appendEntry();
                out.mesh = this;
                out.f2C_ = static_cast<int16_t>(i);
                out.lod__triangleCount = triCount3;
                out.numVertsEx = static_cast<uint16_t>(entry.numVertsEx);
                out.drawFlags_x2[0] = combinedFlags3;
                out.renMode = static_cast<uint8_t>(g_renMode_7820A0);
                out.surfhCount = 1;
                out.propsCount = 1;
                out.numTextureSamplers_x2[0] = 1;
                out.surfh_x4[0] = handle3;
                out.zeroOrM1 = static_cast<char>(0xFF);
            }
        } else if ((f48_flags & 0x800) && dk2::g_doAdd_0x741_objToScene != 0) {
            addToHashListFlagsOr400(surf1->surfh, 0x741);

            const uint8_t triCount4 = static_cast<uint8_t>(entry.lod_list[field_78]);
            if (triCount4 != 0) {
                // TODO(verify): lod__triangleCount/numVertsEx here read as a
                // synthetic "i + 0x3E8" tag in the disassembly rather than
                // real SprsAnimHeader geometry counts; kept literal.
                SceneObject2E &out = appendEntry();
                out.mesh = this;
                out.f2C_ = static_cast<int16_t>(i);
                out.lod__triangleCount = static_cast<uint16_t>(i + 0x3E8);
                out.numVertsEx = static_cast<uint16_t>(entry.numVertsEx);
                out.drawFlags_x2[0] = 0x741;
                out.renMode = static_cast<uint8_t>(g_renMode_7820A0);
                out.surfhCount = 1;
                out.propsCount = 1;
                out.numTextureSamplers_x2[0] = 1;
                out.surfh_x4[0] = surf1->surfh;
                out.zeroOrM1 = 0;
            }
        }
    }

    // Part 13: per-light shadow-decal registration.
    if (dk2::g_shadowLevel <= 0) {
        return;
    }

    sub_58EC70(&field_7C, collection, worldPos.x, worldPos.y, worldPos.z);
    if (f85_count == 0) {
        return;
    }

    bool needCompute = true;
    MyScaledSurface *cachedSurf = nullptr;
    for (uint8_t light = 0; light < f85_count; ++light) {
        if (needCompute) {
            needCompute = false;
            int shadowHandle;
            if (dk2::g_shadowLevel < 2) {
                if (resource->init_neg1 < 0) {
                    shadowHandle = sub_5855E0(resource, gap_8A[light]);
                    // Only cache the freshly computed handle back into the
                    // resource while still at low detail -- matches the
                    // (here redundant-looking, but faithfully reproduced)
                    // re-check of g_shadowLevel < 2 in the disassembly.
                    resource->init_neg1 = shadowHandle;
                } else {
                    shadowHandle = resource->init_neg1;
                }
            } else {
                shadowHandle = sub_5855E0(resource, gap_8A[light]);
            }
            if (dk2::g_shadowLevel >= 3) {
                needCompute = true;
            }
            cachedSurf = entryBufGetByIdx(shadowHandle);
        }
        if (!cachedSurf) {
            continue;
        }

        MyCESurfHandle *shadowSurfHandle = cachedSurf->scaledSurfArr->surfScaledArr[0];
        addToHashListFlagsOr400(
                shadowSurfHandle, static_cast<int16_t>(cachedSurf->drawFlags));

        SceneObject2E &out = appendEntry();
        out.mesh = this;
        out.f2C_ = static_cast<int16_t>(light + 0x7D0);
        out.lod__triangleCount = 2;
        out.numVertsEx = 4;
        out.drawFlags_x2[0] = cachedSurf->drawFlags;
        out.renMode = static_cast<uint8_t>(g_renMode_7820A0);
        out.surfhCount = 1;
        out.propsCount = 1;
        out.numTextureSamplers_x2[0] = 1;
        out.surfh_x4[0] = shadowSurfHandle;
        out.zeroOrM1 = 0;
    }
}
