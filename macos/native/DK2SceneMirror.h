#ifndef DK2_SCENE_MIRROR_H
#define DK2_SCENE_MIRROR_H

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "metal_bridge/DK2BridgeProtocol.h"

namespace dk2 {

// Host-side native scene mirror (Phase 1, LOG-ONLY / observational).
//
// Consumes DK2M_COMMAND_SCENE_REGISTER into an object_id -> entry registry
// scoped to the current SCENE_EPOCH, and cross-checks that registry against the
// meshes the guest actually drew this frame. It CONSUMES NOTHING for rendering:
// no culling, no draw-skip. See .ralph/native-scene-mirror-phase1.md ("Hard
// rules"). All output is diagnostic.
//
// Correlation note: the bridge draw commands (DRAW_MESH / DRAW_MESH_DEFORMED)
// carry mesh_id but NOT object_id, so there is no per-frame drawn-*object*-id
// signal in the protocol. The mirror therefore correlates on the shared
// mesh_id key (registered-this-frame meshes vs drawn-this-frame meshes) as the
// primary match/mismatch signal, and additionally reports frame-over-frame
// churn of the registered object set. Both are honest without inventing a new
// protocol addition.
class SceneMirror {
public:
    // Consume one SCENE_RESET: drop the registry, adopt the new epoch.
    void applyReset(const DK2MSceneResetCommand& reset);

    // Consume one SCENE_REGISTER: on an unannounced epoch jump, self-heal with
    // an implicit reset (a dropped SCENE_RESET must not desync the mirror --
    // see the plan's review note), then upsert object_id -> entry.
    void applyRegister(const DK2MSceneRegisterCommand& reg);

    // Record a mesh id the guest actually drew this frame (from a DRAW_MESH /
    // DRAW_MESH_DEFORMED command in the same frame stream). Unordered relative
    // to registrations -- treat the whole frame as one unordered set.
    void noteDrawnMesh(uint32_t meshId);

    // Consume the frame's CMD_CAMERA_SET cull inputs (Phase 2): the four
    // camera-space frustum-side plane normals + the world->camera transform, so
    // endFrame() can recompute each mirrored object's cull independently of the
    // guest. LOG-ONLY. Copies the fields verbatim from the bridge command.
    void setCullCamera(const DK2MCameraSetCommand& camera);

    // Close out the frame: compute and (periodically) log the match/mismatch
    // signal, then clear per-frame accumulators. Call exactly once per host
    // frame after all commands for the frame have been fed in.
    void endFrame(uint64_t frameNumber);

private:
    struct Entry {
        uint32_t meshId = 0;
        uint64_t signature = 0;
        uint32_t vertexCount = 0;
        uint32_t materialFlags = 0;
        float center[3] = {0.0f, 0.0f, 0.0f};  // world-space bounding-sphere centre
        float radius = 0.0f;
        uint32_t guestCull = 0;  // guest's own verdict: bit0 visible, bit1 fullyInside
    };

    void dropRegistry();

    uint32_t epoch_ = 0;
    bool haveEpoch_ = false;
    uint64_t implicitResets_ = 0;

    // Per-frame cull camera (from CMD_CAMERA_SET). haveCullCamera_ stays false
    // until the first camera arrives, so we never cull against a zero transform.
    bool haveCullCamera_ = false;
    float cullPlane_[4][3] = {};  // camera-space frustum-side plane normals A..D
    float cullCamPos_[3] = {};    // g_camState.v3f (camera world position)
    float cullCamRot_[9] = {};    // g_camState.m row-major 3x3

    // Cumulative host-vs-guest cull agreement counters (LOG-ONLY). "Guest
    // visible" is implicit: an object is only registered when the guest drew it,
    // so a host "culled" verdict on a registered object is a disagreement.
    uint64_t hostCullChecks_ = 0;      // registered objects the host cull-tested
    uint64_t hostCullVisible_ = 0;     // host verdict = visible
    uint64_t hostCullCulled_ = 0;      // host verdict = culled

    // Phase 2 step 5: host verdict vs the guest's stamped verdict (both the full
    // 2-bit result). exactMatch = identical visible+fullyInside bits.
    uint64_t cullExactMatch_ = 0;
    uint64_t cullMismatch_ = 0;

    // The mirror registry: every static object registered in the current epoch.
    std::unordered_map<uint32_t, Entry> objects_;

    // Per-frame accumulators (cleared each endFrame).
    std::unordered_set<uint32_t> registeredObjectsFrame_;
    std::unordered_set<uint32_t> registeredMeshesFrame_;
    std::unordered_set<uint32_t> drawnMeshesFrame_;

    // Previous frame's registered object-id set, for churn measurement.
    std::unordered_set<uint32_t> prevRegisteredObjects_;
    bool havePrevFrame_ = false;
};

}  // namespace dk2

#endif  // DK2_SCENE_MIRROR_H
