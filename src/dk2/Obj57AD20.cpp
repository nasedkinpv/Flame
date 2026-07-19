#include "dk2/Obj57AD20.h"

#include "dk2/MyScaledSurface.h"
#include "dk2/Obj57BCB0.h"
#include "dk2/Obj58EF60.h"
#include "dk2/SceneObject2E.h"
#include "dk2/Uv2f.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <cstddef>
#include <cstdint>
#include <emmintrin.h>


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

struct SpatialSphere {
    uint32_t unused;
    uint32_t flags;
    dk2::Vec3f center;
    uint8_t padding[0x0C];
    float radius;
};
#pragma pack(pop)

static_assert(sizeof(MeshVertex) == 0x28);
static_assert(offsetof(MeshVertex, position) == 0x00);
static_assert(offsetof(MeshVertex, color) == 0x1C);
static_assert(sizeof(MeshEntry) == 0x14);
static_assert(offsetof(SpatialSphere, flags) == 0x04);
static_assert(offsetof(SpatialSphere, center) == 0x08);
static_assert(offsetof(SpatialSphere, radius) == 0x20);

using VertexFun = int (__cdecl *)(uint32_t, dk2::Vec3f *);
using TriangleFun = int (__cdecl *)(uint32_t, uint32_t, uint32_t);
using RenderFun = int (__cdecl *)(uint32_t, dk2::Vec3f *, dk2::Uv2f *);

void transformPosition(VertexFun fun, uint32_t index, dk2::Vec3f *position) {
    if (fun == reinterpret_cast<VertexFun>(0x0058ACB0)) {
        dk2::sub_58ACB0(index, position);
    } else if (fun == reinterpret_cast<VertexFun>(0x0058AD10)) {
        dk2::sub_58AD10(index, position);
    } else {
        fun(index, position);
    }
}

void transformVertex(VertexFun fun, uint32_t index, MeshVertex *vertex) {
    transformPosition(fun, index, &vertex->position);
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


namespace dk2 {

int __fastcall sub_57BBF0(
        int32_t *opaqueCollection, void *,
        float x, float y, float z, float radius, int mask) {
    const int32_t first = opaqueCollection[0];
    int32_t begin = 0;
    int32_t end = first + opaqueCollection[1];
    uint32_t resultBit = 1;
    if ((mask & 1) != 0) {
        begin = first;
        resultBit <<= static_cast<uint32_t>(first) & 31;
    }
    if ((mask & 0x20) != 0) {
        end = first;
    }
    if (begin >= end) {
        return 0;
    }

    const auto spheres = reinterpret_cast<const SpatialSphere *const *>(
            reinterpret_cast<const uint8_t *>(opaqueCollection) + 0x38);
    const __m128 px = _mm_set1_ps(x);
    const __m128 py = _mm_set1_ps(y);
    const __m128 pz = _mm_set1_ps(z);
    const __m128 queryRadius = _mm_set1_ps(radius);
    uint32_t result = 0;

    for (int32_t i = begin; i < end; i += 4) {
        const int32_t remaining = end - i;
        const int32_t laneCount = remaining < 4 ? remaining : 4;
        const SpatialSphere *items[4];
        for (int lane = 0; lane < 4; ++lane) {
            items[lane] = spheres[i + (lane < laneCount ? lane : 0)];
        }

        uint32_t eligible = 0;
        const uint32_t queryMask = static_cast<uint32_t>(mask);
        for (int lane = 0; lane < laneCount; ++lane) {
            if ((items[lane]->flags & queryMask) == queryMask) {
                eligible |= 1u << lane;
            }
        }
        if (eligible != 0) {
            const __m128 dx = _mm_sub_ps(px, _mm_set_ps(
                    items[3]->center.x, items[2]->center.x,
                    items[1]->center.x, items[0]->center.x));
            const __m128 dy = _mm_sub_ps(py, _mm_set_ps(
                    items[3]->center.y, items[2]->center.y,
                    items[1]->center.y, items[0]->center.y));
            const __m128 dz = _mm_sub_ps(pz, _mm_set_ps(
                    items[3]->center.z, items[2]->center.z,
                    items[1]->center.z, items[0]->center.z));
            const __m128 distanceSquared = _mm_add_ps(
                    _mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)),
                    _mm_mul_ps(dz, dz));
            const __m128 radiusSum = _mm_add_ps(queryRadius, _mm_set_ps(
                    items[3]->radius, items[2]->radius,
                    items[1]->radius, items[0]->radius));
            const uint32_t overlaps = static_cast<uint32_t>(_mm_movemask_ps(
                    _mm_sub_ps(distanceSquared, _mm_mul_ps(radiusSum, radiusSum))))
                    & eligible;
            for (int lane = 0; lane < laneCount; ++lane) {
                if ((overlaps & (1u << lane)) != 0) {
                    result |= resultBit << lane;
                }
            }
        }
        resultBit <<= 4;
    }
    return static_cast<int>(result);
}

}  // namespace dk2


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


int *dk2::Obj57AD20::sub_57B0E0(
        int entryIndex,
        SceneObject2E *scene,
        int a3,
        uint32_t *lightData,
        int vectorField,
        int fieldOriginX,
        int fieldOriginY,
        float scale) {
    auto &entry = reinterpret_cast<MeshEntry *>(f4)[entryIndex];
    MyScaledSurface *surface = MyEntryBuf_MyScaledSurface_getByIdx(entry.surfaceIndex);
    __renderFun_setSceneObject2E(scene, 1, nullptr, nullptr, scale, a3 == 0);

    Vec3f ambient{
            vec_14.x + g_vec_760A98.x + surface->vec.x,
            vec_14.y + g_vec_760A98.y + surface->vec.y,
            vec_14.z + g_vec_760A98.z + surface->vec.z};
    if (*reinterpret_cast<const int *>(0x00760B8C) != 0) {
        ambient = {255.0f, 0.0f, 0.0f};
    }

    Obj58EF60 sampler{
            vectorField,
            static_cast<float>(fieldOriginX - 1),
            static_cast<float>(fieldOriginY - 1)};
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

        if (g_idxFlags[a] == 0) {
            Vec3f sampled;
            sampler.sub_58F030(
                    va->position.x, va->position.y, va->position.z, &sampled.x);
            transformPosition(vertexFun, a, &sampled);
        }
        if (g_idxFlags[b] == 0) {
            Vec3f sampled;
            sampler.sub_58F030(
                    vb->position.x, vb->position.y, vb->position.z, &sampled.x);
            transformPosition(vertexFun, b, &sampled);
        }
        if (g_idxFlags[c] == 0) {
            Vec3f sampled;
            sampler.sub_58F030(
                    vc->position.x, vc->position.y, vc->position.z, &sampled.x);
            transformPosition(vertexFun, c, &sampled);
        }
        emitTriangle(triangleFun, a, b, c);
        processVertex(a, va, ambient, lights, renderFun);
        processVertex(b, vb, ambient, lights, renderFun);
        processVertex(c, vc, ambient, lights, renderFun);
    }
    return applyIndxs_sub_58AC20();
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
