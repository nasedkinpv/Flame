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

bool active();
// flametal:MetalShadows active (option on AND the metal bridge is enabled) --
// implemented in CEngineAnimMesh.cpp, next to the option definition.
bool shadowsActive();
void emitCamera();
bool prepareTarget(SceneObject2E *scene, MyScaledSurface *surface, bool lit,
                   InlineTarget *out);
void emitInline(const InlineTarget &target, const DK2MMeshVertex *vertices,
                uint32_t vertexCount, const uint16_t *indices,
                uint32_t indexCount, float ambientR, float ambientG,
                float ambientB);

}  // namespace meshgpu
}  // namespace dk2
