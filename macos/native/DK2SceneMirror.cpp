#include "DK2SceneMirror.h"

#include <cstdio>

namespace dk2 {

void SceneMirror::dropRegistry() {
    objects_.clear();
}

void SceneMirror::applyReset(const DK2MSceneResetCommand& reset) {
    // An explicit epoch bump (level-load / save-load / new game). Drop the whole
    // registry -- object ids are only stable within a generation.
    dropRegistry();
    epoch_ = reset.scene_epoch;
    haveEpoch_ = true;
}

void SceneMirror::applyRegister(const DK2MSceneRegisterCommand& reg) {
    // Defensive drop-on-epoch-change: if a registration arrives stamped with an
    // epoch we have not seen a SCENE_RESET for, treat it as an implicit reset.
    // The guest bumps its local epoch unconditionally but can silently drop the
    // SCENE_RESET command when the frame buffer is full (see the plan's review
    // note); self-healing here keeps the mirror from mixing two generations.
    if (!haveEpoch_ || reg.scene_epoch != epoch_) {
        if (haveEpoch_ && reg.scene_epoch != epoch_) {
            ++implicitResets_;
        }
        dropRegistry();
        epoch_ = reg.scene_epoch;
        haveEpoch_ = true;
    }

    Entry& entry = objects_[reg.object_id];
    entry.meshId = reg.mesh_id;
    entry.signature = reg.signature;
    entry.vertexCount = reg.vertex_count;
    entry.materialFlags = reg.material_flags;

    registeredObjectsFrame_.insert(reg.object_id);
    if (reg.mesh_id) {
        registeredMeshesFrame_.insert(reg.mesh_id);
    }
}

void SceneMirror::noteDrawnMesh(uint32_t meshId) {
    if (meshId) {
        drawnMeshesFrame_.insert(meshId);
    }
}

void SceneMirror::endFrame(uint64_t frameNumber) {
    // Primary signal: of the meshes we registered this frame, how many were
    // actually drawn this frame? SCENE_REGISTER is emitted from the same
    // retained draw path as DRAW_MESH_DEFORMED, so a healthy mirror sits at
    // ~100% here; a shortfall means we are registering objects whose mesh the
    // guest never drew (stale / bogus registration).
    size_t registeredMeshes = registeredMeshesFrame_.size();
    size_t matchedMeshes = 0;
    for (uint32_t meshId : registeredMeshesFrame_) {
        if (drawnMeshesFrame_.count(meshId)) {
            ++matchedMeshes;
        }
    }

    // Secondary signal: frame-over-frame churn of the registered object set.
    // For a static scene this should be ~0 once the level is warm.
    size_t churn = 0;
    if (havePrevFrame_) {
        for (uint32_t objectId : registeredObjectsFrame_) {
            if (!prevRegisteredObjects_.count(objectId)) ++churn;
        }
        for (uint32_t objectId : prevRegisteredObjects_) {
            if (!registeredObjectsFrame_.count(objectId)) ++churn;
        }
    }

    // Log-only, throttled to keep game.log readable. Only emit when the guest
    // is actually registering (flag on); silent otherwise, so production with
    // native_scene_mirror=false prints nothing.
    if (registeredMeshes && (frameNumber % 60) == 0) {
        double matchPct = registeredMeshes
            ? (100.0 * static_cast<double>(matchedMeshes) /
               static_cast<double>(registeredMeshes))
            : 0.0;
        std::fprintf(stderr,
                     "DK2 scene mirror: epoch=%u registry=%zu objects "
                     "(frame reg=%zu meshes, drawn=%zu meshes, match=%zu/%zu "
                     "%.1f%%) churn=%zu implicitResets=%llu\n",
                     epoch_, objects_.size(),
                     registeredMeshes, drawnMeshesFrame_.size(),
                     matchedMeshes, registeredMeshes, matchPct, churn,
                     static_cast<unsigned long long>(implicitResets_));
    }

    prevRegisteredObjects_ = std::move(registeredObjectsFrame_);
    havePrevFrame_ = true;
    registeredObjectsFrame_.clear();
    registeredMeshesFrame_.clear();
    drawnMeshesFrame_.clear();
}

}  // namespace dk2
