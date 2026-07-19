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
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <cstdint>


namespace {

using VertexFun = int (__cdecl *)(uint32_t, dk2::Vec3f *);
using TriangleFun = int (__cdecl *)(uint32_t, uint32_t, uint32_t);
using RenderFun = int (__cdecl *)(uint32_t, dk2::Vec3f *, dk2::Uv2f *);

dk2::Vec3f *animatedPositions() {
    return &dk2::g_vec_766A78;
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
            frameIndex,
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
        dk2::Obj57BCB0 &lights,
        RenderFun renderFun) {
    if (!(dk2::g_idxFlags[index] & 2)) return;

    if (wasCached) {
        animateVertex(resource, animation, frame, frameIndex, index);
    }
    const dk2::Vec3f &position = animatedPositions()[index];
    dk2::Vec3f lightPosition{
            position.x - pivot.x,
            position.y - pivot.y,
            position.z - pivot.z};
    dk2::Vec3f colour = ambient;
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

}  // namespace


void dk2::CEngineAnimMesh::sub_5836A0(int animation, SceneObject2E *scene) {
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
    f10_matrix.multiplyVec(&resource->pos, &pivot);
    Obj57BCB0 lights;
    lights.count = 0;
    if (scene->drawFlags_x2[0] & 0x40) {
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
    const uint32_t triangleCount = static_cast<uint8_t>(entry.triangleCount_list[lod]);

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
                &entry.AnimVertEx_base[a], pivot, ambient, lights, renderFun);
        processVertex(
                b, cachedB, resource, animation, frame, frameIndex,
                &entry.AnimVertEx_base[b], pivot, ambient, lights, renderFun);
        processVertex(
                c, cachedC, resource, animation, frame, frameIndex,
                &entry.AnimVertEx_base[c], pivot, ambient, lights, renderFun);
    }

    applyIndxs_sub_58AC20();
    f50_pMeshHolder->markUsed();
}


void dk2::CEngineAnimMesh::fun_5848B0(int mode, SceneObject2E *scene) {
    if (mode >= 2000) {
        using OriginalFun = void (__thiscall *)(CEngineAnimMesh *, int, SceneObject2E *);
        reinterpret_cast<OriginalFun>(0x00583CA0)(this, mode - 2000, scene);
    } else if (mode >= 1000) {
        sub_583DC0(mode - 1000, scene);
    } else {
        sub_5836A0(mode, scene);
    }
}
