#include "DK2SceneMirror.h"

#include "dk2_core/dk2_cull.h"

#include <cstdio>
#include <cstring>

namespace dk2 {

namespace {

// Reproduce the guest's world->camera transform bit-for-bit (Phase 2). The
// guest (CEngineStaticMeshAdd) forms rel = center - camPos (Vec3f SSE subtract)
// then camSpace = Mat3x3f::sub_594E10(rel) = combineRows(m, rel): per output
// lane i, camSpace[i] = ((m[0][i]*rel.x) + (m[1][i]*rel.y)) + (m[2][i]*rel.z).
// rot is row-major, rot[r][c] = camRot[r*3 + c]. Plain scalar ops, and the host
// TU is built with -ffp-contract=off, so this matches the guest's SSE result.
dk2::core::CullVec3 worldToCamera(const float center[3], const float camPos[3],
                                  const float camRot[9]) {
    const float rx = center[0] - camPos[0];
    const float ry = center[1] - camPos[1];
    const float rz = center[2] - camPos[2];
    dk2::core::CullVec3 out;
    out.x = ((camRot[0] * rx) + (camRot[3] * ry)) + (camRot[6] * rz);  // col 0
    out.y = ((camRot[1] * rx) + (camRot[4] * ry)) + (camRot[7] * rz);  // col 1
    out.z = ((camRot[2] * rx) + (camRot[5] * ry)) + (camRot[8] * rz);  // col 2
    return out;
}

}  // namespace

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
    entry.center[0] = reg.center[0];
    entry.center[1] = reg.center[1];
    entry.center[2] = reg.center[2];
    entry.radius = reg.radius;
    entry.guestCull = reg.guest_cull;

    // Phase 3: the guest sets DK2M_GUEST_CULL_CONSUME on every SCENE_REGISTER
    // when launched with native_scene_mirror_consume. Accumulate per-frame;
    // endFrame commits it into consumeRequested_ so the render pass -- which
    // runs right after endFrame -- sees this frame's authorisation. Per-frame
    // (not a permanent latch) so a config rollback takes effect the very next
    // frame without needing a host restart.
    if (reg.guest_cull & DK2M_GUEST_CULL_CONSUME) {
        consumeRequestedFrame_ = true;
    }

    if (reg.mesh_id) {
        // Draw-path registration: the guest resolved a mesh and drew this object.
        registeredObjectsFrame_.insert(reg.object_id);
        registeredMeshesFrame_.insert(reg.mesh_id);
    } else {
        // Reject-side registration (Phase 2): mesh_id==0 marks an object the
        // guest CULLED before the draw path -- it carries only minimal identity
        // (object_id + world bounds + guest verdict). Keep it OUT of the drawn-
        // mesh and churn sets (those describe visible geometry); endFrame still
        // independently cull-tests it below to verify the host also culls it.
        culledObjectsFrame_.insert(reg.object_id);
    }
}

void SceneMirror::noteDrawnMesh(uint32_t meshId) {
    if (meshId) {
        drawnMeshesFrame_.insert(meshId);
    }
}

void SceneMirror::setCullCamera(const DK2MCameraSetCommand& camera) {
    std::memcpy(cullPlane_, camera.cull_plane, sizeof(cullPlane_));
    std::memcpy(cullCamPos_, camera.cull_cam_pos, sizeof(cullCamPos_));
    std::memcpy(cullCamRot_, camera.cull_cam_rot, sizeof(cullCamRot_));
    // A genuinely-uninitialised camera (all-zero rotation) can never cull
    // sensibly; the emitter always fills these, but guard anyway.
    const bool anyRot = cullCamRot_[0] || cullCamRot_[4] || cullCamRot_[8];
    haveCullCamera_ = anyRot;
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

    // Phase 2: independently recompute the frustum-sphere cull for every object
    // the guest registered (== drew) this frame, using the ported dk2_core math
    // + the frame's threaded camera state. A registered object is one the guest
    // decided was visible, so a host "culled" verdict is a disagreement worth
    // surfacing. LOG-ONLY: this result is never consumed (no draw is skipped).
    size_t frameChecks = 0, frameVisible = 0, frameCulled = 0;
    size_t frameMatch = 0, frameMismatch = 0;
    // Reject-side (guest-CULLED objects) breakdown for this frame.
    size_t frameRejChecks = 0, frameRejMatch = 0, frameRejMismatch = 0;
    if (haveCullCamera_) {
        const dk2::core::CullVec3 A{cullPlane_[0][0], cullPlane_[0][1], cullPlane_[0][2]};
        const dk2::core::CullVec3 B{cullPlane_[1][0], cullPlane_[1][1], cullPlane_[1][2]};
        const dk2::core::CullVec3 C{cullPlane_[2][0], cullPlane_[2][1], cullPlane_[2][2]};
        const dk2::core::CullVec3 D{cullPlane_[3][0], cullPlane_[3][1], cullPlane_[3][2]};
        // Independently recompute the frustum-sphere cull for one registered
        // object and diff the host's full 2-bit verdict against the guest's
        // stamped one. Given identical inputs (world bounds + same-frame camera
        // globals), the difftested dk2_core core makes this an in-game proof that
        // the host's camera reconstruction matches the guest's; any mismatch is a
        // real divergence to investigate (blocker per the plan's hard rule).
        // rejectSide picks out the guest-CULLED direction for the log breakdown.
        auto testObject = [&](uint32_t objectId, bool rejectSide) {
            auto found = objects_.find(objectId);
            if (found == objects_.end()) return;
            const Entry& e = found->second;
            const dk2::core::CullVec3 camSpace =
                worldToCamera(e.center, cullCamPos_, cullCamRot_);
            uint32_t fullyInside = 0;
            const int visible = dk2::core::cullSphere575D70(
                camSpace, e.radius, &fullyInside, A, B, C, D);
            // Phase 3: stamp the host's own verdict onto the registry entry so
            // the render pass can query it (hostWouldCull) for this exact frame.
            // Consumption only ever skips a DRAWN object (mesh_id != 0); the
            // reject-side entries are never drawn, so this is harmless there.
            found->second.hostCulled = (visible == 0);
            found->second.hostCullFrame = frameNumber;
            ++frameChecks;
            if (visible) ++frameVisible; else ++frameCulled;
            const uint32_t hostVerdict =
                (visible ? 1u : 0u) | (fullyInside ? 2u : 0u);
            const bool match = (hostVerdict == (e.guestCull & 3u));
            if (match) ++frameMatch; else ++frameMismatch;
            if (rejectSide) {
                ++frameRejChecks;
                if (match) ++frameRejMatch; else ++frameRejMismatch;
            }
        };
        // Visible direction: objects the guest drew (a host "culled" here is a
        // disagreement). Reject direction: objects the guest CULLED (a host
        // "visible" here is a disagreement) -- the coverage the draw path could
        // never provide, since a culled object never reached sceneRegister.
        for (uint32_t objectId : registeredObjectsFrame_) testObject(objectId, false);
        for (uint32_t objectId : culledObjectsFrame_) testObject(objectId, true);
        hostCullChecks_ += frameChecks;
        hostCullVisible_ += frameVisible;
        hostCullCulled_ += frameCulled;
        cullExactMatch_ += frameMatch;
        cullMismatch_ += frameMismatch;
        rejectChecks_ += frameRejChecks;
        rejectMatch_ += frameRejMatch;
        rejectMismatch_ += frameRejMismatch;
    }

    // Log-only, throttled to keep game.log readable. Only emit when the guest
    // is actually registering (flag on); silent otherwise, so production with
    // native_scene_mirror=false prints nothing.
    if ((registeredMeshes || !culledObjectsFrame_.empty()) &&
        (frameNumber % 60) == 0) {
        double matchPct = registeredMeshes
            ? (100.0 * static_cast<double>(matchedMeshes) /
               static_cast<double>(registeredMeshes))
            : 0.0;
        const double verdictMatchPct = hostCullChecks_
            ? (100.0 * static_cast<double>(cullExactMatch_) /
               static_cast<double>(hostCullChecks_))
            : 0.0;
        const double rejectMatchPct = rejectChecks_
            ? (100.0 * static_cast<double>(rejectMatch_) /
               static_cast<double>(rejectChecks_))
            : 0.0;
        std::fprintf(stderr,
                     "DK2 scene mirror: epoch=%u registry=%zu objects "
                     "(frame reg=%zu meshes, drawn=%zu meshes, match=%zu/%zu "
                     "%.1f%%) churn=%zu implicitResets=%llu | hostCull frame "
                     "checks=%zu vis=%zu culled=%zu match=%zu mismatch=%zu; "
                     "cumulative host-vs-guest verdict match=%.3f%% "
                     "(%llu/%llu, mismatch=%llu) | reject-side (guest-culled) "
                     "frame checks=%zu match=%zu mismatch=%zu; cumulative "
                     "reject match=%.3f%% (%llu/%llu, mismatch=%llu)\n",
                     epoch_, objects_.size(),
                     registeredMeshes, drawnMeshesFrame_.size(),
                     matchedMeshes, registeredMeshes, matchPct, churn,
                     static_cast<unsigned long long>(implicitResets_),
                     frameChecks, frameVisible, frameCulled, frameMatch,
                     frameMismatch, verdictMatchPct,
                     static_cast<unsigned long long>(cullExactMatch_),
                     static_cast<unsigned long long>(hostCullChecks_),
                     static_cast<unsigned long long>(cullMismatch_),
                     frameRejChecks, frameRejMatch, frameRejMismatch,
                     rejectMatchPct,
                     static_cast<unsigned long long>(rejectMatch_),
                     static_cast<unsigned long long>(rejectChecks_),
                     static_cast<unsigned long long>(rejectMismatch_));
    }

    // Phase 3 consume summary (only when the guest authorised consumption this
    // frame). A skip only happens on a genuine host-vs-guest cull disagreement,
    // so in a healthy state this reports 0 skips and rendering is unchanged.
    if (consumeRequestedFrame_ && (frameNumber % 60) == 0) {
        std::fprintf(stderr,
                     "DK2 scene mirror CONSUME (Phase 3): ACTIVE -- cumulative "
                     "draws SKIPPED=%llu of %llu consume-checked deformed draws "
                     "(a skip == host cull disagrees with guest; 0 skips == host "
                     "agrees, rendering byte-identical)\n",
                     static_cast<unsigned long long>(consumeSkips_),
                     static_cast<unsigned long long>(consumeChecks_));
    }

    // Commit this frame's consume authorisation for the render pass (which runs
    // immediately after endFrame), then reset the per-frame accumulator.
    consumeRequested_ = consumeRequestedFrame_;
    consumeRequestedFrame_ = false;

    prevRegisteredObjects_ = std::move(registeredObjectsFrame_);
    havePrevFrame_ = true;
    registeredObjectsFrame_.clear();
    registeredMeshesFrame_.clear();
    drawnMeshesFrame_.clear();
    culledObjectsFrame_.clear();
}

bool SceneMirror::hostWouldCull(uint32_t objectId, uint64_t frameNumber) const {
    auto it = objects_.find(objectId);
    if (it == objects_.end()) return false;
    // Only honour a verdict computed by endFrame for exactly this frame -- a
    // stale verdict from a prior frame must never drive a skip.
    return it->second.hostCullFrame == frameNumber && it->second.hostCulled;
}

void SceneMirror::noteConsumeDecision(bool skipped) {
    ++consumeChecks_;
    if (skipped) ++consumeSkips_;
}

}  // namespace dk2
