#include "dk2/engine/primitive/2d/world/CEngineDynamicMesh.h"

#include "dk2/MeshVertEx.h"
#include "dk2/MyMeshResourceHolder.h"
#include "dk2/MyScaledSurface.h"
#include "dk2/Obj57BCB0.h"
#include "dk2/SceneObject2E.h"
#include "dk2/SprsMeshHeader.h"
#include "dk2/Triangle.h"
#include "dk2/Uv2f.h"
#include "dk2/engine/primitive/resource/CPolyMeshResource.h"
#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <cstdint>


namespace {

using VertexFun = int (__cdecl *)(uint32_t, dk2::Vec3f *);
using TriangleFun = int (__cdecl *)(uint32_t, uint32_t, uint32_t);
using RenderFun = int (__cdecl *)(uint32_t, dk2::Vec3f *, dk2::Uv2f *);

void transformVertex(VertexFun fun, uint32_t index, dk2::Vec3f *position) {
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

void emitVertex(RenderFun fun, uint32_t index, dk2::Vec3f *colour, dk2::Uv2f *uv) {
    if (fun == reinterpret_cast<RenderFun>(0x0058B2A0)) {
        dk2::renderFun_sub_58B2A0(index, colour, uv);
    } else if (fun == reinterpret_cast<RenderFun>(0x0058B370)) {
        dk2::renderFun_sub_58B370(index, colour, uv);
    } else if (fun == reinterpret_cast<RenderFun>(0x0058B440)) {
        dk2::renderFun_sub_58B440(index, colour, uv);
    } else {
        fun(index, colour, uv);
    }
}

void processVertex(
        uint32_t index,
        const dk2::MeshVertEx &vertex,
        const dk2::Vec3f *geometry,
        const dk2::Vec3f &pivot,
        const dk2::Vec3f &ambient,
        dk2::Obj57BCB0 &lights,
        RenderFun renderFun) {
    if (!(dk2::g_idxFlags[index] & 2)) return;

    const dk2::Vec3f &position = geometry[vertex.index];
    dk2::Vec3f lightPosition{
            position.x - pivot.x,
            position.y - pivot.y,
            position.z - pivot.z};
    dk2::Vec3f colour = ambient;
    lights.sub_57C190(
            &colour.x,
            &lightPosition.x,
            const_cast<float *>(&vertex.x));

    const float uvScale = *reinterpret_cast<const float *>(0x0066FC74);
    const uint32_t packedUv = static_cast<uint32_t>(vertex.uv);
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


void dk2::CEngineDynamicMesh::sub_581BE0(int meshIndex, SceneObject2E *scene) {
    auto *resource = static_cast<CPolyMeshResource *>(
            MyMeshResourceHolder_getResource(f34_pMyMeshResourceHolder));
    SprsMeshHeader &entry = resource->ptr[meshIndex];
    MyScaledSurface *surface = MyEntryBuf_MyScaledSurface_getByIdx(
            entry.MyScaledSurface_idx);

    __renderFun_setSceneObject2E(
            scene,
            1,
            &f10_mat,
            &field_4.x,
            *reinterpret_cast<float *>(&field_44),
            f4C_dflags & 0x10000);

    const Vec3f ambient{
            f38_vec.x + surface->vec.x,
            f38_vec.y + surface->vec.y,
            f38_vec.z + surface->vec.z};
    Vec3f pivot;
    f10_mat.multiplyVec(&resource->pos, &pivot);

    Obj57BCB0 lights;
    lights.count = 0;
    if (scene->drawFlags_x2[0] & 0x40) {
        const Vec3f worldPosition{
                field_4.x + pivot.x,
                field_4.y + pivot.y,
                field_4.z + pivot.z};
        buildDirectionalLights(
                lights, f58_pTrgObj, field_5C, f10_mat, worldPosition);
    }

    const uint32_t lod = static_cast<uint32_t>(field_6C);
    const Triangle *triangles = entry.pvertice_list[lod];
    const uint32_t triangleCount = entry.triangleCount_list[lod];
    const Vec3f *geometry = resource->geom_base;
    const VertexFun vertexFun = g_fun_779398;
    const TriangleFun triangleFun = __addTriangleFun;
    const RenderFun renderFun = __renderFun;

    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
        const uint32_t a = triangles[triangle].x;
        const uint32_t b = triangles[triangle].y;
        const uint32_t c = triangles[triangle].z;
        const MeshVertEx &vertexA = entry.MeshVertEx_base[a];
        const MeshVertEx &vertexB = entry.MeshVertEx_base[b];
        const MeshVertEx &vertexC = entry.MeshVertEx_base[c];

        if (!g_idxFlags[a]) {
            transformVertex(vertexFun, a, &resource->geom_base[vertexA.index]);
        }
        if (!g_idxFlags[b]) {
            transformVertex(vertexFun, b, &resource->geom_base[vertexB.index]);
        }
        if (!g_idxFlags[c]) {
            transformVertex(vertexFun, c, &resource->geom_base[vertexC.index]);
        }
        emitTriangle(triangleFun, a, b, c);
        processVertex(a, vertexA, geometry, pivot, ambient, lights, renderFun);
        processVertex(b, vertexB, geometry, pivot, ambient, lights, renderFun);
        processVertex(c, vertexC, geometry, pivot, ambient, lights, renderFun);
    }

    applyIndxs_sub_58AC20();
    f34_pMyMeshResourceHolder->markUsed();
}


void dk2::CEngineDynamicMesh::fun_582CE0(int mode, SceneObject2E *scene) {
    using OriginalModeFun = void (__thiscall *)(
            CEngineDynamicMesh *, int, SceneObject2E *);
    if (mode >= 2000) {
        reinterpret_cast<OriginalModeFun>(0x00582180)(this, mode - 2000, scene);
    } else if (mode >= 1000) {
        reinterpret_cast<OriginalModeFun>(0x00582290)(this, mode - 1000, scene);
    } else {
        sub_581BE0(mode, scene);
    }
}
