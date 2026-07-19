#include "dk2/Obj57AD20.h"

#include "dk2/MyScaledSurface.h"
#include "dk2/Obj57BCB0.h"
#include "dk2/SceneObject2E.h"
#include "dk2/Uv2f.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <cstddef>
#include <cstdint>


namespace {

#pragma pack(push, 1)
struct MeshVertex {
    dk2::Vec3f position;
    uint32_t packedUv;
    dk2::Vec3f normal;
    dk2::Vec3f color;
};

struct MeshEntry {
    uint32_t unused;
    int surfaceIndex;
    const uint8_t *triangleIndices;
    MeshVertex *vertices;
    uint8_t triangleCount;
    uint8_t padding[3];
};
#pragma pack(pop)

static_assert(sizeof(MeshVertex) == 0x28);
static_assert(offsetof(MeshVertex, position) == 0x00);
static_assert(offsetof(MeshVertex, color) == 0x1C);
static_assert(sizeof(MeshEntry) == 0x14);

using VertexFun = int (__cdecl *)(uint32_t, dk2::Vec3f *);
using TriangleFun = int (__cdecl *)(uint32_t, uint32_t, uint32_t);
using RenderFun = int (__cdecl *)(uint32_t, dk2::Vec3f *, dk2::Uv2f *);

void transformVertex(VertexFun fun, uint32_t index, MeshVertex *vertex) {
    if (fun == reinterpret_cast<VertexFun>(0x0058ACB0)) {
        dk2::sub_58ACB0(index, &vertex->position);
    } else if (fun == reinterpret_cast<VertexFun>(0x0058AD10)) {
        dk2::sub_58AD10(index, &vertex->position);
    } else {
        fun(index, &vertex->position);
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
        MeshVertex *vertex,
        const dk2::Vec3f &ambient,
        dk2::Obj57BCB0 &lights,
        RenderFun renderFun) {
    if (!(dk2::g_idxFlags[index] & 2)) {
        return;
    }

    dk2::Vec3f color{
            ambient.x + vertex->color.x,
            ambient.y + vertex->color.y,
            ambient.z + vertex->color.z};
    lights.sub_57BF00(&color.x, &vertex->position.x, &vertex->normal.x);

    const float uvScale = *reinterpret_cast<const float *>(0x0066FB58);
    const dk2::Uv2f uv{
            static_cast<float>(vertex->packedUv & 0xFFFF) * uvScale,
            static_cast<float>(vertex->packedUv >> 16) * uvScale};
    dk2::Vec3f vectors[2]{color, color};
    dk2::Uv2f uvs[2]{uv, uv};
    emitVertex(renderFun, index, vectors, uvs);
}

}


int *dk2::Obj57AD20::sub_57A9A0(
        int entryIndex,
        SceneObject2E *scene,
        uint32_t *lights,
        int a4,
        int selectExtendedPath,
        int a6,
        int a7,
        float scale) {
    if (selectExtendedPath) {
        return sub_57B0E0(
                entryIndex, scene, a4, lights,
                selectExtendedPath, a6, a7, scale);
    }
    return sub_57B6D0(entryIndex, scene, a4, lights, a6, a7, scale);
}


int *dk2::Obj57AD20::sub_57B6D0(
        int entryIndex,
        SceneObject2E *scene,
        int a3,
        uint32_t *lightData,
        int,
        int,
        float scale) {
    auto &entry = reinterpret_cast<MeshEntry *>(f4)[entryIndex];
    MyScaledSurface *surface = MyEntryBuf_MyScaledSurface_getByIdx(entry.surfaceIndex);
    __renderFun_setSceneObject2E(scene, 1, nullptr, nullptr, scale, a3 == 0);

    const Vec3f ambient{
            vec_14.x + g_vec_760A98.x + surface->vec.x,
            vec_14.y + g_vec_760A98.y + surface->vec.y,
            vec_14.z + g_vec_760A98.z + surface->vec.z};
    Obj57BCB0 lights;
    lights.count = 0;
    lights.constructor(lightData, f2C);

    const VertexFun vertexFun = g_fun_779398;
    const TriangleFun triangleFun = __addTriangleFun;
    const RenderFun renderFun = __renderFun;
    const uint8_t *indices = entry.triangleIndices;
    for (uint32_t triangle = 0; triangle < entry.triangleCount; ++triangle, indices += 3) {
        const uint32_t a = indices[0];
        const uint32_t b = indices[1];
        const uint32_t c = indices[2];
        MeshVertex *va = &entry.vertices[a];
        MeshVertex *vb = &entry.vertices[b];
        MeshVertex *vc = &entry.vertices[c];

        if (g_idxFlags[a] == 0) transformVertex(vertexFun, a, va);
        if (g_idxFlags[b] == 0) transformVertex(vertexFun, b, vb);
        if (g_idxFlags[c] == 0) transformVertex(vertexFun, c, vc);
        emitTriangle(triangleFun, a, b, c);
        processVertex(a, va, ambient, lights, renderFun);
        processVertex(b, vb, ambient, lights, renderFun);
        processVertex(c, vc, ambient, lights, renderFun);
    }
    return applyIndxs_sub_58AC20();
}
