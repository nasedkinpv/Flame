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
    };

    void dropRegistry();

    uint32_t epoch_ = 0;
    bool haveEpoch_ = false;
    uint64_t implicitResets_ = 0;

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
