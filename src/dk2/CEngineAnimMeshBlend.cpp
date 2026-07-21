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
// scene)`). This is the sibling of `sub_5836A0` (the mode<1000 "plain"
// path): same resource/entry/surface lookup, pivot, per-triangle
// animate+transform loop, emitTriangle, applyIndxs/markUsed tail. Read
// src/dk2/CEngineAnimMesh.cpp first.
//
// STATUS: fully re-verified instruction-by-instruction, including the extra
// per-vertex chain (see below). Re-enabled in src/CMakeLists.txt.
//
// Method: objdump of the whole range + a stack-offset normalizer script
// (scratchpad/norm583dc0_v2.py) with a compensation table for
// thiscall/self-cleaning callees, cross-checked by objdump'ing each callee
// directly and reading its own `ret N`. That cross-check caught a real bug:
// 0x41C4C0 is `ret 0x8` (2 explicit stack args: `out`, `other`; `this`=`a`;
// computes `*out = *this - *other`), not `ret 0x4` as first assumed by
// analogy with its neighbour 0x41C440 (`Vec3f::copy`, genuinely `ret 0x4`,
// 1 arg). That single wrong compensation value made every stack slot after
// it in the per-vertex block appear 4 bytes off from where it truly is,
// which is what made two operands look untraceable in the previous pass.
// With the fix, every slot resolves cleanly and self-consistently (e.g. the
// `this` for the second `Mat3x3f::multiplyVec` per vertex reads back
// exactly the same slot, offset 0x30, that caches `&f10_matrix` at the top
// of the function) and blocks a/b/c decode to the *same* formula modulo
// register renaming.
//
// Genuine differences from sub_5836A0 (all re-confirmed against the
// disassembly, including the callees' own prologues/epilogues where
// relevant):
//
//  1. The directional-lights setup is gated on `f48_flags & 4` (this
//     mesh's OWN flags field, offset 0x4C), tested for *zero*, not on
//     `scene->drawFlags_x2[0] & 0x40`. Raw opcode: `test byte ptr
//     [esi+0x4c], 4` at 00583eae, esi = `this`.
//
//  2. `frameIndex` (field_64) is round-tripped through the FPU
//     (`fild`->`fstp`->`fld`->`__ftol`) before being passed to
//     CAnimMeshResource::sub_57E5B0 -- value-preserving no-op for any
//     frame index that round-trips exactly through a float, translated as
//     a plain `uint32_t` pass-through.
//
//  3. Each processVertex-equivalent block (00584065..00584308 for vertex
//     a, 00584308..005845cb for b, 005845cb..0058485d for c) computes an
//     extra per-vertex term feeding BOTH the Obj57BCB0::sub_57C190 call's
//     initial colour AND the emitted UV, entirely absent from sub_5836A0.
//     This is the "blend" content of the function -- a per-vertex
//     reflection/environment-map term, confirmed as follows (vertex a's
//     addresses; b/c are the same formula, registers renamed):
//
//       rotated     = f10_matrix.multiplyVec(animatedPositions()[idx])   (5840e1)
//       worldPos    = field_4 + rotated                                  (584107..584134)
//       relative    = worldPos - g_camState.v3f                          (58413c, call 0x41C4C0:
//                     `this`=&worldPos, `other`=literal address 0x760AB8
//                     == &g_camState (v3f is g_camState's first field, see
//                     dk2/utils/MyCameraState.h), `out`=a fresh buffer --
//                     confirmed by objdump'ing 0x41C4C0 itself: `this=ecx`,
//                     `out`=[esp+0x10], `other`=[esp+0x14] from the
//                     callee's own frame, `*out = *this - *other`, ret 8)
//       viewRel     = g_camState.m.sub_594E10(in=relative)                (584147, in/out
//                     convention confirmed against EngineAnimShadows.cpp's
//                     own `shadowMatrix.sub_594E10(&relative, &projected)`
//                     usage: first-declared param is `in`)
//       viewDir     = viewRel * (K1 / |viewRel|), K1 = *floatAt(0x66FC28)  (584158..5841cf,
//                     a "normalize to fixed magnitude K1" -- same
//                     len/K-over-len shape as EngineAnimShadows.cpp's
//                     buildAnimShadowMatrix, in full 3D here)
//       worldNormal = f10_matrix.multiplyVec(&vertex->x)                  (5841cf, confirmed:
//                     `this` for this second multiplyVec reads back slot
//                     0x30, the SAME slot that caches `&f10_matrix` at the
//                     top of the function; `in` is `&vertex->x`, the very
//                     same AnimVertEx normal pointer passed to
//                     sub_57C190's 3rd argument below)
//       scaledNormal = worldNormal * K2, K2 = *floatAt(0x66FC20)          (5841d4..584202)
//       lightPosition = position - pivot                                  (584208..584224,
//                     byte-identical to sub_5836A0's own lightPosition)
//       colour      = field_3C  (NOT field_3C + surface->vec -- this
//                     function never dereferences MyScaledSurface::vec at
//                     all; the surface pointer returned by
//                     MyEntryBuf_MyScaledSurface_getByIdx is immediately
//                     read at offset 0x1D, `f1D`, and never kept around,
//                     confirmed by the fact `colour` is initialised from
//                     the SAME field_3C.x/y/z cache used at the top of the
//                     function, offsets 0x6c/0x70/0x74, re-read verbatim
//                     into the per-vertex colour buffer at 58407b/584086/
//                     58408e)
//       lights.sub_57C190(&colour.x, &lightPosition.x, &vertex->x)        (584251 --
//                     lightPosition/vertex-normal args are byte-identical
//                     to sub_5836A0's call; only the initial `colour`
//                     value differs, see above. sub_57C190 accumulates
//                     directional-light contributions into `colour` in
//                     place -- see Obj57BCB0.cpp's `accumulateDirectional`.)
//       surfaceFactor = bit_cast<float>(surface->f1D)                     (offset 0x24, cached
//                     once per call from the one-time surface lookup)
//       finalColour = colour * surfaceFactor  (componentwise, post-call)  (584256..5842ee)
//       uv.u = (scaledNormal.z*viewDir.x - scaledNormal.x*viewDir.z) - K3
//       uv.v = (scaledNormal.z*viewDir.y - scaledNormal.y*viewDir.z) - K3
//         where K3 = (float)*(const double *)0x66FC78 (widened-then-
//         rounded-once per this repo's single-rounding convention). The
//         two UV components are exactly the two non-zero components of
//         cross(scaledNormal, viewDir), each biased by -K3 -- a classic
//         cross-product/"sphere map" environment-mapped UV, matching a
//         glow/energy-shimmer look for the blend-transition render path.
//       emitVertex(index, &finalColour, &uv)                              (5842f8, in place of
//                     sub_5836A0's static bit-unpacked AnimVertEx::uv)
//
// Callee census over 0x583DC0..0x5848B0 (matches the census this task was
// handed): CAnimMeshResource::sub_57E5B0 x6, __ftol (0x634F30) x6,
// Mat3x3f::multiplyVec (0x594DB0) x7 (1 one-time pivot + 2 per vertex x3),
// Mat3x3f::sub_594E10 (0x594E10) x3 (1 per vertex), Obj57BCB0::sub_57C190
// (0x57C190) x3 (1 per vertex), Vec3f "subtractInto" (0x41C4C0) x1 (vertex
// a only) and Vec3f::copy (0x41C440) x2 (vertices b/c: a per-instance
// codegen difference computing the same `relative = worldPos - camPos`
// step -- b/c copy worldPos into a scratch buffer and fold the
// `fsub [0x760ab8/bc/c0]` triplet inline instead of calling 0x41C4C0),
// __renderFun_setSceneObject2E (0x58A970) x1, applyIndxs_sub_58AC20
// (0x58AC20) x1.

namespace {

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
// plain field here.
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

float doubleConstAsFloat(uintptr_t address) {
    return static_cast<float>(*reinterpret_cast<const double *>(address));
}

// 00584077..00584251 and its 2 siblings (00584319ff for b, 005845dcff for
// c) -- see note 3 in the file banner for the full byte-verified
// derivation of each field below.
struct BlendVertexShading {
    dk2::Vec3f viewDir;       // camera-relative direction, normalized to K1
    dk2::Vec3f scaledNormal;  // world-space vertex normal, scaled by K2
};

BlendVertexShading computeBlendVertexShading(
        const dk2::Mat3x3f &f10_matrix,
        const dk2::Vec3f &field_4,
        const dk2::Vec3f &position,
        const dk2::AnimVertEx &vertex) {
    // rotated = f10_matrix * position; worldPos = field_4 + rotated
    dk2::Vec3f rotated;
    const_cast<dk2::Mat3x3f &>(f10_matrix).multiplyVec(
            &rotated, const_cast<dk2::Vec3f *>(&position));
    const dk2::Vec3f worldPos{
            field_4.x + rotated.x, field_4.y + rotated.y, field_4.z + rotated.z};

    // relative = worldPos - g_camState.v3f  (0x41C4C0: out = this - other)
    const dk2::Vec3f relative{
            worldPos.x - dk2::g_camState.v3f.x,
            worldPos.y - dk2::g_camState.v3f.y,
            worldPos.z - dk2::g_camState.v3f.z};

    // viewRel = g_camState.m.sub_594E10(in=relative)
    dk2::Vec3f viewRel;
    dk2::g_camState.m.sub_594E10(
            const_cast<dk2::Vec3f *>(&relative), &viewRel);

    // viewDir = viewRel * (K1 / |viewRel|)
    const float lenSq =
            viewRel.x * viewRel.x + viewRel.y * viewRel.y + viewRel.z * viewRel.z;
    const float len = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(lenSq)));
    const float k1 = *floatAt(0x0066FC28);
    const float scale = _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(k1), _mm_set_ss(len)));
    const dk2::Vec3f viewDir{
            viewRel.x * scale, viewRel.y * scale, viewRel.z * scale};

    // worldNormal = f10_matrix * vertex-normal (same matrix, no translation)
    dk2::Vec3f worldNormal;
    const_cast<dk2::Mat3x3f &>(f10_matrix).multiplyVec(
            &worldNormal, const_cast<dk2::Vec3f *>(
                                  reinterpret_cast<const dk2::Vec3f *>(&vertex.x)));
    const float k2 = *floatAt(0x0066FC20);
    const dk2::Vec3f scaledNormal{
            worldNormal.x * k2, worldNormal.y * k2, worldNormal.z * k2};

    return BlendVertexShading{viewDir, scaledNormal};
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
        const dk2::Vec3f &field_3C,
        float surfaceFactor,
        const dk2::Mat3x3f &f10_matrix,
        const dk2::Vec3f &field_4,
        dk2::Obj57BCB0 &lights,
        RenderFun renderFun) {
    if (!(dk2::g_idxFlags[index] & 2)) return;

    if (wasCached) {
        animateVertex(resource, animation, frame, frameIndex, index);
    }
    const dk2::Vec3f &position = animatedPositions()[index];

    const BlendVertexShading shading =
            computeBlendVertexShading(f10_matrix, field_4, position, *vertex);

    const dk2::Vec3f lightPosition{
            position.x - pivot.x,
            position.y - pivot.y,
            position.z - pivot.z};

    // colour starts as field_3C alone (this function never adds
    // surface->vec, unlike sub_5836A0's ambient -- see note 3), then
    // Obj57BCB0::sub_57C190 accumulates directional-light contributions
    // into it in place.
    dk2::Vec3f colour = field_3C;
    lights.sub_57C190(
            &colour.x,
            &lightPosition.x,
            const_cast<float *>(&vertex->x));

    const dk2::Vec3f finalColour{
            colour.x * surfaceFactor,
            colour.y * surfaceFactor,
            colour.z * surfaceFactor};

    // uv = cross(scaledNormal, viewDir).zx-plane components, biased by
    // -K3 -- see note 3 for the full derivation.
    const float k3 = doubleConstAsFloat(0x0066FC78);
    const dk2::Uv2f uv{
            (shading.scaledNormal.z * shading.viewDir.x -
             shading.scaledNormal.x * shading.viewDir.z) - k3,
            (shading.scaledNormal.z * shading.viewDir.y -
             shading.scaledNormal.y * shading.viewDir.z) - k3};

    emitVertex(renderFun, index, const_cast<dk2::Vec3f *>(&finalColour),
               const_cast<dk2::Uv2f *>(&uv));
}

}  // namespace


void dk2::CEngineAnimMesh::sub_583DC0(int animation, SceneObject2E *scene) {
    auto *resource = static_cast<CAnimMeshResource *>(
            MyMeshResourceHolder_getResource(f50_pMeshHolder));
    SprsAnimHeader &entry = resource->buf[animation];
    MyScaledSurface *surface = MyEntryBuf_MyScaledSurface_getByIdx(
            static_cast<uint16_t>(entry.MyScaledSurface_idx));
    // 00583e46: the surface pointer itself is never kept -- only f1D
    // (offset 0x1D) is read, reinterpreted as a float scale factor used to
    // modulate the final per-vertex colour (see processVertex). Unlike
    // sub_5836A0, `surface->vec` is never dereferenced by this function.
    float surfaceFactor;
    std::memcpy(&surfaceFactor, &surface->f1D, sizeof(surfaceFactor));

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
                &entry.AnimVertEx_base[a], pivot, field_3C, surfaceFactor,
                f10_matrix, field_4, lights, renderFun);
        processVertex(
                b, cachedB, resource, animation, frame, frameIndex,
                &entry.AnimVertEx_base[b], pivot, field_3C, surfaceFactor,
                f10_matrix, field_4, lights, renderFun);
        processVertex(
                c, cachedC, resource, animation, frame, frameIndex,
                &entry.AnimVertEx_base[c], pivot, field_3C, surfaceFactor,
                f10_matrix, field_4, lights, renderFun);
    }

    applyIndxs_sub_58AC20();
    f50_pMeshHolder->markUsed();
}
