#include "dk2/Obj57AD20.h"

#include "dk2/MeshGpuEmit.h"

#include "dk2/CEngineDDSurface.h"
#include "dk2/CEngineSurfaceBase.h"
#include "dk2/SurfaceHolder.h"
#include "dk2/MyCESurfHandle.h"
#include "dk2/MyCESurfScale.h"
#include "dk2/MyScaledSurface.h"
#include "dk2/Obj57BCB0.h"
#include "dk2/Obj58EF60.h"
#include "dk2/SceneObject2E.h"
#include "dk2/Uv2f.h"
#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"
#include "patches/logging.h"
#include <fake/FakeTexture.h>
#include <metal_bridge/DK2BridgeProtocol.h>
#include <metal_bridge/MetalBridgeProducer.h>
#include <cstdlib>
#include <cstring>
#include <tools/flametal_config.h>
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <emmintrin.h>
#include <unordered_map>
#include <utility>
#include <vector>

// Reroutes the translated deformed-mesh emitter (sub_57B6D0) to the Metal
// bridge's world-space mesh pipeline: the GPU does projection and per-vertex
// point-light accumulation instead of the original per-vertex CPU loop.
flametal_config::define_flame_option<bool> o_gog_meshGpuPath(
    "gog:MeshGpuPath", flametal_config::OG_Config,
    "Emit dynamic meshes through the Metal world-space pipeline (GPU transform + lighting)",
    false
);

// Gates the recurring "mesh tex resolve:", "mesh gpu probe:" (this file) and
// "anim modes:" (CEngineAnimMesh.cpp) debug probes. These fire every ~3s
// while the GPU mesh path is active and were only ever meant for the
// missing-mesh investigation; default off so normal play stays quiet.
flametal_config::define_flame_option<bool> o_flametal_debugProbes(
    "flametal:DebugProbes", flametal_config::OG_Config,
    "Log recurring mesh-resolve/gpu-probe/anim-mode debug stats every ~3s",
    false
);

namespace dk2 {
extern bool g_inMainScenePass;
}

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

#pragma pack(pop)

static_assert(sizeof(MeshVertex) == 0x28);
static_assert(offsetof(MeshVertex, position) == 0x00);
static_assert(offsetof(MeshVertex, color) == 0x1C);
static_assert(sizeof(MeshEntry) == 0x14);

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

// --- GPU mesh path helpers (introduced in v9; retained/deformed v13) ---

struct SceneLightForGpu {   // mirrors Obj57BCB0.cpp's SceneLight view
    uint32_t unused;
    uint32_t flags;
    dk2::Vec3f position;
    dk2::Vec3f color;
    float queryRadius;
    float distanceSquaredLimit;
    float attenuationScale;
    uint8_t padding[8];
    float facingScale;
};

bool meshGpuActive() {
    // DK2 also renders cursor/portrait mini-scenes with different global
    // camera matrices. The bridge currently has one mesh camera per frame,
    // so those draws must stay on the legacy CPU path and must never win the
    // frame camera dedup race ahead of the main world pass.
    return *o_gog_meshGpuPath && gog::metal_bridge::isEnabled() &&
           dk2::g_inMainScenePass;
}

// SceneObject2E carries the final flags consumed by DirectDraw_prepareTexture;
// MyScaledSurface::flags are only the source material bits. A property may own
// more than one texture stage, so map the handle slot through its sampler count.
uint32_t meshDrawFlags(dk2::SceneObject2E *scene,
                       dk2::MyScaledSurface *surface, int stageSlot) {
    if (scene) {
        if (scene->propsCount == 1) return scene->drawFlags_x2[0];
        int firstStage = 0;
        for (int property = 0; property < scene->propsCount && property < 2;
             ++property) {
            const int stageCount = static_cast<uint8_t>(
                scene->numTextureSamplers_x2[property]);
            if (stageSlot >= firstStage && stageSlot < firstStage + stageCount) {
                return scene->drawFlags_x2[property];
            }
            firstStage += stageCount;
        }
    }
    return surface ? surface->drawFlags : 0u;
}

uint32_t metalMeshFlags(uint32_t drawFlags, bool lit) {
    // wip: bring-up instrumentation for the selection-highlight/trap-marker
    // culling/Z-sort investigation (removed once root-caused) - log every
    // distinct (drawFlags, real ZFUNC, real CULLMODE) combo seen, READ-ONLY
    // (does not affect drawn output), so a live A/B (creature selected vs
    // not, trap markup shown vs not) reveals whether ZFUNC/CULLMODE change
    // for the SAME drawFlags value - which would prove the render-state
    // decorrelation hypothesis rather than a drawFlags-derived guess.
    {
        DWORD realZFunc = 0, realCull = 0;
        gog::metal_bridge::getRenderState(D3DRENDERSTATE_ZFUNC, &realZFunc);
        gog::metal_bridge::getRenderState(D3DRENDERSTATE_CULLMODE, &realCull);
        static std::unordered_map<uint64_t, uint32_t> seen;
        const uint64_t key = (uint64_t(drawFlags) << 32) |
                             (uint64_t(realZFunc) << 8) | uint64_t(realCull);
        auto &count = seen[key];
        if (count < 3) {
            patch::log::dbg(
                "metalMeshFlags: drawFlags=0x%08X realZFunc=%u realCull=%u "
                "(seen=%u)",
                drawFlags, static_cast<unsigned>(realZFunc),
                static_cast<unsigned>(realCull), count);
        }
        ++count;
    }
    uint32_t flags = lit ? DK2M_DRAW_MESH_LIT : 0u;
    if (drawFlags & 0x100u) flags |= DK2M_DRAW_MESH_Z_ENABLE;
    if (drawFlags & 0x80u) flags |= DK2M_DRAW_MESH_Z_WRITE;
    // This is the order in DirectDraw_prepareTexture: a later matching mode
    // wins if malformed flags contain more than one blend selector.
    if (drawFlags & 0x1000u) flags |= DK2M_DRAW_MESH_MULTIPLY;
    else if (drawFlags & 0x20u) flags |= DK2M_DRAW_MESH_ALPHA_BLEND;
    else if (drawFlags & 0x1u) flags |= DK2M_DRAW_MESH_ADDITIVE;

    // wip: bring-up instrumentation (menu light-ray flicker investigation,
    // removed once root-caused) - log EVERY additive-blend draw, continuous
    // (not throttled to first-N-distinct), since additive draws should be
    // rare in a menu scene - isolates the ray effect from the noise of every
    // other lit draw.
    if (flags & DK2M_DRAW_MESH_ADDITIVE) {
        static uint32_t additiveCalls = 0;
        patch::log::dbg(
            "metalMeshFlags ADDITIVE: drawFlags=0x%08X lit=%d frame=%u "
            "(call=%u)",
            drawFlags, lit, gog::metal_bridge::frameCounter(), additiveCalls);
        ++additiveCalls;
    }
    return flags;
}

// The colour floats the engine carries per vertex/ambient are stored in the
// float-bias encoding (value = bias1 + bias2 + n); the CPU path recovers n
// via mantissa extraction in colourComponent. Recover it explicitly here.
float debiasColour(float value) {
    const float firstBias = *reinterpret_cast<const float *>(0x0066FE78);
    const float secondBias = *reinterpret_cast<const float *>(0x0066FE8C);
    return value - firstBias - secondBias;
}

uint32_t packBaseColor(const dk2::Vec3f &colour) {
    // Per-vertex colours are plain 0..255 floats - the float-bias encoding
    // lives in the ambient globals only (the CPU path subtracts the bias once
    // from the accumulated sum). Debiasing both painted GPU meshes black.
    auto clampByte = [](float v) -> uint32_t {
        return v <= 0.0f ? 0u : (v >= 255.0f ? 255u : static_cast<uint32_t>(v));
    };
    // alpha must be opaque: the shader multiplies texture alpha by this, and
    // a zero here zeroed every mesh draw's alpha (ghost lattices, dead cutouts)
    return 0xFF000000u | (clampByte(colour.x) << 16) | (clampByte(colour.y) << 8) | clampByte(colour.z);
}

// viewProj = P * [M|T] assembled from the canonical camera globals.
// g_mat_77F3A8/g_vec_77F4C0 are the per-object model-view scratch written by
// __renderFun_setSceneObject2E; reading those here made whichever object was
// first in the frame become the camera for every GPU mesh draw.
void emitMeshCamera() {
    // thousands of entries per frame share one camera - build it once
    static uint32_t lastFrame = 0xFFFFFFFFu;
    const uint32_t stamp = gog::metal_bridge::frameCounter();
    if (stamp == lastFrame) return;
    lastFrame = stamp;
    uint32_t w = 0, h = 0;
    gog::metal_bridge::frameSize(&w, &h);
    if (!w || !h) return;
    const float F = *reinterpret_cast<const float *>(0x0066FE44);
    const float Ax = *reinterpret_cast<const float *>(0x0077F4CC);
    const float Cx = *reinterpret_cast<const float *>(0x0077F4F0);
    const float Ay = *reinterpret_cast<const float *>(0x0078093C);
    const float Cy = *reinterpret_cast<const float *>(0x0077F930);
    const float zAdd3 = dk2::g_zAdd3_7793A0;
    const float zMul3 = dk2::g_zMul3_77F934;
    const auto &M = dk2::g_mat_77F498;
    const auto &T = *reinterpret_cast<const dk2::Vec3f *>(0x00780940);
    const float sx = 2.0f * Ax * F / static_cast<float>(w);
    const float ox = 2.0f * Cx / static_cast<float>(w) - 1.0f;
    const float sy = -2.0f * Ay * F / static_cast<float>(h);
    const float oy = 1.0f - 2.0f * Cy / static_cast<float>(h);
    // sub_594E10 computes out = M^T * v (out_i = sum_c m[c][i] * v_c), so the
    // view rotation rows here are the matrix COLUMNS.
    float R[4][4] = {};
    for (int c = 0; c < 3; ++c) {
        R[0][c] = sx * M.m[c][0] + ox * M.m[c][2];
        R[1][c] = sy * M.m[c][1] + oy * M.m[c][2];
        R[2][c] = zAdd3 * M.m[c][2];
        R[3][c] = M.m[c][2];
    }
    R[0][3] = sx * T.x + ox * T.z;
    R[1][3] = sy * T.y + oy * T.z;
    R[2][3] = zAdd3 * T.z - zMul3 * F;
    R[3][3] = T.z;
    float columnMajor[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            columnMajor[c * 4 + r] = R[r][c];
    // piecewise depth: near = zMul2*z + zAdd2, far = zAdd3 - zMul3*F/z,
    // switch where fd2[5] < z, capped at fd2[3] - replicated in the shader
    const float depthParams[6] = {
        dk2::g_zMul2_77F490, dk2::g_zAdd2_77F4D0,
        zAdd3, zMul3 * F,
        *reinterpret_cast<const float *>(0x0066FE3C),
        *reinterpret_cast<const float *>(0x0066FE34)};
    gog::metal_bridge::cameraSet(columnMajor, depthParams);
}

// Keep a deduplicated frame light table, while preserving the engine's
// per-object mask as indices into that table. Applying the whole union to
// every draw was both slower and visibly over-lit neighbouring cells.
bool prepareFrameLights(uint32_t *lightData, uint32_t mask,
                        dk2::meshgpu::LightSelection *out) {
    if (!out) return false;
    out->count = 0;
    // GPU light selection (settings.toml [game] light_selection_gpu, default
    // on): bypass the CPU-computed per-object selection mask (this->f2C =
    // sub_57BBF0, a bounding-sphere-vs-light-radius test) and let every
    // candidate light for this object (already bounded to <=32 by
    // lightData[0]+lightData[1], see `total` below) through to the GPU,
    // which already does a per-VERTEX distSqLimit+facing test in
    // dk2_mesh_accumulate_lights - finer-grained than the CPU's whole-object
    // sphere test, and removes a guest-side hash-dedup selection step under
    // Rosetta. Set DK2_LIGHT_SELECTION_GPU=0 to fall back to the original
    // CPU selection.
    static const bool testAllLights = [] {
        const char *v = std::getenv("DK2_LIGHT_SELECTION_GPU");
        return !v || std::strcmp(v, "0") != 0;
    }();
    const auto *lut = reinterpret_cast<const float *>(0x007818A0);
    static uint64_t seenKeys[2048];
    static uint16_t seenIndices[2048];
    static std::vector<DK2MLight> scratch;
    static uint32_t lastFrame = 0xFFFFFFFFu;
    const uint32_t stamp = gog::metal_bridge::frameCounter();
    bool firstOfFrame = false;
    if (stamp != lastFrame) {
        lastFrame = stamp;
        std::memset(seenKeys, 0, sizeof(seenKeys));
        scratch.clear();
        firstOfFrame = true;
    }
    const size_t sizeBefore = scratch.size();
    const int32_t total = lightData
        ? std::min<int32_t>(static_cast<int32_t>(lightData[0]) +
                                static_cast<int32_t>(lightData[1]),
                            32)
        : 0;
    const auto lights = lightData
        ? reinterpret_cast<const SceneLightForGpu *const *>(
              reinterpret_cast<const uint8_t *>(lightData) + 0x38)
        : nullptr;
    for (int32_t i = 0; i < total && out->count < 24; ++i) {
        if ((mask & (1u << i)) == 0 || !lights[i]) continue;
        const SceneLightForGpu &s = *lights[i];
        DK2MLight light = {};
        light.px = s.position.x;
        light.py = s.position.y;
        light.pz = s.position.z;
        light.r = s.color.x / 255.0f;
        light.g = s.color.y / 255.0f;
        light.b = s.color.z / 255.0f;
        light.dist_sq_limit = s.distanceSquaredLimit;
        light.atten_scale = s.attenuationScale;
        light.facing_scale = s.facingScale;
        uint64_t key = 1469598103934665603ull;
        const auto *bytes = reinterpret_cast<const uint8_t *>(&light);
        for (size_t byte = 0; byte < sizeof(light); ++byte) {
            key ^= bytes[byte];
            key *= 1099511628211ull;
        }
        if (!key) key = 1;
        uint32_t slot = static_cast<uint32_t>(key ^ (key >> 32)) & 2047u;
        uint16_t index = 0;
        bool found = false;
        while (seenKeys[slot]) {
            index = seenIndices[slot];
            if (seenKeys[slot] == key &&
                std::memcmp(&scratch[index], &light, sizeof(light)) == 0) {
                found = true;
                break;
            }
            slot = (slot + 1) & 2047u;
        }
        if (!found) {
            if (scratch.size() >= 1024) return false;
            index = static_cast<uint16_t>(scratch.size());
            scratch.push_back(light);
            seenKeys[slot] = key;
            seenIndices[slot] = index;
        }
        out->indices[out->count++] = index;
    }
    if (scratch.size() != sizeBefore || firstOfFrame) {
        gog::metal_bridge::lightsSet(scratch.data(), static_cast<uint32_t>(scratch.size()),
                                     0.0f, 0.0f, 0.0f, lut);
    }
    // wip: bring-up instrumentation (menu flicker investigation, removed
    // once root-caused) - continuous (not "first N distinct") sampling of
    // frame/mask/light-count, to see values changing call-to-call rather
    // than just discovering distinct values ever seen.
    {
        static uint32_t calls = 0;
        if ((++calls % 5) == 0) {
            patch::log::dbg(
                "prepareFrameLights: frame=%u mask=0x%08X total=%d "
                "selected=%u scratchSize=%zu testAllLights=%d",
                stamp, mask, total, out->count, scratch.size(), testAllLights);
        }
    }
    return true;
}

// SEH-guarded: level transitions leave stale cesurf/devTex pointers behind,
// and this three-hop chain must degrade to "no texture", never fault.
// devTex is only sometimes our FakeTexture (the legacy SetTexture path made
// it); terrain atlas pages carry a game-side object there whose +0x10 reads
// as garbage. Trust the pointer only when its vtable is literally ours.
bool isOurFakeTexture(void *p) {
    static const void *vtbl = [] {
        gog::FakeTexture probe(nullptr, nullptr);
        return *reinterpret_cast<void **>(&probe);
    }();
    return p && *reinterpret_cast<void **>(p) == vtbl;
}

struct ResolveStats {
    uint32_t calls, nullSurface, noCandidates, cesurfNull, devNull, fakeHit, rawHit, faults;
};
ResolveStats g_resolveStats = {};

uint32_t resolveBridgeTextureIdGuarded(dk2::MyCESurfHandle *slotHandle,
                                       uint32_t *bridgeIdOut, void **bridgeSurfaceOut) {
    __try {
        ++g_resolveStats.calls;
        if (!slotHandle) { ++g_resolveStats.nullSurface; return 0; }
        // Exactly ONE candidate: the handle occupying the scene object's
        // stage slot - its holder placement is what the UV tables describe.
        // Binding any other page (base vs reduction) shows the wrong region.
        dk2::MyCESurfHandle *candidates[1] = {slotHandle};
        const int candidateCount = 1;
        for (int i = 0; i < candidateCount; ++i) {
            dk2::MyCESurfHandle *handle = candidates[i];
            // The actual GPU texture is the HOLDER page the handle was packed
            // into (paint() blends the handle's CPU rect into holder->surf,
            // and the stage-0 UV tables map into holder space) - the 128x128
            // DD-backed atlas pages. Prefer it over the handle's own cesurf.
            if (handle->holder_parent && handle->holder_parent->surf) {
                auto *page = reinterpret_cast<dk2::CEngineDDSurface *>(
                        handle->holder_parent->surf);
                auto *pageTex = reinterpret_cast<gog::FakeTexture *>(page->devTex);
                if (pageTex && isOurFakeTexture(pageTex)) {
                    ++g_resolveStats.fakeHit;
                    *bridgeIdOut = pageTex->bridgeId();
                    *bridgeSurfaceOut = pageTex->bridgeSurface();
                    return 1;
                }
                if (page->surfCreated && page->ddSurf &&
                    reinterpret_cast<uintptr_t>(page->ddSurf) > 0x10000) {
                    ++g_resolveStats.rawHit;
                    *bridgeIdOut = 0;
                    *bridgeSurfaceOut = page->ddSurf;
                    return 1;
                }
            }
            // the engine creates cesurf lazily at paint time; do the same
            if (!handle->cesurf) handle->create();
            if (!handle->cesurf) { ++g_resolveStats.cesurfNull; continue; }
            auto *dd = reinterpret_cast<dk2::CEngineDDSurface *>(handle->cesurf);
            auto *fake = reinterpret_cast<gog::FakeTexture *>(dd->devTex);
            if (fake && isOurFakeTexture(fake)) {
                ++g_resolveStats.fakeHit;
                *bridgeIdOut = fake->bridgeId();
                *bridgeSurfaceOut = fake->bridgeSurface();
                return 1;
            }
            // No lazily-created FakeTexture (the GPU path bypasses legacy
            // SetTexture, so it never gets made) - hand the raw DD surface
            // back for synthetic-id registration.
            // Not a DD-backed surface (terrain atlas pages are plain CPU
            // CEngineSurface objects): capture through the base-class
            // virtuals, which every subclass implements.
            auto *base = reinterpret_cast<dk2::CEngineSurfaceBase *>(handle->cesurf);
            const uint32_t bytesPerPixel = base->fC_desc
                ? *reinterpret_cast<const uint32_t *>(
                      reinterpret_cast<const uint8_t *>(base->fC_desc) + 8)
                : 0;
            if (bytesPerPixel == 4 && base->width > 0 && base->height > 0) {
                void *pixels = base->v_lockBuf();
                if (pixels) {
                    ++g_resolveStats.rawHit;
                    // lineWidth is zero on plain CPU pages - fall back to a
                    // tight row stride
                    const uint32_t stridePixels = base->lineWidth > base->width
                        ? static_cast<uint32_t>(base->lineWidth)
                        : static_cast<uint32_t>(base->width);
                    static bool loggedPage = false;
                    if (!loggedPage) {
                        loggedPage = true;
                        patch::log::dbg("mesh page: w=%d h=%d lineWidth=%d bpp=%u",
                                        base->width, base->height, base->lineWidth,
                                        bytesPerPixel);
                    }
                    const uint32_t id = gog::metal_bridge::ensureBufferTexture(
                        base, pixels,
                        static_cast<uint32_t>(base->width),
                        static_cast<uint32_t>(base->height),
                        stridePixels * 4u);
                    base->v_unlockBuf(reinterpret_cast<int>(pixels));
                    if (id) {
                        *bridgeIdOut = id;
                        *bridgeSurfaceOut = nullptr;
                        return 2;  // already registered, no ensureTexture needed
                    }
                    continue;
                }
            }
            ++g_resolveStats.devNull;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_resolveStats.faults;
        return 0;
    }
}

// SEH-guarded holder fetch for the resolve cache: creature/effect handles can
// be a different subclass whose +holder_parent reads as a small-int garbage
// pointer, and an unguarded read here crashed through WOW64 (7BF2123D).
int readHolderGuarded(dk2::MyCESurfHandle *handle, const void **holderOut) {
    __try {
        *holderOut = handle->holder_parent;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// __renderFun_setSceneObject2E builds the UV scale/offset tables from
// sceneHandle->curReduction (58A996..58A9CD).  The texture page must come
// from that same effective handle; using the unreduced handle pairs reduced
// UVs with a different atlas page and exposes unrelated sprites at far zoom.
dk2::MyCESurfHandle *currentReductionGuarded(dk2::MyCESurfHandle *handle) {
    __try {
        if (!handle) return nullptr;
        dk2::MyCESurfHandle *reduced = handle->curReduction;
        const uintptr_t value = reinterpret_cast<uintptr_t>(reduced);
        return value >= 0x10000u && (value & 3u) == 0 ? reduced : handle;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return handle;
    }
}

uint32_t resolveBridgeTextureId(dk2::MyCESurfHandle *slotHandle) {
    slotHandle = currentReductionGuarded(slotHandle);
    // plausibility gate BEFORE any dereference (guarded or not): garbage
    // handles (e.g. 0x4DC9 from anim scenes) change value every call, so the
    // negative cache never hits and the recovered-SEH storm crashes WOW64.
    if (reinterpret_cast<uintptr_t>(slotHandle) < 0x10000u ||
        (reinterpret_cast<uintptr_t>(slotHandle) & 3u)) {
        return 0;
    }
    // positive cache: the guarded resolve chain + per-entry ensureTexture cost
    // ~microseconds across thousands of entries per frame, while the result
    // only changes when the handle is repacked into a different holder page.
    // Key on (handle, holder_parent) and re-ensure once per producer frame.
    struct ResolvedTexture {
        dk2::MyCESurfHandle *handle;
        const void *holder;
        uint32_t bridgeId;
        void *bridgeSurface;
        uint32_t lastEnsureFrame;
    };
    static ResolvedTexture cache[2048];  // open-addressed, handle==nullptr empty
    // negative cache FIRST: a handle whose holder read faults must not fault
    // again on every call - repeated recovered SEH faults crash WOW64 itself
    static std::vector<const void *> badSurfaces;
    for (const void *bad : badSurfaces) {
        if (bad == slotHandle) return 0;
    }
    const void *holder = nullptr;
    if (slotHandle && readHolderGuarded(slotHandle, &holder)) {
        uint32_t slot = (reinterpret_cast<uintptr_t>(slotHandle) >> 4) * 2654435761u & 2047u;
        for (int probe = 0; probe < 16; ++probe, slot = (slot + 1) & 2047u) {
            ResolvedTexture &hit = cache[slot];
            if (!hit.handle) break;
            if (hit.handle != slotHandle) continue;
            if (hit.holder != holder) { hit.handle = nullptr; break; }  // repacked
            // DD surfaces report dirtiness through Lock/Blt hooks. Raw engine
            // atlas pages have no COM surface, so PAGE_ATLAS_RESET marks their
            // synthetic texture dirty and we re-enter the guarded lock/copy
            // path exactly once here instead of returning stale black pixels.
            if (!hit.bridgeSurface &&
                gog::metal_bridge::bufferTextureNeedsRefresh(hit.bridgeId)) {
                break;
            }
            if (hit.bridgeSurface) {
                gog::metal_bridge::ensureTexture(
                    hit.bridgeId,
                    static_cast<IDirectDrawSurface4 *>(hit.bridgeSurface));
            }
            return hit.bridgeId;
        }
    }
    uint32_t bridgeId = 0;
    void *bridgeSurface = nullptr;
    const uint32_t resolveMode = resolveBridgeTextureIdGuarded(slotHandle, &bridgeId, &bridgeSurface);
    if (!resolveMode) {
        if (badSurfaces.size() < 4096) badSurfaces.push_back(slotHandle);
        return 0;
    }
    static DWORD lastStatsTick = 0;
    static uint32_t retZero = 0, retNonzero = 0, sampleId = 0;
    if (bridgeId) { ++retNonzero; if (!sampleId) sampleId = bridgeId; }
    else ++retZero;
    const DWORD statsTick = GetTickCount();
    if (statsTick - lastStatsTick > 3000) {
        lastStatsTick = statsTick;
        if (o_flametal_debugProbes.get()) {
            patch::log::dbg("mesh tex resolve: calls=%u nullSurf=%u noCand=%u cesurfNull=%u "
                            "devNull=%u fakeHit=%u rawHit=%u faults=%u retZero=%u retNonzero=%u sample=%u",
                            g_resolveStats.calls, g_resolveStats.nullSurface,
                            g_resolveStats.noCandidates, g_resolveStats.cesurfNull,
                            g_resolveStats.devNull, g_resolveStats.fakeHit,
                            g_resolveStats.rawHit, g_resolveStats.faults, retZero, retNonzero, sampleId);
        }
    }
    if (resolveMode == 2) return bridgeId;  // buffer texture already registered
    if (bridgeId) {
        // capture-only registration: never disturbs stage-0 binding state
        gog::metal_bridge::ensureTexture(
            bridgeId, static_cast<IDirectDrawSurface4 *>(bridgeSurface));
        const void *storeHolder = nullptr;
        if (readHolderGuarded(slotHandle, &storeHolder)) {
            uint32_t slot = (reinterpret_cast<uintptr_t>(slotHandle) >> 4) * 2654435761u & 2047u;
            for (int probe = 0; probe < 16; ++probe, slot = (slot + 1) & 2047u) {
                ResolvedTexture &entry = cache[slot];
                if (entry.handle && entry.handle != slotHandle) continue;
                entry.handle = slotHandle;
                entry.holder = storeHolder;
                entry.bridgeId = bridgeId;
                entry.bridgeSurface = bridgeSurface;
                entry.lastEnsureFrame = gog::metal_bridge::frameCounter();
                break;
            }
        }
        return bridgeId;
    }
    if (bridgeSurface) {
        // surface without a FakeTexture: synthetic-id registration
        return gog::metal_bridge::ensureSurfaceTexture(
            static_cast<IDirectDrawSurface4 *>(bridgeSurface));
    }
    return 0;
}

// SEH-guarded raw copy out of a MeshEntry: not every entry reaching
// sub_57B6D0 carries the layout this reroute assumes (a level-load crash
// showed entry.vertices holding float-looking garbage), and a bad pointer
// must mean "fall back to the CPU path", not a page fault. Plain loops only
// inside __try - no C++ objects, so no unwinding is needed.
int copyEntryGuarded(const MeshEntry &entry, uint32_t indexCount,
                     uint32_t *vertexCountOut,
                     DK2MMeshVertex *outVertices, uint32_t outCapacity,
                     uint16_t *outIndices,
                     float uvScale, float uS, float vS, float uO, float vO) {
    __try {
        uint32_t maxIndex = 0;
        for (uint32_t i = 0; i < indexCount; ++i) {
            if (entry.triangleIndices[i] > maxIndex) maxIndex = entry.triangleIndices[i];
            outIndices[i] = entry.triangleIndices[i];
        }
        const uint32_t vertexCount = maxIndex + 1;
        if (vertexCount > outCapacity) return 0;
        for (uint32_t v = 0; v < vertexCount; ++v) {
            const MeshVertex &src = entry.vertices[v];
            DK2MMeshVertex &dst = outVertices[v];
            dst.px = src.position.x;
            dst.py = src.position.y;
            dst.pz = src.position.z;
            dst.nx = src.normal.x;
            dst.ny = src.normal.y;
            dst.nz = src.normal.z;
            dst.u = uS * (static_cast<float>(src.packedUv & 0xFFFF) * uvScale) + uO;
            dst.v = vS * (static_cast<float>(src.packedUv >> 16) * uvScale) + vO;
            dst.base_color = packBaseColor(src.color);
        }
        *vertexCountOut = vertexCount;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int copyEntryPositionsGuarded(const MeshEntry &entry, uint32_t vertexCount,
                              float *outPositions) {
    __try {
        for (uint32_t vertex = 0; vertex < vertexCount; ++vertex) {
            outPositions[vertex * 3 + 0] = entry.vertices[vertex].position.x;
            outPositions[vertex * 3 + 1] = entry.vertices[vertex].position.y;
            outPositions[vertex * 3 + 2] = entry.vertices[vertex].position.z;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int describeEntryGuarded(const MeshEntry &entry, uint32_t indexCount,
                         uint32_t *vertexCountOut, uint64_t *signatureOut) {
    __try {
        if (!entry.vertices || !entry.triangleIndices || !indexCount) return 0;
        uint32_t maxIndex = 0;
        uint64_t signature = 1469598103934665603ull;
        for (uint32_t i = 0; i < indexCount; ++i) {
            const uint8_t index = entry.triangleIndices[i];
            if (index > maxIndex) maxIndex = index;
            signature ^= index;
            signature *= 1099511628211ull;
        }
        const uint32_t vertexCount = maxIndex + 1;
        const uint32_t samples[3] = {0, maxIndex / 2, maxIndex};
        for (uint32_t sample : samples) {
            const auto *bytes = reinterpret_cast<const uint8_t *>(&entry.vertices[sample]);
            for (size_t byte = 0; byte < sizeof(MeshVertex); ++byte) {
                signature ^= bytes[byte];
                signature *= 1099511628211ull;
            }
        }
        *vertexCountOut = vertexCount;
        *signatureOut = signature;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct RetainedEntryMesh {
    uint32_t meshId;
    dk2::Vec3f origin;
};

// bring-up: retained cache reuse ratio (decides retained-instancing viability)
static uint32_t g_reHit = 0, g_reNew = 0;
static uint32_t g_reFailSat = 0, g_reFailCopy = 0, g_reFailReg = 0;
static void reFailReport() {
    uint32_t t = g_reFailSat + g_reFailCopy + g_reFailReg;
    if ((t % 4096) == 0)
        patch::log::dbg("retainfail reasons: saturation=%u copyfail=%u regfail=%u",
                        g_reFailSat, g_reFailCopy, g_reFailReg);
}
static void reuseReport() {
    if (((g_reHit + g_reNew) % 8192) == 0)
        patch::log::dbg("retainedEntryMesh: HIT=%u NEW=%u hit%%=%u uniqueMeshes=%u",
                        g_reHit, g_reNew,
                        (g_reHit + g_reNew) ? g_reHit * 100 / (g_reHit + g_reNew) : 0,
                        g_reNew);
}

bool retainedEntryMesh(const MeshEntry &entry,
                       uint32_t indexCount, uint32_t vertexCount,
                       uint64_t signature, float uvScale,
                       RetainedEntryMesh *out) {
    struct CacheEntry {
        const void *vertices;
        const void *indices;
        uint32_t indexCount;
        uint32_t vertexCount;
        uint64_t signature;
        uint32_t meshId;
        dk2::Vec3f origin;
    };
    struct ContentEntry {
        uint32_t vertexCount;
        uint32_t indexCount;
        uint32_t meshId;
        std::vector<DK2MMeshVertex> vertices;
        std::vector<uint16_t> indices;
    };
    // The entry buffers are pool-allocated in a contiguous region, so raw
    // address bits cluster and the old 16384-slot cache saturated its 64-probe
    // window for ~27% of lookups (measured 1.26M saturation fails), dropping
    // that much static geometry back to CPU. Larger table (18% load at ~12k
    // unique templates) plus a multiplicative bit-mix that spreads clustered
    // pointers across the whole table.
    constexpr uint32_t kCacheSlots = 65536;
    constexpr uint32_t kCacheMask = kCacheSlots - 1;
    static CacheEntry cache[kCacheSlots] = {};
    static std::vector<ContentEntry> contents;
    static std::unordered_multimap<uint64_t, uint32_t> contentLookup;
    uint64_t hash = (reinterpret_cast<uintptr_t>(entry.vertices) >> 4) * 0x9E3779B97F4A7C15ull;
    hash ^= (reinterpret_cast<uintptr_t>(entry.triangleIndices) >> 3) * 0xC2B2AE3D27D4EB4Full;
    hash ^= hash >> 29;
    uint32_t slot = static_cast<uint32_t>(hash) & kCacheMask;
    CacheEntry *available = nullptr;
    for (uint32_t probe = 0; probe < 64; ++probe, slot = (slot + 1) & kCacheMask) {
        CacheEntry &candidate = cache[slot];
        if (!candidate.vertices) {
            available = &candidate;
            break;
        }
        if (candidate.vertices != entry.vertices ||
            candidate.indices != entry.triangleIndices ||
            candidate.indexCount != indexCount ||
            candidate.vertexCount != vertexCount) {
            continue;
        }
        if (candidate.signature == signature) {
            out->meshId = candidate.meshId;
            out->origin = candidate.origin;
            g_reHit++;
            reuseReport();
            return true;
        }
        // Same address+counts but a DIFFERENT signature: the buffer's content
        // changed in place. If this is ~0 during play (digging/building) and
        // only fires on level/resource rebuild, the signature walk is
        // redundant per-frame and describeEntryGuarded can be skipped on an
        // address hit. Count it to decide.
        available = &candidate;  // address reuse after a level/resource rebuild
        static uint32_t g_addrReuse = 0;
        if ((++g_addrReuse % 64) == 1)
            patch::log::dbg("retained: same-address-DIFFERENT-content=%u "
                            "(if this climbs during play, signature is needed)",
                            g_addrReuse);
        break;
    }
    if (!available) { g_reFailSat++; reFailReport(); return false; }
    static DK2MMeshVertex vertices[256];
    static uint16_t indices[765];
    uint32_t copiedVertices = 0;
    if (!copyEntryGuarded(entry, indexCount, &copiedVertices, vertices, 256,
                          indices, uvScale, 1.0f, 1.0f, 0.0f, 0.0f) ||
        copiedVertices != vertexCount) {
        g_reFailCopy++; reFailReport(); return false;
    }
    const dk2::Vec3f origin{vertices[0].px, vertices[0].py, vertices[0].pz};
    for (uint32_t vertex = 0; vertex < copiedVertices; ++vertex) {
        vertices[vertex].px -= origin.x;
        vertices[vertex].py -= origin.y;
        vertices[vertex].pz -= origin.z;
    }
    uint64_t contentHash = 1469598103934665603ull;
    const auto hashBytes = [&](const void *data, size_t size) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; ++i) {
            contentHash ^= bytes[i];
            contentHash *= 1099511628211ull;
        }
    };
    hashBytes(&copiedVertices, sizeof(copiedVertices));
    hashBytes(&indexCount, sizeof(indexCount));
    hashBytes(vertices, copiedVertices * sizeof(DK2MMeshVertex));
    hashBytes(indices, indexCount * sizeof(uint16_t));

    uint32_t meshId = 0;
    const auto range = contentLookup.equal_range(contentHash);
    for (auto found = range.first; found != range.second; ++found) {
        const ContentEntry &content = contents[found->second];
        if (content.vertexCount == copiedVertices &&
            content.indexCount == indexCount &&
            std::memcmp(content.vertices.data(), vertices,
                        copiedVertices * sizeof(DK2MMeshVertex)) == 0 &&
            std::memcmp(content.indices.data(), indices,
                        indexCount * sizeof(uint16_t)) == 0) {
            meshId = content.meshId;
            break;
        }
    }
    if (!meshId) {
        meshId = dk2::meshgpu::allocateMeshId();
        if (!dk2::meshgpu::registerMesh(
                meshId, vertices, copiedVertices, indices, indexCount)) {
            g_reFailReg++; reFailReport(); return false;
        }
        g_reNew++;  // a truly unique object-space template was registered
        if (contents.size() < 16384) {
            ContentEntry content;
            content.vertexCount = copiedVertices;
            content.indexCount = indexCount;
            content.meshId = meshId;
            content.vertices.assign(vertices, vertices + copiedVertices);
            content.indices.assign(indices, indices + indexCount);
            const uint32_t contentIndex = static_cast<uint32_t>(contents.size());
            contents.push_back(std::move(content));
            contentLookup.emplace(contentHash, contentIndex);
        }
    }
    available->vertices = entry.vertices;
    available->indices = entry.triangleIndices;
    available->indexCount = indexCount;
    available->vertexCount = vertexCount;
    available->signature = signature;
    available->meshId = meshId;
    available->origin = origin;
    out->meshId = meshId;
    out->origin = origin;
    return true;
}

// Emit one MeshEntry through the bridge's inline world-space path. The UV
// stage-0 scale/offset tables were just written by __renderFun_setSceneObject2E
// for this very scene object, so reading them here matches writeVertex1C.
bool drawEntryOnGpu(dk2::SceneObject2E *scene, MeshEntry &entry,
                    dk2::MyScaledSurface *surface,
                    const dk2::Vec3f &ambient, uint32_t *lightData,
                    uint32_t lightMask,
                    dk2::Obj58EF60 *sampler = nullptr) {
    // bring-up: why do terrain/static entries fall back to CPU? Count every
    // exit by reason, dump every 4096 calls. reasons: 0=empty 1=badcache
    // 2=unreadable/>256 3=copyfail 4=no-texid 5=lightfail 6=retainfail
    // 7=posfail 8=SUCCESS(inline) 9=SUCCESS(deformed)
    static uint32_t deReason[10] = {0};
    static uint32_t deCalls = 0;
    auto deTally = [&](int r) -> bool {
        ++deReason[r];
        if ((++deCalls % 4096) == 0) {
            patch::log::dbg("drawEntryOnGpu/4096: empty=%u badcache=%u unreadable=%u "
                            "copyfail=%u no-texid=%u lightfail=%u retainfail=%u "
                            "posfail=%u ok-inline=%u ok-deformed=%u",
                            deReason[0], deReason[1], deReason[2], deReason[3],
                            deReason[4], deReason[5], deReason[6], deReason[7],
                            deReason[8], deReason[9]);
            for (uint32_t &c : deReason) c = 0;
        }
        return r >= 8;  // true only on the two success reasons
    };
    if (!entry.triangleCount || !entry.vertices || !entry.triangleIndices) return deTally(0);
    const uint32_t indexCount = static_cast<uint32_t>(entry.triangleCount) * 3u;
    // The UV scale/offset tables are per scene-object STAGE SLOT: find the
    // slot our surface's handle occupies instead of assuming slot 0
    // (multi-texture objects put it elsewhere, shifting every UV).
    int stageSlot = 0;
    dk2::MyCESurfHandle *slotHandle = surface ? surface->surfh : nullptr;
    if (scene) {
        for (int i = 0; i < 4 && i < scene->surfhCount; ++i) {
            if (surface && scene->surfh_x4[i] == surface->surfh) {
                stageSlot = i;
                slotHandle = scene->surfh_x4[i];
                break;
            }
        }
        // reduction in effect: the slot holds a different handle entirely -
        // trust the scene slot, its placement is what the tables map into
        if (scene->surfhCount == 1 && scene->surfh_x4[0]) {
            slotHandle = scene->surfh_x4[0];
            stageSlot = 0;
        }
    }
    const float uvScale = *reinterpret_cast<const float *>(0x0066FB58);
    const float uS = reinterpret_cast<const float *>(0x00779368)[stageSlot];
    const float vS = reinterpret_cast<const float *>(0x0076F340)[stageSlot];
    const float uO = reinterpret_cast<const float *>(0x0077F480)[stageSlot];
    const float vO = reinterpret_cast<const float *>(0x0077F3D8)[stageSlot];
    // Negative cache: a bad entry pointer would otherwise fault (and ride
    // Wine's fragile WOW64 SEH dispatch) on every single frame - fault once,
    // remember, and fall back instantly afterwards.
    static std::vector<const void *> badEntries;
    for (const void *bad : badEntries) {
        if (bad == entry.vertices) return deTally(1);
    }
    uint32_t vertexCount = 0;
    uint64_t signature = 0;
    // bring-up: quantify the per-draw describe cost (the suspected remaining
    // marshalling after retained instancing removed the position copy).
    LARGE_INTEGER deT0, deT1;
    QueryPerformanceCounter(&deT0);
    const bool describeOk = describeEntryGuarded(
            entry, indexCount, &vertexCount, &signature);
    QueryPerformanceCounter(&deT1);
    {
        static uint64_t descTicks = 0, restStart = 0; static uint32_t descN = 0;
        static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        descTicks += (uint64_t)(deT1.QuadPart - deT0.QuadPart);
        if ((++descN % 20000) == 0) {
            patch::log::dbg("describeEntryGuarded avg: %llu ns/call over %u calls (=%llu us/20k)",
                            descTicks * 1000000000ull / (freq.QuadPart * (uint64_t)descN),
                            descN, descTicks * 1000000ull / freq.QuadPart);
            descTicks = 0; descN = 0;
        }
    }
    if (!describeOk || vertexCount > 256) {
        if (badEntries.size() < 4096) badEntries.push_back(entry.vertices);
        static bool loggedBadEntry = false;
        if (!loggedBadEntry) {
            loggedBadEntry = true;
            patch::log::dbg("mesh gpu path: unreadable entry (vertices=%p indices=%p), "
                            "falling back to CPU emission",
                            entry.vertices, entry.triangleIndices);
        }
        return deTally(2);
    }
    static DK2MMeshVertex vertices[256];
    static uint16_t indices[765];
    if (sampler) {
        if (!copyEntryGuarded(entry, indexCount, &vertexCount, vertices, 256,
                              indices, uvScale, uS, vS, uO, vO)) {
            return deTally(3);
        }
        // extended path: vector-field displacement stays on the CPU, exactly
        // as the legacy emitter samples it before transforming
        for (uint32_t v = 0; v < vertexCount; ++v) {
            dk2::Vec3f displaced;
            sampler->sub_58F030(vertices[v].px, vertices[v].py, vertices[v].pz,
                                &displaced.x);
            vertices[v].px = displaced.x;
            vertices[v].py = displaced.y;
            vertices[v].pz = displaced.z;
        }
    }
    const uint32_t alphaTerm = *reinterpret_cast<const uint32_t *>(0x00779380);
    const uint32_t tint = (alphaTerm & 0xFF000000u) | 0x00FFFFFFu;
    const uint32_t textureId = resolveBridgeTextureId(slotHandle);
    if (!textureId) return deTally(4);
    emitMeshCamera();
    dk2::meshgpu::LightSelection lights = {};
    if (!prepareFrameLights(lightData, lightMask, &lights)) return deTally(5);
    // Colours are plain 0..255 floats everywhere (probe confirmed zeros for
    // both vertex colour and ambient); the "bias" constants in the engine's
    // encoding helpers are themselves negative magic numbers, so nothing here
    // needs debiasing.
    const uint32_t meshFlags = metalMeshFlags(
        meshDrawFlags(scene, surface, stageSlot), true);
    const dk2::meshgpu::InlineTarget target = {
        textureId, uS, vS, uO, vO, meshFlags, tint};
    if (sampler) {
        dk2::meshgpu::emitInline(
            target, vertices, vertexCount, indices, indexCount, lights,
            ambient.x / 255.0f, ambient.y / 255.0f, ambient.z / 255.0f);
        return deTally(8);
    } else {
        // REVERTED 2026-07-23 (regression): retained-instanced draw
        // (emitRetained + translate-by-origin, see git history 31ead1e)
        // caused static terrain to render as a black void that grew as the
        // camera rotated - torches/lights still visible floating in it,
        // meaning the terrain draw was simply MISSING, not merely unlit.
        //
        // Root cause (suspected, not yet 100% proven): retainedEntryMesh's
        // cache is keyed by entry.vertices/triangleIndices POOL ADDRESS plus
        // a signature that samples only 3 vertices (index 0, mid, max - see
        // describeEntryGuarded) + full index topology. Tile-based dungeon
        // terrain shares near-identical topology and often shares edge
        // vertices between adjacent tiles; when the game's memory pool
        // recycles an address for a DIFFERENT tile as the camera reveals new
        // geometry, a 3-sample signature collision can make the cache think
        // it's the SAME object and reuse its STALE `origin` - drawing the
        // new tile's shape translated to the OLD tile's world position
        // (likely off-screen/outside the frustum -> appears as missing
        // geometry). The pre-31ead1e code explicitly guarded against this
        // exact failure mode ("an address-cache hit cannot resurrect old
        // terrain") by re-streaming absolute positions every frame instead
        // of trusting the signature to prove positional identity.
        //
        // Reverted to the proven-correct emitDeformed (stream absolute
        // positions every frame, identity transform) until retained-instancing
        // is redesigned with a stronger cache-hit guarantee (e.g. hash all
        // vertices, or store+compare the origin itself as part of the
        // signature). This gives back the ~16.5ms/frame CPU cost the
        // optimization removed - correctness first.
        static float positions[256 * 3];
        if (!copyEntryPositionsGuarded(entry, vertexCount, positions)) {
            return deTally(7);
        }
        static const float identity[12] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0};
        RetainedEntryMesh retained;
        if (!retainedEntryMesh(
                entry, indexCount, vertexCount, signature, uvScale,
                &retained)) {
            return deTally(6);
        }
        dk2::meshgpu::emitDeformed(
            target, retained.meshId, positions, vertexCount, identity, lights,
            ambient.x / 255.0f, ambient.y / 255.0f, ambient.z / 255.0f);
        return deTally(9);
    }
    return true;
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


// Cross-emitter plumbing (MeshGpuEmit.h): lets the animated-mesh emitter in
// CEngineAnimMesh.cpp reuse this file's texture resolve, UV stage tables and
// material-flag translation for GPU inline draws.
namespace dk2::meshgpu {

bool active() { return meshGpuActive(); }

void emitCamera() { emitMeshCamera(); }

uint32_t allocateMeshId() {
    static uint32_t nextMeshId = 1;
    const uint32_t result = nextMeshId++;
    if (!nextMeshId) nextMeshId = 1;
    return result;
}

bool registerMesh(uint32_t meshId, const DK2MMeshVertex *vertices,
                  uint32_t vertexCount, const uint16_t *indices,
                  uint32_t indexCount) {
    return gog::metal_bridge::meshRegister(
        meshId, vertices, vertexCount, indices, indexCount);
}

bool prepareLights(uint32_t *collection, uint32_t mask, LightSelection *out) {
    return prepareFrameLights(collection, mask, out);
}

bool prepareTarget(dk2::SceneObject2E *scene, dk2::MyScaledSurface *surface,
                   bool lit, InlineTarget *out) {
    int stageSlot = 0;
    dk2::MyCESurfHandle *slotHandle = surface ? surface->surfh : nullptr;
    if (scene) {
        for (int i = 0; i < 4 && i < scene->surfhCount; ++i) {
            if (surface && scene->surfh_x4[i] == surface->surfh) {
                stageSlot = i;
                slotHandle = scene->surfh_x4[i];
                break;
            }
        }
        if (scene->surfhCount == 1 && scene->surfh_x4[0]) {
            slotHandle = scene->surfh_x4[0];
            stageSlot = 0;
        }
    }
    if (!slotHandle) return false;
    out->textureId = resolveBridgeTextureId(slotHandle);
    out->uS = reinterpret_cast<const float *>(0x00779368)[stageSlot];
    out->vS = reinterpret_cast<const float *>(0x0076F340)[stageSlot];
    out->uO = reinterpret_cast<const float *>(0x0077F480)[stageSlot];
    out->vO = reinterpret_cast<const float *>(0x0077F3D8)[stageSlot];
    const uint32_t alphaTerm = *reinterpret_cast<const uint32_t *>(0x00779380);
    out->tint = (alphaTerm & 0xFF000000u) | 0x00FFFFFFu;
    out->meshFlags = metalMeshFlags(
        meshDrawFlags(scene, surface, stageSlot), lit);
    return true;
}

void emitRetained(const InlineTarget &target, uint32_t meshId,
                  const float world[12], const LightSelection &lights,
                  float ambientR, float ambientG, float ambientB) {
    const float uvTransform[4] = {target.uS, target.vS, target.uO, target.vO};
    gog::metal_bridge::drawMesh(
        meshId, target.textureId, world, uvTransform,
        target.tint, target.meshFlags,
        lights.indices, lights.count, ambientR, ambientG, ambientB);
}

void emitDeformed(const InlineTarget &target, uint32_t meshId,
                  const float *positions, uint32_t vertexCount,
                  const float world[12], const LightSelection &lights,
                  float ambientR, float ambientG, float ambientB) {
    const float uvTransform[4] = {target.uS, target.vS, target.uO, target.vO};
    gog::metal_bridge::drawMeshDeformed(
        meshId, target.textureId, positions, vertexCount, world,
        uvTransform, target.tint, target.meshFlags,
        lights.indices, lights.count,
        ambientR, ambientG, ambientB);
}

void emitInline(const InlineTarget &target, const DK2MMeshVertex *vertices,
                uint32_t vertexCount, const uint16_t *indices,
                uint32_t indexCount, const LightSelection &lights,
                float ambientR, float ambientG, float ambientB) {
    gog::metal_bridge::drawMeshInline(
        target.textureId, vertices, vertexCount, indices, indexCount,
        target.tint, target.meshFlags, lights.indices, lights.count,
        ambientR, ambientG, ambientB);
}

}  // namespace dk2::meshgpu


// dk2::sub_57BBF0 (DKII 0x0057BBF0) lives in sub_57BBF0.cpp - see its header
// comment for the verified algorithm and a difftest. This file used to carry
// an independent SIMD reimplementation of the same address (a `git log`
// shows both were added days apart, presumably by concurrent work unaware of
// each other); removed here to fix the resulting LNK2005 duplicate-definition.


// DKII 0x0057AC10: computes this->f2C, the per-object light-selection mask,
// by testing the object's bounding sphere (this->vec/this->f20, populated by
// the untranslated sub_57AA40) against every candidate light via sub_57BBF0
// (a real geometric distance-vs-influence-radius test, filter mask
// hardcoded to 1). See .porting/roadmap-native.md "Light selection -> GPU"
// for the full disasm trace that established this signature (this->f2C =
// sub_57BBF0(a1, 1, vec.x, vec.y, vec.z, f20) | f28; ret 0x4, single stack
// arg = a1, a pointer to the frame's light-list, unused here).
//
// We deliberately do NOT replicate that test: the GPU mesh path already
// re-derives light selection itself, per-VERTEX (dk2_mesh_accumulate_lights,
// settings.toml [game] light_selection_gpu, default on) - finer-grained than
// this whole-object sphere test, so its result goes unused on that path.
// Skipping the real test removes a guest-CPU bounding-sphere-vs-<=32-lights
// loop from every object, every frame - the actual Rosetta cost; merely
// ignoring f2C's value (as the settings toggle above does) does not, since
// the original test still ran regardless.
//
// Accepted trade-off: the legacy CPU lighting fallback (Obj57BCB0, exercised
// on ~0.02-0.05% of draws per measured deTally/retainfail counters - see
// drawEntryOnGpu below) still consults f2C and does NOT itself re-test
// distance, so it could rarely over-light in those rare fallback frames.
//
// settings.toml [game] light_selection_gpu (default on) gates this: when
// off, call the already-translated dk2::sub_57BBF0 (src/dk2/sub_57BBF0.cpp) -
// the real geometric sphere-cull test - exactly like CEngineStaticMeshAdd.cpp
// does for its own copy of this same call (mask=1, collection=a1, position=
// vec, radius=f20; see that file's comment for the verified call site).
int dk2::Obj57AD20::sub_57AC10(int *a1) {
    static const bool skipCpuSelection = [] {
        const char *v = std::getenv("DK2_LIGHT_SELECTION_GPU");
        return !v || std::strcmp(v, "0") != 0;
    }();
    if (skipCpuSelection) {
        f2C = static_cast<int>(0xFFFFFFFFu) | f28;
        return f2C;
    }
    f2C = dk2::sub_57BBF0(a1, nullptr, vec.x, vec.y, vec.z, f20, 1) | f28;
    return f2C;
}

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
        // GPU path: use the translated extended emitter, whose GPU branch
        // keeps only the vector-field displacement on the CPU. The original
        // stays the default for the legacy renderer until the translation
        // has a frame-level differential test.
        if (meshGpuActive()) {
            return sub_57B0E0(entryIndex, scene, a4, lights,
                              selectExtendedPath, a6, a7, scale);
        }
        using OriginalExtendedFun = int *(__thiscall *)(
                Obj57AD20 *, int, SceneObject2E *, int, uint32_t *,
                int, int, int, float);
        return reinterpret_cast<OriginalExtendedFun>(0x0057B0E0)(
                this, entryIndex, scene, a4, lights,
                selectExtendedPath, a6, a7, scale);
    }
    return sub_57B6D0(entryIndex, scene, a4, lights, a6, a7, scale);
}


// Staged until the extended spatial-mesh path has a frame-level differential
// test.  sub_57A9A0 deliberately dispatches to the original entry for now.
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
    if (meshGpuActive() &&
        drawEntryOnGpu(scene, entry, surface, ambient, lightData,
                       static_cast<uint32_t>(f2C), &sampler)) {
        return applyIndxs_sub_58AC20();
    }
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

    if (meshGpuActive() &&
        drawEntryOnGpu(scene, entry, surface, ambient, lightData,
                       static_cast<uint32_t>(f2C))) {
        return applyIndxs_sub_58AC20();
    }

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
