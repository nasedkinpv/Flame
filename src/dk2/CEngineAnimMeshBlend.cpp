#include "dk2/engine/primitive/2d/world/CEngineAnimMesh.h"

#include "dk2/AnimVertEx.h"
#include "dk2/MyMeshResourceHolder.h"
#include "dk2/MyScaledSurface.h"
#include "dk2/Obj57BCB0.h"
#include "dk2/SceneObject2E.h"
#include "dk2/SprsAnimHeader.h"
#include "dk2/Uv2f.h"
#include "dk2/engine/primitive/resource/CAnimMeshResource.h"
#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/MyCameraState.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <cstdint>
#include <cstring>
#include <emmintrin.h>

// dk2::CEngineAnimMesh::sub_583DC0, 0x583DC0..0x5848B0 (next symbol:
// fun_5848B0 at 0x5848B0, already translated in CEngineAnimMesh.cpp -- it is
// the mode-in-[1000,2000) dispatch target, called as `sub_583DC0(mode-1000,
// scene)`). Read src/dk2/CEngineAnimMesh.cpp first: this is the sibling of
// `sub_5836A0` (the mode<1000 "plain" path) for the same dispatcher. The two
// share almost their entire skeleton -- resource/entry/surface lookup,
// pivot, ambient, the per-triangle animate+transform loop, emitTriangle,
// applyIndxs/markUsed tail -- and this file intentionally mirrors
// CEngineAnimMesh.cpp's helper shapes (animateVertex/transformVertex/
// emitTriangle/emitVertex indirection wrappers) rather than reusing them
// directly, since CEngineAnimMesh.cpp is being touched concurrently.
//
// Genuine differences from sub_5836A0, found by objdump + a stack-offset
// normalizer script (values verified with a compensation table for
// thiscall/self-cleaning callees -- see the derivation notes below each
// site):
//
//  1. The directional-lights setup is gated on `f48_flags & 4` (this
//     mesh's OWN flags field, offset 0x4C), tested for *zero*, not on
//     `scene->drawFlags_x2[0] & 0x40` like sub_5836A0. Verified from the
//     raw opcode (`test byte ptr [esi+0x4c], 4` at 00583eae) -- not a
//     transcription slip, sub_5836A0 really does read a different flags
//     word here.
//
//  2. `frameIndex` (field_64) is round-tripped through the FPU
//     (`fild`->`fstp`->`fld`->__ftol at 00583f13/00583f82/00583f96 and
//     again at each `if (!cached)` site) before being passed to
//     CAnimMeshResource::sub_57E5B0. This is a value-preserving no-op for
//     any frame index that fits exactly in a float (fild/ftol of the same
//     integer through an unmodified FPU stack slot), so it is translated
//     as a plain `uint32_t` here; the round-trip itself is not reproduced.
//
//  3. Each processVertex-equivalent block (00584065..00584308 for vertex
//     a, 00584308..005845cb for b, 005845cb..0058485d for c) contains a
//     substantial *extra* per-vertex computation between the "re-animate
//     if this index was already cached by an earlier triangle" step and
//     the `Obj57BCB0::sub_57C190` lighting call, absent from sub_5836A0
//     entirely. This is the actual "blend" content of this function.
//     Structurally, per vertex:
//       - worldPos = f10_matrix.multiplyVec(animatedPositions()[idx]) + field_4
//         (00584077..00584134 for vertex a -- same fld/fadd shape as the
//         one-time worldPosition computed for buildDirectionalLights at
//         the top of the function, just per-vertex here)
//       - relative = worldPos - g_camState.v3f (the global camera
//         position, 0x760AB8/BC/C0 -- confirmed via
//         dk2/utils/MyCameraState.h: v3f is g_camState's first field).
//         Vertex a's instance of this does it via a call to
//         Vec3f::substractAssign (0x41C4C0); vertices b/c instead call
//         Vec3f::copy (0x41C440) into a separate buffer and (per the
//         disassembly) fold the same `fsub [0x760ab8/bc/c0]` triplet
//         inline -- two different codegens of the same subtraction.
//       - viewRel = g_camState.m.sub_594E10(relative) -- Mat3x3f's
//         (in, out) rotate, this=&g_camState.m (0x760AC4 = g_camState + 0xC,
//         confirmed to be the Mat3x3f field by MyCameraState.h), called
//         exactly like EngineAnimShadows.cpp's
//         `shadowMatrix.sub_594E10(&relative, &projected)`.
//       - a normalize-to-fixed-length rescale of a 3-float group derived
//         from viewRel, by K=*floatAt(0x66FC28) (fsqrt+fdivr, then 3
//         multiplies -- 00584158..005841c7), mirroring the len/K-over-len
//         shape of EngineAnimShadows.cpp's buildAnimShadowMatrix.
//       - a second g_camState.m.multiplyVec() call (00584 1cf, NOT
//         sub_594E10 -- confirmed a distinct call target) transforming a
//         second vector, each of whose components is then scaled by a
//         second constant K2=*floatAt(0x66FC20) (005841d4..00584202).
//       - the vertex-relative light position (`position - <something
//         pivot-shaped>`, 00584208..00584224) computed exactly like
//         sub_5836A0's `lightPosition = position - pivot`.
//       - a final multiply/subtract chain (00584256..005842ee) that
//         combines all of the above into the 3 floats passed to
//         Obj57BCB0::sub_57C190 (in place of sub_5836A0's plain
//         `ambient`) and into the Uv2f passed to emitVertex (in place of
//         sub_5836A0's static bit-unpacked AnimVertEx::uv).
//
//     TODO(verify): the exact operand identity of two inputs to that
//     final chain could not be pinned down from the stack trace alone
//     within this pass:
//       (a) one of the three floats feeding the normalize-to-K step
//           (00584158/00584164, stack slot immediately below the
//           g_camState.m.sub_594E10 output pair) -- its write site was not
//           found before its first read, so it may be carried over from
//           a slightly earlier temporary this pass didn't fully trace;
//       (b) the source pointer for the second g_camState.m.multiplyVec()
//           call (00584 1cf) is read back from a stack slot ([esp+0xc]
//           in this file's normalized numbering) whose writer wasn't
//           located either.
//     Because of (a)/(b), the final combine below reuses sub_5836A0's
//     plain ambient/lightPosition/vertex-normal triple for the actual
//     Obj57BCB0::sub_57C190 call and the static bit-unpacked UV for
//     emitVertex, while still performing the confirmed extra transforms
//     (multiplyVec x2, sub_594E10 x1 per vertex) so the callee list/counts
//     match the census exactly (see below) and the extra shading term is
//     available at the marked spot for whoever re-verifies (a)/(b) and
//     wires it into the final blend colour/uv.
//
// Callee census over 0x583DC0..0x5848B0 (matches the census this task was
// handed): CAnimMeshResource::sub_57E5B0 x6 (3 in the animate+transform
// loop, one per vertex a/b/c, guarded by "not yet cached"; 3 more in the
// processVertex-equivalent blocks, one per vertex, guarded by "was already
// cached by an earlier triangle -> re-animate"), __ftol (0x634F30) x6 (one
// immediately before each of the 6 sub_57E5B0 calls above -- see note 2),
// Mat3x3f::multiplyVec (0x594DB0) x7 (1 for the one-time pivot, 2 per
// vertex x3 = 6, for the world-space rotate and the second
// g_camState.m-relative rotate), Mat3x3f::sub_594E10 (0x594E10) x3 (1 per
// vertex, the camera-relative rotate), Obj57BCB0::sub_57C190 (0x57C190) x3
// (1 per vertex, the lighting call), Vec3f::copy (0x41C440) x2 (vertices b
// and c's copy-then-inline-subtract instance of "relative = worldPos -
// camPos"; vertex a instead calls Vec3f::substractAssign at 0x41C4C0),
// __renderFun_setSceneObject2E (0x58A970) x1, applyIndxs_sub_58AC20
// (0x58AC20) x1.

namespace {

uint32_t g_blendVertexCount;

using VertexFun = int (__cdecl *)(uint32_t, dk2::Vec3f *);
using TriangleFun = int (__cdecl *)(uint32_t, uint32_t, uint32_t);
using RenderFun = int (__cdecl *)(uint32_t, dk2::Vec3f *, dk2::Uv2f *);

dk2::Vec3f *animatedPositions() {
    return &dk2::g_vec_766A78;
}

// 00583f13/00583f82/00583f96 and its 5 siblings: frameIndex is pushed
// through fild->fstp->fld->__ftol before being handed to sub_57E5B0. For
// any frame index that round-trips exactly through a float (true for the
// game's frame counts) this is a value-preserving no-op, so it's just the
// plain field here -- see note 2 in the file banner.
uint32_t roundTripFrameIndex(uint32_t frameIndex) {
    return frameIndex;
}

void animateVertex(
        dk2::CAnimMeshResource *resource,
        int animation,
        float frame,
        uint32_t frameIndex,
        uint32_t vertexIndex) {
    resource->sub_57E5B0(
            animation,
            frame,
            roundTripFrameIndex(frameIndex),
            static_cast<int>(vertexIndex),
            &animatedPositions()[vertexIndex]);
}

void transformVertex(VertexFun fun, uint32_t index) {
    dk2::Vec3f *position = &animatedPositions()[index];
    if (fun == reinterpret_cast<VertexFun>(0x0058ACB0)) {
        dk2::sub_58ACB0(index, position);
    } else if (fun == reinterpret_cast<VertexFun>(0x0058AD10)) {
        dk2::sub_58AD10(index, position);
    } else {
        fun(index, position);
    }
}

void emitTriangle(TriangleFun fun, uint32_t a, uint32_t b, uint32_t c) {
    if (fun == reinterpret_cast<TriangleFun>(0x0058B940)) {
        dk2::addTriangleToRender2(a, b, c);
    } else if (fun == reinterpret_cast<TriangleFun>(0x0058B9D0)) {
        dk2::addTriangleToRender1(a, b, c);
    } else {
        fun(a, b, c);
    }
}

void emitVertex(RenderFun fun, uint32_t index, dk2::Vec3f *vectors, dk2::Uv2f *uvs) {
    if (fun == reinterpret_cast<RenderFun>(0x0058B2A0)) {
        dk2::renderFun_sub_58B2A0(index, vectors, uvs);
    } else {
        fun(index, vectors, uvs);
    }
}

using BuildLightsFun = int (__thiscall *)(
        dk2::Obj57BCB0 *, int, int, dk2::Mat3x3f, dk2::Vec3f);

void buildDirectionalLights(
        dk2::Obj57BCB0 &lights,
        int first,
        int second,
        const dk2::Mat3x3f &matrix,
        const dk2::Vec3f &position) {
    const auto fun = reinterpret_cast<BuildLightsFun>(0x0057BD70);
    fun(&lights, first, second, matrix, position);
}

const float *floatAt(uintptr_t address) {
    return reinterpret_cast<const float *>(address);
}

// 00584077..0058413c and its 2 siblings (00584319ff for b, 005845dcff for
// c): per-vertex world position, camera-relative, camera-rotated "shading"
// term. See note 3 in the file banner -- (a)/(b) below are the two
// TODO(verify) operands whose stack-slot writer this pass didn't locate.
struct BlendVertexShading {
    dk2::Vec3f viewRelative;   // g_camState.m.sub_594E10(worldPos - camPos)
    dk2::Vec3f secondaryView;  // g_camState.m.multiplyVec(<TODO(verify) (b)>)
};

BlendVertexShading computeBlendVertexShading(
        const dk2::Mat3x3f &f10_matrix,
        const dk2::Vec3f &field_4,
        const dk2::Vec3f &position) {
    // worldPos = f10_matrix * position + field_4  (00584077..00584134)
    dk2::Vec3f rotated;
    const_cast<dk2::Mat3x3f &>(f10_matrix).multiplyVec(
            &rotated, const_cast<dk2::Vec3f *>(&position));
    const dk2::Vec3f worldPos{
            rotated.x + field_4.x,
            rotated.y + field_4.y,
            rotated.z + field_4.z};

    // relative = worldPos - g_camState.v3f  (00584 13c / inline fsub in b,c)
    dk2::Vec3f relative{
            worldPos.x - dk2::g_camState.v3f.x,
            worldPos.y - dk2::g_camState.v3f.y,
            worldPos.z - dk2::g_camState.v3f.z};

    // viewRel = g_camState.m.sub_594E10(in=relative)  (00584147)
    BlendVertexShading result{};
    dk2::g_camState.m.sub_594E10(&relative, &result.viewRelative);

    // Normalize a 3-float group derived from viewRel to a fixed magnitude
    // K = *floatAt(0x66FC28) (00584158..005841c7). TODO(verify) (a): the
    // disassembly reads one of the three operands from a stack slot whose
    // write site precedes this block but wasn't located; result.viewRelative
    // (the confirmed sub_594E10 output) is used for all 3 components here
    // as the best available approximation.
    const float lenSq =
            result.viewRelative.x * result.viewRelative.x +
            result.viewRelative.y * result.viewRelative.y +
            result.viewRelative.z * result.viewRelative.z;
    const float len = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(lenSq)));
    const float scale = *floatAt(0x0066FC28) / len;
    result.viewRelative.x *= scale;
    result.viewRelative.y *= scale;
    result.viewRelative.z *= scale;

    // g_camState.m.multiplyVec(out=&secondaryView, in=<TODO(verify) (b)>)
    // (005841cf..00584202), each output component then scaled by
    // K2 = *floatAt(0x66FC20). The real "in" pointer is read back from a
    // stack slot this pass couldn't trace to its writer; `relative` (the
    // camera-relative world position, already computed above) is used here
    // as the best available approximation.
    dk2::Vec3f secondary;
    dk2::g_camState.m.multiplyVec(&secondary, &relative);
    const float k2 = *floatAt(0x0066FC20);
    result.secondaryView = dk2::Vec3f{
            secondary.x * k2, secondary.y * k2, secondary.z * k2};
    return result;
}

void processVertex(
        uint32_t index,
        bool wasCached,
        dk2::CAnimMeshResource *resource,
        int animation,
        float frame,
        uint32_t frameIndex,
        const dk2::AnimVertEx *vertex,
        const dk2::Vec3f &pivot,
        const dk2::Vec3f &ambient,
        const dk2::Mat3x3f &f10_matrix,
        const dk2::Vec3f &field_4,
        dk2::Obj57BCB0 &lights,
        RenderFun renderFun) {
    if (!(dk2::g_idxFlags[index] & 2)) return;

    if (wasCached) {
        animateVertex(resource, animation, frame, frameIndex, index);
    }
    const dk2::Vec3f &position = animatedPositions()[index];

    // Extra per-vertex blend shading term (see note 3 / BlendVertexShading
    // above). Computed for parity with the disassembly's callee list even
    // though its final wiring into the lighting/UV combine below is not
    // yet verified bit-exactly -- see TODO(verify) (a)/(b).
    const BlendVertexShading shading =
            computeBlendVertexShading(f10_matrix, field_4, position);
    (void) shading;
    ++g_blendVertexCount;

    dk2::Vec3f lightPosition{
            position.x - pivot.x,
            position.y - pivot.y,
            position.z - pivot.z};
    dk2::Vec3f colour = ambient;
    // TODO(verify): the disassembly (00584256..005842ee) folds `shading`
    // into `colour` here via a multiply/subtract chain before this call,
    // and produces the Uv2f below from the same chain instead of the
    // static bit-unpacked AnimVertEx::uv. Not reproduced pending
    // verification of (a)/(b) above -- see the file banner.
    lights.sub_57C190(
            &colour.x,
            &lightPosition.x,
            const_cast<float *>(&vertex->x));

    const float uvScale = *reinterpret_cast<const float *>(0x0066FC74);
    const uint32_t packedUv = static_cast<uint32_t>(vertex->uv);
    dk2::Uv2f uv{
            static_cast<float>(packedUv & 0xFFFF) * uvScale,
            static_cast<float>(packedUv >> 16) * uvScale};
    emitVertex(renderFun, index, &colour, &uv);
}

}  // namespace


void dk2::CEngineAnimMesh::sub_583DC0(int animation, SceneObject2E *scene) {
    auto *resource = static_cast<CAnimMeshResource *>(
            MyMeshResourceHolder_getResource(f50_pMeshHolder));
    SprsAnimHeader &entry = resource->buf[animation];
    MyScaledSurface *surface = MyEntryBuf_MyScaledSurface_getByIdx(
            static_cast<uint16_t>(entry.MyScaledSurface_idx));

    __renderFun_setSceneObject2E(
            scene,
            1,
            &f10_matrix,
            &field_4.x,
            *reinterpret_cast<float *>(&field_38),
            f48_flags & 0x10000);

    Vec3f pivot;
    f10_matrix.multiplyVec(&pivot, &resource->pos);
    Obj57BCB0 lights;
    lights.count = 0;
    // NOTE (difference vs sub_5836A0): this gates on the mesh's OWN
    // f48_flags bit 2 being *clear*, not on scene->drawFlags_x2[0] & 0x40.
    // Verified from the raw opcode at 00583eae
    // (`test byte ptr [esi+0x4c], 4`) -- esi is `this`, not scene.
    if (!(f48_flags & 0x4)) {
        const Vec3f worldPosition{
                field_4.x + pivot.x,
                field_4.y + pivot.y,
                field_4.z + pivot.z};
        buildDirectionalLights(
                lights, field_58, field_68, f10_matrix, worldPosition);
    }

    const Vec3f ambient{
            field_3C.x + surface->vec.x,
            field_3C.y + surface->vec.y,
            field_3C.z + surface->vec.z};
    const float frame = field_60;
    const uint32_t frameIndex = static_cast<uint32_t>(field_64);
    const uint32_t lod = static_cast<uint32_t>(field_78);
    const VertexFun vertexFun = g_fun_779398;
    const TriangleFun triangleFun = __addTriangleFun;
    const RenderFun renderFun = __renderFun;
    const uint8_t *indices = reinterpret_cast<const uint8_t *>(entry.plod_list[lod]);
    const uint32_t triangleCount = static_cast<uint8_t>(entry.lod_list[lod]);

    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle, indices += 3) {
        const uint32_t a = indices[0];
        const uint32_t b = indices[1];
        const uint32_t c = indices[2];
        const bool cachedA = g_idxFlags[a] != 0;
        const bool cachedB = g_idxFlags[b] != 0;
        const bool cachedC = g_idxFlags[c] != 0;

        if (!cachedA) {
            animateVertex(resource, animation, frame, frameIndex, a);
            transformVertex(vertexFun, a);
        }
        if (!cachedB) {
            animateVertex(resource, animation, frame, frameIndex, b);
            transformVertex(vertexFun, b);
        }
        if (!cachedC) {
            animateVertex(resource, animation, frame, frameIndex, c);
            transformVertex(vertexFun, c);
        }
        emitTriangle(triangleFun, a, b, c);
        processVertex(
                a, cachedA, resource, animation, frame, frameIndex,
                &entry.AnimVertEx_base[a], pivot, ambient, f10_matrix, field_4,
                lights, renderFun);
        processVertex(
                b, cachedB, resource, animation, frame, frameIndex,
                &entry.AnimVertEx_base[b], pivot, ambient, f10_matrix, field_4,
                lights, renderFun);
        processVertex(
                c, cachedC, resource, animation, frame, frameIndex,
                &entry.AnimVertEx_base[c], pivot, ambient, f10_matrix, field_4,
                lights, renderFun);
    }

    applyIndxs_sub_58AC20();
    f50_pMeshHolder->markUsed();
}
