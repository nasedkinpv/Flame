#include "dk2/engine/primitive/2d/world/CEngineAnimMesh.h"

#include "dk2/AnimVertEx.h"
#include "dk2/MeshGpuEmit.h"
#include "patches/logging.h"
#include <metal_bridge/DK2BridgeProtocol.h>
#include <windows.h>
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

#include <tools/flametal_config.h>

#include <cstdint>
#include <cstring>
#include <emmintrin.h>

// Defined in Obj57AD20.cpp, near o_gog_meshGpuPath; gates this file's
// recurring "anim modes:" probe alongside that file's "mesh tex resolve:"
// and "mesh gpu probe:" probes.
extern flametal_config::define_flame_option<bool> o_flametal_debugProbes;

namespace {

uint32_t g_animModePlain, g_animModeBlend, g_animModeOther, g_animGpuHit, g_animGpuMiss;

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

struct AnimTopology {
    uint32_t meshId;
    uint32_t vertexCount;
    uint8_t sourceIndices[256];
};

AnimTopology *retainedAnimTopology(
        dk2::CAnimMeshResource *resource, int animation,
        dk2::SprsAnimHeader &entry, const uint8_t *indices,
        uint32_t triangleCount) {
    struct CacheEntry {
        const void *resource;
        const void *vertices;
        const void *indices;
        int animation;
        uint32_t triangleCount;
        AnimTopology topology;
    };
    static CacheEntry cache[4096] = {};
    uintptr_t hash = reinterpret_cast<uintptr_t>(resource) >> 4;
    hash ^= reinterpret_cast<uintptr_t>(indices) >> 3;
    uint32_t slot = static_cast<uint32_t>(hash) & 4095u;
    CacheEntry *available = nullptr;
    for (uint32_t probe = 0; probe < 32; ++probe, slot = (slot + 1) & 4095u) {
        CacheEntry &candidate = cache[slot];
        if (!candidate.resource) {
            available = &candidate;
            break;
        }
        if (candidate.resource == resource &&
            candidate.vertices == entry.AnimVertEx_base &&
            candidate.indices == indices && candidate.animation == animation &&
            candidate.triangleCount == triangleCount) {
            return &candidate.topology;
        }
    }
    if (!available || !triangleCount || triangleCount > 255) return nullptr;
    static DK2MMeshVertex vertices[256];
    static uint16_t outIndices[765];
    uint16_t mapped[256];
    for (uint16_t &value : mapped) value = 0xFFFFu;
    const float uvScale = *reinterpret_cast<const float *>(0x0066FC74);
    uint32_t vertexCount = 0, indexCount = 0;
    const uint8_t *cursor = indices;
    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle, cursor += 3) {
        for (int corner = 0; corner < 3; ++corner) {
            const uint8_t source = cursor[corner];
            if (mapped[source] == 0xFFFFu) {
                if (vertexCount == 256) return nullptr;
                const dk2::AnimVertEx &av = entry.AnimVertEx_base[source];
                DK2MMeshVertex &vertex = vertices[vertexCount];
                vertex.px = vertex.py = vertex.pz = 0.0f;
                vertex.nx = av.x;
                vertex.ny = av.y;
                vertex.nz = av.z;
                const uint32_t packed = static_cast<uint32_t>(av.uv);
                vertex.u = static_cast<float>(packed & 0xFFFFu) * uvScale;
                vertex.v = static_cast<float>(packed >> 16) * uvScale;
                vertex.base_color = 0xFF000000u;
                mapped[source] = static_cast<uint16_t>(vertexCount);
                available->topology.sourceIndices[vertexCount++] = source;
            }
            outIndices[indexCount++] = mapped[source];
        }
    }
    const uint32_t meshId = dk2::meshgpu::allocateMeshId();
    if (!dk2::meshgpu::registerMesh(
            meshId, vertices, vertexCount, outIndices, indexCount)) {
        return nullptr;
    }
    available->resource = resource;
    available->vertices = entry.AnimVertEx_base;
    available->indices = indices;
    available->animation = animation;
    available->triangleCount = triangleCount;
    available->topology.meshId = meshId;
    available->topology.vertexCount = vertexCount;
    return &available->topology;
}

// Animation interpolation remains game-side, but immutable topology, normals,
// UVs and indices are registered once. Each frame now crosses only float3
// positions plus one instance transform.
bool drawAnimOnGpu(
        dk2::CEngineAnimMesh *mesh,
        dk2::SceneObject2E *scene,
        dk2::MyScaledSurface *surface,
        dk2::CAnimMeshResource *resource,
        int animation,
        float frame,
        uint32_t frameIndex,
        dk2::SprsAnimHeader &entry,
        const uint8_t *indices,
        uint32_t triangleCount,
        const dk2::Vec3f &ambient,
        bool lit,
        float scale) {
    dk2::meshgpu::InlineTarget target;
    if (!dk2::meshgpu::prepareTarget(scene, surface, lit, &target)) return false;
    if (!target.textureId) return false;  // CPU fallback keeps it visible
    AnimTopology *topology = retainedAnimTopology(
        resource, animation, entry, indices, triangleCount);
    if (!topology) return false;
    static float positions[256 * 3];
    for (uint32_t vertex = 0; vertex < topology->vertexCount; ++vertex) {
        dk2::Vec3f model;
        resource->sub_57E5B0(
            animation, frame, frameIndex,
            topology->sourceIndices[vertex], &model);
        positions[vertex * 3 + 0] = model.x;
        positions[vertex * 3 + 1] = model.y;
        positions[vertex * 3 + 2] = model.z;
    }
    dk2::meshgpu::LightSelection lights = {};
    auto *collection = lit
        ? reinterpret_cast<uint32_t *>(static_cast<uintptr_t>(mesh->field_58))
        : nullptr;
    if (!dk2::meshgpu::prepareLights(
            collection, lit ? static_cast<uint32_t>(mesh->field_68) : 0u,
            &lights)) {
        return false;
    }
    float world[12];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            world[row * 4 + column] =
                mesh->f10_matrix.m[column][row] * scale;
        }
    }
    world[3] = mesh->field_4.x;
    world[7] = mesh->field_4.y;
    world[11] = mesh->field_4.z;
    dk2::meshgpu::emitCamera();
    dk2::meshgpu::emitDeformed(
        target, topology->meshId, positions, topology->vertexCount,
        world, lights, ambient.x / 255.0f, ambient.y / 255.0f,
        ambient.z / 255.0f);
    return true;
}

__m128 decodeAnimationPosition(
        uint32_t packed, const dk2::CAnimMeshResource *resource) {
    const int32_t x = static_cast<int32_t>(
            (packed & 0x3FF00000u) - 0x20000000u);
    const int32_t y = static_cast<int32_t>(
            (packed & 0x000FFC00u) - 0x00080000u);
    const int32_t z = static_cast<int32_t>(
            (packed & 0x000003FFu) - 0x00000200u);
    const __m128 coordinates = _mm_cvtepi32_ps(_mm_set_epi32(0, z, y, x));
    const __m128 scale = _mm_set_ps(
            0.0f, resource->scale_mini,
            resource->scale_micro, resource->scale_nano);
    return _mm_mul_ps(coordinates, scale);
}

void storeAnimationPosition(dk2::Vec3f *output, __m128 value) {
    _mm_storel_pi(reinterpret_cast<__m64 *>(output), value);
    _mm_store_ss(&output->z,
                 _mm_shuffle_ps(value, value, _MM_SHUFFLE(2, 2, 2, 2)));
}

}  // namespace


dk2::Vec3f *dk2::CAnimMeshResource::sub_57E5B0(
        int animation,
        float frame,
        uint32_t frameIndex,
        int vertexIndex,
        Vec3f *output) {
    // The original stores a 10:10:10 signed position at two adjacent entries
    // and interpolates between them. Keep the exact integer addressing and
    // one-rounding-per-operation float order, but decode all three axes in one
    // SSE2 lane group instead of six x87 fild/fmul sequences.
    const SprsAnimHeader &entry = buf[animation];
    const uint32_t vertexKey = static_cast<uint16_t>(
            entry.AnimVertEx_base[vertexIndex].index);
    const uint32_t frameBlock = frameIndex >> 7;
    const uint32_t tableIndex = indexCount * frameBlock + vertexKey;
    const uint8_t frameOffset = static_cast<const uint8_t *>(
            static_cast<const void *>(triangles_base + frameCount * vertexKey))[
                    frameIndex];
    const uint32_t coordinateIndex =
            static_cast<const uint32_t *>(
                    static_cast<const void *>(itab_base))[tableIndex] +
            frameOffset;

    const __m128 first = decodeAnimationPosition(
            static_cast<uint32_t>(geomCoordinates_base[coordinateIndex]), this);
    const __m128 second = decodeAnimationPosition(
            static_cast<uint32_t>(geomCoordinates_base[coordinateIndex + 1]), this);

    const uint32_t blockStart = frameBlock << 7;
    const uint32_t firstFrame = blockStart +
            static_cast<uint8_t>(geomFrame_base[coordinateIndex]);
    const uint32_t frameSpan =
            blockStart + static_cast<uint8_t>(geomFrame_base[coordinateIndex + 1]) -
            firstFrame;
    const float reciprocal =
            reinterpret_cast<const float *>(0x007810A0)[frameSpan];
    const float factor = (frame - static_cast<float>(firstFrame)) * reciprocal;
    const __m128 result = _mm_add_ps(
            first,
            _mm_mul_ps(_mm_sub_ps(second, first), _mm_set1_ps(factor)));
    storeAnimationPosition(output, result);
    return output;
}


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

    if (dk2::meshgpu::active()) {
        if (drawAnimOnGpu(this, scene, surface, resource, animation, frame,
                          frameIndex, entry, indices, triangleCount, ambient,
                          (scene->drawFlags_x2[0] & 0x40) != 0,
                          *reinterpret_cast<float *>(&field_38))) {
            ++g_animGpuHit;
            f50_pMeshHolder->markUsed();
            return;
        }
        ++g_animGpuMiss;
    }

    // Only the legacy CPU vertex path needs pivot-relative directional light
    // vectors. The retained GPU path selects the same source lights directly.
    Vec3f pivot;
    f10_matrix.multiplyVec(&pivot, &resource->pos);
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
        ++g_animModeOther;
        using OriginalFun = void (__thiscall *)(CEngineAnimMesh *, int, SceneObject2E *);
        reinterpret_cast<OriginalFun>(0x00583CA0)(this, mode - 2000, scene);
    } else if (mode >= 1000) {
        ++g_animModeBlend;
        sub_583DC0(mode - 1000, scene);
    } else {
        ++g_animModePlain;
        sub_5836A0(mode, scene);
    }
    static DWORD lastTick = 0;
    const DWORD now = GetTickCount();
    if (now - lastTick > 3000) {
        lastTick = now;
        if (o_flametal_debugProbes.get()) {
            patch::log::dbg("anim modes: plain=%u blend=%u other=%u gpuHit=%u gpuMiss=%u",
                            g_animModePlain, g_animModeBlend, g_animModeOther,
                            g_animGpuHit, g_animGpuMiss);
        }
    }
    // Cursor mini-scene probe: the Hand of Evil is the anim resource "hand"
    // rasterized into cursorSurf through some scene other than the main one.
    // Log each unique scene pointer once so the cursor scene (tiny draw
    // count, appears when the cursor changes shape) can be identified for
    // the host-rendered-cursor plan.
    if (o_flametal_debugProbes.get()) {
        static void *seenScenes[16] = {};
        static uint32_t seenCounts[16] = {};
        for (int i = 0; i < 16; ++i) {
            if (seenScenes[i] == scene) { ++seenCounts[i]; break; }
            if (!seenScenes[i]) {
                seenScenes[i] = scene;
                seenCounts[i] = 1;
                patch::log::dbg("anim scene probe: new scene=%p mode=%d this=%p",
                                (void *) scene, mode, (void *) this);
                break;
            }
        }
    }
}
