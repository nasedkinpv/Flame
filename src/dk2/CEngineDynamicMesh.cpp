#include "dk2/engine/primitive/2d/world/CEngineDynamicMesh.h"

#include "dk2/MeshGpuEmit.h"
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
#include <metal_bridge/DK2BridgeProtocol.h>

#include <cstdint>
#include <cstring>


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

uint32_t retainedDynamicMesh(
        dk2::CPolyMeshResource *resource, dk2::SprsMeshHeader &entry,
        const dk2::Triangle *triangles, uint32_t triangleCount,
        const dk2::meshgpu::InlineTarget &target) {
    struct CacheEntry {
        const void *resource;
        const void *vertices;
        const void *triangles;
        uint32_t triangleCount;
        uint32_t textureId;
        uint32_t uvBits[4];
        uint32_t meshId;
    };
    static CacheEntry cache[4096] = {};
    uint32_t uvBits[4];
    std::memcpy(&uvBits[0], &target.uS, 4);
    std::memcpy(&uvBits[1], &target.vS, 4);
    std::memcpy(&uvBits[2], &target.uO, 4);
    std::memcpy(&uvBits[3], &target.vO, 4);
    uintptr_t hash = reinterpret_cast<uintptr_t>(resource) >> 4;
    hash ^= reinterpret_cast<uintptr_t>(triangles) >> 3;
    hash ^= static_cast<uintptr_t>(target.textureId) * 2654435761u;
    uint32_t slot = static_cast<uint32_t>(hash) & 4095u;
    CacheEntry *available = nullptr;
    for (uint32_t probe = 0; probe < 32; ++probe, slot = (slot + 1) & 4095u) {
        CacheEntry &candidate = cache[slot];
        if (!candidate.resource) {
            available = &candidate;
            break;
        }
        if (candidate.resource == resource &&
            candidate.vertices == entry.MeshVertEx_base &&
            candidate.triangles == triangles &&
            candidate.triangleCount == triangleCount &&
            candidate.textureId == target.textureId &&
            std::memcmp(candidate.uvBits, uvBits, sizeof(uvBits)) == 0) {
            return candidate.meshId;
        }
    }
    if (!available || !triangleCount || triangleCount > 255) return 0;
    static DK2MMeshVertex vertices[256];
    static uint16_t indices[765];
    uint16_t mapped[256];
    for (uint16_t &value : mapped) value = 0xFFFFu;
    const float uvScale = *reinterpret_cast<const float *>(0x0066FC74);
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
        const uint8_t sourceIndices[3] = {
            triangles[triangle].x, triangles[triangle].y, triangles[triangle].z};
        for (uint8_t source : sourceIndices) {
            if (mapped[source] == 0xFFFFu) {
                if (vertexCount == 256) return 0;
                const dk2::MeshVertEx &mv = entry.MeshVertEx_base[source];
                const dk2::Vec3f &position = resource->geom_base[mv.index];
                DK2MMeshVertex &vertex = vertices[vertexCount];
                vertex.px = position.x;
                vertex.py = position.y;
                vertex.pz = position.z;
                vertex.nx = mv.x;
                vertex.ny = mv.y;
                vertex.nz = mv.z;
                const uint32_t packed = static_cast<uint32_t>(mv.uv);
                vertex.u = target.uS *
                               (static_cast<float>(packed & 0xFFFFu) * uvScale) +
                           target.uO;
                vertex.v = target.vS *
                               (static_cast<float>(packed >> 16) * uvScale) +
                           target.vO;
                vertex.base_color = 0xFF000000u;
                mapped[source] = static_cast<uint16_t>(vertexCount++);
            }
            indices[indexCount++] = mapped[source];
        }
    }
    const uint32_t meshId = dk2::meshgpu::allocateMeshId();
    if (!dk2::meshgpu::registerMesh(
            meshId, vertices, vertexCount, indices, indexCount)) {
        return 0;
    }
    available->resource = resource;
    available->vertices = entry.MeshVertEx_base;
    available->triangles = triangles;
    available->triangleCount = triangleCount;
    available->textureId = target.textureId;
    std::memcpy(available->uvBits, uvBits, sizeof(uvBits));
    available->meshId = meshId;
    return meshId;
}

bool drawDynamicOnGpu(
        dk2::CEngineDynamicMesh *mesh, dk2::SceneObject2E *scene,
        dk2::MyScaledSurface *surface, dk2::CPolyMeshResource *resource,
        dk2::SprsMeshHeader &entry, const dk2::Triangle *triangles,
        uint32_t triangleCount, const dk2::Vec3f &ambient, bool lit) {
    dk2::meshgpu::InlineTarget target;
    if (!dk2::meshgpu::prepareTarget(scene, surface, lit, &target) ||
        !target.textureId) {
        return false;
    }
    const uint32_t meshId = retainedDynamicMesh(
        resource, entry, triangles, triangleCount, target);
    if (!meshId) return false;
    dk2::meshgpu::LightSelection lights = {};
    auto *collection = lit
        ? reinterpret_cast<uint32_t *>(static_cast<uintptr_t>(mesh->f58_pTrgObj))
        : nullptr;
    if (!dk2::meshgpu::prepareLights(
            collection, lit ? static_cast<uint32_t>(mesh->field_5C) : 0u,
            &lights)) {
        return false;
    }
    const float scale = *reinterpret_cast<const float *>(&mesh->field_44);
    float world[12];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            world[row * 4 + column] = mesh->f10_mat.m[row][column] * scale;
        }
    }
    world[3] = mesh->field_4.x;
    world[7] = mesh->field_4.y;
    world[11] = mesh->field_4.z;
    dk2::meshgpu::emitCamera();
    dk2::meshgpu::emitRetained(
        target, meshId, world, lights,
        ambient.x / 255.0f, ambient.y / 255.0f, ambient.z / 255.0f);
    return true;
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
    const uint32_t lod = static_cast<uint32_t>(field_6C);
    const Triangle *triangles = entry.pvertice_list[lod];
    const uint32_t triangleCount = entry.triangleCount_list[lod];
    const bool lit = (scene->drawFlags_x2[0] & 0x40) != 0;
    if (dk2::meshgpu::active() &&
        drawDynamicOnGpu(this, scene, surface, resource, entry, triangles,
                         triangleCount, ambient, lit)) {
        f34_pMyMeshResourceHolder->markUsed();
        return;
    }

    Vec3f pivot;
    f10_mat.multiplyVec(&pivot, &resource->pos);
    Obj57BCB0 lights;
    lights.count = 0;
    if (lit) {
        const Vec3f worldPosition{
                field_4.x + pivot.x,
                field_4.y + pivot.y,
                field_4.z + pivot.z};
        buildDirectionalLights(
                lights, f58_pTrgObj, field_5C, f10_mat, worldPosition);
    }

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
