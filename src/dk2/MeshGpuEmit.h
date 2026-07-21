#pragma once

#include <cstdint>

struct DK2MMeshVertex;

namespace dk2 {
struct SceneObject2E;
struct MyScaledSurface;

// Cross-emitter access to the Metal mesh pipeline plumbing that lives in
// Obj57AD20.cpp (texture resolve, UV stage tables, material-flag translation).
namespace meshgpu {

struct InlineTarget {
    uint32_t textureId;
    float uS, vS, uO, vO;  // stage-slot UV scale/offset tables
    uint32_t meshFlags;    // DK2MDrawMeshFlags
    uint32_t tint;
};

struct LightSelection {
    uint16_t indices[24];
    uint16_t count;
};

bool active();
void emitCamera();
uint32_t allocateMeshId();
bool registerMesh(uint32_t meshId, const DK2MMeshVertex *vertices,
                  uint32_t vertexCount, const uint16_t *indices,
                  uint32_t indexCount);
bool prepareLights(uint32_t *collection, uint32_t mask, LightSelection *out);
bool prepareTarget(SceneObject2E *scene, MyScaledSurface *surface, bool lit,
                   InlineTarget *out);
void emitRetained(const InlineTarget &target, uint32_t meshId,
                  const float world[12], const LightSelection &lights,
                  float ambientR, float ambientG, float ambientB);
void emitDeformed(const InlineTarget &target, uint32_t meshId,
                  const float *positions, uint32_t vertexCount,
                  const float world[12], const LightSelection &lights,
                  float ambientR, float ambientG, float ambientB);
void emitInline(const InlineTarget &target, const DK2MMeshVertex *vertices,
                uint32_t vertexCount, const uint16_t *indices,
                uint32_t indexCount, const LightSelection &lights,
                float ambientR, float ambientG, float ambientB);

}  // namespace meshgpu
}  // namespace dk2
