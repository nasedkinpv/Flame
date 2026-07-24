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

    // --- Phase 3 (REAL CONSUMPTION) query surface, used by the render pass ---
    //
    // consumeActive(): true only when the guest was launched with
    // native_scene_mirror_consume (it stamps DK2M_GUEST_CULL_CONSUME on every
    // SCENE_REGISTER). When false, every method below is inert and the render
    // pass must behave byte-identically to Phase 2 (log-only).
    bool consumeActive() const { return consumeRequested_; }

    // Should the host SKIP the draw of this object this frame? True only when
    // the object is registered, its host cull verdict was computed for exactly
    // `frameNumber` (endFrame ran this frame), and that verdict is "culled".
    // The host verdict is currently "visible" for every registered object in a
    // healthy state, so this returns false for everything and nothing is
    // skipped -- a skip means a genuine host-vs-guest cull disagreement.
    bool hostWouldCull(uint32_t objectId, uint64_t frameNumber) const;

    // Render-pass accounting for the consume path (folded into endFrame's log).
    void noteConsumeDecision(bool skipped);

private:
    struct Entry {
        uint32_t meshId = 0;
        uint64_t signature = 0;
        uint32_t vertexCount = 0;
        uint32_t materialFlags = 0;
        float center[3] = {0.0f, 0.0f, 0.0f};  // world-space bounding-sphere centre
        float radius = 0.0f;
        uint32_t guestCull = 0;  // guest's own verdict: bit0 visible, bit1 fullyInside
        // Phase 3: the host's own recomputed verdict, stamped each endFrame.
        bool hostCulled = false;       // host verdict this frame = "cull it"
        uint64_t hostCullFrame = 0;    // frame endFrame last computed hostCulled for
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
    // 2-bit result). exactMatch = identical visible+fullyInside bits. These are
    // the SINGLE cumulative counters over BOTH diff directions (visible objects
    // the guest drew, AND -- Phase 2 reject-side -- objects the guest CULLED).
    uint64_t cullExactMatch_ = 0;
    uint64_t cullMismatch_ = 0;

    // Reject-side breakdown (Phase 2 coverage gap fix). A SUBSET of the cumulative
    // counters above, not a parallel system: these isolate the guest-CULLED
    // objects (registered with mesh_id==0) so the log can report the reject-side
    // match rate on its own. Before this, culled objects never reached the mirror
    // at all, so the reject direction was structurally unverifiable.
    uint64_t rejectChecks_ = 0;
    uint64_t rejectMatch_ = 0;
    uint64_t rejectMismatch_ = 0;

    // Phase 3 (REAL CONSUMPTION). consumeRequestedFrame_ accumulates the guest's
    // DK2M_GUEST_CULL_CONSUME signal during a frame's registrations; endFrame
    // commits it into consumeRequested_ (read by the render pass). consumeChecks_
    // / consumeSkips_ are cumulative render-pass accounting for the log.
    bool consumeRequested_ = false;
    bool consumeRequestedFrame_ = false;
    uint64_t consumeChecks_ = 0;   // drawn objects the render pass consume-tested
    uint64_t consumeSkips_ = 0;    // draws the host actually skipped (disagreements)

    // The mirror registry: every static object registered in the current epoch.
    std::unordered_map<uint32_t, Entry> objects_;

    // Per-frame accumulators (cleared each endFrame).
    std::unordered_set<uint32_t> registeredObjectsFrame_;
    std::unordered_set<uint32_t> registeredMeshesFrame_;
    std::unordered_set<uint32_t> drawnMeshesFrame_;
    // Reject-side: objects the guest CULLED this frame (registered with
    // mesh_id==0). Kept separate from registeredObjectsFrame_ so churn / drawn-
    // mesh metrics stay about VISIBLE geometry, while endFrame still cull-tests
    // these for the reject-side diff.
    std::unordered_set<uint32_t> culledObjectsFrame_;

    // Previous frame's registered object-id set, for churn measurement.
    std::unordered_set<uint32_t> prevRegisteredObjects_;
    bool havePrevFrame_ = false;
};

}  // namespace dk2

#endif  // DK2_SCENE_MIRROR_H
